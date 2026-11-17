// The XAudio2 reverb effect (xaudio2fx: AudioReverb, XAUDIO2FX_REVERB_PARAMETERS), as used by xaudio2.c.
//
// Not Microsoft's algorithm, but one driven by the same parameters, so a game's presets (often converted from
// I3DL2 with ReverbConvertI3DL2ToNative) sound like rooms of the size and decay they describe:
//   - the room filter: a one-pole high shelf (RoomFilterFreq, RoomFilterHF) and an overall gain (RoomFilterMain);
//   - pre-delay: ReflectionsDelay to the early reflections, + ReverbDelay to the late field;
//   - early reflections: taps spread over a span set by RoomSize, alternating left / right, ReflectionsGain,
//     through an allpass whose strength is EarlyDiffusion;
//   - late field: series allpass diffusers (LateDiffusion, Density) feeding an 8-line feedback delay network with
//     Hadamard mixing; each line's gain gives an RT60 of DecayTime, and a lowpass in the loop makes high frequencies
//     die faster (HighEQGain, HighEQCutoff); a low shelf on the output (LowEQGain, LowEQCutoff); ReverbGain;
//   - WetDryMix: the share of reverb in the output, the rest being the input.
// Mono or stereo in; mono, stereo or more out (front pair, plus the rear pair at -3 dB for 5.1 / 7.1).
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// XAUDIO2FX_REVERB_PARAMETERS (52 bytes)
typedef struct {
    float WetDryMix; uint32_t ReflectionsDelay;
    uint8_t ReverbDelay, RearDelay, PositionLeft, PositionRight, PositionMatrixLeft, PositionMatrixRight,
            EarlyDiffusion, LateDiffusion, LowEQGain, LowEQCutoff, HighEQGain, HighEQCutoff;
    float RoomFilterFreq, RoomFilterMain, RoomFilterHF, ReflectionsGain, ReverbGain, DecayTime, Density, RoomSize;
} XReverbParams;
_Static_assert(sizeof(XReverbParams) == 52, "XAUDIO2FX_REVERB_PARAMETERS is 52 bytes");

#define NLINE 8
#define LINE_LEN 8192        // per FDN line (> 1.9x the longest base length at 48 kHz)
#define PRE_LEN 32768        // pre-delay: 300 + 85 ms at up to 96 kHz... at 48 kHz with room to spare
#define AP_LEN 2048
#define NAP 4
#define NER 6

typedef struct { float *buf; int len, pos; float g; } Allpass;
typedef struct XReverb {
    float rate;
    XReverbParams p;
    // derived
    int pre_er, pre_late, line_len[NLINE], ap_len[NAP], er_tap[NER];
    float line_g[NLINE], damp, room_a, room_hf, room_main, refl_g, rev_g, wet, dry, er_ap_g, late_ap_g, low_g, low_a;
    // state
    float pre[PRE_LEN]; int pre_pos;
    float line[NLINE][LINE_LEN]; int line_pos[NLINE]; float line_lp[NLINE];
    float ap_buf[NAP][AP_LEN]; Allpass ap[NAP];
    float er_ap_buf[AP_LEN]; Allpass er_ap;
    float room_lp, low_lp[2];
} XReverb;

static float db(float d) { return powf(10.0f, d / 20.0f); }
static float flush(float v) { return fabsf(v) < 1e-15f ? 0.0f : v; }
static float allpass(Allpass *a, float x) {
    float d = a->buf[a->pos];
    float y = d - a->g * x;
    a->buf[a->pos] = flush(x + a->g * y);
    if (++a->pos >= a->len) a->pos = 0;
    return y;
}

void xreverb_set(XReverb *r, const void *params) {
    memcpy(&r->p, params, sizeof r->p);
    const XReverbParams *p = &r->p;
    float fs = r->rate;
    float size = fminf(fmaxf(p->RoomSize, 1.0f), 100.0f);
    float scale = 0.35f + 0.65f * size / 100.0f;                       // small rooms: shorter lines, denser echoes
    static const int base[NLINE] = { 1557, 1617, 1491, 1422, 1277, 1356, 1188, 1116 };   // mutually prime-ish
    float decay = fmaxf(p->DecayTime, 0.1f);
    for (int i = 0; i < NLINE; i++) {
        int L = (int)(base[i] * scale * fs / 44100.0f);
        if (L < 16) L = 16;
        if (L > LINE_LEN - 1) L = LINE_LEN - 1;
        r->line_len[i] = L;
        r->line_g[i] = powf(10.0f, -3.0f * (float)L / (decay * fs));      // -60 dB after DecayTime
    }
    // high-frequency decay: HighEQGain 8 = as fast as the mids (no damping), 0 = much faster; cutoff 1-8 kHz
    float hf = fminf(fmaxf(p->HighEQGain / 8.0f, 0.0f), 1.0f);
    float fc = 1000.0f + 500.0f * fminf(p->HighEQCutoff, 14);
    r->damp = (1.0f - hf) * expf(-2.0f * (float)M_PI * fc / fs);
    // input room filter: high shelf at RoomFilterFreq by RoomFilterHF dB, then RoomFilterMain dB
    r->room_a = expf(-2.0f * (float)M_PI * fminf(fmaxf(p->RoomFilterFreq, 20.0f), 20000.0f) / fs);
    r->room_hf = db(fminf(p->RoomFilterHF, 0.0f));
    r->room_main = db(fminf(p->RoomFilterMain, 0.0f));
    r->refl_g = db(fminf(p->ReflectionsGain, 20.0f));
    r->rev_g = db(fminf(p->ReverbGain, 20.0f)) * 0.35f;                // normalises the 8-line network's output
    float w = fminf(fmaxf(p->WetDryMix, 0.0f), 100.0f) / 100.0f;
    r->wet = w; r->dry = 1.0f - w;
    // pre-delays
    float er_ms = fminf((float)p->ReflectionsDelay, 300.0f), late_ms = er_ms + fminf((float)p->ReverbDelay, 85.0f);
    r->pre_er = (int)(er_ms * fs / 1000.0f); r->pre_late = (int)(late_ms * fs / 1000.0f);
    if (r->pre_late > PRE_LEN - 1) r->pre_late = PRE_LEN - 1;
    if (r->pre_er > r->pre_late) r->pre_er = r->pre_late;
    // early reflections spread over 5-40 ms with the room size
    static const float er_frac[NER] = { 0.11f, 0.23f, 0.37f, 0.52f, 0.71f, 0.95f };
    float span = (5.0f + 35.0f * size / 100.0f) * fs / 1000.0f;
    for (int i = 0; i < NER; i++) { int t = r->pre_er + (int)(er_frac[i] * span); r->er_tap[i] = t < PRE_LEN - 1 ? t : PRE_LEN - 1; }
    // diffusion (0-15) and density (0-100)
    float dens = fminf(fmaxf(p->Density, 0.0f), 100.0f) / 100.0f;
    r->er_ap_g = 0.6f * fminf(p->EarlyDiffusion, 15) / 15.0f;
    r->late_ap_g = 0.7f * fminf(p->LateDiffusion, 15) / 15.0f * (0.4f + 0.6f * dens);
    static const int ap_base[NAP] = { 142, 107, 379, 277 };
    for (int i = 0; i < NAP; i++) {
        int L = (int)(ap_base[i] * scale * fs / 44100.0f); if (L < 8) L = 8; if (L > AP_LEN - 1) L = AP_LEN - 1;
        r->ap[i].len = L; r->ap[i].g = r->late_ap_g;
        if (r->ap[i].pos >= L) r->ap[i].pos = 0;
    }
    r->er_ap.len = (int)(225 * scale * fs / 44100.0f); if (r->er_ap.len < 8) r->er_ap.len = 8;
    r->er_ap.g = r->er_ap_g; if (r->er_ap.pos >= r->er_ap.len) r->er_ap.pos = 0;
    // output low shelf: LowEQGain 8 = 0 dB (0-12 -> -8..+4 dB), cutoff 50-500 Hz
    r->low_g = db((float)fminf(p->LowEQGain, 12) - 8.0f);
    r->low_a = expf(-2.0f * (float)M_PI * (50.0f + 50.0f * fminf(p->LowEQCutoff, 9)) / fs);
    for (int i = 0; i < NLINE; i++) if (r->line_pos[i] >= r->line_len[i]) r->line_pos[i] = 0;
}

// XAUDIO2FX_REVERB_DEFAULT_* values
static const XReverbParams defaults = { 100.0f, 5, 5, 5, 6, 6, 27, 27, 8, 8, 8, 4, 8, 4,
                                        5000.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 100.0f, 100.0f };

XReverb *xreverb_new(float rate) {
    XReverb *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->rate = rate > 0 ? rate : 48000.0f;
    for (int i = 0; i < NAP; i++) r->ap[i].buf = r->ap_buf[i];
    r->er_ap.buf = r->er_ap_buf;
    xreverb_set(r, &defaults);
    return r;
}
void xreverb_free(XReverb *r) { free(r); }
void xreverb_get(const XReverb *r, void *params) { memcpy(params, &r->p, sizeof r->p); }

// n frames: in (in_ch interleaved) -> out (out_ch interleaved, overwritten)
void xreverb_process(XReverb *r, const float *in, int in_ch, float *out, int out_ch, int n) {
    const float inv_sqrt8 = 0.35355339f;
    for (int f = 0; f < n; f++) {
        // dry, as the output channels see it
        float dl, dr;
        if (in_ch == 1) dl = dr = in[f];
        else { dl = in[(size_t)f * in_ch]; dr = in[(size_t)f * in_ch + 1]; }
        float x = in_ch == 1 ? in[f] : 0.5f * (dl + dr);
        // room filter: one-pole split, the high band scaled
        r->room_lp = flush(x + r->room_a * (r->room_lp - x));
        x = (r->room_lp + (x - r->room_lp) * r->room_hf) * r->room_main;
        // pre-delay line
        r->pre[r->pre_pos] = x;
        #define PRE(d) r->pre[(r->pre_pos - (d) + PRE_LEN) & (PRE_LEN - 1)]
        // early reflections, alternating sides
        float el = 0, er = 0;
        for (int i = 0; i < NER; i++) { float t = PRE(r->er_tap[i]) * (1.0f - 0.1f * i); if (i & 1) er += t; else el += t; }
        float ed = allpass(&r->er_ap, 0.5f * (el + er));
        el = (0.7f * el + 0.3f * ed) * r->refl_g * 0.5f;
        er = (0.7f * er + 0.3f * ed) * r->refl_g * 0.5f;
        // late field: diffusers, then the feedback network
        float lin = PRE(r->pre_late);
        for (int i = 0; i < NAP; i++) lin = allpass(&r->ap[i], lin);
        #undef PRE
        r->pre_pos = (r->pre_pos + 1) & (PRE_LEN - 1);
        float o[NLINE];
        for (int i = 0; i < NLINE; i++) {
            float v = r->line[i][r->line_pos[i]];
            r->line_lp[i] = flush(v + r->damp * (r->line_lp[i] - v));     // HF dies faster
            o[i] = r->line_lp[i] * r->line_g[i];
        }
        // 8-point Hadamard (in place, butterflies)
        float h[NLINE]; memcpy(h, o, sizeof h);
        for (int len = 1; len < NLINE; len <<= 1)
            for (int i = 0; i < NLINE; i += len << 1)
                for (int j = i; j < i + len; j++) { float a = h[j], b = h[j + len]; h[j] = a + b; h[j + len] = a - b; }
        for (int i = 0; i < NLINE; i++) {
            r->line[i][r->line_pos[i]] = flush(h[i] * inv_sqrt8 + lin);
            if (++r->line_pos[i] >= r->line_len[i]) r->line_pos[i] = 0;
        }
        float ll = (o[0] - o[2] + o[4] - o[6] + o[1] + o[5]) * r->rev_g;
        float lr = (o[1] - o[3] + o[5] - o[7] + o[2] + o[6]) * r->rev_g;
        // output low shelf on the wet signal
        float wl = el + ll, wr = er + lr;
        r->low_lp[0] = flush(wl + r->low_a * (r->low_lp[0] - wl)); wl += r->low_lp[0] * (r->low_g - 1.0f);
        r->low_lp[1] = flush(wr + r->low_a * (r->low_lp[1] - wr)); wr += r->low_lp[1] * (r->low_g - 1.0f);
        float L = r->dry * dl + r->wet * wl, R = r->dry * dr + r->wet * wr;
        float *po = out + (size_t)f * out_ch;
        if (out_ch == 1) { po[0] = 0.5f * (L + R); continue; }
        po[0] = L; po[1] = R;
        for (int c = 2; c < out_ch; c++) po[c] = 0;
        if (out_ch >= 6) { po[4] = 0.7071f * L; po[5] = 0.7071f * R; }   // rear pair of 5.1 / 7.1
    }
}

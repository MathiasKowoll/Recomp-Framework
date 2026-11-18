// XAudio2 2.7 (the DirectX SDK version, created through CoCreateInstance).
// Voices accept formats, buffers and parameters like the real thing; an audio thread runs a 10 ms
// processing pass that consumes queued buffers in real time (play regions, loops, frequency ratio)
// and calls the game's voice/engine callbacks at the points XAudio2 would. PCM (16-bit, float) source
// voices are mixed to stereo into a CoreAudio output unit; submix voices and effects are not applied.
// RT_AUDIO_TRACE=1 logs the playing voices and the output level every 5 s.
//
// All XAudio2 2.7 structures are tightly packed (#pragma pack(1) in xaudio2.h), hence the odd offsets.
// Callbacks run without the engine lock held (as on Windows), so they may call back into the engine.
#include "rt.h"
#include <pthread.h>
#include <time.h>
#include <mach/mach_time.h>
#include <AudioToolbox/AudioToolbox.h>

#define S_OK 0
#define E_NOINTERFACE 0x80004002u
#define E_INVALIDARG 0x80070057u
#define XAUDIO2_E_INVALID_CALL 0x88960001u
#define XAUDIO2_END_OF_STREAM 0x40
#define XAUDIO2_LOOP_INFINITE 255
#define MAX_QUEUED 64
#define PASS_NS 10000000ull              // 10 ms quantum
#define REGDB_E_CLASSNOTREG 0x80040154u

typedef struct {
    uint32_t flags, bytes; uint64_t data; uint32_t play_begin, play_len, loop_begin, loop_len, loop_count; uint64_t ctx;
    uint64_t total;                       // samples in the buffer (decoded)
} Buf;
enum { V_SOURCE, V_SUBMIX, V_MASTER };
// effects (xaudio2fx XAPOs): created by CoCreateInstance, attached to voices through effect chains
typedef struct XReverb XReverb;
XReverb *xreverb_new(float rate);                                   // xreverb.c
void xreverb_set(XReverb *r, const void *params);
void xreverb_get(const XReverb *r, void *params);
void xreverb_process(XReverb *r, const float *in, int in_ch, float *out, int out_ch, int n);
enum { FX_REVERB = 1, FX_METER = 2 };
typedef struct { uint64_t vtbl; uint32_t refs; int kind; XReverb *rv; } Xapo;
#define REVERB_PARAMS_SIZE 52       // XAUDIO2FX_REVERB_PARAMETERS (2.7)
#define MAXSEND 4
#define MAXFX 4
typedef struct Voice {
    uint64_t vtbl;                        // +0: guest-visible COM vtable pointer
    int kind, dead;
    uint32_t flags, channels, rate, block_align, avg_bytes, samples_per_block, format, float_ext;
    float volume, ratio, max_ratio, chan_vol[8];
    uint8_t filter[12];
    uint64_t callback;                    // IXAudio2VoiceCallback* (guest)
    int started;
    Buf q[MAX_QUEUED]; int nq; int cur_started;   // q[0] is the current buffer once cur_started
    double pos; uint32_t loops_left;      // position in the current buffer (samples)
    uint64_t played;                      // samples played since creation
    uint64_t flushed[MAX_QUEUED]; int nflushed;   // contexts whose OnBufferEnd is still owed
    float in_peak;                        // trace: loudest sample read from its buffers since the last report
    struct Voice *send[MAXSEND]; int nsend;   // outputs: submixes, NULL = the master
    float smat[MAXSEND][64]; uint32_t smat_src[MAXSEND], smat_dst[MAXSEND]; uint8_t smat_set[MAXSEND];   // per output
    struct { Xapo *x; uint64_t gx; int on; uint32_t out_ch; int locked, broken; uint8_t *fmt; } fx[MAXFX]; int nfx;
                                          // effect chain: ours (x) or the game's own IXAPO (gx), run as XAudio2 would
    uint32_t stage;                       // submix processing stage (lower runs first)
    float *bus;                           // submix: this pass's input, PASS_FRAMES x channels
    struct Voice *next;
} Voice;
typedef struct {
    uint64_t vtbl; int refs, running, quit, in_pass;
    uint64_t callbacks[8]; int ncallbacks;
    Voice *voices; Voice *master;
    pthread_t thread; CPU *cpu;
} Engine;

static pthread_mutex_t xmu;
static pthread_cond_t xcv = PTHREAD_COND_INITIALIZER;
static pthread_once_t x_once = PTHREAD_ONCE_INIT;
static uint64_t vt_engine[16], vt_source[29], vt_voice[19];
static __thread int on_audio_thread;

#define THIS_V ((Voice *)PTR(A0))
#define THIS_E ((Engine *)PTR(A0))

// ---------------- guest callbacks ----------------
static void cb_call(CPU *cpu, uint64_t obj, int slot, int n, uint64_t a1, uint64_t a2) {
    if (!obj) return;
    uint64_t fn = rd64(rd64(obj) + 8ull * (uint64_t)slot), a[3] = { obj, a1, a2 };
    if (fn) guest_call(cpu, fn, n, a);
}
// IXAudio2VoiceCallback: 0 PassStart(bytes), 1 PassEnd, 2 StreamEnd, 3 BufferStart(ctx), 4 BufferEnd(ctx), 5 LoopEnd(ctx), 6 VoiceError(ctx, hr)
// IXAudio2EngineCallback: 0 ProcessingPassStart, 1 ProcessingPassEnd, 2 CriticalError(hr)

// ---------------- buffer timing ----------------
static uint64_t buf_samples(const Voice *v, uint32_t bytes, uint64_t wma) {
    if (wma) {   // XAUDIO2_BUFFER_WMA: pDecodedPacketCumulativeBytes +0, PacketCount +8
        uint64_t cum = rd64(wma); uint32_t n = rd32(wma + 8);
        uint32_t dec = n && cum ? rd32(cum + 4ull * (n - 1)) : 0;
        return v->channels ? dec / (2ull * v->channels) : 0;
    }
    if (v->format == 2 && v->block_align)   // ADPCM: whole blocks of samples_per_block
        return (uint64_t)(bytes / v->block_align) * v->samples_per_block;
    if ((v->format == 1 || v->format == 3 || v->format == 0xFFFE) && v->block_align) return bytes / v->block_align;
    return v->avg_bytes ? (uint64_t)bytes * v->rate / v->avg_bytes : 0;
}
static uint64_t play_end(const Buf *b) { return b->play_len ? b->play_begin + b->play_len : b->total; }
static uint64_t loop_end(const Buf *b) { return b->loop_len ? b->loop_begin + b->loop_len : play_end(b); }

// One voice's share of a pass. Events are recorded under the lock and delivered after it is released.
typedef struct { int kind; uint64_t ctx; } Ev;   // kind: 3 BufferStart, 4 BufferEnd, 5 LoopEnd, 2 StreamEnd
static int advance(Voice *v, double step, Ev *ev, int maxev) {
    int n = 0;
    for (int i = 0; i < v->nflushed && n < maxev; i++) ev[n++] = (Ev){ 4, v->flushed[i] };
    v->nflushed = 0;
    while (step > 0 && v->nq && n < maxev - 3) {
        Buf *b = &v->q[0];
        if (!v->cur_started) { v->cur_started = 1; v->pos = b->play_begin; v->loops_left = b->loop_count; ev[n++] = (Ev){ 3, b->ctx }; }
        int looping = v->loops_left > 0 && loop_end(b) > b->loop_begin;
        double end = (double)(looping ? loop_end(b) : play_end(b));
        double left = end - v->pos;
        if (left > step) { v->pos += step; v->played += (uint64_t)step; step = 0; break; }
        if (left > 0) { step -= left; v->played += (uint64_t)left; }
        if (looping) {
            if (v->loops_left != XAUDIO2_LOOP_INFINITE) v->loops_left--;
            v->pos = b->loop_begin;
            ev[n++] = (Ev){ 5, b->ctx };
            if (left <= 0 && step > 0 && loop_end(b) - b->loop_begin < 1) break;
            continue;
        }
        ev[n++] = (Ev){ 4, b->ctx };
        if (b->flags & XAUDIO2_END_OF_STREAM) ev[n++] = (Ev){ 2, 0 };
        memmove(&v->q[0], &v->q[1], sizeof(Buf) * (size_t)(v->nq - 1));
        v->nq--; v->cur_started = 0;
    }
    return n;
}
// ---------------- mixing ----------------
#define OUT_RATE 48000
#define PASS_FRAMES (OUT_RATE / 100)   // 10 ms
static int mixable(const Voice *v) {
    if (!v->channels || v->channels > 8 || !v->block_align) return 0;
    uint32_t bits = v->block_align * 8 / v->channels;
    return (v->format == 1 && bits == 16) || (v->format == 3 && bits == 32) || (v->format == 0xFFFE && (bits == 16 || bits == 32));
}
static float pcm(const Voice *v, uint64_t data, uint64_t frame, uint32_t ch) {
    uint64_t p = data + frame * v->block_align;
    uint32_t bits = v->block_align * 8 / v->channels;
    if (bits == 16) { int16_t x = (int16_t)rd16(p + 2ull * ch); return x / 32768.0f; }
    if (v->format == 3 || (v->format == 0xFFFE && v->float_ext)) return rdf(p + 4ull * ch);
    return (int32_t)rd32(p + 4ull * ch) / 2147483648.0f;
}
// Render n output frames of one source voice into mix (stereo), consuming its buffers like advance().
// Render n frames of one source voice at its own channel count into out (n x channels, interleaved), with its
// volume and channel volumes, consuming its buffers like advance(). Routing to the destination is route()'s job.
static int mix_voice(Voice *v, float *out, int n, Ev *ev, int maxev) {
    int k = 0;
    for (int i = 0; i < v->nflushed && k < maxev; i++) ev[k++] = (Ev){ 4, v->flushed[i] };
    v->nflushed = 0;
    double step = (double)v->rate * (double)v->ratio / OUT_RATE;
    uint32_t sc = v->channels;
    float cv[8]; for (uint32_t c = 0; c < sc && c < 8; c++) cv[c] = v->volume * v->chan_vol[c];
    for (int i = 0; i < n; i++) {
        while (v->nq && k < maxev - 3) {
            Buf *b = &v->q[0];
            if (!v->cur_started) { v->cur_started = 1; v->pos = b->play_begin; v->loops_left = b->loop_count; ev[k++] = (Ev){ 3, b->ctx }; }
            int looping = v->loops_left > 0 && loop_end(b) > b->loop_begin;
            double end = (double)(looping ? loop_end(b) : play_end(b));
            if (v->pos < end) break;
            if (looping) {
                if (v->loops_left != XAUDIO2_LOOP_INFINITE) v->loops_left--;
                v->pos = b->loop_begin + (v->pos - end);
                ev[k++] = (Ev){ 5, b->ctx };
                continue;
            }
            ev[k++] = (Ev){ 4, b->ctx };
            if (b->flags & XAUDIO2_END_OF_STREAM) ev[k++] = (Ev){ 2, 0 };
            double over = v->pos - end;
            memmove(&v->q[0], &v->q[1], sizeof(Buf) * (size_t)(v->nq - 1));
            v->nq--; v->cur_started = 0;
            if (v->nq) { v->cur_started = 1; v->pos = v->q[0].play_begin + over; v->loops_left = v->q[0].loop_count; ev[k++] = (Ev){ 3, v->q[0].ctx }; }
        }
        if (!v->nq) break;   // starved: the rest of the pass is silent
        Buf *b = &v->q[0];
        uint64_t f0 = (uint64_t)v->pos, last = play_end(b) ? play_end(b) - 1 : 0;
        uint64_t f1 = f0 + 1 <= last ? f0 + 1 : f0;
        float t = (float)(v->pos - (double)f0);
        for (uint32_t c = 0; c < sc; c++) {
            float x = pcm(v, b->data, f0, c) * (1 - t) + pcm(v, b->data, f1, c) * t;
            if (fabsf(x) > v->in_peak) v->in_peak = fabsf(x);
            out[(size_t)i * sc + c] += x * cv[c];
        }
        v->pos += step;
        v->played++;
    }
    return k;
}

// ---------------- routing: source -> submix(es) -> master ----------------
// XAudio2's default speaker mapping when no output matrix is set (dest-major: g[d * sc + s]); 5.1/7.1 order is
// FL FR FC LFE BL BR (SL SR)
static void default_matrix(uint32_t sc, uint32_t dc, float *g) {
    memset(g, 0, sizeof(float) * sc * dc);
    if (sc == dc) { for (uint32_t i = 0; i < sc; i++) g[i * sc + i] = 1; return; }
    if (sc == 1) { for (uint32_t d = 0; d < dc; d++) g[d] = d < 2 || dc < 3 ? 1 : 0; return; }
    if (dc == 1) { for (uint32_t c = 0; c < sc; c++) g[c] = 1.0f / sc; return; }
    if (dc == 2) {
        static const float l[8] = { 1, 0, 0.7071f, 0, 1, 0, 1, 0 }, r[8] = { 0, 1, 0.7071f, 0, 0, 1, 0, 1 };
        for (uint32_t c = 0; c < sc && c < 8; c++) { g[c] = l[c]; g[sc + c] = r[c]; }
        return;
    }
    for (uint32_t i = 0; i < sc && i < dc; i++) g[i * sc + i] = 1;
}
// add n frames of in (sc channels) into a bus of dc channels, through output i's matrix (or the default mapping)
static void route(Voice *v, int i, const float *in, uint32_t sc, int n, float *bus, uint32_t dc) {
    float g[64];
    if (v->smat_set[i] && v->smat_src[i] == sc && v->smat_dst[i] == dc) memcpy(g, v->smat[i], sizeof(float) * sc * dc);
    else default_matrix(sc, dc, g);
    for (int f = 0; f < n; f++)
        for (uint32_t d = 0; d < dc; d++) {
            float acc = 0;
            for (uint32_t c = 0; c < sc; c++) acc += in[(size_t)f * sc + c] * g[d * sc + c];
            bus[(size_t)f * dc + d] += acc;
        }
}
// every output of v: a live submix of a later stage (its bus), or the master bus. An output that is no longer
// usable (its voice destroyed) is skipped rather than sent to the master twice.
static void route_all(Voice *v, const float *in, uint32_t sc, int n, float *mbus, uint32_t mch) {
    for (int i = 0; i < v->nsend; i++) {
        Voice *d = v->send[i];
        if (!d) { route(v, i, in, sc, n, mbus, mch); continue; }
        if (d->dead || !d->bus || (v->kind == V_SUBMIX && d->stage <= v->stage)) continue;
        route(v, i, in, sc, n, d->bus, d->channels);
    }
}
// XAUDIO2_VOICE_SENDS (2.7, packed): SendCount +0, pSends +4; XAUDIO2_SEND_DESCRIPTOR: Flags +0, pOutputVoice +4
// (12 bytes). No sends: the master. An output that is not a live submix of the engine means the master.
static Engine *g_engine;
static void parse_sends(Voice *self, uint64_t sends) {
    self->nsend = 0;
    memset(self->smat_set, 0, sizeof self->smat_set);
    uint32_t n = sends ? rd32(sends) : 0; uint64_t arr = sends ? rd64(sends + 4) : 0;
    if (!n || !arr) { self->send[0] = NULL; self->nsend = 1; return; }
    for (uint32_t i = 0; i < n && self->nsend < MAXSEND; i++) {
        Voice *d = (Voice *)PTR(rd64(arr + 12ull * i + 4)), *ok = NULL;
        for (Voice *v = g_engine ? g_engine->voices : NULL; v; v = v->next)
            if (v == d && v != self && !v->dead && v->kind == V_SUBMIX) { ok = v; break; }
        self->send[self->nsend++] = ok;
    }
}
static int send_index(Voice *v, Voice *dest) {   // the output a matrix call names (NULL / the master: the master)
    Voice *want = dest && dest->kind == V_SUBMIX ? dest : NULL;
    for (int i = 0; i < v->nsend; i++) if (v->send[i] == want) return i;
    return -1;
}

// ---------------- effect chains ----------------
static uint64_t vt_xapo[15];
static int fx_trace(void) { static int t = -1; if (t < 0) t = getenv("RT_AUDIO_TRACE") != NULL; return t; }
static const char *guest_class(uint64_t obj);   // an effect object's class name, from its MSVC RTTI
// XAUDIO2_EFFECT_CHAIN (packed): EffectCount +0, pEffectDescriptors +4; descriptor: pEffect +0, InitialState +8,
// OutputChannels +12 (16 bytes). Only effects made by this runtime are run; anything else is skipped.
static void parse_chain(Voice *v, uint64_t ch) {
    uint32_t n = ch ? rd32(ch) : 0; uint64_t d = ch ? rd64(ch + 4) : 0;
    CPU *cpu = rt_cur();
    for (int i = 0; i < v->nfx; i++)   // the previous chain: XAudio2 drops its references
        if (v->fx[i].gx) { if (cpu) { uint64_t a[1] = { v->fx[i].gx }; guest_call(cpu, rd64(rd64(v->fx[i].gx) + 16), 1, a); } free(v->fx[i].fmt); }
    memset(v->fx, 0, sizeof v->fx);
    v->nfx = 0;
    for (uint32_t i = 0; i < n && d && v->nfx < MAXFX; i++) {
        Xapo *x = (Xapo *)PTR(rd64(d + 16ull * i));
        uint64_t gx = 0;
        if (x && x->vtbl != G(vt_xapo)) {   // the game's own IXAPO: run through its vtable
            gx = G(x); x = NULL;
            if (cpu) { uint64_t a[1] = { gx }; guest_call(cpu, rd64(rd64(gx) + 8), 1, a); }   // AddRef, as XAudio2 holds it
            if (fx_trace()) rt_log("[xaudio2]   the game's own effect %s (%p)\n", guest_class(gx), (void *)PTR(gx));
        }
        v->fx[v->nfx].x = x; v->fx[v->nfx].gx = gx; v->fx[v->nfx].on = rd32(d + 16ull * i + 8) != 0;
        uint32_t oc = rd32(d + 16ull * i + 12); v->fx[v->nfx].out_ch = oc <= 8 ? oc : 8;
        v->nfx++;
    }
    if (fx_trace() && n) {
        rt_log("[xaudio2] effect chain on %s voice %p (%u ch, stage %u): %u effect(s)\n",
               v->kind == V_SOURCE ? "source" : v->kind == V_SUBMIX ? "submix" : "master", (void *)v, v->channels, v->stage, n);
        for (int i = 0; i < v->nfx; i++)
            rt_log("[xaudio2]   %d: %s, %s, output %u ch\n", i, v->fx[i].gx ? "the game's" : !v->fx[i].x ? "unknown (skipped)" : v->fx[i].x->kind == FX_REVERB ? "reverb" : "volume meter",
                   v->fx[i].on ? "on" : "off", v->fx[i].out_ch);
    }
}
static const char *guest_class(uint64_t obj) {
    static char name[128];
    snprintf(name, sizeof name, "?");
    uint64_t vt = obj ? rd64(obj) : 0;
    if (vt < 0x140000000ull || vt > 0x150000000ull) return name;
    uint64_t col = rd64(vt - 8);
    if (col < 0x140000000ull || col > 0x150000000ull || rd32(col) != 1) return name;   // x64 locators have signature 1
    uint64_t base = col - rd32(col + 20), td = base + rd32(col + 12);
    const char *n = PTR(td + 16);
    snprintf(name, sizeof name, "%.120s", n);
    return name;
}
static float fx_peak;   // trace: loudest reverb output since the last report
// run v's chain over in (ch channels): returns the output (in, or one of the scratch buffers) and its channel count
static float fx_a[PASS_FRAMES * 8], fx_b[PASS_FRAMES * 8];
// The game's IXAPO, in place: LockForProcess once (48 kHz float, its channel count, PASS_FRAMES frames), then
// Process(1, {buf, VALID, frames}, 1, {buf, ...}, enabled). XAPO structures are packed:
// XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS { pFormat +0, MaxFrameCount +8 }, XAPO_PROCESS_BUFFER_PARAMETERS
// { pBuffer +0, BufferFlags +8, ValidFrameCount +12 }.
static int guest_fx(CPU *cpu, Voice *v, int i, float *buf, uint32_t c) {
    uint64_t gx = v->fx[i].gx, vt = rd64(gx);
    if (v->fx[i].broken || !cpu) return 0;
    if (v->fx[i].out_ch && v->fx[i].out_ch != c) {
        v->fx[i].broken = 1;
        rt_log("[xaudio2] %s changes %u to %u channels: not run\n", guest_class(gx), c, v->fx[i].out_ch);
        return 0;
    }
    if (!v->fx[i].locked) {
        uint8_t *f = v->fx[i].fmt = calloc(1, 48);       // WAVEFORMATEXTENSIBLE, IEEE float
        wr16(G(f), 0xFFFE); wr16(G(f) + 2, (uint16_t)c); wr32(G(f) + 4, OUT_RATE); wr32(G(f) + 8, OUT_RATE * 4 * c);
        wr16(G(f) + 12, (uint16_t)(4 * c)); wr16(G(f) + 14, 32); wr16(G(f) + 16, 22); wr16(G(f) + 18, 32);
        wr32(G(f) + 20, c == 1 ? 0x4 : c == 2 ? 0x3 : (1u << c) - 1);
        static const uint8_t ieee_float[16] = { 3, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71 };
        memcpy(f + 24, ieee_float, 16);
        static uint8_t lin[12], lout[12];
        wr64(G(lin), G(f)); wr32(G(lin) + 8, PASS_FRAMES); memcpy(lout, lin, sizeof lin);
        uint64_t a[5] = { gx, 1, G(lin), 1, G(lout) };
        uint32_t hr = (uint32_t)guest_call(cpu, rd64(vt + 8 * 8), 5, a);   // LockForProcess
        if (hr & 0x80000000u) {
            v->fx[i].broken = 1;
            rt_log("[xaudio2] %s refused 48 kHz float x %u (0x%x): not run\n", guest_class(gx), c, hr);
            return 0;
        }
        v->fx[i].locked = 1;
        if (fx_trace()) rt_log("[xaudio2] %s running on %u ch\n", guest_class(gx), c);
    }
    static uint8_t pin[16], pout[16];
    wr64(G(pin), G(buf)); wr32(G(pin) + 8, 1 /* XAPO_BUFFER_VALID */); wr32(G(pin) + 12, PASS_FRAMES);
    memcpy(pout, pin, sizeof pin);
    uint64_t a[6] = { gx, 1, G(pin), 1, G(pout), (uint64_t)(v->fx[i].on != 0) };
    guest_call(cpu, rd64(vt + 8 * 10), 6, a);                             // Process
    return 1;
}
static const float *apply_fx(CPU *cpu, Voice *v, const float *in, uint32_t *ch) {
    const float *cur = in; uint32_t c = *ch; float *dst = fx_a;
    for (int i = 0; i < v->nfx; i++) {
        if (v->fx[i].gx) {   // in place on a copy (the input may be a voice's own bus)
            memcpy(dst, cur, sizeof(float) * PASS_FRAMES * c);
            if (guest_fx(cpu, v, i, dst, c)) { cur = dst; dst = dst == fx_a ? fx_b : fx_a; }
            continue;
        }
        Xapo *x = v->fx[i].x;
        if (!x || !v->fx[i].on) continue;
        uint32_t oc = v->fx[i].out_ch ? v->fx[i].out_ch : c;
        if (x->kind == FX_REVERB) {
            if (!x->rv) x->rv = xreverb_new(OUT_RATE);
            if (!x->rv) continue;
            xreverb_process(x->rv, cur, (int)(c < 2 ? c : 2), dst, (int)oc, PASS_FRAMES);
            if (fx_trace()) for (int k = 0; k < PASS_FRAMES * (int)oc; k++) { float a = fabsf(dst[k]); if (a > fx_peak) fx_peak = a; }
        } else {   // volume meter: the signal passes through (onto its output channel count)
            if (oc == c) memcpy(dst, cur, sizeof(float) * PASS_FRAMES * c);
            else { float g[64]; default_matrix(c, oc, g);
                for (int f = 0; f < PASS_FRAMES; f++) for (uint32_t d = 0; d < oc; d++) {
                    float acc = 0; for (uint32_t k = 0; k < c; k++) acc += cur[(size_t)f * c + k] * g[d * c + k];
                    dst[(size_t)f * oc + d] = acc; } }
        }
        cur = dst; c = oc; dst = dst == fx_a ? fx_b : fx_a;
    }
    *ch = c;
    return cur;
}

// ---------------- output: CoreAudio unit draining a stereo float ring ----------------
#define RING (OUT_RATE / 2)            // 500 ms
static float ring[RING * 2];
static _Atomic uint64_t ring_w, ring_r;
static AudioComponentInstance out_unit;
static OSStatus out_cb(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *io) {
    float *o = io->mBuffers[0].mData;
    uint64_t r = ring_r, have = ring_w - r, k = have < n ? have : n;
    for (uint64_t i = 0; i < k; i++) { uint64_t j = (r + i) % RING; o[2 * i] = ring[2 * j]; o[2 * i + 1] = ring[2 * j + 1]; }
    memset(o + 2 * k, 0, (n - k) * 8);
    ring_r = r + k;
    return noErr;
}
static int out_open(void) {
    AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    if (!comp || AudioComponentInstanceNew(comp, &out_unit)) { out_unit = NULL; return 0; }
    AudioStreamBasicDescription f = { OUT_RATE, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0 };
    AudioUnitSetProperty(out_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    AURenderCallbackStruct cb = { out_cb, NULL };
    AudioUnitSetProperty(out_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (AudioUnitInitialize(out_unit) || AudioOutputUnitStart(out_unit)) { out_unit = NULL; return 0; }
    return 1;
}

static uint32_t bytes_needed(const Voice *v, double step) {
    double have = 0;
    for (int i = 0; i < v->nq; i++) {
        const Buf *b = &v->q[i];
        if (b->loop_count == XAUDIO2_LOOP_INFINITE) return 0;
        have += (double)(play_end(b) - (i == 0 && v->cur_started ? (uint64_t)v->pos : b->play_begin));
        if (have >= step) return 0;
    }
    double missing = step - have;
    uint32_t per = v->format == 2 && v->samples_per_block ? 0 : v->block_align;
    return per ? (uint32_t)(missing * per) : (uint32_t)(missing * v->avg_bytes / (v->rate ? v->rate : 48000));
}

// ---------------- audio thread ----------------
static void *audio_main(void *p) {
    Engine *e = p;
    on_audio_thread = 1;
    rt_thread_signals();
    rt_set_cur(e->cpu);
    int have_out = out_open();
    pthread_setname_np(have_out ? "XAudio2" : "XAudio2 (silent)");
    rt_log("[xaudio2] output: %s\n", have_out ? "CoreAudio, 48 kHz stereo" : "none (silent timing)");
    uint64_t next = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    for (;;) {
        if (have_out) {   // the device is the clock: keep ~30 ms queued
            while (ring_w - ring_r >= 3 * PASS_FRAMES) { struct timespec ts = { 0, 2000000 }; nanosleep(&ts, NULL); }
        } else {
            next += PASS_NS;
            uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            if (next > now) { struct timespec ts = { 0, (long)(next - now) }; nanosleep(&ts, NULL); }
            else if (now - next > 10 * PASS_NS) next = now;   // fell far behind (debugger, stall): don't burst
        }
        pthread_mutex_lock(&xmu);
        if (e->quit) { pthread_mutex_unlock(&xmu); break; }
        if (!e->running) {
            pthread_mutex_unlock(&xmu);
            if (have_out) { for (int i = 0; i < PASS_FRAMES; i++) { uint64_t j = (ring_w + i) % RING; ring[2 * j] = ring[2 * j + 1] = 0; } ring_w += PASS_FRAMES; }
            continue;
        }
        float master = e->master ? e->master->volume : 1;
        uint32_t mch = e->master && e->master->channels && e->master->channels <= 8 ? e->master->channels : 2;
        static float mix[PASS_FRAMES * 2], mbus[PASS_FRAMES * 8], tmp[PASS_FRAMES * 8];
        memset(mix, 0, sizeof mix); memset(mbus, 0, sizeof mbus);
        e->in_pass = 1;
        uint64_t cbs[8]; int ncb = e->ncallbacks; memcpy(cbs, e->callbacks, sizeof cbs);
        Voice *list[512]; int nv = 0;
        Voice *subs[64]; int ns = 0;
        for (Voice *v = e->voices; v; v = v->next) {
            if (v->kind == V_SOURCE && v->started && !v->dead && nv < 512) list[nv++] = v;
            if (v->kind == V_SUBMIX && !v->dead && v->bus && ns < 64) { subs[ns++] = v; memset(v->bus, 0, sizeof(float) * PASS_FRAMES * v->channels); }
        }
        pthread_mutex_unlock(&xmu);

        for (int i = 0; i < ncb; i++) cb_call(e->cpu, cbs[i], 0, 1, 0, 0);
        for (int i = 0; i < nv; i++) {
            Voice *v = list[i];
            double step = (double)v->rate * (double)v->ratio * (PASS_NS / 1e9);
            cb_call(e->cpu, v->callback, 0, 2, bytes_needed(v, step), 0);
            Ev ev[64];
            pthread_mutex_lock(&xmu);
            int n = 0;
            if (!v->dead && have_out && mixable(v) && v->channels <= 8) {
                memset(tmp, 0, sizeof(float) * PASS_FRAMES * v->channels);
                n = mix_voice(v, tmp, PASS_FRAMES, ev, 64);
                uint32_t oc = v->channels;
                const float *out = apply_fx(e->cpu, v, tmp, &oc);
                route_all(v, out, oc, PASS_FRAMES, mbus, mch);
            } else if (!v->dead) n = advance(v, step, ev, 64);
            pthread_mutex_unlock(&xmu);
            for (int k = 0; k < n; k++) cb_call(e->cpu, v->callback, ev[k].kind, ev[k].kind == 2 ? 1 : 2, ev[k].ctx, 0);
            cb_call(e->cpu, v->callback, 1, 1, 0, 0);
        }
        // submixes, lowest processing stage first: volume, channel volumes, then on to their output
        pthread_mutex_lock(&xmu);
        for (int a = 1; a < ns; a++) { Voice *t = subs[a]; int b = a - 1; while (b >= 0 && subs[b]->stage > t->stage) { subs[b + 1] = subs[b]; b--; } subs[b + 1] = t; }
        for (int a = 0; a < ns; a++) {
            Voice *sm = subs[a];
            if (sm->dead) continue;
            // as XAudio2: the effect chain, then the volume and channel volumes, then each output
            uint32_t sc = sm->channels;
            const float *in = apply_fx(e->cpu, sm, sm->bus, &sc);
            float *o = in == sm->bus ? sm->bus : (float *)in;
            for (int i = 0; i < PASS_FRAMES; i++) for (uint32_t c = 0; c < sc; c++) o[(size_t)i * sc + c] *= sm->volume * (c < 8 ? sm->chan_vol[c] : 1);
            route_all(sm, o, sc, PASS_FRAMES, mbus, mch);
        }
        pthread_mutex_unlock(&xmu);
        // master: its effect chain (e.g. the game's limiter), then down to the stereo device with the master volume
        const float *mo = mbus; uint32_t moc = mch;
        pthread_mutex_lock(&xmu);
        if (e->master && !e->master->dead && e->master->nfx) mo = apply_fx(e->cpu, e->master, mbus, &moc);
        pthread_mutex_unlock(&xmu);
        { float g[64]; default_matrix(moc, 2, g);
          for (int i = 0; i < PASS_FRAMES; i++) for (uint32_t d = 0; d < 2; d++) {
              float acc = 0; for (uint32_t c = 0; c < moc; c++) acc += mo[(size_t)i * moc + c] * g[d * moc + c];
              mix[2 * i + d] = acc * master; } }
        for (int i = 0; i < ncb; i++) cb_call(e->cpu, cbs[i], 1, 1, 0, 0);
        // RT_AUDIO_TRACE=1: every 5 s, the playing voices and the output peak
        { static int tr = -1; static uint64_t passes; static float peak;
          if (tr < 0) tr = getenv("RT_AUDIO_TRACE") != NULL;
          if (tr) {
              for (int i = 0; i < PASS_FRAMES * 2; i++) { float a = mix[i] < 0 ? -mix[i] : mix[i]; if (a > peak) peak = a; }
              if (++passes % 500 == 0) {
                  pthread_mutex_lock(&xmu);
                  int ns = 0; for (Voice *v = e->voices; v; v = v->next) if (v->kind == V_SOURCE) ns++;
                  rt_log("[xaudio2] trace: %d source voices, %d started, master vol %.2f, output peak %.3f, reverb peak %.3f, have_out %d\n", ns, nv, master, peak, fx_peak, have_out);
                  fx_peak = 0;
                  int shown = 0;
                  for (Voice *v = e->voices; v && shown < 12; v = v->next) if (v->kind == V_SOURCE && v->started && !v->dead) {
                      shown++;
                      rt_log("[xaudio2]   voice %p fmt 0x%x %uch %uHz block %u mixable %d vol %.2f chvol %.2f/%.2f ratio %.2f sends %d (matrix %s) effects %d queued %d played %llu, input peak %.3f\n",
                             (void *)v, v->format, v->channels, v->rate, v->block_align, mixable(v), v->volume, v->chan_vol[0], v->chan_vol[1], v->ratio,
                             v->nsend, v->smat_set[0] ? "set" : "default", v->nfx, v->nq, (unsigned long long)v->played, v->in_peak);
                      v->in_peak = 0;
                  }
                  pthread_mutex_unlock(&xmu);
                  peak = 0;
              }
          } }
        if (have_out) {
            for (int i = 0; i < PASS_FRAMES; i++) {
                uint64_t j = (ring_w + i) % RING;
                float l = mix[2 * i], r = mix[2 * i + 1];
                ring[2 * j] = l > 1 ? 1 : l < -1 ? -1 : l; ring[2 * j + 1] = r > 1 ? 1 : r < -1 ? -1 : r;
            }
            ring_w += PASS_FRAMES;
        }

        pthread_mutex_lock(&xmu);
        e->in_pass = 0;
        pthread_cond_broadcast(&xcv);
        pthread_mutex_unlock(&xmu);
    }
    return NULL;
}

// ---------------- IXAudio2Voice (shared by all voice kinds) ----------------
// GetVoiceDetails(this, XAUDIO2_VOICE_DETAILS*): CreationFlags, InputChannels, InputSampleRate
static void v_GetVoiceDetails(CPU *c) { Voice *v = THIS_V; wr32(A1, v->flags); wr32(A1 + 4, v->channels); wr32(A1 + 8, v->rate); }
static void v_SetOutputVoices(CPU *c) { pthread_mutex_lock(&xmu); parse_sends(THIS_V, A1); pthread_mutex_unlock(&xmu); RET(S_OK); }
static void v_SetEffectChain(CPU *c) { pthread_mutex_lock(&xmu); parse_chain(THIS_V, A1); pthread_mutex_unlock(&xmu); RET(S_OK); }
// EnableEffect / DisableEffect(this, index, opset); GetEffectState(this, index, BOOL *on)
static void v_EnableEffect(CPU *c) { Voice *v = THIS_V; if ((uint32_t)A1 >= (uint32_t)v->nfx) { RET(E_INVALIDARG); return; } v->fx[A1].on = 1; RET(S_OK); }
static void v_DisableEffect(CPU *c) { Voice *v = THIS_V; if ((uint32_t)A1 >= (uint32_t)v->nfx) { RET(E_INVALIDARG); return; } v->fx[A1].on = 0; RET(S_OK); }
static void v_GetEffectState(CPU *c) { Voice *v = THIS_V; if (A2) wr32(A2, (uint32_t)A1 < (uint32_t)v->nfx && v->fx[A1].on); }
// effect parameters, through a voice or the effect itself
static void fx_set_params(Xapo *x, uint64_t p, uint32_t sz) {
    if (!x || !p || x->kind != FX_REVERB || sz < REVERB_PARAMS_SIZE) return;
    pthread_mutex_lock(&xmu);
    if (!x->rv) x->rv = xreverb_new(OUT_RATE);
    if (x->rv) xreverb_set(x->rv, PTR(p));
    pthread_mutex_unlock(&xmu);
    if (fx_trace()) {
        static int n; if (n++ < 12)
            rt_log("[xaudio2] reverb %p: wet %.0f%%, reflections %u ms, delay %u ms, decay %.2f s, density %.0f%%, room %.0f ft, reflections %.1f dB, reverb %.1f dB\n",
                   (void *)x, rdf(p), rd32(p + 4), rd8(p + 8), rdf(p + 40), rdf(p + 44), rdf(p + 48), rdf(p + 32), rdf(p + 36));
    }
}
// XAUDIO2FX_VOLUMEMETER_LEVELS (packed): pPeakLevels +0, pRMSLevels +8, ChannelCount +16: no levels measured, zeros
static void fx_get_params(Xapo *x, uint64_t p, uint32_t sz) {
    if (!x || !p) return;
    if (x->kind == FX_REVERB) {
        uint8_t tmp[REVERB_PARAMS_SIZE] = { 0 };
        pthread_mutex_lock(&xmu); if (x->rv) xreverb_get(x->rv, tmp); pthread_mutex_unlock(&xmu);
        memcpy(PTR(p), tmp, sz < REVERB_PARAMS_SIZE ? sz : REVERB_PARAMS_SIZE);
    } else if (sz >= 20) {
        uint64_t peak = rd64(p), rms = rd64(p + 8); uint32_t n = rd32(p + 16);
        for (uint32_t i = 0; i < n && i < 64; i++) { if (peak) wrf(peak + 4ull * i, 0); if (rms) wrf(rms + 4ull * i, 0); }
    }
}
// SetEffectParameters(this, index, params, size, opset) / GetEffectParameters(this, index, params, size)
static void v_SetEffectParameters(CPU *c) {
    Voice *v = THIS_V;
    if ((uint32_t)A1 >= (uint32_t)v->nfx) { RET(E_INVALIDARG); return; }
    fx_set_params(v->fx[A1].x, A2, (uint32_t)A3);
    RET(S_OK);
}
static void v_GetEffectParameters(CPU *c) {
    Voice *v = THIS_V;
    if ((uint32_t)A1 >= (uint32_t)v->nfx) { RET(E_INVALIDARG); return; }
    fx_get_params(v->fx[A1].x, A2, (uint32_t)A3);
    RET(S_OK);
}
// XAUDIO2_FILTER_PARAMETERS: Type, Frequency, OneOverQ (12 bytes)
static void v_SetFilterParameters(CPU *c) { if (A1) memcpy(THIS_V->filter, PTR(A1), 12); RET(S_OK); }
static void v_GetFilterParameters(CPU *c) { if (A1) memcpy(PTR(A1), THIS_V->filter, 12); }
static void v_SetOutputFilterParameters(CPU *c) { RET(S_OK); }
static void v_GetOutputFilterParameters(CPU *c) { if (A2) memcpy(PTR(A2), THIS_V->filter, 12); }
static void v_SetVolume(CPU *c) {
    if (fx_trace() && AF(1) != THIS_V->volume) { static int n; if (n++ < 100) rt_log("[xaudio2] SetVolume(voice %p, %.3f)\n", (void *)THIS_V, AF(1)); }
    THIS_V->volume = AF(1); RET(S_OK);
}
static void v_GetVolume(CPU *c) { if (A1) wrf(A1, THIS_V->volume); }
// SetChannelVolumes(this, channels, const float *volumes, op) / GetChannelVolumes(this, channels, float *volumes)
static void v_SetChannelVolumes(CPU *c) {
    Voice *v = THIS_V;
    for (uint32_t i = 0; i < (uint32_t)A1 && i < 8; i++) v->chan_vol[i] = rdf(A2 + 4ull * i);
    if (fx_trace()) { static int n; if (n++ < 100) rt_log("[xaudio2] SetChannelVolumes(voice %p, %u ch: %.3f %.3f) after %.2f s\n", (void *)v, (uint32_t)A1, v->chan_vol[0], v->chan_vol[1], v->played / (double)OUT_RATE); }
    RET(S_OK);
}
static void v_GetChannelVolumes(CPU *c) { Voice *v = THIS_V; for (uint32_t i = 0; i < (uint32_t)A1; i++) wrf(A2 + 4ull * i, i < 8 ? v->chan_vol[i] : 1); }
// SetOutputMatrix(this, dest, srcCh, dstCh, const float *levels, op) / GetOutputMatrix(this, dest, srcCh, dstCh, float *levels)
static void v_SetOutputMatrix(CPU *c) {
    Voice *v = THIS_V; uint32_t sc = (uint32_t)A2, dc = (uint32_t)A3;
    if (sc * dc > 64 || !AN(4)) { RET(E_INVALIDARG); return; }
    pthread_mutex_lock(&xmu);
    int i = send_index(v, (Voice *)PTR(A1));
    if (i >= 0) { memcpy(v->smat[i], PTR(AN(4)), 4ull * sc * dc); v->smat_src[i] = sc; v->smat_dst[i] = dc; v->smat_set[i] = 1; }
    if (fx_trace()) { static int n; if (n++ < 100) { const float *m = PTR(AN(4));
        rt_log("[xaudio2] SetOutputMatrix(voice %p -> %p, %ux%u, output %d): %.3f %.3f %.3f %.3f\n", (void *)v, PTR(A1), sc, dc, i,
               m[0], sc * dc > 1 ? m[1] : 0, sc * dc > 2 ? m[2] : 0, sc * dc > 3 ? m[3] : 0); } }
    pthread_mutex_unlock(&xmu);
    RET(i >= 0 ? S_OK : E_INVALIDARG);
}
static void v_GetOutputMatrix(CPU *c) {
    Voice *v = THIS_V; uint32_t sc = (uint32_t)A2, dc = (uint32_t)A3; uint64_t out = AN(4);
    pthread_mutex_lock(&xmu);
    int i = send_index(v, (Voice *)PTR(A1));
    float g[64];
    if (i >= 0 && v->smat_set[i] && v->smat_src[i] == sc && v->smat_dst[i] == dc) memcpy(g, v->smat[i], 4ull * sc * dc);
    else if (sc * dc <= 64) default_matrix(sc, dc, g);
    pthread_mutex_unlock(&xmu);
    for (uint32_t k = 0; k < sc * dc && k < 64; k++) wrf(out + 4ull * k, g[k]);
}
static void v_DestroyVoice(CPU *c) {
    Voice *v = THIS_V;
    pthread_mutex_lock(&xmu);
    v->dead = 1;
    for (Voice **pp = &g_engine->voices; *pp; pp = &(*pp)->next) if (*pp == v) { *pp = v->next; break; }
    if (g_engine->master == v) g_engine->master = NULL;
    while (g_engine->in_pass && !on_audio_thread) pthread_cond_wait(&xcv, &xmu);   // the pass may still hold it
    pthread_mutex_unlock(&xmu);
    if (!on_audio_thread) free(v);   // from a callback (not allowed by XAudio2) it is leaked rather than freed under the pass
}

// ---------------- IXAudio2SourceVoice ----------------
static void s_Start(CPU *c) { pthread_mutex_lock(&xmu); THIS_V->started = 1; pthread_mutex_unlock(&xmu); RET(S_OK); }
static void s_Stop(CPU *c) { pthread_mutex_lock(&xmu); THIS_V->started = 0; pthread_mutex_unlock(&xmu); RET(S_OK); }
// SubmitSourceBuffer(this, const XAUDIO2_BUFFER*, const XAUDIO2_BUFFER_WMA*)
// XAUDIO2_BUFFER: Flags +0, AudioBytes +4, pAudioData +8, PlayBegin +16, PlayLength +20, LoopBegin +24,
// LoopLength +28, LoopCount +32, pContext +36
static void s_SubmitSourceBuffer(CPU *c) {
    Voice *v = THIS_V; uint64_t b = A1;
    if (!b) { RET(E_INVALIDARG); return; }
    Buf nb = { rd32(b), rd32(b + 4), rd64(b + 8), rd32(b + 16), rd32(b + 20), rd32(b + 24), rd32(b + 28), rd32(b + 32), rd64(b + 36), 0 };
    nb.total = buf_samples(v, nb.bytes, A2);
    if (nb.play_begin > nb.total) nb.play_begin = (uint32_t)nb.total;
    if (play_end(&nb) > nb.total && nb.total) nb.play_len = (uint32_t)(nb.total - nb.play_begin);
    pthread_mutex_lock(&xmu);
    uint32_t r = S_OK;
    if (v->nq == MAX_QUEUED) r = XAUDIO2_E_INVALID_CALL;
    else v->q[v->nq++] = nb;
    pthread_mutex_unlock(&xmu);
    RET(r);
}
// pending buffers are dropped; their OnBufferEnd comes on the next pass, as on Windows
static void s_FlushSourceBuffers(CPU *c) {
    Voice *v = THIS_V;
    pthread_mutex_lock(&xmu);
    int keep = v->cur_started && v->started ? 1 : 0;
    for (int i = keep; i < v->nq && v->nflushed < MAX_QUEUED; i++) v->flushed[v->nflushed++] = v->q[i].ctx;
    v->nq = keep;
    pthread_mutex_unlock(&xmu);
    RET(S_OK);
}
static void s_Discontinuity(CPU *c) { RET(S_OK); }
static void s_ExitLoop(CPU *c) { pthread_mutex_lock(&xmu); THIS_V->loops_left = 0; pthread_mutex_unlock(&xmu); RET(S_OK); }
// GetState(this, XAUDIO2_VOICE_STATE*): pCurrentBufferContext +0, BuffersQueued +8, SamplesPlayed +12
static void s_GetState(CPU *c) {
    Voice *v = THIS_V;
    pthread_mutex_lock(&xmu);
    wr64(A1, v->nq ? v->q[0].ctx : 0); wr32(A1 + 8, (uint32_t)v->nq); wr64(A1 + 12, v->played);
    pthread_mutex_unlock(&xmu);
}
static void s_SetFrequencyRatio(CPU *c) {
    Voice *v = THIS_V; float r = AF(1);
    if (r < 1.0f / 1024) r = 1.0f / 1024;
    if (r > v->max_ratio) r = v->max_ratio;
    v->ratio = r;
    RET(S_OK);
}
static void s_GetFrequencyRatio(CPU *c) { if (A1) wrf(A1, THIS_V->ratio); }
static void s_SetSourceSampleRate(CPU *c) { Voice *v = THIS_V; if (v->nq) { RET(XAUDIO2_E_INVALID_CALL); return; } v->rate = (uint32_t)A1; RET(S_OK); }

// ---------------- IXAudio2 ----------------
static void e_QueryInterface(CPU *c) {
    // IID_IXAudio2 (2.7) {8BCF1F58-9FE7-4583-8AC6-E2ADC465C8BB}; IUnknown {00000000-0000-0000-C000-000000000046}
    uint32_t d1 = rd32(A1);
    if (d1 == 0x8BCF1F58u || d1 == 0) { THIS_E->refs++; wr64(A2, A0); RET(S_OK); return; }
    wr64(A2, 0); RET(E_NOINTERFACE);
}
static void e_AddRef(CPU *c) { RET((uint32_t)++THIS_E->refs); }
static void e_Release(CPU *c) {
    Engine *e = THIS_E;
    int r = --e->refs;
    if (r == 0) { pthread_mutex_lock(&xmu); e->running = 0; pthread_mutex_unlock(&xmu); rt_log("[xaudio2] engine released\n"); }
    RET((uint32_t)(r > 0 ? r : 0));
}
static void e_GetDeviceCount(CPU *c) { wr32(A1, 1); RET(S_OK); }
// XAUDIO2_DEVICE_DETAILS: DeviceID[256] +0, DisplayName[256] +512, Role +1024, WAVEFORMATEXTENSIBLE OutputFormat +1028
static void e_GetDeviceDetails(CPU *c) {
    if ((uint32_t)A1 != 0) { RET(E_INVALIDARG); return; }
    uint64_t d = A2; memset(PTR(d), 0, 1068);
    const char *id = "macOS-default-output", *name = "macOS Audio Output";
    for (int i = 0; id[i]; i++) wr16(d + 2ull * i, (uint16_t)id[i]);
    for (int i = 0; name[i]; i++) wr16(d + 512 + 2ull * i, (uint16_t)name[i]);
    wr32(d + 1024, 0xF);                                         // GlobalDefaultDevice
    uint64_t f = d + 1028;                                       // 48 kHz stereo float, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
    wr16(f, 0xFFFE); wr16(f + 2, 2); wr32(f + 4, 48000); wr32(f + 8, 48000 * 8); wr16(f + 12, 8); wr16(f + 14, 32); wr16(f + 16, 22);
    wr16(f + 18, 32); wr32(f + 20, 0x3);
    static const uint8_t subtype[16] = { 3, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71 };
    memcpy(PTR(f + 24), subtype, 16);
    RET(S_OK);
}
static void e_Initialize(CPU *c) { RET(S_OK); }
static void e_RegisterForCallbacks(CPU *c) {
    Engine *e = THIS_E;
    pthread_mutex_lock(&xmu); if (e->ncallbacks < 8) e->callbacks[e->ncallbacks++] = A1; pthread_mutex_unlock(&xmu);
    RET(S_OK);
}
static void e_UnregisterForCallbacks(CPU *c) {
    Engine *e = THIS_E;
    pthread_mutex_lock(&xmu);
    for (int i = 0; i < e->ncallbacks; i++) if (e->callbacks[i] == A1) { e->callbacks[i] = e->callbacks[--e->ncallbacks]; break; }
    pthread_mutex_unlock(&xmu);
}
static Voice *new_voice(Engine *e, int kind, uint32_t channels, uint32_t rate, uint32_t flags) {
    Voice *v = calloc(1, sizeof *v);
    v->vtbl = G(kind == V_SOURCE ? vt_source : vt_voice);
    v->kind = kind; v->channels = channels; v->rate = rate; v->flags = flags;
    v->volume = 1; v->ratio = 1; v->max_ratio = 2;
    v->nsend = 1; v->send[0] = NULL;
    for (int i = 0; i < 8; i++) v->chan_vol[i] = 1;
    pthread_mutex_lock(&xmu); v->next = e->voices; e->voices = v; pthread_mutex_unlock(&xmu);
    return v;
}
// CreateSourceVoice(this, **voice, const WAVEFORMATEX*, flags, float maxRatio, callback, sends, effects)
// WAVEFORMATEX: wFormatTag +0, nChannels +2, nSamplesPerSec +4, nAvgBytesPerSec +8, nBlockAlign +12, wBitsPerSample +14, cbSize +16
static void e_CreateSourceVoice(CPU *c) {
    uint64_t fmt = A2, out = A1;   // read now: parse_chain calls into the game (AddRef), which reuses this CPU's registers
    if (!fmt || !out) { RET(E_INVALIDARG); return; }
    Voice *v = new_voice(THIS_E, V_SOURCE, rd16(fmt + 2), rd32(fmt + 4), (uint32_t)A3);
    v->format = rd16(fmt); v->avg_bytes = rd32(fmt + 8); v->block_align = rd16(fmt + 12);
    if (v->format == 2 && rd16(fmt + 16) >= 2) v->samples_per_block = rd16(fmt + 18);   // ADPCMWAVEFORMAT.wSamplesPerBlock
    if (v->format == 0xFFFE && rd16(fmt + 16) >= 22) v->float_ext = rd8(fmt + 24) == 3;   // SubFormat: KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
    float mr = rdf(c->rsp + 8 + 8 * 4);
    v->max_ratio = mr >= 1 ? mr : 2;
    v->callback = AN(5);
    uint64_t sends = AN(6), chain = AN(7);
    pthread_mutex_lock(&xmu); parse_sends(v, sends); parse_chain(v, chain); pthread_mutex_unlock(&xmu);
    wr64(out, G(v));
    static int logged;
    if (logged++ < 8) rt_log("[xaudio2] source voice: format 0x%x, %u ch, %u Hz, block %u%s\n", v->format, v->channels, v->rate, v->block_align, v->callback ? ", callback" : "");
    RET(S_OK);
}
// CreateSubmixVoice(this, **voice, channels, rate, flags, stage, sends, effects)
static void e_CreateSubmixVoice(CPU *c) {
    uint64_t out = A1, sends = AN(6), chain = AN(7);   // read now: parse_chain calls into the game (see above)
    if (!out || !(uint32_t)A2 || (uint32_t)A2 > 8) { RET(E_INVALIDARG); return; }
    Voice *v = new_voice(THIS_E, V_SUBMIX, (uint32_t)A2, (uint32_t)A3, (uint32_t)AN(4));
    pthread_mutex_lock(&xmu);
    v->stage = (uint32_t)AN(5);
    v->bus = calloc((size_t)PASS_FRAMES * v->channels, sizeof(float));
    parse_sends(v, sends);
    parse_chain(v, chain);
    pthread_mutex_unlock(&xmu);
    static int logged; if (logged++ < 16) rt_log("[xaudio2] submix voice: %u ch, %u Hz, stage %u, %d output(s), %d effect(s)\n", v->channels, v->rate, v->stage, v->nsend, v->nfx);
    wr64(out, G(v));
    RET(S_OK);
}
// CreateMasteringVoice(this, **voice, channels, rate, flags, deviceIndex, effects): 0 means the device default
static void e_CreateMasteringVoice(CPU *c) {
    Engine *e = THIS_E;
    if (!A1) { RET(E_INVALIDARG); return; }
    uint32_t ch = (uint32_t)A2 ? (uint32_t)A2 : 2, rate = (uint32_t)A3 ? (uint32_t)A3 : 48000;
    Voice *v = new_voice(e, V_MASTER, ch, rate, (uint32_t)AN(4));
    e->master = v;
    wr64(A1, G(v));
    rt_log("[xaudio2] mastering voice: %u ch, %u Hz\n", ch, rate);
    RET(S_OK);
}
static void e_StartEngine(CPU *c) { pthread_mutex_lock(&xmu); THIS_E->running = 1; pthread_mutex_unlock(&xmu); RET(S_OK); }
static void e_StopEngine(CPU *c) { pthread_mutex_lock(&xmu); THIS_E->running = 0; pthread_mutex_unlock(&xmu); }
static void e_CommitChanges(CPU *c) {
    if (fx_trace()) { static int n; if (n++ < 100) rt_log("[xaudio2] CommitChanges(%u)\n", (uint32_t)A1); }
    RET(S_OK);
}
// XAUDIO2_PERFORMANCE_DATA (2.7, packed: 2 x UINT64 + 11 x UINT32 = 60 bytes): only the voice counts and latency are meaningful
static void e_GetPerformanceData(CPU *c) {
    uint64_t p = A1; memset(PTR(p), 0, 60);
    uint32_t src = 0, active = 0, sub = 0;
    pthread_mutex_lock(&xmu);
    for (Voice *v = THIS_E->voices; v; v = v->next) { if (v->kind == V_SOURCE) { src++; active += v->started; } else if (v->kind == V_SUBMIX) sub++; }
    pthread_mutex_unlock(&xmu);
    wr32(p + 28, 480);                     // CurrentLatencyInSamples
    wr32(p + 36, active); wr32(p + 40, src); wr32(p + 44, sub);
}
static void e_SetDebugConfiguration(CPU *c) { }

// ---------------- creation ----------------
static void build_vtables(void) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&xmu, &a);
    static const gfn eng[16] = { e_QueryInterface, e_AddRef, e_Release, e_GetDeviceCount, e_GetDeviceDetails, e_Initialize,
        e_RegisterForCallbacks, e_UnregisterForCallbacks, e_CreateSourceVoice, e_CreateSubmixVoice, e_CreateMasteringVoice,
        e_StartEngine, e_StopEngine, e_CommitChanges, e_GetPerformanceData, e_SetDebugConfiguration };
    static const gfn voice[19] = { v_GetVoiceDetails, v_SetOutputVoices, v_SetEffectChain, v_EnableEffect, v_DisableEffect,
        v_GetEffectState, v_SetEffectParameters, v_GetEffectParameters, v_SetFilterParameters, v_GetFilterParameters,
        v_SetOutputFilterParameters, v_GetOutputFilterParameters, v_SetVolume, v_GetVolume, v_SetChannelVolumes,
        v_GetChannelVolumes, v_SetOutputMatrix, v_GetOutputMatrix, v_DestroyVoice };
    static const gfn src[10] = { s_Start, s_Stop, s_SubmitSourceBuffer, s_FlushSourceBuffers, s_Discontinuity, s_ExitLoop,
        s_GetState, s_SetFrequencyRatio, s_GetFrequencyRatio, s_SetSourceSampleRate };
    for (int i = 0; i < 16; i++) vt_engine[i] = rt_host_fn(eng[i], "IXAudio2 (2.7)");
    for (int i = 0; i < 19; i++) vt_voice[i] = vt_source[i] = rt_host_fn(voice[i], "IXAudio2Voice");
    for (int i = 0; i < 10; i++) vt_source[19 + i] = rt_host_fn(src[i], "IXAudio2SourceVoice");
}
// ---------------- XAPO effects (xaudio2fx: AudioReverb, AudioVolumeMeter) ----------------
// The silent engine ignores effect chains, so an effect only has to be a well-behaved object: IUnknown, IXAPO and
// IXAPOParameters, passing audio through. Games create these and hand them to SetEffectChain.

static void x_QueryInterface(CPU *c) { wr64(A2, A0); ((Xapo *)PTR(A0))->refs++; RET(S_OK); }
static void x_AddRef(CPU *c) { RET(++((Xapo *)PTR(A0))->refs); }
static void x_Release(CPU *c) { Xapo *x = PTR(A0); RET(x->refs ? --x->refs : 0); }   // kept: a voice may still name it
static void x_GetRegistrationProperties(CPU *c) { if (A1) wr64(A1, 0); RET(0x80004001u); }   // E_NOTIMPL
static void x_IsFormatSupported(CPU *c) { if (A3) wr64(A3, 0); RET(S_OK); }
static void x_Initialize(CPU *c) { RET(S_OK); }
static void x_Nothing(CPU *c) { }
static void x_LockForProcess(CPU *c) { RET(S_OK); }
static void x_CalcFrames(CPU *c) { RET((uint32_t)A1); }
static void x_SetParameters(CPU *c) { fx_set_params((Xapo *)PTR(A0), A1, (uint32_t)A2); }
static void x_GetParameters(CPU *c) { fx_get_params((Xapo *)PTR(A0), A1, (uint32_t)A2); }
static void build_xapo_vtable(void) {
    static const gfn f[15] = { x_QueryInterface, x_AddRef, x_Release, x_GetRegistrationProperties, x_IsFormatSupported,
        x_IsFormatSupported, x_Initialize, x_Nothing /* Reset */, x_LockForProcess, x_Nothing /* UnlockForProcess */,
        x_Nothing /* Process */, x_CalcFrames, x_CalcFrames, x_SetParameters, x_GetParameters };
    for (int i = 0; i < 15; i++) vt_xapo[i] = rt_host_fn(f[i], "IXAPO (pass-through)");
}

// Called by CoCreateInstance. Returns REGDB_E_CLASSNOTREG for classes that are not XAudio2.
uint32_t xaudio2_cocreate(uint64_t clsid, uint64_t iid, uint64_t ppv) {
    uint32_t d1 = rd32(clsid);
    // 2.7 effects: AudioReverb {6A93130E-...} (+ _Debug {C4F82DD4-...}), AudioVolumeMeter {CAC1105F-...} (+ _Debug {2D9A0F9C-...})
    if (d1 == 0x6A93130Eu || d1 == 0xC4F82DD4u || d1 == 0xCAC1105Fu || d1 == 0x2D9A0F9Cu) {
        static pthread_once_t once = PTHREAD_ONCE_INIT;
        pthread_once(&once, build_xapo_vtable);
        Xapo *x = calloc(1, sizeof *x);
        x->vtbl = G(vt_xapo); x->refs = 1;
        x->kind = (d1 == 0x6A93130Eu || d1 == 0xC4F82DD4u) ? FX_REVERB : FX_METER;
        wr64(ppv, G(x));
        return S_OK;
    }
    // CLSID_XAudio2 (2.7) {5A508685-...}, CLSID_XAudio2_Debug (2.7) {DB05EA35-...}
    if (d1 != 0x5A508685u && d1 != 0xDB05EA35u) return REGDB_E_CLASSNOTREG;
    pthread_once(&x_once, build_vtables);
    if (!g_engine) {
        Engine *e = calloc(1, sizeof *e);
        e->vtbl = G(vt_engine); e->running = 1;
        e->cpu = cpu_new(1u << 20);
        pthread_create(&e->thread, NULL, audio_main, e);
        g_engine = e;
        rt_log("[xaudio2] engine created (2.7)\n");
    }
    static int released_before;
    if (g_engine->refs == 0 && released_before++) {
        // The previous instance was fully released: hand out a fresh one, as XAudio2Create would. Its voices die with
        // it (unlinked, not freed: the guest may still hold stale pointers), and a new engine starts out running.
        pthread_mutex_lock(&xmu);
        while (g_engine->in_pass) pthread_cond_wait(&xcv, &xmu);
        for (Voice *v = g_engine->voices; v; v = v->next) v->dead = 1;
        g_engine->voices = NULL; g_engine->master = NULL;
        g_engine->ncallbacks = 0;
        g_engine->running = 1;
        pthread_mutex_unlock(&xmu);
        rt_log("[xaudio2] engine re-created (2.7)\n");
    }
    g_engine->refs++;
    wr64(ppv, G(g_engine));
    return S_OK;
}

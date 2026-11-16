// XAudio2 2.7 (the DirectX SDK version, created through CoCreateInstance) as a silent engine.
// Voices accept formats, buffers and parameters like the real thing; an audio thread runs a 10 ms
// processing pass that consumes queued buffers in real time (play regions, loops, frequency ratio)
// and calls the game's voice/engine callbacks at the points XAudio2 would. No sound is produced yet:
// a mixer can later render the same buffers into CoreAudio.
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
typedef struct Voice {
    uint64_t vtbl;                        // +0: guest-visible COM vtable pointer
    int kind, dead;
    uint32_t flags, channels, rate, block_align, avg_bytes, samples_per_block, format, float_ext;
    float volume, ratio, max_ratio, chan_vol[8], matrix[64]; uint32_t mat_src, mat_dst;
    uint8_t filter[12];
    uint64_t callback;                    // IXAudio2VoiceCallback* (guest)
    int started;
    Buf q[MAX_QUEUED]; int nq; int cur_started;   // q[0] is the current buffer once cur_started
    double pos; uint32_t loops_left;      // position in the current buffer (samples)
    uint64_t played;                      // samples played since creation
    uint64_t flushed[MAX_QUEUED]; int nflushed;   // contexts whose OnBufferEnd is still owed
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
static int mix_voice(Voice *v, float *mix, int n, float master, Ev *ev, int maxev) {
    int k = 0;
    for (int i = 0; i < v->nflushed && k < maxev; i++) ev[k++] = (Ev){ 4, v->flushed[i] };
    v->nflushed = 0;
    double step = (double)v->rate * (double)v->ratio / OUT_RATE;
    // gains: output matrix (dest-major: level[d * src + s]) when set for 2 outputs, else mono->both / stereo direct
    float g[2][8] = { { 0 } };
    uint32_t sc = v->channels;
    if (v->mat_dst == 2 && v->mat_src == sc) for (uint32_t d = 0; d < 2; d++) for (uint32_t c = 0; c < sc; c++) g[d][c] = v->matrix[d * sc + c];
    else if (sc == 1) g[0][0] = g[1][0] = 1;
    else { g[0][0] = g[1][1] = 1; }
    float vol = v->volume * master;
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
            mix[2 * i] += x * g[0][c] * vol; mix[2 * i + 1] += x * g[1][c] * vol;
        }
        v->pos += step;
        v->played++;
    }
    return k;
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
        static float mix[PASS_FRAMES * 2];
        memset(mix, 0, sizeof mix);
        e->in_pass = 1;
        uint64_t cbs[8]; int ncb = e->ncallbacks; memcpy(cbs, e->callbacks, sizeof cbs);
        Voice *list[512]; int nv = 0;
        for (Voice *v = e->voices; v && nv < 512; v = v->next) if (v->kind == V_SOURCE && v->started && !v->dead) list[nv++] = v;
        pthread_mutex_unlock(&xmu);

        for (int i = 0; i < ncb; i++) cb_call(e->cpu, cbs[i], 0, 1, 0, 0);
        for (int i = 0; i < nv; i++) {
            Voice *v = list[i];
            double step = (double)v->rate * (double)v->ratio * (PASS_NS / 1e9);
            cb_call(e->cpu, v->callback, 0, 2, bytes_needed(v, step), 0);
            Ev ev[64];
            pthread_mutex_lock(&xmu);
            int n = v->dead ? 0 : have_out && mixable(v) ? mix_voice(v, mix, PASS_FRAMES, master, ev, 64) : advance(v, step, ev, 64);
            pthread_mutex_unlock(&xmu);
            for (int k = 0; k < n; k++) cb_call(e->cpu, v->callback, ev[k].kind, ev[k].kind == 2 ? 1 : 2, ev[k].ctx, 0);
            cb_call(e->cpu, v->callback, 1, 1, 0, 0);
        }
        for (int i = 0; i < ncb; i++) cb_call(e->cpu, cbs[i], 1, 1, 0, 0);
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
static Engine *g_engine;
// GetVoiceDetails(this, XAUDIO2_VOICE_DETAILS*): CreationFlags, InputChannels, InputSampleRate
static void v_GetVoiceDetails(CPU *c) { Voice *v = THIS_V; wr32(A1, v->flags); wr32(A1 + 4, v->channels); wr32(A1 + 8, v->rate); }
static void v_SetOutputVoices(CPU *c) { RET(S_OK); }
static void v_SetEffectChain(CPU *c) { RET(S_OK); }
static void v_EnableEffect(CPU *c) { RET(S_OK); }
static void v_DisableEffect(CPU *c) { RET(S_OK); }
static void v_GetEffectState(CPU *c) { if (A2) wr32(A2, 1); }
static void v_SetEffectParameters(CPU *c) { RET(S_OK); }
static void v_GetEffectParameters(CPU *c) { if (A2 && A3) memset(PTR(A2), 0, (uint32_t)A3); RET(S_OK); }
// XAUDIO2_FILTER_PARAMETERS: Type, Frequency, OneOverQ (12 bytes)
static void v_SetFilterParameters(CPU *c) { if (A1) memcpy(THIS_V->filter, PTR(A1), 12); RET(S_OK); }
static void v_GetFilterParameters(CPU *c) { if (A1) memcpy(PTR(A1), THIS_V->filter, 12); }
static void v_SetOutputFilterParameters(CPU *c) { RET(S_OK); }
static void v_GetOutputFilterParameters(CPU *c) { if (A2) memcpy(PTR(A2), THIS_V->filter, 12); }
static void v_SetVolume(CPU *c) { THIS_V->volume = AF(1); RET(S_OK); }
static void v_GetVolume(CPU *c) { if (A1) wrf(A1, THIS_V->volume); }
// SetChannelVolumes(this, channels, const float *volumes, op) / GetChannelVolumes(this, channels, float *volumes)
static void v_SetChannelVolumes(CPU *c) { Voice *v = THIS_V; for (uint32_t i = 0; i < (uint32_t)A1 && i < 8; i++) v->chan_vol[i] = rdf(A2 + 4ull * i); RET(S_OK); }
static void v_GetChannelVolumes(CPU *c) { Voice *v = THIS_V; for (uint32_t i = 0; i < (uint32_t)A1; i++) wrf(A2 + 4ull * i, i < 8 ? v->chan_vol[i] : 1); }
// SetOutputMatrix(this, dest, srcCh, dstCh, const float *levels, op) / GetOutputMatrix(this, dest, srcCh, dstCh, float *levels)
static void v_SetOutputMatrix(CPU *c) {
    Voice *v = THIS_V; uint32_t s = (uint32_t)A2, d = (uint32_t)A3;
    if (s * d <= 64 && AN(4)) { memcpy(v->matrix, PTR(AN(4)), 4ull * s * d); v->mat_src = s; v->mat_dst = d; }
    RET(S_OK);
}
static void v_GetOutputMatrix(CPU *c) {
    Voice *v = THIS_V; uint32_t s = (uint32_t)A2, d = (uint32_t)A3; uint64_t out = AN(4);
    for (uint32_t i = 0; i < s * d; i++)
        wrf(out + 4ull * i, (s == v->mat_src && d == v->mat_dst) ? v->matrix[i] : (i / s == i % s ? 1.0f : 0.0f));
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
    for (int i = 0; i < 8; i++) v->chan_vol[i] = 1;
    pthread_mutex_lock(&xmu); v->next = e->voices; e->voices = v; pthread_mutex_unlock(&xmu);
    return v;
}
// CreateSourceVoice(this, **voice, const WAVEFORMATEX*, flags, float maxRatio, callback, sends, effects)
// WAVEFORMATEX: wFormatTag +0, nChannels +2, nSamplesPerSec +4, nAvgBytesPerSec +8, nBlockAlign +12, wBitsPerSample +14, cbSize +16
static void e_CreateSourceVoice(CPU *c) {
    uint64_t fmt = A2;
    if (!fmt || !A1) { RET(E_INVALIDARG); return; }
    Voice *v = new_voice(THIS_E, V_SOURCE, rd16(fmt + 2), rd32(fmt + 4), (uint32_t)A3);
    v->format = rd16(fmt); v->avg_bytes = rd32(fmt + 8); v->block_align = rd16(fmt + 12);
    if (v->format == 2 && rd16(fmt + 16) >= 2) v->samples_per_block = rd16(fmt + 18);   // ADPCMWAVEFORMAT.wSamplesPerBlock
    if (v->format == 0xFFFE && rd16(fmt + 16) >= 22) v->float_ext = rd8(fmt + 24) == 3;   // SubFormat: KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
    float mr = rdf(c->rsp + 8 + 8 * 4);
    v->max_ratio = mr >= 1 ? mr : 2;
    v->callback = AN(5);
    wr64(A1, G(v));
    static int logged;
    if (logged++ < 8) rt_log("[xaudio2] source voice: format 0x%x, %u ch, %u Hz, block %u%s\n", v->format, v->channels, v->rate, v->block_align, v->callback ? ", callback" : "");
    RET(S_OK);
}
// CreateSubmixVoice(this, **voice, channels, rate, flags, stage, sends, effects)
static void e_CreateSubmixVoice(CPU *c) { if (!A1) { RET(E_INVALIDARG); return; } wr64(A1, G(new_voice(THIS_E, V_SUBMIX, (uint32_t)A2, (uint32_t)A3, (uint32_t)AN(4)))); RET(S_OK); }
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
static void e_CommitChanges(CPU *c) { RET(S_OK); }
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
// Called by CoCreateInstance. Returns REGDB_E_CLASSNOTREG for classes that are not XAudio2.
uint32_t xaudio2_cocreate(uint64_t clsid, uint64_t iid, uint64_t ppv) {
    uint32_t d1 = rd32(clsid);
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
    g_engine->refs++;
    wr64(ppv, G(g_engine));
    return S_OK;
}

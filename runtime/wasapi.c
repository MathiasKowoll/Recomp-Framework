// WASAPI (shared-mode render) on CoreAudio. The game's audio engine (Wwise, FMOD, ...) mixes everything itself
// and writes finished PCM into IAudioRenderClient buffers; this plays them through a CoreAudio output
// unit. The output unit is the clock: as it consumes audio it signals the client's buffer event, which
// is what drives the engine's rendering. Capture devices are reported as absent.
//
// Objects are guest-visible COM objects (vtable pointer at +0) whose methods are host functions.
#include "rt.h"
#include "handles.h"
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>

#define S_OK 0
#define S_FALSE 1
#define E_NOINTERFACE 0x80004002u
#define E_POINTER 0x80004003u
#define E_NOTFOUND 0x80070490u
#define AUDCLNT_E_NOT_INITIALIZED 0x88890001u
#define AUDCLNT_E_BUFFER_TOO_LARGE 0x88890006u
#define AUDCLNT_BUFFERFLAGS_SILENT 2
#define RATE 48000
#define MAXFRAMES (RATE * 2)            // ring capacity: up to 2 s

typedef struct { uint64_t vtbl; int refs; } Obj;
static uint64_t vt_enum[8], vt_dev[7], vt_coll[5], vt_props[8], vt_client[15], vt_render[5], vt_clock[6];
static Obj enumerator, device, collection_render, collection_empty, props, render, aclock;

// ---------------- the audio client (one stream) ----------------
static struct {
    Obj obj;
    int initialized, running;
    uint32_t channels, bits, is_float, block_align;   // client format
    uint32_t buffer_frames;
    uint64_t event;                                   // guest event handle (SetEventHandle)
    float ring[MAXFRAMES * 2];                        // stereo float
    _Atomic uint64_t wpos, rpos;                      // frames written / played
    uint8_t scratch[MAXFRAMES * 32];                  // GetBuffer hands this out (client format)
    AudioComponentInstance unit;
    uint64_t played_total;
} cl;

static void signal_event(uint64_t h) {
    if (!h) return;
    h_lock(); HObj *o = h_get(h); if (o && o->type == HT_EVENT) { o->state = 1; h_wake(o); } h_unlock();
}
static OSStatus render_cb(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 n, AudioBufferList *io) {
    float *out = io->mBuffers[0].mData;
    uint64_t r = cl.rpos, w = cl.wpos, have = w - r, k = have < n ? have : n;
    for (uint64_t i = 0; i < k; i++) { uint64_t j = (r + i) % MAXFRAMES; out[2 * i] = cl.ring[2 * j]; out[2 * i + 1] = cl.ring[2 * j + 1]; }
    memset(out + 2 * k, 0, (n - k) * 8);
    cl.rpos = r + k;
    cl.played_total += n;
    signal_event(cl.event);   // room for more: wake the renderer
    return noErr;
}
static int unit_open(void) {
    if (cl.unit) return 1;
    AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    if (!comp || AudioComponentInstanceNew(comp, &cl.unit)) { rt_log("[wasapi] no CoreAudio output unit\n"); cl.unit = NULL; return 0; }
    AudioStreamBasicDescription f = { RATE, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0 };
    AudioUnitSetProperty(cl.unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    AURenderCallbackStruct cb = { render_cb, NULL };
    AudioUnitSetProperty(cl.unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (AudioUnitInitialize(cl.unit)) { rt_log("[wasapi] AudioUnitInitialize failed\n"); return 0; }
    return 1;
}

// client format -> stereo float into the ring
static float sample(const uint8_t *p, uint32_t ch) {
    if (cl.is_float) { float v; memcpy(&v, p + 4 * ch, 4); return v; }
    if (cl.bits == 16) { int16_t v; memcpy(&v, p + 2 * ch, 2); return v / 32768.0f; }
    if (cl.bits == 32) { int32_t v; memcpy(&v, p + 4 * ch, 4); return v / 2147483648.0f; }
    return 0;
}
static void push_frames(const uint8_t *src, uint32_t n, int silent) {
    uint64_t w = cl.wpos;
    for (uint32_t i = 0; i < n; i++) {
        float l = 0, r = 0;
        if (!silent) {
            const uint8_t *p = src + (size_t)i * cl.block_align;
            if (cl.channels == 1) l = r = sample(p, 0);
            else {
                l = sample(p, 0); r = sample(p, 1);
                if (cl.channels >= 3) { float cn = 0.707f * sample(p, 2); l += cn; r += cn; }            // center
                if (cl.channels >= 6) { l += 0.5f * sample(p, 4); r += 0.5f * sample(p, 5); }          // back L/R (5.1)
                if (cl.channels >= 8) { l += 0.5f * sample(p, 6); r += 0.5f * sample(p, 7); }          // side L/R (7.1)
            }
        }
        uint64_t j = (w + i) % MAXFRAMES;
        cl.ring[2 * j] = l; cl.ring[2 * j + 1] = r;
    }
    cl.wpos = w + n;
}

// ---------------- formats ----------------
// WAVEFORMATEXTENSIBLE (40 bytes): 48 kHz stereo float, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
static uint64_t mix_format(void) {
    uint8_t *f = calloc(1, 40); uint64_t p = G(f);
    wr16(p, 0xFFFE); wr16(p + 2, 2); wr32(p + 4, RATE); wr32(p + 8, RATE * 8); wr16(p + 12, 8); wr16(p + 14, 32); wr16(p + 16, 22);
    wr16(p + 18, 32); wr32(p + 20, 3);
    static const uint8_t sub[16] = { 3, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71 };
    memcpy(f + 24, sub, 16);
    return p;
}
// is a guest WAVEFORMATEX(TENSIBLE) something we can play? sets the client format fields if so
static int accept_format(uint64_t f, int apply) {
    uint16_t tag = rd16(f), ch = rd16(f + 2), bits = rd16(f + 14); uint32_t rate = rd32(f + 4);
    int is_float = tag == 3;
    if (tag == 0xFFFE && rd16(f + 16) >= 22) is_float = rd8(f + 24) == 3;   // SubFormat GUID Data1: 3 float, 1 PCM
    int ok = rate == RATE && ch >= 1 && ch <= 8 && ((is_float && bits == 32) || (!is_float && (bits == 16 || bits == 32)));
    if (ok && apply) { cl.channels = ch; cl.bits = bits; cl.is_float = is_float; cl.block_align = rd16(f + 12); }
    return ok;
}

// ---------------- IUnknown helpers ----------------
static void obj_addref(CPU *c) { RET((uint32_t)++((Obj *)PTR(A0))->refs); }
static void obj_release(CPU *c) { Obj *o = PTR(A0); RET((uint32_t)(o->refs > 1 ? --o->refs : 1)); }
static void obj_qi(CPU *c) { ((Obj *)PTR(A0))->refs++; wr64(A2, A0); RET(S_OK); }
static void ok(CPU *c) { RET(S_OK); }
static void notimpl(CPU *c) { RET(0x80004001u); }

// ---------------- IMMDeviceEnumerator ----------------
// EnumAudioEndpoints(flow, mask, **collection): render -> one device, capture -> none
static void en_EnumAudioEndpoints(CPU *c) { wr64(A3, (uint32_t)A1 == 0 || (uint32_t)A1 == 2 ? G(&collection_render) : G(&collection_empty)); RET(S_OK); }
// GetDefaultAudioEndpoint(flow, role, **device)
static void en_GetDefaultAudioEndpoint(CPU *c) {
    if ((uint32_t)A1 != 0) { wr64(A3, 0); RET(E_NOTFOUND); return; }   // no capture device
    wr64(A3, G(&device)); RET(S_OK);
}
static void en_GetDevice(CPU *c) { wr64(A2, G(&device)); RET(S_OK); }

// ---------------- IMMDeviceCollection ----------------
static void co_GetCount(CPU *c) { wr32(A1, A0 == G(&collection_render) ? 1 : 0); RET(S_OK); }
static void co_Item(CPU *c) { if (A0 != G(&collection_render) || A1) { wr64(A2, 0); RET(0x80070057u); return; } wr64(A2, G(&device)); RET(S_OK); }

// ---------------- IMMDevice ----------------
// Activate(iid, clsctx, params, **iface): IAudioClient {1CB9AD4C-...}
static void dv_Activate(CPU *c) {
    uint32_t d1 = rd32(A1);
    if (d1 == 0x1CB9AD4Cu) { cl.obj.refs++; wr64(AN(4), G(&cl.obj)); RET(S_OK); return; }
    rt_log("[wasapi] IMMDevice::Activate({%08X-...}) not supported\n", d1);
    wr64(AN(4), 0); RET(E_NOINTERFACE);
}
static void dv_OpenPropertyStore(CPU *c) { wr64(A2, G(&props)); RET(S_OK); }
static void dv_GetId(CPU *c) { wr64(A1, G(utf8_to_w16("{0.0.0.00000000}.{macos-default-output}"))); RET(S_OK); }
static void dv_GetState(CPU *c) { wr32(A1, 1); RET(S_OK); }   // DEVICE_STATE_ACTIVE

// ---------------- IPropertyStore: friendly name and device format ----------------
// GetValue(PROPERTYKEY*, PROPVARIANT*): vt +0, value +8 (blob: cbSize +8, pBlobData +16)
static void ps_GetValue(CPU *c) {
    uint64_t key = A1, pv = A2; uint32_t d1 = rd32(key), pid = rd32(key + 16);
    memset(PTR(pv), 0, 24);
    if (d1 == 0xA45C254Eu || d1 == 0xB3F8FA53u || d1 == 0x026E516Eu) {   // PKEY_Device_FriendlyName / DeviceDesc / DeviceInterface_FriendlyName
        wr16(pv, 31); wr64(pv + 8, G(utf8_to_w16("Speakers (macOS)"))); RET(S_OK); return;   // VT_LPWSTR
    }
    if (d1 == 0xF19F064Du && pid == 0) { wr16(pv, 65); wr32(pv + 8, 40); wr64(pv + 16, mix_format()); RET(S_OK); return; }   // VT_BLOB
    RET(S_OK);   // VT_EMPTY
}
static void ps_GetCount(CPU *c) { wr32(A1, 0); RET(S_OK); }

// ---------------- IAudioClient ----------------
// Initialize(this, shareMode, flags, bufferDuration, periodicity, format, session)
static void ac_Initialize(CPU *c) {
    if (!accept_format(AN(5), 1)) { rt_log("[wasapi] Initialize: unsupported format\n"); RET(0x88890008u); return; }   // AUDCLNT_E_UNSUPPORTED_FORMAT
    int64_t dur = (int64_t)A3;
    uint32_t frames = (uint32_t)(dur > 0 ? (uint64_t)dur * RATE / 10000000 : RATE / 10);
    if (frames < RATE / 50) frames = RATE / 50;   // at least 20 ms
    if (frames > MAXFRAMES / 2) frames = MAXFRAMES / 2;
    cl.buffer_frames = frames; cl.wpos = cl.rpos = 0;
    cl.initialized = unit_open();
    rt_log("[wasapi] Initialize: %u ch, %u-bit %s, %u Hz, buffer %u frames%s -> %s\n", cl.channels, cl.bits, cl.is_float ? "float" : "int",
           RATE, frames, ((uint32_t)A2 & 0x40000) ? ", event-driven" : "", cl.initialized ? "CoreAudio" : "no output");
    RET(cl.initialized ? S_OK : 0x80004005u);
}
static void ac_GetBufferSize(CPU *c) { if (!cl.initialized) { RET(AUDCLNT_E_NOT_INITIALIZED); return; } wr32(A1, cl.buffer_frames); RET(S_OK); }
static void ac_GetStreamLatency(CPU *c) { wr64(A1, 100000); RET(S_OK); }   // 10 ms (100 ns units)
static void ac_GetCurrentPadding(CPU *c) {
    if (!cl.initialized) { RET(AUDCLNT_E_NOT_INITIALIZED); return; }
    uint64_t pad = cl.wpos - cl.rpos;
    wr32(A1, (uint32_t)(pad < cl.buffer_frames ? pad : cl.buffer_frames)); RET(S_OK);
}
// IsFormatSupported(shareMode, format, **closest)
static void ac_IsFormatSupported(CPU *c) {
    if (accept_format(A2, 0)) { if (A3) wr64(A3, 0); RET(S_OK); return; }
    if (A3) wr64(A3, mix_format());
    RET(S_FALSE);
}
static void ac_GetMixFormat(CPU *c) { wr64(A1, mix_format()); RET(S_OK); }
static void ac_GetDevicePeriod(CPU *c) { if (A1) wr64(A1, 100000); if (A2) wr64(A2, 30000); RET(S_OK); }
static void ac_Start(CPU *c) {
    if (!cl.initialized) { RET(AUDCLNT_E_NOT_INITIALIZED); return; }
    if (!cl.running && AudioOutputUnitStart(cl.unit) == noErr) cl.running = 1;
    signal_event(cl.event);
    RET(S_OK);
}
static void ac_Stop(CPU *c) { if (cl.running) { AudioOutputUnitStop(cl.unit); cl.running = 0; } RET(S_OK); }
static void ac_Reset(CPU *c) { cl.rpos = cl.wpos; RET(S_OK); }
static void ac_SetEventHandle(CPU *c) { cl.event = A1; RET(S_OK); }
// GetService(riid, **svc): IAudioRenderClient {F294ACFC-...}, IAudioClock {CD63314F-...}
static void ac_GetService(CPU *c) {
    uint32_t d1 = rd32(A1);
    if (d1 == 0xF294ACFCu) { wr64(A2, G(&render)); RET(S_OK); return; }
    if (d1 == 0xCD63314Fu) { wr64(A2, G(&aclock)); RET(S_OK); return; }
    wr64(A2, 0); RET(E_NOINTERFACE);
}

// ---------------- IAudioRenderClient ----------------
static uint32_t pending;
// GetBuffer(frames, **data)
static void rc_GetBuffer(CPU *c) {
    uint32_t n = (uint32_t)A1;
    uint64_t pad = cl.wpos - cl.rpos;
    if (n + pad > cl.buffer_frames || (uint64_t)n * cl.block_align > sizeof cl.scratch) { RET(AUDCLNT_E_BUFFER_TOO_LARGE); return; }
    pending = n; wr64(A2, G(cl.scratch)); RET(S_OK);
}
// ReleaseBuffer(frames, flags)
static void rc_ReleaseBuffer(CPU *c) {
    uint32_t n = (uint32_t)A1;
    if (n > pending) n = pending;
    push_frames(cl.scratch, n, ((uint32_t)A2 & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
    pending = 0; RET(S_OK);
}

// ---------------- IAudioClock: position in frames ----------------
static void ck_GetFrequency(CPU *c) { wr64(A1, RATE); RET(S_OK); }
static void ck_GetPosition(CPU *c) { wr64(A1, cl.rpos); if (A2) wr64(A2, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 100); RET(S_OK); }
static void ck_GetCharacteristics(CPU *c) { wr32(A1, 0); RET(S_OK); }

// ---------------- creation ----------------
static void fill(uint64_t *vt, int n, const gfn *f, const char *name) { for (int i = 0; i < n; i++) vt[i] = rt_host_fn(f[i], name); }
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void build(void) {
    const gfn en[8] = { obj_qi, obj_addref, obj_release, en_EnumAudioEndpoints, en_GetDefaultAudioEndpoint, en_GetDevice, ok, ok };
    const gfn dv[7] = { obj_qi, obj_addref, obj_release, dv_Activate, dv_OpenPropertyStore, dv_GetId, dv_GetState };
    const gfn co[5] = { obj_qi, obj_addref, obj_release, co_GetCount, co_Item };
    const gfn ps[8] = { obj_qi, obj_addref, obj_release, ps_GetCount, notimpl, ps_GetValue, notimpl, notimpl };
    const gfn ac[15] = { obj_qi, obj_addref, obj_release, ac_Initialize, ac_GetBufferSize, ac_GetStreamLatency, ac_GetCurrentPadding,
                         ac_IsFormatSupported, ac_GetMixFormat, ac_GetDevicePeriod, ac_Start, ac_Stop, ac_Reset, ac_SetEventHandle, ac_GetService };
    const gfn rc[5] = { obj_qi, obj_addref, obj_release, rc_GetBuffer, rc_ReleaseBuffer };
    const gfn ck[6] = { obj_qi, obj_addref, obj_release, ck_GetFrequency, ck_GetPosition, ck_GetCharacteristics };
    fill(vt_enum, 8, en, "IMMDeviceEnumerator"); fill(vt_dev, 7, dv, "IMMDevice"); fill(vt_coll, 5, co, "IMMDeviceCollection");
    fill(vt_props, 8, ps, "IPropertyStore"); fill(vt_client, 15, ac, "IAudioClient"); fill(vt_render, 5, rc, "IAudioRenderClient");
    fill(vt_clock, 6, ck, "IAudioClock");
    enumerator = (Obj){ G(vt_enum), 1 }; device = (Obj){ G(vt_dev), 1 }; collection_render = (Obj){ G(vt_coll), 1 };
    collection_empty = (Obj){ G(vt_coll), 1 }; props = (Obj){ G(vt_props), 1 }; cl.obj = (Obj){ G(vt_client), 1 };
    render = (Obj){ G(vt_render), 1 }; aclock = (Obj){ G(vt_clock), 1 };
}
// CoCreateInstance hook: CLSID_MMDeviceEnumerator {BCDE0395-E52F-467C-8E3D-C4579291692E}
uint32_t wasapi_cocreate(uint64_t clsid, uint64_t iid, uint64_t ppv) {
    if (rd32(clsid) != 0xBCDE0395u) return 0x80040154u;   // REGDB_E_CLASSNOTREG
    pthread_once(&once, build);
    enumerator.refs++;
    wr64(ppv, G(&enumerator));
    return S_OK;
}

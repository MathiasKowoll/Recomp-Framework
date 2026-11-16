// dinput8.dll: DirectInput 8 keyboard and mouse, fed by the AppKit events window.c/cocoa.m translate.
// The engine reads keyboard and mouse through DirectInput, not window messages.
//   keyboard: 256-byte DIK state (DIK codes are set-1 scancodes, extended keys | 0x80) + buffered events
//   mouse:    relative X/Y/wheel and 8 buttons (DIMOUSESTATE/DIMOUSESTATE2) + buffered events;
//             while an exclusive mouse is acquired the cursor is detached and hidden (mouse look)
// No joysticks are enumerated.
#include "rt.h"
#include <pthread.h>
#include <CoreGraphics/CoreGraphics.h>
#include "cocoa.h"

#define DI_OK 0
#define DI_BUFFEROVERFLOW 1
#define DI_NOEFFECT 1
#define DIERR_NOTACQUIRED 0x8007000Cu
#define DIERR_INVALIDPARAM 0x80070057u
#define DIERR_DEVICENOTREG 0x80040154u
#define E_NOINTERFACE 0x80004002u
#define DISCL_EXCLUSIVE 1
#define DIGDD_PEEK 1
enum { DEV_KEYBOARD, DEV_MOUSE };

typedef struct { uint32_t ofs, data, time, seq; } Ev;
typedef struct Dev {
    uint64_t vtbl;                       // +0: guest-visible COM vtable
    int kind, refs, acquired, exclusive;
    uint32_t data_size;                  // from SetDataFormat
    uint32_t buf_cap;                    // DIPROP_BUFFERSIZE (0 = unbuffered)
    Ev q[1024]; int qh, qn, overflow;
} Dev;

static pthread_mutex_t dmu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t keys[256];               // DIK state
static int32_t mx, my, mz;              // mouse motion not yet read by GetDeviceState
static uint8_t mbuttons[8];
static uint32_t seq;
static Dev *devs[8]; static int ndevs;
static uint64_t vt_di[11], vt_dev[32];
static uint64_t di_obj[2];               // the IDirectInput8 object: vtable pointer + refcount

static uint32_t now_ms(void) { return (uint32_t)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000); }
static void push(int kind, uint32_t ofs, uint32_t data) {   // dmu held
    for (int i = 0; i < ndevs; i++) {
        Dev *d = devs[i];
        if (d->kind != kind || !d->acquired || !d->buf_cap) continue;
        if (d->qn >= (int)d->buf_cap || d->qn >= 1024) { d->overflow = 1; continue; }
        d->q[(d->qh + d->qn++) % 1024] = (Ev){ ofs, data, now_ms(), seq };
    }
    seq++;
}

// ---------------- feeds (called by window.c / cocoa.m on the main thread) ----------------
void di_key(uint32_t scan, int ext, int down) {
    uint32_t dik = (scan & 0x7F) | (ext ? 0x80 : 0);
    if (!dik) return;
    pthread_mutex_lock(&dmu);
    if (!!(keys[dik] & 0x80) != !!down) { keys[dik] = down ? 0x80 : 0; push(DEV_KEYBOARD, dik, down ? 0x80 : 0); }
    pthread_mutex_unlock(&dmu);
}
void di_mouse_move(int dx, int dy) {
    if (!dx && !dy) return;
    pthread_mutex_lock(&dmu);
    mx += dx; my += dy;
    if (dx) push(DEV_MOUSE, 0, (uint32_t)dx);   // DIMOFS_X
    if (dy) push(DEV_MOUSE, 4, (uint32_t)dy);   // DIMOFS_Y
    pthread_mutex_unlock(&dmu);
}
void di_mouse_wheel(int dz) {
    pthread_mutex_lock(&dmu);
    mz += dz; push(DEV_MOUSE, 8, (uint32_t)dz);  // DIMOFS_Z
    pthread_mutex_unlock(&dmu);
}
void di_mouse_button(int b, int down) {
    if (b < 0 || b > 7) return;
    pthread_mutex_lock(&dmu);
    mbuttons[b] = down ? 0x80 : 0; push(DEV_MOUSE, 12 + (uint32_t)b, down ? 0x80 : 0);   // DIMOFS_BUTTON0..7
    pthread_mutex_unlock(&dmu);
}

// ---------------- IDirectInputDevice8 ----------------
#define THIS_D ((Dev *)PTR(A0))
static void d_QueryInterface(CPU *c) { THIS_D->refs++; wr64(A2, A0); RET(DI_OK); }
static void d_AddRef(CPU *c) { RET((uint32_t)++THIS_D->refs); }
static void d_Release(CPU *c) { RET((uint32_t)(--THIS_D->refs > 0 ? THIS_D->refs : 0)); }
// GetCapabilities(DIDEVCAPS*): dwSize +0, dwFlags +4, dwDevType +8, dwAxes +12, dwButtons +16, dwPOVs +20
static void d_GetCapabilities(CPU *c) {
    Dev *d = THIS_D; uint64_t p = A1; uint32_t sz = rd32(p);
    if (sz > 4) memset(PTR(p + 4), 0, sz - 4);
    wr32(p + 4, 1 | 4);   // DIDC_ATTACHED | DIDC_EMULATED
    if (d->kind == DEV_MOUSE) { wr32(p + 8, 0x12 | (1 << 8)); wr32(p + 12, 3); wr32(p + 16, 5); }   // DI8DEVTYPE_MOUSE / unknown
    else { wr32(p + 8, 0x13 | (4 << 8)); wr32(p + 16, 128); }                                      // DI8DEVTYPE_KEYBOARD / PCENH
    RET(DI_OK);
}
static void d_EnumObjects(CPU *c) { RET(DI_OK); }
static void d_GetProperty(CPU *c) {
    // DIPROPDWORD: diph {dwSize, dwHeaderSize, dwObj, dwHow} + dwData at +16; DIPROP_BUFFERSIZE is MAKEDIPROP(1)
    if (A1 == 1) { wr32(A2 + 16, THIS_D->buf_cap); RET(DI_OK); return; }
    RET(DI_OK);
}
static void d_SetProperty(CPU *c) {
    if (A1 == 1) { pthread_mutex_lock(&dmu); THIS_D->buf_cap = rd32(A2 + 16); if (THIS_D->buf_cap > 1024) THIS_D->buf_cap = 1024; pthread_mutex_unlock(&dmu); }
    RET(DI_OK);
}
static void set_mouse_capture(int on) {
    static int captured;
    if (on == captured) return;
    captured = on;
    CGAssociateMouseAndMouseCursorPosition(!on);
    cocoa_cursor_visible(!on);
}
static void d_Acquire(CPU *c) {
    Dev *d = THIS_D;
    pthread_mutex_lock(&dmu);
    int was = d->acquired; d->acquired = 1;
    if (!was && d->kind == DEV_MOUSE) { mx = my = mz = 0; }
    pthread_mutex_unlock(&dmu);
    if (d->kind == DEV_MOUSE && d->exclusive) set_mouse_capture(1);
    RET(was ? DI_NOEFFECT : DI_OK);
}
static void d_Unacquire(CPU *c) {
    Dev *d = THIS_D;
    pthread_mutex_lock(&dmu); int was = d->acquired; d->acquired = 0; d->qn = 0; pthread_mutex_unlock(&dmu);
    if (d->kind == DEV_MOUSE && d->exclusive) set_mouse_capture(0);
    RET(was ? DI_OK : DI_NOEFFECT);
}
// GetDeviceState(cbData, data)
static void d_GetDeviceState(CPU *c) {
    Dev *d = THIS_D; uint32_t n = (uint32_t)A1; uint8_t *out = PTR(A2);
    if (!d->acquired) { RET(DIERR_NOTACQUIRED); return; }
    pthread_mutex_lock(&dmu);
    memset(out, 0, n);
    if (d->kind == DEV_KEYBOARD) memcpy(out, keys, n < 256 ? n : 256);
    else {   // DIMOUSESTATE (16 bytes) / DIMOUSESTATE2 (20): lX, lY, lZ, rgbButtons[4 or 8]
        int32_t v[3] = { mx, my, mz };
        memcpy(out, v, n < 12 ? n : 12);
        if (n > 12) memcpy(out + 12, mbuttons, n - 12 < 8 ? n - 12 : 8);
        mx = my = mz = 0;
    }
    pthread_mutex_unlock(&dmu);
    RET(DI_OK);
}
// GetDeviceData(cbObjectData, DIDEVICEOBJECTDATA *rgdod, DWORD *inout, flags)
// DIDEVICEOBJECTDATA (x64): dwOfs +0, dwData +4, dwTimeStamp +8, dwSequence +12, uAppData +16 (24 bytes)
static void d_GetDeviceData(CPU *c) {
    Dev *d = THIS_D; uint32_t sz = (uint32_t)A1; uint64_t out = A2, pn = A3; uint32_t fl = (uint32_t)AN(4);
    if (!d->acquired) { RET(DIERR_NOTACQUIRED); return; }
    if (!d->buf_cap) { RET(DIERR_INVALIDPARAM); return; }
    pthread_mutex_lock(&dmu);
    uint32_t want = rd32(pn), got = 0;
    if (!out) { got = (uint32_t)d->qn < want ? (uint32_t)d->qn : want; if (!(fl & DIGDD_PEEK) && want) { d->qh = (d->qh + (int)got) % 1024; d->qn -= (int)got; } }
    else {
        for (; got < want && (int)got < d->qn; got++) {
            Ev *e = &d->q[(d->qh + (int)got) % 1024]; uint64_t p = out + (uint64_t)got * sz;
            wr32(p, e->ofs); wr32(p + 4, e->data); wr32(p + 8, e->time); wr32(p + 12, e->seq);
            if (sz >= 24) wr64(p + 16, 0);
        }
        if (!(fl & DIGDD_PEEK)) { d->qh = (d->qh + (int)got) % 1024; d->qn -= (int)got; }
    }
    int ov = d->overflow; d->overflow = 0;
    pthread_mutex_unlock(&dmu);
    wr32(pn, got);
    RET(ov ? DI_BUFFEROVERFLOW : DI_OK);
}
// SetDataFormat(DIDATAFORMAT*): dwSize +0, dwObjSize +4, dwFlags +8, dwDataSize +12
static void d_SetDataFormat(CPU *c) { THIS_D->data_size = rd32(A1 + 12); RET(DI_OK); }
static void d_SetEventNotification(CPU *c) { RET(DI_OK); }
static void d_SetCooperativeLevel(CPU *c) { THIS_D->exclusive = ((uint32_t)A2 & DISCL_EXCLUSIVE) != 0; RET(DI_OK); }
static void d_GetObjectInfo(CPU *c) { RET(DIERR_INVALIDPARAM); }
// GetDeviceInfo(DIDEVICEINSTANCEW*): dwSize +0, guidInstance +4, guidProduct +20, dwDevType +36, tszInstanceName[260] +40
static void d_GetDeviceInfo(CPU *c) {
    Dev *d = THIS_D; uint64_t p = A1; uint32_t sz = rd32(p);
    if (sz > 4) memset(PTR(p + 4), 0, sz - 4);
    wr32(p + 4, d->kind == DEV_MOUSE ? 0x6F1D2B60u : 0x6F1D2B61u); wr32(p + 20, d->kind == DEV_MOUSE ? 0x6F1D2B60u : 0x6F1D2B61u);
    wr32(p + 36, d->kind == DEV_MOUSE ? 0x112 : 0x413);
    const char *n = d->kind == DEV_MOUSE ? "Mouse" : "Keyboard";
    for (int i = 0; n[i] && sz >= 40 + 2u * 260; i++) { wr16(p + 40 + 2ull * i, (uint16_t)n[i]); wr16(p + 560 + 2ull * i, (uint16_t)n[i]); }
    RET(DI_OK);
}
static void d_Poll(CPU *c) { RET(DI_NOEFFECT); }
static void d_unsupported(CPU *c) { RET(0x80004001u); }   // E_NOTIMPL (effects, action maps)

// ---------------- IDirectInput8 ----------------
// CreateDevice(this, REFGUID, **device, outer): GUID_SysMouse {6F1D2B60-...}, GUID_SysKeyboard {6F1D2B61-...}
static void di_CreateDevice(CPU *c) {
    uint32_t d1 = rd32(A1);
    int kind = d1 == 0x6F1D2B60u ? DEV_MOUSE : d1 == 0x6F1D2B61u ? DEV_KEYBOARD : -1;
    if (kind < 0 || ndevs == 8) { rt_log("[dinput] CreateDevice({%08X-...}) not available\n", d1); wr64(A2, 0); RET(DIERR_DEVICENOTREG); return; }
    Dev *d = calloc(1, sizeof *d);
    d->vtbl = G(vt_dev); d->kind = kind; d->refs = 1;
    pthread_mutex_lock(&dmu); devs[ndevs++] = d; pthread_mutex_unlock(&dmu);
    wr64(A2, G(d));
    rt_log("[dinput] %s created\n", kind == DEV_MOUSE ? "mouse" : "keyboard");
    RET(DI_OK);
}
// EnumDevices(this, type, callback, ref, flags): keyboard and mouse only; no game controllers
static void di_EnumDevices(CPU *c) {
    uint32_t type = (uint32_t)A1; uint64_t cb = A2, ref = A3;
    static uint8_t inst[2][1100];
    for (int k = 0; k < 2; k++) {
        uint32_t devclass = k == 0 ? 3 : 2;   // DI8DEVCLASS_KEYBOARD 3, POINTER 2
        if (type && type != devclass && type != (uint32_t)(k == 0 ? 0x13 : 0x12)) continue;
        uint64_t p = G(inst[k]); memset(inst[k], 0, sizeof inst[k]); wr32(p, 1092);
        wr32(p + 4, k ? 0x6F1D2B60u : 0x6F1D2B61u); wr32(p + 20, k ? 0x6F1D2B60u : 0x6F1D2B61u); wr32(p + 36, k ? 0x112 : 0x413);
        uint64_t a[2] = { p, ref };
        if (!(uint32_t)guest_call(c, cb, 2, a)) break;   // DIENUM_STOP
    }
    RET(DI_OK);
}
static void di_QueryInterface(CPU *c) { wr64(A2, A0); RET(DI_OK); }
static void di_AddRef(CPU *c) { RET((uint32_t)++di_obj[1]); }
static void di_Release(CPU *c) { RET((uint32_t)(di_obj[1] > 1 ? --di_obj[1] : 0)); }
static void di_GetDeviceStatus(CPU *c) { uint32_t d1 = rd32(A1); RET(d1 == 0x6F1D2B60u || d1 == 0x6F1D2B61u ? DI_OK : 1); }   // DI_NOTATTACHED
static void di_ok(CPU *c) { RET(DI_OK); }

static pthread_once_t di_once = PTHREAD_ONCE_INIT;
static void build(void) {
    gfn di[11] = { di_QueryInterface, di_AddRef, di_Release, di_CreateDevice, di_EnumDevices, di_GetDeviceStatus,
                   di_ok, di_ok, d_unsupported, d_unsupported, d_unsupported };
    for (int i = 0; i < 11; i++) vt_di[i] = rt_host_fn(di[i], "IDirectInput8");
    gfn dv[32] = { d_QueryInterface, d_AddRef, d_Release, d_GetCapabilities, d_EnumObjects, d_GetProperty, d_SetProperty,
                   d_Acquire, d_Unacquire, d_GetDeviceState, d_GetDeviceData, d_SetDataFormat, d_SetEventNotification,
                   d_SetCooperativeLevel, d_GetObjectInfo, d_GetDeviceInfo, di_ok, di_ok, d_unsupported, d_unsupported,
                   d_unsupported, d_unsupported, d_unsupported, d_unsupported, d_unsupported, d_Poll, d_unsupported,
                   d_unsupported, d_unsupported, d_unsupported, d_unsupported, d_unsupported };
    for (int i = 0; i < 32; i++) vt_dev[i] = rt_host_fn(dv[i], "IDirectInputDevice8");
    di_obj[0] = G(vt_di); di_obj[1] = 1;
}
// DirectInput8Create(hinst, version, riid, **out, outer)
void I_dinput8_dll_DirectInput8Create(CPU *c) {
    pthread_once(&di_once, build);
    if (!A3) { RET(DIERR_INVALIDPARAM); return; }
    di_obj[1]++;
    wr64(A3, G(di_obj));
    rt_log("[dinput] DirectInput8Create(version 0x%x)\n", (unsigned)A1);
    RET(DI_OK);
}

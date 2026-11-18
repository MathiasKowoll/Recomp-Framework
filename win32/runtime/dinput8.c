// DirectInput 8 (ANSI interfaces) for 32-bit guests: the system keyboard and mouse, fed by the Cocoa host
// (cocoa32.m: DIK scan codes, mouse deltas and buttons). Joysticks are not enumerated here: games read
// controllers through XInput.
#include "com32.h"
#include <strings.h>

extern uint8_t rt_keys[256];
extern int rt_mouse_dx, rt_mouse_dy, rt_mouse_dz;

#define DI_OK 0
#define DI_NOEFFECT 1
#define DIERR_UNSUPPORTED 0x80004001u
#define DIERR_DEVICENOTREG 0x80040154u
#define DIERR_INVALIDPARAM 0x80070057u
#define DIERR_NOTACQUIRED 0x8007000Cu
#define DI_BUFFEROVERFLOW 1

typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } Guid;
static const Guid GUID_SysMouse    = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const Guid GUID_SysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

// ---- keyboard state and buffered events ----
static uint8_t dik_state[256];
typedef struct { uint8_t dik, down; uint32_t time, seq; } KeyEv;
static KeyEv kq[256]; static int kq_head, kq_tail; static uint32_t kseq;
static pthread_mutex_t kmu = PTHREAD_MUTEX_INITIALIZER;
void rt_dinput_key(uint8_t dik, int down) {
    pthread_mutex_lock(&kmu);
    dik_state[dik] = down ? 0x80 : 0;
    int nt = (kq_tail + 1) & 255;
    if (nt != kq_head) { kq[kq_tail] = (KeyEv){dik, (uint8_t)(down ? 0x80 : 0), 0, kseq++}; kq_tail = nt; }
    pthread_mutex_unlock(&kmu);
}

typedef struct {
    ComObj base;
    int kind;            // 1 keyboard, 2 mouse
    int acquired;
    uint32_t data_size;  // from SetDataFormat
    uint32_t buffer;     // DIPROP_BUFFERSIZE
} DiDev;

// ---- IDirectInputDevice8A ----
static uint32_t dev_GetCapabilities(CPU *c, ComObj *s, const ComMethod *m) {
    DiDev *d = (DiDev *)s; uint32_t p = M(1);
    if (!p) return DIERR_INVALIDPARAM;
    uint32_t size = rd32(p);
    memset(G2H(p + 4), 0, size > 4 ? size - 4 : 0);
    wr32(p + 4, 1 | 4);   // DIDC_ATTACHED | DIDC_EMULATED
    wr32(p + 8, d->kind == 1 ? 0x13 | (1u << 8) : 0x12 | (2u << 8));   // DI8DEVTYPE_KEYBOARD/PC101, DI8DEVTYPE_MOUSE/TRADITIONAL
    wr32(p + 12, d->kind == 2 ? 3 : 0);    // axes
    wr32(p + 16, d->kind == 2 ? 5 : 128);  // buttons / keys
    return DI_OK;
}
static uint32_t dev_EnumObjects(CPU *c, ComObj *s, const ComMethod *m) { return DI_OK; }
static uint32_t dev_GetProperty(CPU *c, ComObj *s, const ComMethod *m) {
    DiDev *d = (DiDev *)s;
    if (M(1) == 1 && M(2)) { wr32(M(2) + 16, d->buffer); return DI_OK; }   // DIPROP_BUFFERSIZE: DIPROPDWORD.dwData
    return DIERR_UNSUPPORTED;
}
static uint32_t dev_SetProperty(CPU *c, ComObj *s, const ComMethod *m) {
    DiDev *d = (DiDev *)s;
    if (M(1) == 1 && M(2)) d->buffer = rd32(M(2) + 16);
    return DI_OK;
}
static uint32_t dev_Acquire(CPU *c, ComObj *s, const ComMethod *m) { DiDev *d = (DiDev *)s; int was = d->acquired; d->acquired = 1; return was ? DI_NOEFFECT : DI_OK; }
static uint32_t dev_Unacquire(CPU *c, ComObj *s, const ComMethod *m) { ((DiDev *)s)->acquired = 0; return DI_OK; }
static uint32_t dev_GetDeviceState(CPU *c, ComObj *s, const ComMethod *m) {
    DiDev *d = (DiDev *)s; uint32_t n = M(1), p = M(2);
    if (!p) return DIERR_INVALIDPARAM;
    memset(G2H(p), 0, n);
    if (d->kind == 1) {
        pthread_mutex_lock(&kmu);
        memcpy(G2H(p), dik_state, n < 256 ? n : 256);
        pthread_mutex_unlock(&kmu);
    } else {
        // DIMOUSESTATE / DIMOUSESTATE2: relative axes since the last read, then buttons
        int dx = __atomic_exchange_n(&rt_mouse_dx, 0, __ATOMIC_RELAXED);
        int dy = __atomic_exchange_n(&rt_mouse_dy, 0, __ATOMIC_RELAXED);
        int dz = __atomic_exchange_n(&rt_mouse_dz, 0, __ATOMIC_RELAXED);
        if (n >= 12) { wr32(p, (uint32_t)dx); wr32(p + 4, (uint32_t)dy); wr32(p + 8, (uint32_t)dz); }
        if (n >= 13) wr8(p + 12, rt_keys[1] ? 0x80 : 0);
        if (n >= 14) wr8(p + 13, rt_keys[2] ? 0x80 : 0);
        if (n >= 15) wr8(p + 14, rt_keys[4] ? 0x80 : 0);
    }
    return DI_OK;
}
// GetDeviceData(cbObjectData, DIDEVICEOBJECTDATA *rgdod, DWORD *pdwInOut, flags): buffered key events
static uint32_t dev_GetDeviceData(CPU *c, ComObj *s, const ComMethod *m) {
    DiDev *d = (DiDev *)s;
    uint32_t sz = M(1), out = M(2), io = M(3), flags = M(4);
    if (!io) return DIERR_INVALIDPARAM;
    uint32_t want = rd32(io), got = 0;
    if (d->kind != 1) { wr32(io, 0); return DI_OK; }
    pthread_mutex_lock(&kmu);
    int h = kq_head;
    while (h != kq_tail && got < want) {
        if (out) {
            uint32_t e = out + got * sz;
            wr32(e, kq[h].dik); wr32(e + 4, kq[h].down); wr32(e + 8, kq[h].time); wr32(e + 12, kq[h].seq);
            if (sz >= 20) wr32(e + 16, 0);
        }
        got++; h = (h + 1) & 255;
    }
    if (!(flags & 1)) kq_head = h;   // DIGDD_PEEK leaves them queued
    pthread_mutex_unlock(&kmu);
    wr32(io, got);
    return DI_OK;
}
static uint32_t dev_SetDataFormat(CPU *c, ComObj *s, const ComMethod *m) {
    if (M(1)) ((DiDev *)s)->data_size = rd32(M(1) + 12);   // DIDATAFORMAT.dwDataSize
    return DI_OK;
}
static uint32_t dev_ok(CPU *c, ComObj *s, const ComMethod *m) { return DI_OK; }
static uint32_t dev_GetDeviceInfo(CPU *c, ComObj *s, const ComMethod *m) {
    DiDev *d = (DiDev *)s; uint32_t p = M(1);
    if (!p) return DIERR_INVALIDPARAM;
    uint32_t size = rd32(p);
    memset(G2H(p + 4), 0, size > 4 ? size - 4 : 0);
    const Guid *g = d->kind == 1 ? &GUID_SysKeyboard : &GUID_SysMouse;
    memcpy(G2H(p + 4), g, 16); memcpy(G2H(p + 20), g, 16);
    wr32(p + 36, d->kind == 1 ? 0x13 | (1u << 8) : 0x12 | (2u << 8));
    snprintf(G2H(p + 40), 260, "%s", d->kind == 1 ? "Keyboard" : "Mouse");
    snprintf(G2H(p + 300), 260, "%s", d->kind == 1 ? "Keyboard" : "Mouse");
    return DI_OK;
}

#define DEV(name, n, fn) {name, n, fn, 0, 0, 0}
#define DEVX(name, n, r) {name, n, NULL, r, 0, 0}
static const ComMethod didev_m[] = {
    {"QueryInterface", 3, com_QueryInterface, 0, 0, 0}, {"AddRef", 1, com_AddRef, 0, 0, 0}, {"Release", 1, com_Release, 0, 0, 0},
    DEV("GetCapabilities", 2, dev_GetCapabilities), DEV("EnumObjects", 4, dev_EnumObjects), DEV("GetProperty", 3, dev_GetProperty),
    DEV("SetProperty", 3, dev_SetProperty), DEV("Acquire", 1, dev_Acquire), DEV("Unacquire", 1, dev_Unacquire),
    DEV("GetDeviceState", 3, dev_GetDeviceState), DEV("GetDeviceData", 5, dev_GetDeviceData), DEV("SetDataFormat", 2, dev_SetDataFormat),
    DEV("SetEventNotification", 2, dev_ok), DEV("SetCooperativeLevel", 3, dev_ok), DEVX("GetObjectInfo", 4, DIERR_UNSUPPORTED),
    DEV("GetDeviceInfo", 2, dev_GetDeviceInfo), DEV("RunControlPanel", 3, dev_ok), DEV("Initialize", 4, dev_ok),
    DEVX("CreateEffect", 5, DIERR_UNSUPPORTED), DEV("EnumEffects", 4, dev_ok), DEVX("GetEffectInfo", 3, DIERR_UNSUPPORTED),
    DEVX("GetForceFeedbackState", 2, DIERR_UNSUPPORTED), DEVX("SendForceFeedbackCommand", 2, DIERR_UNSUPPORTED),
    DEV("EnumCreatedEffectObjects", 4, dev_ok), DEVX("Escape", 2, DIERR_UNSUPPORTED), DEV("Poll", 1, dev_ok),
    DEVX("SendDeviceData", 5, DIERR_UNSUPPORTED), DEVX("EnumEffectsInFile", 5, DIERR_UNSUPPORTED),
    DEVX("WriteEffectToFile", 5, DIERR_UNSUPPORTED), DEVX("BuildActionMap", 4, DIERR_UNSUPPORTED),
    DEVX("SetActionMap", 4, DIERR_UNSUPPORTED), DEVX("GetImageInfo", 2, DIERR_UNSUPPORTED),
};
static ComClass didev_cls = {"IDirectInputDevice8A", sizeof didev_m / sizeof didev_m[0], didev_m, 0, NULL};

// ---- IDirectInput8A ----
static uint32_t di_CreateDevice(CPU *c, ComObj *s, const ComMethod *m) {
    Guid g; memcpy(&g, G2H(M(1)), 16);
    int kind = !memcmp(&g, &GUID_SysKeyboard, 16) ? 1 : !memcmp(&g, &GUID_SysMouse, 16) ? 2 : 0;
    if (!kind || !M(2)) { if (M(2)) wr32(M(2), 0); return DIERR_DEVICENOTREG; }
    DiDev *d = calloc(1, sizeof *d);
    d->kind = kind;
    wr32(M(2), com_wrap(&d->base, &didev_cls));
    return DI_OK;
}
// DIDEVICEINSTANCEA (580 bytes): dwSize, guidInstance, guidProduct, dwDevType, tszInstanceName[260], tszProductName[260], ...
static uint32_t di_EnumDevices(CPU *c, ComObj *s, const ComMethod *m) {
    uint32_t type = M(1), cb = M(2), ref = M(3);
    for (int kind = 1; kind <= 2; kind++) {
        // DI8DEVCLASS_ALL 0, DEVICE 1, POINTER 2, KEYBOARD 3, GAMECTRL 4; or a DI8DEVTYPE_*
        int match = type == 0 || (kind == 1 && (type == 3 || (type & 0xFF) == 0x13)) || (kind == 2 && (type == 2 || (type & 0xFF) == 0x12));
        if (!match || !cb) continue;
        uint32_t inst = gcalloc(1, 580);
        wr32(inst, 580);
        const Guid *g = kind == 1 ? &GUID_SysKeyboard : &GUID_SysMouse;
        memcpy(G2H(inst + 4), g, 16); memcpy(G2H(inst + 20), g, 16);
        wr32(inst + 36, kind == 1 ? 0x13 | (1u << 8) : 0x12 | (2u << 8));
        snprintf(G2H(inst + 40), 260, "%s", kind == 1 ? "Keyboard" : "Mouse");
        snprintf(G2H(inst + 300), 260, "%s", kind == 1 ? "Keyboard" : "Mouse");
        uint32_t args[2] = {inst, ref};
        uint32_t r = guest_call(c, cb, 2, args);
        gfree(inst);
        if (r == 0) break;   // DIENUM_STOP
    }
    return DI_OK;
}
static uint32_t di_GetDeviceStatus(CPU *c, ComObj *s, const ComMethod *m) {
    Guid g; memcpy(&g, G2H(M(1)), 16);
    return (!memcmp(&g, &GUID_SysKeyboard, 16) || !memcmp(&g, &GUID_SysMouse, 16)) ? DI_OK : 0x8007048Fu;   // DIERR_NOTATTACHED
}
static const ComMethod di_m[] = {
    {"QueryInterface", 3, com_QueryInterface, 0, 0, 0}, {"AddRef", 1, com_AddRef, 0, 0, 0}, {"Release", 1, com_Release, 0, 0, 0},
    DEV("CreateDevice", 4, di_CreateDevice), DEV("EnumDevices", 5, di_EnumDevices), DEV("GetDeviceStatus", 2, di_GetDeviceStatus),
    DEV("RunControlPanel", 3, dev_ok), DEV("Initialize", 3, dev_ok), DEVX("FindDevice", 4, DIERR_DEVICENOTREG),
    DEV("EnumDevicesBySemantics", 6, dev_ok), DEVX("ConfigureDevices", 5, DIERR_UNSUPPORTED),
};
static ComClass di_cls = {"IDirectInput8A", sizeof di_m / sizeof di_m[0], di_m, 0, NULL};

// DirectInput8Create(HINSTANCE, DWORD version, REFIID, LPVOID *out, LPUNKNOWN outer)
SHIM(dinput8_DirectInput8Create, 5) {
    if (!ARG(3)) return DIERR_INVALIDPARAM;
    ComObj *o = calloc(1, sizeof *o);
    wr32(ARG(3), com_wrap(o, &di_cls));
    if (rt_verbose) rt_log("[dinput8] DirectInput8Create(version 0x%x)", ARG(1));
    return DI_OK;
}

// user32/gdi32: the parts the engine and its MFC startup query before any window exists.
#include "rt.h"
#include <CoreGraphics/CoreGraphics.h>

#define PSEUDO_GDI 0x4744490000000000ull   // fake DC/brush/cursor handles

int rt_primary_rect(int32_t r[4]);   // display.c: current (possibly emulated) primary display mode
static void screen_px(uint32_t *w, uint32_t *h) { int32_t r[4]; rt_primary_rect(r); *w = (uint32_t)(r[2] - r[0]); *h = (uint32_t)(r[3] - r[1]); }
void I_user32_dll_GetSystemMetrics(CPU *c) {
    uint32_t w, h; screen_px(&w, &h);
    switch ((int)A0) {
    case 0: case 78: RET(w); break;          // SM_CXSCREEN, SM_CXVIRTUALSCREEN
    case 1: case 79: RET(h); break;          // SM_CYSCREEN, SM_CYVIRTUALSCREEN
    case 76: case 77: RET(0); break;         // virtual screen origin
    case 80: RET(1); break;                  // SM_CMONITORS
    case 5: case 6: RET(1); break;           // borders
    case 4: RET(23); break;                  // caption
    case 11: case 12: RET(32); break;        // icon
    case 13: case 14: RET(32); break;        // cursor
    case 32: case 33: RET(4); break;         // frame
    case 43: RET(5); break;                  // mouse buttons
    case 19: RET(1); break;                  // mouse present
    default: RET(0); break;
    }
}
// A window's DC carries its hwnd (bit 32 set), so SwapBuffers / wglMakeCurrent can find the window (opengl.m)
#define DC_WINDOW 0x100000000ull
void I_user32_dll_GetDC(CPU *c) { RET(PSEUDO_GDI | (A0 ? DC_WINDOW | (uint32_t)A0 : 1)); }
void I_user32_dll_GetWindowDC(CPU *c) { RET(PSEUDO_GDI | (A0 ? DC_WINDOW | (uint32_t)A0 : 1)); }
uint64_t rt_dc_window(uint64_t dc) { return (dc & ~0xFFFFFFFFull) == (PSEUDO_GDI | DC_WINDOW) ? (uint32_t)dc : 0; }
void I_user32_dll_ReleaseDC(CPU *c) { RET(1); }
void I_gdi32_dll_GetDeviceCaps(CPU *c) {
    uint32_t w, h; screen_px(&w, &h);
    switch ((int)A1) {
    case 8: RET(w); break; case 10: RET(h); break;          // HORZRES, VERTRES
    case 12: RET(32); break; case 14: RET(1); break;        // BITSPIXEL, PLANES
    case 88: case 90: RET(96); break;                       // LOGPIXELSX/Y
    case 116: RET(60); break;                               // VREFRESH
    case 4: RET(w * 254 / 960); break; case 6: RET(h * 254 / 960); break;   // HORZSIZE/VERTSIZE (mm)
    default: RET(0); break;
    }
}
void I_user32_dll_GetSysColor(CPU *c) { RET(0x00F0F0F0); }
void I_user32_dll_GetSysColorBrush(CPU *c) { RET(PSEUDO_GDI | 0x100 | (A0 & 0xFF)); }
void I_user32_dll_LoadCursorA(CPU *c) { RET(PSEUDO_GDI | 0x10000 | (A1 & 0xFFFF)); }
void I_user32_dll_LoadCursorW(CPU *c) { RET(PSEUDO_GDI | 0x10000 | (A1 & 0xFFFF)); }
void I_user32_dll_LoadIconA(CPU *c) { RET(PSEUDO_GDI | 0x20000 | (A1 & 0xFFFF)); }
void I_user32_dll_LoadIconW(CPU *c) { RET(PSEUDO_GDI | 0x20000 | (A1 & 0xFFFF)); }
static uint32_t next_msg = 0xC000;
void I_user32_dll_RegisterWindowMessageA(CPU *c) { RET(__atomic_fetch_add(&next_msg, 1, __ATOMIC_SEQ_CST)); }
void I_user32_dll_RegisterWindowMessageW(CPU *c) { RET(__atomic_fetch_add(&next_msg, 1, __ATOMIC_SEQ_CST)); }
void I_gdi32_dll_CreateSolidBrush(CPU *c) { RET(PSEUDO_GDI | 0x400 | (A0 & 0xFF)); }
void I_gdi32_dll_CreateFontA(CPU *c) { RET(PSEUDO_GDI | 0x500); }
void I_gdi32_dll_SetTextColor(CPU *c) { RET(0); }
void I_gdi32_dll_SetBkColor(CPU *c) { RET(0xFFFFFF); }
// EnumDisplayDevicesA(device, index, DISPLAY_DEVICEA*, flags): one adapter with one monitor.
// DISPLAY_DEVICEA: cb +0, DeviceName[32] +4, DeviceString[128] +36, StateFlags +164, DeviceID[128] +168, DeviceKey[128] +296
void I_user32_dll_EnumDisplayDevicesA(CPU *c) {
    uint64_t d = A2;
    if ((uint32_t)A1 != 0 || !d) { RET(0); return; }
    uint32_t cb = rd32(d); if (cb < 168) { RET(0); return; }
    memset(PTR(d + 4), 0, cb - 4);
    const char *name = A0 ? "\\\\.\\DISPLAY1\\Monitor0" : "\\\\.\\DISPLAY1";
    const char *desc = A0 ? "Built-in Display" : "Apple GPU";
    strncpy(PTR(d + 4), name, 31); strncpy(PTR(d + 36), desc, 127);
    wr32(d + 164, A0 ? 0x3 : 0x5);   // monitor: ACTIVE | ATTACHED; adapter: ATTACHED_TO_DESKTOP | PRIMARY_DEVICE
    RET(1);
}
void I_gdi32_dll_GetStockObject(CPU *c) { RET(PSEUDO_GDI | 0x300 | (A0 & 0xFF)); }
void I_gdi32_dll_DeleteObject(CPU *c) { RET(1); }
void I_gdi32_dll_DeleteDC(CPU *c) { RET(1); }
void I_gdi32_dll_CreateCompatibleDC(CPU *c) { RET(PSEUDO_GDI | 2); }
void I_gdi32_dll_SelectObject(CPU *c) { RET(PSEUDO_GDI | 0x300); }
void I_user32_dll_GetKeyboardLayout(CPU *c) { RET(0x04090409); }
void I_user32_dll_GetDoubleClickTime(CPU *c) { RET(500); }

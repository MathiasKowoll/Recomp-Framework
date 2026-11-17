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
void I_gdi32_dll_DeleteDC(CPU *c) { RET(1); }
void I_gdi32_dll_CreateCompatibleDC(CPU *c) { RET(PSEUDO_GDI | 2); }
void I_gdi32_dll_SelectObject(CPU *c) { RET(PSEUDO_GDI | 0x300); }
void I_user32_dll_GetKeyboardLayout(CPU *c) { RET(0x04090409); }
void I_user32_dll_GetDoubleClickTime(CPU *c) { RET(500); }

// ---------------- bitmaps and custom cursors ----------------
// UE4 builds its hardware cursors from PNGs: CreateBitmap(32-bit BGRA colour + 1-bit mask) -> CreateIconIndirect.
#include "cocoa.h"
#include <pthread.h>
typedef struct { uint32_t w, h, bpp; uint8_t *bits; } Bmp;
#define MAXGDI 256
#define H_BMP (PSEUDO_GDI | 0x1000000)
#define H_CUR (PSEUDO_GDI | 0x2000000)
static Bmp *bmps[MAXGDI]; static void *curs[MAXGDI];
static pthread_mutex_t gdi_mu = PTHREAD_MUTEX_INITIALIZER;
static int slot_of(uint64_t h, uint64_t kind) { return (h & ~0xFFFFFFull) == kind && (h & 0xFFFFFF) < MAXGDI ? (int)(h & 0xFFFFFF) : -1; }
// CreateBitmap(width, height, planes, bitsPerPixel, bits): rows are WORD-aligned, top row first
void I_gdi32_dll_CreateBitmap(CPU *c) {
    uint32_t w = (uint32_t)A0, h = (uint32_t)A1, planes = (uint32_t)A2, bpp = (uint32_t)A3; uint64_t bits = AN(4);
    if (!w || !h || w > 4096 || h > 4096 || planes != 1 || !bpp || bpp > 32) { RET(0); return; }
    size_t stride = ((size_t)w * bpp + 15) / 16 * 2;
    Bmp *b = calloc(1, sizeof *b); b->w = w; b->h = h; b->bpp = bpp; b->bits = calloc(stride, h);
    if (bits) memcpy(b->bits, PTR(bits), stride * h);
    pthread_mutex_lock(&gdi_mu);
    int i = 0; while (i < MAXGDI && bmps[i]) i++;
    if (i < MAXGDI) bmps[i] = b;
    pthread_mutex_unlock(&gdi_mu);
    if (i == MAXGDI) { free(b->bits); free(b); RET(0); return; }
    RET(H_BMP | (uint64_t)i);
}
// Regions: rectangles only. UE4 hit-tests the cursor against its window with CreateRectRgn + PtInRegion
// (FWindowsWindow::IsPointInWindow); without them no point is ever inside the window, so menus ignore the mouse.
#define H_RGN (PSEUDO_GDI | 0x3000000)
static int32_t (*rgns)[4]; static uint8_t rgn_used[MAXGDI];
static uint64_t new_rgn(int32_t l, int32_t t, int32_t r, int32_t b) {
    pthread_mutex_lock(&gdi_mu);
    if (!rgns) rgns = calloc(MAXGDI, sizeof *rgns);
    int i = 0; while (i < MAXGDI && rgn_used[i]) i++;
    if (i < MAXGDI) { rgn_used[i] = 1; rgns[i][0] = l; rgns[i][1] = t; rgns[i][2] = r; rgns[i][3] = b; }
    pthread_mutex_unlock(&gdi_mu);
    return i < MAXGDI ? H_RGN | (uint64_t)i : 0;
}
void I_gdi32_dll_CreateRectRgn(CPU *c) { RET(new_rgn((int32_t)A0, (int32_t)A1, (int32_t)A2, (int32_t)A3)); }
void I_gdi32_dll_CreateRoundRectRgn(CPU *c) { RET(new_rgn((int32_t)A0, (int32_t)A1, (int32_t)A2, (int32_t)A3)); }   // corners ignored
// PtInRegion(rgn, x, y): the rectangle includes its left/top edge, not its right/bottom
void I_gdi32_dll_PtInRegion(CPU *c) {
    int i = slot_of(A0, H_RGN); int32_t x = (int32_t)A1, y = (int32_t)A2, r[4] = { 0 }; int ok = 0;
    pthread_mutex_lock(&gdi_mu); if (i >= 0 && rgn_used[i]) { memcpy(r, rgns[i], sizeof r); ok = 1; } pthread_mutex_unlock(&gdi_mu);
    RET(ok && x >= r[0] && x < r[2] && y >= r[1] && y < r[3]);
}
void I_gdi32_dll_DeleteObject(CPU *c) {
    int i = slot_of(A0, H_BMP);
    if (i >= 0) { pthread_mutex_lock(&gdi_mu); Bmp *b = bmps[i]; bmps[i] = NULL; pthread_mutex_unlock(&gdi_mu); if (b) { free(b->bits); free(b); } }
    i = slot_of(A0, H_RGN);
    if (i >= 0) { pthread_mutex_lock(&gdi_mu); rgn_used[i] = 0; pthread_mutex_unlock(&gdi_mu); }
    RET(1);
}
// CreateIconIndirect(ICONINFO*): { BOOL fIcon; DWORD xHotspot, yHotspot; HBITMAP hbmMask @16, hbmColor @24 }
void I_user32_dll_CreateIconIndirect(CPU *c) {
    uint64_t ii = A0; if (!ii) { RET(0); return; }
    uint32_t hx = rd32(ii + 4), hy = rd32(ii + 8);
    int bi = slot_of(rd64(ii + 24), H_BMP);
    pthread_mutex_lock(&gdi_mu); Bmp *b = bi >= 0 ? bmps[bi] : NULL; pthread_mutex_unlock(&gdi_mu);
    if (!b || b->bpp != 32) { rt_log("[user32] CreateIconIndirect: only 32-bit colour cursors are supported\n"); RET(0); return; }
    void *cur = cocoa_cursor_new(b->bits, (int)b->w, (int)b->h, (int)hx, (int)hy);
    if (!cur) { RET(0); return; }
    pthread_mutex_lock(&gdi_mu);
    int i = 0; while (i < MAXGDI && curs[i]) i++;
    if (i < MAXGDI) curs[i] = cur;
    pthread_mutex_unlock(&gdi_mu);
    if (i == MAXGDI) { cocoa_cursor_free(cur); RET(0); return; }
    RET(H_CUR | (uint64_t)i);
}
static void destroy_cursor(CPU *c) {
    int i = slot_of(A0, H_CUR);
    if (i >= 0) { pthread_mutex_lock(&gdi_mu); void *cur = curs[i]; curs[i] = NULL; pthread_mutex_unlock(&gdi_mu); cocoa_cursor_free(cur); }
    RET(1);
}
void I_user32_dll_DestroyCursor(CPU *c) { destroy_cursor(c); }
void I_user32_dll_DestroyIcon(CPU *c) { destroy_cursor(c); }
// the AppKit cursor behind a cursor handle: a custom one, or NULL (system arrow) for LoadCursor's
void *rt_cursor_native(uint64_t h) {
    int i = slot_of(h, H_CUR); if (i < 0) return NULL;
    pthread_mutex_lock(&gdi_mu); void *cur = curs[i]; pthread_mutex_unlock(&gdi_mu);
    return cur;
}
// .cur/.ani files are not supported: UE4 then falls back to its PNG cursors (CreateIconIndirect above)
void I_user32_dll_LoadCursorFromFileW(CPU *c) { set_last_error(2); RET(0); }
void I_user32_dll_LoadCursorFromFileA(CPU *c) { set_last_error(2); RET(0); }

// user32 windowing: window classes, HWNDs, per-thread message queues, window procedures, geometry,
// focus, cursor and keyboard state. Top-level windows are NSWindows (cocoa.m); child and message-only
// windows exist only here. Coordinates are pixels, Windows-style (top-left of the primary display).
//
// AppKit events become posted messages during PeekMessage/GetMessage on the main thread.
// Messages the system sends synchronously on Windows (WM_SIZE, WM_ACTIVATE...) are posted instead.
// TranslateMessage is a no-op: WM_CHAR/WM_SYSCHAR are posted right after their WM_KEYDOWN.
#include "rt.h"
#include "cocoa.h"
#include "game.h"
#include <pthread.h>
#include <unistd.h>
#include <strings.h>
#include <time.h>

enum {
    WM_CREATE = 1, WM_DESTROY = 2, WM_MOVE = 3, WM_SIZE = 5, WM_ACTIVATE = 6, WM_SETFOCUS = 7, WM_KILLFOCUS = 8,
    WM_SETTEXT = 0xC, WM_GETTEXT = 0xD, WM_GETTEXTLENGTH = 0xE, WM_PAINT = 0xF, WM_CLOSE = 0x10, WM_QUIT = 0x12,
    WM_ERASEBKGND = 0x14, WM_SHOWWINDOW = 0x18, WM_ACTIVATEAPP = 0x1C, WM_SETCURSOR = 0x20, WM_MOUSEACTIVATE = 0x21,
    WM_NCCREATE = 0x81, WM_NCDESTROY = 0x82, WM_NCHITTEST = 0x84, WM_NCACTIVATE = 0x86,
    WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102, WM_SYSKEYDOWN = 0x104, WM_SYSKEYUP = 0x105, WM_SYSCHAR = 0x106,
    WM_SYSCOMMAND = 0x112, WM_TIMER = 0x113, WM_MOUSEMOVE = 0x200,
};
#define WS_POPUP 0x80000000u
#define WS_CHILD 0x40000000u
#define WS_MINIMIZE 0x20000000u
#define WS_VISIBLE 0x10000000u
#define WS_DISABLED 0x08000000u
#define WS_MAXIMIZE 0x01000000u
#define WS_CAPTION 0x00C00000u
#define WS_THICKFRAME 0x00040000u
#define WS_MINIMIZEBOX 0x00020000u
#define CW_USEDEFAULT ((int32_t)0x80000000)
#define DESKTOP_HWND 0x10010ull
#define HWND_MESSAGE ((uint64_t)-3)
#define HWND_BROADCAST 0xFFFFull
#define HWND_BASE 0x100000ull
#define MAXW 4096

int rt_primary_rect(int32_t r[4]);   // display.c

// ---------------- classes ----------------
typedef struct WClass {
    char *name; uint16_t atom; uint32_t style; uint64_t proc, inst, icon, cursor, brush, menu, iconsm;
    int32_t cls_extra, wnd_extra; uint8_t *extra; int unicode;
} WClass;
static WClass *classes[512];
static int nclasses;
static pthread_mutex_t wmu = PTHREAD_MUTEX_INITIALIZER;

static char *str_arg(uint64_t p, int wide) { return wide ? w16_to_utf8(PTR(p)) : strdup(PTR(p)); }
static WClass *builtin_class(uint64_t name, int wide);
// class name argument: string, or an atom in the low word
static WClass *class_find(uint64_t name, int wide) {
    WClass *r = NULL;
    pthread_mutex_lock(&wmu);
    if (name && name < 0x10000) { for (int i = 0; i < nclasses; i++) if (classes[i]->atom == (uint16_t)name) r = classes[i]; }
    else if (name) {
        char *s = str_arg(name, wide);
        for (int i = 0; i < nclasses; i++) if (!strcasecmp(classes[i]->name, s)) r = classes[i];
        free(s);
    }
    pthread_mutex_unlock(&wmu);
    if (!r && name >= 0x10000) r = builtin_class(name, wide);
    return r;
}
// WNDCLASS: style +0, proc +8, cls_extra +16, wnd_extra +20, inst +24, icon +32, cursor +40, brush +48, menu +56, name +64
// WNDCLASSEX: cbSize +0, style +4, then the same from +8, iconsm +72
static uint16_t register_class(uint64_t wc, int ex, int wide) {
    uint64_t name = rd64(wc + 64);
    if (class_find(name, wide)) { set_last_error(1410); return 0; }   // ERROR_CLASS_ALREADY_EXISTS
    WClass *k = calloc(1, sizeof *k);
    k->style = rd32(wc + (ex ? 4 : 0)); k->proc = rd64(wc + 8);
    k->cls_extra = (int32_t)rd32(wc + 16); k->wnd_extra = (int32_t)rd32(wc + 20);
    k->inst = rd64(wc + 24); k->icon = rd64(wc + 32); k->cursor = rd64(wc + 40); k->brush = rd64(wc + 48); k->menu = rd64(wc + 56);
    k->iconsm = ex ? rd64(wc + 72) : 0; k->unicode = wide;
    if (k->cls_extra > 0) k->extra = calloc(1, (size_t)k->cls_extra);
    char buf[16];
    if (name < 0x10000) { snprintf(buf, sizeof buf, "#%u", (unsigned)name); k->name = strdup(buf); }
    else k->name = str_arg(name, wide);
    pthread_mutex_lock(&wmu);
    k->atom = (uint16_t)(0xC000 + nclasses);
    if (nclasses < 512) classes[nclasses++] = k;
    pthread_mutex_unlock(&wmu);
    return k->atom;
}
// System control classes (the console window's edit box and buttons): plain windows run by DefWindowProc.
static WClass *builtin_class(uint64_t name, int wide) {
    static const char *const sys[] = { "edit", "button", "static", "listbox", "combobox", "scrollbar", "richedit20a", "richedit20w", "msctls_progress32" };
    char *s = str_arg(name, wide); WClass *k = NULL;
    for (size_t i = 0; i < sizeof sys / sizeof *sys; i++)
        if (!strcasecmp(s, sys[i])) {
            extern void I_user32_dll_DefWindowProcW(CPU *);
            k = calloc(1, sizeof *k);
            k->name = strdup(sys[i]); k->proc = rt_host_fn(I_user32_dll_DefWindowProcW, "user32.dll!DefWindowProcW"); k->unicode = 1;
            pthread_mutex_lock(&wmu);
            k->atom = (uint16_t)(0xC000 + nclasses);
            if (nclasses < 512) classes[nclasses++] = k;
            pthread_mutex_unlock(&wmu);
            break;
        }
    free(s);
    return k;
}
void I_user32_dll_RegisterClassA(CPU *c) { RET(register_class(A0, 0, 0)); }
void I_user32_dll_RegisterClassW(CPU *c) { RET(register_class(A0, 0, 1)); }
void I_user32_dll_RegisterClassExA(CPU *c) { RET(register_class(A0, 1, 0)); }
void I_user32_dll_RegisterClassExW(CPU *c) { RET(register_class(A0, 1, 1)); }
void I_user32_dll_UnregisterClassA(CPU *c) { RET(1); }
void I_user32_dll_UnregisterClassW(CPU *c) { RET(1); }
static void fill_class(uint64_t wc, WClass *k, int ex) {
    if (ex) wr32(wc + 4, k->style); else wr32(wc, k->style);
    wr64(wc + 8, k->proc); wr32(wc + 16, (uint32_t)k->cls_extra); wr32(wc + 20, (uint32_t)k->wnd_extra);
    wr64(wc + 24, k->inst); wr64(wc + 32, k->icon); wr64(wc + 40, k->cursor); wr64(wc + 48, k->brush); wr64(wc + 56, k->menu);
    wr64(wc + 64, k->atom); if (ex) wr64(wc + 72, k->iconsm);
}
// GetClassInfo[Ex](inst, name, WNDCLASS*)
static void class_info(CPU *c, int ex, int wide) {
    WClass *k = class_find(A1, wide);
    if (!k) { set_last_error(1411); RET(0); return; }   // ERROR_CLASS_DOES_NOT_EXIST
    fill_class(A2, k, ex);
    RET(k->atom);
}
void I_user32_dll_GetClassInfoA(CPU *c) { class_info(c, 0, 0); }
void I_user32_dll_GetClassInfoW(CPU *c) { class_info(c, 0, 1); }
void I_user32_dll_GetClassInfoExA(CPU *c) { class_info(c, 1, 0); }
void I_user32_dll_GetClassInfoExW(CPU *c) { class_info(c, 1, 1); }

// ---------------- windows ----------------
typedef struct { char *name; uint64_t atom, value; } Prop;
typedef struct Win {
    uint64_t hwnd; WClass *cls; uint64_t proc, parent, owner, id, inst, userdata;
    uint32_t style, exstyle, tid; int unicode, enabled, visible;
    int32_t x, y, cw, ch;                 // client rect: screen pixels (top-level), parent client (child)
    uint8_t *extra; char *title; void *ns;
    Prop props[16]; int nprops;
} Win;
static Win *wins[MAXW];
static uint64_t g_active, g_focus, g_capture, g_cursor;

static Win *win_get(uint64_t h) {
    if (h < HWND_BASE || (h - HWND_BASE) % 16) return NULL;
    uint64_t i = (h - HWND_BASE) / 16;
    return i < MAXW ? wins[i] : NULL;
}
static int titled(uint32_t style) { return (style & WS_CAPTION) == WS_CAPTION; }
static int toplevel(const Win *w) { return !(w->style & WS_CHILD); }
static void insets(const Win *w, int *l, int *t, int *r, int *b) {
    if (w && !toplevel(w)) { *l = *t = *r = *b = 0; return; }
    cocoa_frame_insets(titled(w ? w->style : WS_CAPTION), !!((w ? w->style : 0) & WS_THICKFRAME), l, t, r, b);
}
// refresh the cached client rect of a top-level window from AppKit
static void sync_rect(Win *w) { if (w && w->ns) cocoa_window_get_client(w->ns, &w->x, &w->y, &w->cw, &w->ch); }
static void client_origin(Win *w, int32_t *x, int32_t *y) {   // client top-left in screen pixels
    *x = 0; *y = 0;
    for (int depth = 0; w && depth < 32; depth++) {
        if (toplevel(w)) { sync_rect(w); *x += w->x; *y += w->y; return; }
        *x += w->x; *y += w->y;
        w = win_get(w->parent);
    }
}

// call a window procedure with the caller's CPU
static uint64_t call_proc(CPU *c, uint64_t proc, uint64_t h, uint32_t msg, uint64_t wp, uint64_t lp) {
    if (!proc) return 0;
    uint64_t a[4] = { h, msg, wp, lp };
    return guest_call(c, proc, 4, a);
}
static uint64_t send_msg(CPU *c, uint64_t h, uint32_t msg, uint64_t wp, uint64_t lp) {
    Win *w = win_get(h);
    return w ? call_proc(c, w->proc, h, msg, wp, lp) : 0;
}

// ---------------- message queues ----------------
typedef struct { uint64_t hwnd; uint32_t msg; uint64_t wp, lp; uint32_t time; int32_t x, y; } QMsg;
typedef struct { uint32_t tid; QMsg *q; int n, cap; int quit; uint32_t quit_code; } Queue;
static Queue *queues[512];
static int nqueues;
static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcv = PTHREAD_COND_INITIALIZER;
static uint32_t last_time; static int32_t last_x, last_y;
static uint32_t main_tid;   // thread that owns the AppKit windows

static uint32_t now_ms(void) { return (uint32_t)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000); }
static Queue *queue_for(uint32_t tid) {   // qmu held
    for (int i = 0; i < nqueues; i++) if (queues[i]->tid == tid) return queues[i];
    if (nqueues == 512) return queues[0];
    Queue *q = calloc(1, sizeof *q); q->tid = tid;
    queues[nqueues++] = q;
    return q;
}
static void post(uint32_t tid, uint64_t h, uint32_t msg, uint64_t wp, uint64_t lp) {
    int x = 0, y = 0;
    if (cocoa_active()) cocoa_cursor_pos(&x, &y);
    pthread_mutex_lock(&qmu);
    Queue *q = queue_for(tid);
    if (msg == WM_MOUSEMOVE && q->n && q->q[q->n - 1].msg == WM_MOUSEMOVE && q->q[q->n - 1].hwnd == h) q->n--;   // coalesce
    if (q->n == q->cap) { q->cap = q->cap ? q->cap * 2 : 64; q->q = realloc(q->q, sizeof(QMsg) * (size_t)q->cap); }
    q->q[q->n++] = (QMsg){ h, msg, wp, lp, now_ms(), x, y };
    pthread_cond_broadcast(&qcv);
    pthread_mutex_unlock(&qmu);
}
static uint32_t owner_tid(uint64_t h) { Win *w = win_get(h); return w ? w->tid : main_tid; }
static int is_descendant(uint64_t h, uint64_t ancestor) {
    for (int d = 0; d < 32; d++) { Win *w = win_get(h); if (!w || !(w->style & WS_CHILD)) return 0; h = w->parent; if (h == ancestor) return 1; }
    return 0;
}

// ---------------- timers ----------------
typedef struct { uint64_t hwnd, id, proc; uint32_t ms, tid; uint64_t next; int used; } Timer;
static Timer timers[128];
static uint64_t next_timer_id = 0x7E00;
// SetTimer(hwnd, id, ms, proc)
void I_user32_dll_SetTimer(CPU *c) {
    uint64_t h = A0, id = A1; uint32_t ms = (uint32_t)A2 < 10 ? 10 : (uint32_t)A2;
    pthread_mutex_lock(&qmu);
    if (!h) id = next_timer_id++;
    int slot = -1;
    for (int i = 0; i < 128; i++) if (timers[i].used && timers[i].hwnd == h && timers[i].id == id) slot = i;
    for (int i = 0; slot < 0 && i < 128; i++) if (!timers[i].used) slot = i;
    if (slot >= 0) timers[slot] = (Timer){ h, id, A3, ms, h ? owner_tid(h) : rt_thread_id(), now_ms() + ms, 1 };
    pthread_mutex_unlock(&qmu);
    RET(slot >= 0 ? (id ? id : 1) : 0);
}
void I_user32_dll_KillTimer(CPU *c) {
    int ok = 0;
    pthread_mutex_lock(&qmu);
    for (int i = 0; i < 128; i++) if (timers[i].used && timers[i].hwnd == A0 && timers[i].id == A1) { timers[i].used = 0; ok = 1; }
    pthread_mutex_unlock(&qmu);
    RET(ok);
}

// MSG: hwnd +0, message +8, wParam +16, lParam +24, time +32, pt +36, lPrivate +44
static void put_msg(uint64_t m, const QMsg *q) {
    wr64(m, q->hwnd); wr32(m + 8, q->msg); wr32(m + 12, 0); wr64(m + 16, q->wp); wr64(m + 24, q->lp);
    wr32(m + 32, q->time); wr32(m + 36, (uint32_t)q->x); wr32(m + 40, (uint32_t)q->y); wr32(m + 44, 0);
    last_time = q->time; last_x = q->x; last_y = q->y;
}
static int peek(uint64_t m, uint64_t hf, uint32_t mn, uint32_t mx, int remove) {
    uint32_t tid = rt_thread_id();
    if (cocoa_is_main_thread()) cocoa_pump(0);
    pthread_mutex_lock(&qmu);
    Queue *q = queue_for(tid);
    for (int i = 0; i < q->n; i++) {
        QMsg *e = &q->q[i];
        if (hf == (uint64_t)-1 ? e->hwnd != 0 : hf && e->hwnd != hf && !is_descendant(e->hwnd, hf)) continue;
        if ((mn || mx) && (e->msg < mn || e->msg > mx)) continue;
        QMsg got = *e;
        if (remove) { memmove(e, e + 1, sizeof(QMsg) * (size_t)(q->n - i - 1)); q->n--; }
        pthread_mutex_unlock(&qmu);
        put_msg(m, &got);
        return 1;
    }
    if (q->quit && (!(mn || mx) || (WM_QUIT >= mn && WM_QUIT <= mx))) {
        QMsg qm = { 0, WM_QUIT, q->quit_code, 0, now_ms(), 0, 0 };
        if (remove) q->quit = 0;
        pthread_mutex_unlock(&qmu);
        put_msg(m, &qm);
        return 1;
    }
    uint64_t t = now_ms();
    for (int i = 0; i < 128; i++) {
        Timer *tm = &timers[i];
        if (!tm->used || tm->tid != tid || t < tm->next) continue;
        if (hf && hf != (uint64_t)-1 && tm->hwnd != hf) continue;
        if ((mn || mx) && (WM_TIMER < mn || WM_TIMER > mx)) continue;
        if (remove) tm->next = t + tm->ms;
        QMsg tq = { tm->hwnd, WM_TIMER, tm->id, tm->proc, (uint32_t)t, 0, 0 };
        pthread_mutex_unlock(&qmu);
        put_msg(m, &tq);
        return 1;
    }
    pthread_mutex_unlock(&qmu);
    return 0;
}
static void wait_input(void) {
    if (cocoa_is_main_thread() && cocoa_active()) { cocoa_pump(10); return; }
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 10000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&qmu); pthread_cond_timedwait(&qcv, &qmu, &ts); pthread_mutex_unlock(&qmu);
}
// PeekMessage(msg, hwnd, min, max, flags: PM_REMOVE 1)
void I_user32_dll_PeekMessageA(CPU *c) { RET(peek(A0, A1, (uint32_t)A2, (uint32_t)A3, (uint32_t)AN(4) & 1)); }
void I_user32_dll_PeekMessageW(CPU *c) { I_user32_dll_PeekMessageA(c); }
// GetMessage(msg, hwnd, min, max): 0 on WM_QUIT
void I_user32_dll_GetMessageA(CPU *c) {
    uint64_t m = A0, h = A1; uint32_t mn = (uint32_t)A2, mx = (uint32_t)A3;
    while (!peek(m, h, mn, mx, 1)) wait_input();
    RET(rd32(m + 8) != WM_QUIT);
}
void I_user32_dll_GetMessageW(CPU *c) { I_user32_dll_GetMessageA(c); }
void I_user32_dll_WaitMessage(CPU *c) {
    uint8_t m[48];
    if (!peek(G(m), 0, 0, 0, 0)) wait_input();
    RET(1);
}
void I_user32_dll_TranslateMessage(CPU *c) { uint32_t m = rd32(A0 + 8); RET(m == WM_KEYDOWN || m == WM_SYSKEYDOWN); }
void I_user32_dll_DispatchMessageA(CPU *c) {
    uint64_t m = A0, h = rd64(m), wp = rd64(m + 16), lp = rd64(m + 24); uint32_t msg = rd32(m + 8);
    if (msg == WM_TIMER && lp) { uint64_t a[4] = { h, WM_TIMER, wp, now_ms() }; RET(guest_call(c, lp, 4, a)); return; }
    RET(send_msg(c, h, msg, wp, lp));
}
void I_user32_dll_DispatchMessageW(CPU *c) { I_user32_dll_DispatchMessageA(c); }
void I_user32_dll_GetMessageTime(CPU *c) { RET(last_time); }
void I_user32_dll_GetMessagePos(CPU *c) { RET((uint32_t)(uint16_t)last_x | (uint32_t)(uint16_t)last_y << 16); }
void I_user32_dll_PostQuitMessage(CPU *c) {
    pthread_mutex_lock(&qmu); Queue *q = queue_for(rt_thread_id()); q->quit = 1; q->quit_code = (uint32_t)A0;
    pthread_cond_broadcast(&qcv); pthread_mutex_unlock(&qmu);
}
static void post_shim(CPU *c) {
    uint64_t h = A0;
    if (h == HWND_BROADCAST) { for (int i = 0; i < MAXW; i++) if (wins[i] && toplevel(wins[i])) post(wins[i]->tid, wins[i]->hwnd, (uint32_t)A1, A2, A3); RET(1); return; }
    if (h && !win_get(h)) { set_last_error(1400); RET(0); return; }   // ERROR_INVALID_WINDOW_HANDLE
    post(h ? owner_tid(h) : rt_thread_id(), h, (uint32_t)A1, A2, A3);
    RET(1);
}
void I_user32_dll_PostMessageA(CPU *c) { post_shim(c); }
void I_user32_dll_PostMessageW(CPU *c) { post_shim(c); }
void I_user32_dll_PostThreadMessageA(CPU *c) { post((uint32_t)A0, 0, (uint32_t)A1, A2, A3); RET(1); }
void I_user32_dll_PostThreadMessageW(CPU *c) { post((uint32_t)A0, 0, (uint32_t)A1, A2, A3); RET(1); }
static void send_shim(CPU *c) {
    uint64_t h = A0, wp = A2, lp = A3; uint32_t msg = (uint32_t)A1;
    if (h == HWND_BROADCAST) { for (int i = 0; i < MAXW; i++) if (wins[i] && toplevel(wins[i])) send_msg(c, wins[i]->hwnd, msg, wp, lp); RET(0); return; }
    RET(send_msg(c, h, msg, wp, lp));
}
void I_user32_dll_SendMessageA(CPU *c) { send_shim(c); }
void I_user32_dll_SendMessageW(CPU *c) { send_shim(c); }
// CallWindowProc(prev, hwnd, msg, wp, lp)
void I_user32_dll_CallWindowProcA(CPU *c) { uint64_t p = A0; RET(call_proc(c, p, A1, (uint32_t)A2, A3, AN(4))); }
void I_user32_dll_CallWindowProcW(CPU *c) { I_user32_dll_CallWindowProcA(c); }

// messages from AppKit (main thread, inside cocoa_pump)
void win_from_cocoa(uint64_t h, uint32_t msg, uint64_t wp, uint64_t lp) {
    if (!h) {   // application-wide: every top-level window
        for (int i = 0; i < MAXW; i++) if (wins[i] && toplevel(wins[i])) post(wins[i]->tid, wins[i]->hwnd, msg, wp, lp);
        return;
    }
    Win *w = win_get(h);
    if (!w) return;
    if (msg == WM_ACTIVATE) { if (wp) g_active = g_focus = h; else if (g_active == h) g_active = g_focus = 0; }
    if (msg == WM_SIZE || msg == WM_MOVE) sync_rect(w);
    post(w->tid, h, msg, wp, lp);
}

// ---------------- DefWindowProc ----------------
void *rt_cursor_native(uint64_t h);   // user32.c
static void apply_cursor(uint64_t h);
static void destroy_window(CPU *c, uint64_t h);
struct Win; static void apply_show(CPU *c, struct Win *w, int cmd);
static uint64_t def_proc(CPU *c, uint64_t h, uint32_t msg, uint64_t wp, uint64_t lp, int wide) {
    Win *w = win_get(h);
    switch (msg) {
    case WM_NCCREATE: return 1;
    case WM_CLOSE: destroy_window(c, h); return 0;
    case WM_SYSCOMMAND:
        switch (wp & 0xFFF0) {
        case 0xF060: send_msg(c, h, WM_CLOSE, 0, 0); return 0;                  // SC_CLOSE
        case 0xF020: if (w && w->ns) cocoa_window_minimize(w->ns, 1); return 0; // SC_MINIMIZE
        case 0xF030: if (w && w->ns) cocoa_window_zoom(w->ns, 1); return 0;     // SC_MAXIMIZE
        case 0xF120: if (w && w->ns) { cocoa_window_minimize(w->ns, 0); cocoa_window_zoom(w->ns, 0); } return 0;   // SC_RESTORE
        default: return 0;
        }
    case WM_SYSKEYDOWN: if (wp == 0x73 && (lp & (1u << 29))) send_msg(c, h, WM_SYSCOMMAND, 0xF060, 0); return 0;   // Alt+F4
    case WM_NCHITTEST: return 1;           // HTCLIENT
    // over the client area, Windows puts the class cursor back (if the class has one) on every mouse move:
    // a game that hid the cursor with SetCursor(NULL) and leaves WM_SETCURSOR to DefWindowProc gets it back
    case WM_SETCURSOR: if (rt_game.win32_cursors && (lp & 0xFFFF) == 1 && w && w->cls && w->cls->cursor) apply_cursor(w->cls->cursor); return 0;
    case WM_NCACTIVATE: return 1;
    case WM_MOUSEACTIVATE: return 1;       // MA_ACTIVATE
    case WM_ERASEBKGND: return 1;
    case WM_GETTEXTLENGTH: return w && w->title ? strlen(w->title) : 0;
    case WM_GETTEXT: {
        if (!w || !wp) return 0;
        const char *t = w->title ? w->title : "";
        if (wide) { uint16_t *u = utf8_to_w16(t); size_t n = w16len(u); if (n >= wp) n = wp - 1; memcpy(PTR(lp), u, n * 2); wr16(lp + n * 2, 0); free(u); return n; }
        size_t n = strlen(t); if (n >= wp) n = wp - 1; memcpy(PTR(lp), t, n); wr8(lp + n, 0); return n;
    }
    case WM_SETTEXT:
        if (w) { free(w->title); w->title = lp ? str_arg(lp, wide) : strdup(""); if (w->ns) cocoa_window_set_title(w->ns, w->title); }
        return 1;
    default: return 0;
    }
}
void I_user32_dll_DefWindowProcA(CPU *c) { RET(def_proc(c, A0, (uint32_t)A1, A2, A3, 0)); }
void I_user32_dll_DefWindowProcW(CPU *c) { RET(def_proc(c, A0, (uint32_t)A1, A2, A3, 1)); }

// ---------------- create / destroy ----------------
// CreateWindowEx(exstyle, class, title, style, x, y, w, h, parent, menu, inst, param)
static void create_window(CPU *c, int wide) {
    uint32_t ex = (uint32_t)A0, style = (uint32_t)A3;
    uint64_t clsname = A1, title = A2;
    int32_t x = (int32_t)AN(4), y = (int32_t)AN(5), ww = (int32_t)AN(6), hh = (int32_t)AN(7);
    uint64_t parent = AN(8), menu = AN(9), inst = AN(10), param = AN(11);
    WClass *k = class_find(clsname, wide);
    if (!k) {
        char *n = clsname >= 0x10000 ? str_arg(clsname, wide) : strdup("#atom");
        rt_log("[user32] CreateWindowEx: unknown class \"%s\"\n", n); free(n);
        set_last_error(1407); RET(0); return;   // ERROR_CANNOT_FIND_WND_CLASS
    }
    if (parent == HWND_MESSAGE) parent = 0, style |= WS_CHILD, style &= ~WS_VISIBLE;   // message-only window
    Win *w = calloc(1, sizeof *w);
    w->cls = k; w->proc = k->proc; w->style = style; w->exstyle = ex; w->inst = inst; w->unicode = wide; w->enabled = !(style & WS_DISABLED);
    w->tid = rt_thread_id();
    if (style & WS_CHILD) { w->parent = parent; w->id = menu; } else w->owner = parent;
    w->title = title ? str_arg(title, wide) : strdup("");
    if (k->wnd_extra > 0) w->extra = calloc(1, (size_t)k->wnd_extra);
    pthread_mutex_lock(&wmu);
    int slot = -1;
    for (int i = 0; i < MAXW; i++) if (!wins[i]) { slot = i; break; }
    if (slot >= 0) { w->hwnd = HWND_BASE + 16ull * (uint64_t)slot; wins[slot] = w; }
    pthread_mutex_unlock(&wmu);
    if (slot < 0) { free(w); RET(0); return; }
    uint64_t h = w->hwnd;

    // geometry: the caller gives the outer window rect; the client is that minus the frame
    int32_t sr[4]; rt_primary_rect(sr);
    int l, t, r, b; insets(w, &l, &t, &r, &b);
    if (ww == CW_USEDEFAULT || ww <= 0) { ww = 1280 + l + r; hh = 720 + t + b; }
    if (x == CW_USEDEFAULT) { x = sr[0] + (sr[2] - sr[0] - ww) / 2; y = sr[1] + (sr[3] - sr[1] - hh) / 2; }
    w->x = x + l; w->y = y + t; w->cw = ww - l - r; w->ch = hh - t - b;
    if (w->cw < 1) w->cw = 1;
    if (w->ch < 1) w->ch = 1;
    if (toplevel(w)) {
        if (!main_tid) main_tid = w->tid;
        if (!cocoa_is_main_thread()) rt_log("[user32] warning: window created on thread %u, not the main thread; AppKit calls wait for the main thread\n", w->tid);
        w->ns = cocoa_window_new(h, w->x, w->y, w->cw, w->ch, titled(style), !!(style & WS_THICKFRAME), !!(style & WS_MINIMIZEBOX), w->title);
    }
    rt_log("[user32] CreateWindowEx(\"%s\", \"%s\", style 0x%x, ex 0x%x) -> 0x%llx, client %dx%d at (%d,%d)%s\n",
           k->name, w->title, style, ex, (unsigned long long)h, w->cw, w->ch, w->x, w->y, w->ns ? "" : " (no NSWindow)");

    // CREATESTRUCT: params +0, inst +8, menu +16, parent +24, cy +32, cx +36, y +40, x +44, style +48, name +56, class +64, ex +72
    uint8_t cs[80]; uint64_t p = G(cs);
    wr64(p, param); wr64(p + 8, inst); wr64(p + 16, menu); wr64(p + 24, parent);
    wr32(p + 32, (uint32_t)hh); wr32(p + 36, (uint32_t)ww); wr32(p + 40, (uint32_t)y); wr32(p + 44, (uint32_t)x);
    wr32(p + 48, style); wr64(p + 56, title); wr64(p + 64, clsname); wr32(p + 72, ex);
    if (!send_msg(c, h, WM_NCCREATE, 0, p)) { rt_log("[user32] WM_NCCREATE refused\n"); destroy_window(c, h); RET(0); return; }
    if ((int32_t)send_msg(c, h, WM_CREATE, 0, p) == -1) { rt_log("[user32] WM_CREATE refused\n"); destroy_window(c, h); RET(0); return; }
    if (!win_get(h)) { RET(0); return; }
    send_msg(c, h, WM_SIZE, 0, (uint64_t)(uint16_t)w->cw | (uint64_t)(uint16_t)w->ch << 16);
    send_msg(c, h, WM_MOVE, 0, (uint64_t)(uint16_t)w->x | (uint64_t)(uint16_t)w->y << 16);
    if (style & WS_VISIBLE) { w->style &= ~WS_VISIBLE; apply_show(c, w, (style & WS_MAXIMIZE) ? 3 : (style & WS_MINIMIZE) ? 2 : 1); }
    RET(h);
}
void I_user32_dll_CreateWindowExA(CPU *c) { create_window(c, 0); }
void I_user32_dll_CreateWindowExW(CPU *c) { create_window(c, 1); }

static void destroy_window(CPU *c, uint64_t h) {
    Win *w = win_get(h);
    if (!w) return;
    for (int i = 0; i < MAXW; i++)   // children and owned windows first
        if (wins[i] && (wins[i]->parent == h || wins[i]->owner == h)) destroy_window(c, wins[i]->hwnd);
    send_msg(c, h, WM_DESTROY, 0, 0);
    send_msg(c, h, WM_NCDESTROY, 0, 0);
    pthread_mutex_lock(&wmu);
    wins[(h - HWND_BASE) / 16] = NULL;
    pthread_mutex_unlock(&wmu);
    pthread_mutex_lock(&qmu);
    for (int i = 0; i < 128; i++) if (timers[i].used && timers[i].hwnd == h) timers[i].used = 0;
    pthread_mutex_unlock(&qmu);
    if (g_active == h) g_active = 0;
    if (g_focus == h) g_focus = 0;
    if (g_capture == h) g_capture = 0;
    if (w->ns) cocoa_window_free(w->ns);
    for (int i = 0; i < w->nprops; i++) free(w->props[i].name);
    free(w->title); free(w->extra); free(w);
}
void I_user32_dll_DestroyWindow(CPU *c) { if (!win_get(A0)) { set_last_error(1400); RET(0); return; } destroy_window(c, A0); RET(1); }

// after a display mode change: put every top-level window back at its guest rect under the new mapping
void win_relayout(void) {
    for (int i = 0; i < MAXW; i++) {
        Win *w = wins[i];
        if (w && w->ns && toplevel(w)) cocoa_window_set_client(w->ns, w->x, w->y, w->cw, w->ch);
    }
}

// metal layer for vkCreateWin32SurfaceKHR (overrides the weak default in vulkan.c)
void *rt_window_metal_layer(uint64_t h) {
    Win *w = win_get(h);
    while (w && !toplevel(w)) w = win_get(w->parent);
    return w && w->ns ? cocoa_window_layer(w->ns) : NULL;
}

// client size in pixels of the top-level window that contains h (for the GL drawable, opengl.m)
int rt_window_client_size(uint64_t h, int *cw, int *ch) {
    Win *w = win_get(h);
    if (!w) return 0;
    *cw = w->cw; *ch = w->ch;
    return 1;
}

// ---------------- show / position ----------------
static void apply_show(CPU *c, struct Win *w, int cmd) {
    if (toplevel(w)) rt_log("[user32] ShowWindow(0x%llx \"%s\", %d)\n", (unsigned long long)w->hwnd, w->title, cmd);
    int show = cmd != 0, activate = cmd != 4 && cmd != 7 && cmd != 8 && cmd != 0;
    int was = !!(w->style & WS_VISIBLE);
    if (show != was) send_msg(c, w->hwnd, WM_SHOWWINDOW, show, 0);
    if (show) w->style |= WS_VISIBLE; else w->style &= ~WS_VISIBLE;
    if (!w->ns) return;
    if (cmd == 2 || cmd == 6 || cmd == 7) { cocoa_window_show(w->ns, 1, 0); cocoa_window_minimize(w->ns, 1); return; }
    cocoa_window_show(w->ns, show, activate);
    if (cmd == 3) cocoa_window_zoom(w->ns, 1);
    if (cmd == 9 || cmd == 1) cocoa_window_minimize(w->ns, 0);
    if (show && activate) g_active = g_focus = w->hwnd;
}
// ShowWindow(hwnd, cmd) -> was visible
void I_user32_dll_ShowWindow(CPU *c) {
    Win *w = win_get(A0);
    if (!w) { RET(0); return; }
    int was = !!(w->style & WS_VISIBLE);
    apply_show(c, w, (int)A1);
    RET(was);
}
void I_user32_dll_UpdateWindow(CPU *c) { RET(win_get(A0) != NULL); }
// SetWindowPos(hwnd, after, x, y, cx, cy, flags): outer window rect
static int set_window_pos(CPU *c, uint64_t h, uint64_t after, int32_t x, int32_t y, int32_t cx, int32_t cy, uint32_t fl) {
    Win *w = win_get(h);
    if (!w) { set_last_error(1400); return 0; }
    if (toplevel(w)) rt_log("[user32] SetWindowPos(0x%llx, after %lld, %d,%d %dx%d, flags 0x%x)\n", (unsigned long long)h, (long long)after, x, y, cx, cy, fl);
    int l, t, r, b; insets(w, &l, &t, &r, &b);
    if (fl & 0x20) { insets(w, &l, &t, &r, &b); if (w->ns) cocoa_window_set_style(w->ns, titled(w->style), !!(w->style & WS_THICKFRAME), !!(w->style & WS_MINIMIZEBOX)); }   // FRAMECHANGED
    sync_rect(w);
    int32_t nx = (fl & 2) ? w->x : x + l, ny = (fl & 2) ? w->y : y + t;
    int32_t ncw = (fl & 1) ? w->cw : cx - l - r, nch = (fl & 1) ? w->ch : cy - t - b;
    if (!toplevel(w)) { w->x = nx; w->y = ny; w->cw = ncw; w->ch = nch; }
    else if (w->ns && (!(fl & 1) || !(fl & 2) || (fl & 0x20))) {
        int32_t ox = w->x, oy = w->y, ow = w->cw, oh = w->ch;
        cocoa_window_set_client(w->ns, nx, ny, ncw > 0 ? ncw : 1, nch > 0 ? nch : 1);
        // The new rect, now, as Windows reports it from inside SetWindowPos: Cocoa's resize notification never
        // comes when the Mac window keeps its size, e.g. a fullscreen window going from one emulated display
        // mode to another of the same aspect (it covers the screen either way; only the scale changes)
        sync_rect(w);
        if (w->cw != ow || w->ch != oh) send_msg(c, w->hwnd, WM_SIZE, 0, (uint64_t)(uint16_t)w->cw | (uint64_t)(uint16_t)w->ch << 16);
        if (w->x != ox || w->y != oy) send_msg(c, w->hwnd, WM_MOVE, 0, (uint64_t)(uint16_t)w->x | (uint64_t)(uint16_t)w->y << 16);
    }
    if (!toplevel(w) && !(fl & 1)) send_msg(c, w->hwnd, WM_SIZE, 0, (uint64_t)(uint16_t)ncw | (uint64_t)(uint16_t)nch << 16);
    if (fl & 0x40) apply_show(c, w, (fl & 0x10) ? 8 : 5);         // SHOWWINDOW (NOACTIVATE -> SHOWNA)
    else if (fl & 0x80) apply_show(c, w, 0);                       // HIDEWINDOW
    else if (!(fl & 4) && w->ns && (after == 0 || after == (uint64_t)-1) && (w->style & WS_VISIBLE) && !(fl & 0x10)) cocoa_window_activate(w->ns);
    return 1;
}
void I_user32_dll_SetWindowPos(CPU *c) { RET(set_window_pos(c, A0, A1, (int32_t)A2, (int32_t)A3, (int32_t)AN(4), (int32_t)AN(5), (uint32_t)AN(6))); }
// MoveWindow(hwnd, x, y, w, h, repaint)
void I_user32_dll_MoveWindow(CPU *c) { RET(set_window_pos(c, A0, 0, (int32_t)A1, (int32_t)A2, (int32_t)A3, (int32_t)AN(4), 4 | 0x10)); }
static void put_rect(uint64_t p, int32_t l, int32_t t, int32_t r, int32_t b) { wr32(p, (uint32_t)l); wr32(p + 4, (uint32_t)t); wr32(p + 8, (uint32_t)r); wr32(p + 12, (uint32_t)b); }
void I_user32_dll_GetClientRect(CPU *c) {
    if (A0 == DESKTOP_HWND) { int32_t r[4]; rt_primary_rect(r); put_rect(A1, 0, 0, r[2] - r[0], r[3] - r[1]); RET(1); return; }
    Win *w = win_get(A0);
    if (!w) { set_last_error(1400); RET(0); return; }
    sync_rect(w);
    put_rect(A1, 0, 0, w->cw, w->ch);
    RET(1);
}
void I_user32_dll_GetWindowRect(CPU *c) {
    if (A0 == DESKTOP_HWND) { int32_t r[4]; rt_primary_rect(r); put_rect(A1, r[0], r[1], r[2], r[3]); RET(1); return; }
    Win *w = win_get(A0);
    if (!w) { set_last_error(1400); RET(0); return; }
    int32_t x, y; client_origin(w, &x, &y);
    int l, t, r, b; insets(w, &l, &t, &r, &b);
    put_rect(A1, x - l, y - t, x + w->cw + r, y + w->ch + b);
    RET(1);
}
// AdjustWindowRect[Ex](rect, style, menu[, exstyle])
static void adjust(uint64_t rc, uint32_t style) {
    if (style & WS_CHILD) return;
    int l, t, r, b; cocoa_frame_insets(titled(style), !!(style & WS_THICKFRAME), &l, &t, &r, &b);
    put_rect(rc, (int32_t)rd32(rc) - l, (int32_t)rd32(rc + 4) - t, (int32_t)rd32(rc + 8) + r, (int32_t)rd32(rc + 12) + b);
}
void I_user32_dll_AdjustWindowRect(CPU *c) { adjust(A0, (uint32_t)A1); RET(1); }
void I_user32_dll_AdjustWindowRectEx(CPU *c) { adjust(A0, (uint32_t)A1); RET(1); }
void I_user32_dll_ClientToScreen(CPU *c) {
    Win *w = win_get(A0); int32_t x = 0, y = 0;
    if (w) client_origin(w, &x, &y);
    wr32(A1, rd32(A1) + (uint32_t)x); wr32(A1 + 4, rd32(A1 + 4) + (uint32_t)y);
    RET(A0 == DESKTOP_HWND || w);
}
void I_user32_dll_ScreenToClient(CPU *c) {
    Win *w = win_get(A0); int32_t x = 0, y = 0;
    if (w) client_origin(w, &x, &y);
    wr32(A1, rd32(A1) - (uint32_t)x); wr32(A1 + 4, rd32(A1 + 4) - (uint32_t)y);
    RET(A0 == DESKTOP_HWND || w);
}
// MapWindowPoints(from, to, points, n) -> packed (dx, dy)
void I_user32_dll_MapWindowPoints(CPU *c) {
    int32_t fx = 0, fy = 0, tx = 0, ty = 0;
    Win *f = win_get(A0), *t = win_get(A1);
    if (f) client_origin(f, &fx, &fy);
    if (t) client_origin(t, &tx, &ty);
    int32_t dx = fx - tx, dy = fy - ty;
    for (uint32_t i = 0; i < (uint32_t)A3; i++) { uint64_t p = A2 + 8ull * i; wr32(p, rd32(p) + (uint32_t)dx); wr32(p + 4, rd32(p + 4) + (uint32_t)dy); }
    RET((uint32_t)(uint16_t)dx | (uint32_t)(uint16_t)dy << 16);
}
// WINDOWPLACEMENT: length +0, flags +4, showCmd +8, ptMin +12, ptMax +20, rcNormal +28
void I_user32_dll_GetWindowPlacement(CPU *c) {
    Win *w = win_get(A0);
    if (!w) { RET(0); return; }
    int mn = 0, zm = 0, vis = !!(w->style & WS_VISIBLE);
    if (w->ns) cocoa_window_state(w->ns, &mn, &zm, &vis);
    uint64_t p = A1; memset(PTR(p + 4), 0, 40);
    wr32(p + 8, mn ? 2 : zm ? 3 : 1);   // showCmd describes the restored state, not visibility: SW_SHOWNORMAL even while hidden (UE4 hides, reads, then restores the placement)
    int32_t x, y; client_origin(w, &x, &y); int l, t, r, b; insets(w, &l, &t, &r, &b);
    put_rect(p + 28, x - l, y - t, x + w->cw + r, y + w->ch + b);
    RET(1);
}
// GetWindowInfo(hwnd, WINDOWINFO*): rcWindow, rcClient, style, exstyle, status (1 = active), border sizes, atom, version.
// UE4 reads it before every window-mode change; an unfilled struct put the window off screen.
void I_user32_dll_GetWindowInfo(CPU *c) {
    Win *w = win_get(A0); uint64_t o = A1;
    if (!w || !o || rd32(o) < 60) { set_last_error(1400); RET(0); return; }
    sync_rect(w);
    int l, t, r, b; insets(w, &l, &t, &r, &b);
    int32_t cx = w->x, cy = w->y;
    for (Win *q = w; q && !toplevel(q); ) { q = win_get(q->parent); if (q) { cx += q->x; cy += q->y; } }   // child: client -> screen
    wr32(o + 4, (uint32_t)(cx - l)); wr32(o + 8, (uint32_t)(cy - t)); wr32(o + 12, (uint32_t)(cx + w->cw + r)); wr32(o + 16, (uint32_t)(cy + w->ch + b));
    wr32(o + 20, (uint32_t)cx); wr32(o + 24, (uint32_t)cy); wr32(o + 28, (uint32_t)(cx + w->cw)); wr32(o + 32, (uint32_t)(cy + w->ch));
    wr32(o + 36, w->style); wr32(o + 40, w->exstyle); wr32(o + 44, g_active == w->hwnd ? 1 : 0);
    wr32(o + 48, (uint32_t)l); wr32(o + 52, (uint32_t)t); wr16(o + 56, 0); wr16(o + 58, 0x0500);
    RET(1);
}
void I_user32_dll_SetWindowPlacement(CPU *c) {
    Win *w = win_get(A0);
    if (!w) { RET(0); return; }
    uint64_t p = A1; int l, t, r, b; insets(w, &l, &t, &r, &b);
    int32_t rl = (int32_t)rd32(p + 28), rt = (int32_t)rd32(p + 32), rr = (int32_t)rd32(p + 36), rb = (int32_t)rd32(p + 40);
    if (w->ns) cocoa_window_set_client(w->ns, rl + l, rt + t, rr - rl - l - r, rb - rt - t - b);
    apply_show(c, w, (int)rd32(p + 8));
    RET(1);
}
void I_user32_dll_IsIconic(CPU *c) { Win *w = win_get(A0); int mn = 0, zm = 0, v = 0; if (w && w->ns) cocoa_window_state(w->ns, &mn, &zm, &v); RET(mn); }
void I_user32_dll_IsZoomed(CPU *c) { Win *w = win_get(A0); int mn = 0, zm = 0, v = 0; if (w && w->ns) cocoa_window_state(w->ns, &mn, &zm, &v); RET(zm); }
void I_user32_dll_CloseWindow(CPU *c) { Win *w = win_get(A0); if (w && w->ns) cocoa_window_minimize(w->ns, 1); RET(w != NULL); }

// ---------------- window data ----------------
static uint64_t get_long(Win *w, int32_t idx) {
    switch (idx) {
    case -4: return w->proc;           // GWLP_WNDPROC
    case -6: return w->inst;           // GWLP_HINSTANCE
    case -8: return w->style & WS_CHILD ? w->parent : w->owner;   // GWLP_HWNDPARENT
    case -12: return w->id;            // GWLP_ID
    case -16: return w->style;         // GWL_STYLE
    case -20: return w->exstyle;       // GWL_EXSTYLE
    case -21: return w->userdata;      // GWLP_USERDATA
    default:
        if (idx >= 0 && w->extra && idx + 8 <= w->cls->wnd_extra) return rd64(G(w->extra) + (uint64_t)idx);
        if (idx >= 0 && w->extra && idx + 4 <= w->cls->wnd_extra) return rd32(G(w->extra) + (uint64_t)idx);
        set_last_error(1413); return 0;   // ERROR_INVALID_INDEX
    }
}
static uint64_t set_long(Win *w, int32_t idx, uint64_t v, int size) {
    uint64_t old = get_long(w, idx);
    switch (idx) {
    case -4: w->proc = v; break;
    case -6: w->inst = v; break;
    case -8: if (w->style & WS_CHILD) w->parent = v; else w->owner = v; break;
    case -12: w->id = v; break;
    case -16: {
        uint32_t vis = w->style & WS_VISIBLE;
        int was_titled = titled(w->style);
        w->style = ((uint32_t)v & ~WS_VISIBLE) | vis;   // visibility changes only through ShowWindow
        rt_log("[user32] SetWindowLong(0x%llx, GWL_STYLE, 0x%x)\n", (unsigned long long)w->hwnd, (uint32_t)v);
        if (toplevel(w) && titled(w->style) && !was_titled) { extern void display_restore_desktop(void); display_restore_desktop(); }
        break;
    }
    case -20: w->exstyle = (uint32_t)v; break;
    case -21: w->userdata = v; break;
    default:
        if (idx >= 0 && w->extra && idx + size <= w->cls->wnd_extra) { if (size == 8) wr64(G(w->extra) + (uint64_t)idx, v); else wr32(G(w->extra) + (uint64_t)idx, (uint32_t)v); }
        break;
    }
    return old;
}
void I_user32_dll_GetWindowLongPtrA(CPU *c) { Win *w = win_get(A0); RET(w ? get_long(w, (int32_t)A1) : 0); }
void I_user32_dll_GetWindowLongPtrW(CPU *c) { I_user32_dll_GetWindowLongPtrA(c); }
void I_user32_dll_SetWindowLongPtrA(CPU *c) { Win *w = win_get(A0); RET(w ? set_long(w, (int32_t)A1, A2, 8) : 0); }
void I_user32_dll_SetWindowLongPtrW(CPU *c) { I_user32_dll_SetWindowLongPtrA(c); }
void I_user32_dll_GetWindowLongA(CPU *c) { Win *w = win_get(A0); RET(w ? (uint32_t)get_long(w, (int32_t)A1) : 0); }
void I_user32_dll_GetWindowLongW(CPU *c) { I_user32_dll_GetWindowLongA(c); }
void I_user32_dll_SetWindowLongA(CPU *c) { Win *w = win_get(A0); RET(w ? (uint32_t)set_long(w, (int32_t)A1, (uint32_t)A2, 4) : 0); }
void I_user32_dll_SetWindowLongW(CPU *c) { I_user32_dll_SetWindowLongA(c); }
// class data: GCLP_MENUNAME -8, HBRBACKGROUND -10, HCURSOR -12, HICON -14, HMODULE -16, CBWNDEXTRA -18, CBCLSEXTRA -20, WNDPROC -24, STYLE -26, HICONSM -34
static uint64_t *class_field(WClass *k, int32_t idx) {
    switch (idx) { case -8: return &k->menu; case -10: return &k->brush; case -12: return &k->cursor; case -14: return &k->icon;
                   case -16: return &k->inst; case -24: return &k->proc; case -34: return &k->iconsm; default: return NULL; }
}
static uint64_t get_class_long(Win *w, int32_t idx) {
    if (!w) return 0;
    uint64_t *f = class_field(w->cls, idx);
    if (f) return *f;
    if (idx == -26) return w->cls->style;
    if (idx == -18) return (uint32_t)w->cls->wnd_extra;
    if (idx == -20) return (uint32_t)w->cls->cls_extra;
    if (idx == -32) return w->cls->atom;
    if (idx >= 0 && idx + 8 <= w->cls->cls_extra) return rd64(G(w->cls->extra) + (uint64_t)idx);
    return 0;
}
void I_user32_dll_GetClassLongPtrA(CPU *c) { RET(get_class_long(win_get(A0), (int32_t)A1)); }
void I_user32_dll_GetClassLongPtrW(CPU *c) { I_user32_dll_GetClassLongPtrA(c); }
void I_user32_dll_GetClassLongA(CPU *c) { RET((uint32_t)get_class_long(win_get(A0), (int32_t)A1)); }
void I_user32_dll_SetClassLongPtrA(CPU *c) {
    Win *w = win_get(A0); int32_t idx = (int32_t)A1;
    if (!w) { RET(0); return; }
    uint64_t old = get_class_long(w, idx), *f = class_field(w->cls, idx);
    if (f) *f = A2;
    else if (idx == -26) w->cls->style = (uint32_t)A2;
    else if (idx >= 0 && idx + 8 <= w->cls->cls_extra) wr64(G(w->cls->extra) + (uint64_t)idx, A2);
    RET(old);
}
void I_user32_dll_SetClassLongPtrW(CPU *c) { I_user32_dll_SetClassLongPtrA(c); }
void I_user32_dll_GetClassNameA(CPU *c) {
    Win *w = win_get(A0); uint32_t n = (uint32_t)A2;
    if (!w || !n) { RET(0); return; }
    strlcpy(PTR(A1), w->cls->name, n);
    RET(strlen(PTR(A1)));
}
static void text_shim(CPU *c, int wide, int get) {
    Win *w = win_get(A0);
    if (!w) { RET(0); return; }
    RET(get ? def_proc(c, A0, WM_GETTEXT, A2, A1, wide) : def_proc(c, A0, WM_SETTEXT, 0, A1, wide));
}
void I_user32_dll_GetWindowTextA(CPU *c) { text_shim(c, 0, 1); }
void I_user32_dll_GetWindowTextW(CPU *c) { text_shim(c, 1, 1); }
void I_user32_dll_SetWindowTextA(CPU *c) { text_shim(c, 0, 0); }
void I_user32_dll_SetWindowTextW(CPU *c) { text_shim(c, 1, 0); }
void I_user32_dll_GetWindowTextLengthA(CPU *c) { Win *w = win_get(A0); RET(w && w->title ? strlen(w->title) : 0); }
void I_user32_dll_GetWindowTextLengthW(CPU *c) { I_user32_dll_GetWindowTextLengthA(c); }
// properties: name string or atom
static int prop_find(Win *w, uint64_t name) {
    for (int i = 0; i < w->nprops; i++) {
        if (name < 0x10000 ? w->props[i].atom == name : (w->props[i].name && !strcmp(w->props[i].name, PTR(name)))) return i;
    }
    return -1;
}
void I_user32_dll_SetPropA(CPU *c) {
    Win *w = win_get(A0);
    if (!w) { RET(0); return; }
    int i = prop_find(w, A1);
    if (i < 0) {
        if (w->nprops == 16) { RET(0); return; }
        i = w->nprops++;
        w->props[i].atom = A1 < 0x10000 ? A1 : 0; w->props[i].name = A1 < 0x10000 ? NULL : strdup(PTR(A1));
    }
    w->props[i].value = A2;
    RET(1);
}
void I_user32_dll_GetPropA(CPU *c) { Win *w = win_get(A0); int i = w ? prop_find(w, A1) : -1; RET(i >= 0 ? w->props[i].value : 0); }
void I_user32_dll_RemovePropA(CPU *c) {
    Win *w = win_get(A0); int i = w ? prop_find(w, A1) : -1;
    if (i < 0) { RET(0); return; }
    uint64_t v = w->props[i].value; free(w->props[i].name);
    w->props[i] = w->props[--w->nprops];
    RET(v);
}

// ---------------- hierarchy / state ----------------
void I_user32_dll_GetDesktopWindow(CPU *c) { RET(DESKTOP_HWND); }
void I_user32_dll_IsWindow(CPU *c) { RET(win_get(A0) != NULL || A0 == DESKTOP_HWND); }
void I_user32_dll_IsWindowVisible(CPU *c) {
    Win *w = win_get(A0); int v = 1;
    for (int d = 0; w && d < 32; d++) { if (!(w->style & WS_VISIBLE)) { v = 0; break; } if (toplevel(w)) break; w = win_get(w->parent); }
    RET(w && v);
}
void I_user32_dll_IsWindowEnabled(CPU *c) { Win *w = win_get(A0); RET(w && w->enabled); }
void I_user32_dll_EnableWindow(CPU *c) { Win *w = win_get(A0); int was_disabled = w && !w->enabled; if (w) w->enabled = A1 != 0; RET(was_disabled); }
void I_user32_dll_GetParent(CPU *c) { Win *w = win_get(A0); RET(w ? (w->style & WS_CHILD ? w->parent : w->owner) : 0); }
void I_user32_dll_SetParent(CPU *c) { Win *w = win_get(A0); uint64_t old = w ? w->parent : 0; if (w) w->parent = A1; RET(old); }
void I_user32_dll_IsChild(CPU *c) { RET(is_descendant(A1, A0)); }
// GetAncestor(hwnd, GA_PARENT 1 / GA_ROOT 2 / GA_ROOTOWNER 3)
void I_user32_dll_GetAncestor(CPU *c) {
    Win *w = win_get(A0);
    if (!w) { RET(0); return; }
    if (A1 == 1) { RET(toplevel(w) ? DESKTOP_HWND : w->parent); return; }
    for (int d = 0; d < 32 && !toplevel(w); d++) { Win *p = win_get(w->parent); if (!p) break; w = p; }
    if (A1 == 3) for (int d = 0; d < 32 && w->owner; d++) { Win *o = win_get(w->owner); if (!o) break; w = o; }
    RET(w->hwnd);
}
// GetWindow(hwnd, GW_HWNDNEXT 2 / GW_OWNER 4 / GW_CHILD 5)
void I_user32_dll_GetWindow(CPU *c) {
    Win *w = win_get(A0);
    if (!w) { RET(0); return; }
    switch ((uint32_t)A1) {
    case 4: RET(w->owner); return;
    case 5: for (int i = 0; i < MAXW; i++) if (wins[i] && (wins[i]->style & WS_CHILD) && wins[i]->parent == A0) { RET(wins[i]->hwnd); return; } break;
    case 2: {
        int start = (int)((A0 - HWND_BASE) / 16) + 1;
        for (int i = start; i < MAXW; i++)
            if (wins[i] && toplevel(wins[i]) == toplevel(w) && (toplevel(w) || wins[i]->parent == w->parent)) { RET(wins[i]->hwnd); return; }
        break;
    }
    }
    RET(0);
}
void I_user32_dll_GetTopWindow(CPU *c) {
    for (int i = 0; i < MAXW; i++)
        if (wins[i] && (A0 && A0 != DESKTOP_HWND ? (wins[i]->style & WS_CHILD) && wins[i]->parent == A0 : toplevel(wins[i]))) { RET(wins[i]->hwnd); return; }
    RET(0);
}
void I_user32_dll_GetLastActivePopup(CPU *c) { RET(A0); }
void I_user32_dll_GetDlgCtrlID(CPU *c) { Win *w = win_get(A0); RET(w ? w->id : 0); }
void I_user32_dll_GetDlgItem(CPU *c) {
    for (int i = 0; i < MAXW; i++) if (wins[i] && wins[i]->parent == A0 && wins[i]->id == A1) { RET(wins[i]->hwnd); return; }
    RET(0);
}
// GetWindowThreadProcessId(hwnd, *pid) -> tid
void I_user32_dll_GetWindowThreadProcessId(CPU *c) { Win *w = win_get(A0); if (A1) wr32(A1, 0x1000); RET(w ? w->tid : 0); }
// EnumThreadWindows(tid, callback(hwnd, lparam), lparam)
void I_user32_dll_EnumThreadWindows(CPU *c) {
    uint32_t tid = (uint32_t)A0; uint64_t cb = A1, lp = A2;
    for (int i = 0; i < MAXW; i++) {
        Win *w = wins[i];
        if (!w || w->tid != tid || !toplevel(w)) continue;
        uint64_t a[2] = { w->hwnd, lp };
        if (!(uint32_t)guest_call(c, cb, 2, a)) break;
    }
    RET(1);
}
void I_user32_dll_WindowFromPoint(CPU *c) {
    int32_t x = (int32_t)A0, y = (int32_t)(A0 >> 32);
    for (int i = 0; i < MAXW; i++) {
        Win *w = wins[i];
        if (!w || !toplevel(w) || !(w->style & WS_VISIBLE)) continue;
        sync_rect(w);
        if (x >= w->x && x < w->x + w->cw && y >= w->y && y < w->y + w->ch) { RET(w->hwnd); return; }
    }
    RET(DESKTOP_HWND);
}
void I_user32_dll_RealChildWindowFromPoint(CPU *c) { RET(A0); }

// ---------------- activation / focus / capture ----------------
void I_user32_dll_GetActiveWindow(CPU *c) { RET(g_active); }
void I_user32_dll_GetForegroundWindow(CPU *c) { RET(g_active); }
void I_user32_dll_GetFocus(CPU *c) { RET(g_focus); }
static uint64_t activate(uint64_t h) {
    uint64_t prev = g_active; Win *w = win_get(h);
    while (w && !toplevel(w)) w = win_get(w->parent);
    if (w) { if (w->ns) cocoa_window_activate(w->ns); g_active = w->hwnd; g_focus = h; }
    return prev;
}
void I_user32_dll_SetActiveWindow(CPU *c) { RET(activate(A0)); }
void I_user32_dll_SetForegroundWindow(CPU *c) { activate(A0); RET(win_get(A0) != NULL); }
void I_user32_dll_BringWindowToTop(CPU *c) { activate(A0); RET(win_get(A0) != NULL); }
void I_user32_dll_SetFocus(CPU *c) { uint64_t prev = g_focus; if (win_get(A0)) g_focus = A0; else if (!A0) g_focus = 0; RET(prev); }
void I_user32_dll_GetCapture(CPU *c) { RET(g_capture); }
void I_user32_dll_SetCapture(CPU *c) { uint64_t prev = g_capture; g_capture = A0; RET(prev); }
void I_user32_dll_ReleaseCapture(CPU *c) { g_capture = 0; RET(1); }
void I_user32_dll_AttachThreadInput(CPU *c) { RET(1); }

// ---------------- painting (the game renders through Vulkan) ----------------
#define PSEUDO_DC 0x4744490000000001ull
void I_user32_dll_InvalidateRect(CPU *c) { RET(1); }
void I_user32_dll_ValidateRect(CPU *c) { RET(1); }
void I_user32_dll_RedrawWindow(CPU *c) { RET(1); }
void I_user32_dll_LockWindowUpdate(CPU *c) { RET(1); }
void I_user32_dll_GetUpdateRect(CPU *c) { if (A1) put_rect(A1, 0, 0, 0, 0); RET(0); }
// BeginPaint(hwnd, PAINTSTRUCT*): hdc +0, fErase +8, rcPaint +12 (72 bytes)
void I_user32_dll_BeginPaint(CPU *c) {
    Win *w = win_get(A0); memset(PTR(A1), 0, 72); wr64(A1, PSEUDO_DC);
    if (w) put_rect(A1 + 12, 0, 0, w->cw, w->ch);
    RET(PSEUDO_DC);
}
void I_user32_dll_EndPaint(CPU *c) { RET(1); }
void I_user32_dll_SetLayeredWindowAttributes(CPU *c) { RET(1); }
void I_user32_dll_UpdateLayeredWindow(CPU *c) { RET(1); }
void I_user32_dll_ShowOwnedPopups(CPU *c) { RET(1); }

// ---------------- cursor ----------------
static int cursor_count;
void *rt_cursor_native(uint64_t h);   // user32.c
// SetCursor(h): with rt_game.win32_cursors, a custom cursor (CreateIconIndirect) shows its image, a system cursor
// the arrow and NULL no image; without it the handle is only recorded (GetCursor returns it)
static void apply_cursor(uint64_t h) {
    if (h == g_cursor) return;
    g_cursor = h;
    if (rt_game.win32_cursors) cocoa_cursor_set(rt_cursor_native(h), h == 0);
}
void I_user32_dll_SetCursor(CPU *c) {
    uint64_t prev = g_cursor;
    apply_cursor(A0);
    RET(prev);
}
void I_user32_dll_GetCursor(CPU *c) { RET(g_cursor); }
// ShowCursor(show) -> new display counter; the cursor is hidden while it is negative
void I_user32_dll_ShowCursor(CPU *c) {
    int before = cursor_count;
    cursor_count += A0 ? 1 : -1;
    if (before >= 0 && cursor_count < 0) cocoa_cursor_visible(0);
    if (before < 0 && cursor_count >= 0) cocoa_cursor_visible(1);
    RET((uint32_t)cursor_count);
}
void I_user32_dll_GetCursorPos(CPU *c) { int x = 0, y = 0; cocoa_cursor_pos(&x, &y); wr32(A0, (uint32_t)x); wr32(A0 + 4, (uint32_t)y); RET(1); }
void I_user32_dll_SetCursorPos(CPU *c) { cocoa_set_cursor_pos((int32_t)A0, (int32_t)A1); RET(1); }
static int32_t clip_rect[4]; static int clip_on;
void I_user32_dll_ClipCursor(CPU *c) { clip_on = A0 != 0; if (A0) memcpy(clip_rect, PTR(A0), 16); RET(1); }   // recorded, not enforced
void I_user32_dll_GetClipCursor(CPU *c) { int32_t r[4]; if (clip_on) memcpy(r, clip_rect, 16); else rt_primary_rect(r); memcpy(PTR(A0), r, 16); RET(1); }
void I_user32_dll_TrackMouseEvent(CPU *c) { RET(1); }

// ---------------- keyboard ----------------
// Mac virtual keycode -> Windows virtual key, set-1 scancode, extended flag, key name
typedef struct { uint8_t mac, vk, scan, ext; uint8_t lr; const char *name; } Key;
enum { VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12, VK_CAPITAL = 0x14, VK_F10 = 0x79, VK_LSHIFT = 0xA0, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU };
static const Key keys[] = {
    {0x00,'A',0x1E,0,0,"A"},{0x01,'S',0x1F,0,0,"S"},{0x02,'D',0x20,0,0,"D"},{0x03,'F',0x21,0,0,"F"},{0x04,'H',0x23,0,0,"H"},
    {0x05,'G',0x22,0,0,"G"},{0x06,'Z',0x2C,0,0,"Z"},{0x07,'X',0x2D,0,0,"X"},{0x08,'C',0x2E,0,0,"C"},{0x09,'V',0x2F,0,0,"V"},
    {0x0B,'B',0x30,0,0,"B"},{0x0C,'Q',0x10,0,0,"Q"},{0x0D,'W',0x11,0,0,"W"},{0x0E,'E',0x12,0,0,"E"},{0x0F,'R',0x13,0,0,"R"},
    {0x10,'Y',0x15,0,0,"Y"},{0x11,'T',0x14,0,0,"T"},{0x12,'1',0x02,0,0,"1"},{0x13,'2',0x03,0,0,"2"},{0x14,'3',0x04,0,0,"3"},
    {0x15,'4',0x05,0,0,"4"},{0x16,'6',0x07,0,0,"6"},{0x17,'5',0x06,0,0,"5"},{0x18,0xBB,0x0D,0,0,"="},{0x19,'9',0x0A,0,0,"9"},
    {0x1A,'7',0x08,0,0,"7"},{0x1B,0xBD,0x0C,0,0,"-"},{0x1C,'8',0x09,0,0,"8"},{0x1D,'0',0x0B,0,0,"0"},{0x1E,0xDD,0x1B,0,0,"]"},
    {0x1F,'O',0x18,0,0,"O"},{0x20,'U',0x16,0,0,"U"},{0x21,0xDB,0x1A,0,0,"["},{0x22,'I',0x17,0,0,"I"},{0x23,'P',0x19,0,0,"P"},
    {0x24,0x0D,0x1C,0,0,"Enter"},{0x25,'L',0x26,0,0,"L"},{0x26,'J',0x24,0,0,"J"},{0x27,0xDE,0x28,0,0,"'"},{0x28,'K',0x25,0,0,"K"},
    {0x29,0xBA,0x27,0,0,";"},{0x2A,0xDC,0x2B,0,0,"\\"},{0x2B,0xBC,0x33,0,0,","},{0x2C,0xBF,0x35,0,0,"/"},{0x2D,'N',0x31,0,0,"N"},
    {0x2E,'M',0x32,0,0,"M"},{0x2F,0xBE,0x34,0,0,"."},{0x30,0x09,0x0F,0,0,"Tab"},{0x31,0x20,0x39,0,0,"Space"},{0x32,0xC0,0x29,0,0,"`"},
    {0x33,0x08,0x0E,0,0,"Backspace"},{0x35,0x1B,0x01,0,0,"Esc"},
    {0x37,0x5B,0x5B,1,0,"Left Windows"},{0x36,0x5C,0x5C,1,0,"Right Windows"},
    {0x38,VK_SHIFT,0x2A,0,VK_LSHIFT,"Shift"},{0x3C,VK_SHIFT,0x36,0,VK_RSHIFT,"Right Shift"},
    {0x3B,VK_CONTROL,0x1D,0,VK_LCONTROL,"Ctrl"},{0x3E,VK_CONTROL,0x1D,1,VK_RCONTROL,"Right Ctrl"},
    {0x3A,VK_MENU,0x38,0,VK_LMENU,"Alt"},{0x3D,VK_MENU,0x38,1,VK_RMENU,"Right Alt"},
    {0x39,VK_CAPITAL,0x3A,0,0,"Caps Lock"},
    {0x41,0x6E,0x53,0,0,"Num Del"},{0x43,0x6A,0x37,0,0,"Num *"},{0x45,0x6B,0x4E,0,0,"Num +"},{0x47,0x90,0x45,0,0,"Num Lock"},
    {0x4B,0x6F,0x35,1,0,"Num /"},{0x4C,0x0D,0x1C,1,0,"Num Enter"},{0x4E,0x6D,0x4A,0,0,"Num -"},
    {0x52,0x60,0x52,0,0,"Num 0"},{0x53,0x61,0x4F,0,0,"Num 1"},{0x54,0x62,0x50,0,0,"Num 2"},{0x55,0x63,0x51,0,0,"Num 3"},
    {0x56,0x64,0x4B,0,0,"Num 4"},{0x57,0x65,0x4C,0,0,"Num 5"},{0x58,0x66,0x4D,0,0,"Num 6"},{0x59,0x67,0x47,0,0,"Num 7"},
    {0x5B,0x68,0x48,0,0,"Num 8"},{0x5C,0x69,0x49,0,0,"Num 9"},
    {0x7A,0x70,0x3B,0,0,"F1"},{0x78,0x71,0x3C,0,0,"F2"},{0x63,0x72,0x3D,0,0,"F3"},{0x76,0x73,0x3E,0,0,"F4"},{0x60,0x74,0x3F,0,0,"F5"},
    {0x61,0x75,0x40,0,0,"F6"},{0x62,0x76,0x41,0,0,"F7"},{0x64,0x77,0x42,0,0,"F8"},{0x65,0x78,0x43,0,0,"F9"},{0x6D,0x79,0x44,0,0,"F10"},
    {0x67,0x7A,0x57,0,0,"F11"},{0x6F,0x7B,0x58,0,0,"F12"},{0x69,0x7C,0x64,0,0,"F13"},{0x6B,0x7D,0x65,0,0,"F14"},{0x71,0x7E,0x66,0,0,"F15"},
    {0x72,0x2D,0x52,1,0,"Insert"},{0x73,0x24,0x47,1,0,"Home"},{0x74,0x21,0x49,1,0,"Page Up"},{0x75,0x2E,0x53,1,0,"Delete"},
    {0x77,0x23,0x4F,1,0,"End"},{0x79,0x22,0x51,1,0,"Page Down"},
    {0x7B,0x25,0x4B,1,0,"Left"},{0x7C,0x27,0x4D,1,0,"Right"},{0x7D,0x28,0x50,1,0,"Down"},{0x7E,0x26,0x48,1,0,"Up"},
};
#define NKEYS (sizeof keys / sizeof *keys)
static uint8_t keystate[256];   // 0x80 down, 0x01 toggled
static const Key *key_mac(uint16_t m) { for (size_t i = 0; i < NKEYS; i++) if (keys[i].mac == m) return &keys[i]; return NULL; }
static const Key *key_vk(uint32_t vk) {
    for (size_t i = 0; i < NKEYS; i++) if (keys[i].vk == vk || (keys[i].lr && keys[i].lr == vk)) return &keys[i];
    return NULL;
}
static const Key *key_scan(uint32_t scan, int ext) { for (size_t i = 0; i < NKEYS; i++) if (keys[i].scan == scan && keys[i].ext == ext) return &keys[i]; return NULL; }
void win_set_key(int vk, int down) {
    if (vk > 0 && vk < 256) keystate[vk] = (uint8_t)((keystate[vk] & 1) | (down ? 0x80 : 0));
    static const int mouse_button[7] = { -1, 0, 1, -1, 2, 3, 4 };   // VK_LBUTTON, RBUTTON, MBUTTON, XBUTTON1/2
    if (vk > 0 && vk < 7 && mouse_button[vk] >= 0) di_mouse_button(mouse_button[vk], down);
}

void win_key_from_cocoa(uint64_t h, uint16_t mac, int down, int repeat, const uint16_t *chars, int nchars) {
    Win *w = win_get(h);
    if (!w) return;
    if (mac == 0x65 && down && !repeat) { extern void rt_vk_snapshot_request(void); rt_vk_snapshot_request(); }   // F9: RT_VK_DUMP frame snapshot
    if (mac == 0x6D && down && !repeat) { extern void rt_vk_skip_next(void); rt_vk_skip_next(); }               // F10: RT_VK_DUMP bisection
    const Key *k = key_mac(mac);
    if (k) {
        if (k->vk == VK_CAPITAL && down) keystate[VK_CAPITAL] ^= 1;
        win_set_key(k->vk, down);
        if (k->lr) { win_set_key(k->lr, down); win_set_key(k->vk, (keystate[k->lr ^ 1] & 0x80) || down); }   // generic = either side
        di_key(k->scan, k->ext, down);
    }
    int alt = (keystate[VK_MENU] & 0x80) != 0;
    uint32_t scan = k ? k->scan : 0, ext = k ? k->ext : 0;
    uint64_t lp = 1 | (uint64_t)scan << 16 | (uint64_t)ext << 24 | (alt ? 1ull << 29 : 0) | ((repeat || !down) ? 1ull << 30 : 0) | (!down ? 1ull << 31 : 0);
    if (k) {
        int sys = alt || k->vk == VK_MENU || k->vk == VK_F10;
        if (k->vk == VK_MENU && !down) sys = 1;
        post(w->tid, h, down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP), k->vk, lp);
    }
    for (int i = 0; i < nchars; i++) {
        uint16_t ch = chars[i];
        if (ch >= 0xF700 && ch <= 0xF8FF) continue;   // AppKit function-key placeholders
        if (ch == 0x7F) ch = 0x08;                    // Mac backspace
        post(w->tid, h, alt ? WM_SYSCHAR : WM_CHAR, ch, lp);
    }
}
void I_user32_dll_GetAsyncKeyState(CPU *c) { uint32_t vk = (uint32_t)A0 & 0xFF; RET((keystate[vk] & 0x80) ? 0x8000 : 0); }
void I_user32_dll_GetKeyState(CPU *c) { uint32_t vk = (uint32_t)A0 & 0xFF; RET((uint64_t)(int64_t)(int16_t)(((keystate[vk] & 0x80) ? 0xFF80 : 0) | (keystate[vk] & 1))); }
void I_user32_dll_GetKeyboardState(CPU *c) { memcpy(PTR(A0), keystate, 256); RET(1); }
// US layout character for a virtual key
static uint16_t vk_char(uint32_t vk, int shift) {
    if (vk >= 'A' && vk <= 'Z') return (uint16_t)(shift ? vk : vk + 32);
    if (vk >= '0' && vk <= '9') return shift ? (uint16_t)")!@#$%^&*("[vk - '0'] : (uint16_t)vk;
    if (vk >= 0x60 && vk <= 0x69) return (uint16_t)('0' + vk - 0x60);
    switch (vk) {
    case 0x20: return ' '; case 0x0D: return '\r'; case 0x09: return '\t'; case 0x08: return 8; case 0x1B: return 0x1B;
    case 0xBA: return shift ? ':' : ';'; case 0xBB: return shift ? '+' : '='; case 0xBC: return shift ? '<' : ',';
    case 0xBD: return shift ? '_' : '-'; case 0xBE: return shift ? '>' : '.'; case 0xBF: return shift ? '?' : '/';
    case 0xC0: return shift ? '~' : '`'; case 0xDB: return shift ? '{' : '['; case 0xDC: return shift ? '|' : '\\';
    case 0xDD: return shift ? '}' : ']'; case 0xDE: return shift ? '"' : '\'';
    case 0x6A: return '*'; case 0x6B: return '+'; case 0x6D: return '-'; case 0x6E: return '.'; case 0x6F: return '/';
    default: return 0;
    }
}
// MapVirtualKey(code, type: VK_TO_VSC 0, VSC_TO_VK 1, VK_TO_CHAR 2, VSC_TO_VK_EX 3, VK_TO_VSC_EX 4)
static uint32_t map_vk(uint32_t code, uint32_t type) {
    const Key *k;
    switch (type) {
    case 0: case 4: k = key_vk(code); return k ? (k->scan | (type == 4 && k->ext ? 0xE000 : 0)) : 0;
    case 1: case 3: k = key_scan(code & 0xFF, (code & 0xFF00) == 0xE000); return k ? (type == 3 && k->lr ? k->lr : k->vk) : 0;
    case 2: return vk_char(code, 1) >= 'A' && vk_char(code, 1) <= 'Z' ? vk_char(code, 1) : vk_char(code, 0);
    default: return 0;
    }
}
void I_user32_dll_MapVirtualKeyA(CPU *c) { RET(map_vk((uint32_t)A0, (uint32_t)A1)); }
void I_user32_dll_MapVirtualKeyW(CPU *c) { RET(map_vk((uint32_t)A0, (uint32_t)A1)); }
void I_user32_dll_MapVirtualKeyExA(CPU *c) { RET(map_vk((uint32_t)A0, (uint32_t)A1)); }
void I_user32_dll_MapVirtualKeyExW(CPU *c) { RET(map_vk((uint32_t)A0, (uint32_t)A1)); }
// ToUnicode(vk, scan, keystate[256], buf, n, flags)
void I_user32_dll_ToUnicode(CPU *c) {
    const uint8_t *ks = PTR(A2);
    uint16_t ch = vk_char((uint32_t)A0, ks && ((ks[VK_SHIFT] & 0x80) != 0) != ((ks[VK_CAPITAL] & 1) && A0 >= 'A' && A0 <= 'Z'));
    if (!ch || !AN(4)) { RET(0); return; }
    wr16(A3, ch); if (AN(4) > 1) wr16(A3 + 2, 0);
    RET(1);
}
// ToAsciiEx(vk, scan, keystate[256], WORD *out, flags, hkl)
void I_user32_dll_ToAsciiEx(CPU *c) {
    const uint8_t *ks = PTR(A2);
    uint16_t ch = vk_char((uint32_t)A0, ks && (ks[VK_SHIFT] & 0x80));
    if (!ch || ch > 0x7F) { RET(0); return; }
    wr16(A3, ch);
    RET(1);
}
void I_user32_dll_ToAscii(CPU *c) { I_user32_dll_ToAsciiEx(c); }
// GetKeyNameText(lParam, buf, n): scancode in bits 16-23, extended in bit 24
static void key_name(CPU *c, int wide) {
    const Key *k = key_scan((uint32_t)(A0 >> 16) & 0xFF, (int)(A0 >> 24) & 1);
    if (!k) k = key_scan((uint32_t)(A0 >> 16) & 0xFF, 0);
    const char *n = k ? k->name : "";
    uint32_t sz = (uint32_t)A2;
    if (!sz) { RET(0); return; }
    if (wide) { uint16_t *u = utf8_to_w16(n); size_t L = w16len(u); if (L >= sz) L = sz - 1; memcpy(PTR(A1), u, L * 2); wr16(A1 + 2 * L, 0); free(u); RET(L); }
    else { strlcpy(PTR(A1), n, sz); RET(strlen(PTR(A1))); }
}
void I_user32_dll_GetKeyNameTextA(CPU *c) { key_name(c, 0); }
void I_user32_dll_GetKeyNameTextW(CPU *c) { key_name(c, 1); }

// ---------------- rectangles ----------------
static int32_t R(uint64_t p, int i) { return (int32_t)rd32(p + 4ull * (uint64_t)i); }
void I_user32_dll_SetRect(CPU *c) { put_rect(A0, (int32_t)A1, (int32_t)A2, (int32_t)A3, (int32_t)AN(4)); RET(1); }
void I_user32_dll_SetRectEmpty(CPU *c) { put_rect(A0, 0, 0, 0, 0); RET(1); }
void I_user32_dll_CopyRect(CPU *c) { if (!A0 || !A1) { RET(0); return; } memmove(PTR(A0), PTR(A1), 16); RET(1); }
void I_user32_dll_IsRectEmpty(CPU *c) { RET(!A0 || R(A0, 2) <= R(A0, 0) || R(A0, 3) <= R(A0, 1)); }
void I_user32_dll_EqualRect(CPU *c) { RET(!memcmp(PTR(A0), PTR(A1), 16)); }
void I_user32_dll_InflateRect(CPU *c) { int32_t dx = (int32_t)A1, dy = (int32_t)A2; put_rect(A0, R(A0, 0) - dx, R(A0, 1) - dy, R(A0, 2) + dx, R(A0, 3) + dy); RET(1); }
void I_user32_dll_OffsetRect(CPU *c) { int32_t dx = (int32_t)A1, dy = (int32_t)A2; put_rect(A0, R(A0, 0) + dx, R(A0, 1) + dy, R(A0, 2) + dx, R(A0, 3) + dy); RET(1); }
void I_user32_dll_PtInRect(CPU *c) { int32_t x = (int32_t)A1, y = (int32_t)(A1 >> 32); RET(x >= R(A0, 0) && x < R(A0, 2) && y >= R(A0, 1) && y < R(A0, 3)); }
void I_user32_dll_IntersectRect(CPU *c) {
    int32_t l = R(A1, 0) > R(A2, 0) ? R(A1, 0) : R(A2, 0), t = R(A1, 1) > R(A2, 1) ? R(A1, 1) : R(A2, 1);
    int32_t r = R(A1, 2) < R(A2, 2) ? R(A1, 2) : R(A2, 2), b = R(A1, 3) < R(A2, 3) ? R(A1, 3) : R(A2, 3);
    if (r <= l || b <= t) { put_rect(A0, 0, 0, 0, 0); RET(0); return; }
    put_rect(A0, l, t, r, b); RET(1);
}
void I_user32_dll_UnionRect(CPU *c) {
    int e1 = R(A1, 2) <= R(A1, 0) || R(A1, 3) <= R(A1, 1), e2 = R(A2, 2) <= R(A2, 0) || R(A2, 3) <= R(A2, 1);
    if (e1 && e2) { put_rect(A0, 0, 0, 0, 0); RET(0); return; }
    if (e1) { memmove(PTR(A0), PTR(A2), 16); RET(1); return; }
    if (e2) { memmove(PTR(A0), PTR(A1), 16); RET(1); return; }
    put_rect(A0, R(A1, 0) < R(A2, 0) ? R(A1, 0) : R(A2, 0), R(A1, 1) < R(A2, 1) ? R(A1, 1) : R(A2, 1),
             R(A1, 2) > R(A2, 2) ? R(A1, 2) : R(A2, 2), R(A1, 3) > R(A2, 3) ? R(A1, 3) : R(A2, 3));
    RET(1);
}
// SubtractRect: only exact when the second rect covers a full side of the first (as on Windows)
void I_user32_dll_SubtractRect(CPU *c) {
    int32_t a[4], b[4]; for (int i = 0; i < 4; i++) { a[i] = R(A1, i); b[i] = R(A2, i); }
    if (b[0] <= a[0] && b[2] >= a[2]) { if (b[1] <= a[1] && b[3] > a[1]) a[1] = b[3]; else if (b[3] >= a[3] && b[1] < a[3]) a[3] = b[1]; }
    else if (b[1] <= a[1] && b[3] >= a[3]) { if (b[0] <= a[0] && b[2] > a[0]) a[0] = b[2]; else if (b[2] >= a[2] && b[0] < a[2]) a[2] = b[0]; }
    put_rect(A0, a[0], a[1], a[2], a[3]);
    RET(a[2] > a[0] && a[3] > a[1]);
}

// ---------------- system parameters / message boxes / misc ----------------
static void put_logfont(uint64_t p) { memset(PTR(p), 0, 60); wr32(p, (uint32_t)-12); wr32(p + 16, 400); strlcpy(PTR(p + 28), "Segoe UI", 32); }
// SystemParametersInfoA(action, uiParam, pv, winIni)
void I_user32_dll_SystemParametersInfoA(CPU *c) {
    uint32_t a = (uint32_t)A0; uint64_t pv = A2;
    switch (a) {
    case 0x30: { int32_t r[4]; rt_primary_rect(r); put_rect(pv, r[0], r[1], r[2], r[3]); RET(1); return; }   // SPI_GETWORKAREA
    case 0x29: {   // SPI_GETNONCLIENTMETRICS (NONCLIENTMETRICSA, 344 bytes)
        uint32_t cb = rd32(pv); if (cb > 344) cb = 344;
        memset(PTR(pv + 4), 0, cb - 4);
        wr32(pv + 4, 1); wr32(pv + 8, 17); wr32(pv + 12, 17); wr32(pv + 16, 22); wr32(pv + 20, 22);
        put_logfont(pv + 24); wr32(pv + 84, 22); wr32(pv + 88, 22); put_logfont(pv + 92);
        wr32(pv + 152, 19); wr32(pv + 156, 19); put_logfont(pv + 160); put_logfont(pv + 220); put_logfont(pv + 280);
        RET(1); return;
    }
    case 0x32: case 0x34: case 0x3A: case 0x42:   // FILTERKEYS / TOGGLEKEYS / STICKYKEYS / HIGHCONTRAST: all off
        { uint32_t cb = rd32(pv); if (cb >= 8 && cb <= 64) memset(PTR(pv + 4), 0, cb - 4); RET(1); return; }
    case 0x33: case 0x35: case 0x3B: case 0x11: case 0x43: case 0x71: RET(1); return;   // matching SETs
    case 0x10: wr32(pv, 0); RET(1); return;       // SPI_GETSCREENSAVEACTIVE
    case 0x03: wr32(pv, 6); wr32(pv + 4, 10); wr32(pv + 8, 1); RET(1); return;   // SPI_GETMOUSE
    case 0x70: wr32(pv, 10); RET(1); return;      // SPI_GETMOUSESPEED
    case 0x0A: wr32(pv, 31); RET(1); return;      // SPI_GETKEYBOARDSPEED
    case 0x16: wr32(pv, 1); RET(1); return;       // SPI_GETKEYBOARDDELAY
    case 0x68: wr32(pv, 3); RET(1); return;       // SPI_GETWHEELSCROLLLINES
    case 0x4A: wr32(pv, 1); RET(1); return;       // SPI_GETFONTSMOOTHING
    case 0x1002: case 0x1024: case 0x1022: case 0x1018: case 0x100A: wr32(pv, 0); RET(1); return;   // menu/UI effects: off
    default: {
        static uint32_t seen[64]; static int ns; int dup = 0;
        for (int i = 0; i < ns; i++) if (seen[i] == a) dup = 1;
        if (!dup && ns < 64) { seen[ns++] = a; rt_log("[user32] SystemParametersInfoA(0x%x) not supported\n", a); }
        RET(0);
    }
    }
}
// MessageBox(hwnd, text, caption, type): logged; the "yes"/"ok" answer is returned
static void message_box(CPU *c, int wide) {
    char *t = A1 ? str_arg(A1, wide) : strdup(""), *cap = A2 ? str_arg(A2, wide) : strdup("");
    rt_log("[user32] MessageBox \"%s\": %s\n", cap, t);
    free(t); free(cap);
    switch ((uint32_t)A3 & 0xF) { case 2: RET(3); break; case 3: case 4: RET(6); break; case 5: RET(2); break; default: RET(1); }
}
void I_user32_dll_MessageBoxA(CPU *c) { message_box(c, 0); }
void I_user32_dll_MessageBoxW(CPU *c) { message_box(c, 1); }
void I_user32_dll_MessageBeep(CPU *c) { RET(1); }
void I_user32_dll_DisableProcessWindowsGhosting(CPU *c) { }
void I_user32_dll_NotifyWinEvent(CPU *c) { }
void I_user32_dll_RegisterDeviceNotificationW(CPU *c) { RET(0xDE7000001ull); }
void I_user32_dll_UnregisterDeviceNotification(CPU *c) { RET(1); }
// hooks are accepted but never called
void I_user32_dll_SetWindowsHookExA(CPU *c) { RET(0x400001); }
void I_user32_dll_SetWindowsHookExW(CPU *c) { RET(0x400001); }
void I_user32_dll_UnhookWindowsHookEx(CPU *c) { RET(1); }
void I_user32_dll_CallNextHookEx(CPU *c) { RET(0); }

// ---------------- RT_INPUT_SCRIPT: scripted input for testing menus without a person ----------------
// "ms:item,ms:item,...": after each delay (ms since the previous item) press a key or take a screenshot.
// Items: esc enter up down left right tab space backspace, a mac key code number, or shot=/path.ppm.
// Keys go to the window the game renders into, like real key presses (window messages + DirectInput).
#include <pthread.h>
#include <unistd.h>
extern uint64_t rt_last_surface_hwnd;
void rt_request_screenshot(const char *path);
static void *input_script(void *arg) {
    char *script = strdup((const char *)arg), *save = NULL;
    for (char *it = strtok_r(script, ",", &save); it; it = strtok_r(NULL, ",", &save)) {
        char *colon = strchr(it, ':'); if (!colon) continue;
        *colon = 0; usleep((useconds_t)atoi(it) * 1000); const char *what = colon + 1;
        if (!strncmp(what, "shot=", 5)) { rt_request_screenshot(what + 5); rt_log("[input] screenshot %s\n", what + 5); continue; }
        static const struct { const char *n; uint16_t mac; } names[] = { { "esc", 0x35 }, { "enter", 0x24 }, { "up", 0x7E },
            { "down", 0x7D }, { "left", 0x7B }, { "right", 0x7C }, { "tab", 0x30 }, { "space", 0x31 }, { "backspace", 0x33 } };
        int mac = -1;
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) if (!strcmp(what, names[i].n)) mac = names[i].mac;
        if (mac < 0 && *what >= '0' && *what <= '9') mac = atoi(what);
        uint64_t h = rt_last_surface_hwnd;
        if (mac < 0 || !h) { rt_log("[input] skipped %s\n", what); continue; }
        rt_log("[input] %s\n", what);
        uint16_t ch = mac == 0x24 ? '\r' : mac == 0x35 ? 27 : 0;
        win_key_from_cocoa(h, (uint16_t)mac, 1, 0, &ch, ch ? 1 : 0);
        usleep(60000);
        win_key_from_cocoa(h, (uint16_t)mac, 0, 0, NULL, 0);
    }
    free(script);
    return NULL;
}
__attribute__((constructor)) static void input_script_start(void) {
    const char *s = getenv("RT_INPUT_SCRIPT");
    if (!s || !*s) return;
    pthread_t t; pthread_create(&t, NULL, input_script, (void *)s); pthread_detach(t);
}

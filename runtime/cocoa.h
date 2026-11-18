// AppKit side of the window system (cocoa.m), used by window.c.
// Coordinates are Windows-style pixels: origin at the top-left of the primary display, y down.
// The cocoa_window_* calls may come from any thread; the work runs on the main thread, which
// must be pumping messages (PeekMessage/GetMessage) for a call from another thread to finish.
#pragma once
#include <stdint.h>

int  cocoa_is_main_thread(void);
int  cocoa_active(void);                       // any window created yet
void cocoa_pump(int wait_ms);                  // main thread: handle AppKit events, waiting up to wait_ms for the first

void *cocoa_window_new(uint64_t hwnd, int x, int y, int cw, int ch, int titled, int resizable, int minimizable, const char *title);
void cocoa_window_free(void *w);
void cocoa_window_show(void *w, int show, int activate);
void cocoa_window_minimize(void *w, int minimize);
void cocoa_window_zoom(void *w, int zoom);
void cocoa_window_state(void *w, int *minimized, int *zoomed, int *visible);
void cocoa_window_set_style(void *w, int titled, int resizable, int minimizable);
void cocoa_window_set_client(void *w, int x, int y, int cw, int ch);   // client rect, screen pixels
void cocoa_window_get_client(void *w, int *x, int *y, int *cw, int *ch);
void cocoa_window_set_title(void *w, const char *title);
void cocoa_window_activate(void *w);
void *cocoa_window_layer(void *w);             // the CAMetalLayer behind the client area
void cocoa_frame_insets(int titled, int resizable, int *l, int *t, int *r, int *b);
void cocoa_set_virtual_screen(int w, int h);   // emulated primary display mode (0, 0 = native)
char *cocoa_choose_folder(const char *message);   // folder picker (malloc'd path or NULL)
void cocoa_alert(const char *title, const char *text);
char *cocoa_clipboard_text(void);              // malloc'd UTF-8 text on the pasteboard, or NULL
void cocoa_clipboard_set_text(const char *utf8);
void cocoa_clipboard_clear(void);
void cocoa_pick_friends(int n, const char *const *names, const char *const *details,
                        void (*done)(const int *picked, int npicked, void *ctx), void *ctx);   // async friends picker

void cocoa_cursor_pos(int *x, int *y);
void cocoa_set_cursor_pos(int x, int y);
void cocoa_cursor_visible(int visible);
void cocoa_mouse_delta(int *dx, int *dy);      // raw motion accumulated since the last call, pixels

// implemented by window.c; called on the main thread while cocoa_pump runs
void win_from_cocoa(uint64_t hwnd, uint32_t msg, uint64_t wparam, uint64_t lparam);
void win_key_from_cocoa(uint64_t hwnd, uint16_t mac_keycode, int down, int repeat, const uint16_t *chars, int nchars);
void win_set_key(int vk, int down);
void win_relayout(void);   // re-apply every top-level window's rect (after a display mode change)

// DirectInput feeds (dinput.c)
void di_key(uint32_t scan, int ext, int down);
void di_mouse_move(int dx, int dy);
void di_mouse_wheel(int dz);
void di_mouse_button(int button, int down);

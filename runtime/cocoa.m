// AppKit windows for the guest's HWNDs: an NSWindow whose content view is backed by a CAMetalLayer
// (the Vulkan surface), and AppKit events translated into Win32 messages for window.c.
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>
#include "cocoa.h"
void rt_log(const char *fmt, ...);

enum {
    WM_MOVE = 3, WM_SIZE = 5, WM_ACTIVATE = 6, WM_SETFOCUS = 7, WM_KILLFOCUS = 8, WM_CLOSE = 0x10, WM_ACTIVATEAPP = 0x1C,
    WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK, WM_RBUTTONDOWN, WM_RBUTTONUP, WM_RBUTTONDBLCLK,
    WM_MBUTTONDOWN, WM_MBUTTONUP, WM_MBUTTONDBLCLK, WM_MOUSEWHEEL, WM_XBUTTONDOWN, WM_XBUTTONUP,
};
enum { VK_LBUTTON = 1, VK_RBUTTON = 2, VK_MBUTTON = 4, VK_XBUTTON1 = 5, VK_XBUTTON2 = 6 };

static double g_scale = 1;     // pixels per point (primary display)
static int g_active;
static _Atomic int g_dx, g_dy;

// pixels per point of the primary display, known before AppKit starts (any thread)
static void scale_init(void) {
    CGDirectDisplayID d = CGMainDisplayID();
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(d);
    double pts = CGDisplayBounds(d).size.width;
    if (m && pts > 0) g_scale = CGDisplayModeGetPixelWidth(m) / pts;
    if (m) CGDisplayModeRelease(m);
    if (g_scale <= 0) g_scale = 1;
}
static pthread_once_t scale_once = PTHREAD_ONCE_INIT;
@class RTWindow;
static void update_presentation(RTWindow *w);
static void on_main(dispatch_block_t b) { if ([NSThread isMainThread]) b(); else dispatch_sync(dispatch_get_main_queue(), b); }
int cocoa_is_main_thread(void) { return [NSThread isMainThread]; }
int cocoa_active(void) { return g_active; }

// ---------------- coordinates ----------------
static double primary_h(void) { double h = CGDisplayBounds(CGMainDisplayID()).size.height; return h > 0 ? h : 1080; }   // thread-safe
// Guest pixels -> screen points: pt = offset + px * k (top-left origin). Natively k = 1 / backing scale.
// After ChangeDisplaySettings(W x H) the primary screen is emulated at W x H: k scales that mode onto the
// real screen keeping its aspect ratio, and the offset centres it (black bars fill the rest).
static int g_vw, g_vh;   // emulated mode of the primary display (0 = native)
static NSCursor *g_cur;   // the guest's custom cursor (SetCursor), nil = the system arrow
static BOOL g_cur_blank;  // SetCursor(NULL): no cursor image
static double primary_w(void) { double w = CGDisplayBounds(CGMainDisplayID()).size.width; return w > 0 ? w : 1920; }
static void xform(double *k, double *ox, double *oy) {
    if (!g_vw || !g_vh) { *k = 1.0 / g_scale; *ox = *oy = 0; return; }
    double W = primary_w(), H = primary_h(), kx = W / g_vw, ky = H / g_vh;
    *k = kx < ky ? kx : ky; *ox = (W - g_vw * *k) / 2; *oy = (H - g_vh * *k) / 2;
}
static double px_per_pt(void) { double k, ox, oy; xform(&k, &ox, &oy); return 1.0 / k; }
static NSRect px_to_mac(int x, int y, int w, int h) {
    double k, ox, oy; xform(&k, &ox, &oy);
    return NSMakeRect(ox + x * k, primary_h() - (oy + (y + h) * k), w * k, h * k);
}
static void mac_to_px(NSRect r, int *x, int *y, int *w, int *h) {
    double k, ox, oy; xform(&k, &ox, &oy);
    *x = (int)lround((r.origin.x - ox) / k); *y = (int)lround((primary_h() - r.origin.y - r.size.height - oy) / k);
    *w = (int)lround(r.size.width / k); *h = (int)lround(r.size.height / k);
}
static NSWindowStyleMask style_mask(int titled, int resizable, int minimizable) {
    if (!titled) return NSWindowStyleMaskBorderless;
    return NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | (resizable ? NSWindowStyleMaskResizable : 0) | (minimizable ? NSWindowStyleMaskMiniaturizable : 0);
}

// ---------------- app ----------------
@interface RTAppDelegate : NSObject <NSApplicationDelegate> @end
@implementation RTAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    for (NSWindow *w in app.windows) [w performClose:nil];   // Cmd-Q: WM_CLOSE to every window; the game decides
    return NSTerminateCancel;
}
- (void)applicationDidBecomeActive:(NSNotification *)n {
    for (NSWindow *w in NSApp.windows) if (w.visible && [w respondsToSelector:@selector(view)]) update_presentation((RTWindow *)w);
    win_from_cocoa(0, WM_ACTIVATEAPP, 1, 0);
}
- (void)applicationDidResignActive:(NSNotification *)n { win_from_cocoa(0, WM_ACTIVATEAPP, 0, 0); }
@end

static void app_init(void) {   // main thread
    static RTAppDelegate *delegate;
    if (delegate) return;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    delegate = [RTAppDelegate new];
    NSApp.delegate = delegate;
    NSMenu *bar = [NSMenu new], *app = [NSMenu new];
    NSMenuItem *item = [NSMenuItem new];
    [app addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
    item.submenu = app; [bar addItem:item];
    NSApp.mainMenu = bar;
    [NSApp finishLaunching];
    { extern void gamepad_start(void); gamepad_start(); }   // controllers are known before the game first asks
    pthread_once(&scale_once, scale_init);
}

// ---------------- view: Metal layer + input ----------------
@interface RTView : NSView
@property double pxPerPt;   // guest pixels per point for this view (0 = the global mapping)
@property uint64_t hwnd;
@end
@implementation RTView
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer { CAMetalLayer *l = [CAMetalLayer layer]; l.contentsScale = px_per_pt(); return l; }   // drawable = guest pixels
- (void)viewDidChangeBackingProperties { self.layer.contentsScale = px_per_pt(); }
- (BOOL)isFlipped { return YES; }
// The guest's cursor (SetCursor) as this view's cursor rect: AppKit re-applies a view's cursor whenever the mouse
// enters or moves in it, and would otherwise put the arrow back over our [NSCursor set].
- (void)resetCursorRects { [self addCursorRect:self.bounds cursor:(g_cur ?: [NSCursor arrowCursor])]; }
- (void)cursorUpdate:(NSEvent *)e { [(g_cur ?: [NSCursor arrowCursor]) set]; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }

static uint64_t mk_flags(NSEvent *e) {
    NSUInteger b = [NSEvent pressedMouseButtons]; NSEventModifierFlags m = e.modifierFlags;
    return ((b & 1) ? 1 : 0) | ((b & 2) ? 2 : 0) | ((m & NSEventModifierFlagShift) ? 4 : 0) | ((m & NSEventModifierFlagControl) ? 8 : 0) |
           ((b & 4) ? 0x10 : 0) | ((b & 8) ? 0x20 : 0) | ((b & 16) ? 0x40 : 0);
}
- (uint64_t)clientPos:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    double s = self.pxPerPt > 0 ? self.pxPerPt : px_per_pt(); int x = (int)lround(p.x * s), y = (int)lround(p.y * s);
    return (uint64_t)(uint16_t)x | (uint64_t)(uint16_t)y << 16;
}
- (void)button:(NSEvent *)e msg:(uint32_t)msg vk:(int)vk down:(int)down xbutton:(int)xb {
    win_set_key(vk, down);
    win_from_cocoa(self.hwnd, msg, mk_flags(e) | (uint64_t)xb << 16, [self clientPos:e]);
}
- (void)mouseDown:(NSEvent *)e { [self button:e msg:WM_LBUTTONDOWN vk:VK_LBUTTON down:1 xbutton:0]; }
- (void)mouseUp:(NSEvent *)e { [self button:e msg:WM_LBUTTONUP vk:VK_LBUTTON down:0 xbutton:0]; }
- (void)rightMouseDown:(NSEvent *)e { [self button:e msg:WM_RBUTTONDOWN vk:VK_RBUTTON down:1 xbutton:0]; }
- (void)rightMouseUp:(NSEvent *)e { [self button:e msg:WM_RBUTTONUP vk:VK_RBUTTON down:0 xbutton:0]; }
- (void)otherMouseDown:(NSEvent *)e {
    if (e.buttonNumber == 2) [self button:e msg:WM_MBUTTONDOWN vk:VK_MBUTTON down:1 xbutton:0];
    else if (e.buttonNumber <= 4) [self button:e msg:WM_XBUTTONDOWN vk:(int)(VK_XBUTTON1 + e.buttonNumber - 3) down:1 xbutton:(int)(e.buttonNumber - 2)];
}
- (void)otherMouseUp:(NSEvent *)e {
    if (e.buttonNumber == 2) [self button:e msg:WM_MBUTTONUP vk:VK_MBUTTON down:0 xbutton:0];
    else if (e.buttonNumber <= 4) [self button:e msg:WM_XBUTTONUP vk:(int)(VK_XBUTTON1 + e.buttonNumber - 3) down:0 xbutton:(int)(e.buttonNumber - 2)];
}
- (void)moved:(NSEvent *)e {
    int dx = (int)lround(e.deltaX * g_scale), dy = (int)lround(e.deltaY * g_scale);
    atomic_fetch_add(&g_dx, dx); atomic_fetch_add(&g_dy, dy);
    di_mouse_move(dx, dy);
    if (g_cur) [g_cur set];   // AppKit may have put the arrow back (window edges, other apps)
    // Windows sends WM_SETCURSOR(hwnd, HTCLIENT | WM_MOUSEMOVE << 16) before each mouse move; UE4 picks and sets
    // its cursor in response. Sent at most every 50 ms so moves still coalesce in the queue.
    static uint64_t last_sc; uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now - last_sc > 50000000ull) { last_sc = now; win_from_cocoa(self.hwnd, 0x20, self.hwnd, 1 | (uint64_t)WM_MOUSEMOVE << 16); }
    win_from_cocoa(self.hwnd, WM_MOUSEMOVE, mk_flags(e), [self clientPos:e]);
}
- (void)mouseMoved:(NSEvent *)e { [self moved:e]; }
- (void)mouseDragged:(NSEvent *)e { [self moved:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self moved:e]; }
- (void)otherMouseDragged:(NSEvent *)e { [self moved:e]; }
- (void)scrollWheel:(NSEvent *)e {
    double d = e.hasPreciseScrollingDeltas ? e.scrollingDeltaY * 4 : e.scrollingDeltaY * 120;
    if (fabs(d) < 1) return;
    di_mouse_wheel((int)lround(d));
    int x, y; cocoa_cursor_pos(&x, &y);
    win_from_cocoa(self.hwnd, WM_MOUSEWHEEL, mk_flags(e) | (uint64_t)(uint16_t)(int16_t)lround(d) << 16, (uint64_t)(uint16_t)x | (uint64_t)(uint16_t)y << 16);
}
- (void)key:(NSEvent *)e down:(int)down {
    // with Option held, Windows reports the unmodified character (WM_SYSCHAR)
    NSString *s = (e.modifierFlags & NSEventModifierFlagOption) ? e.charactersIgnoringModifiers : e.characters;
    uint16_t buf[16]; int n = (int)MIN(s.length, (NSUInteger)16);
    [s getCharacters:buf range:NSMakeRange(0, n)];
    win_key_from_cocoa(self.hwnd, e.keyCode, down, down && e.isARepeat, buf, down ? n : 0);
}
- (void)keyDown:(NSEvent *)e { [self key:e down:1]; }
- (void)keyUp:(NSEvent *)e { [self key:e down:0]; }
- (void)flagsChanged:(NSEvent *)e {
    NSUInteger f = e.modifierFlags, mask;
    switch (e.keyCode) {   // device-dependent left/right modifier bits
    case 0x38: mask = 0x2; break;   case 0x3C: mask = 0x4; break;      // shift
    case 0x3B: mask = 0x1; break;   case 0x3E: mask = 0x2000; break;   // control
    case 0x3A: mask = 0x20; break;  case 0x3D: mask = 0x40; break;     // option
    case 0x37: mask = 0x8; break;   case 0x36: mask = 0x10; break;     // command
    case 0x39: win_key_from_cocoa(self.hwnd, 0x39, 1, 0, NULL, 0); win_key_from_cocoa(self.hwnd, 0x39, 0, 0, NULL, 0); return;
    default: return;
    }
    win_key_from_cocoa(self.hwnd, e.keyCode, (f & mask) != 0, 0, NULL, 0);
}
@end

// ---------------- window ----------------
@interface RTWindow : NSWindow <NSWindowDelegate>
@property uint64_t hwnd;
@property(strong) RTView *view;   // the game's client area (inside a black container view)
@property BOOL fullscreen;          // borderless and covering the screen: the guest rect below is reported as-is
@property int fsx, fsy, fsw, fsh;
@end
static NSRect client_screen_rect(RTWindow *w) { return [w convertRectToScreen:[w.view convertRect:w.view.bounds toView:nil]]; }
static void guest_client(RTWindow *w, int *x, int *y, int *cw, int *ch) {
    if (w.fullscreen) { *x = w.fsx; *y = w.fsy; *cw = w.fsw; *ch = w.fsh; return; }
    mac_to_px(client_screen_rect(w), x, y, cw, ch);
}
static void update_presentation(RTWindow *w);
static void layout(RTWindow *w, int x, int y, int cw, int ch);
@implementation RTWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
- (uint64_t)clientSize { int x, y, w, h; guest_client(self, &x, &y, &w, &h); return (uint64_t)(uint16_t)w | (uint64_t)(uint16_t)h << 16; }
- (uint64_t)clientOrigin { int x, y, w, h; guest_client(self, &x, &y, &w, &h); return (uint64_t)(uint16_t)x | (uint64_t)(uint16_t)y << 16; }
- (BOOL)windowShouldClose:(NSWindow *)w { win_from_cocoa(self.hwnd, WM_CLOSE, 0, 0); return NO; }
- (void)windowDidResize:(NSNotification *)n { win_from_cocoa(self.hwnd, WM_SIZE, self.zoomed ? 2 : 0, [self clientSize]); }
- (void)windowDidMove:(NSNotification *)n { win_from_cocoa(self.hwnd, WM_MOVE, 0, [self clientOrigin]); }
- (void)windowDidMiniaturize:(NSNotification *)n { win_from_cocoa(self.hwnd, WM_SIZE, 1, 0); }
- (void)windowDidDeminiaturize:(NSNotification *)n { win_from_cocoa(self.hwnd, WM_SIZE, 0, [self clientSize]); }
- (void)windowDidBecomeKey:(NSNotification *)n { update_presentation(self); win_from_cocoa(self.hwnd, WM_ACTIVATE, 1, 0); win_from_cocoa(self.hwnd, WM_SETFOCUS, 0, 0); }
- (void)windowDidResignKey:(NSNotification *)n { win_from_cocoa(self.hwnd, WM_ACTIVATE, 0, 0); win_from_cocoa(self.hwnd, WM_KILLFOCUS, 0, 0); }
@end

// A borderless window covering a whole display hides the menu bar and Dock (Windows-style "fullscreen").
// Menu bar and Dock hidden while a fullscreen window is shown. macOS only applies presentation options to the
// active app, so this is re-applied on activation and whenever the window becomes key.
static void update_presentation(RTWindow *w) {
    (void)w;
    if (!NSApp.active) return;
    // global, not per window: a hidden or windowed second window (the game's console) must not undo it
    BOOL full = NO;
    for (NSWindow *nw in NSApp.windows)
        if ([nw isKindOfClass:[RTWindow class]] && ((RTWindow *)nw).fullscreen && nw.visible) full = YES;
    NSApplicationPresentationOptions want = full ? (NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar) : NSApplicationPresentationDefault;
    if (NSApp.presentationOptions != want) NSApp.presentationOptions = want;
}

// window titles come from ANSI APIs too: UTF-8 if valid, else Windows-1252 (stringWithUTF8String: gives nil)
static NSString *title_str(const char *s) {
    if (!s) return @"";
    NSString *t = [NSString stringWithUTF8String:s];
    if (!t) t = [[NSString alloc] initWithBytes:s length:strlen(s) encoding:NSWindowsCP1252StringEncoding];
    return t ? t : @"";
}
void *cocoa_window_new(uint64_t hwnd, int x, int y, int cw, int ch, int titled, int resizable, int minimizable, const char *title) {
    __block void *res = NULL;
    NSString *t = title_str(title);
    on_main(^{
        app_init();
        NSWindowStyleMask sm = style_mask(titled, resizable, minimizable);
        RTWindow *w = [[RTWindow alloc] initWithContentRect:px_to_mac(x, y, cw, ch) styleMask:sm backing:NSBackingStoreBuffered defer:NO];
        w.hwnd = hwnd; w.delegate = w; w.releasedWhenClosed = NO; w.acceptsMouseMovedEvents = YES; w.title = t;
        w.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone;
        NSRect cr = px_to_mac(x, y, cw, ch);
        NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, cr.size.width, cr.size.height)];
        box.wantsLayer = YES; box.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        RTView *v = [[RTView alloc] initWithFrame:box.bounds];
        v.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        v.hwnd = hwnd; v.wantsLayer = YES; v.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
        [box addSubview:v]; w.contentView = box; w.view = v; [w makeFirstResponder:v];
        layout(w, x, y, cw, ch);
        g_active = 1;
        res = (__bridge_retained void *)w;
    });
    return res;
}
void cocoa_window_free(void *p) { on_main(^{ RTWindow *w = (__bridge_transfer RTWindow *)p; w.delegate = nil; [w orderOut:nil]; [w close]; update_presentation(w); }); }
void cocoa_window_show(void *p, int show, int activate) {
    on_main(^{
        RTWindow *w = (__bridge RTWindow *)p;
        if (!show) [w orderOut:nil];
        else if (activate) { [NSApp activateIgnoringOtherApps:YES]; [w makeKeyAndOrderFront:nil]; }
        else [w orderFront:nil];
        update_presentation(w);
    });
}
void cocoa_window_minimize(void *p, int m) { on_main(^{ RTWindow *w = (__bridge RTWindow *)p; if (m) [w miniaturize:nil]; else [w deminiaturize:nil]; }); }
void cocoa_window_zoom(void *p, int z) { on_main(^{ RTWindow *w = (__bridge RTWindow *)p; if (!!w.zoomed != !!z) [w zoom:nil]; }); }
void cocoa_window_state(void *p, int *mn, int *zm, int *vis) {
    __block int a = 0, b = 0, v = 0;
    on_main(^{ RTWindow *w = (__bridge RTWindow *)p; a = w.miniaturized; b = w.zoomed; v = w.visible; });
    *mn = a; *zm = b; *vis = v;
}
void cocoa_window_set_style(void *p, int titled, int resizable, int minimizable) {
    on_main(^{
        RTWindow *w = (__bridge RTWindow *)p;
        NSRect content = [w contentRectForFrameRect:w.frame];
        w.styleMask = style_mask(titled, resizable, minimizable);
        [w setFrame:[w frameRectForContentRect:content] display:YES];
        [w makeFirstResponder:w.view];
        update_presentation(w);
    });
}
// Place a window at a guest client rect. A borderless window at the screen origin that is at least as wide
// or as tall as the (possibly emulated) screen is a fullscreen window: it covers the real screen and the
// game view is letterboxed inside at the window's own aspect ratio, rendering at the window's own size.
static void layout(RTWindow *w, int x, int y, int cw, int ch) {
    NSArray *scr = [NSScreen screens];
    int sw = g_vw ? g_vw : (int)lround(primary_w() * g_scale), sh = g_vh ? g_vh : (int)lround(primary_h() * g_scale);
    BOOL fs = !(w.styleMask & NSWindowStyleMaskTitled) && x <= 0 && y <= 0 && (x + cw >= sw || y + ch >= sh) && cw > 0 && ch > 0 && scr.count;
    w.fullscreen = fs;
    if (fs) {
        NSRect sf = ((NSScreen *)scr[0]).frame;
        double k = fmin(sf.size.width / cw, sf.size.height / ch);   // points per guest pixel
        NSRect v = NSMakeRect((sf.size.width - cw * k) / 2, (sf.size.height - ch * k) / 2, cw * k, ch * k);
        w.fsx = x; w.fsy = y; w.fsw = cw; w.fsh = ch;
        [w setFrame:sf display:YES];
        w.view.autoresizingMask = NSViewNotSizable;
        w.view.frame = v;
        w.view.pxPerPt = 1.0 / k;
        w.view.layer.contentsScale = 1.0 / k;   // drawable = cw x ch
    } else {
        [w setFrame:[w frameRectForContentRect:px_to_mac(x, y, cw, ch)] display:YES];
        w.view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        w.view.frame = w.contentView.bounds;
        w.view.pxPerPt = 0;
        w.view.layer.contentsScale = px_per_pt();
    }
    update_presentation(w);
    NSRect f = w.frame, v = w.view.frame;
    rt_log("[cocoa] layout hwnd %llx guest %d,%d %dx%d (screen %dx%d%s) -> %s, window %.0f,%.0f %.0fx%.0f pt, view %.0f,%.0f %.0fx%.0f, scale %.3f, presentation %lx, active %d\n",
           (unsigned long long)w.hwnd, x, y, cw, ch, sw, sh, g_vw ? " emulated" : "", fs ? "FULLSCREEN" : "windowed",
           f.origin.x, f.origin.y, f.size.width, f.size.height, v.origin.x, v.origin.y, v.size.width, v.size.height,
           w.view.layer.contentsScale, (unsigned long)NSApp.presentationOptions, NSApp.active);
}
void cocoa_window_set_client(void *p, int x, int y, int cw, int ch) {
    on_main(^{ layout((__bridge RTWindow *)p, x, y, cw, ch); });
}
void cocoa_window_get_client(void *p, int *x, int *y, int *cw, int *ch) {
    __block NSRect r;
    __block int gx, gy, gw, gh;
    on_main(^{ guest_client((__bridge RTWindow *)p, &gx, &gy, &gw, &gh); });
    (void)r; *x = gx; *y = gy; *cw = gw; *ch = gh;
}
void cocoa_window_set_title(void *p, const char *title) {
    NSString *t = title_str(title);
    on_main(^{ ((__bridge RTWindow *)p).title = t; });
}
void cocoa_window_activate(void *p) { on_main(^{ [NSApp activateIgnoringOtherApps:YES]; [(__bridge RTWindow *)p makeKeyAndOrderFront:nil]; }); }
void *cocoa_window_layer(void *p) {
    __block void *l = NULL;
    on_main(^{ l = (__bridge void *)((__bridge RTWindow *)p).view.layer; });
    return l;
}
void cocoa_frame_insets(int titled, int resizable, int *l, int *t, int *r, int *b) {
    pthread_once(&scale_once, scale_init);
    NSRect c = NSMakeRect(0, 0, 1000, 1000);
    NSRect f = [NSWindow frameRectForContentRect:c styleMask:style_mask(titled, resizable, 1)];   // class method: no app needed
    double s = px_per_pt();
    *l = (int)lround((c.origin.x - f.origin.x) * s);
    *b = (int)lround((c.origin.y - f.origin.y) * s);
    *r = (int)lround((NSMaxX(f) - NSMaxX(c)) * s);
    *t = (int)lround((NSMaxY(f) - NSMaxY(c)) * s);
}

// ---------------- events ----------------
void cocoa_pump(int wait_ms) {
    if (![NSThread isMainThread]) return;
    if (!g_active) {   // no app yet: still run the main queue (window creation from another thread waits on it)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, wait_ms > 0 ? wait_ms / 1000.0 : 0, true);
        return;
    }
    @autoreleasepool {
        NSDate *until = wait_ms > 0 ? [NSDate dateWithTimeIntervalSinceNow:wait_ms / 1000.0] : [NSDate distantPast];
        NSEvent *e;
        while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES])) {
            [NSApp sendEvent:e];
            until = [NSDate distantPast];
        }
        [NSApp updateWindows];
    }
}

// ---------------- cursor ----------------
void cocoa_cursor_pos(int *x, int *y) {
    pthread_once(&scale_once, scale_init);
    NSPoint p = [NSEvent mouseLocation];
    double k, ox, oy; xform(&k, &ox, &oy);
    *x = (int)lround((p.x - ox) / k); *y = (int)lround((primary_h() - p.y - oy) / k);
}
void cocoa_set_cursor_pos(int x, int y) {
    pthread_once(&scale_once, scale_init);
    double k, ox, oy; xform(&k, &ox, &oy);
    CGWarpMouseCursorPosition(CGPointMake(ox + x * k, oy + y * k));
    CGAssociateMouseAndMouseCursorPosition(true);   // no input freeze after the warp
}
void cocoa_cursor_visible(int v) { on_main(^{ if (v) [NSCursor unhide]; else [NSCursor hide]; }); }
// Custom cursors (CreateIconIndirect): an NSCursor from a 32-bit BGRA image (straight alpha, top row first),
// sized so one cursor pixel covers one guest pixel. SetCursor shows it; SetCursor(NULL) shows no image.
static NSCursor *blank_cursor(void) {
    static NSCursor *b;
    if (!b) { NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(1, 1)]; b = [[NSCursor alloc] initWithImage:img hotSpot:NSZeroPoint]; }
    return b;
}
void *cocoa_cursor_new(const uint8_t *bgra, int w, int h, int hx, int hy) {
    __block void *res = NULL;
    on_main(^{
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:h bitsPerSample:8
            samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
            bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:w * 4 bitsPerPixel:32];
        if (!rep) return;
        uint8_t *d = rep.bitmapData;
        for (int i = 0; i < w * h; i++) { d[4 * i] = bgra[4 * i + 2]; d[4 * i + 1] = bgra[4 * i + 1]; d[4 * i + 2] = bgra[4 * i]; d[4 * i + 3] = bgra[4 * i + 3]; }
        double s = px_per_pt();   // guest pixels per point
        NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(w / s, h / s)];
        [img addRepresentation:rep];
        NSCursor *cur = [[NSCursor alloc] initWithImage:img hotSpot:NSMakePoint(hx / s, hy / s)];
        res = (__bridge_retained void *)cur;
    });
    return res;
}
void cocoa_cursor_free(void *c) {
    if (!c) return;
    on_main(^{
        NSCursor *x = (__bridge_transfer NSCursor *)c;
        if (g_cur == x) { g_cur = nil; [[NSCursor arrowCursor] set]; }
    });
}
void cocoa_cursor_set(void *c, int blank) {
    on_main(^{
        g_cur = blank ? blank_cursor() : (__bridge NSCursor *)c;
        for (NSWindow *nw in NSApp.windows)   // re-register every game view's cursor rect with the new cursor
            if ([nw isKindOfClass:[RTWindow class]]) [nw invalidateCursorRectsForView:((RTWindow *)nw).view];
        [(g_cur ?: [NSCursor arrowCursor]) set];
    });
}
void cocoa_mouse_delta(int *dx, int *dy) { *dx = atomic_exchange(&g_dx, 0); *dy = atomic_exchange(&g_dy, 0); }

// Emulated display mode for the primary screen (ChangeDisplaySettings); 0 x 0 restores the native mode.
// Windows keep their guest rects; window.c re-applies them afterwards.
void cocoa_set_virtual_screen(int w, int h) {
    on_main(^{
        app_init();
        g_vw = w; g_vh = h;
        for (NSWindow *nw in NSApp.windows)
            if ([nw isKindOfClass:[RTWindow class]]) ((RTWindow *)nw).view.layer.contentsScale = px_per_pt();
    });
}

// ---------------- app launch helpers (the app bundle) ----------------
// Ask the user for the game folder (the one containing the game's executable). Returns a malloc'd path or NULL.
char *cocoa_choose_folder(const char *message) {
    __block char *res = NULL;
    NSString *msg = [NSString stringWithUTF8String:message];
    on_main(^{
        app_init();
        [NSApp activateIgnoringOtherApps:YES];
        NSOpenPanel *p = [NSOpenPanel openPanel];
        p.canChooseDirectories = YES; p.canChooseFiles = NO; p.allowsMultipleSelection = NO;
        p.message = msg; p.prompt = @"Choose";
        if ([p runModal] == NSModalResponseOK && p.URL.path) res = strdup(p.URL.path.fileSystemRepresentation);
    });
    return res;
}
void cocoa_alert(const char *title, const char *text) {
    NSString *t = [NSString stringWithUTF8String:title], *x = [NSString stringWithUTF8String:text];
    on_main(^{
        app_init();
        [NSApp activateIgnoringOtherApps:YES];
        NSAlert *a = [NSAlert new];
        a.messageText = t; a.informativeText = x; a.alertStyle = NSAlertStyleCritical;
        [a runModal];
    });
}

// Steam's in-game overlay isn't available to the Mac port, so "Invite friends" opens this instead: a list of the
// online friends with checkboxes. Runs on the main thread without blocking the caller; done() gets the chosen
// indices (on the main thread) and frees nothing (the caller's arrays are copied).
void cocoa_pick_friends(int n, const char *const *names, const char *const *details,
                        void (*done)(const int *picked, int npicked, void *ctx), void *ctx) {
    NSMutableArray<NSString *> *nm = [NSMutableArray array], *dt = [NSMutableArray array];
    for (int i = 0; i < n; i++) {
        [nm addObject:[NSString stringWithUTF8String:names[i] ?: "?"]];
        [dt addObject:[NSString stringWithUTF8String:details[i] ?: ""]];
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        app_init();
        NSAlert *a = [NSAlert new];
        a.messageText = @"Invite Friends";
        if (!n) {
            a.informativeText = @"None of your Steam friends are online right now.";
            [a addButtonWithTitle:@"OK"];
            [a layout]; a.window.level = NSPopUpMenuWindowLevel;
            [NSApp activateIgnoringOtherApps:YES];
            [a runModal];
            done(NULL, 0, ctx);
            return;
        }
        a.informativeText = @"Choose who to invite to your lobby. They get the invite in Steam.";
        [a addButtonWithTitle:@"Invite"];
        [a addButtonWithTitle:@"Cancel"];
        NSMutableArray<NSButton *> *boxes = [NSMutableArray array];
        NSStackView *stack = [NSStackView new];
        stack.orientation = NSUserInterfaceLayoutOrientationVertical;
        stack.alignment = NSLayoutAttributeLeading; stack.spacing = 6;
        stack.edgeInsets = NSEdgeInsetsMake(4, 4, 4, 4);
        for (int i = 0; i < n; i++) {
            NSString *title = dt[i].length ? [NSString stringWithFormat:@"%@  —  %@", nm[i], dt[i]] : nm[i];
            NSButton *b = [NSButton checkboxWithTitle:title target:nil action:nil];
            [boxes addObject:b]; [stack addArrangedSubview:b];
        }
        CGFloat h = MIN(320, n * 24 + 8);
        NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 340, h)];
        sv.hasVerticalScroller = YES; sv.drawsBackground = NO; sv.borderType = NSNoBorder;
        NSView *doc = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 320, n * 24 + 8)];
        stack.frame = doc.bounds; stack.autoresizingMask = NSViewWidthSizable;
        [doc addSubview:stack];
        sv.documentView = doc;
        [doc scrollPoint:NSMakePoint(0, NSMaxY(doc.bounds))];
        a.accessoryView = sv;
        [a layout];
        a.window.level = NSPopUpMenuWindowLevel;   // above the game window, also in fullscreen
        [NSApp activateIgnoringOtherApps:YES];
        NSModalResponse r = [a runModal];
        int *picked = calloc((size_t)n, sizeof *picked), np = 0;
        if (r == NSAlertFirstButtonReturn)
            for (int i = 0; i < n; i++) if (boxes[i].state == NSControlStateValueOn) picked[np++] = i;
        done(picked, np, ctx);
        free(picked);
    });
}

// the display's refresh rate (ProMotion screens report their maximum, e.g. 120)
double cocoa_refresh_hz(void) {
    __block double hz = 60;
    on_main(^{ NSScreen *s = NSApp.keyWindow.screen ?: NSScreen.mainScreen; if (s.maximumFramesPerSecond > 0) hz = (double)s.maximumFramesPerSecond; });
    return hz;
}

// debugging: the CAMetalLayer's real state (drivers may change it when creating a swapchain)
void cocoa_log_layer(void *p) {
    on_main(^{
        CAMetalLayer *l = (__bridge CAMetalLayer *)p;
        rt_log("[cocoa] layer bounds %.0fx%.0f pt, contentsScale %.3f, drawableSize %.0fx%.0f, gravity %s, frame %.0f,%.0f, superlayer %p\n",
               l.bounds.size.width, l.bounds.size.height, l.contentsScale, l.drawableSize.width, l.drawableSize.height,
               l.contentsGravity.UTF8String, l.frame.origin.x, l.frame.origin.y, (__bridge void *)l.superlayer);
    });
}

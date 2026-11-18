// The macOS host for win32 guests: AppKit owns the main thread, the guest's WinMain runs on a game thread.
// Windows the guest creates become NSWindows whose content view is backed by a CAMetalLayer (presented by the
// D3D9 layer). Keyboard and mouse events become WM_* messages and the key state GetAsyncKeyState reads.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include "rt32.h"

void rt_post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
uint8_t rt_keys[256];                 // VK -> down
int rt_mouse_x, rt_mouse_y;
static id<MTLDevice> device;

id<MTLDevice> rt_metal_device(void) {
    if (!device) device = MTLCreateSystemDefaultDevice();
    return device;
}

// macOS virtual key code -> Windows VK
static uint8_t vk_of(unsigned short k) {
    static const uint8_t map[128] = {
        [0x00] = 'A', [0x0B] = 'B', [0x08] = 'C', [0x02] = 'D', [0x0E] = 'E', [0x03] = 'F', [0x05] = 'G', [0x04] = 'H',
        [0x22] = 'I', [0x26] = 'J', [0x28] = 'K', [0x25] = 'L', [0x2E] = 'M', [0x2D] = 'N', [0x1F] = 'O', [0x23] = 'P',
        [0x0C] = 'Q', [0x0F] = 'R', [0x01] = 'S', [0x11] = 'T', [0x20] = 'U', [0x09] = 'V', [0x0D] = 'W', [0x07] = 'X',
        [0x10] = 'Y', [0x06] = 'Z',
        [0x1D] = '0', [0x12] = '1', [0x13] = '2', [0x14] = '3', [0x15] = '4', [0x17] = '5', [0x16] = '6', [0x1A] = '7',
        [0x1C] = '8', [0x19] = '9',
        [0x24] = 0x0D, [0x35] = 0x1B, [0x31] = 0x20, [0x30] = 0x09, [0x33] = 0x08, [0x75] = 0x2E,
        [0x7B] = 0x25, [0x7E] = 0x26, [0x7C] = 0x27, [0x7D] = 0x28,
        [0x38] = 0x10, [0x3C] = 0x10, [0x3B] = 0x11, [0x3E] = 0x11, [0x3A] = 0x12, [0x3D] = 0x12,
        [0x7A] = 0x70, [0x78] = 0x71, [0x63] = 0x72, [0x76] = 0x73, [0x60] = 0x74, [0x61] = 0x75, [0x62] = 0x76,
        [0x64] = 0x77, [0x65] = 0x78, [0x6D] = 0x79, [0x67] = 0x7A, [0x6F] = 0x7B,
        [0x74] = 0x21, [0x79] = 0x22, [0x73] = 0x24, [0x77] = 0x23,
    };
    return k < 128 ? map[k] : 0;
}

// macOS virtual key code -> DirectInput scan code (DIK_*)
static uint8_t dik_of(unsigned short k) {
    static const uint8_t map[128] = {
        [0x35] = 0x01, [0x12] = 0x02, [0x13] = 0x03, [0x14] = 0x04, [0x15] = 0x05, [0x17] = 0x06, [0x16] = 0x07, [0x1A] = 0x08,
        [0x1C] = 0x09, [0x19] = 0x0A, [0x1D] = 0x0B, [0x1B] = 0x0C, [0x18] = 0x0D, [0x33] = 0x0E, [0x30] = 0x0F,
        [0x0C] = 0x10, [0x0D] = 0x11, [0x0E] = 0x12, [0x0F] = 0x13, [0x11] = 0x14, [0x10] = 0x15, [0x20] = 0x16, [0x22] = 0x17,
        [0x1F] = 0x18, [0x23] = 0x19, [0x21] = 0x1A, [0x1E] = 0x1B, [0x24] = 0x1C, [0x3B] = 0x1D,
        [0x00] = 0x1E, [0x01] = 0x1F, [0x02] = 0x20, [0x03] = 0x21, [0x05] = 0x22, [0x04] = 0x23, [0x26] = 0x24, [0x28] = 0x25,
        [0x25] = 0x26, [0x29] = 0x27, [0x27] = 0x28, [0x32] = 0x29, [0x38] = 0x2A, [0x2A] = 0x2B,
        [0x06] = 0x2C, [0x07] = 0x2D, [0x08] = 0x2E, [0x09] = 0x2F, [0x0B] = 0x30, [0x2D] = 0x31, [0x2E] = 0x32,
        [0x2B] = 0x33, [0x2F] = 0x34, [0x2C] = 0x35, [0x3C] = 0x36, [0x43] = 0x37, [0x3A] = 0x38, [0x31] = 0x39, [0x39] = 0x3A,
        [0x7A] = 0x3B, [0x78] = 0x3C, [0x63] = 0x3D, [0x76] = 0x3E, [0x60] = 0x3F, [0x61] = 0x40, [0x62] = 0x41, [0x64] = 0x42,
        [0x65] = 0x43, [0x6D] = 0x44, [0x67] = 0x57, [0x6F] = 0x58,
        [0x3E] = 0x9D, [0x3D] = 0xB8, [0x73] = 0xC7, [0x7E] = 0xC8, [0x74] = 0xC9, [0x7B] = 0xCB, [0x7C] = 0xCD, [0x77] = 0xCF,
        [0x7D] = 0xD0, [0x79] = 0xD1, [0x72] = 0xD2, [0x75] = 0xD3,
        [0x52] = 0x52, [0x53] = 0x4F, [0x54] = 0x50, [0x55] = 0x51, [0x56] = 0x4B, [0x57] = 0x4C, [0x58] = 0x4D, [0x59] = 0x47,
        [0x5B] = 0x48, [0x5C] = 0x49, [0x4C] = 0x9C, [0x45] = 0x4E, [0x4E] = 0x4A,
    };
    return k < 128 ? map[k] : 0;
}
void rt_dinput_key(uint8_t dik, int down);   // dinput8.c
int rt_mouse_dx, rt_mouse_dy, rt_mouse_dz;

@interface RtView : NSView
@property uint32_t hwnd;
@end
@implementation RtView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer {
    CAMetalLayer *l = [CAMetalLayer layer];
    l.device = rt_metal_device();
    l.pixelFormat = MTLPixelFormatBGRA8Unorm;
    l.framebufferOnly = NO;
    l.maximumDrawableCount = 3;
    return l;
}
- (void)key:(NSEvent *)e down:(int)down {
    uint8_t dik = dik_of(e.keyCode);
    if (dik) rt_dinput_key(dik, down);
    uint8_t vk = vk_of(e.keyCode);
    if (!vk) return;
    int repeat = down && rt_keys[vk];
    rt_keys[vk] = (uint8_t)down;
    uint32_t lp = 1u | ((uint32_t)e.keyCode << 16) | (repeat ? 1u << 30 : 0) | (down ? 0 : 3u << 30);
    rt_post_message(self.hwnd, down ? 0x0100 : 0x0101, vk, lp);   // WM_KEYDOWN / WM_KEYUP
}
- (void)keyDown:(NSEvent *)e { [self key:e down:1]; }
- (void)keyUp:(NSEvent *)e { [self key:e down:0]; }
- (void)flagsChanged:(NSEvent *)e {
    uint8_t vk = vk_of(e.keyCode); if (!vk) return;
    NSEventModifierFlags f = e.modifierFlags;
    int down = vk == 0x10 ? !!(f & NSEventModifierFlagShift) : vk == 0x11 ? !!(f & NSEventModifierFlagControl) : !!(f & NSEventModifierFlagOption);
    [self key:e down:down];
}
- (void)mouse:(NSEvent *)e msg:(uint32_t)msg {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    NSSize s = self.bounds.size;
    CAMetalLayer *l = (CAMetalLayer *)self.layer;
    // client coordinates in the game's back-buffer pixels
    rt_mouse_x = (int)(p.x / s.width * l.drawableSize.width);
    rt_mouse_y = (int)((s.height - p.y) / s.height * l.drawableSize.height);
    rt_post_message(self.hwnd, msg, 0, (uint32_t)(rt_mouse_x & 0xFFFF) | ((uint32_t)rt_mouse_y << 16));
}
- (void)mouseMoved:(NSEvent *)e { __atomic_add_fetch(&rt_mouse_dx, (int)e.deltaX, __ATOMIC_RELAXED); __atomic_add_fetch(&rt_mouse_dy, (int)e.deltaY, __ATOMIC_RELAXED); [self mouse:e msg:0x0200]; }
- (void)mouseDragged:(NSEvent *)e { [self mouseMoved:e]; }
- (void)scrollWheel:(NSEvent *)e { __atomic_add_fetch(&rt_mouse_dz, (int)(e.scrollingDeltaY * 40), __ATOMIC_RELAXED); }
- (void)mouseDown:(NSEvent *)e { rt_keys[1] = 1; [self mouse:e msg:0x0201]; }
- (void)mouseUp:(NSEvent *)e { rt_keys[1] = 0; [self mouse:e msg:0x0202]; }
- (void)rightMouseDown:(NSEvent *)e { rt_keys[2] = 1; [self mouse:e msg:0x0204]; }
- (void)rightMouseUp:(NSEvent *)e { rt_keys[2] = 0; [self mouse:e msg:0x0205]; }
@end

@interface RtWindowDelegate : NSObject <NSWindowDelegate>
@property uint32_t hwnd;
@end
@implementation RtWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)w { rt_post_message(self.hwnd, 0x0010, 0, 0); return NO; }   // WM_CLOSE
- (void)windowDidBecomeKey:(NSNotification *)n { rt_post_message(self.hwnd, 0x0006, 1, 0); }        // WM_ACTIVATE
- (void)windowDidResignKey:(NSNotification *)n { memset(rt_keys, 0, sizeof rt_keys); rt_post_message(self.hwnd, 0x0006, 0, 0); }
@end

static NSMutableDictionary<NSNumber *, NSWindow *> *windows;

// called from the game thread; returns the window's CAMetalLayer (retained)
void *rt_cocoa_create_window(uint32_t hwnd, const char *title, int w, int h) {
    __block CAMetalLayer *layer = nil;
    NSString *t = [NSString stringWithUTF8String:title ? title : ""] ?: @"";
    dispatch_sync(dispatch_get_main_queue(), ^{
        if (!windows) windows = [NSMutableDictionary new];
        NSRect r = NSMakeRect(0, 0, w, h);
        NSWindow *win = [[NSWindow alloc] initWithContentRect:r styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                              NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                      backing:NSBackingStoreBuffered defer:NO];
        win.title = t;
        win.releasedWhenClosed = NO;
        RtView *v = [[RtView alloc] initWithFrame:r];
        v.hwnd = hwnd; v.wantsLayer = YES;
        win.contentView = v;
        RtWindowDelegate *d = [RtWindowDelegate new]; d.hwnd = hwnd;
        win.delegate = d;
        objc_setAssociatedObject(win, "rtdelegate", d, OBJC_ASSOCIATION_RETAIN);
        win.acceptsMouseMovedEvents = YES;
        [win center];
        [win makeKeyAndOrderFront:nil];
        [win makeFirstResponder:v];
        [NSApp activateIgnoringOtherApps:YES];
        windows[@(hwnd)] = win;
        layer = (CAMetalLayer *)v.layer;
        layer.drawableSize = CGSizeMake(w, h);
    });
    return (__bridge_retained void *)layer;
}

void rt_cocoa_set_size(uint32_t hwnd, int w, int h) {
    dispatch_async(dispatch_get_main_queue(), ^{
        NSWindow *win = windows[@(hwnd)];
        if (!win) return;
        [win setContentSize:NSMakeSize(w, h)];
        ((CAMetalLayer *)win.contentView.layer).drawableSize = CGSizeMake(w, h);
    });
}

void rt_cocoa_set_title(uint32_t hwnd, const char *title) {
    NSString *t = [NSString stringWithUTF8String:title] ?: @"";
    dispatch_async(dispatch_get_main_queue(), ^{ windows[@(hwnd)].title = t; });
}

void rt_cocoa_screen_size(int *w, int *h) {
    __block NSSize s;
    if ([NSThread isMainThread]) s = NSScreen.mainScreen.frame.size;
    else dispatch_sync(dispatch_get_main_queue(), ^{ s = NSScreen.mainScreen.frame.size; });
    *w = (int)s.width; *h = (int)s.height;
}

// ---- app ----
static void (*game_main)(void);
static void *game_thread(void *p) { (void)p; game_main(); return NULL; }

void rt_cocoa_run(void (*fn)(void)) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSMenu *bar = [NSMenu new], *app = [NSMenu new];
        NSMenuItem *item = [NSMenuItem new]; item.submenu = app; [bar addItem:item];
        [app addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
        NSApp.mainMenu = bar;
        [NSApp finishLaunching];
        game_main = fn;
        pthread_t t; pthread_attr_t a; pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 256u << 20);   // translated frames are large; the guest recurses deeply at load
        pthread_create(&t, &a, game_thread, NULL);
        [NSApp run];
    }
}

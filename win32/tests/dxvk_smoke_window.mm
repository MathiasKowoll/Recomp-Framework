// AppKit half of the DXVK smoke test (kept apart from windows.h: ObjC and Win32 both define BOOL)
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
static NSWindow *g_win; static CAMetalLayer *g_layer;
extern "C" void *smoke_window(int w, int h) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    g_win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, w, h) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    g_layer = [CAMetalLayer layer];
    g_win.contentView.wantsLayer = YES;
    g_win.contentView.layer = g_layer;
    g_layer.drawableSize = CGSizeMake(w, h);
    [g_win makeKeyAndOrderFront:nil];
    return (__bridge void *)g_win;
}
extern "C" void *smoke_layer(void) { return (__bridge void *)g_layer; }
extern "C" void smoke_pump(void) {
    NSEvent *ev; while ((ev = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES])) [NSApp sendEvent:ev];
}

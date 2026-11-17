// mac32: the native macOS host for a recompiled 32-bit Mac game. Replaces the original app's Cocoa glue (window,
// OpenGL view, event loop) and drives the engine through the game32_* hooks (game32.h).
//   App bundle: finds the user's copy of the original game (remembered choice, Steam, else asks), logs to
//               ~/Library/Logs/<log_name>.log.
//   Development: <binary> ORIG_BINARY RESOURCES_DIR
// Testing: RT32_SHOT=frame[,frame...] writes shot_<frame>.png (to RT32_SHOT_DIR); RT32_EXIT_AFTER=n quits after n
// frames; RT32_INPUT="frame:move:x,y;frame:click;frame:down;frame:up;frame:key:c" scripts input (the real mouse and
// keyboard are then ignored).
#define GL_SILENCE_DEPRECATION
#import <Cocoa/Cocoa.h>
#import <OpenGL/gl.h>
#import <OpenGL/glext.h>
#include "rt.h"
#include "tables.h"
#include "game32.h"

void steam_set_app_id(uint32_t id);
extern float gl_win_scale, gl_win_off_x, gl_win_off_y;   // gl.c: engine viewport -> drawable mapping

static CPU *gcpu;
static char resources_dir[2048];
const char *host_resources_dir(void) { return resources_dir; }
void host_present(void) { [[NSOpenGLContext currentContext] flushBuffer]; }

static BOOL game_wants_system_cursor;   // set by the game through host_show_cursor
static NSView *game_view;
static NSCursor *blank_cursor(void) {
    static NSCursor *c;
    if (!c) {
        NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(16, 16)];
        [img lockFocus]; [[NSColor clearColor] set]; NSRectFill(NSMakeRect(0, 0, 16, 16)); [img unlockFocus];
        c = [[NSCursor alloc] initWithImage:img hotSpot:NSZeroPoint];
    }
    return c;
}
static BOOL scripted(void) { return getenv("RT32_INPUT") != NULL; }
void host_show_cursor(int show) {
    if (game_wants_system_cursor == (BOOL)show) return;
    game_wants_system_cursor = (BOOL)show;
    dispatch_async(dispatch_get_main_queue(), ^{ [game_view.window invalidateCursorRectsForView:game_view]; });
}

@interface RT32View : NSOpenGLView
@property (strong) NSTimer *timer;
@end

@implementation RT32View
- (BOOL)acceptsFirstResponder { return YES; }

- (void)prepareOpenGL {
    [super prepareOpenGL];
    GLint swap = 1;
    [[self openGLContext] setValues:&swap forParameter:NSOpenGLContextParameterSwapInterval];
    [[self openGLContext] makeCurrentContext];
    fprintf(stderr, "[host] GL %s / %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    if (!game32_start(gcpu)) { fprintf(stderr, "[host] the game failed to start\n"); [NSApp terminate:nil]; return; }
    float w = 0, h = 0; game32_window_size(gcpu, &w, &h);
    fprintf(stderr, "[host] engine started, window %gx%g\n", w, h);
    if (w > 0 && h > 0) [self fitWindowToEngineWidth:w height:h];
    [[self window] center];
    self.timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60.0 target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    [[NSRunLoop currentRunLoop] addTimer:self.timer forMode:NSEventTrackingRunLoopMode];
}

- (void)tick:(NSTimer *)t { [self setNeedsDisplay:YES]; }

// Map the engine's resolution onto the real drawable (uniform scale, centered) and clear the bars.
- (void)updateSurfaceMapping {
    float ew = 0, eh = 0; game32_window_size(gcpu, &ew, &eh);
    if (ew < 1 || eh < 1) return;
    NSSize d = [self convertSizeToBacking:self.bounds.size];
    float s = (float)MIN(d.width / ew, d.height / eh);
    gl_win_scale = s;
    gl_win_off_x = (float)((d.width - ew * s) / 2); gl_win_off_y = (float)((d.height - eh * s) / 2);
    // clear the bars without disturbing GL state the engine has cached
    GLint fb = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &fb);
    GLint vp[4]; glGetIntegerv(GL_VIEWPORT, vp);
    GLfloat cc[4]; glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST), cm[4]; glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    if (fb) glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    glDisable(GL_SCISSOR_TEST); glColorMask(1, 1, 1, 1);
    glViewport(0, 0, (GLsizei)d.width, (GLsizei)d.height);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(cc[0], cc[1], cc[2], cc[3]); glColorMask(cm[0], cm[1], cm[2], cm[3]);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    if (sc) glEnable(GL_SCISSOR_TEST);
    if (fb) glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, (GLuint)fb);
}

// Follow the game's own fullscreen setting (only when it changes, so the green button keeps working).
- (void)followEngineFullscreen {
    static int last = -1;
    int windowed = game32_windowed(gcpu);
    if (windowed == last) return;
    BOOL isFull = (self.window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    if (last != -1 && windowed == isFull) [self.window toggleFullScreen:nil];
    else if (last == -1 && !windowed && !isFull) [self.window toggleFullScreen:nil];
    last = windowed;
}

// The engine's resolution is in backing pixels: on a Retina display the window is half that in points.
// If it still does not fit the screen, scale it down (keeping aspect): the engine's viewport is mapped onto the
// smaller surface.
- (void)fitWindowToEngineWidth:(float)w height:(float)h {
    NSWindow *win = [self window];
    win.contentAspectRatio = NSMakeSize(w, h);
    if (win.styleMask & NSWindowStyleMaskFullScreen) return;   // fullscreen: the mapping handles it
    CGFloat scale = win.backingScaleFactor > 0 ? win.backingScaleFactor : 1;
    NSSize pts = NSMakeSize(w / scale, h / scale);
    NSRect vis = (win.screen ?: [NSScreen mainScreen]).visibleFrame;
    NSRect frame = [win frameRectForContentRect:NSMakeRect(0, 0, pts.width, pts.height)];
    CGFloat chrome = frame.size.height - pts.height;
    CGFloat k = MIN(1.0, MIN(vis.size.width / pts.width, (vis.size.height - chrome) / pts.height));
    pts.width *= k; pts.height *= k;
    [win setContentSize:pts];
    if (k < 1) fprintf(stderr, "[host] %gx%g does not fit the screen; window scaled by %.2f\n", w, h, k);
    [win center];
    [[self openGLContext] update];
    [self updateTrackingAreas];
    [win invalidateCursorRectsForView:self];
}

// ---- mouse ----
- (void)moveTo:(NSEvent *)e {
    if (scripted()) return;
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    NSPoint px = [self convertPointToBacking:p];   // engine coordinates are backing pixels
    NSSize sz = [self convertSizeToBacking:self.bounds.size];
    game32_mouse_move(gcpu, (float)((px.x - gl_win_off_x) / gl_win_scale), (float)((sz.height - px.y - gl_win_off_y) / gl_win_scale));
}
- (void)mouseMoved:(NSEvent *)e { [self moveTo:e]; }
- (void)mouseDragged:(NSEvent *)e { [self moveTo:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self moveTo:e]; }
- (void)otherMouseDragged:(NSEvent *)e { [self moveTo:e]; }
- (void)button:(int)b down:(BOOL)down event:(NSEvent *)e {
    if (scripted()) return;
    [self moveTo:e];
    game32_mouse_button(gcpu, b, down, (int)[e clickCount]);
}
- (void)mouseDown:(NSEvent *)e { [self button:0 down:YES event:e]; }
- (void)mouseUp:(NSEvent *)e { [self button:0 down:NO event:e]; }
- (void)rightMouseDown:(NSEvent *)e { [self button:1 down:YES event:e]; }
- (void)rightMouseUp:(NSEvent *)e { [self button:1 down:NO event:e]; }
- (void)otherMouseDown:(NSEvent *)e { [self button:2 down:YES event:e]; }
- (void)otherMouseUp:(NSEvent *)e { [self button:2 down:NO event:e]; }
- (void)scrollWheel:(NSEvent *)e {
    if (scripted()) return;
    float d = (float)[e deltaY];
    if ((int)d) game32_wheel(gcpu, (int)d);
}
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea *t in self.trackingAreas) [self removeTrackingArea:t];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:self.bounds
        options:NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
        owner:self userInfo:nil]];
}
// Games of this era draw their own cursor: over the game view the macOS pointer is invisible, unless the game
// asked for the system cursor (host_show_cursor). macOS restores the pointer outside the view.
- (void)resetCursorRects {
    [self addCursorRect:self.bounds cursor:game_wants_system_cursor ? [NSCursor arrowCursor] : blank_cursor()];
}

// ---- keyboard ----
- (uint32_t)keyChar:(NSEvent *)e {
    NSString *s = [e charactersIgnoringModifiers];
    return s.length ? (uint32_t)[s characterAtIndex:0] : 0;
}
- (void)keyDown:(NSEvent *)e {
    if (scripted() || [e isARepeat]) return;
    game32_key(gcpu, 1, [self keyChar:e], (uint32_t)[e modifierFlags]);
}
- (void)keyUp:(NSEvent *)e { if (scripted()) return; game32_key(gcpu, 0, [self keyChar:e], (uint32_t)[e modifierFlags]); }
- (void)flagsChanged:(NSEvent *)e {
    if (scripted()) return;
    static BOOL was[3];
    NSEventModifierFlags f = [e modifierFlags];
    const NSEventModifierFlags mask[3] = { NSEventModifierFlagShift, NSEventModifierFlagControl, NSEventModifierFlagOption };
    for (int i = 0; i < 3; i++) {
        BOOL now = (f & mask[i]) != 0;
        if (now != was[i]) { game32_modifier(gcpu, i, now); was[i] = now; }
    }
}

- (void)drawRect:(NSRect)r {
    [[self openGLContext] makeCurrentContext];
    if (game32_quit_requested(gcpu)) { [NSApp terminate:nil]; return; }
    {   // follow resolution changes made in the game's settings
        float w = 0, h = 0; game32_window_size(gcpu, &w, &h);
        static float lw, lh;
        if (w >= 320 && h >= 200 && (w != lw || h != lh)) {
            lw = w; lh = h;
            fprintf(stderr, "[host] engine resolution -> %gx%g\n", w, h);
            [self fitWindowToEngineWidth:w height:h];
        }
    }
    [self updateSurfaceMapping];
    [self followEngineFullscreen];
    game32_frame(gcpu);
    if (game32_quit_requested(gcpu)) [NSApp terminate:nil];
    [self afterFrame];
}

- (void)scriptedInput:(int)frame {
    const char *spec = getenv("RT32_INPUT");
    if (!spec) return;
    char buf[1024]; snprintf(buf, sizeof buf, "%s", spec);
    for (char *save = NULL, *t = strtok_r(buf, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
        int f = atoi(t); char *act = strchr(t, ':');
        if (f != frame || !act) continue;
        act++;
        if (!strncmp(act, "move:", 5)) { float x = 0, y = 0; sscanf(act + 5, "%f,%f", &x, &y); game32_mouse_move(gcpu, x, y); }
        else if (!strcmp(act, "down")) game32_mouse_button(gcpu, 0, 1, 1);
        else if (!strcmp(act, "up")) game32_mouse_button(gcpu, 0, 0, 1);
        else if (!strcmp(act, "click")) { game32_mouse_button(gcpu, 0, 1, 1); game32_mouse_button(gcpu, 0, 0, 1); }
        else if (!strncmp(act, "key:", 4)) {
            uint32_t k = (uint32_t)(act[4] == '#' ? strtoul(act + 5, NULL, 0) : (uint8_t)act[4]);
            game32_key(gcpu, 1, k, 0); game32_key(gcpu, 0, k, 0);
        }
        fprintf(stderr, "[host] frame %d: input %s\n", frame, act);
    }
}

- (void)afterFrame {
    static int frame = 0; frame++;
    [self scriptedInput:frame];
    if (getenv("RT32_TRACE_GL")) fprintf(stderr, "[host] ---- end of frame %d ----\n", frame);
    const char *shots = getenv("RT32_SHOT");
    if (shots) {
        char key[32]; snprintf(key, sizeof key, ",%d,", frame);
        char list[512]; snprintf(list, sizeof list, ",%s,", shots);
        if (strstr(list, key)) {
            NSSize sz = [self convertSizeToBacking:self.bounds.size];
            int w = (int)sz.width, h = (int)sz.height;
            unsigned char *px = malloc((size_t)w * h * 4);
            glReadBuffer(GL_FRONT);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
            NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:h
                bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:w * 4 bitsPerPixel:32];
            for (int y = 0; y < h; y++) {   // flip and force opaque
                unsigned char *dst = rep.bitmapData + (size_t)y * w * 4, *src = px + (size_t)(h - 1 - y) * w * 4;
                memcpy(dst, src, (size_t)w * 4);
                for (int x = 0; x < w; x++) dst[4 * x + 3] = 255;
            }
            NSString *path = [NSString stringWithFormat:@"shot_%d.png", frame];
            NSString *dir = getenv("RT32_SHOT_DIR") ? @(getenv("RT32_SHOT_DIR")) : @".";
            [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[dir stringByAppendingPathComponent:path] atomically:YES];
            fprintf(stderr, "[host] wrote %s (%dx%d)\n", path.UTF8String, w, h);
            free(px);
        }
    }
    const char *ex = getenv("RT32_EXIT_AFTER");
    if (ex && frame >= atoi(ex)) { fprintf(stderr, "[host] exiting after %d frames\n", frame); exit(0); }
}
@end

@interface RT32AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property (strong) NSWindow *window;
@end
@implementation RT32AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    NSOpenGLPixelFormatAttribute attrs[] = {
        NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersionLegacy,
        NSOpenGLPFADoubleBuffer, NSOpenGLPFAAccelerated,
        NSOpenGLPFAColorSize, 24, NSOpenGLPFAAlphaSize, 8,
        NSOpenGLPFADepthSize, 24, NSOpenGLPFAStencilSize, 8, 0 };
    NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    NSRect frame = NSMakeRect(0, 0, rt32_game.width > 0 ? rt32_game.width : 1024, rt32_game.height > 0 ? rt32_game.height : 768);
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                                                backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @(rt32_game.name ? rt32_game.name : "Game");
    self.window.delegate = self;
    self.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    self.window.styleMask |= NSWindowStyleMaskResizable;
    RT32View *v = [[RT32View alloc] initWithFrame:frame pixelFormat:pf];
    v.wantsBestResolutionOpenGLSurface = YES;  // full-resolution GL surface; engine sizes are backing pixels
    self.window.contentView = v;
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:v];
    game_view = v;
    self.window.acceptsMouseMovedEvents = YES;
    [NSApp activateIgnoringOtherApps:YES];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { return YES; }
@end

// The user's copy of the original Mac game. Remembered once chosen.
static NSString *bundle_exe(NSString *app) {
    return [app stringByAppendingPathComponent:[@"Contents/MacOS" stringByAppendingPathComponent:@(rt32_game.exe_name)]];
}
static BOOL valid_game(NSString *p) { return [[NSFileManager defaultManager] fileExistsAtPath:bundle_exe(p)]; }
static NSString *find_game(void) {
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    NSString *saved = [ud stringForKey:@"GamePath"];
    if (saved && valid_game(saved)) return saved;
    if (rt32_game.steam_folder) {
        NSString *rel = [NSString stringWithFormat:@"Library/Application Support/Steam/steamapps/common/%s/%s",
                         rt32_game.steam_folder, rt32_game.app_bundle];
        NSString *steam = [NSHomeDirectory() stringByAppendingPathComponent:rel];
        if (valid_game(steam)) { [ud setObject:steam forKey:@"GamePath"]; return steam; }
    }
    [NSApplication sharedApplication];
    NSAlert *a = [NSAlert new];
    a.messageText = [NSString stringWithFormat:@"Locate %s", rt32_game.name];
    a.informativeText = [NSString stringWithFormat:@"Choose %s from your installation of %s.", rt32_game.app_bundle, rt32_game.name];
    [a runModal];
    NSOpenPanel *op = [NSOpenPanel openPanel];
    op.canChooseDirectories = NO; op.canChooseFiles = YES; op.treatsFilePackagesAsDirectories = NO;
    op.allowedFileTypes = @[ @"app" ];
    if ([op runModal] != NSModalResponseOK) return nil;
    NSString *p = op.URL.path;
    if (!valid_game(p)) {
        NSAlert *e = [NSAlert new]; e.messageText = [NSString stringWithFormat:@"That is not the Mac version of %s.", rt32_game.name];
        [e runModal]; return nil;
    }
    [ud setObject:p forKey:@"GamePath"];
    return p;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        const char *bin = argc > 2 && argv[1][0] != '-' ? argv[1] : NULL;
        const char *res = argc > 2 && argv[1][0] != '-' ? argv[2] : NULL;
        static char binbuf[2048];
        if (!bin || !res) {
            // Launched as an app: log to ~/Library/Logs and locate the user's copy of the game.
            if (!isatty(2)) {
                NSString *log = [NSHomeDirectory() stringByAppendingPathComponent:
                    [NSString stringWithFormat:@"Library/Logs/%s.log", rt32_game.log_name ? rt32_game.log_name : "recomp32"]];
                freopen(log.UTF8String, "w", stderr); freopen(log.UTF8String, "a", stdout);
                setvbuf(stdout, NULL, _IONBF, 0); setvbuf(stderr, NULL, _IONBF, 0);   // a freeze must not lose the last lines
            }
            NSString *game = find_game();
            if (!game) return 1;
            snprintf(binbuf, sizeof binbuf, "%s", bundle_exe(game).UTF8String);
            snprintf(resources_dir, sizeof resources_dir, "%s/Contents/Resources", game.UTF8String);
            bin = binbuf;
        } else snprintf(resources_dir, sizeof resources_dir, "%s", res);
        gcpu = rt_init(bin);
        if (chdir(resources_dir)) { perror(resources_dir); return 1; }
        // Steam games ship steam_appid.txt next to the executable
        char p[2048]; snprintf(p, sizeof p, "%s/../MacOS/steam_appid.txt", resources_dir);
        FILE *f = fopen(p, "r"); unsigned id = 0;
        if (f) { if (fscanf(f, "%u", &id) != 1) id = 0; fclose(f); }
        steam_set_app_id(id);
        for (int i = 0; i < ngmodinit; i++) { uint32_t a[4] = { 0, 0, 0, 0 }; guest_call(gcpu, gmodinit[i], 4, a); }

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        RT32AppDelegate *d = [RT32AppDelegate new];
        app.delegate = d;
        [app run];
    }
    return 0;
}

// RT_GPU_CAPTURE=seconds[:path]: write a Metal GPU trace (.gputrace, opens in Xcode) of one frame presented that
// many seconds after launch. Default path: ~/Library/Application Support/<data_name>/debug/frame.gputrace.
// Metal only allows captures when MTL_CAPTURE_ENABLED=1 is in the environment before it starts; this sets it,
// and MESA_KK_GPU_CAPTURE=1, which makes KosmicKrisp stub out timestamp queries (GPU timings read as zero).
#import <Metal/Metal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "game.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static double cap_at = -1;
static char cap_path[1024];
static int cap_state;   // 0 waiting, 1 capturing, 2 done
static uint64_t t_start;

__attribute__((constructor)) static void gpu_capture_init(void) {
    const char *e = getenv("RT_GPU_CAPTURE");
    if (!e || !*e) return;
    cap_at = atof(e);
    const char *colon = strchr(e, ':');
    if (colon && colon[1]) snprintf(cap_path, sizeof cap_path, "%s", colon + 1);
    else snprintf(cap_path, sizeof cap_path, "%s/Library/Application Support/%s/debug/frame.gputrace", getenv("HOME"), rt_game.data_name);
    setenv("MTL_CAPTURE_ENABLED", "1", 1);
    setenv("MESA_KK_GPU_CAPTURE", "1", 1);   // KosmicKrisp: no Metal 4 counter heaps (captures refuse them)
    setenv("MESA_KK_GPU_CAPTURE_IMMEDIATELY", "0", 1);
    t_start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

// called after every present
void rt_gpu_capture_frame(void) {
    if (cap_at < 0 || cap_state == 2) return;
    @autoreleasepool {
        MTLCaptureManager *m = [MTLCaptureManager sharedCaptureManager];
        if (cap_state == 1) {
            [m stopCapture];
            cap_state = 2;
            rt_log("[gpu] capture written: %s\n", cap_path);
            return;
        }
        if ((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t_start) / 1e9 < cap_at) return;
        if (![m supportsDestination:MTLCaptureDestinationGPUTraceDocument]) { rt_log("[gpu] capture to a file is not supported\n"); cap_state = 2; return; }
        MTLCaptureDescriptor *d = [MTLCaptureDescriptor new];
        d.captureObject = MTLCreateSystemDefaultDevice();
        d.destination = MTLCaptureDestinationGPUTraceDocument;
        [[NSFileManager defaultManager] removeItemAtPath:[NSString stringWithUTF8String:cap_path] error:nil];
        d.outputURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:cap_path]];
        NSError *err = nil;
        if (![m startCaptureWithDescriptor:d error:&err]) { rt_log("[gpu] capture failed: %s\n", err.localizedDescription.UTF8String); cap_state = 2; return; }
        cap_state = 1;
        rt_log("[gpu] capturing one frame\n");
    }
}

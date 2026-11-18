// Offline check of the D3D9 -> MSL shader translator: finds every D3D9 shader in a file (a game's shader package,
// or a dump directory's concatenation), translates it and compiles the result with Metal.
//
//   clang -fobjc-arc -O1 -I../runtime d3d9_shader_test.m ../runtime/d3d9_shader.c -framework Metal -framework Foundation -o d3d9_shader_test
//   ./d3d9_shader_test SHADERS.bin [--dump DIR] [--verbose]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "d3d9_internal.h"

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) { fprintf(stderr, "usage: %s FILE [--dump DIR] [--verbose]\n", argv[0]); return 2; }
        const char *dump = NULL; int verbose = 0;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
            else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        }
        NSData *data = [NSData dataWithContentsOfFile:@(argv[1])];
        if (!data) { fprintf(stderr, "can't read %s\n", argv[1]); return 1; }
        const uint8_t *b = data.bytes; size_t n = data.length;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        int found = 0, translated = 0, compiled = 0, seen_dup = 0;
        NSMutableSet *hashes = [NSMutableSet set];
        NSMutableDictionary<NSString *, NSNumber *> *errors = [NSMutableDictionary dictionary];
        for (size_t off = 0; off + 8 <= n; off++) {
            uint32_t v; memcpy(&v, b + off, 4);
            uint32_t hi = v >> 16, mj = (v >> 8) & 0xFF, mn = v & 0xFF;
            if (!((hi == 0xFFFE || hi == 0xFFFF) && (mj == 2 || mj == 3) && (mn == 0 || mn == 1))) continue;
            size_t max = (n - off) / 4;
            uint32_t *tok = malloc(max * 4 > (1u << 20) ? (1u << 20) : max * 4);
            size_t cap = max > (1u << 18) ? (1u << 18) : max;
            memcpy(tok, b + off, cap * 4);
            size_t len = d3d9_shader_length(tok, cap);
            if (!len) { free(tok); continue; }
            found++;
            ShaderInfo info;
            if (!d3d9_translate_shader(tok, len, &info)) {
                NSString *k = @(info.error); errors[k] = @(errors[k].intValue + 1);
                if (verbose) printf("0x%zx %s: translate: %s\n", off, hi == 0xFFFF ? "ps" : "vs", info.error);
                free(tok); off += len * 4 - 1; continue;
            }
            translated++;
            if ([hashes containsObject:@(info.hash)]) { seen_dup++; free(info.msl); free(tok); off += len * 4 - 1; continue; }
            [hashes addObject:@(info.hash)];
            char *src = info.stage ? d3d9_ps_source(&info) : d3d9_vs_source(&info, NULL, NULL);
            NSError *err = nil;
            MTLCompileOptions *opt = [MTLCompileOptions new];
            id<MTLLibrary> lib = [dev newLibraryWithSource:@(src) options:opt error:&err];
            if (lib && [lib newFunctionWithName:info.stage ? @"ps_main" : @"vs_main"]) compiled++;
            else {
                NSString *msg = err.localizedDescription ?: @"no function";
                NSRange r = [msg rangeOfString:@"error: "];
                NSString *k = r.location != NSNotFound ? [[msg substringFromIndex:r.location] componentsSeparatedByString:@"\n"][0] : msg;
                errors[k] = @(errors[k].intValue + 1);
                if (verbose) printf("0x%zx %s %08x: compile: %s\n", off, info.stage ? "ps" : "vs", info.hash, k.UTF8String);
                if (dump) {
                    NSString *p = [NSString stringWithFormat:@"%s/%s_%08x.metal", dump, info.stage ? "ps" : "vs", info.hash];
                    [@(src) writeToFile:p atomically:NO encoding:NSUTF8StringEncoding error:nil];
                }
            }
            free(src); free(info.msl); free(tok);
            off += len * 4 - 1;
        }
        printf("shaders found %d, translated %d (%d duplicates), unique compiled %d of %lu\n",
               found, translated, seen_dup, compiled, (unsigned long)hashes.count);
        NSArray *keys = [errors keysSortedByValueUsingSelector:@selector(compare:)];
        for (NSString *k in keys.reverseObjectEnumerator) printf("  %5d  %s\n", errors[k].intValue, k.UTF8String);
        return compiled == (int)hashes.count && errors.count == 0 ? 0 : 1;
    }
}

// Direct3D 9 on Metal: internal types shared by d3d9_metal.m (device, resources, presentation) and
// d3d9_shader.c (SM2/SM3 bytecode -> MSL).
#pragma once
#include "com32.h"

// ---- formats ----
enum {
    FMT_A8R8G8B8 = 21, FMT_X8R8G8B8 = 22, FMT_R5G6B5 = 23, FMT_X1R5G5B5 = 24, FMT_A1R5G5B5 = 25, FMT_A4R4G4B4 = 26,
    FMT_A8 = 28, FMT_A2B10G10R10 = 31, FMT_A8B8G8R8 = 32, FMT_X8B8G8R8 = 33, FMT_G16R16 = 34, FMT_A2R10G10B10 = 35,
    FMT_A16B16G16R16 = 36, FMT_L8 = 50, FMT_A8L8 = 51, FMT_V8U8 = 60, FMT_L16 = 81,
    FMT_D16_LOCKABLE = 70, FMT_D32 = 71, FMT_D15S1 = 73, FMT_D24S8 = 75, FMT_D24X8 = 77, FMT_D24X4S4 = 79, FMT_D16 = 80,
    FMT_D32F_LOCKABLE = 82, FMT_D24FS8 = 83,
    FMT_INDEX16 = 101, FMT_INDEX32 = 102,
    FMT_R16F = 111, FMT_G16R16F = 112, FMT_A16B16G16R16F = 113, FMT_R32F = 114, FMT_G32R32F = 115, FMT_A32B32G32R32F = 116,
};
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define FMT_DXT1 FOURCC('D', 'X', 'T', '1')
#define FMT_DXT2 FOURCC('D', 'X', 'T', '2')
#define FMT_DXT3 FOURCC('D', 'X', 'T', '3')
#define FMT_DXT4 FOURCC('D', 'X', 'T', '4')
#define FMT_DXT5 FOURCC('D', 'X', 'T', '5')
#define FMT_ATI1 FOURCC('A', 'T', 'I', '1')
#define FMT_ATI2 FOURCC('A', 'T', 'I', '2')
#define FMT_INTZ FOURCC('I', 'N', 'T', 'Z')
#define FMT_NULL FOURCC('N', 'U', 'L', 'L')
#define FMT_RAWZ FOURCC('R', 'A', 'W', 'Z')

typedef struct {
    uint32_t d3d;
    int mtl;            // MTLPixelFormat
    int bpp;            // bytes per pixel, or per 4x4 block when block
    int block;          // block-compressed
    int depth;          // depth (1) / depth+stencil (2)
    int swz;            // component swizzle needed: 1 = alpha forced to 1 (X8R8G8B8...), 2 = L8 (rrr1), 3 = A8L8 (rrrg)
    int convert;        // CPU conversion on upload: 1 = 16-bit 565/1555/4444 kept as-is (native packed formats)
} FmtInfo;
const FmtInfo *fmt_info(uint32_t d3d);   // NULL: unsupported

// ---- shaders (d3d9_shader.c) ----
typedef struct {
    int stage;                 // 0 vertex, 1 pixel
    uint32_t version;          // 0xFFFE0300...
    uint32_t hash;
    char *msl;                 // the translated function (named vs_main / ps_main)
    // vertex shader inputs (dcl usage/index per input register), outputs
    struct { uint8_t used, usage, index; } in[16];
    struct { uint8_t used, usage, index, mask; } out[12];   // SM3 output registers; SM2: fixed set (see translator)
    uint32_t samplers;         // bitmask of used samplers
    uint8_t sampler_type[16];  // 2 = 2D, 3 = cube, 4 = volume
    int max_const_f;           // highest c# referenced + 1 (relative addressing: 256)
    uint8_t color_mask;        // pixel shader: oC# written
    uint8_t writes_depth;      // pixel shader: oDepth written
    char error[256];           // translation failure
} ShaderInfo;
int d3d9_translate_shader(const uint32_t *tokens, size_t ntokens, ShaderInfo *out);
// complete MSL sources (prelude + body + entry point "vs_main" / "ps_main"):
//   vertex: `vsin` is the VSIn struct (attributes from the vertex declaration) and `fetch` the statements that fill
//   v[] from `in`; both from the device's declaration -> shader matching. NULL, NULL: no inputs (tests).
char *d3d9_vs_source(const ShaderInfo *vs, const char *vsin, const char *fetch);
char *d3d9_ps_source(const ShaderInfo *ps);
size_t d3d9_shader_length(const uint32_t *tokens, size_t max);   // tokens up to and including the end token, 0 if none

// constant buffer layout shared by the translator and the device
#define VS_CONST_F 256
#define PS_CONST_F 224
typedef struct {
    float c[VS_CONST_F][4];
    int32_t i[16][4];
    uint32_t b[16];
    float pos_fixup[4];        // half-pixel offset (x, y) in clip space, viewport flip
} VSConsts;
typedef struct {
    float c[PS_CONST_F][4];
    int32_t i[16][4];
    uint32_t b[16];
    float alpha_ref[4];        // alpha test reference (x), enabled (y), func (z)
    float fog[4];
} PSConsts;

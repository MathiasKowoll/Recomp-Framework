// MSL shared by every translated D3D9 shader: register structs used by the body functions (d3d9_shader.c), the
// stage interface structs used by the entry points (d3d9_metal.m), constant buffers and D3D-exact helpers.
#pragma once
#define D3D9_VARYINGS(X) X(FOG0) X(COLOR0) X(COLOR1) X(TEXCOORD0) X(TEXCOORD1) X(TEXCOORD2) X(TEXCOORD3) X(TEXCOORD4) \
    X(TEXCOORD5) X(TEXCOORD6) X(TEXCOORD7) X(TEXCOORD8) X(TEXCOORD9) X(NORMAL0) X(TANGENT0) X(BINORMAL0)
#define D3D9_STR_(...) #__VA_ARGS__
#define D3D9_STR(...) D3D9_STR_(__VA_ARGS__)

static const char d3d9_msl_prelude[] =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"#pragma clang diagnostic ignored \"-Wunused-variable\"\n"
"#pragma clang diagnostic ignored \"-Wunused-parameter\"\n"
"struct VSConsts { float4 c[256]; int4 i[16]; uint b[16]; float4 pos_fixup; };\n"
"struct PSConsts { float4 c[224]; int4 i[16]; uint b[16]; float4 alpha_ref; float4 fog; };\n"
"struct VSRegs { float4 position, psize4, FOG0, COLOR0, COLOR1, TEXCOORD0, TEXCOORD1, TEXCOORD2, TEXCOORD3, TEXCOORD4,\n"
"                TEXCOORD5, TEXCOORD6, TEXCOORD7, TEXCOORD8, TEXCOORD9, NORMAL0, TANGENT0, BINORMAL0; };\n"
"struct PSRegs { float4 vpos; bool face; float4 FOG0, COLOR0, COLOR1, TEXCOORD0, TEXCOORD1, TEXCOORD2, TEXCOORD3, TEXCOORD4,\n"
"                TEXCOORD5, TEXCOORD6, TEXCOORD7, TEXCOORD8, TEXCOORD9, NORMAL0, TANGENT0, BINORMAL0; };\n"
"struct PSORegs { float4 color0, color1, color2, color3, depth4; };\n"
"struct VSOut { float4 position [[position]];\n"
"  float4 FOG0 [[user(FOG0)]]; float4 COLOR0 [[user(COLOR0)]]; float4 COLOR1 [[user(COLOR1)]];\n"
"  float4 TEXCOORD0 [[user(TEXCOORD0)]]; float4 TEXCOORD1 [[user(TEXCOORD1)]]; float4 TEXCOORD2 [[user(TEXCOORD2)]];\n"
"  float4 TEXCOORD3 [[user(TEXCOORD3)]]; float4 TEXCOORD4 [[user(TEXCOORD4)]]; float4 TEXCOORD5 [[user(TEXCOORD5)]];\n"
"  float4 TEXCOORD6 [[user(TEXCOORD6)]]; float4 TEXCOORD7 [[user(TEXCOORD7)]]; float4 TEXCOORD8 [[user(TEXCOORD8)]];\n"
"  float4 TEXCOORD9 [[user(TEXCOORD9)]]; float4 NORMAL0 [[user(NORMAL0)]]; float4 TANGENT0 [[user(TANGENT0)]];\n"
"  float4 BINORMAL0 [[user(BINORMAL0)]]; };\n"
"typedef VSOut PSIn;\n"
// D3D9 arithmetic: rcp/rsq of 0 are +inf, rsq/log/pow use |x|, results replicate to all four components
"static inline float4 d3d_rcp(float x) { return float4(x == 0.0 ? INFINITY : 1.0 / x); }\n"
"static inline float4 d3d_rsq(float x) { float a = abs(x); return float4(a == 0.0 ? INFINITY : rsqrt(a)); }\n"
"static inline float4 d3d_log(float x) { float a = abs(x); return float4(a == 0.0 ? -INFINITY : log2(a)); }\n"
"static inline float4 d3d_pow(float x, float y) { return float4(pow(abs(x), y)); }\n"
"static inline float4 d3d_nrm(float4 v) { return v * rsqrt(dot(v.xyz, v.xyz)); }\n"
"static inline float4 d3d_lit(float4 s) { float d = max(s.x, 0.0); float sp = s.x > 0.0 ? pow(max(s.y, 0.0), clamp(s.w, -127.9961, 127.9961)) : 0.0;\n"
"  return float4(1.0, d, sp, 1.0); }\n"
"static inline void ps_regs_from(thread PSRegs &r, PSIn in, bool face) {\n"
"  r.vpos = float4(floor(in.position.xy), 0.0, 1.0); r.face = face;\n"
"#define CP(n) r.n = in.n;\n"
"  " D3D9_STR(D3D9_VARYINGS(CP)) "\n"
"#undef CP\n"
"}\n"
"static inline VSOut vs_out_from(thread VSRegs &o, constant VSConsts &k) {\n"
"  VSOut r; r.position = o.position;\n"
// D3D9 rasterizes pixel centers at integer coordinates: shift by half a pixel (pos_fixup = (-1/w, 1/h)) in clip space
"  r.position.xy += k.pos_fixup.xy * o.position.w;\n"
"#define CP(n) r.n = o.n;\n"
"  " D3D9_STR(D3D9_VARYINGS(CP)) "\n"
"#undef CP\n"
"  return r; }\n";

// Direct3D 9 shader bytecode (vs/ps 2.0, 2.x, 3.0) -> Metal Shading Language.
//
// The output is a body function the device wraps per draw configuration (d3d9_metal.m):
//   vertex: void vs_body(thread float4 *v, thread VSOut &o, constant VSConsts &k, <vertex textures>)
//   pixel:  void ps_body(thread PSIn &in, thread PSOut &o, constant PSConsts &k, <textures>)
// Registers become locals; constants read the constant buffer (k.c / k.i / k.b) unless the shader defines them
// (def), in which case a local holds the value. Varyings use D3D semantic names, see the VSOut / PSIn structs in
// the common prelude (d3d9_msl_prelude), so any vertex shader links with any pixel shader.
#include "d3d9_internal.h"
#include "d3d9_prelude.h"
#include <stdarg.h>

enum { T_TEMP = 0, T_INPUT = 1, T_CONST = 2, T_ADDR = 3, T_TEXTURE = 3, T_RASTOUT = 4, T_ATTROUT = 5, T_OUTPUT = 6,
       T_TEXCRDOUT = 6, T_CONSTINT = 7, T_COLOROUT = 8, T_DEPTHOUT = 9, T_SAMPLER = 10, T_CONSTBOOL = 14, T_LOOP = 15,
       T_MISC = 17, T_LABEL = 18, T_PREDICATE = 19 };
enum { U_POSITION = 0, U_BLENDWEIGHT, U_BLENDINDICES, U_NORMAL, U_PSIZE, U_TEXCOORD, U_TANGENT, U_BINORMAL,
       U_TESSFACTOR, U_POSITIONT, U_COLOR, U_FOG, U_DEPTH, U_SAMPLE };

typedef struct { char *s; size_t n, cap; } Buf;
static void bput(Buf *b, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    if (b->n + (size_t)n + 1 > b->cap) { b->cap = (b->n + (size_t)n + 1) * 2; b->s = realloc(b->s, b->cap); }
    vsnprintf(b->s + b->n, (size_t)n + 1, fmt, ap2); va_end(ap2);
    b->n += (size_t)n;
}

typedef struct {
    const uint32_t *t; size_t n, pc;
    int ps, major, minor;
    ShaderInfo *info;
    Buf body;
    int indent;
    // defined constants
    uint8_t def_f[256], def_i[16], def_b[16];
    float def_fv[256][4]; int32_t def_iv[16][4]; uint32_t def_bv[16];
    // declarations
    struct { int used, usage, index; } vin[16], vout[12], pin[16];
    int temps, uses_a0, uses_p0, uses_loop, oDepth, oColor_mask, ps_t_used;
    int err;
} Tr;

static int reg_type(uint32_t tok) { return (int)(((tok >> 28) & 7) | ((tok >> 8) & 0x18)); }
static int reg_num(uint32_t tok) { return (int)(tok & 0x7FF); }

static void fail(Tr *x, const char *fmt, ...) {
    if (x->err) return;
    va_list ap; va_start(ap, fmt); vsnprintf(x->info->error, sizeof x->info->error, fmt, ap); va_end(ap);
    x->err = 1;
}

// semantic name of a varying, shared with the device's VSOut / PSIn
static const char *semantic(int usage, int index, char *buf) {
    switch (usage) {
    case U_TEXCOORD: if (index < 10) { sprintf(buf, "TEXCOORD%d", index); return buf; } break;
    case U_COLOR: if (index < 2) { sprintf(buf, "COLOR%d", index); return buf; } break;
    case U_FOG: return "FOG0";
    case U_NORMAL: return "NORMAL0";
    case U_TANGENT: return "TANGENT0";
    case U_BINORMAL: return "BINORMAL0";
    case U_POSITION: if (index > 0) { sprintf(buf, "TEXCOORD%d", 9); return buf; } break;   // extra positions as varyings
    default: break;
    }
    sprintf(buf, "TEXCOORD9");
    return buf;
}

static const char *swz_str(uint32_t swz, char *b) {
    static const char c[4] = {'x', 'y', 'z', 'w'};
    for (int i = 0; i < 4; i++) b[i] = c[(swz >> (2 * i)) & 3];
    b[4] = 0;
    return b;
}

// base register expression (a float4 lvalue or rvalue), without swizzle or modifiers
static void reg_expr(Tr *x, uint32_t tok, uint32_t rel, int has_rel, char *out, size_t on) {
    int type = reg_type(tok), n = reg_num(tok);
    char relx[64] = "";
    if (has_rel) {
        int rt = reg_type(rel); char sw[5]; swz_str((rel >> 16) & 0xFF, sw);
        if (rt == T_LOOP) snprintf(relx, sizeof relx, "aL + ");
        else snprintf(relx, sizeof relx, "a0.%c + ", sw[0]);
    }
    switch (type) {
    case T_TEMP: snprintf(out, on, "r%d", n); if (n + 1 > x->temps) x->temps = n + 1; break;
    case T_INPUT:
        if (x->ps) {
            if (x->major < 3) snprintf(out, on, "in.COLOR%d", n);
            else { char sb[32]; snprintf(out, on, "in.%s", x->pin[n & 15].used ? semantic(x->pin[n & 15].usage, x->pin[n & 15].index, sb) : "TEXCOORD9"); }
        } else if (has_rel) snprintf(out, on, "v[%s%d]", relx, n);
        else snprintf(out, on, "v[%d]", n);
        break;
    case T_CONST:
        if (!has_rel && n < 256 && x->def_f[n]) snprintf(out, on, "d%d", n);
        else {
            snprintf(out, on, has_rel ? "k.c[clamp(%s%d, 0, %d)]" : "k.c[%s%d]", relx, n, (x->ps ? PS_CONST_F : VS_CONST_F) - 1);
            if (has_rel) x->info->max_const_f = x->ps ? PS_CONST_F : VS_CONST_F;
            else if (n + 1 > x->info->max_const_f) x->info->max_const_f = n + 1;
        }
        break;
    case T_ADDR:   // vs: a0; ps (2.x): t# texture coordinate input
        if (x->ps) { snprintf(out, on, "in.TEXCOORD%d", n); x->ps_t_used |= 1 << n; }
        else { snprintf(out, on, "float4(a0)"); x->uses_a0 = 1; }
        break;
    case T_RASTOUT: snprintf(out, on, n == 0 ? "o.position" : n == 1 ? "o.FOG0" : "o.psize4"); break;
    case T_ATTROUT: snprintf(out, on, "o.COLOR%d", n & 1); break;
    case T_OUTPUT:
        if (x->major >= 3) {
            char sb[32];
            if (!x->vout[n & 15].used) snprintf(out, on, "o.TEXCOORD9");
            else if (x->vout[n & 15].usage == U_POSITION && x->vout[n & 15].index == 0) snprintf(out, on, "o.position");
            else if (x->vout[n & 15].usage == U_PSIZE) snprintf(out, on, "o.psize4");
            else snprintf(out, on, "o.%s", semantic(x->vout[n & 15].usage, x->vout[n & 15].index, sb));
        } else snprintf(out, on, "o.TEXCOORD%d", n);
        break;
    case T_CONSTINT:
        if (n < 16 && x->def_i[n]) snprintf(out, on, "float4(di%d)", n);
        else snprintf(out, on, "float4(k.i[%d])", n);
        break;
    case T_CONSTBOOL: snprintf(out, on, (n < 16 && x->def_b[n]) ? "float4(db%d)" : "float4(k.b[%d] != 0)", n); break;
    case T_COLOROUT: snprintf(out, on, "o.color%d", n); x->oColor_mask |= 1 << n; break;
    case T_DEPTHOUT: snprintf(out, on, "o.depth4"); x->oDepth = 1; break;
    case T_LOOP: snprintf(out, on, "float4(aL)"); x->uses_loop = 1; break;
    case T_MISC: snprintf(out, on, n == 0 ? "in.vpos" : "float4(in.face ? 1.0 : -1.0)"); break;
    case T_PREDICATE: snprintf(out, on, "float4(p0)"); x->uses_p0 = 1; break;
    case T_SAMPLER: snprintf(out, on, "s%d", n); break;
    default: fail(x, "register type %d", type); snprintf(out, on, "float4(0)"); break;
    }
}

// source operand with swizzle and modifier; `reg_add` offsets the register number (matrix rows)
static void src_from(Tr *x, uint32_t tok, uint32_t rel, int reg_add, char *out, size_t on) {
    int has_rel = (tok >> 13) & 1;
    uint32_t t2 = (tok & ~0x7FFu) | ((tok + (uint32_t)reg_add) & 0x7FFu);
    char base[160], sw[5];
    reg_expr(x, t2, rel, has_rel, base, sizeof base);
    swz_str((tok >> 16) & 0xFF, sw);
    char v[200];
    if (!strcmp(sw, "xyzw")) snprintf(v, sizeof v, "%s", base); else snprintf(v, sizeof v, "(%s).%s", base, sw);
    switch ((tok >> 24) & 0xF) {
    case 0: snprintf(out, on, "%s", v); break;
    case 1: snprintf(out, on, "(-%s)", v); break;
    case 2: snprintf(out, on, "(%s - 0.5)", v); break;
    case 3: snprintf(out, on, "(0.5 - %s)", v); break;
    case 4: snprintf(out, on, "(2.0 * %s - 1.0)", v); break;
    case 5: snprintf(out, on, "(1.0 - 2.0 * %s)", v); break;
    case 6: snprintf(out, on, "(1.0 - %s)", v); break;
    case 7: snprintf(out, on, "(2.0 * %s)", v); break;
    case 8: snprintf(out, on, "(-2.0 * %s)", v); break;
    case 11: snprintf(out, on, "abs(%s)", v); break;
    case 12: snprintf(out, on, "(-abs(%s))", v); break;
    case 13: snprintf(out, on, "float4(!bool4(%s))", v); break;
    default: snprintf(out, on, "%s", v); fail(x, "source modifier %u", (tok >> 24) & 0xF); break;
    }
}
static void src_n(Tr *x, int reg_add, char *out, size_t on) {
    uint32_t tok = x->t[x->pc++], rel = 0;
    if (((tok >> 13) & 1) && x->major >= 2) rel = x->t[x->pc++];
    src_from(x, tok, rel, reg_add, out, on);
}
static void src(Tr *x, char *out, size_t on) { src_n(x, 0, out, on); }

typedef struct { char name[160]; char mask[5]; int sat, nmask; } Dst;
static void dst(Tr *x, Dst *d) {
    uint32_t tok = x->t[x->pc++], rel = 0;
    int has_rel = (tok >> 13) & 1;
    if (has_rel) rel = x->t[x->pc++];
    reg_expr(x, tok, rel, has_rel, d->name, sizeof d->name);
    int wm = (tok >> 16) & 0xF, k = 0;
    if (!wm) wm = 0xF;
    for (int i = 0; i < 4; i++) if (wm & (1 << i)) d->mask[k++] = "xyzw"[i];
    d->mask[k] = 0; d->nmask = k;
    d->sat = (tok >> 20) & 1;
}

static void line(Tr *x, const char *fmt, ...) {
    char tmp[2048]; va_list ap; va_start(ap, fmt); vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    bput(&x->body, "%*s%s\n", 4 + 4 * x->indent, "", tmp);
}

// predication: the instruction's predicate token comes after its destination and before its sources (SM3 ps/vs)
static int pred_tok(Tr *x, uint32_t ins, char *p, size_t pn) {
    if (!((ins >> 28) & 1)) return 0;
    // the predicate source token follows the destination; caller reads it after dst()
    uint32_t tok = x->t[x->pc++];
    char sw[5]; swz_str((tok >> 16) & 0xFF, sw);
    int neg = ((tok >> 24) & 0xF) == 13;
    snprintf(p, pn, "%sp0.%s", neg ? "!" : "", sw);
    x->uses_p0 = 1;
    return 1;
}

// write `expr` (a float4 expression) to the destination, honouring mask, saturate and predication
static void assign(Tr *x, Dst *d, const char *expr, const char *pred) {
    char e[2400];
    if (d->sat) snprintf(e, sizeof e, "saturate(float4(%s))", expr); else snprintf(e, sizeof e, "float4(%s)", expr);
    if (pred && *pred) {
        line(x, "{ float4 t_ = %s; bool4 pr_ = bool4(%s); %s.%s = select(%s.%s, t_.%s, pr_.%s); }",
             e, pred, d->name, d->mask, d->name, d->mask, d->mask, d->mask);
        return;
    }
    if (d->nmask == 4) line(x, "%s = %s;", d->name, e);
    else line(x, "%s.%s = (%s).%s;", d->name, d->mask, e, d->mask);
}

static const char *cmp_op(int c) {
    switch (c) { case 1: return ">"; case 2: return "=="; case 3: return ">="; case 4: return "<"; case 5: return "!="; case 6: return "<="; default: return "=="; }
}

static void decl(Tr *x) {
    uint32_t u = x->t[x->pc++], d = x->t[x->pc];
    int type = reg_type(d), n = reg_num(d);
    int usage = (int)(u & 0x1F), index = (int)((u >> 16) & 0xF);
    if (type == T_SAMPLER) {
        int tt = (int)((u >> 27) & 0xF);
        x->info->samplers |= 1u << n;
        x->info->sampler_type[n & 15] = (uint8_t)(tt ? tt : 2);
    } else if (type == T_INPUT) {
        if (x->ps) { x->pin[n & 15].used = 1; x->pin[n & 15].usage = usage; x->pin[n & 15].index = index; }
        else { x->vin[n & 15].used = 1; x->vin[n & 15].usage = usage; x->vin[n & 15].index = index;
               x->info->in[n & 15].used = 1; x->info->in[n & 15].usage = (uint8_t)usage; x->info->in[n & 15].index = (uint8_t)index; }
    } else if (type == T_OUTPUT && !x->ps) {
        x->vout[n & 15].used = 1; x->vout[n & 15].usage = usage; x->vout[n & 15].index = index;
        x->info->out[n & 15].used = 1; x->info->out[n & 15].usage = (uint8_t)usage; x->info->out[n & 15].index = (uint8_t)index;
    } else if (type == T_TEXTURE && x->ps) {
        x->ps_t_used |= 1 << n;
    }
    x->pc++;
}

// first pass: declarations and defs (they may come after use in the token stream? no, but outputs are declared
// before the code that writes them, and we need all of them to name registers)
static void prepass(Tr *x) {
    size_t pc = 1;
    while (pc < x->n) {
        uint32_t ins = x->t[pc];
        uint32_t op = ins & 0xFFFF;
        if (op == 0xFFFF) break;
        if (op == 0xFFFE) { pc += 1 + ((ins >> 16) & 0x7FFF); continue; }
        size_t len = (ins >> 24) & 0xF;
        size_t save = x->pc;
        x->pc = pc + 1;
        if (op == 31) decl(x);
        else if (op == 81) {   // def c#, f, f, f, f
            int r = reg_num(x->t[pc + 1]);
            if (r < 256) { x->def_f[r] = 1; memcpy(x->def_fv[r], &x->t[pc + 2], 16); }
        } else if (op == 48) { int r = reg_num(x->t[pc + 1]); if (r < 16) { x->def_i[r] = 1; memcpy(x->def_iv[r], &x->t[pc + 2], 16); } }
        else if (op == 47) { int r = reg_num(x->t[pc + 1]); if (r < 16) { x->def_b[r] = 1; x->def_bv[r] = x->t[pc + 2]; } }
        x->pc = save;
        pc += 1 + len;
    }
}

static void emit(Tr *x) {
    x->pc = 1;
    while (x->pc < x->n && !x->err) {
        uint32_t ins = x->t[x->pc];
        uint32_t op = ins & 0xFFFF;
        if (op == 0xFFFF) break;
        if (op == 0xFFFE) { x->pc += 1 + ((ins >> 16) & 0x7FFF); continue; }
        size_t len = (ins >> 24) & 0xF, next = x->pc + 1 + len;
        uint32_t ctl = (ins >> 16) & 0xFF;
        x->pc++;
        Dst d; char a[400], b[400], c[400], p[64] = "", e[1400];
        switch (op) {
        case 0: break;
        case 31: case 81: case 48: case 47: break;   // dcl / def*: handled in the prepass
        case 1:  dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); assign(x, &d, a, p); break;
        case 2:  dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); snprintf(e, sizeof e, "%s + %s", a, b); assign(x, &d, e, p); break;
        case 3:  dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); snprintf(e, sizeof e, "%s - %s", a, b); assign(x, &d, e, p); break;
        case 4:  dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); src(x, c, sizeof c); snprintf(e, sizeof e, "fma(%s, %s, %s)", a, b, c); assign(x, &d, e, p); break;
        case 5:  dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); snprintf(e, sizeof e, "%s * %s", a, b); assign(x, &d, e, p); break;
        case 6:  dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); snprintf(e, sizeof e, "d3d_rcp((%s).x)", a); assign(x, &d, e, p); break;
        case 7:  dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); snprintf(e, sizeof e, "d3d_rsq((%s).x)", a); assign(x, &d, e, p); break;
        case 8:  dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); snprintf(e, sizeof e, "dot((%s).xyz, (%s).xyz)", a, b); assign(x, &d, e, p); break;
        case 9:  dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); snprintf(e, sizeof e, "dot(%s, %s)", a, b); assign(x, &d, e, p); break;
        case 10: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); snprintf(e, sizeof e, "min(%s, %s)", a, b); assign(x, &d, e, p); break;
        case 11: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); snprintf(e, sizeof e, "max(%s, %s)", a, b); assign(x, &d, e, p); break;
        case 12: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); snprintf(e, sizeof e, "float4(%s < %s)", a, b); assign(x, &d, e, p); break;
        case 13: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); snprintf(e, sizeof e, "float4(%s >= %s)", a, b); assign(x, &d, e, p); break;
        case 14: case 78: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); snprintf(e, sizeof e, "exp2((%s).x)", a); assign(x, &d, e, p); break;
        case 15: case 79: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); snprintf(e, sizeof e, "d3d_log((%s).x)", a); assign(x, &d, e, p); break;
        case 16: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); snprintf(e, sizeof e, "d3d_lit(%s)", a); assign(x, &d, e, p); break;
        case 17: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); snprintf(e, sizeof e, "float4(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", a, b, a, b); assign(x, &d, e, p); break;
        case 18: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); src(x, c, sizeof c); snprintf(e, sizeof e, "mix(%s, %s, %s)", c, b, a); assign(x, &d, e, p); break;
        case 19: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); snprintf(e, sizeof e, "fract(%s)", a); assign(x, &d, e, p); break;
        case 20: case 21: case 22: case 23: case 24: {   // m4x4 m4x3 m3x4 m3x3 m3x2: rows are consecutive registers
            dst(x, &d); pred_tok(x, ins, p, sizeof p);
            src(x, a, sizeof a);
            int rows = op == 20 ? 4 : op == 21 ? 3 : op == 22 ? 4 : op == 23 ? 3 : 2;
            int dim = (op == 20 || op == 21) ? 4 : 3;
            uint32_t mtok = x->t[x->pc], mrel = ((mtok >> 13) & 1) ? x->t[x->pc + 1] : 0;
            char comp[4][420];
            for (int r = 0; r < 4; r++) {
                char row[400];
                if (r < rows) { src_from(x, mtok, mrel, r, row, sizeof row);
                                snprintf(comp[r], sizeof comp[r], dim == 4 ? "dot(%s, %s)" : "dot((%s).xyz, (%s).xyz)", a, row); }
                else snprintf(comp[r], sizeof comp[r], "0.0");
            }
            snprintf(e, sizeof e, "float4(%s, %s, %s, %s)", comp[0], comp[1], comp[2], comp[3]);
            assign(x, &d, e, p);
            break;
        }
        case 32: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); snprintf(e, sizeof e, "d3d_pow((%s).x, (%s).x)", a, b); assign(x, &d, e, p); break;
        case 33: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); snprintf(e, sizeof e, "float4(cross((%s).xyz, (%s).xyz), 0.0)", a, b); assign(x, &d, e, p); break;
        case 34: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); if (x->major < 3) { src(x, b, sizeof b); src(x, c, sizeof c); } snprintf(e, sizeof e, "sign(%s)", a); assign(x, &d, e, p); break;
        case 35: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); snprintf(e, sizeof e, "abs(%s)", a); assign(x, &d, e, p); break;
        case 36: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); snprintf(e, sizeof e, "d3d_nrm(%s)", a); assign(x, &d, e, p); break;
        case 37: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); if (x->major < 3) { src(x, b, sizeof b); src(x, c, sizeof c); }
                 snprintf(e, sizeof e, "float4(cos((%s).x), sin((%s).x), 0.0, 0.0)", a, a); assign(x, &d, e, p); break;
        case 46: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); x->uses_a0 = 1;
                 line(x, "{ int4 t_ = int4(floor(%s + 0.5)); a0.%s = t_.%s; }", a, d.mask, d.mask); break;
        case 88: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); src(x, c, sizeof c); snprintf(e, sizeof e, "select(%s, %s, %s >= 0.0)", c, b, a); assign(x, &d, e, p); break;
        case 80: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); src(x, c, sizeof c); snprintf(e, sizeof e, "select(%s, %s, %s > 0.5)", c, b, a); assign(x, &d, e, p); break;
        case 90: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); src(x, b, sizeof b); src(x, c, sizeof c); snprintf(e, sizeof e, "dot((%s).xy, (%s).xy) + (%s).x", a, b, c); assign(x, &d, e, p); break;
        case 91: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); snprintf(e, sizeof e, "dfdx(%s)", a); assign(x, &d, e, p); break;
        case 92: dst(x, &d); pred_tok(x, ins, p, sizeof p); src(x, a, sizeof a); snprintf(e, sizeof e, "dfdy(%s)", a); assign(x, &d, e, p); break;
        case 65: {   // texkill: the operand is encoded as a destination; any masked component < 0 discards
            uint32_t tok = x->t[x->pc]; Dst k; dst(x, &k); (void)tok;
            line(x, "if (any((%s).%s < 0.0)) discard_fragment();", k.name, k.mask);
            break;
        }
        case 66: case 93: case 95: {   // texld / texldp / texldb, texldd, texldl
            dst(x, &d); pred_tok(x, ins, p, sizeof p);
            src(x, a, sizeof a);
            uint32_t stok = x->t[x->pc];
            int s = reg_num(stok);
            src(x, b, sizeof b);
            char g1[400] = "", g2[400] = "";
            if (op == 93) { src(x, g1, sizeof g1); src(x, g2, sizeof g2); }
            int tt = x->info->sampler_type[s & 15]; if (!tt) tt = 2;
            x->info->samplers |= 1u << s;
            const char *crd = tt == 2 ? "xy" : "xyz";
            char coord[500];
            if (op == 66 && ctl == 1) snprintf(coord, sizeof coord, "((%s).%s / (%s).w)", a, crd, a);
            else snprintf(coord, sizeof coord, "(%s).%s", a, crd);
            if (op == 95) snprintf(e, sizeof e, "t%d.sample(z%d, %s, level((%s).w))", s, s, coord, a);
            else if (op == 66 && ctl == 2) snprintf(e, sizeof e, "t%d.sample(z%d, %s, bias((%s).w))", s, s, coord, a);
            else if (op == 93) snprintf(e, sizeof e, tt == 2 ? "t%d.sample(z%d, %s, gradient2d((%s).xy, (%s).xy))" : "t%d.sample(z%d, %s, gradientcube((%s).xyz, (%s).xyz))", s, s, coord, g1, g2);
            else if (!x->ps) snprintf(e, sizeof e, "t%d.sample(z%d, %s, level(0.0))", s, s, coord);
            else snprintf(e, sizeof e, "t%d.sample(z%d, %s)", s, s, coord);
            assign(x, &d, e, p);
            break;
        }
        case 94: {   // setp_comp dst, src0, src1
            dst(x, &d); src(x, a, sizeof a); src(x, b, sizeof b); x->uses_p0 = 1;
            line(x, "{ bool4 t_ = (%s %s %s); p0.%s = t_.%s; }", a, cmp_op((int)ctl), b, d.mask, d.mask);
            break;
        }
        // flow control
        case 40: src(x, a, sizeof a); line(x, "if (bool((%s).x)) {", a); x->indent++; break;
        case 41: src(x, a, sizeof a); src(x, b, sizeof b); line(x, "if ((%s).x %s (%s).x) {", a, cmp_op((int)ctl), b); x->indent++; break;
        case 42: x->indent--; line(x, "} else {"); x->indent++; break;
        case 43: x->indent--; line(x, "}"); break;
        case 27: {   // loop aL, i#
            x->pc++;   // aL
            uint32_t itok = x->t[x->pc++]; int n = reg_num(itok);
            char iv[32]; snprintf(iv, sizeof iv, (n < 16 && x->def_i[n]) ? "di%d" : "k.i[%d]", n);
            x->uses_loop = 1;
            line(x, "{ int aL_save_ = aL; aL = %s.y; for (int it_ = 0; it_ < min(%s.x, 255); it_++, aL += %s.z) {", iv, iv, iv); x->indent++;
            break;
        }
        case 29: x->indent--; line(x, "} aL = aL_save_; }"); break;
        case 38: {   // rep i#
            uint32_t itok = x->t[x->pc++]; int n = reg_num(itok);
            char iv[32]; snprintf(iv, sizeof iv, (n < 16 && x->def_i[n]) ? "di%d" : "k.i[%d]", n);
            line(x, "for (int rp_%zu = 0; rp_%zu < min(%s.x, 255); rp_%zu++) {", x->pc, x->pc, iv, x->pc); x->indent++;
            break;
        }
        case 39: x->indent--; line(x, "}"); break;
        case 44: line(x, "break;"); break;
        case 45: src(x, a, sizeof a); src(x, b, sizeof b); line(x, "if ((%s).x %s (%s).x) break;", a, cmp_op((int)ctl), b); break;
        case 96: src(x, a, sizeof a); line(x, "if (bool((%s).x)) break;", a); break;
        case 28: if (x->indent == 0) line(x, "return;"); else line(x, "return;"); break;
        case 25: case 26: case 30: fail(x, "subroutines (call/label) are not supported yet"); break;
        default: fail(x, "opcode %u", op); break;
        }
        x->pc = next;
    }
}

int d3d9_translate_shader(const uint32_t *tokens, size_t ntokens, ShaderInfo *info) {
    memset(info, 0, sizeof *info);
    Tr x = {0};
    x.t = tokens; x.n = ntokens; x.info = info;
    uint32_t ver = tokens[0];
    info->version = ver;
    x.ps = (ver >> 16) == 0xFFFF; x.major = (int)((ver >> 8) & 0xFF); x.minor = (int)(ver & 0xFF);
    info->stage = x.ps;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < ntokens; i++) { h ^= tokens[i]; h *= 16777619u; }
    info->hash = h;
    if ((ver >> 16) != 0xFFFE && (ver >> 16) != 0xFFFF) { snprintf(info->error, sizeof info->error, "not a shader (0x%08x)", ver); return 0; }
    if (x.major < 2) { snprintf(info->error, sizeof info->error, "shader model %d.%d is not supported", x.major, x.minor); return 0; }
    // SM2 vertex shaders have fixed outputs; SM2 pixel shaders fixed inputs (v0/v1 = COLOR0/1, t# = TEXCOORD#)
    prepass(&x);
    emit(&x);
    if (x.err) return 0;
    info->color_mask = (uint8_t)x.oColor_mask; info->writes_depth = (uint8_t)x.oDepth;
    // assemble the body function
    Buf o = {0};
    if (x.ps) bput(&o, "static void ps_%08x(thread PSRegs &in, thread PSORegs &o, constant PSConsts &k", h);
    else bput(&o, "static void vs_%08x(thread float4 *v, thread VSRegs &o, constant VSConsts &k", h);
    for (int s = 0; s < 16; s++) if (info->samplers & (1u << s)) {
        int tt = info->sampler_type[s] ? info->sampler_type[s] : 2;
        bput(&o, ", %s<float> t%d, sampler z%d", tt == 3 ? "texturecube" : tt == 4 ? "texture3d" : "texture2d", s, s);
    }
    bput(&o, ") {\n");
    for (int r = 0; r < x.temps; r++) bput(&o, "    float4 r%d = float4(0.0);\n", r);
    for (int r = 0; r < 256; r++) if (x.def_f[r]) bput(&o, "    const float4 d%d = float4(%.9g, %.9g, %.9g, %.9g);\n", r, x.def_fv[r][0], x.def_fv[r][1], x.def_fv[r][2], x.def_fv[r][3]);
    for (int r = 0; r < 16; r++) if (x.def_i[r]) bput(&o, "    const int4 di%d = int4(%d, %d, %d, %d);\n", r, x.def_iv[r][0], x.def_iv[r][1], x.def_iv[r][2], x.def_iv[r][3]);
    for (int r = 0; r < 16; r++) if (x.def_b[r]) bput(&o, "    const bool db%d = %s;\n", r, x.def_bv[r] ? "true" : "false");
    if (x.uses_a0) bput(&o, "    int4 a0 = int4(0);\n");
    if (x.uses_p0) bput(&o, "    bool4 p0 = bool4(false);\n");
    bput(&o, "    int aL = 0; (void)aL;\n");
    bput(&o, "%s}\n", x.body.s ? x.body.s : "");
    free(x.body.s);
    info->msl = o.s;
    return 1;
}

size_t d3d9_shader_length(const uint32_t *t, size_t max) {
    size_t pc = 1;
    while (pc < max) {
        uint32_t ins = t[pc], op = ins & 0xFFFF;
        if (op == 0xFFFF) return pc + 1;
        if (op == 0xFFFE) pc += 1 + ((ins >> 16) & 0x7FFF);
        else pc += 1 + ((ins >> 24) & 0xF);
    }
    return 0;
}

static void tex_params(Buf *o, const ShaderInfo *s, int call) {
    for (int i = 0; i < 16; i++) if (s->samplers & (1u << i)) {
        int tt = s->sampler_type[i] ? s->sampler_type[i] : 2;
        if (call) bput(o, ", t%d, z%d", i, i);
        else bput(o, ", %s<float> t%d [[texture(%d)]], sampler z%d [[sampler(%d)]]", tt == 3 ? "texturecube" : tt == 4 ? "texture3d" : "texture2d", i, i, i, i);
    }
}

char *d3d9_vs_source(const ShaderInfo *vs, const char *vsin, const char *fetch) {
    Buf o = {0};
    bput(&o, "%s", d3d9_msl_prelude);
    o.n += 0;
    // the body can exceed bput's buffer: append raw
    size_t bl = strlen(vs->msl);
    if (o.n + bl + 1 > o.cap) { o.cap = o.n + bl + 4096; o.s = realloc(o.s, o.cap); }
    memcpy(o.s + o.n, vs->msl, bl + 1); o.n += bl;
    bput(&o, "%s\n", vsin ? vsin : "struct VSIn { float4 dummy [[attribute(0)]]; };");
    bput(&o, "vertex VSOut vs_main(VSIn in [[stage_in]], constant VSConsts &k [[buffer(0)]]");
    tex_params(&o, vs, 0);
    bput(&o, ") {\n    float4 v[16];\n    for (int i = 0; i < 16; i++) v[i] = float4(0.0, 0.0, 0.0, 1.0);\n");
    bput(&o, "%s\n", fetch ? fetch : "    (void)in;");
    bput(&o, "    VSRegs o = {};\n    o.COLOR0 = float4(1.0); o.COLOR1 = float4(0.0);\n    vs_%08x(v, o, k", vs->hash);
    tex_params(&o, vs, 1);
    bput(&o, ");\n    return vs_out_from(o, k);\n}\n");
    return o.s;
}

char *d3d9_ps_source(const ShaderInfo *ps) {
    Buf o = {0};
    bput(&o, "%s", d3d9_msl_prelude);
    size_t bl = strlen(ps->msl);
    if (o.n + bl + 1 > o.cap) { o.cap = o.n + bl + 4096; o.s = realloc(o.s, o.cap); }
    memcpy(o.s + o.n, ps->msl, bl + 1); o.n += bl;
    bput(&o, "struct PSOutC {");
    for (int i = 0; i < 4; i++) if (ps->color_mask & (1 << i)) bput(&o, " float4 c%d [[color(%d)]];", i, i);
    if (ps->writes_depth) bput(&o, " float depth [[depth(any)]];");
    if (!ps->color_mask && !ps->writes_depth) bput(&o, " float4 c0 [[color(0)]];");
    bput(&o, " };\n");
    bput(&o, "fragment PSOutC ps_main(PSIn in [[stage_in]], bool face [[front_facing]], constant PSConsts &k [[buffer(0)]]");
    tex_params(&o, ps, 0);
    bput(&o, ") {\n    PSRegs r; ps_regs_from(r, in, face);\n    PSORegs o = {};\n    ps_%08x(r, o, k", ps->hash);
    tex_params(&o, ps, 1);
    bput(&o, ");\n");
    // alpha test (D3DRS_ALPHATESTENABLE): alpha_ref = (ref, enabled, D3DCMPFUNC)
    bput(&o, "    if (k.alpha_ref.y != 0.0) { float a = o.color0.w, ref = k.alpha_ref.x; int f = int(k.alpha_ref.z); bool pass =\n"
             "        f == 1 ? false : f == 2 ? a < ref : f == 3 ? a == ref : f == 4 ? a <= ref : f == 5 ? a > ref : f == 6 ? a != ref : f == 7 ? a >= ref : true;\n"
             "        if (!pass) discard_fragment(); }\n");
    bput(&o, "    PSOutC res;\n");
    for (int i = 0; i < 4; i++) if (ps->color_mask & (1 << i)) bput(&o, "    res.c%d = o.color%d;\n", i, i);
    if (ps->writes_depth) bput(&o, "    res.depth = o.depth4.x;\n");
    if (!ps->color_mask && !ps->writes_depth) bput(&o, "    res.c0 = float4(0.0);\n");
    bput(&o, "    return res;\n}\n");
    return o.s;
}

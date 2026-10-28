// Maxwell (SM 5.x) shader binary -> Metal Shading Language, or Vulkan GLSL (key->glsl; contract in shader.h).
//
// The binary is an NVN header (0x30), the shader program header (SPH, 0x50; graphics stages), then
// instructions in groups of four 64-bit words (one scheduling word + three instructions).
// Registers are emitted as `uint` variables; float ops use as_type<> conversions. Shaders with
// branches become a `switch (pc)` state machine with explicit SSY/PBK/PCNT stacks (SSY / PBK: 16 scalar variables shifted on push /
// pop, not arrays: a dynamically indexed per-thread array lands in scratch memory on AMD, and these shaders use it constantly).
//
// GLSL: the instruction body is generated in the MSL spelling and converted by glsl_rewrite() (a token-aware
// pass: as_type<T>() -> as_u/as_f overloads, float4 -> vec4, ulong -> uint64_t, char()/short() casts -> sx8/sx16,
// in.x / out.x stage I/O -> GLSL globals and built-ins), helpers with MSL names (saturate, fmin, clz, mulhi, ...)
// are defined in the GLSL prelude, and the structurally different constructs (texture lookups, global memory,
// half-float packing, subgroup shuffles, barriers, discard, declarations and entry point) are emitted per dialect.
// The MSL text is byte-identical to the MSL-only translator (tools/test_translate.c checks it).
#if defined(_WIN32)
#include <direct.h>  // _mkdir
#include <malloc.h>  // alloca
#else
#include <sys/stat.h>  // mkdir
#endif
#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shader.h"

uint64_t maxwell_gmem_base, maxwell_gmem_size;
uint32_t maxwell_gmem_chunk_shift;  // 0: one buffer for the whole heap (GLSL; shader.h)
bool maxwell_gmem_lazy;
bool maxwell_gmem_fs_miss;
const uint8_t *maxwell_dbg_code;
uint32_t maxwell_dbg_pc;
int maxwell_dbg_reg;
const uint8_t *maxwell_dbg_vs;

#define RZ 255
#define LMEM_WORDS 256  // local memory per invocation (1 KB)
#define PT 7

typedef struct {
    char *buf;
    size_t len, cap;
} Str;

static void sp(Str *s, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (s->len + n + 1 > s->cap) {
        s->cap = (s->len + n + 1) * 2;
        s->buf = realloc(s->buf, s->cap);
    }
    vsnprintf(s->buf + s->len, n + 1, fmt, ap2);
    va_end(ap2);
    s->len += n;
}

// The per-draw tint's recolour mode (ltint[0].w < 0; runtime/nvn/recolour.c, Cosmetics tab): an HSV change of the
// output colour, in its gamma encoding: for colours whose hue is within ltint[1].y of ltint[1].x (>= 0.5: any) and
// saturation >= ltint[1].z, the hue goes to ltint[1].x + ltint[0].x + (its distance from ltint[1].x) * ltint[1].w
// (turns: .w = 1 a plain shift, < 1 also pulls the hues together), saturation * .y, value * .z (kept <= 1 unless it
// was above). The shading stays: only the colour moves. ltint[2] / [3] (ltint[2].w < 0): a second such change with
// its own hue window, made from the same colour (a draw blending two recoloured maps, e.g. grass and sand: each keeps
// its own; where both windows take a colour, the two results are averaged by weight). `c` is the colour target's
// expression ("c0" / "o.c0"), v3 / v4 the vector types ("vec3" / "float3").
static void emit_recolour_window(Str *s, const char *v3, const char *v4, int k, const char *r, const char *m) {
    // (rc_hh0 / rc_ss0 / rc_vv0: the colour's HSV) -> r: the changed colour (linear), m: its weight
    sp(s, "    float rc_sd%d = fract(rc_hh0 - ltint[%d].x + 0.5f) - 0.5f, rc_dh%d = abs(rc_sd%d);\n", k, k + 1, k, k);
    sp(s, "    float %s = (ltint[%d].y >= 0.5f ? 1.0f : 1.0f - smoothstep(ltint[%d].y * 0.6f, ltint[%d].y, rc_dh%d)) *\n"
          "              smoothstep(ltint[%d].z * 0.5f, ltint[%d].z + 1e-4f, rc_ss0);\n", m, k + 1, k + 1, k + 1, k, k + 1, k + 1);
    sp(s, "    float rc_hh%d = fract(ltint[%d].x + ltint[%d].x + rc_sd%d * ltint[%d].w), rc_ss%d = clamp(rc_ss0 * ltint[%d].y, 0.0f, 1.0f);\n",
       k + 1, k + 1, k, k, k + 1, k + 1, k);
    sp(s, "    float rc_vv%d = min(rc_vv0 * ltint[%d].z, max(rc_vv0, 1.0f));\n", k + 1, k);
    sp(s, "    %s rc_k%d = clamp(abs(fract(%s(rc_hh%d) + %s(1.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - %s(3.0f)) - %s(1.0f), "
          "%s(0.0f), %s(1.0f));\n", v3, k, v3, k + 1, v3, v3, v3, v3, v3);
    sp(s, "    %s %s = pow(rc_vv%d * mix(%s(1.0f), rc_k%d, %s(rc_ss%d)), %s(2.2f));\n", v3, r, k + 1, v3, k, v3, k + 1, v3);
    (void)v4;
}

static void emit_recolour(Str *s, const char *c, const char *v3, const char *v4) {
    sp(s, "  else if (ltint[0].w < 0.0f) {\n");
    sp(s, "    %s rc_g = pow(max(%s.rgb, %s(0.0f)), %s(1.0f / 2.2f));\n", v3, c, v3, v3);
    sp(s, "    %s rc_K = %s(0.0f, -1.0f / 3.0f, 2.0f / 3.0f, -1.0f);\n", v4, v4);
    sp(s, "    %s rc_p = mix(%s(rc_g.b, rc_g.g, rc_K.w, rc_K.z), %s(rc_g.g, rc_g.b, rc_K.x, rc_K.y), %s(step(rc_g.b, rc_g.g)));\n", v4, v4, v4, v4);
    sp(s, "    %s rc_q = mix(%s(rc_p.x, rc_p.y, rc_p.w, rc_g.r), %s(rc_g.r, rc_p.y, rc_p.z, rc_p.x), %s(step(rc_p.x, rc_g.r)));\n", v4, v4, v4, v4);
    sp(s, "    float rc_d = rc_q.x - min(rc_q.w, rc_q.y);\n");
    sp(s, "    float rc_hh0 = abs(rc_q.z + (rc_q.w - rc_q.y) / (6.0f * rc_d + 1e-6f)), rc_ss0 = rc_d / (rc_q.x + 1e-6f), rc_vv0 = rc_q.x;\n");
    emit_recolour_window(s, v3, v4, 0, "rc_r", "rc_m");
    sp(s, "    if (ltint[2].w < 0.0f) {\n");
    emit_recolour_window(s, v3, v4, 2, "rc_r2", "rc_m2");
    sp(s, "      float rc_w = rc_m + rc_m2;\n");
    sp(s, "      if (rc_w > 0.0f) { rc_r = (rc_r * rc_m + rc_r2 * rc_m2) / rc_w; rc_m = min(rc_w, 1.0f); }\n    }\n");
    sp(s, "    %s.rgb = mix(%s.rgb, rc_r, %s(rc_m));\n  }\n", c, c, v3);
}

typedef struct {
    const uint8_t *code;   // first instruction group
    size_t ninsts;         // number of 8-byte words to scan
    int stage;
    const ShaderKey *key;
    ShaderInfo *info;
    Str body;
    uint8_t reg_used[256];
    uint8_t pred_used[8];
    uint32_t in_attr_mask, out_attr_mask;
    uint32_t vs_in_comp[32];
    bool uses_branches, uses_cc, uses_local, uses_vid, uses_iid, uses_frag_coord, uses_front_facing, uses_lane, uses_layer;
    uint8_t out_comp[32];  // vertex: output components written per varying (transform feedback order)
    bool uses_rscale;  // resolution-scale table (fragment/compute only)
    bool warned_surface;  // a dropped bindless surface op was noted
    uint32_t flat_mask;           // fragment inputs using constant (flat) interpolation
    uint32_t noperspective_mask;  // fragment inputs using pass (linear) interpolation
    uint32_t omap_target, omap_depth;
    int tex_slot[8192];           // texture index -> Metal slot + 1
    bool tex_weak[64];            // slot type only guessed (from a size query): the next sample decides
    bool fail;
    bool branchy;  // program uses the pc state machine
    int ipa_mode;  // interpolation mode of the IPA being translated (-1 for plain loads)
    int ipa_mul;   // its multiplier register (mode 1: the result is the "pass" value attr/w times this register)
    // GLSL (fields kept last: the MSL path's layout, including its out-of-range reg_used writes, is unchanged)
    bool glsl;               // emit Vulkan GLSL: dialect-specific constructs here, the rest via glsl_rewrite()
    bool uses_gmem_store;    // STG present: the gmem buffer is not readonly
    uint32_t cbuf_need[18];  // highest constant vec4 index + 1 per cbuf (array size of the GLSL block)
    // each BRX's jump table targets (glsl_flatten); brx_unknown: one whose table wasn't found (any instruction)
    uint32_t brx_pc[32], brx_tgt[32][64];
    int brx_ntgt[32], nbrx;
    bool brx_unknown;
} Ctx;

static uint64_t bits(uint64_t w, int lo, int n) { return (w >> lo) & ((n == 64) ? ~0ull : ((1ull << n) - 1)); }
static int64_t sbits(uint64_t w, int lo, int n) {
    int64_t v = (int64_t)bits(w, lo, n);
    return (v & (1ll << (n - 1))) ? v - (1ll << n) : v;
}

static void fail(Ctx *c, const char *fmt, ...) {
    if (c->fail) return;
    c->fail = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->info->error, sizeof c->info->error, fmt, ap);
    va_end(ap);
}

// Branch targets may point at a scheduling word; execution continues at the next instruction.
static uint32_t norm_target(int64_t t) {
    if (t >= 0 && ((t / 8) % 4) == 0) t += 8;
    return (uint32_t)t;
}

// ------------------------------------------------------------------ operands
static const char *reg(Ctx *c, int r, char *buf) {
    if (r == RZ) return "0u";
    c->reg_used[r] = 1;
    sprintf(buf, "r%d", r);
    return buf;
}

#define R(r) reg(c, (r), alloca(16))
#define F(expr) fmt_f(alloca(512), (expr))
static char *fmt_f(char *b, const char *e) { sprintf(b, "as_type<float>(%s)", e); return b; }

static const char *pred(Ctx *c, int p, bool neg, char *buf) {
    if (p == PT) return neg ? "false" : "true";
    c->pred_used[p] = 1;
    sprintf(buf, "%sp%d", neg ? "!" : "", p);
    return buf;
}
#define P(p, n) pred(c, (p), (n), alloca(16))

static const char *cbuf_ref(Ctx *c, int idx, int64_t off, const char *dyn, char *buf) {
    c->info->cbuf_mask |= 1u << idx;
    if (idx == 0) {  // a storage buffer binding: which slot (any, when indexed with a register)
        if (dyn) c->info->ssbo_read = 0xFFFF;
        else if (off >= 0x110 && off < 0x210) c->info->ssbo_read |= (uint16_t)(1u << ((off - 0x110) >> 4));
        else if (off >= 0x510 && off < 0x610) c->info->ssbo_read |= (uint16_t)(1u << ((off - 0x510) >> 4));
    }
    if (off + 16 > (int64_t)c->info->cbuf_size[idx]) c->info->cbuf_size[idx] = (uint32_t)((off + 16 + 255) & ~255);
    if (dyn) c->info->cbuf_dynamic |= 1u << idx;
    if (!dyn && idx < 18 && off >= 0 && (uint32_t)(off >> 4) + 1 > c->cbuf_need[idx]) c->cbuf_need[idx] = (uint32_t)(off >> 4) + 1;
    if (dyn) sprintf(buf, "cbr(cb%d, (%s) + %lldu)", idx, dyn, (long long)off);
    else sprintf(buf, "cb%d[%lld][%lld]", idx, (long long)(off >> 4), (long long)((off >> 2) & 3));
    return buf;
}

// Operand B in its register / constant buffer / immediate forms, selected by the opcode class.
typedef enum { B_REG, B_CBUF, B_IMM } BForm;

static BForm b_form(uint64_t w) {
    uint64_t top = w >> 60;
    if (top == 0x5) return B_REG;
    if (top == 0x4) return B_CBUF;
    return B_IMM;
}

static const char *operand_b(Ctx *c, uint64_t w, bool float_imm, char *buf) {
    switch (b_form(w)) {
    case B_REG: return reg(c, (int)bits(w, 20, 8), buf);
    case B_CBUF: return cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, buf);
    default: {
        uint32_t v = (uint32_t)bits(w, 20, 19) | ((uint32_t)bits(w, 56, 1) << 19);
        if (float_imm) v <<= 12;
        else if (v & 0x80000u) v |= 0xFFF00000u;  // sign-extend 20 bits
        sprintf(buf, "0x%08xu", v);
        return buf;
    }
    }
}
#define B(fi) operand_b(c, w, (fi), alloca(128))

// The "C" operand of three-source ops: REG_39, or the cbuf/reg swap form (0x51/0x53/... classes).
static const char *operand_c_for_ffma(Ctx *c, uint64_t w, char *buf, const char **b_out, char *bbuf) {
    uint64_t op8 = w >> 56;
    if ((op8 & 0xF8) == 0x50 && (op8 & 7) != 0) {  // e.g. 0x51: B = REG_39, C = cbuf
        *b_out = reg(c, (int)bits(w, 39, 8), bbuf);
        return cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, buf);
    }
    *b_out = operand_b(c, w, true, bbuf);
    return reg(c, (int)bits(w, 39, 8), buf);
}

static void set_reg(Ctx *c, int r, const char *expr) {
    if (r == RZ) return;
    c->reg_used[r] = 1;
    sp(&c->body, "    r%d = %s;\n", r, expr);
}

static const char *fneg_abs(const char *e, bool neg, bool abs_, char *buf) {
    if (abs_ && neg) sprintf(buf, "(-abs(%s))", e);
    else if (abs_) sprintf(buf, "abs(%s)", e);
    else if (neg) sprintf(buf, "(-%s)", e);
    else return e;
    return buf;
}
#define NA(e, n, a) fneg_abs((e), (n), (a), alloca(600))

static const char *fresult(const char *e, bool sat, char *buf) {
    if (sat) sprintf(buf, "as_type<uint>(saturate(%s))", e);
    else sprintf(buf, "as_type<uint>(%s)", e);
    return buf;
}
#define FRES(e, sat) fresult((e), (sat), alloca(700))

static const char *cmp_op(int op) {
    static const char *const ops[] = {"false", "<", "==", "<=", ">", "!=", ">=", "num", "nan",
                                      "<", "==", "<=", ">", "!=", ">=", "true"};
    return ops[op & 15];
}

// float comparison including unordered variants
static void fcmp_expr(char *out, int op, const char *a, const char *b) {
    switch (op) {
    case 0: strcpy(out, "false"); break;
    case 15: strcpy(out, "true"); break;
    case 7: sprintf(out, "(!isnan(%s) && !isnan(%s))", a, b); break;
    case 8: sprintf(out, "(isnan(%s) || isnan(%s))", a, b); break;
    default:
        if (op < 7) sprintf(out, "(%s %s %s)", a, cmp_op(op), b);
        else sprintf(out, "((%s %s %s) || isnan(%s) || isnan(%s))", a, cmp_op(op), b, a, b);
    }
}

static const char *bool_op(int op) { return op == 0 ? "&&" : op == 1 ? "||" : "!="; }

static void icmp_expr(char *out, int op, const char *a, const char *b, bool is_signed) {
    static const char *const ops[] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
    if (op == 0) strcpy(out, "false");
    else if (op == 7) strcpy(out, "true");
    else if (is_signed) sprintf(out, "(int(%s) %s int(%s))", a, ops[op], b);
    else sprintf(out, "(%s %s %s)", a, ops[op], b);
}

// ------------------------------------------------------------------ attributes
// Maxwell attribute space: 0x70 position, 0x80 + 16*i generic attribute i, 0x2F8 instance id,
// 0x2FC vertex id, 0x3FC front facing, 0x6C point size.
static void attr_load(Ctx *c, int dst, uint32_t addr, const char *idx) {
    char e[256];
    if (idx) { fail(c, "indexed attribute load"); return; }
    if (c->stage == STAGE_VERTEX) {
        if (addr >= 0x80 && addr < 0x280) {
            int a = (addr - 0x80) >> 4, comp = (addr >> 2) & 3;
            c->in_attr_mask |= 1u << a;
            c->vs_in_comp[a] |= 1u << comp;
            int kind = c->key ? c->key->attr_kind[a] : 0;
            if (kind == 0) sprintf(e, "as_type<uint>(in.a%d[%d])", a, comp);
            else sprintf(e, "as_type<uint>(in.a%d[%d])", a, comp);
        } else if (addr == 0x2F8) { c->uses_iid = true; strcpy(e, "iid"); }
        else if (addr == 0x2FC) { c->uses_vid = true; strcpy(e, "vid"); }
        else { sprintf(e, "0u /* attr %#x */", addr); }
    } else if (c->stage == STAGE_FRAGMENT) {
        if (addr >= 0x70 && addr < 0x80) {
            c->uses_frag_coord = true;
            int comp = (addr >> 2) & 3;
            if (comp == 0 && c->key && c->key->flip_x) {  // mirror mode's composite: x mirrored (rscale[65].x = width)
                c->uses_rscale = true;
                strcpy(e, "as_type<uint>(rscale[65][0] - (in.pos[0] - rscale[0][2]) / rscale[0][0])");
            } else if (comp < 2) {  // guest pixel units: the render target may be allocated larger
                c->uses_rscale = true;
                sprintf(e, "as_type<uint>((in.pos[%d] - rscale[0][%d]) / rscale[0][%d])", comp, comp + 2, comp);
            } else {
                sprintf(e, "as_type<uint>(in.pos[%d])", comp);
            }
        } else if (addr >= 0x80 && addr < 0x280) {
            int a = (addr - 0x80) >> 4, comp = (addr >> 2) & 3;
            // (MSL: a mask of 0 means unknown. GLSL: the renderer always passes the vertex shader's outputs, and
            // Vulkan requires every fragment input to be a vertex output, so 0 means none.)
            if (c->key && (c->key->vs_out_mask || c->key->glsl) && !(c->key->vs_out_mask & (1u << a))) {
                strcpy(e, comp == 3 ? "0x3f800000u" : "0u");  // not provided by the vertex shader
            } else {
                c->in_attr_mask |= 1u << a;
                // IPA "pass" yields attr/w (the shader multiplies by w itself); Metal gives attr. "multiply" is the
                // pass value times a register - usually w (from the pass of position w: Metal's attr again), but a
                // shader may pass its own (the Mysterious Woods mist: 1/(v4.w * pos.w), a projective screen lookup)
                if (c->ipa_mode == 0) sprintf(e, "as_type<uint>(in.v%d[%d] * in.pos.w)", a, comp);
                else if (c->ipa_mode == 1 && c->ipa_mul != RZ) {
                    char mb[16];
                    sprintf(e, "as_type<uint>(in.v%d[%d] * in.pos.w * as_type<float>(%s))", a, comp, reg(c, c->ipa_mul, mb));
                } else sprintf(e, "as_type<uint>(in.v%d[%d])", a, comp);
            }
        } else if (addr == 0x3FC) { c->uses_front_facing = true; strcpy(e, "(front_facing ? 0xFFFFFFFFu : 0u)"); }
        else if (addr == 0x2E0 || addr == 0x2E4) { sprintf(e, "as_type<uint>(point_coord[%d])", (addr >> 2) & 1); }
        else if (addr == 0x64) { c->uses_layer = true; strcpy(e, "rt_layer"); }  // the render target layer drawn to
        else { sprintf(e, "0u /* attr %#x */", addr); }
    } else {
        sprintf(e, "0u /* attr %#x */", addr);
    }
    set_reg(c, dst, e);
}

static void attr_store(Ctx *c, int src, uint32_t addr) {
    char b[16];
    const char *v = reg(c, src, b);
    if (c->glsl && c->stage != STAGE_VERTEX) {  // no vertex outputs in this stage (MSL: undeclared `out`)
        if (addr >= 0x80 && addr < 0x280) c->out_attr_mask |= 1u << ((addr - 0x80) >> 4);
        if (addr == 0x6C) c->info->writes_point_size = true;
        sp(&c->body, "    /* store attr %#x */\n", addr);
        return;
    }
    if (addr >= 0x70 && addr < 0x80) {
        sp(&c->body, "    out.pos[%d] = as_type<float>(%s);\n", (addr >> 2) & 3, v);
    } else if (addr >= 0x80 && addr < 0x280) {
        int a = (addr - 0x80) >> 4;
        c->out_attr_mask |= 1u << a;
        c->out_comp[a] |= (uint8_t)(1u << ((addr >> 2) & 3));
        sp(&c->body, "    out.v%d[%d] = as_type<float>(%s);\n", a, (addr >> 2) & 3, v);
    } else if (addr == 0x6C) {
        c->info->writes_point_size = true;
        sp(&c->body, "    out.psize = as_type<float>(%s);\n", v);
    } else if (addr == 0x64) {  // render target layer: layered rendering into array / 3D targets
        c->info->writes_layer = true;
        sp(&c->body, "    out.layer = int(%s);\n", v);
    } else {
        sp(&c->body, "    /* store attr %#x */\n", addr);
    }
}


// ------------------------------------------------------------------ half precision (SM53)
// Swizzle: 0 H1_H0 (both halves), 1 F32 (one f32 used for both lanes), 2 H0_H0, 3 H1_H1.
// Merge: 0 H1_H0 (pack both), 1 F32 (lane 0 as f32), 2 MRG_H0 (replace low half), 3 MRG_H1 (replace high half).
static const char *hsrc(Ctx *c, const char *v, int swz, bool neg, bool abs_, char *buf) {
    (void)c;
    char t[300];
    sprintf(t, "hx(%s, %du)", v, swz);
    if (abs_ && neg) sprintf(buf, "(-abs(%s))", t);
    else if (abs_) sprintf(buf, "abs(%s)", t);
    else if (neg) sprintf(buf, "(-%s)", t);
    else strcpy(buf, t);
    return buf;
}
#define HS(v, sw, n, ab) hsrc(c, (v), (sw), (n), (ab), alloca(400))

static void hstore(Ctx *c, int d, const char *expr, int merge, bool sat) {
    char t[2048], e[1700];
    if (sat) sprintf(e, "saturate(%s)", expr); else strcpy(e, expr);
    if (d == RZ) return;
    c->reg_used[d] = 1;
    sprintf(t, "hpack(%s, %du, r%d)", e, merge, d);
    set_reg(c, d, t);
}

// immediate halves: 9 bits of each half's top (the low 6 mantissa bits are zero) plus separate signs
static uint32_t himm(uint64_t w) {
    uint32_t lo = ((uint32_t)bits(w, 20, 9) << 6) | ((uint32_t)bits(w, 29, 1) << 15);
    uint32_t hi = ((uint32_t)bits(w, 30, 9) << 6) | ((uint32_t)bits(w, 56, 1) << 15);
    return lo | (hi << 16);
}

// LARECOMP_WARP_EMU=0: plain subgroup calls instead of the prelude's per-32-lane-warp w* functions
static bool warp_emu(void) {
    static int on = -1;
    if (on < 0) on = !getenv("LARECOMP_WARP_EMU") || atoi(getenv("LARECOMP_WARP_EMU")) != 0;
    return on;
}

static bool emit_half(Ctx *c, uint64_t w) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    char t[1800], ib[32], cb[256];
    uint16_t op = (uint16_t)(w >> 48);
    // ---- HADD2 / HMUL2 (reg 5d10/5d08, cbuf 7a8x/788x, imm 7a0x/780x, 32I 2c/2a)
    int kind = -1;  // 0 add, 1 mul
    int form = -1;  // 0 reg, 1 cbuf, 2 imm, 3 32i
    if ((op & 0xfff8) == 0x5d10) { kind = 0; form = 0; }
    else if ((op & 0xfff8) == 0x5d08) { kind = 1; form = 0; }
    else if ((op & 0xfe80) == 0x7a80) { kind = 0; form = 1; }
    else if ((op & 0xfe80) == 0x7a00) { kind = 0; form = 2; }
    else if ((op & 0xfe80) == 0x7880) { kind = 1; form = 1; }
    else if ((op & 0xfe80) == 0x7800) { kind = 1; form = 2; }
    else if ((op & 0xfe00) == 0x2c00) { kind = 0; form = 3; }
    else if ((op & 0xfe00) == 0x2a00) { kind = 1; form = 3; }
    if (kind >= 0) {
        int merge, swa, swb = 0;
        bool sat, nega, absa = false, negb = false, absb = false;
        const char *bv;
        if (form == 3) {
            merge = 0; sat = bits(w, 52, 1); swa = (int)bits(w, 53, 2); nega = kind == 0 ? bits(w, 56, 1) : false;
            sprintf(ib, "0x%08xu", (uint32_t)bits(w, 20, 32)); bv = ib;
        } else {
            // field layout per Ryujinx's InstDecoders (InstHadd2*/InstHmul2*)
            merge = (int)bits(w, 49, 2); swa = (int)bits(w, 47, 2); nega = bits(w, 43, 1); absa = bits(w, 44, 1);
            if (form == 0) {
                sat = bits(w, 32, 1); absb = bits(w, 30, 1); swb = (int)bits(w, 28, 2);
                if (kind == 0) negb = bits(w, 31, 1);
                else { nega = bits(w, 31, 1); negb = false; }  // HMUL2 reg: the negate bit is A's at 31
                bv = R((int)bits(w, 20, 8));
            } else if (form == 1) {
                sat = bits(w, 52, 1); negb = kind == 0 ? bits(w, 56, 1) : false; absb = bits(w, 54, 1); swb = 1;
                bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            } else {
                sat = bits(w, 52, 1); swb = 0;
                sprintf(ib, "0x%08xu", himm(w)); bv = ib;
            }
        }
        sprintf(t, kind == 0 ? "(%s + %s)" : "(%s * %s)", HS(R(a), swa, nega, absa), HS(bv, swb, negb, absb));
        hstore(c, d, t, merge, sat);
        return true;
    }
    // ---- HFMA2 (reg 5d00, cr 70xx|0x80, rc 60xx|0x80, imm 70xx, 32I 28)
    form = -1;
    if ((op & 0xfff8) == 0x5d00) form = 0;
    else if ((op & 0xf880) == 0x7080) form = 1;  // b = cbuf, c = reg
    else if ((op & 0xf880) == 0x6080) form = 2;  // b = reg, c = cbuf
    else if ((op & 0xf880) == 0x7000) form = 3;  // b = imm, c = reg
    else if ((op & 0xfe00) == 0x2800) form = 4;  // 32I: c = d
    if (form >= 0) {
        int merge, swa = (int)bits(w, 47, 2), swb = 0, swc = 0;
        bool sat, negb, negc;
        const char *bv, *cv;
        char cb2[256];
        switch (form) {  // field layout per Ryujinx's InstDecoders (InstHfma2*); NegA is applied to B (same product)
        case 0:
            merge = (int)bits(w, 49, 2); sat = bits(w, 32, 1); negb = bits(w, 31, 1); negc = bits(w, 30, 1);
            swb = (int)bits(w, 28, 2); swc = (int)bits(w, 35, 2);
            bv = R((int)bits(w, 20, 8)); cv = R((int)bits(w, 39, 8));
            break;
        case 1:  // b = cbuf, c = reg
            merge = (int)bits(w, 49, 2); sat = bits(w, 52, 1); negb = bits(w, 56, 1); negc = bits(w, 51, 1);
            swb = 1; swc = (int)bits(w, 53, 2);
            bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb); cv = R((int)bits(w, 39, 8));
            break;
        case 2:  // b = reg (field at 39), c = cbuf
            merge = (int)bits(w, 49, 2); sat = bits(w, 52, 1); negb = bits(w, 56, 1); negc = bits(w, 51, 1);
            swb = (int)bits(w, 53, 2); swc = 1;
            bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb2);
            break;
        case 3:  // b = imm, c = reg
            merge = (int)bits(w, 49, 2); sat = bits(w, 52, 1); negb = false; negc = bits(w, 51, 1);
            swb = 0; swc = (int)bits(w, 53, 2);
            sprintf(ib, "0x%08xu", himm(w)); bv = ib; cv = R((int)bits(w, 39, 8));
            break;
        default:  // 32I: c = d
            merge = 0; sat = false; negb = false; negc = bits(w, 51, 1);
            swa = (int)bits(w, 47, 2); swb = 0; swc = 0;
            sprintf(ib, "0x%08xu", (uint32_t)bits(w, 20, 32)); bv = ib; cv = R(d);
            break;
        }
        sprintf(t, "fma(%s, %s, %s)", HS(R(a), swa, false, false), HS(bv, swb, negb, false), HS(cv, swc, negc, false));
        hstore(c, d, t, merge, sat);
        return true;
    }
    // ---- HSETP2 / HSET2 (reg forms)
    if ((op & 0xfff8) == 0x5d20 || (op & 0xfff8) == 0x5d18) {
        bool setp = (op & 0xfff8) == 0x5d20;
        const char *x = HS(R(a), (int)bits(w, 47, 2), bits(w, 43, 1), bits(w, 44, 1));
        const char *y = HS(R((int)bits(w, 20, 8)), (int)bits(w, 28, 2), bits(w, 31, 1), bits(w, 30, 1));
        int cop = (int)bits(w, 35, 4), bop = (int)bits(w, 45, 2);
        const char *pp = P((int)bits(w, 39, 3), bits(w, 42, 1));
        char lx[512], ly[512];
        sp(&c->body, "    { float2 hx_ = %s, hy_ = %s; bool pp = %s;\n", x, y, pp);
        fcmp_expr(lx, cop, "hx_.x", "hy_.x");
        fcmp_expr(ly, cop, "hx_.y", "hy_.y");
        sp(&c->body, "      bool l = %s, h = %s;\n", lx, ly);
        if (setp) {
            int pa = (int)bits(w, 3, 3), pb = (int)bits(w, 0, 3);
            if (bits(w, 49, 1)) {  // .H_AND: each half is combined with the predicate first, then they're ANDed
                sp(&c->body, "      l = (l %s pp) && (h %s pp); h = !l;\n", bool_op(bop), bool_op(bop));
                if (pa != PT) { c->pred_used[pa] = 1; sp(&c->body, "      p%d = l;\n", pa); }
                if (pb != PT) { c->pred_used[pb] = 1; sp(&c->body, "      p%d = h;\n", pb); }
            } else {
                if (pa != PT) { c->pred_used[pa] = 1; sp(&c->body, "      p%d = l %s pp;\n", pa, bool_op(bop)); }
                if (pb != PT) { c->pred_used[pb] = 1; sp(&c->body, "      p%d = h %s pp;\n", pb, bool_op(bop)); }
            }
        } else {
            bool bf = bits(w, 49, 1);
            if (d != RZ) {
                c->reg_used[d] = 1;
                sp(&c->body, "      l = l %s pp; h = h %s pp;\n", bool_op(bop), bool_op(bop));
                sp(&c->body, bf ? "      r%d = (l ? 0x3c00u : 0u) | (h ? 0x3c000000u : 0u);\n"
                                : "      r%d = (l ? 0xFFFFu : 0u) | (h ? 0xFFFF0000u : 0u);\n", d);
            }
        }
        sp(&c->body, "    }\n");
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ textures
static int tex_slot(Ctx *c, uint32_t index, int type, bool shadow, bool array) {
    if (index >= 8192) return 0;
    if (c->tex_slot[index]) {
        int s = c->tex_slot[index] - 1;
        if (type >= 0 && c->tex_weak[s]) {
            ShaderTexture *t = &c->info->tex[s];
            t->type = (uint8_t)type;
            t->shadow = shadow;
            t->is_array = array;
            c->tex_weak[s] = false;
        } else if (c->glsl && type >= 0 && (c->info->tex[s].type != type || c->info->tex[s].shadow != shadow)) {
            fail(c, "texture %u used as two types", index);  // one GLSL sampler type per binding
        }
        return s;
    }
    bool weak = type < 0;
    if (weak) type = TEXTYPE_2D;
    int s = c->info->ntex++;
    if (s >= 31) { fail(c, "too many textures"); return 0; }
    c->tex_slot[index] = s + 1;
    c->tex_weak[s] = weak;
    ShaderTexture *t = &c->info->tex[s];
    t->handle_offset = index * 4;
    t->type = (uint8_t)type;
    t->shadow = shadow;
    t->is_array = array;
    int hs = ((int)index * 4 - 0x20) / 8;  // (the handle slot the renderer binds, exec.c shader_texture_object)
    t->stencil = c->glsl && c->key && hs >= 0 && hs < 32 && ((c->key->stencil_slots >> hs) & 1);
    t->sint = c->glsl && c->key && hs >= 0 && hs < 32 && !t->stencil && ((c->key->sint_slots >> hs) & 1);
    return s;
}

static const char *tex_type_name(int t) {
    switch (t) {
    case TEXTYPE_1D: return "1d";
    case TEXTYPE_3D: return "3d";
    case TEXTYPE_CUBE: return "cube";
    case TEXTYPE_2D_ARRAY: return "2d_array";
    case TEXTYPE_CUBE_ARRAY: return "cube_array";
    case TEXTYPE_1D_ARRAY: return "1d_array";
    default: return "2d";
    }
}

// Write texture results: component mask selects which of r,g,b,a go to consecutive registers.
static void write_tex_result(Ctx *c, int slot, const char *call, uint32_t mask, int d0, int d1, bool texs, bool f16) {
    if (c->info->tex[slot].stencil) sp(&c->body, "    { float4 t = stc(%s);\n", call);  // (stencil: uint in .x)
    else if (c->info->tex[slot].sint) sp(&c->body, "    { float4 t = itc(%s);\n", call);
    else sp(&c->body, "    { float4 t = %s;\n", call);
    if (f16) {
        // TEXS/TLDS .F16: components (in mask order) packed as half pairs into d0 (lo, hi) and d1 (lo, hi)
        int comps[4], n = 0;
        for (int comp = 0; comp < 4; comp++) if (mask & (1u << comp)) comps[n++] = comp;
        char v[4][16];
        for (int i = 0; i < 4; i++) { if (i < n) sprintf(v[i], "t[%d]", comps[i]); else strcpy(v[i], "0.0f"); }
        const char *pk = c->glsl ? "      r%d = packHalf2x16(vec2(%s, %s));\n" : "      r%d = as_type<uint>(half2(half(%s), half(%s)));\n";
        if (d0 != RZ) { c->reg_used[d0] = 1; sp(&c->body, pk, d0, v[0], v[1]); }
        if (d1 != RZ) { c->reg_used[d1] = 1; sp(&c->body, pk, d1, v[2], v[3]); }
        sp(&c->body, "    }\n");
        return;
    }
    int regs[4], n = 0;
    if (texs) {
        // TEXS: up to two components in d0/d0+1, two more in d1/d1+1
        int out[4] = {d0, d0 == RZ ? RZ : d0 + 1, d1, d1 == RZ ? RZ : d1 + 1};
        for (int i = 0; i < 4; i++) regs[i] = out[i];
    } else {
        for (int i = 0; i < 4; i++) regs[i] = d0 == RZ ? RZ : d0 + i;
    }
    for (int comp = 0; comp < 4; comp++) {
        if (!(mask & (1u << comp))) continue;
        int r = regs[n++];
        if (r != RZ) { c->reg_used[r] = 1; sp(&c->body, "      r%d = as_type<uint>(t[%d]);\n", r, comp); }
    }
    sp(&c->body, "    }\n");
}

// Integer texel coordinates in guest units -> the (possibly larger) allocation of texture slot `slot`.
static const char *RSC2(Ctx *c, int slot, const char *fmt, ...) {
    static char bufs[4][1024];
    static int bi;
    char *o = bufs[bi++ & 3];
    char inner[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(inner, sizeof inner, fmt, ap);
    va_end(ap);
    if (c->stage == STAGE_VERTEX) { snprintf(o, 1024, "%s", inner); return o; }
    c->uses_rscale = true;
    if (c->stage == STAGE_FRAGMENT) {  // (rscf: this pixel's place inside its guest texel, see the helper)
        c->uses_frag_coord = true;
        snprintf(o, 1024, "rscf(%s, rscale[%d], rscale[0], float2(in.pos[0], in.pos[1]))", inner, slot + 1);
        return o;
    }
    snprintf(o, 1024, "rsc(%s, rscale[%d])", inner, slot + 1);
    return o;
}

// TEXS / TLDS: texture_info selects target and coordinate layout
static const char *alayer(Ctx *c, const char *r, int slot, char *buf) {
    if (c->glsl) sprintf(buf, "min(%s, uint(tsize%d(0).z) - 1u)", r, slot);
    else sprintf(buf, "min(%s, t%d.get_array_size() - 1u)", r, slot);
    return buf;
}
#define ALAYER(r, slot) alayer(c, (r), (slot), alloca(96))

// 2D normalized coordinates: a scaled texture whose contents occupy its top-left part (Dynamic resolution) is sampled
// there (prelude uvs(); rscale[1 + slot].zw). Vertex shaders have no rscale table: their lookups stay as they are.
static const char *uv2(Ctx *c, int slot, const char *x, const char *y, char *buf) {
    if (c->stage == STAGE_VERTEX) { snprintf(buf, 700, "%s2(%s, %s)", c->glsl ? "vec" : "float", x, y); return buf; }
    c->uses_rscale = true;
    snprintf(buf, 700, "uvs(%s2(%s, %s), rscale[%d])", c->glsl ? "vec" : "float", x, y, slot + 1);
    return buf;
}
#define UV2(slot, x, y) uv2(c, (slot), (x), (y), alloca(700))

// GLSL texture lookup on slot `slot` (its type is final here: a conflicting use fails in tex_slot).
// co: coordinate expressions (1 for 1D, 2 for 2D, 3 for 3D / cube), layer: uint expression or NULL, dref: depth
// reference or NULL, lod_kind 0 implicit / 1 level / 2 bias (lod), off: ivec2 texel offset or NULL.
// GLSL offsets must be constants, so register offsets move the coordinates by offset / size of the level
// (level 0 for implicit-LOD lookups). Non-fragment stages have no implicit LOD: level 0 (+ bias).
static void glsl_sample(Ctx *c, char *out, int slot, const char *const *co, const char *layer, const char *dref,
                        int lod_kind, const char *lod, const char *off) {
    if (c->fail) { strcpy(out, "vec4(0.0f)"); return; }  // e.g. a type conflict: tex[slot] is not this lookup's type
    const ShaderTexture *t = &c->info->tex[slot];
    int type = t->type;
    bool cube = type == TEXTYPE_CUBE || type == TEXTYPE_CUBE_ARRAY;
    int n = (type == TEXTYPE_1D || type == TEXTYPE_1D_ARRAY) ? 1 : (type == TEXTYPE_2D || type == TEXTYPE_2D_ARRAY) ? 2 : 3;
    if (c->stage != STAGE_FRAGMENT && lod_kind != 1) {
        if (lod_kind == 0) lod = "0.0f";
        lod_kind = 1;
    }
    if (t->shadow && type == TEXTYPE_3D) { fail(c, "3d shadow texture"); strcpy(out, "vec4(0.0f)"); return; }
    if (layer && type != TEXTYPE_1D_ARRAY && type != TEXTYPE_2D_ARRAY && type != TEXTYPE_CUBE_ARRAY) {
        fail(c, "array lookup on a 3d texture");  // TEX .3D with the array bit
        strcpy(out, "vec4(0.0f)");
        return;
    }
    char L[600], xs[1600], P[2000];
    if (lod_kind == 1) snprintf(L, sizeof L, "int(%s)", lod); else strcpy(L, "0");
    if (n == 1) {
        if (off) snprintf(xs, sizeof xs, "(%s + float((%s).x) / float(tsize%d(%s).x))", co[0], off, slot, L);
        else snprintf(xs, sizeof xs, "%s", co[0]);
    } else if (n == 2) {
        if (off) snprintf(xs, sizeof xs, "(vec2(%s, %s) + vec2(%s) / vec2(tsize%d(%s).xy))", co[0], co[1], off, slot, L);
        else snprintf(xs, sizeof xs, "vec2(%s, %s)", co[0], co[1]);
        if (c->stage != STAGE_VERTEX) { char w[1700]; snprintf(w, sizeof w, "uvs(%s, rscale[%d])", xs, slot + 1); strcpy(xs, w); c->uses_rscale = true; }
    } else {
        if (off && !cube) snprintf(xs, sizeof xs, "vec3(vec2(%s, %s) + vec2(%s) / vec2(tsize%d(%s).xy), %s)", co[0], co[1], off, slot, L, co[2]);
        else snprintf(xs, sizeof xs, "vec3(%s, %s, %s)", co[0], co[1], co[2]);  // cube lookups take no offset
    }
    const char *extra = NULL;  // separate compare argument (cube array shadow)
    if (!t->shadow) {
        if (layer) snprintf(P, sizeof P, "vec%d(%s, float(%s))", n + 1, xs, layer);
        else snprintf(P, sizeof P, "%s", xs);
    } else if (type == TEXTYPE_CUBE_ARRAY) {
        snprintf(P, sizeof P, "vec4(%s, float(%s))", xs, layer ? layer : "0u");
        extra = dref;
    } else if (type == TEXTYPE_1D) {
        snprintf(P, sizeof P, "vec3(%s, 0.0f, %s)", xs, dref);
    } else if (layer) {
        snprintf(P, sizeof P, "vec%d(%s, float(%s), %s)", n + 2, xs, layer, dref);
    } else {
        snprintf(P, sizeof P, "vec%d(%s, %s)", n + 1, xs, dref);
    }
    char e[2000], ex[300] = "";
    if (extra) snprintf(ex, sizeof ex, ", %s", extra);
    if (lod_kind == 0) snprintf(e, sizeof e, "texture(t%d, %s%s)", slot, P, ex);
    else if (lod_kind == 1) snprintf(e, sizeof e, "textureLod(t%d, %s%s, %s)", slot, P, ex, lod);
    else snprintf(e, sizeof e, "texture(t%d, %s%s, %s)", slot, P, ex, lod);
    if (t->shadow) sprintf(out, "vec4(%s)", e);
    else strcpy(out, e);
}

// TEXS / TLDS in GLSL (same slot allocation order and result layout as the MSL switch in emit_texs)
static bool glsl_texs(Ctx *c, uint64_t w, bool fetch, int *slot_out, char *call, const char *A, const char *A1,
                      const char *Bv, const char *B1) {
    uint32_t index = (uint32_t)bits(w, 36, 13);
    int info = (int)bits(w, 53, 4);
    int slot;
    const char *co[3];
    char fa[64], fa1[64], fb[64], fb1[64];
    sprintf(fa, "as_type<float>(%s)", A); sprintf(fa1, "as_type<float>(%s)", A1);
    sprintf(fb, "as_type<float>(%s)", Bv); sprintf(fb1, "as_type<float>(%s)", B1);
    if (!fetch) {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false); co[0] = fa;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_2D, false, false); co[0] = fa; co[1] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false); co[0] = fa; co[1] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, "0.0f", NULL); break;
        case 3: slot = tex_slot(c, index, TEXTYPE_2D, false, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, fb, NULL); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, true, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, fb, 0, NULL, NULL); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, true, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, fb1, 1, fb, NULL); break;
        case 6: slot = tex_slot(c, index, TEXTYPE_2D, true, false); co[0] = fa; co[1] = fa1;
            glsl_sample(c, call, slot, co, NULL, fb, 1, "0.0f", NULL); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true); co[0] = fa1; co[1] = fb;
            glsl_sample(c, call, slot, co, ALAYER(A, slot), NULL, 0, NULL, NULL); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true); co[0] = fa1; co[1] = fb;
            glsl_sample(c, call, slot, co, ALAYER(A, slot), NULL, 1, "0.0f", NULL); break;
        case 9: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, true, true); co[0] = fa1; co[1] = fb;
            glsl_sample(c, call, slot, co, A, fb1, 1, "0.0f", NULL); break;
        case 10: slot = tex_slot(c, index, TEXTYPE_3D, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 11: slot = tex_slot(c, index, TEXTYPE_3D, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, "0.0f", NULL); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 0, NULL, NULL); break;
        case 13: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false); co[0] = fa; co[1] = fa1; co[2] = fb;
            glsl_sample(c, call, slot, co, NULL, NULL, 1, fb1, NULL); break;
        default: fail(c, "texs info %d", info); return false;
        }
    } else {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "texelFetch(t%d, int(%s), 0)", slot, A); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "texelFetch(t%d, int(%s), int(%s))", slot, A, Bv); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), 0)", slot, RSC2(c, slot, "uint2(%s, %s)", A, Bv)); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), 0)", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, Bv, Bv)); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), int(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", A, A1), Bv); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "texelFetch(t%d, ivec3(%s, %s, %s), 0)", slot, A, A1, Bv); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            // (TLDS 2D array: x = Rb, y = Rb + 1, layer = Ra - unlike TEXS, whose array layout is layer, Ra + 1, Rb; per Ryujinx)
            sprintf(call, "texelFetch(t%d, ivec3(ivec2(%s), int(%s)), 0)", slot, RSC2(c, slot, "uint2(%s, %s)", Bv, B1), A); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "texelFetch(t%d, ivec2(%s), int(%s))", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, B1, B1), Bv); break;
        default: fail(c, "tlds info %d", info); return false;
        }
    }
    *slot_out = slot;
    return true;
}

static void emit_texs(Ctx *c, uint64_t w, bool fetch) {
    int d0 = (int)bits(w, 0, 8), d1 = (int)bits(w, 28, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    uint32_t index = (uint32_t)bits(w, 36, 13);
    int info = (int)bits(w, 53, 4);
    int msel = (int)bits(w, 50, 3);
    uint32_t mask;
    if (d1 == RZ) {
        static const uint32_t m2[] = {1, 2, 4, 8, 3, 9, 10, 12};
        mask = m2[msel];
    } else {
        static const uint32_t m4[] = {7, 11, 13, 14, 15, 15, 15, 15};
        mask = m4[msel];
    }
    char ra[16], ra1[16], rb[16], rb1[16];
    const char *A = reg(c, a, ra), *A1 = reg(c, a == RZ ? RZ : a + 1, ra1), *Bv = reg(c, b, rb),
               *B1 = reg(c, b == RZ ? RZ : b + 1, rb1);
    char call[1024];
    int slot;
    if (c->glsl) {
        char gcall[4096];
        if (!glsl_texs(c, w, fetch, &slot, gcall, A, A1, Bv, B1)) return;
        write_tex_result(c, slot, gcall, mask, d0, d1, true, !bits(w, 59, 1));
        return;
    }
    if (!fetch) {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "t%d.sample(s%d, %s)", slot, slot, F(A)); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.sample(s%d, %s)", slot, slot, UV2(slot, F(A), F(Bv))); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.sample(s%d, %s, level(0))", slot, slot, UV2(slot, F(A), F(Bv))); break;
        case 3: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.sample(s%d, %s, level(%s))", slot, slot, UV2(slot, F(A), F(A1)), F(Bv)); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, true, false);
            sprintf(call, "float4(t%d.sample_compare(s%d, %s, %s))", slot, slot, UV2(slot, F(A), F(A1)), F(Bv)); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, true, false);
            sprintf(call, "float4(t%d.sample_compare(s%d, %s, %s, level(%s)))", slot, slot, UV2(slot, F(A), F(A1)), F(B1), F(Bv)); break;
        case 6: slot = tex_slot(c, index, TEXTYPE_2D, true, false);
            sprintf(call, "float4(t%d.sample_compare(s%d, %s, %s, level(0)))", slot, slot, UV2(slot, F(A), F(A1)), F(Bv)); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            sprintf(call, "t%d.sample(s%d, %s, %s)", slot, slot, UV2(slot, F(A1), F(Bv)), ALAYER(A, slot)); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            sprintf(call, "t%d.sample(s%d, %s, %s, level(0))", slot, slot, UV2(slot, F(A1), F(Bv)), ALAYER(A, slot)); break;
        case 9: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, true, true);
            sprintf(call, "float4(t%d.sample_compare(s%d, %s, %s, %s, level(0)))", slot, slot, UV2(slot, F(A1), F(Bv)), A, F(B1)); break;
        case 10: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s))", slot, slot, F(A), F(A1), F(Bv)); break;
        case 11: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s), level(0))", slot, slot, F(A), F(A1), F(Bv)); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false);
            sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s))", slot, slot, F(A), F(A1), F(Bv)); break;
        case 13: slot = tex_slot(c, index, TEXTYPE_CUBE, false, false);
            sprintf(call, "t%d.sample(s%d, float3(%s, %s, %s), level(%s))", slot, slot, F(A), F(A1), F(Bv), F(B1)); break;
        default: fail(c, "texs info %d", info); return;
        }
    } else {
        switch (info) {
        case 0: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "t%d.read(uint(%s))", slot, A); break;
        case 1: slot = tex_slot(c, index, TEXTYPE_1D, false, false);
            sprintf(call, "t%d.read(uint(%s), uint(%s))", slot, A, Bv); break;
        case 2: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s)", slot, RSC2(c, slot, "uint2(%s, %s)", A, Bv)); break;
        case 4: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s)", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, Bv, Bv)); break;
        case 5: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s, uint(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", A, A1), Bv); break;
        case 7: slot = tex_slot(c, index, TEXTYPE_3D, false, false);
            sprintf(call, "t%d.read(uint3(%s, %s, %s))", slot, A, A1, Bv); break;
        case 8: slot = tex_slot(c, index, TEXTYPE_2D_ARRAY, false, true);
            // (TLDS 2D array: x = Rb, y = Rb + 1, layer = Ra - unlike TEXS, whose array layout is layer, Ra + 1, Rb; per Ryujinx)
            sprintf(call, "t%d.read(%s, uint(%s))", slot, RSC2(c, slot, "uint2(%s, %s)", Bv, B1), A); break;
        case 12: slot = tex_slot(c, index, TEXTYPE_2D, false, false);
            sprintf(call, "t%d.read(%s, uint(%s))", slot, RSC2(c, slot, "uint2(int2(%s, %s) + int2(int(%s << 28) >> 28, int(%s << 24) >> 28))", A, A1, B1, B1), Bv); break;
        default: fail(c, "tlds info %d", info); return;
        }
    }
    write_tex_result(c, slot, call, mask, d0, d1, true, !bits(w, 59, 1));  // 0xd0/0xd2: .F16 results
}

// ATOM (global memory atomic, 32-bit; per Ryujinx's InstAtom): address Ra (+ Ra+1 with .E) + a signed 20-bit offset,
// operand Rb, the old value to Rd. Same addressing as LDG / STG (compute: the dispatch's device-local copy first).
// RED is ATOM without the result: value in Rb at bits 0-7, size at 20, op at 23.
static void emit_atom(Ctx *c, uint64_t w, bool red) {
    int d = red ? RZ : (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)(red ? bits(w, 0, 8) : bits(w, 20, 8));
    int64_t off = sbits(w, 28, 20);
    bool wide = bits(w, 48, 1);
    int size = (int)(red ? bits(w, 20, 3) : bits(w, 49, 3)), op = (int)(red ? bits(w, 23, 3) : bits(w, 52, 4));
    if (!c->glsl) { fail(c, "atom (msl)"); return; }
    if (size > 1) { fail(c, "atom size %d", size); return; }
    if (op > 8) { fail(c, "atom op %d", op); return; }
    c->info->uses_gmem = true;
    c->uses_gmem_store = true;
    c->info->uses_gmem_store = true;
    char addr[256], v[16];
    if (a == RZ) sprintf(addr, "ulong(%lld)", (long long)off);
    else if (wide) sprintf(addr, "((ulong(%s) << 32 | ulong(%s)) + ulong(%lld))", R(a + 1), R(a), (long long)off);
    else sprintf(addr, "(ulong(%s) + ulong(%lld))", R(a), (long long)off);
    strcpy(v, R(b));
    sp(&c->body, "    { ulong ga = %s - 0x%llxul; bool ok = ga < 0x%llxul; uint old = 0u;\n", addr,
       (unsigned long long)maxwell_gmem_base, (unsigned long long)maxwell_gmem_size);
    bool fast = c->stage == STAGE_COMPUTE;
    if (fast) {
        c->info->uses_gmem_map = true;
        sp(&c->body, "      uint fw = ok ? gfind(uint(ga)) : 0xFFFFFFFFu;\n");
    }
    static const char *const direct[] = {"atomicAdd", "atomicMin", "atomicMax", NULL, NULL, "atomicAnd", "atomicOr",
                                         "atomicXor", "atomicExchange"};
    bool sgn = size == 1;
    static const char *const gma[] = {"GMA_ADD", "GMA_MIN", "GMA_MAX", NULL, NULL, "GMA_AND", "GMA_OR", "GMA_XOR", "GMA_XCH"};
    const bool lazy = maxwell_gmem_chunk_shift && maxwell_gmem_lazy;  // (chunks imported lazily: no gmem_ buffer, the GMA_ functions)
    for (int k = 0; k < (fast ? 2 : 1); k++) {
        const char *m = fast && k == 0 ? "gfast_.w[fw]" : "gmem_.w[uint(ga >> 2)]";
        const char *guard = fast && k == 0 ? "if (fw != 0xFFFFFFFFu)" : fast ? "else if (ok)" : "if (ok)";
        const bool chunked = lazy && !(fast && k == 0);
        if (chunked && direct[op] && !(sgn && (op == 1 || op == 2))) {
            sp(&c->body, "      %s old = %s(uint(ga >> 2), %s);\n", guard, gma[op], v);
        } else if (chunked) {
            char nv[300];
            if (op == 1) sprintf(nv, "uint(min(int(o_), int(%s)))", v);
            else if (op == 2) sprintf(nv, "uint(max(int(o_), int(%s)))", v);
            else if (op == 3) sprintf(nv, "(o_ >= %s ? 0u : o_ + 1u)", v);
            else sprintf(nv, "(o_ == 0u || o_ > %s ? %s : o_ - 1u)", v, v);
            sp(&c->body, "      %s { uint o_ = GMR(uint(ga >> 2)); for (;;) { uint r_ = GMCAS(uint(ga >> 2), o_, %s); if (r_ == o_) break; "
               "o_ = r_; } old = o_; }\n", guard, nv);
        } else if (direct[op] && !(sgn && (op == 1 || op == 2))) {
            sp(&c->body, "      %s old = %s(%s, %s);\n", guard, direct[op], m, v);
        } else {
            char nv[300];
            if (op == 1) sprintf(nv, "uint(min(int(o_), int(%s)))", v);
            else if (op == 2) sprintf(nv, "uint(max(int(o_), int(%s)))", v);
            else if (op == 3) sprintf(nv, "(o_ >= %s ? 0u : o_ + 1u)", v);
            else sprintf(nv, "(o_ == 0u || o_ > %s ? %s : o_ - 1u)", v, v);
            sp(&c->body, "      %s { uint o_ = %s; for (;;) { uint r_ = atomicCompSwap(%s, o_, %s); if (r_ == o_) break; o_ = r_; } "
               "old = o_; }\n", guard, m, m, nv);
        }
    }
    if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = old;\n", d); }
    sp(&c->body, "    }\n");
}

// TLD4S (2D gather, one component or a depth compare; per Ryujinx's EmitTexs Tld4s): x in Ra, y in Rb - or y in Ra+1
// when Rb holds the offset (.AOFFI: 6-bit signed x, y at bits 0 / 8) and / or the depth reference (.DC, after the
// offset). Results as TEXS: two components (Rd, Rd+1), four with Rd2 (Rd2, Rd2+1); .F16 (0xdf8) packs them.
static void emit_tld4s(Ctx *c, uint64_t w) {
    int d0 = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8), d1 = (int)bits(w, 28, 8);
    uint32_t index = (uint32_t)bits(w, 36, 13);
    int comp = (int)bits(w, 52, 2);
    bool aoffi = bits(w, 51, 1), dc = bits(w, 50, 1), f16 = bits(w, 55, 1);
    if (!c->glsl) { fail(c, "tld4s (msl)"); return; }
    if (d0 == RZ) return;
    char ra[16], ra1[16], rb[16], rb1[16];
    const char *A = reg(c, a, ra), *A1 = reg(c, a == RZ ? RZ : a + 1, ra1), *Bv = reg(c, b, rb),
               *B1 = reg(c, b == RZ ? RZ : b + 1, rb1);
    const char *y = dc || aoffi ? A1 : Bv;
    const char *dref = dc ? (aoffi ? B1 : Bv) : NULL;
    int slot = tex_slot(c, index, TEXTYPE_2D, dc, false);
    if (c->fail) return;
    char P[512], call[1200];
    if (aoffi)  // (GLSL gather offsets must be constant: move the coordinates instead, as glsl_sample does)
        snprintf(P, sizeof P, "(vec2(as_type<float>(%s), as_type<float>(%s)) + vec2(bitfieldExtract(int(%s), 0, 6), "
                 "bitfieldExtract(int(%s), 8, 6)) / vec2(tsize%d(0).xy))", A, y, Bv, Bv, slot);
    else
        snprintf(P, sizeof P, "vec2(as_type<float>(%s), as_type<float>(%s))", A, y);
    if (dc) snprintf(call, sizeof call, "textureGather(t%d, %s, as_type<float>(%s))", slot, P, dref);
    else snprintf(call, sizeof call, "textureGather(t%d, %s, %d)", slot, P, comp);
    write_tex_result(c, slot, call, d1 == RZ ? 3u : 15u, d0, d1, true, f16);
}

// TLD4 (gather; per Ryujinx's EmitTld4): Ra = [array index], coordinates; Rb = [offsets: one (.AOFFI) or two
// (.PTP: four texels, 6-bit signed x / y at bits 0 / 8, 16 / 24)], [depth reference]. The WMask components go to
// consecutive registers from Rd. GLSL gather offsets must be constant: the coordinates move instead, and .PTP takes
// one gather per texel (component i of textureGatherOffsets comes from offset i).
static const char *tld4_next(Ctx *c, int *r, char *buf) {
    const char *s = reg(c, *r, buf);
    if (*r != RZ) (*r)++;
    return s;
}

static void emit_tld4(Ctx *c, uint64_t w) {
    int d = (int)bits(w, 0, 8), ra = (int)bits(w, 8, 8), rb = (int)bits(w, 20, 8);
    uint32_t index = (uint32_t)bits(w, 36, 13), mask = (uint32_t)bits(w, 31, 4);
    int dim = (int)bits(w, 28, 3), comp = (int)bits(w, 56, 2), toff = (int)bits(w, 54, 2);
    bool dc = bits(w, 50, 1);
    static const int types[8] = {-1, -1, TEXTYPE_2D, TEXTYPE_2D_ARRAY, -1, -1, TEXTYPE_CUBE, TEXTYPE_CUBE_ARRAY};
    if (!c->glsl) { fail(c, "tld4 (msl)"); return; }
    if (types[dim] < 0 || toff == 3) { fail(c, "tld4 dim %d offset %d", dim, toff); return; }
    if (d == RZ) return;
    bool array = dim & 1, cube = dim >= 6;
    if (cube && toff) { fail(c, "tld4 cube with offsets"); return; }
    int slot = tex_slot(c, index, types[dim], dc, array);
    if (c->fail) return;
    char b[8][16], lay[128] = "", co[3][48], dref[48] = "", off[2][24];
    if (array) snprintf(lay, sizeof lay, "float(min(%s, uint(tsize%d(0).z) - 1u))", tld4_next(c, &ra, b[0]), slot);
    for (int i = 0; i < (cube ? 3 : 2); i++) snprintf(co[i], sizeof co[i], "as_type<float>(%s)", tld4_next(c, &ra, b[1 + i]));
    for (int i = 0; i < (toff == 2 ? 2 : toff == 1 ? 1 : 0); i++) snprintf(off[i], sizeof off[i], "int(%s)", tld4_next(c, &rb, b[4 + i]));
    if (dc) snprintf(dref, sizeof dref, "as_type<float>(%s)", tld4_next(c, &rb, b[6]));
    char gather[8][700];
    int ngather = toff == 2 ? 4 : 1;
    for (int k = 0; k < ngather; k++) {
        char xy[400], P[600];
        if (toff) {  // offset k: register k / 4's byte pair (k & 3) * 8 ... (x at bit 0 / 16, y at 8 / 24 of its register)
            const char *o = off[toff == 2 ? (k * 2) / 4 : 0];
            int bx = toff == 2 ? ((k * 2) & 3) * 8 : 0;
            snprintf(xy, sizeof xy, "(vec2(%s, %s) + vec2(bitfieldExtract(%s, %d, 6), bitfieldExtract(%s, %d, 6)) / vec2(tsize%d(0).xy))",
                     co[0], co[1], o, bx, o, bx + 8, slot);
        } else {
            snprintf(xy, sizeof xy, "vec2(%s, %s)", co[0], co[1]);
        }
        if (cube && array) snprintf(P, sizeof P, "vec4(%s, %s, %s, %s)", co[0], co[1], co[2], lay);
        else if (cube) snprintf(P, sizeof P, "vec3(%s, %s, %s)", co[0], co[1], co[2]);
        else if (array) snprintf(P, sizeof P, "vec3(%s, %s)", xy, lay);
        else snprintf(P, sizeof P, "%s", xy);
        if (dc) snprintf(gather[k], sizeof gather[k], "textureGather(t%d, %s, %s)", slot, P, dref);
        else snprintf(gather[k], sizeof gather[k], "textureGather(t%d, %s, %d)", slot, P, comp);
    }
    char call[4096];
    if (ngather == 4)
        snprintf(call, sizeof call, "vec4((%s).x, (%s).y, (%s).z, (%s).w)", gather[0], gather[1], gather[2], gather[3]);
    else
        snprintf(call, sizeof call, "%s", gather[0]);
    write_tex_result(c, slot, call, mask, d, RZ, false, false);
}

// TMML (LOD query; per Ryujinx's EmitTmml): coordinates from Ra (the array index first), results in 8.8 fixed point,
// x = the LOD the hardware would use, y = the mip level; z / w read 0.
static void emit_tmml(Ctx *c, uint64_t w) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    uint32_t index = (uint32_t)bits(w, 36, 13), mask = (uint32_t)bits(w, 31, 4);
    int dim = (int)bits(w, 28, 3);
    static const int types[8] = {TEXTYPE_1D, TEXTYPE_1D_ARRAY, TEXTYPE_2D, TEXTYPE_2D_ARRAY, TEXTYPE_3D, -1,
                                 TEXTYPE_CUBE, TEXTYPE_CUBE_ARRAY};
    static const int ncoord[8] = {1, 1, 2, 2, 3, 3, 3, 3};
    if (!c->glsl) { fail(c, "tmml (msl)"); return; }
    if (types[dim] < 0) { fail(c, "tmml 3d array"); return; }
    if (d == RZ) return;
    bool array = dim == 1 || dim == 3 || dim == 7;
    int slot = tex_slot(c, index, types[dim], false, array);
    if (c->fail) return;
    int r = a == RZ ? RZ : a + (array ? 1 : 0);
    char co[3][64];
    for (int i = 0; i < 3; i++) {
        char rr[16];
        snprintf(co[i], sizeof co[i], "as_type<float>(%s)", reg(c, r == RZ ? RZ : r + i, rr));
    }
    char q[300];
    if (c->stage != STAGE_FRAGMENT) strcpy(q, "vec2(0.0f)");  // (no implicit derivatives outside fragment shaders)
    else if (ncoord[dim] == 1) snprintf(q, sizeof q, "textureQueryLod(t%d, %s)", slot, co[0]);
    else if (ncoord[dim] == 2) snprintf(q, sizeof q, "textureQueryLod(t%d, vec2(%s, %s))", slot, co[0], co[1]);
    else snprintf(q, sizeof q, "textureQueryLod(t%d, vec3(%s, %s, %s))", slot, co[0], co[1], co[2]);
    sp(&c->body, "    { vec2 lq = %s;\n", q);
    int out = d;
    for (int comp = 0; comp < 4 && out < RZ; comp++) {
        if (!(mask >> comp & 1)) continue;
        c->reg_used[out] = 1;
        // (the instruction's component order is the reverse of GLSL's)
        if (comp < 2) sp(&c->body, "      r%d = uint(int(lq[%d] * 256.0f));\n", out, comp ^ 1);
        else sp(&c->body, "      r%d = 0u;\n", out);
        out++;
    }
    sp(&c->body, "    }\n");
}

// VMNMX (video min / max of byte / half / word selections, then an accumulate / min / max / merge with Rc; per
// Ryujinx's Vmnmx). Selection: 0-3 u8 bytes, 4-5 u16 halves, 6 u32, | 8 signed.
static const char *vsel(char *o, const char *r, int sel) {
    static const int sh[16] = {0, 8, 16, 24, 0, 16, 0, 0, 0, 8, 16, 24, 0, 16, 0, 0};
    int width = (sel & 7) < 4 ? 8 : (sel & 7) < 6 ? 16 : 32;
    if (width == 32) sprintf(o, "%s", r);
    else if (sel & 8) sprintf(o, "uint(bitfieldExtract(int(%s), %d, %d))", r, sh[sel], width);
    else sprintf(o, "bitfieldExtract(%s, %d, %d)", r, sh[sel], width);
    return o;
}
static void emit_vmnmx(Ctx *c, uint64_t w) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8), cr = (int)bits(w, 39, 8);
    int asel = (int)(bits(w, 48, 1) << 3 | bits(w, 36, 3)), bsel = (int)(bits(w, 49, 1) << 3 | bits(w, 28, 3));
    bool bvideo = bits(w, 50, 1), mn = bits(w, 56, 1), sat = bits(w, 55, 1), dfmt = bits(w, 54, 1);
    int vop = (int)bits(w, 51, 3);
    if (bits(w, 47, 1)) { fail(c, "vmnmx .cc"); return; }
    char sa[300], sb[300], ib[32];
    vsel(sa, R(a), asel);
    if (bvideo) vsel(sb, R(b), bsel);
    else {
        int imm = (int)bits(w, 20, 16);
        if (bsel & 8) imm = (int)(int16_t)imm;
        sprintf(ib, "%uu", (uint32_t)imm);
        strcpy(sb, ib);
    }
    bool as = asel & 8, bs = bsel & 8;
    char res[1200];
    bool rs;
    if (as != bs) {  // signedness differs: max fits a u32, min an s32; the unsigned one past INT_MAX wins
        rs = !mn;
        const char *u = as ? sb : sa;
        if (mn) sprintf(res, "(int(%s) < 0 ? %s : max(%s, %s))", u, u, sa, sb);
        else sprintf(res, "(int(%s) < 0 ? %s : uint(min(int(%s), int(%s))))", u, u, sa, sb);
    } else {
        rs = as;
        if (rs) sprintf(res, "uint(%s(int(%s), int(%s)))", mn ? "max" : "min", sa, sb);
        else sprintf(res, "%s(%s, %s)", mn ? "max" : "min", sa, sb);
    }
    char t2[1400];
    if (sat && dfmt && !rs) { sprintf(t2, "min(%s, 2147483647u)", res); strcpy(res, t2); }
    else if (sat && !dfmt && rs) { sprintf(t2, "uint(max(int(%s), 0))", res); strcpy(res, t2); }
    const char *C = R(cr);
    char t[1600];
    switch (vop) {
    case 4: sprintf(t, "(%s + %s)", res, C); break;
    case 5: sprintf(t, dfmt ? "uint(min(int(%s), int(%s)))" : "min(%s, %s)", res, C); break;
    case 6: sprintf(t, dfmt ? "uint(max(int(%s), int(%s)))" : "max(%s, %s)", res, C); break;
    case 0: sprintf(t, "bitfieldInsert(%s, %s, 16, 16)", C, res); break;
    case 1: sprintf(t, "bitfieldInsert(%s, %s, 0, 16)", C, res); break;
    case 2: sprintf(t, "bitfieldInsert(%s, %s, 0, 8)", C, res); break;
    case 3: sprintf(t, "bitfieldInsert(%s, %s, 16, 8)", C, res); break;
    default: strcpy(t, res); break;
    }
    if (!c->glsl) { fail(c, "vmnmx (msl)"); return; }
    set_reg(c, d, t);
}

// TEX (full form): coordinates from consecutive registers at gpr8, extra parameters at gpr20.
static void emit_tex(Ctx *c, uint64_t w, bool bindless) {
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8), b = (int)bits(w, 20, 8);
    uint32_t index = bindless ? 0 : (uint32_t)bits(w, 36, 13);
    int target = (int)bits(w, 29, 2);
    bool array = bits(w, 28, 1), dc = bits(w, 50, 1), aoffi = bindless ? bits(w, 36, 1) : bits(w, 54, 1);
    uint32_t mask = (uint32_t)bits(w, 31, 4);
    int lod_mode = bindless ? (int)bits(w, 37, 3) : (int)bits(w, 55, 3);  // 0 none, 1 lz, 2 lb, 3 ll, 6 lba, 7 lla
    if (bindless) { fail(c, "bindless tex"); return; }
    int type = target == 0 ? (array ? TEXTYPE_1D_ARRAY : TEXTYPE_1D)
             : target == 1 ? (array ? TEXTYPE_2D_ARRAY : TEXTYPE_2D)
             : target == 2 ? TEXTYPE_3D
                           : (array ? TEXTYPE_CUBE_ARRAY : TEXTYPE_CUBE);
    int ncoord = target == 0 ? 1 : target == 1 ? 2 : 3;
    int slot = tex_slot(c, index, type, dc, array);
    if (c->glsl) {  // same register layout as below
        char cs[3][40], lay[96], lodv[48], offv[96], drv[48], gcall[4096];
        const char *co[3], *layp = NULL, *lodp = NULL, *offp = NULL, *drp = NULL;
        int ar2 = a, p2 = b, lk = 0;
        if (array) { sprintf(lay, "min(uint(r%d), uint(tsize%d(0).z) - 1u)", a, slot); c->reg_used[a] = 1; ar2 = a + 1; layp = lay; }
        for (int i = 0; i < ncoord; i++) { sprintf(cs[i], "as_type<float>(r%d)", ar2 + i); c->reg_used[ar2 + i] = 1; co[i] = cs[i]; }
        if (lod_mode == 2 || lod_mode == 3 || lod_mode == 6 || lod_mode == 7) {
            c->reg_used[p2] = 1;
            sprintf(lodv, "as_type<float>(r%d)", p2);
            lodp = lodv;
            lk = lod_mode == 2 || lod_mode == 6 ? 2 : 1;
            p2++;
        } else if (lod_mode == 1) {
            lk = 1;
            lodp = "0.0f";
        }
        if (aoffi) { c->reg_used[p2] = 1; sprintf(offv, "int2(int(r%d << 28) >> 28, int(r%d << 24) >> 28)", p2, p2); offp = offv; p2++; }
        if (dc) { c->reg_used[p2] = 1; sprintf(drv, "as_type<float>(r%d)", p2); drp = drv; p2++; }
        glsl_sample(c, gcall, slot, co, layp, drp, lk, lodp, offp);
        write_tex_result(c, slot, gcall, mask, d, RZ, false, false);
        return;
    }
    char coords[256] = "", layer[64] = "", tmp[64];
    int ar = a;
    // the hardware clamps the array index to the layer count
    if (array) { sprintf(layer, "min(uint(r%d), t%d.get_array_size() - 1u)", a, slot); c->reg_used[a] = 1; ar = a + 1; }
    for (int i = 0; i < ncoord; i++) {
        sprintf(tmp, "%sas_type<float>(r%d)", i ? ", " : "", ar + i);
        c->reg_used[ar + i] = 1;
        strcat(coords, tmp);
    }
    int pr = b;
    char lod[96] = "", dref[64] = "", off[128] = "";
    if (lod_mode == 2 || lod_mode == 3 || lod_mode == 6 || lod_mode == 7) {
        c->reg_used[pr] = 1;
        sprintf(lod, lod_mode == 2 || lod_mode == 6 ? ", bias(as_type<float>(r%d))" : ", level(as_type<float>(r%d))", pr);
        pr++;
    } else if (lod_mode == 1) {
        strcpy(lod, ", level(0)");
    }
    if (aoffi) {
        c->reg_used[pr] = 1;
        sprintf(off, ", int2(int(r%d << 28) >> 28, int(r%d << 24) >> 28)", pr, pr);
        pr++;
    }
    if (dc) { c->reg_used[pr] = 1; sprintf(dref, "as_type<float>(r%d)", pr); pr++; }
    const char *vec = ncoord == 1 ? "" : ncoord == 2 ? "float2" : "float3";
    char cexpr[420], call[1024];
    if (ncoord == 2 && c->stage != STAGE_VERTEX) { snprintf(cexpr, sizeof cexpr, "uvs(float2(%s), rscale[%d])", coords, slot + 1); c->uses_rscale = true; }
    else snprintf(cexpr, sizeof cexpr, "%s(%s)", vec, coords);
    if (dc)
        sprintf(call, "float4(t%d.sample_compare(s%d, %s%s%s, %s%s%s))", slot, slot, cexpr, array ? ", " : "",
                layer, dref, lod, off);
    else
        sprintf(call, "t%d.sample(s%d, %s%s%s%s%s)", slot, slot, cexpr, array ? ", " : "", layer, lod, off);
    write_tex_result(c, slot, call, mask, d, RZ, false, false);
}

// ------------------------------------------------------------------ instruction translation
// The game's light-culling compute shader (a 30x17 grid of 64-pixel tiles for the tiled lighting of interiors and
// caves) turns a tile's pixel column into a view direction with the screen height alone: x = px * 2 / height -
// aspect, which assumes square pixels (width / height = aspect). With a widened camera the image keeps its 1920x1080
// target, so pixels are no longer square: at 32:9 the tiles right of the screen centre got the lights meant for
// half as far out, and the lighting changed along a hard vertical line where a light's tiles ended (the floor
// right of it lighter, MarthaUkuku at 2/3 of the window). The two column steps (pc 0x198: tile x, 0x1a8: tile x +
// 1) are scaled by aspect * height / width, which is 1 at 16:9: the original shader's result there.
static void light_tiles_fix(Ctx *c, const uint64_t *code, size_t end, uint32_t pc) {
    static const uint64_t sig[][2] = {{0x188, 0x5c68100000970309ull}, {0x198, 0x5c68100000d7030dull},
                                      {0x1a8, 0x5c6810000087030bull}, {0x1b0, 0x5c68100000470303ull}};
    if (c->stage != STAGE_COMPUTE || (pc != 0x198 && pc != 0x1a8) || end <= 0x1b0 / 8) return;
    for (size_t i = 0; i < sizeof sig / sizeof sig[0]; i++)
        if (code[sig[i][0] / 8] != sig[i][1]) return;
    int d = (int)(code[pc / 8] & 0xff);  // (the destination: r13 / r11)
    char a[48], h[48], w[48];
    cbuf_ref(c, 4, 0x1bc, NULL, a);  // camera aspect
    cbuf_ref(c, 4, 0x1c4, NULL, h);  // target height
    cbuf_ref(c, 4, 0x1c0, NULL, w);  // target width
    sp(&c->body, "    r%d = as_type<uint>(as_type<float>(r%d) * (as_type<float>(%s) * as_type<float>(%s) / as_type<float>(%s)));\n", d, d, a, h, w);  // (GLSL: as_type is rewritten)
}

static void emit_insn(Ctx *c, uint64_t w, uint32_t pc) {
    char t[2048], u[2048];
    int d = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    int pr = (int)bits(w, 16, 3);
    bool prn = bits(w, 19, 1);
    uint16_t op13 = (uint16_t)(w >> 51);  // top 13 bits
    // predicate guard. SSY/PBK/PCNT/PRET take no predicate: those bits are zero in the encoding,
    // which would otherwise read as @P0.
    uint64_t top12 = w & 0xfff0000000000000ull;
    if (top12 == 0xe290000000000000ull || top12 == 0xe2a0000000000000ull || top12 == 0xe2b0000000000000ull ||
        top12 == 0xe270000000000000ull) {
        pr = PT;
        prn = false;
    }
    bool guarded = !(pr == PT && !prn);
    if (pr == PT && prn) return;  // @!pt: never executes
    if (guarded) sp(&c->body, "  if (%s) {\n", P(pr, prn));

#define OPM(val, mask) ((w & (uint64_t)(mask)) == (uint64_t)(val))
    if (OPM(0xe300000000000000ull, 0xfff0000000000000ull)) {  // EXIT
        sp(&c->body, c->branchy ? "    exit_now = true; break;\n" : "    break;\n");
    } else if (OPM(0xe330000000000000ull, 0xfff0000000000000ull)) {  // KIL
        c->info->uses_discard = true;
        if (getenv("LARECOMP_NO_DISCARD")) {
        } else if (!c->glsl) sp(&c->body, "    discard_fragment();\n");
        else if (c->stage == STAGE_FRAGMENT) sp(&c->body, "    discard;\n");
        else sp(&c->body, "    /* kil */\n");  // GLSL has discard in fragment shaders only
    } else if (OPM(0xe240000000000000ull, 0xfff0000000000020ull)) {  // BRA
        uint32_t target = norm_target((int64_t)pc + 8 + sbits(w, 20, 24));
        c->uses_branches = true;
        sp(&c->body, "    pc = %uu; continue;\n", target);
    } else if (OPM(0xe290000000000000ull, 0xfff0000000000020ull)) {  // SSY
        c->uses_branches = true;
        sp(&c->body, "    ssy_15 = ssy_14; ssy_14 = ssy_13; ssy_13 = ssy_12; ssy_12 = ssy_11; ssy_11 = ssy_10; ssy_10 = ssy_9; ssy_9 = ssy_8; ssy_8 = ssy_7; ssy_7 = ssy_6; ssy_6 = ssy_5; ssy_5 = ssy_4; ssy_4 = ssy_3; ssy_3 = ssy_2; ssy_2 = ssy_1; ssy_1 = ssy_0; ssy_0 = %uu;\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xe2a0000000000000ull, 0xfff0000000000020ull)) {  // PBK
        c->uses_branches = true;
        sp(&c->body, "    pbk_15 = pbk_14; pbk_14 = pbk_13; pbk_13 = pbk_12; pbk_12 = pbk_11; pbk_11 = pbk_10; pbk_10 = pbk_9; pbk_9 = pbk_8; pbk_8 = pbk_7; pbk_7 = pbk_6; pbk_6 = pbk_5; pbk_5 = pbk_4; pbk_4 = pbk_3; pbk_3 = pbk_2; pbk_2 = pbk_1; pbk_1 = pbk_0; pbk_0 = %uu;\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xe2b0000000000000ull, 0xfff0000000000020ull)) {  // PCNT
        c->uses_branches = true;
        sp(&c->body, "    pcnt_0 = %uu;\n", norm_target((int64_t)pc + 8 + sbits(w, 20, 24)));
    } else if (OPM(0xf0f8000000000000ull, 0xfff8000000000000ull)) {  // SYNC
        sp(&c->body, "    pc = ssy_0; ssy_0 = ssy_1; ssy_1 = ssy_2; ssy_2 = ssy_3; ssy_3 = ssy_4; ssy_4 = ssy_5; ssy_5 = ssy_6; ssy_6 = ssy_7; ssy_7 = ssy_8; ssy_8 = ssy_9; ssy_9 = ssy_10; ssy_10 = ssy_11; ssy_11 = ssy_12; ssy_12 = ssy_13; ssy_13 = ssy_14; ssy_14 = ssy_15; continue;\n");
    } else if (OPM(0xe340000000000000ull, 0xfff0000000000000ull)) {  // BRK
        sp(&c->body, "    pc = pbk_0; pbk_0 = pbk_1; pbk_1 = pbk_2; pbk_2 = pbk_3; pbk_3 = pbk_4; pbk_4 = pbk_5; pbk_5 = pbk_6; pbk_6 = pbk_7; pbk_7 = pbk_8; pbk_8 = pbk_9; pbk_9 = pbk_10; pbk_10 = pbk_11; pbk_11 = pbk_12; pbk_12 = pbk_13; pbk_13 = pbk_14; pbk_14 = pbk_15; continue;\n");
    } else if (OPM(0xe350000000000000ull, 0xfff0000000000000ull)) {  // CONT
        sp(&c->body, "    pc = pcnt_0; continue;\n");
    } else if (OPM(0xf0f0000000000000ull, 0xfff8000000000000ull) || OPM(0x50b0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0xef98000000000000ull, 0xfff8000000000000ull)) {
        // DEPBAR / NOP / MEMBAR
    } else if (OPM(0xe00000000000ff00ull, 0xff0000400000ff00ull) || OPM(0xe000004000000000ull, 0xff00004000000000ull)) {
        // IPA: interpolate fragment input
        bool idx = bits(w, 38, 1);
        uint32_t addr = (uint32_t)bits(w, 28, 10);
        int mode = (int)bits(w, 54, 2);  // 0 pass, 1 multiply, 2 constant, 3 sc
        if (idx) { fail(c, "ipa idx"); }
        if (addr >= 0x80 && addr < 0x280) {
            int at = (addr - 0x80) >> 4;
            if (mode == 2) c->flat_mask |= 1u << at;
        }
        c->ipa_mode = mode;
        c->ipa_mul = (int)bits(w, 20, 8);
        attr_load(c, d, addr, NULL);
        c->ipa_mode = -1;
        if (bits(w, 51, 1) && d != RZ) sp(&c->body, "    r%d = as_type<uint>(saturate(as_type<float>(r%d)));\n", d, d);
    } else if (OPM(0xefd8000000000000ull, 0xfff8000000000000ull)) {  // LD attribute
        int n = 1 + (int)bits(w, 47, 2);
        uint32_t addr = (uint32_t)bits(w, 20, 10);
        int ia = (int)bits(w, 8, 8);
        if (ia != RZ) { fail(c, "indexed ld a[]"); }
        for (int i = 0; i < n; i++) attr_load(c, d + i, addr + 4 * i, NULL);
    } else if (OPM(0xeff0000000000000ull, 0xfff8000000000000ull)) {  // ST attribute
        int n = 1 + (int)bits(w, 47, 2);
        uint32_t addr = (uint32_t)bits(w, 20, 10);
        for (int i = 0; i < n; i++) attr_store(c, d + i, addr + 4 * i);
    } else if (OPM(0xef90000000000000ull, 0xfff8000000000000ull)) {  // LD c[idx][reg + imm]
        int idx = (int)bits(w, 36, 5);
        int64_t off = (int64_t)bits(w, 20, 16);  // unsigned (Ryujinx InstLdc.CbufOffset)
        int sz = (int)bits(w, 48, 3);
        int n = sz == 5 ? 2 : 1;
        char rb[16];
        const char *dyn = a == RZ ? NULL : reg(c, a, rb);
        // LDC.64 R4, c[i][R4 + x]: the address register is also a destination. Read it once before the writes, or
        // the second word comes from c[i][<first word> + x + 4] (fix from letsgo-recomp)
        bool overlap = dyn && n > 1 && a >= d && a < d + n;
        if (overlap) { sp(&c->body, "    { uint ldc_addr = %s;\n", dyn); dyn = "ldc_addr"; }
        for (int i = 0; i < n; i++) {
            char cb[256];
            cbuf_ref(c, idx, off + 4 * i, dyn, cb);
            if (dyn) c->info->cbuf_size[idx] = 0x10000;
            set_reg(c, d + i, cb);
        }
        if (overlap) sp(&c->body, "    }\n");
    } else if (OPM(0xd000000000000000ull, 0xf600000000000000ull)) {  // TEXS
        emit_texs(c, w, false);
    } else if (OPM(0xd200000000000000ull, 0xf600000000000000ull)) {  // TLDS
        emit_texs(c, w, true);
    } else if (OPM(0xc038000000000000ull, 0xfc38000000000000ull)) {  // TEX
        emit_tex(c, w, false);
    } else if (OPM(0xdeb8000000000000ull, 0xfff8000000000000ull)) {  // TEX.B (bindless)
        emit_tex(c, w, true);
    } else if (OPM(0xdf00000000000000ull, 0xffc0000000000000ull) || OPM(0xdf80000000000000ull, 0xffc0000000000000ull)) {
        emit_tld4s(c, w);  // TLD4S (.F16: 0xdf8)
    } else if (OPM(0xc800000000000000ull, 0xfc00000000000000ull)) {  // TLD4 (110010: per Ryujinx's InstTable)
        emit_tld4(c, w);
    } else if (OPM(0xdf58000000000000ull, 0xfff8000000000000ull)) {  // TMML
        emit_tmml(c, w);
    } else if (OPM(0x3a00000000000000ull, 0xfe00000000000000ull)) {  // VMNMX
        emit_vmnmx(c, w);
    } else if (OPM(0xeb20000000000000ull, 0xfff8000000000000ull) || OPM(0xeb50000000000000ull, 0xfff8000000000000ull)) {
        // SUST.B / SURED.B: bindless surface store / reduction - no image-by-handle support yet: dropped, so the rest
        // of the shader still runs
        if (!c->warned_surface) { c->warned_surface = true; sp(&c->body, "    /* bindless surface store dropped */\n"); }
    } else if (OPM(0x5bd0000000000000ull, 0xfff8000000000000ull) || OPM(0x4bd0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x36d0000000000000ull, 0xfef8000000000000ull)) {  // LEA: (±Ra << sh) + B (per Ryujinx's EmitLea)
        if (bits(w, 46, 1) || bits(w, 47, 1)) { fail(c, "lea .x / .cc"); return; }
        int sh = (int)bits(w, 39, 5);
        if (bits(w, 45, 1)) sprintf(t, "((0u - (%s << %du)) + %s)", R(a), sh, B(false));
        else sprintf(t, "((%s << %du) + %s)", R(a), sh, B(false));
        set_reg(c, d, t);
    } else if (OPM(0x5bd8000000000000ull, 0xfff8000000000000ull) || OPM(0x1800000000000000ull, 0xfc00000000000000ull)) {
        // LEA.HI: the high word of (±(Rc:Ra) << sh), + B (per Ryujinx's EmitLeaHi)
        bool cb = (w >> 58) == 0x6;
        int sh = (int)(cb ? bits(w, 51, 5) : bits(w, 28, 5)), rc = (int)bits(w, 39, 8);
        bool neg = cb ? bits(w, 56, 1) : bits(w, 37, 1), x = cb ? bits(w, 57, 1) : bits(w, 38, 1);
        if (x) { fail(c, "lea.hi .x"); return; }
        char bb[128];
        const char *bv = cb ? cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, bb) : R((int)bits(w, 20, 8));
        char hi[400];
        if (sh) sprintf(hi, "((%s >> %du) | (%s << %du))", R(a), 32 - sh, R(rc), sh);
        else strcpy(hi, R(rc));
        if (neg) sprintf(t, "((~%s + ((~(%s << %du)) == 0xFFFFFFFFu ? 1u : 0u)) + %s)", hi, R(a), sh, bv);
        else sprintf(t, "(%s + %s)", hi, bv);
        set_reg(c, d, t);
    } else if (OPM(0xed00000000000000ull, 0xff00000000000000ull)) {  // ATOM (global)
        emit_atom(c, w, false);
    } else if (OPM(0xebf8000000000000ull, 0xfff8000000000000ull)) {  // RED (global)
        emit_atom(c, w, true);
    } else if (OPM(0x50d8000000000000ull, 0xfff8000000000000ull)) {  // VOTE (per Ryujinx's Vote; warps of 32 = NVIDIA subgroups)
        int vp = (int)bits(w, 45, 3), mode = (int)bits(w, 48, 2);
        const char *src = P((int)bits(w, 39, 3), bits(w, 42, 1));
        if (!c->glsl) { fail(c, "vote (msl)"); return; }
        static const char *const fn_w[] = {"wall", "wany", "wequal"}, *const fn_s[] = {"subgroupAll", "subgroupAny", "subgroupAllEqual"};
        const char *const *fn = warp_emu() ? fn_w : fn_s;  // (per 32-lane warp: the prelude's w* functions)
        if (mode > 2) { fail(c, "vote mode %d", mode); return; }
        if (vp != PT) { c->pred_used[vp] = 1; sp(&c->body, "    p%d = %s(%s);\n", vp, fn[mode], src); }
        if (d != RZ) { sprintf(t, warp_emu() ? "wballot(%s)" : "subgroupBallot(%s).x", src); set_reg(c, d, t); }
    } else if (OPM(0xdf48000000000000ull, 0xfff8000000000000ull)) {  // TXQ
        uint32_t index = (uint32_t)bits(w, 36, 13);
        int qt = (int)bits(w, 22, 6);
        uint32_t mask = (uint32_t)bits(w, 31, 4);
        int slot = tex_slot(c, index, -1, false, false);  // the type comes from a sample, if any
        if (qt == 1 && c->glsl) {  // tsize<slot>() is emitted with the slot's final type
            if (c->stage != STAGE_VERTEX) {
                c->uses_rscale = true;
                sp(&c->body, "    { uint lv = %s; uvec4 q = uvec4(uint(floor(float(tsize%d(int(lv)).x) * rscale[%d].z / rscale[%d].x + 0.5f)), "
                   "uint(floor(float(tsize%d(int(lv)).y) * rscale[%d].w / rscale[%d].y + 0.5f)), 1u, uint(textureQueryLevels(t%d)));\n",
                   R(a), slot, slot + 1, slot + 1, slot, slot + 1, slot + 1, slot);
            } else {
                sp(&c->body, "    { uint lv = %s; uvec4 q = uvec4(uint(tsize%d(int(lv)).x), uint(tsize%d(int(lv)).y), 1u, uint(textureQueryLevels(t%d)));\n",
                   R(a), slot, slot, slot);
            }
            int n = 0;
            for (int i = 0; i < 4; i++)
                if (mask & (1u << i)) { c->reg_used[d + n] = 1; sp(&c->body, "      r%d = q[%d];\n", d + n, i); n++; }
            sp(&c->body, "    }\n");
        } else if (qt == 1) {  // dimension
            if (c->stage != STAGE_VERTEX) {  // report the guest size of scaled allocations
                c->uses_rscale = true;
                sp(&c->body, "    { uint lv = %s; uint4 q = uint4(uint(round(float(t%d.get_width(lv)) * rscale[%d].z / rscale[%d].x)), "
                   "uint(round(float(t%d.get_height(lv)) * rscale[%d].w / rscale[%d].y)), 1, t%d.get_num_mip_levels());\n",
                   R(a), slot, slot + 1, slot + 1, slot, slot + 1, slot + 1, slot);
            } else {
                sp(&c->body, "    { uint lv = %s; uint4 q = uint4(t%d.get_width(lv), t%d.get_height(lv), 1, t%d.get_num_mip_levels());\n",
                   R(a), slot, slot, slot);
            }
            int n = 0;
            for (int i = 0; i < 4; i++)
                if (mask & (1u << i)) { c->reg_used[d + n] = 1; sp(&c->body, "      r%d = q[%d];\n", d + n, i); n++; }
            sp(&c->body, "    }\n");
        } else {
            fail(c, "txq type %d", qt);
        }
    } else if (OPM(0x5080000000000000ull, 0xfff8000000000000ull)) {  // MUFU
        int fn = (int)bits(w, 20, 4);
        const char *x = NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1));
        switch (fn) {
        case 0: sprintf(t, "cos(%s)", x); break;
        case 1: sprintf(t, "sin(%s)", x); break;
        case 2: sprintf(t, "exp2(%s)", x); break;
        case 3: sprintf(t, "log2(%s)", x); break;
        case 4: sprintf(t, "(1.0f / %s)", x); break;
        case 5: sprintf(t, "rsqrt(%s)", x); break;
        case 8: sprintf(t, "sqrt(%s)", x); break;
        default: fail(c, "mufu %d", fn); return;
        }
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x5c90000000000000ull, 0xfff8000000000000ull) || OPM(0x4c90000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3890000000000000ull, 0xfef8000000000000ull)) {  // RRO: range reduction (no-op for us)
        const char *x = NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1));
        set_reg(c, d, FRES(x, false));
    } else if (OPM(0x5c58000000000000ull, 0xfff8000000000000ull) || OPM(0x4c58000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3858000000000000ull, 0xfef8000000000000ull)) {  // FADD
        sprintf(t, "(%s + %s)", NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1)), NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1)));
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x0800000000000000ull, 0xfc00000000000000ull)) {  // FADD32I
        sprintf(u, "as_type<float>(0x%08xu)", (uint32_t)bits(w, 20, 32));
        sprintf(t, "(%s + %s)", NA(F(R(a)), bits(w, 56, 1), bits(w, 54, 1)), NA(u, bits(w, 53, 1), bits(w, 57, 1)));
        set_reg(c, d, FRES(t, false));
    } else if (OPM(0x5c68000000000000ull, 0xfff8000000000000ull) || OPM(0x4c68000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3868000000000000ull, 0xfef8000000000000ull)) {  // FMUL
        static const char *const scale[] = {"", " * 0.5f", " * 0.25f", " * 0.125f", " * 8.0f", " * 4.0f", " * 2.0f", ""};
        sprintf(t, "(%s * %s%s)", F(R(a)), NA(F(B(true)), bits(w, 48, 1), false), scale[bits(w, 41, 3)]);
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x1e00000000000000ull, 0xff00000000000000ull)) {  // FMUL32I
        sprintf(t, "(%s * as_type<float>(0x%08xu))", F(R(a)), (uint32_t)bits(w, 20, 32));
        set_reg(c, d, FRES(t, bits(w, 55, 1)));
    } else if (OPM(0x5980000000000000ull, 0xff80000000000000ull) || OPM(0x4980000000000000ull, 0xff80000000000000ull) ||
               OPM(0x5180000000000000ull, 0xff80000000000000ull) || OPM(0x3280000000000000ull, 0xfe80000000000000ull)) {  // FFMA
        const char *bv;
        char cbuf_c[256], bbuf[256];
        const char *cv = operand_c_for_ffma(c, w, cbuf_c, &bv, bbuf);
        sprintf(t, "fma(%s, %s, %s)", F(R(a)), NA(F(bv), bits(w, 48, 1), false), NA(F(cv), bits(w, 49, 1), false));
        set_reg(c, d, FRES(t, bits(w, 50, 1)));
    } else if (OPM(0x0c00000000000000ull, 0xfc00000000000000ull)) {  // FFMA32I
        sprintf(u, "as_type<float>(0x%08xu)", (uint32_t)bits(w, 20, 32));
        sprintf(t, "fma(%s, %s, %s)", NA(F(R(a)), bits(w, 56, 1), false), u, NA(F(R(d)), bits(w, 57, 1), false));
        set_reg(c, d, FRES(t, bits(w, 55, 1)));
    } else if (OPM(0x5c60000000000000ull, 0xfff8000000000000ull) || OPM(0x4c60000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3860000000000000ull, 0xfef8000000000000ull)) {  // FMNMX
        const char *x = NA(F(R(a)), bits(w, 48, 1), bits(w, 46, 1)), *y = NA(F(B(true)), bits(w, 45, 1), bits(w, 49, 1));
        sprintf(t, "(%s ? fmin(%s, %s) : fmax(%s, %s))", P((int)bits(w, 39, 3), bits(w, 42, 1)), x, y, x, y);
        set_reg(c, d, FRES(t, false));
    } else if (OPM(0x5bb0000000000000ull, 0xfff0000000000000ull) || OPM(0x4bb0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36b0000000000000ull, 0xfef0000000000000ull)) {  // FSETP
        const char *x = NA(F(R(a)), bits(w, 43, 1), bits(w, 7, 1)), *y = NA(F(B(true)), bits(w, 6, 1), bits(w, 44, 1));
        char cmp[512];
        fcmp_expr(cmp, (int)bits(w, 48, 4), x, y);
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pc_ = P((int)bits(w, 39, 3), bits(w, 42, 1));
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n      p%d = cmp %s pp;\n", cmp, pc_, p, bool_op(bo)); }
        else sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cmp, pc_);
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5800000000000000ull, 0xff00000000000000ull) || OPM(0x4800000000000000ull, 0xfe00000000000000ull) ||
               OPM(0x3000000000000000ull, 0xfe00000000000000ull)) {  // FSET
        const char *x = NA(F(R(a)), bits(w, 43, 1), bits(w, 54, 1)), *y = NA(F(B(true)), bits(w, 53, 1), bits(w, 44, 1));
        char cmp[512];
        fcmp_expr(cmp, (int)bits(w, 48, 4), x, y);
        sprintf(t, "((%s) %s %s)", cmp, bool_op((int)bits(w, 45, 2)), P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 52, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        set_reg(c, d, u);
    } else if (OPM(0x5ba0000000000000ull, 0xfff0000000000000ull) || OPM(0x4ba0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36a0000000000000ull, 0xfef0000000000000ull) || OPM(0x53a0000000000000ull, 0xfff0000000000000ull)) {  // FCMP
        char cmp[512], cb[256], bb[256];
        const char *bv, *cv;
        if (OPM(0x53a0000000000000ull, 0xfff0000000000000ull)) {
            bv = R((int)bits(w, 39, 8));
            cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
        } else {
            bv = operand_b(c, w, true, bb);
            cv = R((int)bits(w, 39, 8));
        }
        fcmp_expr(cmp, (int)bits(w, 48, 4), F(cv), "0.0f");
        sprintf(t, "(%s ? %s : %s)", cmp, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c98000000000000ull, 0xfff8000000000000ull) || OPM(0x4c98000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3898000000000000ull, 0xfef8000000000000ull)) {  // MOV
        set_reg(c, d, B(false));
    } else if (OPM(0x0100000000000000ull, 0xfff0000000000000ull)) {  // MOV32I
        sprintf(t, "0x%08xu", (uint32_t)bits(w, 20, 32));
        set_reg(c, d, t);
    } else if (OPM(0x5ca0000000000000ull, 0xfff8000000000000ull) || OPM(0x4ca0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38a0000000000000ull, 0xfef8000000000000ull)) {  // SEL
        sprintf(t, "(%s ? %s : %s)", P((int)bits(w, 39, 3), bits(w, 42, 1)), R(a), B(false));
        set_reg(c, d, t);
    } else if (OPM(0x5cb0000000000000ull, 0xfff8000000000000ull) || OPM(0x4cb0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38b0000000000000ull, 0xfef8000000000000ull)) {  // F2I (src fmt bits 10-11: 1 F16; dst size bits 8-9)
        char srcv[512];
        if (bits(w, 10, 2) == 1 && c->glsl) sprintf(srcv, "h2f((%s >> %du) & 0xFFFFu)", B(false), 16 * (int)bits(w, 41, 1));
        else if (bits(w, 10, 2) == 1) sprintf(srcv, "float(as_type<half2>(%s)[%d])", B(false), (int)bits(w, 41, 1));
        else strcpy(srcv, F(B(true)));
        const char *x = NA(srcv, bits(w, 45, 1), bits(w, 49, 1));
        int rnd = (int)bits(w, 39, 2);
        bool sgn = bits(w, 12, 1), i16 = bits(w, 8, 2) == 1;
        static const char *const rf[] = {"rint", "floor", "ceil", "trunc"};
        if (i16)
            sprintf(t, sgn ? "uint(int(clamp(%s(%s), -32768.0f, 32767.0f)))" : "uint(clamp(%s(%s), 0.0f, 65535.0f))", rf[rnd], x);
        else
            sprintf(t, sgn ? "uint(int(clamp(%s(%s), -2147483648.0f, 2147483520.0f)))" : "uint(clamp(%s(%s), 0.0f, 4294967040.0f))", rf[rnd], x);
        set_reg(c, d, t);
    } else if (OPM(0x5cb8000000000000ull, 0xfff8000000000000ull) || OPM(0x4cb8000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38b8000000000000ull, 0xfef8000000000000ull)) {  // I2F
        bool sgn = bits(w, 13, 1);
        int sz = (int)bits(w, 10, 2), sel = (int)bits(w, 41, 2);
        char src[256];
        const char *bv = B(false);
        if (sz == 0) sprintf(src, sgn ? "int(char((%s >> %d) & 0xFF))" : "((%s >> %d) & 0xFFu)", bv, sel * 8);
        else if (sz == 1) sprintf(src, sgn ? "int(short((%s >> %d) & 0xFFFF))" : "((%s >> %d) & 0xFFFFu)", bv, sel * 8);
        else sprintf(src, sgn ? "int(%s)" : "%s", bv);
        sprintf(u, "float(%s)", src);
        if (bits(w, 8, 2) == 1) {  // F16 result in the low half
            char h[700];
            snprintf(h, sizeof h, c->glsl ? "(packHalf2x16(vec2(%s, 0.0f)))" : "(uint(as_type<ushort>(half(%s))))",
                     NA(u, bits(w, 45, 1), bits(w, 49, 1)));
            set_reg(c, d, h);
        } else {
            set_reg(c, d, FRES(NA(u, bits(w, 45, 1), bits(w, 49, 1)), false));
        }
    } else if (OPM(0x5ca8000000000000ull, 0xfff8000000000000ull) || OPM(0x4ca8000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38a8000000000000ull, 0xfef8000000000000ull)) {  // F2F (formats: 1 F16, 2 F32; bit 41 = high half)
        int dfmt = (int)bits(w, 8, 2), sfmt = (int)bits(w, 10, 2);
        char srcv[512];
        if (sfmt == 1 && c->glsl) sprintf(srcv, "h2f((%s >> %du) & 0xFFFFu)", B(false), 16 * (int)bits(w, 41, 1));
        else if (sfmt == 1) sprintf(srcv, "float(as_type<half2>(%s)[%d])", B(false), (int)bits(w, 41, 1));
        else strcpy(srcv, F(B(true)));
        const char *x = NA(srcv, bits(w, 45, 1), bits(w, 49, 1));
        int mode = (int)((bits(w, 39, 2)) | (bits(w, 42, 1) << 2));
        switch (mode) {
        case 4: sprintf(t, "rint(%s)", x); break;
        case 5: sprintf(t, "floor(%s)", x); break;
        case 6: sprintf(t, "ceil(%s)", x); break;
        case 7: sprintf(t, "trunc(%s)", x); break;
        default: sprintf(t, "%s", x);
        }
        if (dfmt == 1) {  // F16 result in the low half, high half zero
            char h[1100];
            snprintf(h, sizeof h, c->glsl ? "(packHalf2x16(vec2(%s, 0.0f)))" : "(uint(as_type<ushort>(half(%s))))",
                     bits(w, 50, 1) ? (sprintf(u, "saturate(%s)", t), u) : t);
            set_reg(c, d, h);
        } else {
            set_reg(c, d, FRES(t, bits(w, 50, 1)));
        }
    } else if (OPM(0x5c10000000000000ull, 0xfff8000000000000ull) || OPM(0x4c10000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3810000000000000ull, 0xfef8000000000000ull)) {  // IADD
        if (bits(w, 43, 1) || bits(w, 47, 1)) c->uses_cc = true;
        sprintf(t, "(%s%s + %s%s%s)", bits(w, 49, 1) ? "-" : "", R(a), bits(w, 48, 1) ? "-" : "", B(false),
                bits(w, 43, 1) ? " + cc" : "");
        if (bits(w, 47, 1)) {
            sp(&c->body, "    { uint s = %s%s; uint t2 = %s%s; uint res = s + t2%s; cc = (res < s) ? 1u : 0u;\n", bits(w, 49, 1) ? "-" : "",
               R(a), bits(w, 48, 1) ? "-" : "", B(false), bits(w, 43, 1) ? " + cc" : "");
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
            sp(&c->body, "    }\n");
        } else {
            set_reg(c, d, t);
        }
    } else if (OPM(0x1c00000000000000ull, 0xfe00000000000000ull)) {  // IADD32I (CC bit 52, X bit 53)
        bool wcc = bits(w, 52, 1), x = bits(w, 53, 1);
        if (wcc || x) c->uses_cc = true;
        sp(&c->body, "    { ulong sum = ulong(%s%s) + ulong(0x%08xu)%s;%s\n", bits(w, 56, 1) ? "-" : "", R(a), (uint32_t)bits(w, 20, 32),
           x ? " + ulong(cc)" : "", wcc ? " cc = uint(sum >> 32) & 1u;" : "");
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = uint(sum);\n", d); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5cc0000000000000ull, 0xfff0000000000000ull) || OPM(0x4cc0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x38c0000000000000ull, 0xfef0000000000000ull)) {  // IADD3
        bool wcc3 = bits(w, 47, 1), x3 = bits(w, 48, 1);
        if (wcc3 || x3) {  // carry in/out (64-bit address math)
            c->uses_cc = true;
            sp(&c->body, "    { ulong sum = ulong(%s%s) + ulong(%s%s) + ulong(%s%s)%s;\n", bits(w, 51, 1) ? "-" : "", R(a),
               bits(w, 50, 1) ? "-" : "", B(false), bits(w, 49, 1) ? "-" : "", R((int)bits(w, 39, 8)), x3 ? " + ulong(cc)" : "");
            if (wcc3) sp(&c->body, "      cc = uint(sum >> 32) & 1u;\n");
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = uint(sum);\n", d); }
            sp(&c->body, "    }\n");
        } else {
            sprintf(t, "(%s%s + %s%s + %s%s)", bits(w, 51, 1) ? "-" : "", R(a), bits(w, 50, 1) ? "-" : "", B(false),
                    bits(w, 49, 1) ? "-" : "", R((int)bits(w, 39, 8)));
            set_reg(c, d, t);
        }
    } else if (OPM(0x5c18000000000000ull, 0xfff8000000000000ull) || OPM(0x4c18000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3818000000000000ull, 0xfef8000000000000ull)) {  // ISCADD
        sprintf(t, "((%s%s << %du) + %s%s)", bits(w, 49, 1) ? "-" : "", R(a), (int)bits(w, 39, 5), bits(w, 48, 1) ? "-" : "",
                B(false));
        if (bits(w, 47, 1)) {  // .CC: carry out for a following IADD.X (64-bit address math)
            c->uses_cc = true;
            sp(&c->body, "    { uint s = %s%s << %du; uint t2 = %s%s; uint res = s + t2; cc = (res < s) ? 1u : 0u;\n",
               bits(w, 49, 1) ? "-" : "", R(a), (int)bits(w, 39, 5), bits(w, 48, 1) ? "-" : "", B(false));
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
            sp(&c->body, "    }\n");
        } else {
            set_reg(c, d, t);
        }
    } else if (OPM(0x1400000000000000ull, 0xfc00000000000000ull)) {  // ISCADD32I
        sprintf(t, "((%s << %du) + 0x%08xu)", R(a), (int)bits(w, 53, 5), (uint32_t)bits(w, 20, 32));
        set_reg(c, d, t);
    } else if (OPM(0x5b60000000000000ull, 0xfff0000000000000ull) || OPM(0x4b60000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3660000000000000ull, 0xfef0000000000000ull)) {  // ISETP
        char cmp[512];
        icmp_expr(cmp, (int)bits(w, 49, 3), R(a), B(false), bits(w, 48, 1));
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pp = P((int)bits(w, 39, 3), bits(w, 42, 1));
        sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cmp, pp);
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "      p%d = cmp %s pp;\n", p, bool_op(bo)); }
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5b50000000000000ull, 0xfff0000000000000ull) || OPM(0x4b50000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3650000000000000ull, 0xfef0000000000000ull)) {  // ISET
        char cmp[512];
        icmp_expr(cmp, (int)bits(w, 49, 3), R(a), B(false), bits(w, 48, 1));
        sprintf(t, "((%s) %s %s)", cmp, bool_op((int)bits(w, 45, 2)), P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 44, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        set_reg(c, d, u);
    } else if (OPM(0x5c20000000000000ull, 0xfff8000000000000ull) || OPM(0x4c20000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3820000000000000ull, 0xfef8000000000000ull)) {  // IMNMX
        bool sgn = bits(w, 48, 1);
        const char *x = R(a), *y = B(false), *p = P((int)bits(w, 39, 3), bits(w, 42, 1));
        if (sgn) sprintf(t, "uint(%s ? min(int(%s), int(%s)) : max(int(%s), int(%s)))", p, x, y, x, y);
        else sprintf(t, "(%s ? min(%s, %s) : max(%s, %s))", p, x, y, x, y);
        set_reg(c, d, t);
    } else if (OPM(0x5c48000000000000ull, 0xfff8000000000000ull) || OPM(0x4c48000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3848000000000000ull, 0xfef8000000000000ull)) {  // SHL
        const char *bv = B(false);
        if (bits(w, 39, 1)) sprintf(t, "(%s << (%s & 31u))", R(a), bv);
        else sprintf(t, "((%s) >= 32u ? 0u : (%s << (%s)))", bv, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c28000000000000ull, 0xfff8000000000000ull) || OPM(0x4c28000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3828000000000000ull, 0xfef8000000000000ull)) {  // SHR
        const char *bv = B(false);
        bool sgn = bits(w, 48, 1);
        if (sgn) sprintf(t, "uint(int(%s) >> min(%s, 31u))", R(a), bv);
        else sprintf(t, "((%s) >= 32u ? 0u : (%s >> (%s)))", bv, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c40000000000000ull, 0xfff8000000000000ull) || OPM(0x4c40000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3840000000000000ull, 0xfef8000000000000ull)) {  // LOP
        char x[64], y[256];
        sprintf(x, bits(w, 39, 1) ? "(~%s)" : "%s", R(a));
        sprintf(y, bits(w, 40, 1) ? "(~%s)" : "%s", B(false));
        static const char *const ops[] = {"&", "|", "^", ""};
        int op = (int)bits(w, 41, 2);
        if (op == 3) sprintf(t, "%s", y);
        else sprintf(t, "(%s %s %s)", x, ops[op], y);
        int pd = (int)bits(w, 48, 3);
        if (pd != PT) {
            c->pred_used[pd] = 1;
            int pop = (int)bits(w, 44, 2);
            sp(&c->body, "    { uint res = %s;\n", t);
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
            sp(&c->body, "      p%d = %s; }\n", pd, pop == 2 ? "(res == 0u)" : pop == 3 ? "(res != 0u)" : pop == 1 ? "true" : "false");
        } else {
            set_reg(c, d, t);
        }
    } else if (OPM(0x0400000000000000ull, 0xfc00000000000000ull)) {  // LOP32I
        char x[64], y[64];
        sprintf(x, bits(w, 55, 1) ? "(~%s)" : "%s", R(a));
        sprintf(y, bits(w, 56, 1) ? "(~0x%08xu)" : "0x%08xu", (uint32_t)bits(w, 20, 32));
        static const char *const ops[] = {"&", "|", "^", ""};
        int op = (int)bits(w, 53, 2);
        if (op == 3) sprintf(t, "%s", y);
        else sprintf(t, "(%s %s %s)", x, ops[op], y);
        set_reg(c, d, t);
    } else if (OPM(0x5be0000000000000ull, 0xfff8000000000000ull) || OPM(0x3c00000000000000ull, 0xfc00000000000000ull) ||
               OPM(0x0200000000000000ull, 0xfe00000000000000ull)) {  // LOP3
        uint32_t lut;
        const char *bv, *cv;
        char bb[256];
        if ((w >> 56) == 0x5b) { lut = (uint32_t)bits(w, 28, 8); bv = R((int)bits(w, 20, 8)); cv = R((int)bits(w, 39, 8)); }
        else if ((w >> 58) == 0xf) { lut = (uint32_t)bits(w, 48, 8); sprintf(bb, "0x%08xu", (uint32_t)((uint32_t)bits(w, 20, 19) | ((uint32_t)bits(w, 56, 1) << 19) | (bits(w, 56, 1) ? 0xFFF00000u : 0))); bv = bb; cv = R((int)bits(w, 39, 8)); }
        else { lut = (uint32_t)bits(w, 48, 8); bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, bb); cv = R((int)bits(w, 39, 8)); }
        char e[1024] = "0u";
        // sum of minterms
        int first = 1;
        for (int i = 0; i < 8; i++) {
            if (!(lut & (1u << i))) continue;
            char term[256];
            sprintf(term, "(%s%s & %s%s & %s%s)", (i & 4) ? "" : "~", R(a), (i & 2) ? "" : "~", bv, (i & 1) ? "" : "~", cv);
            if (first) { strcpy(e, term); first = 0; }
            else { strcat(e, " | "); strcat(e, term); }
        }
        sprintf(t, "(%s)", e);
        set_reg(c, d, t);
    } else if (OPM(0x5c00000000000000ull, 0xfff8000000000000ull) || OPM(0x4c00000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3800000000000000ull, 0xfef8000000000000ull)) {  // BFE
        const char *bv = B(false);
        bool sgn = bits(w, 48, 1);
        if (bits(w, 40, 1)) fail(c, "bfe brev");
        sprintf(t, sgn ? "bfe_s(%s, %s)" : "bfe_u(%s, %s)", R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0x5bf0000000000000ull, 0xfff8000000000000ull) || OPM(0x4bf0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x36f0000000000000ull, 0xfef8000000000000ull) || OPM(0x53f0000000000000ull, 0xfff8000000000000ull)) {  // BFI
        const char *bv, *cv;
        char cb[256];
        if (OPM(0x53f0000000000000ull, 0xfff8000000000000ull)) {
            bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            sprintf(t, "bfi(%s, %s, %s)", R(a), cv, bv);  // insert a into base (cbuf) with b = position/length
            // bfi with cbuf form: B = REG_39 as position/length, C = cbuf as base
            sprintf(t, "bfi(%s, %s, %s)", R(a), bv, cv);
        } else {
            bv = B(false); cv = R((int)bits(w, 39, 8));
            sprintf(t, "bfi(%s, %s, %s)", R(a), bv, cv);
        }
        set_reg(c, d, t);
    } else if (OPM(0x5b00000000000000ull, 0xffc0000000000000ull) || OPM(0x4e00000000000000ull, 0xfe00000000000000ull) ||
               OPM(0x3600000000000000ull, 0xfec0000000000000ull) || OPM(0x5100000000000000ull, 0xff80000000000000ull)) {  // XMAD
        // per Ryujinx's InstXmad* decoders and EmitXmad
        const char *bv, *cv;
        char cb[256], bb[256];
        bool hb, psl, mrg, ext, ha = bits(w, 53, 1);
        int cmode;
        uint64_t top = w >> 56;
        if (top == 0x5b) {  // reg, reg
            bv = R((int)bits(w, 20, 8)); cv = R((int)bits(w, 39, 8));
            hb = bits(w, 35, 1); psl = bits(w, 36, 1); mrg = bits(w, 37, 1); ext = bits(w, 38, 1); cmode = (int)bits(w, 50, 3);
        } else if ((top & 0xfe) == 0x4e) {  // cbuf B
            bv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb); cv = R((int)bits(w, 39, 8));
            hb = bits(w, 52, 1); psl = bits(w, 55, 1); mrg = bits(w, 56, 1); ext = bits(w, 54, 1); cmode = (int)bits(w, 50, 2);
        } else if ((top & 0xfe) == 0x36) {  // imm16 B
            sprintf(bb, "0x%xu", (uint32_t)bits(w, 20, 16)); bv = bb; cv = R((int)bits(w, 39, 8));
            hb = false; psl = bits(w, 36, 1); mrg = bits(w, 37, 1); ext = bits(w, 38, 1); cmode = (int)bits(w, 50, 3);
        } else {  // 0x51: B = reg39, C = cbuf
            bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
            hb = bits(w, 52, 1); psl = false; mrg = false; ext = bits(w, 54, 1); cmode = (int)bits(w, 50, 2);
        }
        bool sa = bits(w, 48, 1), sb = bits(w, 49, 1), wcc = bits(w, 47, 1);
        if (ext || wcc) c->uses_cc = true;
        sp(&c->body, "    { uint xa = (%s >> %d) & 0xFFFFu; uint xb = (%s >> %d) & 0xFFFFu; uint xbu = %s;\n", R(a), ha ? 16 : 0, bv, hb ? 16 : 0, bv);
        if (sa) sp(&c->body, "      xa = uint(int(short(xa)));\n");
        if (sb) sp(&c->body, "      xb = uint(int(short(xb)));\n");
        sp(&c->body, "      uint prod = xa * xb;%s\n", psl ? " prod <<= 16;" : "");
        switch (cmode) {
        case 1: sp(&c->body, "      uint cc_ = %s & 0xFFFFu;\n", cv); break;                                  // clo
        case 2: sp(&c->body, "      uint cc_ = %s >> 16;\n", cv); break;                                      // chi
        case 3: sp(&c->body, "      uint cc_ = %s - (((xa >> 31) << 16) + ((xb >> 31) << 16));\n", cv); break;  // csfu
        case 4: sp(&c->body, "      uint cc_ = %s + (xbu << 16);\n", cv); break;                              // cbcc
        default: sp(&c->body, "      uint cc_ = %s;\n", cv);                                                  // cfull
        }
        if (ext) sp(&c->body, "      uint res = prod + (cc & 1u);\n");
        else sp(&c->body, "      uint res = prod + cc_;\n");
        if (wcc) sp(&c->body, "      cc = (res < prod) ? 1u : 0u;\n");
        if (mrg) sp(&c->body, "      res = (res & 0xFFFFu) | (xbu << 16);\n");
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = res;\n", d); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5090000000000000ull, 0xfff8000000000000ull)) {  // PSETP
        const char *pa = P((int)bits(w, 12, 3), bits(w, 15, 1)), *pb = P((int)bits(w, 29, 3), bits(w, 32, 1));
        const char *pcc = P((int)bits(w, 39, 3), bits(w, 42, 1));
        int bo1 = (int)bits(w, 24, 2), bo2 = (int)bits(w, 45, 2);
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3);
        sp(&c->body, "    { bool v = (%s %s %s); bool pp = %s;\n", pa, bool_op(bo1), pb, pcc);
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "      p%d = v %s pp;\n", p, bool_op(bo2)); }
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !v %s pp;\n", q, bool_op(bo2)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5088000000000000ull, 0xfff8000000000000ull)) {  // PSET
        const char *pa = P((int)bits(w, 12, 3), bits(w, 15, 1)), *pb = P((int)bits(w, 29, 3), bits(w, 32, 1));
        sprintf(t, "(((%s %s %s)) %s %s)", pa, bool_op((int)bits(w, 24, 2)), pb, bool_op((int)bits(w, 45, 2)),
                P((int)bits(w, 39, 3), bits(w, 42, 1)));
        sprintf(u, bits(w, 44, 1) ? "(%s ? 0x3f800000u : 0u)" : "(%s ? 0xFFFFFFFFu : 0u)", t);
        set_reg(c, d, u);
    } else if (OPM(0xf0c8000000000000ull, 0xfff8000000000000ull)) {  // S2R
        int sr = (int)bits(w, 20, 8);
        switch (sr) {
        case 0x12: set_reg(c, d, "0xbf800000u"); break;  // y_direction: -1 (lower-left)
        case 0x21: set_reg(c, d, "tid.x"); break;
        case 0x22: set_reg(c, d, "tid.y"); break;
        case 0x23: set_reg(c, d, "tid.z"); break;
        case 0x25: set_reg(c, d, "ctaid.x"); break;
        case 0x26: set_reg(c, d, "ctaid.y"); break;
        case 0x27: set_reg(c, d, "ctaid.z"); break;
        // SR_LANEID and the lane masks (eq, lt, le, gt, ge: per Ryujinx's register numbering) - warp-aggregated atomics
        // (ballot, a leader lane adds the popcount: Pokémon Z-A's culling counts instances so). (MSL vertex functions
        // have no SIMD-group index: lane 0 there.)
        case 0x00: case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: {
            static const char *const fmt[] = {"lane", "(1u << lane)", "((1u << lane) - 1u)", "((2u << lane) - 1u)",
                                              "(~((2u << lane) - 1u))", "(~((1u << lane) - 1u))"};
            int k = sr == 0 ? 0 : sr - 0x37;
            // (LARECOMP_LANE_COMPUTE_ONLY=1, investigation: graphics stages get lane 0, as before vertex SHFL was real)
            static int compute_only = -1;
            if (compute_only < 0) compute_only = getenv("LARECOMP_LANE_COMPUTE_ONLY") != NULL;
            if ((!c->glsl && c->stage == STAGE_VERTEX) || (compute_only && c->stage != STAGE_COMPUTE)) {
                static const char *const v0[] = {"0u", "1u", "0u", "1u", "0xFFFFFFFEu", "0xFFFFFFFFu"};
                set_reg(c, d, v0[k]);
            } else {
                c->uses_lane = true;
                set_reg(c, d, fmt[k]);
            }
            break;
        }
        default: sprintf(t, "0u /* sr %#x */", sr); set_reg(c, d, t);
        }
    } else if (OPM(0x5c30000000000000ull, 0xfff8000000000000ull) || OPM(0x4c30000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3830000000000000ull, 0xfef8000000000000ull)) {  // FLO
        const char *bv = B(false);
        sprintf(t, bits(w, 48, 1) ? "(int(%s) < 0 ? (31u - clz(~%s)) : (31u - clz(%s)))" : "(31u - clz(%s))", bv, bv, bv);
        set_reg(c, d, t);
    } else if (OPM(0x5c08000000000000ull, 0xfff8000000000000ull) || OPM(0x4c08000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3808000000000000ull, 0xfef8000000000000ull)) {  // POPC
        sprintf(t, bits(w, 40, 1) ? "popcount(~%s)" : "popcount(%s)", B(false));
        set_reg(c, d, t);
    } else if (OPM(0x5c38000000000000ull, 0xfff8000000000000ull) || OPM(0x4c38000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x3838000000000000ull, 0xfef8000000000000ull)) {  // IMUL
        if (bits(w, 39, 1)) sprintf(t, bits(w, 40, 1) ? "uint(mulhi(int(%s), int(%s)))" : "mulhi(%s, %s)", R(a), B(false));
        else sprintf(t, "(%s * %s)", R(a), B(false));
        set_reg(c, d, t);
    } else if (OPM(0x1f00000000000000ull, 0xff00000000000000ull)) {  // IMUL32I
        sprintf(t, "(%s * 0x%08xu)", R(a), (uint32_t)bits(w, 20, 32));
        set_reg(c, d, t);
    } else if (OPM(0x5ce0000000000000ull, 0xfff8000000000000ull) || OPM(0x4ce0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x38e0000000000000ull, 0xfef8000000000000ull)) {  // I2I
        int dt = (int)((bits(w, 8, 2)) | (bits(w, 12, 1) << 2)), st = (int)((bits(w, 10, 2)) | (bits(w, 13, 1) << 2));
        int sel = (int)bits(w, 41, 2);
        char src[256];
        const char *bv = B(false);
        int ssz = st & 3;
        bool ssg = st & 4;
        if (ssz == 0) sprintf(src, ssg ? "int(char((%s >> %d) & 0xFFu))" : "int((%s >> %d) & 0xFFu)", bv, sel * 8);
        else if (ssz == 1) sprintf(src, ssg ? "int(short((%s >> %d) & 0xFFFFu))" : "int((%s >> %d) & 0xFFFFu)", bv, sel * 8);
        else sprintf(src, "int(%s)", bv);
        if (bits(w, 49, 1)) { sprintf(u, "abs(%s)", src); strcpy(src, u); }
        if (bits(w, 45, 1)) { sprintf(u, "(-%s)", src); strcpy(src, u); }
        int dsz = dt & 3;
        bool dsg = dt & 4, sat = bits(w, 50, 1);
        if (sat && !ssg) {  // .SAT from an unsigned source: compare unsigned (a U32 value above 0x7FFFFFFF isn't negative)
            static const char *const hi[2][3] = {{"0xFFu", "0xFFFFu", ""}, {"0x7Fu", "0x7FFFu", "0x7FFFFFFFu"}};
            if (dsz < 2 || dsg) sprintf(t, "min(uint(%s), %s)", src, hi[dsg][dsz < 2 ? dsz : 2]);
            else sprintf(t, "uint(%s)", src);
        } else if (dsz == 0) sprintf(t, sat ? (dsg ? "uint(clamp(%s, -128, 127))" : "uint(clamp(%s, 0, 255))") : "(uint(%s) & 0xFFu)", src);
        else if (dsz == 1) sprintf(t, sat ? (dsg ? "uint(clamp(%s, -32768, 32767))" : "uint(clamp(%s, 0, 65535))") : "(uint(%s) & 0xFFFFu)", src);
        else if (sat && !dsg) sprintf(t, "uint(max(%s, 0))", src);  // S32 -> U32 .SAT: negatives become 0
        else sprintf(t, "uint(%s)", src);
        set_reg(c, d, t);
        if (bits(w, 47, 1)) {  // .CC: zero/sign flags of the result
            c->uses_cc = true;
            sp(&c->body, "    { uint ccv = %s; ccz = ccv == 0u; ccs = int(ccv) < 0; }\n", t);
        }
    } else if (OPM(0x5bc0000000000000ull, 0xfff0000000000000ull) || OPM(0x4bc0000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x36c0000000000000ull, 0xfef0000000000000ull) || OPM(0x53c0000000000000ull, 0xfff0000000000000ull)) {  // PRMT
        const char *bv, *cv;
        char cb[256];
        if (OPM(0x53c0000000000000ull, 0xfff0000000000000ull)) { bv = R((int)bits(w, 39, 8)); cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb); }
        else { bv = B(false); cv = R((int)bits(w, 39, 8)); }
        if (bits(w, 48, 4)) fail(c, "prmt mode");
        sprintf(t, "prmt(%s, %s, %s)", R(a), cv, bv);
        set_reg(c, d, t);
    } else if (OPM(0x5cf8000000000000ull, 0xfff8000000000000ull) || OPM(0x38f8000000000000ull, 0xfef8000000000000ull) ||
               OPM(0x5bf8000000000000ull, 0xfff8000000000000ull) || OPM(0x36f8000000000000ull, 0xfef8000000000000ull)) {  // SHF
        // funnel shift of hi:lo (C:A). .W (bit 50) wraps the shift amount, otherwise it's clamped to the width;
        // bits 37-38: 2 .U64 / 3 .S64 shift the full 64 bits (width 64), else 32. The immediate is 6 bits (20-25:
        // bits 37-38 of the 19-bit B immediate are the width field). .CC / .X / .HI (47-49) aren't modelled.
        bool right = (w >> 56) == 0x5c || (w >> 57) == (0x38 >> 1);
        if (bits(w, 47, 3)) { fail(c, "shf cc/x/hi"); return; }
        char shb[16];
        const char *sh = ((w >> 60) == 3) ? (sprintf(shb, "%uu", (unsigned)bits(w, 20, 6)), shb) : R((int)bits(w, 20, 8));
        const char *hi = R((int)bits(w, 39, 8));
        int width = bits(w, 38, 1) ? 64 : 32;
        bool s64 = bits(w, 37, 2) == 3, wrap = bits(w, 50, 1);
        sp(&c->body, wrap ? "    { uint s = %s & %du; ulong v = (ulong(%s) << 32) | ulong(%s);\n"
                          : "    { uint s = min(%s, %du); ulong v = (ulong(%s) << 32) | ulong(%s);\n",
           sh, wrap ? width - 1 : width, hi, R(a));
        if (right && width == 64)
            sprintf(t, "s >= 64u ? %s : uint(%s >> s)", s64 ? "uint(int(v >> 32) >> 31)" : "0u", s64 ? "long(v)" : "v");
        else if (right) sprintf(t, "uint(v >> s)");
        else if (width == 64) sprintf(t, "s >= 64u ? 0u : uint((v << s) >> 32)");
        else sprintf(t, "uint((v << s) >> 32)");
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = %s;\n", d, t); }
        sp(&c->body, "    }\n");
    } else if (OPM(0xeed0000000000000ull, 0xfff8000000000000ull) || OPM(0xeed8000000000000ull, 0xfff8000000000000ull)) {  // LDG / STG
        bool store = bits(w, 51, 1);
        bool wide = bits(w, 45, 1);
        int size = (int)bits(w, 48, 3);
        int64_t off = sbits(w, 20, 24);
        c->info->uses_gmem = true;
        char addr[256];
        if (a == RZ) sprintf(addr, "ulong(%lld)", (long long)off);
        else if (wide) sprintf(addr, "((ulong(%s) << 32 | ulong(%s)) + ulong(%lld))", R(a + 1), R(a), (long long)off);
        else sprintf(addr, "(ulong(%s) + ulong(%lld))", R(a), (long long)off);
        sp(&c->body, "    { ulong ga = %s - 0x%llxul; bool ok = ga < 0x%llxul;\n", addr,
           (unsigned long long)maxwell_gmem_base, (unsigned long long)maxwell_gmem_size);
        static const char *const ld_small[] = {"uint(gmem[ga])", "uint(int(char(gmem[ga])))",
                                               "uint(*(device ushort *)(gmem + ga))", "uint(int(*(device short *)(gmem + ga)))"};
        int nw = size == 5 ? 2 : size >= 6 ? 4 : 1;
        bool lazy_wide = c->glsl && maxwell_gmem_chunk_shift && maxwell_gmem_lazy && nw > 1;  // (one chunk check per access)
        if (c->glsl && c->stage == STAGE_COMPUTE) {
            // Compute (Vulkan): the guest heap is host memory across PCIe on a discrete GPU, and a shader storing to
            // it in a loop crawls (the little chain chomps in Madam MeowMeow's house: ~4 ms a dispatch, ~0.07 ms without
            // the heap stores). The dispatch's storage buffers are copied to a device-local buffer (gfast_, address
            // table gmap: exec.c gfast_prepare) and back after it: loads and stores inside them use that copy.
            // Anything outside the table goes to the heap as before.
            c->info->uses_gmem_map = true;
            static const char *const gl_small[] = {
                "(%s >> (uint(ga & 3ul) * 8u)) & 0xFFu", "uint(sx8(%s >> (uint(ga & 3ul) * 8u)))",
                "(%s >> (uint(ga & 2ul) * 8u)) & 0xFFFFu", "uint(sx16(%s >> (uint(ga & 2ul) * 8u)))"};
            sp(&c->body, "      uint fw = ok ? gfind(uint(ga)) : 0xFFFFFFFFu;\n");
            if (!store) {
                if (size < 4) {
                    if (d != RZ) {
                        char fast[160], slow[160];
                        snprintf(fast, sizeof fast, gl_small[size], "gfast_.w[fw]");
                        snprintf(slow, sizeof slow, gl_small[size], "GMR(uint(ga >> 2))");
                        c->reg_used[d] = 1;
                        sp(&c->body, "      r%d = fw != 0xFFFFFFFFu ? %s : ok ? %s : 0u;\n", d, fast, slow);
                    }
                } else if (lazy_wide) {
                    sp(&c->body, "      { uvec%d t = fw != 0xFFFFFFFFu ? uvec%d(", nw, nw);
                    for (int k = 0; k < nw; k++) sp(&c->body, "%sgfast_.w[fw + %du]", k ? ", " : "", k);
                    sp(&c->body, ") : ok ? GMR%d(uint(ga >> 2)) : uvec%d(0u);\n", nw, nw);
                    for (int k = 0; k < nw; k++)
                        if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "        r%d = t[%d];\n", d + k, k); }
                    sp(&c->body, "      }\n");
                } else {
                    for (int k = 0; k < nw; k++)
                        if (d != RZ && d + k < RZ) {
                            c->reg_used[d + k] = 1;
                            sp(&c->body, "      r%d = fw != 0xFFFFFFFFu ? gfast_.w[fw + %du] : ok ? GMR(uint(ga >> 2) + %du) : 0u;\n",
                               d + k, k, k);
                        }
                }
            } else {
                c->uses_gmem_store = true;
                c->info->uses_gmem_store = true;
                if (size < 4) {
                    uint32_t m = size < 2 ? 0xFFu : 0xFFFFu;
                    const char *bm = size < 2 ? "3ul" : "2ul";
                    sp(&c->body, "      if (ok) { uint sh = uint(ga & %s) * 8u; uint v = (%s & 0x%xu) << sh;\n", bm, R(d), m);
                    sp(&c->body, "        if (fw != 0xFFFFFFFFu) { atomicAnd(gfast_.w[fw], ~(0x%xu << sh)); atomicOr(gfast_.w[fw], v); }\n", m);
                    sp(&c->body, "        else { GMAND(uint(ga >> 2), ~(0x%xu << sh)); GMOR(uint(ga >> 2), v); } }\n", m);
                } else if (lazy_wide) {
                    for (int k = 0; k < nw; k++) sp(&c->body, "      if (fw != 0xFFFFFFFFu) gfast_.w[fw + %du] = %s;\n", k, R(d == RZ ? RZ : d + k));
                    sp(&c->body, "      if (fw == 0xFFFFFFFFu && ok) GMW%d(uint(ga >> 2), uvec%d(", nw, nw);
                    for (int k = 0; k < nw; k++) sp(&c->body, "%s%s", k ? ", " : "", R(d == RZ ? RZ : d + k));
                    sp(&c->body, "));\n");
                } else {
                    for (int k = 0; k < nw; k++)
                        sp(&c->body, "      if (fw != 0xFFFFFFFFu) gfast_.w[fw + %du] = %s; else if (ok) GMW(uint(ga >> 2) + %du, %s);\n", k,
                           R(d == RZ ? RZ : d + k), k, R(d == RZ ? RZ : d + k));
                }
            }
        } else if (c->glsl) {  // 32-bit words (GMR / GMW / GMAND / GMOR: the prelude); sub-word accesses within the containing word
            static const char *const gl_small[] = {
                "(GMR(uint(ga >> 2)) >> (uint(ga & 3ul) * 8u)) & 0xFFu",
                "uint(sx8(GMR(uint(ga >> 2)) >> (uint(ga & 3ul) * 8u)))",
                "(GMR(uint(ga >> 2)) >> (uint(ga & 2ul) * 8u)) & 0xFFFFu",
                "uint(sx16(GMR(uint(ga >> 2)) >> (uint(ga & 2ul) * 8u)))"};
            if (!store) {
                if (size < 4) {
                    if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      if (ok) r%d = %s; else r%d = 0u;\n", d, gl_small[size], d); }
                } else if (lazy_wide) {  // (one bounds check and address for the whole aligned access)
                    sp(&c->body, "      { uvec%d t = ok ? GMR%d(uint(ga >> 2)) : uvec%d(0u);\n", nw, nw, nw);
                    for (int k = 0; k < nw; k++)
                        if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "        r%d = t[%d];\n", d + k, k); }
                    sp(&c->body, "      }\n");
                } else {
                    for (int k = 0; k < nw; k++)
                        if (d != RZ && d + k < RZ) {
                            c->reg_used[d + k] = 1;
                            sp(&c->body, "      if (ok) r%d = GMR(uint(ga >> 2) + %du); else r%d = 0u;\n", d + k, k, d + k);
                        }
                }
            } else {
                c->uses_gmem_store = true;
                c->info->uses_gmem_store = true;  // (the draw marks the pages it stores to: vertex cache copies go stale)
                if (size < 4)
                    sp(&c->body, "      if (ok) { uint wi = uint(ga >> 2); uint sh = uint(ga & %s) * 8u; GMAND(wi, ~(0x%xu << sh)); "
                       "GMOR(wi, (%s & 0x%xu) << sh); }\n", size < 2 ? "3ul" : "2ul", size < 2 ? 0xFFu : 0xFFFFu, R(d),
                       size < 2 ? 0xFFu : 0xFFFFu);
                else if (lazy_wide) {
                    sp(&c->body, "      if (ok) GMW%d(uint(ga >> 2), uvec%d(", nw, nw);
                    for (int k = 0; k < nw; k++) sp(&c->body, "%s%s", k ? ", " : "", R(d == RZ ? RZ : d + k));
                    sp(&c->body, "));\n");
                } else
                    for (int k = 0; k < nw; k++)
                        sp(&c->body, "      if (ok) GMW(uint(ga >> 2) + %du, %s);\n", k, R(d == RZ ? RZ : d + k));
            }
        } else if (!store) {
            if (size < 4) {
                if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = ok ? %s : 0u;\n", d, ld_small[size]); }
            } else {
                for (int k = 0; k < nw; k++)
                    if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "      r%d = ok ? *(device uint *)(gmem + ga + %du) : 0u;\n", d + k, k * 4); }
            }
        } else {
            if (size == 0 || size == 1) sp(&c->body, "      if (ok) gmem[ga] = uchar(%s);\n", R(d));
            else if (size == 2 || size == 3) sp(&c->body, "      if (ok) *(device ushort *)(gmem + ga) = ushort(%s);\n", R(d));
            else
                for (int k = 0; k < nw; k++)
                    sp(&c->body, "      if (ok) *(device uint *)(gmem + ga + %du) = %s;\n", k * 4, R(d == RZ ? RZ : d + k));
        }
        sp(&c->body, "    }\n");
    } else if (emit_half(c, w)) {
    } else if (OPM(0x50f8000000000000ull, 0xfff8000000000000ull)) {  // FSWZADD
        // per quad lane (laneid & 3), 2 bits of the swizzle pick: a+b, b-a, a-b, b
        uint32_t swz = (uint32_t)bits(w, 28, 8);
        if (c->stage != STAGE_FRAGMENT) {
            sprintf(t, "as_type<uint>(%s + %s)", F(R(a)), F(R((int)bits(w, 20, 8))));
        } else {
            c->uses_lane = true;
            sprintf(t, "as_type<uint>(fswzadd(%s, %s, 0x%02xu, lane))", F(R(a)), F(R((int)bits(w, 20, 8))), swz);
        }
        set_reg(c, d, t);
    } else if (OPM(0x5b40000000000000ull, 0xfff0000000000000ull) || OPM(0x4b40000000000000ull, 0xfff0000000000000ull) ||
               OPM(0x3640000000000000ull, 0xfef0000000000000ull) || OPM(0x5340000000000000ull, 0xfff0000000000000ull)) {  // ICMP
        char cmp[512], cb[256], bb[256];
        const char *bv, *cv;
        if (OPM(0x5340000000000000ull, 0xfff0000000000000ull)) {
            bv = R((int)bits(w, 39, 8));
            cv = cbuf_ref(c, (int)bits(w, 34, 5), bits(w, 20, 14) * 4, NULL, cb);
        } else {
            bv = operand_b(c, w, false, bb);
            cv = R((int)bits(w, 39, 8));
        }
        icmp_expr(cmp, (int)bits(w, 49, 3), cv, "0u", bits(w, 48, 1));
        sprintf(t, "(%s ? %s : %s)", cmp, R(a), bv);
        set_reg(c, d, t);
    } else if (OPM(0xef40000000000000ull, 0xfff8000000000000ull) || OPM(0xef50000000000000ull, 0xfff8000000000000ull)) {  // LDL / STL
        // thread-private local memory, modelled as a word array (byte/short accesses within a word)
        bool store = bits(w, 52, 1);
        int size = (int)bits(w, 48, 3);
        int32_t off = (int32_t)sbits(w, 20, 24);
        c->uses_local = true;
        char addr[128];
        if (a == RZ) sprintf(addr, "%du", off);
        else sprintf(addr, "(%s + %du)", R(a), (uint32_t)off);
        int nw = size == 5 ? 2 : size >= 6 ? 4 : 1;
        sp(&c->body, "    { uint la = %s;\n", addr);
        if (!store) {
            if (size < 4) {
                static const char *const ext[] = {"(v >> sh) & 0xFFu", "uint(int(char((v >> sh) & 0xFFu)))", "(v >> sh) & 0xFFFFu",
                                                  "uint(int(short((v >> sh) & 0xFFFFu)))"};
                if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      uint v = lmem[(la >> 2) & %du]; uint sh = (la & 3u) * 8u; r%d = %s;\n", LMEM_WORDS - 1, d, ext[size]); }
            } else {
                for (int k = 0; k < nw; k++)
                    if (d != RZ && d + k < RZ) { c->reg_used[d + k] = 1; sp(&c->body, "      r%d = lmem[((la >> 2) + %du) & %du];\n", d + k, k, LMEM_WORDS - 1); }
            }
        } else {
            if (size < 4) {
                uint32_t m = size < 2 ? 0xFFu : 0xFFFFu;
                sp(&c->body, "      uint sh = (la & 3u) * 8u; uint i = (la >> 2) & %du; lmem[i] = (lmem[i] & ~(0x%xu << sh)) | ((%s & 0x%xu) << sh);\n",
                   LMEM_WORDS - 1, m, R(d), m);
            } else {
                for (int k = 0; k < nw; k++)
                    sp(&c->body, "      lmem[((la >> 2) + %du) & %du] = %s;\n", k, LMEM_WORDS - 1, R(d == RZ ? RZ : d + k));
            }
        }
        sp(&c->body, "    }\n");
    } else if (OPM(0xef10000000000000ull, 0xfff8000000000000ull)) {  // SHFL
        int mode = (int)bits(w, 30, 2);
        char bb[32];
        const char *bv = bits(w, 28, 1) ? (sprintf(bb, "%uu", (unsigned)bits(w, 20, 5)), bb) : R((int)bits(w, 20, 8));
        int pd = (int)bits(w, 48, 3);
        if (c->stage == STAGE_VERTEX && !c->glsl) {
            set_reg(c, d, R(a));  // no SIMD-group functions in MSL vertex functions: lane-local value
        } else if (c->glsl) {  // (all stages: Vulkan subgroups. A quad xor - lane mask < 4 - is the subgroup xor in the quad)
            static const char *const fn_w[] = {"wshfl", "wup", "wdown", "wxor"},
                              *const fn_s[] = {"subgroupShuffle", "subgroupShuffleUp", "subgroupShuffleDown", "subgroupShuffleXor"};
            const char *const *fn = warp_emu() ? fn_w : fn_s;  // (per 32-lane warp: the prelude's w* functions)
            sprintf(t, "%s(%s, uint(%s))", fn[mode], R(a), bv);
            set_reg(c, d, t);
        } else if (mode == 3) {
            bool quad = bits(w, 28, 1) && bits(w, 20, 5) < 4;
            sprintf(t, quad ? "quad_shuffle_xor(%s, ushort(%s))" : "simd_shuffle_xor(%s, ushort(%s))", R(a), bv);
            set_reg(c, d, t);
        } else {
            static const char *const fn[] = {"simd_shuffle", "simd_shuffle_up", "simd_shuffle_down"};
            sprintf(t, "%s(%s, ushort(%s))", fn[mode], R(a), bv);
            set_reg(c, d, t);
        }
        if (pd != PT) { c->pred_used[pd] = 1; sp(&c->body, "    p%d = true;\n", pd); }
    } else if (OPM(0x38f0000000000000ull, 0xfef8000000000000ull) || OPM(0x5cf0000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x4cf0000000000000ull, 0xfff8000000000000ull)) {  // R2P
        if (bits(w, 40, 1)) { fail(c, "r2p cc"); }
        int shift = (int)bits(w, 41, 2) * 8;
        const char *mask = B(false);
        for (int i = 0; i < 7; i++) {
            c->pred_used[i] = 1;
            sp(&c->body, c->glsl ? "    if (((%s >> %du) & 1u) != 0u) p%d = ((%s >> %du) & 1u) != 0u;\n"
                                 : "    if ((%s >> %du) & 1u) p%d = ((%s >> %du) & 1u) != 0u;\n", mask, i, i, R(a), shift + i);
        }
    } else if (OPM(0x38e8000000000000ull, 0xfef8000000000000ull) || OPM(0x5ce8000000000000ull, 0xfff8000000000000ull) ||
               OPM(0x4ce8000000000000ull, 0xfff8000000000000ull)) {  // P2R
        int shift = (int)bits(w, 41, 2) * 8;
        const char *mask = B(false);
        if (bits(w, 40, 1)) {  // .CC: the flags Z, S, C, O as bits 0-3 (overflow isn't tracked: 0)
            c->uses_cc = true;
            sp(&c->body, "    { uint pr = (ccz ? 1u : 0u) | (ccs ? 2u : 0u) | ((cc & 1u) << 2);\n");
            sp(&c->body, "      uint m = (%s & 0xFu) << %du;\n", mask, shift);
            if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = (%s & ~m) | ((pr << %du) & m);\n", d, R(a), shift); }
            sp(&c->body, "    }\n");
            goto p2r_done;
        }
        sp(&c->body, "    { uint pr = 0u;\n");
        for (int i = 0; i < 7; i++) { c->pred_used[i] = 1; sp(&c->body, "      pr |= (p%d ? 1u : 0u) << %du;\n", i, i); }
        sp(&c->body, "      uint m = (%s & 0x7Fu) << %du;\n", mask, shift);
        if (d != RZ) { c->reg_used[d] = 1; sp(&c->body, "      r%d = (%s & ~m) | ((pr << %du) & m);\n", d, R(a), shift); }
        sp(&c->body, "    }\n");
    p2r_done:;
    } else if (OPM(0xe250000000000000ull, 0xfff0000000000020ull)) {  // BRX reg + imm (relative)
        // computed targets can name a scheduling word; execution continues at the next slot (as for BRA)
        sp(&c->body, "    { uint t = %uu + %s + 0x%08xu; if (((t >> 3) & 3u) == 0u) t += 8u; pc = t; continue; }\n", pc + 8, R(a),
           (uint32_t)sbits(w, 20, 24));
    } else if (OPM(0x50a0000000000000ull, 0xfff8000000000000ull)) {  // CSETP (zero/sign flags only)
        int test = (int)bits(w, 8, 5);
        const char *cond;
        switch (test) {
        case 0: cond = "false"; break;
        case 15: cond = "true"; break;
        case 2: case 10: cond = "ccz"; break;
        case 5: case 13: cond = "!ccz"; break;
        case 1: case 9: cond = "ccs"; break;
        case 6: case 14: cond = "!ccs"; break;
        case 4: case 12: cond = "(!ccz && !ccs)"; break;
        case 3: case 11: cond = "(ccz || ccs)"; break;
        default: cond = "false"; fail(c, "csetp test %d", test);
        }
        c->uses_cc = true;
        int p = (int)bits(w, 3, 3), q = (int)bits(w, 0, 3), bo = (int)bits(w, 45, 2);
        const char *pc_ = P((int)bits(w, 39, 3), bits(w, 42, 1));
        sp(&c->body, "    { bool cmp = %s; bool pp = %s;\n", cond, pc_);
        if (p != PT) { c->pred_used[p] = 1; sp(&c->body, "      p%d = cmp %s pp;\n", p, bool_op(bo)); }
        if (q != PT) { c->pred_used[q] = 1; sp(&c->body, "      p%d = !cmp %s pp;\n", q, bool_op(bo)); }
        sp(&c->body, "    }\n");
    } else if (OPM(0x5098000000000000ull, 0xfff8000000000000ull)) {  // CSET
        fail(c, "cset");
    } else if (OPM(0xf0a8000000000000ull, 0xfff8000000000000ull)) {  // BAR
        if (!c->glsl) sp(&c->body, "    threadgroup_barrier(mem_flags::mem_threadgroup);\n");
        else if (c->stage == STAGE_COMPUTE) sp(&c->body, "    barrier();\n");
        else sp(&c->body, "    /* bar */\n");  // GLSL barrier() is compute / tessellation only
    } else {
        fail(c, "unknown instruction %016llx at %#x", (unsigned long long)w, pc);
    }
    (void)op13;
    if (guarded) sp(&c->body, "  }\n");
}

// ------------------------------------------------------------------ program assembly
static bool is_branch_target_op(uint64_t w) {
    return ((w & 0xfff0000000000020ull) == 0xe240000000000000ull);
}

// ------------------------------------------------------------------ GLSL output
static bool id_start(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; }
static bool id_char(char ch) { return id_start(ch) || (ch >= '0' && ch <= '9'); }

// MSL-spelled instruction body -> GLSL (token-aware; comments and numbers are copied as they are).
static void glsl_rewrite(Ctx *c, Str *o, const char *src) {
    static const char *const map[][2] = {
        {"float2", "vec2"}, {"float3", "vec3"}, {"float4", "vec4"}, {"int2", "ivec2"}, {"int3", "ivec3"},
        {"int4", "ivec4"}, {"uint2", "uvec2"}, {"uint3", "uvec3"}, {"uint4", "uvec4"}, {"bool2", "bvec2"},
        {"ulong", "uint64_t"}, {"long", "int64_t"}, {"rsqrt", "inversesqrt"}, {"rint", "roundEven"},
        {"front_facing", "gl_FrontFacing"}, {"point_coord", "gl_PointCoord"}, {"lane", "wlane()"},
        {"vid", "uint(gl_VertexIndex)"}, {"iid", "uint(gl_InstanceIndex)"}, {"rt_layer", "uint(gl_Layer)"},
    };
    const char *p = src;
    while (*p) {
        if (p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            e = e ? e + 2 : p + strlen(p);
            sp(o, "%.*s", (int)(e - p), p);
            p = e;
            continue;
        }
        if (p[0] == '/' && p[1] == '/') {
            const char *e = strchr(p, '\n');
            if (!e) e = p + strlen(p);
            sp(o, "%.*s", (int)(e - p), p);
            p = e;
            continue;
        }
        if (*p >= '0' && *p <= '9') {  // number (0x..u, 1.0f, ...)
            const char *e = p;
            while (id_char(*e) || *e == '.') e++;
            sp(o, "%.*s", (int)(e - p), p);
            p = e;
            continue;
        }
        if (!id_start(*p)) { sp(o, "%c", *p); p++; continue; }
        const char *e = p;
        while (id_char(*e)) e++;
        int n = (int)(e - p);
        char id[64];
        if (n >= (int)sizeof id || (p > src && p[-1] == '.')) {  // member / swizzle
            sp(o, "%.*s", n, p);
            p = e;
            continue;
        }
        memcpy(id, p, n);
        id[n] = 0;
        p = e;
        if (!strcmp(id, "as_type") && *p == '<') {
            const char *gt = strchr(p, '>');
            if (!gt) { fail(c, "glsl: bad as_type"); return; }
            int tn = (int)(gt - p - 1);
            if (tn == 5 && !strncmp(p + 1, "float", 5)) sp(o, "as_f");
            else if (tn == 4 && !strncmp(p + 1, "uint", 4)) sp(o, "as_u");
            else if (tn == 3 && !strncmp(p + 1, "int", 3)) sp(o, "as_i");
            else if (tn == 5 && !strncmp(p + 1, "half2", 5)) sp(o, "unpack_h2");
            else { fail(c, "glsl: as_type<%.*s>", tn, p + 1); return; }
            p = gt + 1;
            continue;
        }
        if ((!strcmp(id, "in") || !strcmp(id, "out")) && *p == '.') {  // stage I/O struct members
            const char *m = p + 1, *me = m;
            while (id_char(*me)) me++;
            char mem[32];
            snprintf(mem, sizeof mem, "%.*s", (int)(me - m), m);
            if (!strcmp(mem, "pos")) sp(o, c->stage == STAGE_FRAGMENT ? "gl_FragCoord" : "gl_Position");
            else if (!strcmp(mem, "psize")) sp(o, "gl_PointSize");
            else if (!strcmp(mem, "layer")) sp(o, "gl_Layer");
            else sp(o, "%s", mem);  // a<i> (vertex input), v<i> (varying)
            p = me;
            continue;
        }
        if ((!strcmp(id, "char") || !strcmp(id, "short")) && *p == '(') { sp(o, id[0] == 'c' ? "sx8" : "sx16"); continue; }
        if (!strcmp(id, "tid") || !strcmp(id, "ctaid")) {
            sp(o, c->stage != STAGE_COMPUTE ? "uvec3(0u)" : id[0] == 't' ? "gl_LocalInvocationID" : "gl_WorkGroupID");
            continue;
        }
        if (id[0] == 'r' && n > 1 && n < 6) {  // registers beyond r255 (RZ) come from out-of-range encodings
            bool num = true;
            for (int i = 1; i < n; i++) num = num && id[i] >= '0' && id[i] <= '9';
            if (num && atoi(id + 1) > 255) { fail(c, "register %s out of range", id); return; }
        }
        const char *rep = id;
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (!strcmp(id, map[i][0])) { rep = map[i][1]; break; }
        if (!strcmp(id, "lane") && !warp_emu()) rep = "gl_SubgroupInvocationID";
        sp(o, "%s", rep);
    }
}

static const char glsl_prelude[] =
    "#version 460\n"
    "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require\n"
    "#extension GL_KHR_shader_subgroup_basic : require\n"
    "#extension GL_KHR_shader_subgroup_shuffle : require\n"
    "#extension GL_KHR_shader_subgroup_shuffle_relative : require\n"
    "#extension GL_KHR_shader_subgroup_vote : require\n"
    "#extension GL_KHR_shader_subgroup_ballot : require\n"
    "#extension GL_EXT_texture_shadow_lod : require\n"
    "#extension GL_ARB_shader_viewport_layer_array : enable\n"
    "#define cbr(cb, off) cb[min((off) >> 4, uint(cb.length() - 1))][((off) >> 2) & 3u]\n"
    "uint as_u(float x) { return floatBitsToUint(x); }\n"
    "vec4 stc(uvec4 v) { return uintBitsToFloat(v); }\n"
    "vec4 itc(ivec4 v) { return intBitsToFloat(v); }\n"
    "uint as_u(uint x) { return x; }\n"
    "uint as_u(int x) { return uint(x); }\n"
    "float as_f(uint x) { return uintBitsToFloat(x); }\n"
    "float as_f(int x) { return intBitsToFloat(x); }\n"
    "float as_f(float x) { return x; }\n"
    "int as_i(uint x) { return int(x); }\n"
    "int as_i(float x) { return floatBitsToInt(x); }\n"
    "int sx8(uint v) { return bitfieldExtract(int(v), 0, 8); }\n"
    "int sx8(int v) { return bitfieldExtract(v, 0, 8); }\n"
    "int sx16(uint v) { return bitfieldExtract(int(v), 0, 16); }\n"
    "int sx16(int v) { return bitfieldExtract(v, 0, 16); }\n"
    // MSL semantics: saturate(NaN) = 0, fmin/fmax return the non-NaN operand
    "float saturate(float x) { return isnan(x) ? 0.0f : clamp(x, 0.0f, 1.0f); }\n"
    "vec2 saturate(vec2 x) { return vec2(saturate(x.x), saturate(x.y)); }\n"
    "float fmin(float a, float b) { return isnan(a) ? b : (isnan(b) ? a : min(a, b)); }\n"
    "float fmax(float a, float b) { return isnan(a) ? b : (isnan(b) ? a : max(a, b)); }\n"
    // Maxwell's warps are 32 lanes. A subgroup of 64 (AMD's GCN / Vega GPUs always, any GPU whose driver picks it) holds
    // two of them: lane, ballot, vote and shuffle work on each 32-lane half as on a warp. gl_SubgroupSize is a
    // constant to the driver's compiler: with 32-lane subgroups only the plain subgroup operation is left.
    "bool w32() { return gl_SubgroupSize == 32u; }\n"
    "uint wlane() { return w32() ? gl_SubgroupInvocationID : gl_SubgroupInvocationID & 31u; }\n"
    "uint wbase() { return gl_SubgroupInvocationID & ~31u; }\n"
    "uint wballot(bool p) { uvec4 b = subgroupBallot(p); return w32() ? b.x : b[gl_SubgroupInvocationID >> 5]; }\n"
    "bool wall(bool p) { return w32() ? subgroupAll(p) : wballot(!p) == 0u; }\n"
    "bool wany(bool p) { return w32() ? subgroupAny(p) : wballot(p) != 0u; }\n"
    "bool wequal(bool p) { if (w32()) return subgroupAllEqual(p); uint b = wballot(p); return b == 0u || b == wballot(true); }\n"
    "uint wshfl(uint v, uint i) { return subgroupShuffle(v, w32() ? i : wbase() | (i & 31u)); }\n"
    "uint wup(uint v, uint n) { if (w32()) return subgroupShuffleUp(v, n); uint l = wlane(), s = subgroupShuffle(v, wbase() | ((l - n) & 31u)); return l < n ? v : s; }\n"
    "uint wdown(uint v, uint n) { if (w32()) return subgroupShuffleDown(v, n); uint l = wlane(), s = subgroupShuffle(v, wbase() | ((l + n) & 31u)); return l + n > 31u ? v : s; }\n"
    "uint wxor(uint v, uint m) { return w32() ? subgroupShuffleXor(v, m) : subgroupShuffle(v, gl_SubgroupInvocationID ^ (m & 31u)); }\n"
    "uint clz(uint v) { return v == 0u ? 32u : uint(31 - findMSB(v)); }\n"
    "uint popcount(uint v) { return uint(bitCount(v)); }\n"
    "uint mulhi(uint a, uint b) { uint hi, lo; umulExtended(a, b, hi, lo); return hi; }\n"
    "int mulhi(int a, int b) { int hi, lo; imulExtended(a, b, hi, lo); return hi; }\n"
    "uint bfe_u(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return 0u; return bitfieldExtract(v, int(pos), int(min(len, 32u - pos))); }\n"
    "uint bfe_s(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return (int(v) < 0) ? 0xFFFFFFFFu : 0u; return uint(bitfieldExtract(int(v), int(pos), int(min(len, 32u - pos)))); }\n"
    "uint bfi(uint ins, uint ctl, uint base) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u || pos >= 32u) return base; return bitfieldInsert(base, ins, int(pos), int(min(len, 32u - pos))); }\n"
    "float fswzadd(float a, float b, uint swz, uint lane) { uint m = (swz >> ((lane & 3u) * 2u)) & 3u; float ma = m == 1u ? -1.0f : (m == 3u ? 0.0f : 1.0f); float mb = m == 2u ? -1.0f : 1.0f; return a * ma + b * mb; }\n"
    "uvec2 rsc(uvec2 c, vec4 s) { return (s.x == 1.0f && s.y == 1.0f) ? c : uvec2(floor((vec2(c) + 0.5f) * s.xy)); }\n"
    // a scaled texture whose contents occupy its top-left part (Dynamic resolution): sample that part
    "vec2 uvs(vec2 uv, vec4 s) { return (s.z == 1.0f && s.w == 1.0f) ? uv : min(uv, 1.0f) * s.zw; }\n"
    // A fragment shader's texel fetch from a texture at its render target's scale (r = rscale[0]: the target's scale,
    // offset; p = the pixel's host position): the s x s host pixels of one guest pixel each read their own texel of
    // the guest texel, not all the same one. Deferred lighting reads its G-buffer at the pixel's own guest coordinate,
    // so with rsc() every 2 x 2 block at 2x came out one colour (the scene at 1x, pixel-doubled).
    "uvec2 rscf(uvec2 c, vec4 s, vec4 r, vec2 p) { if (s.x == 1.0f && s.y == 1.0f) return c;"
    " if (any(notEqual(s.xy, r.xy))) return uvec2(floor((vec2(c) + 0.5f) * s.xy));"
    " return uvec2(floor(vec2(c) * s.xy) + floor(fract((p - r.zw) / r.xy) * s.xy)); }\n";
// The half <-> float helpers (glsl_finish: glsl_h2f_exact or glsl_h2f_native, then glsl_prelude_half).
// Half -> float in vertex (and other pre-raster) shaders: by bits, not unpackHalf2x16. NVIDIA's driver recognises
// unpacked halves and runs some of the fp32 math after them in native fp16, differently per shader, so a depth
// pre-pass and its colour pass (EQUAL test) placed wind-animated foliage apart: black slashes / flicker (notes/PLAN.md
// 2026-10-08).
static const char glsl_h2f_exact[] =
    "float h2f(uint h) { uint s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;"
    " if (e == 0u) { float d = float(m) * 5.9604644775390625e-8; return s != 0u ? -d : d; }"
    " if (e == 31u) return uintBitsToFloat(s | 0x7F800000u | (m << 13));"
    " return uintBitsToFloat(s | ((e + 112u) << 23) | (m << 13)); }\n"
    "vec2 unpack_h2(uint v) { return vec2(h2f(v & 0xFFFFu), h2f(v >> 16)); }\n";
// ... and in fragment / compute shaders the native conversion: positions don't depend on them, and the by-bits
// version (~12 integer ops and two branches per half) was half of the main lighting pass's GPU time on an Intel UHD
// 770 and a third on an RTX 3080 Ti (the game's shaders use packed halves heavily). LARECOMP_VK_EXACT_H2F=1: by bits
// in every stage.
static const char glsl_h2f_native[] =
    "float h2f(uint h) { return unpackHalf2x16(h).x; }\n"
    "vec2 unpack_h2(uint v) { return unpackHalf2x16(v); }\n";
static const char glsl_prelude_half[] =
    "vec2 hx(uint v, uint swz) { if (swz == 1u) return vec2(uintBitsToFloat(v)); vec2 h = unpack_h2(v); return swz == 2u ? h.xx : (swz == 3u ? h.yy : h); }\n"
    "uint hpack(vec2 r, uint merge, uint old) { if (merge == 1u) return floatBitsToUint(r.x); uint lo = packHalf2x16(vec2(r.x, 0.0f)), hi = packHalf2x16(vec2(r.y, 0.0f)); if (merge == 2u) return (old & 0xFFFF0000u) | lo; if (merge == 3u) return (old & 0xFFFFu) | (hi << 16); return lo | (hi << 16); }\n"
    "uint prmt(uint a, uint b, uint sel) { uint r = 0u; for (uint i = 0u; i < 4u; i++) { uint s = (sel >> (i * 4u)) & 7u; uint bt = s < 4u ? (a >> (s * 8u)) & 0xFFu : (b >> ((s - 4u) * 8u)) & 0xFFu; r |= bt << (i * 8u); } return r; }\n";

static const char *glsl_sampler_type(const ShaderTexture *t) {
    static const char *const names[][2] = {
        {"sampler1D", "sampler1DShadow"}, {"sampler2D", "sampler2DShadow"}, {"sampler3D", "sampler3D"},
        {"samplerCube", "samplerCubeShadow"}, {"sampler2DArray", "sampler2DArrayShadow"},
        {"samplerCubeArray", "samplerCubeArrayShadow"}, {"sampler1DArray", "sampler1DArrayShadow"}};
    static const char *const unames[] = {"usampler1D", "usampler2D", "usampler3D", "usamplerCube", "usampler2DArray",
                                         "usamplerCubeArray", "usampler1DArray"};
    static const char *const inames[] = {"isampler1D", "isampler2D", "isampler3D", "isamplerCube", "isampler2DArray",
                                         "isamplerCubeArray", "isampler1DArray"};
    int ty = t->type <= TEXTYPE_1D_ARRAY ? t->type : TEXTYPE_2D;
    if (t->stencil) return unames[ty];
    if (t->sint) return inames[ty];
    return names[ty][t->shadow ? 1 : 0];
}

// (GLSL fragment, lazy heap chunks) Early depth / stencil tests for a shader that counts chunk misses (GMiss atomics):
// only where the tests' outcome can't depend on the shader - no discard, no depth output (a sample mask output
// isn't translated) - and the shader has no side effect of its own (stores), so whether a hidden fragment runs it is
// not observable. (Alpha to
// coverage with early tests would write depth for samples it drops: the renderer uses this shader's module without
// the execution mode for those pipelines, vulkan/exec.c fs_late.)
static bool glsl_early_tests(const Ctx *c) {
    return c->glsl && c->stage == STAGE_FRAGMENT && c->info->uses_gmem && maxwell_gmem_chunk_shift && maxwell_gmem_lazy &&
           !maxwell_gmem_fs_miss && !c->info->uses_discard && !c->omap_depth && !c->uses_gmem_store;
}

// Declarations + entry point around the (rewritten) body; mirrors the MSL assembly in maxwell_translate.
// ---- straight-line form (Vulkan / GLSL). A shader with branches is translated as a `switch (pc)` state machine in a
// loop, with the SSY / PBK reconvergence stacks as 16 shifted variables each: drivers compile that poorly (Pokémon Z-A's
// deferred lighting pass took 5.2 ms of an RTX 4060 at 2x; written as plain code, 1.7 ms). When every branch goes
// forward, the stacks are resolved here, at translation time (each SYNC / BRK gets the one target every path to it
// agrees on), and the dispatcher becomes straight-line code: one variable, tgt, holds the address being skipped to
// (0: running); each run of code is guarded by tgt == 0, each branch target clears it on arrival. An EXIT, or a computed
// branch to an address that isn't a target, leaves it set to the end. Anything else (loops: a backward branch, PCNT /
// CONT, stacks that differ between paths, an unknown jump table) keeps the state machine. LARECOMP_SHADER_FLAT=0: off.
// The input is the body after glsl_rewrite(); the patterns are the ones emit_insn() writes.
typedef struct { int kind; uint32_t arg; int guard; size_t line; } FlatTok;  // guard: line index of its `if (..) {`, or -1
enum { FT_LINE, FT_LABEL, FT_PUSH_SSY, FT_PUSH_PBK, FT_POP_SSY, FT_POP_PBK, FT_BRA, FT_BRX, FT_EXIT };

static const char *skip_sp(const char *s) { while (*s == ' ') s++; return s; }
static bool starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }

// a control line's kind (FT_LINE: not one; -1: a form this pass doesn't know)
static int flat_kind(const char *l, uint32_t *arg) {
    const char *s = skip_sp(l);
    const char *q;
    if (starts(s, "ssy_15 = ssy_14;") && (q = strstr(s, " ssy_0 = "))) { *arg = (uint32_t)strtoul(q + 9, NULL, 10); return FT_PUSH_SSY; }
    if (starts(s, "pbk_15 = pbk_14;") && (q = strstr(s, " pbk_0 = "))) { *arg = (uint32_t)strtoul(q + 9, NULL, 10); return FT_PUSH_PBK; }
    if (starts(s, "pc = ssy_0;")) return FT_POP_SSY;
    if (starts(s, "pc = pbk_0;")) return FT_POP_PBK;
    if (starts(s, "{ uint t = ") && strstr(s, "; pc = t; continue; }")) return FT_BRX;
    if (starts(s, "exit_now = true; break;")) return FT_EXIT;
    if (starts(s, "pc = ") && s[5] >= '0' && s[5] <= '9') {
        char *e;
        *arg = (uint32_t)strtoul(s + 5, &e, 10);
        if (!strcmp(e, "u; continue;")) return FT_BRA;
    }
    if (strstr(s, "pc = ") || strstr(s, "continue;") || strstr(s, "pcnt_") || strstr(s, "exit_now") || strstr(s, "ssy_") ||
        strstr(s, "pbk_"))
        return -1;
    return FT_LINE;
}

static bool flat_fail(const char *why) {
    static int log = -1;
    if (log < 0) log = getenv("LARECOMP_SHADER_FLAT_LOG") != NULL;
    if (log) fprintf(stderr, "[flat] kept the state machine: %s\n", why);
    return false;
}

// body -> *out (the straight-line form, without the tgt declaration); false: keep the state machine
static bool glsl_flatten(Ctx *c, const char *body, Str *out) {
    if (c->brx_unknown) return flat_fail("a computed branch without a known table");
    // lines
    size_t nl = 0, cap = 256;
    char **L = malloc(cap * sizeof *L);
    for (const char *p = body; *p;) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (nl == cap) L = realloc(L, (cap *= 2) * sizeof *L);
        L[nl] = malloc(n + 1);
        memcpy(L[nl], p, n);
        L[nl++][n] = 0;
        p += n + (e ? 1 : 0);
    }
    FlatTok *T = malloc((nl + 1) * sizeof *T);
    size_t nt = 0;
    bool ok = true;
    const char *why = "";
    int depth = 0;  // brace depth of plain lines (control lines must be at the top level of the body)
    for (size_t i = 0; i < nl && ok; i++) {
        const char *l = L[i];
        uint32_t arg = 0;
        if (starts(l, "  case ")) {
            char *e;
            uint32_t a = (uint32_t)strtoul(l + 7, &e, 10);
            if (strcmp(e, "u:") || depth) { ok = false, why = "a label"; break; }
            T[nt++] = (FlatTok){FT_LABEL, a, -1, i};
            continue;
        }
        int k = flat_kind(l, &arg);
        if (k < 0) { ok = false, why = l; break; }
        // a guarded control line: `  if (cond) {`, the line, `  }`
        if (k == FT_LINE && depth == 0 && starts(l, "  if (") && l[strlen(l) - 1] == '{' && i + 2 < nl && !strcmp(L[i + 2], "  }")) {
            uint32_t a2 = 0;
            int k2 = flat_kind(L[i + 1], &a2);
            if (k2 < 0) { ok = false, why = L[i + 1]; break; }
            if (k2 == FT_PUSH_SSY || k2 == FT_PUSH_PBK) { ok = false, why = "a guarded push"; break; }
            if (k2 != FT_LINE) {
                T[nt++] = (FlatTok){k2, a2, (int)i, i + 1};
                i += 2;
                continue;
            }
        }
        if (k != FT_LINE && depth) { ok = false, why = "a control line inside a block"; break; }
        T[nt++] = (FlatTok){k, arg, -1, i};
        if (k == FT_LINE)
            for (const char *q = l; *q; q++) depth += *q == '{' ? 1 : *q == '}' ? -1 : 0;
        if (depth < 0) { ok = false, why = "unbalanced braces"; }
    }
    if (ok && depth) ok = false, why = "unbalanced braces";
    // the stacks along every path (forward branches only: each token is reached with one stack state)
    enum { DMAX = 16 };
    typedef struct { uint8_t seen, nssy, npbk; uint32_t ssy[DMAX], pbk[DMAX]; } St;
    St *S = ok ? calloc(nt + 1, sizeof *S) : NULL;
    uint32_t *res = ok ? calloc(nt + 1, sizeof *res) : NULL;  // a pop's resolved target (0: never reached)
    size_t *lab = NULL;  // token index of each label, by search
    typedef struct { size_t k; St st; } Work;
    size_t nw = 0, wcap = 64;
    Work *W = ok ? malloc(wcap * sizeof *W) : NULL;
#define LABEL_TOK(addr, outk) do { outk = (size_t)-1; for (size_t q_ = 0; q_ < nt; q_++) if (T[q_].kind == FT_LABEL && T[q_].arg == (addr)) { outk = q_; break; } } while (0)
#define PUSH_WORK(kk, ss) do { if (nw == wcap) W = realloc(W, (wcap *= 2) * sizeof *W); W[nw].k = (kk); W[nw].st = (ss); nw++; } while (0)
    if (ok) { St s0 = {0}; PUSH_WORK(0, s0); }
    while (ok && nw) {
        Work w = W[--nw];
        size_t k = w.k;
        St st = w.st;
        for (; k < nt && ok; k++) {
            if (S[k].seen) {
                if (S[k].nssy != st.nssy || S[k].npbk != st.npbk || memcmp(S[k].ssy, st.ssy, sizeof st.ssy[0] * st.nssy) ||
                    memcmp(S[k].pbk, st.pbk, sizeof st.pbk[0] * st.npbk))
                    ok = false, why = "the stacks differ between paths";
                break;
            }
            S[k] = st;
            S[k].seen = 1;
            FlatTok *t = &T[k];
            bool cond = t->guard >= 0;
            if (t->kind == FT_PUSH_SSY || t->kind == FT_PUSH_PBK) {
                uint32_t *a = t->kind == FT_PUSH_SSY ? st.ssy : st.pbk;
                uint8_t *n = t->kind == FT_PUSH_SSY ? &st.nssy : &st.npbk;
                if (*n == DMAX) { ok = false, why = "stack depth"; break; }
                memmove(a + 1, a, *n * sizeof *a);
                a[0] = t->arg;
                (*n)++;
            } else if (t->kind == FT_POP_SSY || t->kind == FT_POP_PBK || t->kind == FT_BRA) {
                St nx = st;
                uint32_t to;
                if (t->kind == FT_BRA) to = t->arg;
                else {
                    uint32_t *a = t->kind == FT_POP_SSY ? nx.ssy : nx.pbk;
                    uint8_t *n = t->kind == FT_POP_SSY ? &nx.nssy : &nx.npbk;
                    if (!*n) { ok = false, why = "a pop of an empty stack"; break; }
                    to = a[0];
                    memmove(a, a + 1, (*n - 1) * sizeof *a);
                    (*n)--;
                    if (res[k] && res[k] != to) { ok = false, why = "a pop with two targets"; break; }
                    res[k] = to;
                }
                size_t tk;
                LABEL_TOK(to, tk);
                if (tk == (size_t)-1 || tk <= k) { ok = false, why = "a backward branch (a loop)"; break; }
                PUSH_WORK(tk, nx);
                if (!cond) break;
            } else if (t->kind == FT_BRX) {
                int b = -1;
                for (int q = 0; q < c->nbrx; q++) if (strstr(L[t->line], "uint t = ") && b < 0) {
                    // (the BRX's own pc: its line reads `uint t = <pc + 8>u + ...`)
                    uint32_t at = (uint32_t)strtoul(strstr(L[t->line], "uint t = ") + 9, NULL, 10) - 8;
                    if (c->brx_pc[q] == at) b = q;
                }
                if (b < 0) { ok = false, why = "a computed branch without its table"; break; }
                for (int q = 0; q < c->brx_ntgt[b] && ok; q++) {
                    // (inside a PBK region the table's targets are its cases, before the break target: an entry past
                    // that is the word after the table, read as one more - brx_table_labels)
                    if (st.npbk && c->brx_tgt[b][q] >= st.pbk[0]) continue;
                    size_t tk;
                    LABEL_TOK(c->brx_tgt[b][q], tk);
                    if (tk == (size_t)-1 || tk <= k) { ok = false, why = "a computed branch backward"; break; }
                    PUSH_WORK(tk, st);
                }
                if (!cond) break;
            } else if (t->kind == FT_EXIT) {
                if (!cond) break;
            }
        }
    }
    // emit: runs of code under `if (tgt == 0u)`, each label clearing tgt
    if (ok) {
        bool open = false;
#define CLOSE() do { if (open) { sp(out, "  }\n"); open = false; } } while (0)
#define OPEN() do { if (!open) { sp(out, "  if (tgt == 0u) {\n"); open = true; } } while (0)
        for (size_t k = 0; k < nt; k++) {
            FlatTok *t = &T[k];
            switch (t->kind) {
            case FT_LABEL: CLOSE(); sp(out, "  if (tgt == %uu) tgt = 0u;\n", t->arg); break;
            case FT_PUSH_SSY: case FT_PUSH_PBK: break;
            case FT_LINE: OPEN(); sp(out, "%s\n", L[t->line]); break;
            default: {
                OPEN();
                if (t->guard >= 0) sp(out, "%s\n", L[t->guard]);
                if (t->kind == FT_BRA) sp(out, "    tgt = %uu;\n", t->arg);
                else if (t->kind == FT_POP_SSY || t->kind == FT_POP_PBK) {
                    if (res[k]) sp(out, "    tgt = %uu;\n", res[k]);
                    else sp(out, "    tgt = 0xFFFFFFFFu;\n");  // (never reached)
                } else if (t->kind == FT_BRX) {
                    const char *a = strstr(L[t->line], "{ uint t = ");
                    const char *e = strstr(a, "; pc = t; continue; }");
                    sp(out, "    %.*s; tgt = t; }\n", (int)(e - a), a);
                } else sp(out, "    tgt = 0xFFFFFFFFu;\n");  // EXIT
                if (t->guard >= 0) sp(out, "  }\n");
                CLOSE();
            }
            }
        }
        CLOSE();
#undef CLOSE
#undef OPEN
    } else {
        flat_fail(why);
    }
#undef LABEL_TOK
#undef PUSH_WORK
    for (size_t i = 0; i < nl; i++) free(L[i]);
    free(L), free(T), free(S), free(res), free(W), free(lab);
    return ok;
}

static ShaderInfo *glsl_finish(Ctx *c, bool any_branch, bool dbg) {
    ShaderInfo *info = c->info;
    const ShaderKey *key = c->key;
    int stage = c->stage;
    bool xfb = stage == STAGE_VERTEX && key && (key->xfb_count[0] | key->xfb_count[1] | key->xfb_count[2] | key->xfb_count[3]);
    if (xfb) { info->uses_gmem = true; c->uses_gmem_store = true; info->uses_gmem_store = true; }
    Str body = {0};
    glsl_rewrite(c, &body, c->body.buf ? c->body.buf : "");
    if (info->uses_gmem && (info->cbuf_mask & (1u << GMEM_BUFFER_INDEX))) fail(c, "cbuf %d clashes with gmem", GMEM_BUFFER_INDEX);
    if (info->cbuf_mask >> 18) fail(c, "cbuf index out of range");
    for (int i = 0; i < info->ntex; i++)
        if (info->tex[i].shadow && info->tex[i].type == TEXTYPE_3D) fail(c, "3d shadow texture");
    if (c->fail) { free(body.buf); free(c->body.buf); free(c); return info; }
    int set = stage == STAGE_FRAGMENT ? 1 : 0;
    Str s = {0};
    sp(&s, "#version 460\n");  // (the prelude's first line; an extension directive has to follow it)
    if (info->uses_gmem && maxwell_gmem_chunk_shift && maxwell_gmem_lazy) sp(&s, "#extension GL_EXT_buffer_reference : require\n");
    {   // (test) LARECOMP_WARP_FORCE_EMU=1: the 64-lane warp code even on 32-lane subgroups (it must change nothing there)
        const char *pre = glsl_prelude + sizeof "#version 460\n" - 1;
        static int force = -1;
        if (force < 0) force = getenv("LARECOMP_WARP_FORCE_EMU") && atoi(getenv("LARECOMP_WARP_FORCE_EMU"));
        const char *w = force ? strstr(pre, "gl_SubgroupSize == 32u") : NULL;
        if (w) sp(&s, "%.*sfalse%s", (int)(w - pre), pre, w + strlen("gl_SubgroupSize == 32u"));
        else sp(&s, "%s", pre);
    }
    static int exact = -1;
    if (exact < 0) exact = getenv("LARECOMP_VK_EXACT_H2F") && atoi(getenv("LARECOMP_VK_EXACT_H2F")) != 0;
    sp(&s, "%s%s", !exact && (stage == STAGE_FRAGMENT || stage == STAGE_COMPUTE) ? glsl_h2f_native : glsl_h2f_exact,
       glsl_prelude_half);
    for (int i = 0; i < 18; i++) {
        if (!(info->cbuf_mask & (1u << i))) continue;
        uint32_t n = info->cbuf_size[i] / 16;
        if (c->cbuf_need[i] > n) n = c->cbuf_need[i];
        if ((info->cbuf_dynamic & (1u << i)) && n < 4096) n = 4096;
        if (!n) n = 1;
        sp(&s, "layout(std140, set = %d, binding = %d) uniform CB%d { uvec4 cb%d[%u]; };\n", set, i, i, i, n);
    }
    if (info->uses_gmem && !maxwell_gmem_chunk_shift) {
        sp(&s, "layout(std430, set = %d, binding = %d) %sbuffer GMem { uint w[]; } gmem_;\n", set, GMEM_BUFFER_INDEX,
           c->uses_gmem_store ? "" : "readonly ");
        sp(&s, "#define GMR(i) gmem_.w[i]\n");
        if (c->uses_gmem_store)
            sp(&s, "#define GMW(i, v) gmem_.w[i] = (v)\n#define GMAND(i, m) atomicAnd(gmem_.w[i], m)\n"
                   "#define GMOR(i, m) atomicOr(gmem_.w[i], m)\n");
    } else if (info->uses_gmem && !maxwell_gmem_lazy) {  // the heap in windows, one binding each, plain branches (shader.h)
        int n = maxwell_gmem_chunks(), ws = (int)maxwell_gmem_chunk_shift - 2, cs = GMEM_CHUNK_SET(stage);
        for (int k = 0; k < n; k++)
            sp(&s, "layout(std430, set = %d, binding = %d) %sbuffer GMem%d { uint w[]; } gmem%d_;\n", cs, GMEM_CHUNK_BINDING + k,
               c->uses_gmem_store ? "" : "readonly ", k, k);
        sp(&s, "uint GMR_(uint i) { uint c = i >> %du, o = i & 0x%xu;\n", ws, (1u << ws) - 1);
        for (int k = 0; k < n; k++) sp(&s, "  if (c == %du) return gmem%d_.w[o];\n", k, k);
        sp(&s, "  return 0u; }\n#define GMR(i) GMR_(i)\n");
        if (c->uses_gmem_store) {
            static const char *const ops[3][2] = {{"GMW_", "gmem%d_.w[o] = v"}, {"GMAND_", "atomicAnd(gmem%d_.w[o], v)"},
                                                  {"GMOR_", "atomicOr(gmem%d_.w[o], v)"}};
            for (int op = 0; op < 3; op++) {
                sp(&s, "void %s(uint i, uint v) { uint c = i >> %du, o = i & 0x%xu;\n", ops[op][0], ws, (1u << ws) - 1);
                for (int k = 0; k < n; k++) {
                    char body[64];
                    snprintf(body, sizeof body, ops[op][1], k);
                    sp(&s, "  if (c == %du) { %s; return; }\n", k, body);
                }
                sp(&s, "}\n");
            }
            sp(&s, "#define GMW(i, v) GMW_(i, v)\n#define GMAND(i, m) GMAND_(i, m)\n#define GMOR(i, m) GMOR_(i, m)\n");
        }
    } else if (info->uses_gmem) {  // chunks imported lazily: a table of device addresses, buffer references (shader.h)
        int ws = (int)maxwell_gmem_chunk_shift - 2, cs = GMEM_CHUNK_SET(stage);
        bool direct = stage != STAGE_COMPUTE;  // (compute: its storage buffers are in gfast_ already)
        sp(&s, "layout(std140, set = %d, binding = %d) uniform GChunks { uvec4 gchunk[%d]; };\n", cs, GMEM_CHUNK_BINDING, GMEM_CHUNKS_MAX);
        sp(&s, "layout(buffer_reference, std430, buffer_reference_align = 4) %sbuffer GW { uint w[]; };\n", c->uses_gmem_store ? "" : "readonly ");
        // Misses are counted with atomics on GMiss: a side effect, which in a fragment shader turns early depth / stencil
        // tests off - every overdrawn fragment ran in full (UHD 770 main pass 13.3 ms vs 8.4). So a fragment shader
        // whose own result can't depend on late tests (no discard or depth output, no stores) declares
        // early tests (glsl_early_tests) and keeps its counter; one that can't (discard / depth, no stores)
        // counts nothing - it would cost those tests - and relies on the runtime bringing its chunks in before the draw:
        // the bound storage buffers' chunks (gmem_chunk_bind), pointers in uniform buffers, the chunks other shaders
        // missed (cache/gmem_chunks.txt). LARECOMP_VK_GMEM_FS_MISS=1 (maxwell_gmem_fs_miss): every shader counts, no
        // early tests (as before; to find a chunk only such a shader reaches).
        bool count = stage != STAGE_FRAGMENT || maxwell_gmem_fs_miss || c->uses_gmem_store || glsl_early_tests(c);
        if (count) {
            sp(&s, "layout(std430, set = %d, binding = %d) buffer GMiss { uint n[%d]; uint w[%d]; } gmiss_;\n", cs, GMEM_MISS_BINDING,
               GMEM_CHUNKS_MAX, GMEM_CHUNKS_MAX);
            sp(&s, "void gmiss(uint c, uint i) { atomicAdd(gmiss_.n[c], 1u); atomicMin(gmiss_.w[c], i); }\n");
        } else {
            sp(&s, "void gmiss(uint c, uint i) { }\n");
        }
        // (gchunk[c]: x, y = device address, z = words; the first word's chunk holds the whole aligned access)
        sp(&s, "#define GMC(i) ((i) >> %du)\n#define GMO(i) ((i) & 0x%xu)\n", ws, (1u << ws) - 1);
        if (direct) {  // (gd[k]: x = the chunk's first word, y = its words (0: unused), z, w = its device address; GDir<k>: it)
            sp(&s, "layout(push_constant) uniform GDirect { uvec4 gd[%d]; };\n", GMEM_DIRECT_MAX);
            for (int k = 0; k < GMEM_DIRECT_MAX; k++)
                sp(&s, "layout(std430, set = %d, binding = %d) %sbuffer GDir%d { uint w[]; } gdir%d_;\n", cs, GMEM_DIRECT_BINDING + k,
                   c->uses_gmem_store ? "" : "readonly ", k, k);
        }
        // Each access: the draw's storage buffer chunks first (gd, uniform: no table read, no miss path), then the table.
        static const struct { const char *head, *act, *miss; int last; bool store; } fn[] = {
            {"uint GMR1(uint i)", "return p.w[o];", "return 0u;", 0, false},
            {"uvec2 GMR2(uint i)", "return uvec2(p.w[o], p.w[o + 1u]);", "return uvec2(0u);", 1, false},
            {"uvec4 GMR4(uint i)", "return uvec4(p.w[o], p.w[o + 1u], p.w[o + 2u], p.w[o + 3u]);", "return uvec4(0u);", 3, false},
            {"void GMW1(uint i, uint v)", "p.w[o] = v; return;", "", 0, true},
            {"void GMW2(uint i, uvec2 v)", "p.w[o] = v.x; p.w[o + 1u] = v.y; return;", "", 1, true},
            {"void GMW4(uint i, uvec4 v)", "p.w[o] = v.x; p.w[o + 1u] = v.y; p.w[o + 2u] = v.z; p.w[o + 3u] = v.w; return;", "", 3, true},
            {"void GMAND1(uint i, uint v)", "atomicAnd(p.w[o], v); return;", "", 0, true},
            {"void GMOR1(uint i, uint v)", "atomicOr(p.w[o], v); return;", "", 0, true},
        };
        for (int f = 0; f < (int)(sizeof fn / sizeof fn[0]); f++) {
            if (fn[f].store && !c->uses_gmem_store) continue;
            sp(&s, "%s { uint o;", fn[f].head);
            if (direct)  // (whole chunks: an aligned access in one never leaves it; the storage buffer, not the address)
                for (int k = 0; k < GMEM_DIRECT_MAX; k++) {
                    char act[192], *d = act;
                    for (const char *a = fn[f].act; *a && d < act + sizeof act - 8; a++) {
                        if (a[0] == 'p' && a[1] == '.') d += snprintf(d, 8, "gdir%d_", k);
                        else *d++ = *a;
                    }
                    *d = 0;
                    sp(&s, " o = i - gd[%d].x; if (o < gd[%d].y) { %s }", k, k, act);
                }
            char lim[16];
            if (fn[f].last) snprintf(lim, sizeof lim, "o + %du", fn[f].last); else snprintf(lim, sizeof lim, "o");
            sp(&s, " uint c = GMC(i); o = GMO(i); uvec4 e = gchunk[c]; if (%s < e.z) { GW p = GW((uint64_t(e.y) << 32) | uint64_t(e.x)); %s } "
                       "gmiss(c, i); %s }\n", lim, fn[f].act, fn[f].miss);
        }
        if (c->uses_gmem_store) {  // ATOM / RED on the heap: the old value comes back (GMiss counts what the chunk table lacks)
            static const char *const ops[][2] = {{"GMA_ADD", "atomicAdd"}, {"GMA_MIN", "atomicMin"}, {"GMA_MAX", "atomicMax"},
                                                 {"GMA_AND", "atomicAnd"}, {"GMA_OR", "atomicOr"}, {"GMA_XOR", "atomicXor"},
                                                 {"GMA_XCH", "atomicExchange"}};
            for (int f = 0; f < (int)(sizeof ops / sizeof ops[0]); f++)
                sp(&s, "uint %s(uint i, uint v) { uint c = GMC(i), o = GMO(i); uvec4 e = gchunk[c]; if (o < e.z) { "
                       "GW p = GW((uint64_t(e.y) << 32) | uint64_t(e.x)); return %s(p.w[o], v); } gmiss(c, i); return 0u; }\n", ops[f][0], ops[f][1]);
            sp(&s, "uint GMCAS(uint i, uint cmp, uint v) { uint c = GMC(i), o = GMO(i); uvec4 e = gchunk[c]; if (o < e.z) { "
                   "GW p = GW((uint64_t(e.y) << 32) | uint64_t(e.x)); return atomicCompSwap(p.w[o], cmp, v); } gmiss(c, i); return cmp; }\n");
        }
        sp(&s, "#define GMR(i) GMR1(i)\n");
        if (c->uses_gmem_store) sp(&s, "#define GMW(i, v) GMW1(i, v)\n#define GMAND(i, m) GMAND1(i, m)\n#define GMOR(i, m) GMOR1(i, m)\n");
    }
    if (xfb) sp(&s, "layout(std430, set = %d, binding = %d) buffer XfbOut { uint w[]; } xfbo_;\n", set, XFB_BUFFER_INDEX);
    if (info->uses_gmem_map) {  // (compute) the dispatch's storage buffers, device-local: gmap[i] = heap offset, size, word
        sp(&s, "layout(std140, set = %d, binding = %d) uniform GMap { uvec4 gmap[%d]; };\n", set, GMAP_BUFFER_INDEX, GMAP_MAX + 1);
        sp(&s, "layout(std430, set = %d, binding = %d) %sbuffer GFast { uint w[]; } gfast_;\n", set, GFAST_BUFFER_INDEX,
           c->uses_gmem_store ? "" : "readonly ");
        sp(&s, "uint gfind(uint g) { uint n = gmap[%d].x; for (uint i = 0u; i < n; i++) { uint o = g - gmap[i].x; "
               "if (g >= gmap[i].x && o < gmap[i].y) return gmap[i].z + (o >> 2); } return 0xFFFFFFFFu; }\n", GMAP_MAX);
    }
    if (xfb) sp(&s, "layout(std140, set = 0, binding = 18) uniform RScale { vec4 rscale[70]; };\n");
    if (c->uses_rscale && stage != STAGE_VERTEX) {
        sp(&s, "layout(std140, set = %d, binding = 18) uniform RScale { vec4 rscale[%d]; };\n", set,
           key && key->flip_x ? 66 : 65);  // ([65]: mirror mode's render target width)
        info->uses_rscale = true;
    }
    bool tint = stage == STAGE_FRAGMENT && !dbg && (c->omap_target & 0x7) == 0x7 && (!key || key->rt_kind[0] == 0);
    if (tint) {
        sp(&s, "layout(std140, set = %d, binding = 19) uniform Tint { vec4 ltint[4]; };\n", set);
        info->uses_tint = true;
    }
    for (int i = 0; i < info->ntex; i++) {
        ShaderTexture *t = &info->tex[i];
        sp(&s, "layout(set = %d, binding = %d) uniform %s t%d;\n", set, 32 + i, glsl_sampler_type(t), i);
        // size of level l as (width, height, depth or layers)
        switch (t->type) {
        case TEXTYPE_1D: sp(&s, "ivec3 tsize%d(int l) { return ivec3(textureSize(t%d, l), 1, 1); }\n", i, i); break;
        case TEXTYPE_1D_ARRAY: sp(&s, "ivec3 tsize%d(int l) { ivec2 s = textureSize(t%d, l); return ivec3(s.x, 1, s.y); }\n", i, i); break;
        case TEXTYPE_3D: case TEXTYPE_2D_ARRAY: case TEXTYPE_CUBE_ARRAY:
            sp(&s, "ivec3 tsize%d(int l) { return textureSize(t%d, l); }\n", i, i); break;
        default: sp(&s, "ivec3 tsize%d(int l) { return ivec3(textureSize(t%d, l), 1); }\n", i, i);
        }
    }
    if (stage == STAGE_VERTEX) {
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            int kind = key ? key->attr_kind[i] : 0;
            sp(&s, "layout(location = %d) in %s a%d;\n", i, kind == 1 ? "ivec4" : kind == 2 ? "uvec4" : "vec4", i);
        }
        // depth pre-pass + EQUAL test needs identical positions from different shaders: invariant, and precise so no
        // compiler fuses or reorders the math feeding it (MSL gets this from preserveInvariance; without it foliage
        // drawn twice flickered black)
        sp(&s, "invariant gl_Position;\nprecise gl_Position;\n");
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "layout(location = %d) out vec4 v%d;\n", i, i);
    } else if (stage == STAGE_FRAGMENT) {
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            const char *interp = (c->flat_mask & (1u << i)) ? "flat " : (c->noperspective_mask & (1u << i)) ? "noperspective " : "";
            sp(&s, "layout(location = %d) %sin vec4 v%d;\n", i, interp, i);
        }
        for (int rt = 0; rt < 8; rt++) {
            if (!((c->omap_target >> (rt * 4)) & 0xF)) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            sp(&s, "layout(location = %d) out %s c%d;\n", rt, kind == 1 ? "ivec4" : kind == 2 ? "uvec4" : "vec4", rt);
        }
        if (c->omap_depth) info->writes_depth = true;
        if (glsl_early_tests(c)) sp(&s, "layout(early_fragment_tests) in;\n");
    } else {
        sp(&s, "layout(local_size_x_id = 0, local_size_y_id = 1, local_size_z_id = 2) in;\n");
    }
    sp(&s, "void main() {\n");
    if (stage == STAGE_VERTEX) {
        sp(&s, "  gl_Position = vec4(0, 0, 0, 1);\n");
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "  v%d = vec4(0, 0, 0, 1);\n", i);
        if (info->writes_point_size) sp(&s, "  gl_PointSize = 0.0f;\n");
        if (info->writes_layer) sp(&s, "  gl_Layer = 0;\n");
    }
    for (int i = 0; i < 256; i++)
        if (c->reg_used[i]) sp(&s, "  uint r%d = 0u;\n", i);
    for (int i = 0; i < 7; i++)
        if (c->pred_used[i]) sp(&s, "  bool p%d = false;\n", i);
    if (c->uses_cc) sp(&s, "  uint cc = 0u; bool ccz = false, ccs = false;\n");
    if (c->uses_local) sp(&s, "  uint lmem[%d]; for (int li_ = 0; li_ < %d; li_++) lmem[li_] = 0u;\n", LMEM_WORDS, LMEM_WORDS);
    if (dbg) sp(&s, maxwell_dbg_pc == 0xffffffffu ? "  float dbgv = 1.0f;\n" : "  float dbgv = -1.0f;\n");
    static int flat_on = -1;
    if (flat_on < 0) { const char *e = getenv("LARECOMP_SHADER_FLAT"); flat_on = !e || atoi(e) != 0; }
    Str flat = {0};
    if (any_branch && flat_on && !dbg && glsl_flatten(c, body.buf ? body.buf : "", &flat)) {
        sp(&s, "  uint tgt = 0u;  // (straight-line form: the address being skipped to, 0 running - glsl_flatten)\n");
        sp(&s, "%s", flat.buf ? flat.buf : "");
        free(flat.buf);
    } else if (any_branch) {
        free(flat.buf);
        sp(&s, "  uint pc = 8u; uint ssy_0 = 0u; uint ssy_1 = 0u; uint ssy_2 = 0u; uint ssy_3 = 0u; uint ssy_4 = 0u; uint ssy_5 = 0u; uint ssy_6 = 0u; uint ssy_7 = 0u; uint ssy_8 = 0u; uint ssy_9 = 0u; uint ssy_10 = 0u; uint ssy_11 = 0u; uint ssy_12 = 0u; uint ssy_13 = 0u; uint ssy_14 = 0u; uint ssy_15 = 0u; uint pbk_0 = 0u; uint pbk_1 = 0u; uint pbk_2 = 0u; uint pbk_3 = 0u; uint pbk_4 = 0u; uint pbk_5 = 0u; uint pbk_6 = 0u; uint pbk_7 = 0u; uint pbk_8 = 0u; uint pbk_9 = 0u; uint pbk_10 = 0u; uint pbk_11 = 0u; uint pbk_12 = 0u; uint pbk_13 = 0u; uint pbk_14 = 0u; uint pbk_15 = 0u; uint pcnt_0 = 0u;\n");
        sp(&s, "  bool exit_now = false;\n");
        sp(&s, "  for (uint guard = 0u; guard < 100000u; guard++) {\n  switch (pc) {\n");
        sp(&s, "%s", body.buf ? body.buf : "");
        sp(&s, "    exit_now = true; break;\n  default: exit_now = true; break;\n  }\n  if (exit_now) break;\n  }\n");
    } else {
        sp(&s, "  do {\n%s  } while (false);\n", body.buf ? body.buf : "");
    }
    free(body.buf);
    if (xfb) {  // transform feedback: the written varying components, in order, to the buffers (ShaderKey.xfb_count)
        int b = 0, k = 0;
        for (int i = 0; i < 32 && b < 4; i++)
            for (int comp = 0; comp < 4 && b < 4; comp++) {
                if (!(c->out_comp[i] & (1u << comp))) continue;
                while (b < 4 && k >= key->xfb_count[b]) { b++; k = 0; }
                if (b >= 4) break;
                // (as on the hardware, nothing past the bound size: rscale[66 + b].y = the buffer's size in words)
                unsigned sw = (unsigned)(key->xfb_stride[b] / 4);
                sp(&s, "  if (floatBitsToUint(rscale[%d].z) != 0u && uint(gl_VertexIndex) * %uu + %du < floatBitsToUint(rscale[%d].y)) "
                       "xfbo_.w[floatBitsToUint(rscale[%d].x) + uint(gl_VertexIndex) * %uu + %du] = floatBitsToUint(v%d[%d]);\n",
                       66 + b, sw, k, 66 + b, 66 + b, sw, k, i, comp);
                k++;
            }
    }
    if (stage == STAGE_VERTEX) {
        if (key && key->flip_y) sp(&s, "  gl_Position.y = -gl_Position.y;\n");
        if (key && key->flip_x) sp(&s, "  gl_Position.x = -gl_Position.x;\n");
        if (key && key->depth_minus_one) sp(&s, "  gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5f;\n");
    } else if (stage == STAGE_FRAGMENT) {
        int r = 0;
        for (int rt = 0; rt < 8; rt++) {
            uint32_t m = (c->omap_target >> (rt * 4)) & 0xF;
            if (!m) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            const char *cv = kind == 1 ? "int" : kind == 2 ? "uint" : "";
            for (int comp = 0; comp < 4; comp++) {
                if (!(m & (1u << comp))) continue;
                if (dbg) {
                    if (comp == 3) sp(&s, "  c%d[%d] = %s(1.0f);\n", rt, comp, cv);
                    else sp(&s, "  c%d[%d] = %s(dbgv);\n", rt, comp, cv);
                } else if (r < 256 && c->reg_used[r]) {
                    if (kind == 0) sp(&s, "  c%d[%d] = as_f(r%d);\n", rt, comp, r);
                    else sp(&s, "  c%d[%d] = %s(r%d);\n", rt, comp, cv, r);
                } else {
                    sp(&s, "  c%d[%d] = 0;\n", rt, comp);
                }
                r++;
            }
        }
        if (tint)
            sp(&s, "  if (ltint[0].w > 0.0f) { vec3 g = c0.rgb; float l = dot(g, vec3(0.299f, 0.587f, 0.114f));\n"
                   "    c0.rgb = mix(g, ltint[0].rgb * pow(l, ltint[1].w) + ltint[1].rgb * l, ltint[0].w); }\n");
        if (tint) emit_recolour(&s, "c0", "vec3", "vec4");
        if (c->omap_depth) {
            if (c->reg_used[r + 1]) sp(&s, "  gl_FragDepth = as_f(r%d);\n", r + 1);
            else sp(&s, "  gl_FragDepth = gl_FragCoord.z;\n");
        }
    }
    sp(&s, "}\n");
    info->msl = s.buf;
    info->attr_mask = c->in_attr_mask;
    info->out_mask = stage == STAGE_FRAGMENT ? c->omap_target : c->out_attr_mask;
    free(c->body.buf);
    free(c);
    return info;
}

// BRX (indirect branch, Vulkan / GLSL only): label just the targets its jump table can name. A BRX used to make every
// instruction a case of the dispatcher switch (any one may be a target): hundreds of cases falling into each other
// inside the dispatcher loop. Suspected of what AMD's Windows driver never finishes building (issue #237: the dialogue
// body text's vertex shader stayed "building", its draws skipped - Tarin's name showed, the message stayed blank).
// The compiler puts the table in the program's own constants (cb1, after the code: blob + const_offset), loaded into
// the BRX register by an LDC c[0x1][Rx + table] shortly before: BRX Ra, imm goes to pc + 8 + table[k] + imm. Entries
// are read until one isn't an instruction of this shader (the end of the table; a word past it that still looks like
// one only adds a harmless case). False: not that shape (the register set some other way, no constants, nothing
// valid) - the caller labels every instruction as before. LARECOMP_BRX_ALL_LABELS=1: always every instruction.
static bool brx_table_labels(const uint8_t *code, size_t i, size_t end, const uint8_t *blob, size_t const_offset, uint8_t *label,
                             uint32_t *tgts, int *ntgts) {
    static int all = -1;
    if (all < 0) { const char *e = getenv("LARECOMP_BRX_ALL_LABELS"); all = e && atoi(e) != 0; }
    if (all || const_offset >= 0x10000 || const_offset < 0x80) return false;  // (0x10000: no constants known)
    uint64_t brx = ((const uint64_t *)code)[i];
    int ra = (int)bits(brx, 8, 8);
    int64_t imm = sbits(brx, 20, 24);
    if (ra == RZ) return false;
    // the instruction that set ra: an LDC from cb1 within the few before, nothing else writing ra (or a branch) between
    int64_t table = -1;
    for (size_t k = i, seen = 0; k-- > 0 && seen < 32;) {
        if (k % 4 == 0) continue;  // (scheduling words)
        seen++;
        uint64_t w = ((const uint64_t *)code)[k];
        if ((w & 0xfff0000000000000ull) >= 0xe200000000000000ull && (w & 0xfff0000000000000ull) <= 0xe350000000000000ull)
            return false;  // flow control between: not straight-line
        if ((w & 0xfff8000000000000ull) == 0xef90000000000000ull && (int)bits(w, 0, 8) == ra) {  // LDC ra, c[idx][Rx + off]
            if (bits(w, 36, 5) != 1 || bits(w, 48, 3) != 4) return false;  // (cb1, 32-bit)
            table = (int64_t)bits(w, 20, 16);
            break;
        }
        if ((int)bits(w, 0, 8) == ra) return false;  // (something else may write ra: unknown)
    }
    if (table < 0) return false;
    const uint8_t *tab = blob + const_offset + table;
    uint8_t *mark = calloc(end + 4, 1);
    int n = 0;
    for (int k = 0; k < 256; k++) {
        int32_t v;
        memcpy(&v, tab + 4 * k, 4);
        int64_t t = (int64_t)i * 8 + 8 + v + imm;
        if (t < 8 || (t & 7) || (size_t)(t / 8) >= end) break;
        t = norm_target(t);  // (as the BRX itself does at run time)
        if ((size_t)(t / 8) >= end) break;
        mark[t / 8] = 1;
        if (*ntgts < 64) tgts[(*ntgts)++] = (uint32_t)t;
        n++;
    }
    if (n) for (size_t k = 0; k < end; k++) label[k] |= mark[k];
    free(mark);
    return n > 0;
}

// compute programs have no SPH: code starts at the blob, or (newer SDKs, e.g. Pokémon Z-A's 19) after a 0x100-byte
// header whose first word is 0x12345679
static const uint8_t *code_start(const uint8_t *blob, int stage) {
    if (stage == STAGE_COMPUTE) return *(const uint32_t *)blob == 0x12345679 ? blob + 0x100 : blob;
    return blob + 0x80;
}

// the program's length in instruction words: the last EXIT followed by self-branch padding, or max_size
static size_t code_end(const uint8_t *blob, const uint8_t *code, size_t max_size) {
    size_t n = (max_size - (code - blob)) / 8;
    if (n > 65536) n = 65536;
    size_t end = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t w = ((const uint64_t *)code)[i];
        if (i % 4 == 0) continue;
        if ((w & 0xfff0000000000000ull) == 0xe240000000000000ull && sbits(w, 20, 24) == -8 && end) break;  // bra self
        if (w == 0 && i > 4) break;
        end = i + 1;
    }
    return end;
}

static ShaderInfo *translate(const uint8_t *blob, size_t max_size, int stage, const ShaderKey *key) {
    ShaderInfo *info = calloc(1, sizeof *info);
    info->stage = stage;
    Ctx *c = calloc(1, sizeof *c);
    c->info = info;
    c->key = key;
    c->stage = stage;
    c->ipa_mode = -1;
    c->glsl = key && key->glsl;
    const uint8_t *sph = blob + 0x30;
    const uint8_t *code = code_start(blob, stage);
    if (stage == STAGE_FRAGMENT) {
        c->omap_target = *(const uint32_t *)(sph + 0x48);
        c->omap_depth = (*(const uint32_t *)(sph + 0x4C) >> 1) & 1;
    }
    size_t end = code_end(blob, code, max_size);
    // collect branch targets (labels)
    uint8_t *label = calloc(end + 4, 1);
    label[1] = 1;
    bool any_branch = false;
    for (size_t i = 0; i < end; i++) {
        if (i % 4 == 0) continue;
        uint64_t w = ((const uint64_t *)code)[i];
        uint32_t pc = (uint32_t)(i * 8);
        bool is_flow = (w & 0xfff0000000000020ull) == 0xe240000000000000ull ||
                       (w & 0xfff0000000000020ull) == 0xe290000000000000ull ||
                       (w & 0xfff0000000000020ull) == 0xe2a0000000000000ull ||
                       (w & 0xfff0000000000020ull) == 0xe2b0000000000000ull;
        if (is_flow) {
            int64_t t = norm_target((int64_t)pc + 8 + sbits(w, 20, 24));
            if (t >= 0 && (size_t)(t / 8) < end) label[t / 8] = 1;
            any_branch = true;
        }
        if ((w & 0xfff0000000000000ull) == 0xe250000000000000ull) {  // BRX: its jump table's targets, else any instruction
            any_branch = true;
            bool known = false;
            if (c->glsl && c->nbrx < 32) {
                int k = c->nbrx;
                c->brx_pc[k] = (uint32_t)(i * 8), c->brx_ntgt[k] = 0;
                known = brx_table_labels(code, i, end, blob, max_size, label, c->brx_tgt[k], &c->brx_ntgt[k]);
                if (known && c->brx_ntgt[k] < 64) c->nbrx++;
                else known = false;
            }
            if (!known) { c->brx_unknown = true; memset(label, 1, end); }
        }
        if ((w & 0xfff8000000000000ull) == 0xf0f8000000000000ull || (w & 0xfff0000000000000ull) == 0xe340000000000000ull ||
            (w & 0xfff0000000000000ull) == 0xe350000000000000ull)
            any_branch = true;
        (void)is_branch_target_op;
    }
    // translate
    bool dbg = maxwell_dbg_code && blob == maxwell_dbg_code && stage == STAGE_FRAGMENT;
    static int annotate = -1;
    if (annotate < 0) annotate = getenv("LARECOMP_SAVE_MSL") != NULL;
    c->branchy = any_branch;
    for (size_t i = 0; i < end && !c->fail; i++) {
        if (i % 4 == 0) continue;
        uint32_t pc = (uint32_t)(i * 8);
        if (any_branch && label[i]) sp(&c->body, "  case %uu:\n", pc);
        if (annotate) sp(&c->body, "    // pc %x: %016llx\n", pc, (unsigned long long)((const uint64_t *)code)[i]);
        emit_insn(c, ((const uint64_t *)code)[i], pc);
        light_tiles_fix(c, (const uint64_t *)code, end, pc);
        if (dbg && pc == maxwell_dbg_pc) {
            int rr = maxwell_dbg_reg % 1000, half = maxwell_dbg_reg / 1000;  // +1000: low f16, +2000: high f16
            c->reg_used[rr] = 1;
            if (half) sp(&c->body, c->glsl ? "    dbgv = unpackHalf2x16(r%d)[%d];\n" : "    dbgv = float(as_type<half2>(r%d)[%d]);\n", rr, half - 1);
            else sp(&c->body, "    dbgv = as_type<float>(r%d);\n", rr);
        }
        info->num_insts++;
    }
    free(label);
    // (MSL: more textures than sampler slots take a slot table, at a buffer index the vertex stage's streams use)
    if (!c->glsl && stage == STAGE_VERTEX && info->ntex > MSL_MAX_SAMPLERS)
        fail(c, "%d textures in a vertex shader (Metal: %d samplers)", info->ntex, MSL_MAX_SAMPLERS);
    if (c->fail) { free(c->body.buf); free(c); return info; }
    if (c->glsl) return glsl_finish(c, any_branch, dbg);

    // ---------------------------------------------------------- MSL source
    Str s = {0};
    sp(&s, "#include <metal_stdlib>\nusing namespace metal;\n");
    sp(&s, "static inline uint cbr(constant uint4 *cb, uint off) { return cb[off >> 4][(off >> 2) & 3]; }\n");
    sp(&s, "static inline uint bfe_u(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return 0u; return extract_bits(v, pos, min(len, 32u - pos)); }\n");
    sp(&s, "static inline uint bfe_s(uint v, uint ctl) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u) return 0u; if (pos >= 32u) return (int(v) < 0) ? 0xFFFFFFFFu : 0u; return uint(extract_bits(int(v), pos, min(len, 32u - pos))); }\n");
    sp(&s, "static inline uint bfi(uint ins, uint ctl, uint base) { uint pos = ctl & 0xFFu, len = (ctl >> 8) & 0xFFu; if (len == 0u || pos >= 32u) return base; return insert_bits(base, ins, pos, min(len, 32u - pos)); }\n");
    sp(&s, "static inline float fswzadd(float a, float b, uint swz, uint lane) { uint m = (swz >> ((lane & 3u) * 2u)) & 3u; float ma = m == 1u ? -1.0f : (m == 3u ? 0.0f : 1.0f); float mb = m == 2u ? -1.0f : 1.0f; return a * ma + b * mb; }\n");
    sp(&s, "static inline uint2 rsc(uint2 c, float4 s) { return (s.x == 1.0f && s.y == 1.0f) ? c : uint2(floor((float2(c) + 0.5f) * s.xy)); }\n");
    sp(&s, "static inline float2 uvs(float2 uv, float4 s) { return (s.z == 1.0f && s.w == 1.0f) ? uv : min(uv, 1.0f) * s.zw; }\n");
    sp(&s, "static inline uint2 rscf(uint2 c, float4 s, float4 r, float2 p) { if (s.x == 1.0f && s.y == 1.0f) return c;"
           " if (any(s.xy != r.xy)) return uint2(floor((float2(c) + 0.5f) * s.xy));"
           " return uint2(floor(float2(c) * s.xy) + floor(fract((p - r.zw) / r.xy) * s.xy)); }\n");
    sp(&s, "static inline float2 hx(uint v, uint swz) { if (swz == 1u) return float2(as_type<float>(v)); float2 h = float2(as_type<half2>(v)); return swz == 2u ? h.xx : swz == 3u ? h.yy : h; }\n");
    // merges on half2 lanes rather than with integer masks: on an M1 (macOS 15.0.1) the Metal compiler miscompiled
    // (old & 0xFFFF0000u) | lo in Link's body shader - the merged low half read back as a constant 512.0 and Link
    // rendered white. Same bits either way.
    sp(&s, "static inline uint hpack(float2 r, uint merge, uint old) { if (merge == 1u) return as_type<uint>(r.x); half2 o = as_type<half2>(old), n = half2(r); if (merge == 2u) o.x = n.x; else if (merge == 3u) o.y = n.y; else o = n; return as_type<uint>(o); }\n");
    sp(&s, "static inline uint prmt(uint a, uint b, uint sel) { uint r = 0; for (uint i = 0; i < 4; i++) { uint s = (sel >> (i * 4)) & 7u; uint byte = s < 4 ? (a >> (s * 8)) & 0xFFu : (b >> ((s - 4) * 8)) & 0xFFu; r |= byte << (i * 8); } return r; }\n");

    // inputs/outputs
    if (stage == STAGE_VERTEX) {
        sp(&s, "struct VIn {\n");
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            int kind = key ? key->attr_kind[i] : 0;
            sp(&s, "  %s4 a%d [[attribute(%d)]];\n", kind == 1 ? "int" : kind == 2 ? "uint" : "float", i, i);
        }
        sp(&s, "};\n");
        sp(&s, "struct VOut {\n  float4 pos [[position, invariant]];\n");  // depth pre-pass + EQUAL test needs identical positions
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "  float4 v%d [[user(locn%d)]];\n", i, i);
        if (info->writes_point_size) sp(&s, "  float psize [[point_size]];\n");
        if (info->writes_layer) sp(&s, "  uint layer [[render_target_array_index]];\n");
        sp(&s, "};\n");
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "struct FIn {\n  float4 pos [[position]];\n");
        for (int i = 0; i < 32; i++) {
            if (!(c->in_attr_mask & (1u << i))) continue;
            const char *interp = (c->flat_mask & (1u << i)) ? " [[flat]]" : (c->noperspective_mask & (1u << i)) ? " [[center_no_perspective]]" : "";
            sp(&s, "  float4 v%d [[user(locn%d)]]%s;\n", i, i, interp);
        }
        sp(&s, "};\n");
        sp(&s, "struct FOut {\n");
        for (int rt = 0; rt < 8; rt++) {
            if (!((c->omap_target >> (rt * 4)) & 0xF)) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            sp(&s, "  %s4 c%d [[color(%d)]];\n", kind == 1 ? "int" : kind == 2 ? "uint" : "float", rt, rt);
        }
        if (c->omap_depth) { sp(&s, "  float depth [[depth(any)]];\n"); info->writes_depth = true; }
        sp(&s, "};\n");
    }

    // entry point
    Str args = {0};
    for (int i = 0; i < 18; i++)
        if (info->cbuf_mask & (1u << i)) sp(&args, ", constant uint4 *cb%d [[buffer(%d)]]", i, i);
    if (info->uses_gmem) {
        if (info->cbuf_mask & (1u << GMEM_BUFFER_INDEX)) {
            free(args.buf);
            args = (Str){0};  // (was a use after free + double free of args.buf)
            fail(c, "cbuf %d clashes with gmem", GMEM_BUFFER_INDEX);
        }
        sp(&args, ", device uchar *gmem [[buffer(%d)]]", GMEM_BUFFER_INDEX);
    }
    if (c->uses_rscale && stage != STAGE_VERTEX) {
        sp(&args, ", constant float4 *rscale [[buffer(18)]]");
        info->uses_rscale = true;
    }
    // Runtime highlight (e.g. golden unchecked pots): fragment shaders writing a float color target 0 get a
    // per-draw tint, float4 [0] = colour.rgb + strength, [1] = added glow.rgb (strength 0 = untouched; < 0: the
    // Cosmetics recolour, emit_recolour).
    bool tint = stage == STAGE_FRAGMENT && !dbg && (c->omap_target & 0x7) == 0x7 && (!key || key->rt_kind[0] == 0);
    if (tint) {
        sp(&args, ", constant float4 *ltint [[buffer(19)]]");
        info->uses_tint = true;
    }
    // Metal has 16 sampler slots a stage (textures: 31). Up to 16 textures, texture i samples with sampler i. With more
    // (issue #215: a cave floor's fragment shader with 17), the samplers come as one array of the 16 slots and a table
    // (buffer SMAP_BUFFER_INDEX) names each texture's slot: the renderer puts each distinct sampler state the draw uses
    // in a slot of its own, so every texture keeps its own filtering and wrapping (renderer.m sampler_slots).
    bool smap = info->ntex > MSL_MAX_SAMPLERS;  // (not in vertex shaders: refused above)
    if (smap) sp(&args, ", constant uint *smap [[buffer(%d)]], array<sampler, %d> ss [[sampler(0)]]", SMAP_BUFFER_INDEX,
                 MSL_MAX_SAMPLERS);
    for (int i = 0; i < info->ntex; i++) {
        ShaderTexture *t = &info->tex[i];
        const char *tn = tex_type_name(t->type);
        sp(&args, ", %s%s<float> t%d [[texture(%d)]]", t->shadow ? "depth" : "texture", tn, i, i);
        if (!smap) sp(&args, ", sampler s%d [[sampler(%d)]]", i, i);
    }
    if (stage == STAGE_VERTEX) {
        sp(&s, "vertex VOut main0(VIn in [[stage_in]], uint vid [[vertex_id]], uint iid [[instance_id]]%s%s) {\n",
           (maxwell_dbg_vs && blob == maxwell_dbg_vs) ? ", device float4 *dbgout [[buffer(29)]]" : "",
           args.buf ? args.buf : "");
        sp(&s, "  VOut out = {};\n  out.pos = float4(0, 0, 0, 1);\n");
        for (int i = 0; i < 32; i++)
            if (c->out_attr_mask & (1u << i)) sp(&s, "  out.v%d = float4(0, 0, 0, 1);\n", i);
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "fragment FOut main0(FIn in [[stage_in]], bool front_facing [[front_facing]]%s%s%s) {\n",
           c->uses_lane ? ", uint lane [[thread_index_in_simdgroup]]" : "",
           c->uses_layer ? ", uint rt_layer [[render_target_array_index]]" : "", args.buf ? args.buf : "");
    } else {
        sp(&s, "kernel void main0(uint3 tid [[thread_position_in_threadgroup]], uint3 ctaid [[threadgroup_position_in_grid]]%s) {\n",
           args.buf ? args.buf : "");
    }
    free(args.buf);
    if (smap)
        for (int i = 0; i < info->ntex; i++) sp(&s, "  sampler s%d = ss[smap[%d]];\n", i, i);
    for (int i = 0; i < 256; i++)
        if (c->reg_used[i]) sp(&s, "  uint r%d = 0;\n", i);
    for (int i = 0; i < 7; i++)
        if (c->pred_used[i]) sp(&s, "  bool p%d = false;\n", i);
    if (c->uses_cc) sp(&s, "  uint cc = 0; bool ccz = false, ccs = false;\n");
    if (c->uses_local) sp(&s, "  uint lmem[%d] = {};\n", LMEM_WORDS);
    if (dbg) sp(&s, maxwell_dbg_pc == 0xffffffffu ? "  float dbgv = 1.0f;\n" : "  float dbgv = -1.0f;\n");
    if (any_branch) {
        sp(&s, "  uint pc = 8u; uint ssy_0 = 0u; uint ssy_1 = 0u; uint ssy_2 = 0u; uint ssy_3 = 0u; uint ssy_4 = 0u; uint ssy_5 = 0u; uint ssy_6 = 0u; uint ssy_7 = 0u; uint ssy_8 = 0u; uint ssy_9 = 0u; uint ssy_10 = 0u; uint ssy_11 = 0u; uint ssy_12 = 0u; uint ssy_13 = 0u; uint ssy_14 = 0u; uint ssy_15 = 0u; uint pbk_0 = 0u; uint pbk_1 = 0u; uint pbk_2 = 0u; uint pbk_3 = 0u; uint pbk_4 = 0u; uint pbk_5 = 0u; uint pbk_6 = 0u; uint pbk_7 = 0u; uint pbk_8 = 0u; uint pbk_9 = 0u; uint pbk_10 = 0u; uint pbk_11 = 0u; uint pbk_12 = 0u; uint pbk_13 = 0u; uint pbk_14 = 0u; uint pbk_15 = 0u; uint pcnt_0 = 0u;\n");
        sp(&s, "  bool exit_now = false;\n");
        sp(&s, "  for (uint guard = 0; guard < 100000u; guard++) {\n  switch (pc) {\n");
        sp(&s, "%s", c->body.buf ? c->body.buf : "");
        sp(&s, "    exit_now = true; break;\n  default: exit_now = true; break;\n  }\n  if (exit_now) break;\n  }\n");
    } else {
        sp(&s, "  do {\n%s  } while (false);\n", c->body.buf ? c->body.buf : "");
    }
    if (stage == STAGE_VERTEX) {
        if (key && key->flip_y) sp(&s, "  out.pos.y = -out.pos.y;\n");
        if (key && key->flip_x) sp(&s, "  out.pos.x = -out.pos.x;\n");
        if (key && key->depth_minus_one) sp(&s, "  out.pos.z = (out.pos.z + out.pos.w) * 0.5f;\n");
        if (maxwell_dbg_vs && blob == maxwell_dbg_vs) sp(&s, "  if (iid < 64u && vid < 8u) { dbgout[iid * 8u + vid] = out.pos; dbgout[512u + iid * 8u + vid] = %s; }\n", (c->out_attr_mask & 2u) ? "out.v1" : "float4(0)");
        sp(&s, "  return out;\n}\n");
    } else if (stage == STAGE_FRAGMENT) {
        sp(&s, "  FOut o;\n");
        int r = 0;
        for (int rt = 0; rt < 8; rt++) {
            uint32_t m = (c->omap_target >> (rt * 4)) & 0xF;
            if (!m) continue;
            int kind = key ? key->rt_kind[rt] : 0;
            for (int comp = 0; comp < 4; comp++) {
                if (!(m & (1u << comp))) continue;
                if (dbg) {
                    sp(&s, comp == 3 ? "  o.c%d[%d] = 1.0f;\n" : "  o.c%d[%d] = dbgv;\n", rt, comp);
                } else if (r < 256 && c->reg_used[r]) {
                    if (kind == 0) sp(&s, "  o.c%d[%d] = as_type<float>(r%d);\n", rt, comp, r);
                    else sp(&s, "  o.c%d[%d] = r%d;\n", rt, comp, r);
                } else {
                    sp(&s, "  o.c%d[%d] = 0;\n", rt, comp);
                }
                r++;
            }
        }
        if (tint)
            sp(&s, "  if (ltint[0].w > 0.0f) { float3 g = o.c0.rgb; float l = dot(g, float3(0.299f, 0.587f, 0.114f));\n"
                   "    o.c0.rgb = mix(g, ltint[0].rgb * pow(l, ltint[1].w) + ltint[1].rgb * l, ltint[0].w); }\n");
        if (tint) emit_recolour(&s, "o.c0", "float3", "float4");
        if (c->omap_depth) {
            if (c->reg_used[r + 1]) sp(&s, "  o.depth = as_type<float>(r%d);\n", r + 1);
            else sp(&s, "  o.depth = in.pos.z;\n");
        }
        sp(&s, "  return o;\n}\n");
    } else {
        sp(&s, "  return;\n}\n");
    }
    info->msl = s.buf;
    info->attr_mask = c->in_attr_mask;
    info->out_mask = stage == STAGE_FRAGMENT ? c->omap_target : c->out_attr_mask;
    free(c->body.buf);
    free(c);
    return info;
}

void maxwell_free(ShaderInfo *s) {
    if (!s) return;
    free(s->msl);
    free(s);
}

void maxwell_disassemble(const uint8_t *code, size_t max_size, int stage, void *file) {
    (void)code; (void)max_size; (void)stage; (void)file;
}

// Disk cache of translations: cache/maxwell/<key>.bin, the ShaderInfo and its source. A known program then costs a
// file read instead of a translation - ~4 ms each, and the first frame of an area or an effect can wait for dozens of
// them in a row (vulkan/exec.c wait_built), every session: the SPIR-V and pipeline caches only cover the steps after
// this one. The key: the translator's id, the stage, max_size, the guest memory window baked into the source, the
// ShaderKey and the program's bytes to its end. Another translator empties the folder first (its translations would
// never be read again: cache/maxwell/build names the one they're from). The id is the Makefile's hash of this file and
// shader.h (TRANSLATOR_ID), so a release or rebuild that leaves the translator alone keeps the translations (the build
// time it was before emptied them with every update: a session of translation hitches); builds without it (tools,
// tests) use their build time. A program with a BRX (its
// jump table is read from past the code) or a header past max_size, and the debug switches, go uncached. On when the
// runtime sets maxwell_disk_cache (not in the tools and tests); LARECOMP_NO_TRANSLATE_CACHE=1: off (and with
// LARECOMP_SHADER_FLAT set, which changes the translations without changing the key).
// (called under the runtime's translate lock, as the translator itself)
bool maxwell_disk_cache;
#ifdef SHADER_TRANSLATOR_ID
static const char cache_build[] = SHADER_TRANSLATOR_ID;
#else
static const char cache_build[] = __DATE__ " " __TIME__;
#endif

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= ((const uint8_t *)p)[i]; h *= 0x100000001B3ull; }
    return h;
}

// the folder, emptied when its translations are from another build
static void cache_open(void) {
#ifdef _WIN32
    _mkdir("cache"), _mkdir("cache/maxwell");
#else
    mkdir("cache", 0755), mkdir("cache/maxwell", 0755);
#endif
    char had[sizeof cache_build] = {0};
    FILE *f = fopen("cache/maxwell/build", "rb");
    if (f) { if (fread(had, 1, sizeof had, f) != sizeof had) had[0] = 0; fclose(f); }
    if (!memcmp(had, cache_build, sizeof had)) return;
    DIR *d = opendir("cache/maxwell");
    for (struct dirent *e; d && (e = readdir(d));) {
        char path[300];
        snprintf(path, sizeof path, "cache/maxwell/%s", e->d_name);
        if (strstr(e->d_name, ".bin")) remove(path);
    }
    if (d) closedir(d);
    if ((f = fopen("cache/maxwell/build", "wb"))) { fwrite(cache_build, 1, sizeof cache_build, f); fclose(f); }
}

static uint64_t cache_key(const uint8_t *blob, size_t max_size, int stage, const ShaderKey *key) {
    static int on = -1;
    if (on < 0) {
        on = maxwell_disk_cache && !getenv("LARECOMP_NO_TRANSLATE_CACHE") && !getenv("LARECOMP_NO_DISCARD") &&
             !getenv("LARECOMP_LANE_COMPUTE_ONLY") && !getenv("LARECOMP_BRX_ALL_LABELS") && !getenv("LARECOMP_SAVE_MSL") &&
             !getenv("LARECOMP_SHADER_FLAT");  // (its translations aren't in the key: a kill switch must translate)
        if (on) cache_open();
    }
    if (!on || !key || maxwell_dbg_code || maxwell_dbg_vs) return 0;
    const uint8_t *code = code_start(blob, stage);
    if (max_size <= (size_t)(code - blob)) return 0;
    size_t end = code_end(blob, code, max_size);
    for (size_t i = 0; i < end; i++)
        if ((((const uint64_t *)code)[i] & 0xfff0000000000000ull) == 0xe250000000000000ull) return 0;  // BRX
    uint64_t v[4] = {(uint64_t)stage, max_size, maxwell_gmem_base, maxwell_gmem_size};
    uint64_t h = fnv(fnv(fnv(0xcbf29ce484222325ull, cache_build, sizeof cache_build), v, sizeof v), key, sizeof *key);
    h = fnv(h, blob, (size_t)(code - blob) + end * 8);
    return h ? h : 1;
}

static ShaderInfo *cache_load(uint64_t h) {
    char path[64];
    snprintf(path, sizeof path, "cache/maxwell/%016llx.bin", (unsigned long long)h);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    ShaderInfo *info = malloc(sizeof *info);
    uint32_t len = 0;
    char *src = NULL;
    bool ok = fread(info, sizeof *info, 1, f) == 1 && fread(&len, 4, 1, f) == 1 && len < (64u << 20) &&
              (src = malloc(len + 1)) && fread(src, 1, len, f) == len;
    fclose(f);
    if (!ok) { free(info); free(src); return NULL; }
    src[len] = 0;
    info->msl = src;
    return info;
}

static void cache_store(uint64_t h, const ShaderInfo *info) {
    char path[64], tmp[96];
    snprintf(path, sizeof path, "cache/maxwell/%016llx.bin", (unsigned long long)h);
    snprintf(tmp, sizeof tmp, "%s.%p", path, (const void *)info);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    uint32_t len = (uint32_t)strlen(info->msl);
    bool ok = fwrite(info, sizeof *info, 1, f) == 1 && fwrite(&len, 4, 1, f) == 1 && fwrite(info->msl, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;  // (a full disk can fail only at the flush)
    if (!ok || rename(tmp, path) != 0) remove(tmp);  // (written meanwhile by another thread: theirs is the same)
}

ShaderInfo *maxwell_translate(const uint8_t *blob, size_t max_size, int stage, const ShaderKey *key) {
    uint64_t h = cache_key(blob, max_size, stage, key);
    ShaderInfo *info = h ? cache_load(h) : NULL;
    if (info) return info;
    info = translate(blob, max_size, stage, key);
    if (h && info->msl) cache_store(h, info);
    return info;
}

ShaderInfo *maxwell_empty_fragment(void) {
    ShaderInfo *info = calloc(1, sizeof *info);
    info->stage = STAGE_FRAGMENT;
    info->msl = strdup("#version 460\nvoid main() {}\n");
    return info;
}

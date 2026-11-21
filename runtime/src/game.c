/* Natives for the game's own hot code (runtime/natives.txt): measured first,
 * then replaced where the recompiled code is too slow for the Vita. */
#include "runtime.h"

#include <arm_neon.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
static uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }
#else
#include <time.h>
static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
#endif

/* Hot natives read the guest memory base once (GM_BEGIN): through the global
 * the compiler reloads it after every store to guest memory. A guest pointer's
 * low 32 bits are the address. */
#define GM_BEGIN uint8_t *const gm_ = g_mem
#define GP(a) ((void *)(gm_ + (uint32_t)(a)))
#define lp(a) ((uint64_t)rt_ld32(GP(a)))
#define ldfp(a) ({ float f_; memcpy(&f_, GP(a), 4); f_; })
#define stfp(a, v) do { float f_ = (v); memcpy(GP(a), &f_, 4); } while (0)

/* 64- and 24-byte float blocks into guest memory (any alignment) with NEON:
 * GCC's memcpy for them is a loop of four single stores per 16 bytes */
static inline void put_mat(void *dst, const float *m)
{
    float *d = dst;
    const float32x4_t a = vld1q_f32(m), b = vld1q_f32(m + 4), c = vld1q_f32(m + 8), e = vld1q_f32(m + 12);
    vst1q_f32(d, a);
    vst1q_f32(d + 4, b);
    vst1q_f32(d + 8, c);
    vst1q_f32(d + 12, e);
}
/* a draw call (0x220 bytes) with NEON: memcpy goes to sceClibMemcpy */
static inline void copy_dc(void *dst, const void *src)
{
    uint8_t *d = dst;
    const uint8_t *q = src;
    for (int i = 0; i < 0x220; i += 32) {
        const uint8x16_t a = vld1q_u8(q + i), b = vld1q_u8(q + i + 16);
        vst1q_u8(d + i, a);
        vst1q_u8(d + i + 16, b);
    }
}
static inline void put_box(void *dst, const float *b)
{
    float *d = dst;
    vst1q_f32(d, vld1q_f32(b));
    vst1_f32(d + 4, vld1_f32(b + 4));
}

/* MINA_FPS: fixed updates (Game::FixedUpdate) and updates per presented frame,
 * and the time spent in them */
uint32_t g_fixed_updates, g_updates;
uint64_t g_fixed_us, g_update_us;
uint32_t g_composites, g_bounds;        /* calls (MINA_FPS) */
uint32_t g_recalcs, g_recalc_same;      /* RecalcWorldTransform runs, and those whose matrix came out unchanged (MINA_RECALC_STATS) */
/* gameplay (as opposed to menus and cut-scenes): fixed updates moving
 * thousands of transforms for a second; MINA_INPUT_UNTIL_PLAY stops the
 * scripted keys there */
int g_in_play;
static uint32_t composites_total, recalcs_total;

void f_99f430_orig(Ctx *c);
void nat__ZN4Game11FixedUpdateEv(Ctx *c)
{
    uint64_t t0 = now_us();
    uint32_t before = recalcs_total;
    g_fixed_updates++;
    f_99f430_orig(c);
    g_fixed_us += now_us() - t0;
    static int heavy;          /* consecutive fixed updates moving thousands of transforms */
    heavy = recalcs_total - before > 1500 ? heavy + 1 : 0;
    if (heavy == 20 && !g_in_play) {
        g_in_play = 1;
        rt_log("[play] gameplay (%u world transforms in a fixed update)\n", recalcs_total - before);
    }
}

void f_9a0638_orig(Ctx *c);
void nat__ZN4Game6UpdateEf(Ctx *c)
{
    uint64_t t0 = now_us();
    g_updates++;
    f_9a0638_orig(c);
    g_update_us += now_us() - t0;
}

/* ---- ycTransform ----------------------------------------------------------------------
 * 40 bytes: rotation quaternion q[4] (x y z w), scale s[3], translation t[3]. The
 * natives below do what the game's code does, operation for operation (the same
 * float results: the file is built with -ffp-contract=off). */
typedef struct XForm {
    float q[4], s[3], t[3];
} XForm;

/* o = b composed under a (flags pick the parts), then o.q normalized.
 * x0 = out, x1 = a, x2 = b, x3 = flags (the code addresses them like this,
 * whatever its symbol says) */
static void composite(Ctx *c);
void f_1e4cdc_orig(Ctx *c);

/* MINA_VERIFY_XFORM: each call also runs the game's code on a scratch copy and
 * compares (logs the first mismatches) */
static int verify_xform(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("MINA_VERIFY_XFORM") != NULL;
    return on;
}
static void verify_against(Ctx *c, void (*nat)(Ctx *), void (*orig)(Ctx *), const char *name, uint32_t out_size)
{
    static uint32_t scratch, checked, bad;
    if (!scratch)
        scratch = gmalloc(4096);
    Ctx save = *c;
    uint32_t out = (uint32_t)c->x[0];
    memcpy(G(scratch), G(out), out_size);          /* the output's previous contents, for the original */
    nat(c);
    uint8_t mine[256];
    memcpy(mine, G(out), out_size);
    memcpy(G(out), G(scratch), out_size);
    *c = save;
    orig(c);
    checked++;
    if (memcmp(mine, G(out), out_size) != 0 && bad++ < 10) {
        const float *m = (const float *)mine, *g = G(out);
        rt_log("[verify] %s differs (%u of %u):", name, bad, checked);
        for (uint32_t i = 0; i < out_size / 4; i++)
            rt_log(" %g/%g", m[i], g[i]);
        rt_log(" flags %#llx\n", (unsigned long long)save.x[3]);
    }
    if ((checked & 0xfffff) == 0)
        rt_log("[verify] %s: %u calls checked, %u differ\n", name, checked, bad);
}

void nat__ZNK11ycTransform9CompositeERKS_y(Ctx *c)
{
    g_composites++;
    composites_total++;
    if (verify_xform()) {
        verify_against(c, composite, f_1e4cdc_orig, "ycTransform::Composite", sizeof(XForm));
        return;
    }
    composite(c);
}

static void compose(XForm *o, const XForm *a, const XForm *bp, uint64_t f);
static void composite(Ctx *c)
{
    compose(G(c->x[0]), G(c->x[1]), G(c->x[2]), c->x[3]);
}

/* o may be a: b is copied into o first, a is read after (as the game does) */
/* exactly (+0, +0, +0, 1): the product of two of them is one again, bit for
 * bit, and normalizing one changes nothing (n = 1, 1/sqrt(1) = 1) */
static inline int q_identity(const float *q)
{
    uint32_t b[4];
    memcpy(b, q, 16);
    return (b[0] | b[1] | b[2]) == 0 && b[3] == 0x3f800000u;
}
/* rotated by the identity, t gains 2 (±0) per component: no change unless a
 * component is -0 (-0 + +0 is +0) or not finite (0 * inf is NaN) */
static inline int t_plain(const float *t)
{
    uint32_t b[3];
    memcpy(b, t, 12);
    for (int i = 0; i < 3; i++)
        if (b[i] == 0x80000000u || (b[i] & 0x7f800000u) == 0x7f800000u)
            return 0;
    return 1;
}
static void compose(XForm *o, const XForm *a, const XForm *bp, uint64_t f)
{
    const XForm b = *bp;     /* a struct copy: no library call */
    *o = b;
    int scale_t = 0;
    if (!(f & 0x10)) {
        if (!(f & 0x80)) {
            o->s[0] = a->s[0] * o->s[0];
            o->s[1] = a->s[1] * o->s[1];
            o->s[2] = a->s[2] * o->s[2];
        }
        if ((f & 0x48) == 0)
            scale_t = 1;
    }
    if (scale_t) {
        o->t[0] = a->s[0] * o->t[0];
        o->t[1] = a->s[1] * o->t[1];
        o->t[2] = a->s[2] * o->t[2];
    }
    const int a_id = q_identity(a->q);
    if (!(f & 0x20)) {
        if (!(f & 0x200) && !(a_id && q_identity(o->q))) {
            const float ox = o->q[0], oy = o->q[1], oz = o->q[2], ow = o->q[3];
            const float ax = a->q[0], ay = a->q[1], az = a->q[2], aw = a->q[3];
            const float w = ow * aw - ((ox * ax + oy * ay) + oz * az);
            const float zz = aw * oz + ow * az;
            o->q[0] = (ox * aw + ax * ow) + (oz * ay - oy * az);
            o->q[1] = (oy * aw + ay * ow) + (ox * az - oz * ax);
            o->q[2] = (oy * ax - ox * ay) + zz;
            o->q[3] = w;
        }
        if (!(f & 0x108) && !(a_id && t_plain(o->t))) {
            /* t rotated by a.q: t + 2 (w (u x t) + u x (u x t)) */
            const float ax = a->q[0], ay = a->q[1], az = a->q[2], aw = a->q[3];
            const float tx = o->t[0], ty = o->t[1], tz = o->t[2];
            const float cx = ay * tz - az * ty, cy = az * tx - ax * tz, cz = ax * ty - ay * tx;
            float rx = cx * aw + (ay * cz - az * cy);
            float rz = cz * aw + (ax * cy - ay * cx);
            float ry = cy * aw + (az * cx - ax * cz);
            rx = rx + rx;
            rz = rz + rz;
            ry = ry + ry;
            o->t[0] = tx + rx;
            o->t[1] = ty + ry;
            o->t[2] = tz + rz;
        }
    }
    if (!(f & 0x8)) {
        o->t[0] = a->t[0] + o->t[0];
        o->t[1] = a->t[1] + o->t[1];
        o->t[2] = a->t[2] + o->t[2];
    }
    if (q_identity(o->q))
        return;
    float sq0 = o->q[0] * o->q[0], sq1 = o->q[1] * o->q[1], sq2 = o->q[2] * o->q[2], sq3 = o->q[3] * o->q[3];
    float n = ((sq0 + sq1) + sq2) + sq3;
    if (n != 0.0f) {
        float inv = 1.0f / sqrtf(n);
        o->q[0] *= inv;
        o->q[1] *= inv;
        o->q[2] *= inv;
        o->q[3] *= inv;
    }
}

/* ---- ycCutterAnimComponent::UpdateBounds ------------------------------------------------
 * The frame's box (center x/y, size w/h at +0x30 of the object at this+0x48, if this+0x40
 * and this+0x48 are set), its corners through the 4x4 matrix at this+0x60 (rows of 4
 * floats, y negated, z 0), min/max per axis: center and half extents (6 floats) into
 * the boxes at this+0x80 and, up to the first null, +0x88, +0x90, +0x98. */
static uint64_t ld_ptr(uint64_t a)
{
    uint64_t v;
    memcpy(&v, G(a), 8);
    return v;
}
/* one row's contribution to an axis: lo/hi products, ordered as the game does */
static inline void minmax(float lo, float hi, float *mn, float *mx)
{
    int m = hi > lo;
    *mn = m ? lo : hi;
    *mx = m ? hi : lo;
}
static void update_bounds(Ctx *c)
{
    GM_BEGIN;
    const uint64_t self = c->x[0];
    const float *m = GP(lp(self + 0x60));
    float box[6];
    uint64_t src = lp(self + 0x40) ? lp(self + 0x48) : 0;
    const float *b = src ? GP(src + 0x30) : NULL;
    float hw = 0, hh = 0;
    if (b) {
        hw = b[2] * 0.5f;
        hh = b[3] * 0.5f;
    }
    if (b && !(hw < 0.0f) && !(hh < 0.0f)) {
        const float cx = b[0], ny = -b[1], hz = 0.0f, zero = 0.0f;
        const float xl = cx - hw, xh = cx + hw, yl = ny - hh, yh = ny + hh, zl = zero - hz, zh = hz + zero;
        float mn[3][3], mx[3][3];          /* [row][axis] */
        const float lo[3] = { xl, yl, zl }, hi[3] = { xh, yh, zh };
        for (int r = 0; r < 3; r++)
            for (int a = 0; a < 3; a++)
                minmax(m[r * 4 + a] * lo[r], m[r * 4 + a] * hi[r], &mn[r][a], &mx[r][a]);
        for (int a = 0; a < 3; a++) {
            const float t = m[12 + a];
            float hi_ = mx[2][a] + (mx[1][a] + (mx[0][a] + t));
            float lo_ = mn[2][a] + (mn[1][a] + (mn[0][a] + t));
            float e = (hi_ - lo_) * 0.5f;
            box[a] = lo_ + e;
            box[3 + a] = e;
        }
    } else {
        for (int a = 0; a < 3; a++) {
            const float t = m[12 + a];
            float e = (t - t) * 0.5f;
            box[a] = t + e;
            box[3 + a] = e;
        }
    }
    put_box(GP(lp(self + 0x80)), box);
    for (int k = 1; k < 4; k++) {
        uint64_t p = lp(self + 0x80 + 8 * k);
        if (!p)
            break;
        put_box(GP(p), box);
    }
}
void f_d3e24_orig(Ctx *c);
void nat__ZN21ycCutterAnimComponent12UpdateBoundsEv(Ctx *c)
{
    g_bounds++;
    if (verify_xform()) {
        /* the outputs are the boxes the object points at */
        static uint32_t checked, bad;
        uint64_t self = c->x[0];
        uint64_t outs[4] = { 0 };
        float before[4][6], mine[4][6];
        int n = 0;
        for (; n < 4; n++) {
            outs[n] = ld_ptr(self + 0x80 + 8 * n);
            if (!outs[n])
                break;
            memcpy(before[n], G(outs[n]), 24);
        }
        Ctx save = *c;
        update_bounds(c);
        for (int i = 0; i < n; i++) {
            memcpy(mine[i], G(outs[i]), 24);
            memcpy(G(outs[i]), before[i], 24);
        }
        *c = save;
        f_d3e24_orig(c);
        checked++;
        for (int i = 0; i < n; i++)
            if (memcmp(mine[i], G(outs[i]), 24) != 0 && bad++ < 10) {
                const float *g = G(outs[i]);
                rt_log("[verify] UpdateBounds box %d differs: %g %g %g %g %g %g / %g %g %g %g %g %g\n", i, mine[i][0],
                       mine[i][1], mine[i][2], mine[i][3], mine[i][4], mine[i][5], g[0], g[1], g[2], g[3], g[4], g[5]);
            }
        if ((checked & 0xfffff) == 0)
            rt_log("[verify] UpdateBounds: %u calls checked, %u differ\n", checked, bad);
        return;
    }
    update_bounds(c);
}

/* ---- IsoAnim::CalcTransform ----------------------------------------------------------------
 * x0 out, x1 the iso parameters P, x2 transform A, x3 transform B, x4 flags,
 * w5/w6 bools. With w5 and P's mode (P+0xc) set, B's z comes from
 * IsoAnim::GetIsoDepth (the game's code) and A's z is dropped before the
 * composite. */
static float iso_depth(uint64_t comp, uint32_t mode, float a, float b, float cc);
void f_87363c_orig(Ctx *c);
static void calc_transform_p(Ctx *c, XForm *o, uint64_t pa, const XForm *ap, const XForm *bp, uint64_t flags,
                             uint32_t w5, uint32_t w6)
{
    GM_BEGIN;
    const uint32_t mode = w5 ? rt_ld32(GP(pa + 0xc)) : 0;
    if (!mode) {
        compose(o, ap, bp, flags);
        return;
    }
    const float *p = GP(pa);
    XForm b = *bp;
    const float bz = b.t[2];
    const float ty = w6 ? b.t[1] : -0.0f;
    /* GetIsoDepth(x0 = P+0x18, x1 = mode, s0 = A.t[1] + P[0] + (B.t[1] or -0), s1 = P[4], s2 = P[2]) */
    const float depth = iso_depth(lp(pa + 0x18), mode, (ap->t[1] + p[0]) + ty, p[4], p[2]);
    b.t[2] = depth - p[1];
    XForm a = *ap;
    const uint32_t pflags = rt_ld32(GP(pa + 0x14));
    const float az = (a.t[2] < 0.0f && (pflags >> 8 & 1)) ? 0.0f : a.t[2];
    a.t[2] = az;
    if (mode == 1) {
        b.t[2] = bz;
        compose(o, &a, &b, flags);
        return;
    }
    a.t[2] = 0.0f;
    compose(o, &a, &b, flags);
    if (mode != 8 && !(pflags >> 10 & 1))
        o->t[1] = bz + (az + o->t[1]);
}
static void calc_transform(Ctx *c)
{
    calc_transform_p(c, G(c->x[0]), c->x[1], G(c->x[2]), G(c->x[3]), c->x[4], (uint32_t)c->x[5], (uint32_t)c->x[6]);
}
void nat__ZN7IsoAnim13CalcTransformERK11ycTransformS2_ybb(Ctx *c)
{
    if (verify_xform()) {
        verify_against(c, calc_transform, f_87363c_orig, "IsoAnim::CalcTransform", sizeof(XForm));
        return;
    }
    calc_transform(c);
}

/* ---- GameAnim::RecalcWorldTransform -------------------------------------------------------
 * x0 the GameAnim O, x1 the world transform W. The parent's transform (O+0x200)
 * composed with the local one (O+0x180), the iso transform (IsoAnim::CalcTransform),
 * a virtual hook (O+0x2b8, slot 0x60) that may move it, then the 4x4 matrix
 * (rotation x scale, translation) into up to 4 places and the bounds. Fields
 * O+0x1d0/0x1d8/0x1e0 are only changed for the duration. The locals the hook
 * sees are in a guest stack frame, as in the game. */
static inline float ldf(uint64_t a) { float f; memcpy(&f, G(a), 4); return f; }
static inline void stf(uint64_t a, float f) { memcpy(G(a), &f, 4); }
/* MINA_NO_NATIVES=1: the game's functions made native run recompiled;
 * =a,b,...: only those whose names contain one of the words */
int rt_native_off(const char *name)
{
    const char *v = getenv("MINA_NO_NATIVES");
    int off = 0;
    if (v && strcmp(v, "1") == 0)
        off = 1;
    else if (v) {
        char buf[256];
        snprintf(buf, sizeof buf, "%s", v);
        char *save = NULL;
        for (char *w = strtok_r(buf, ",", &save); w && !off; w = strtok_r(NULL, ",", &save))
            off = strstr(name, w) != NULL;
    }
    if (off)
        rt_log("[natives] %s runs recompiled\n", name);
    return off;
}
void f_9a65c4_orig(Ctx *c);
static void recalc_world_transform(Ctx *c)
{
    GM_BEGIN;
    const uint64_t O = c->x[0];
    const XForm *W = GP(c->x[1]);
    const uint64_t saved_sp = c->sp;
    const uint64_t sp = saved_sp - 0xf0;
    c->sp = sp;
    XForm *L = GP(sp + 0x8), *Wc = GP(sp + 0x30), *R = GP(sp + 0x60);

    const uint64_t par = lp(O + 0x200);
    if (par && !(*(uint8_t *)GP(O + 0x1f8) & 2)) {
        rt_st32(GP(O + 0x1dc), rt_ld32(GP(par + 0xbc)));
        rt_st32(GP(O + 0x1e4), rt_ld32(GP(O + 0x1e4)) | 0x20);
    }
    const float s8 = ldfp(O + 0x1d8);
    uint32_t w22 = rt_ld32(GP(O + 0x1e4));
    float s11 = s8;
    if ((w22 & (1u << 20)) && rt_ld32(GP(O + 0x1dc)) != 4) {
        s11 = -s8;
        stfp(O + 0x1d8, s11);
    }
    const float s9 = ldfp(O + 0x1d0), s10 = ldfp(O + 0x1e0);

    /* the layer: O+0x1e0 (when 0) from the depth of the parent's or own sprite */
    int layer = 0;            /* 0: keep, 1: from x8 below, 2: 1.0 */
    uint64_t x8 = 0;
    if (par) {
        compose(R, GP(par + 0x78), GP(O + 0x180), lp(O + 0x18));
        const uint32_t f = rt_ld32(GP(O + 0x1f8));
        if (!(f & 1))
            stfp(O + 0x1d0, ldfp(par + 0xc0) + s9);
        if (!(f & 2)) {
            rt_st32(GP(O + 0x1dc), rt_ld32(GP(par + 0xbc)));
            w22 |= 0x20;
            rt_st32(GP(O + 0x1e4), w22);
            stfp(O + 0x1d8, ldfp(par + 0xc4) + s11);
        }
        if (f & 0x20) {
            if (s10 == 0.0f) {
                x8 = lp(O + 0x248);
                layer = x8 && !(*(uint8_t *)GP(O + 0x1f8) & 0x40) ? 1 : 2;
                if (!x8)
                    layer = 3;               /* 1.0 stored as is */
                else if (layer == 2)
                    layer = 3;
            }
        } else {
            x8 = lp(par + 0x50);
            layer = x8 ? 1 : 2;
        }
    } else {
        *R = *(const XForm *)GP(O + 0x180);
        if (s10 == 0.0f) {
            x8 = lp(O + 0x248);
            layer = x8 && !(*(uint8_t *)GP(O + 0x1f8) & 0x40) ? 1 : 3;
        }
    }
    if (layer) {
        float v = 1.0f;
        if (layer == 1) {
            int32_t d = (int32_t)rt_ld32(GP(x8 + 0x15c));
            if (d > 10)
                d -= 10;
            d += 2;
            v = (float)(int32_t)((float)d * 0.5f);
        }
        stfp(O + 0x1e0, layer == 3 ? v : s10 + v);
    }

    *Wc = *W;
    if (*(uint8_t *)GP(O + 0x1cb)) {
        /* whole pixels: floor(t * k0 + 0.5) * k1 */
        const float k0 = ldfp(0x6039f5c), k1 = ldfp(0x6039f58);
        for (int i = 0; i < 2; i++) {
            Wc->t[i] = floorf(Wc->t[i] * k0 + 0.5f) * k1;
            R->t[i] = floorf(R->t[i] * k0 + 0.5f) * k1;
        }
    }
    memcpy(GP(O + 0x274), R->t, 12);
    calc_transform_p(c, L, O + 0x1d0, Wc, R, lp(O + 0x18), w22 >> 4 & 1, w22 >> 2 & 1);
    memcpy(GP(O + 0x25c), L->t, 12);
    stfp(sp + 4, 0.0f);
    float hook = 0.0f;
    const uint64_t h = lp(O + 0x2b8);
    if (h) {
        *(uint8_t *)GP(sp + 3) = 0;
        c->v[0] = vset_f32((rt_ld32(GP(O + 0x1e4)) & 0x1000000) ? -0.8f : 0.0f);
        c->x[0] = h;
        c->x[1] = O;
        c->x[2] = sp + 0x8 + 0x1c;
        c->x[3] = sp + 0x8 + 0x10;
        c->x[4] = sp + 4;
        c->x[5] = sp + 3;
        rt_call(c, lp(lp(h) + 0x60));
        if (!(*(uint8_t *)GP(O + 0x1e6) & 0x20)) {
            const float step = *(uint8_t *)GP(sp + 3) ? 10.0f : -10.0f;
            if (ldfp(O + 0x1e0) == 1.0f)
                L->t[2] = L->t[2] + step;
        }
        hook = ldfp(sp + 4);
    }
    memcpy(GP(O + 0x268), L->t, 12);

    /* rotation (L.q) x scale (L.s), translation (L.t) */
    const float x = L->q[0], y = L->q[1], z = L->q[2], w = L->q[3];
    const float xx = x * x, yy = y * y, zz = z * z, xz = x * z, xy = x * y, yz = y * z, xw = x * w, yw = y * w,
                zw = z * w;
    float m00 = yy + zz; m00 = m00 + m00; m00 = 1.0f - m00;
    float m01 = xy + zw; m01 = m01 + m01;
    float m02 = xz - yw; m02 = m02 + m02;
    float m10 = xy - zw; m10 = m10 + m10;
    float m11 = xx + zz; m11 = m11 + m11; m11 = 1.0f - m11;
    float m12 = yz + xw; m12 = m12 + m12;
    float m20 = xz + yw; m20 = m20 + m20;
    float m21 = yz - xw; m21 = m21 + m21;
    float m22 = xx + yy; m22 = m22 + m22; m22 = 1.0f - m22;
    const float sx = L->s[0], sy = L->s[1], sz = L->s[2];
    const float m[16] = { m00 * sx, m01 * sx, m02 * sx, 0.0f * sx, m10 * sy, m11 * sy, m12 * sy, 0.0f * sy,
                          m20 * sz, m21 * sz, m22 * sz, 0.0f * sz, L->t[0], L->t[1], L->t[2], 1.0f };
    {
        static int st = -1;
        if (st < 0)
            st = getenv("MINA_RECALC_STATS") != NULL;
        if (st) {
            g_recalcs++;
            if (!memcmp(GP(lp(O + 0x60)), m, 64))
                g_recalc_same++;
        }
    }
    stfp(lp(O + 0xa0) + 4, hook);
    for (int k = 1; k < 4; k++) {
        uint64_t p = lp(O + 0xa0 + 8 * k);
        if (!p)
            break;
        stfp(p + 4, hook);
    }
    put_mat(GP(lp(O + 0x60)), m);
    for (int k = 1; k < 4; k++) {
        uint64_t p = lp(O + 0x60 + 8 * k);
        if (!p)
            break;
        put_mat(GP(p), m);
    }
    c->x[0] = O;
    update_bounds(c);
    stfp(O + 0x1e0, s10);
    stfp(O + 0x1d0, s9);
    stfp(O + 0x1d8, s8);
    c->sp = saved_sp;
}
/* what it writes: the object, its 4 matrices, the 4 hook floats, the 4 boxes */
typedef struct RwtSnap {
    uint8_t obj[0x300];
    uint64_t mat[4], hk[4], box[4];
    float m[4][16], h[4], b[4][6];
} RwtSnap;
static void rwt_take(uint64_t O, RwtSnap *s)
{
    memcpy(s->obj, G(O), sizeof s->obj);
    for (int k = 0; k < 4; k++) {
        s->mat[k] = ld_ptr(O + 0x60 + 8 * k);
        s->hk[k] = ld_ptr(O + 0xa0 + 8 * k);
        s->box[k] = ld_ptr(O + 0x80 + 8 * k);
        if (s->mat[k]) memcpy(s->m[k], G(s->mat[k]), 64);
        if (s->hk[k]) memcpy(&s->h[k], G(s->hk[k] + 4), 4);
        if (s->box[k]) memcpy(s->b[k], G(s->box[k]), 24);
    }
}
static void rwt_put(uint64_t O, const RwtSnap *s)
{
    memcpy(G(O), s->obj, sizeof s->obj);
    for (int k = 0; k < 4; k++) {
        if (s->mat[k]) memcpy(G(s->mat[k]), s->m[k], 64);
        if (s->hk[k]) memcpy(G(s->hk[k] + 4), &s->h[k], 4);
        if (s->box[k]) memcpy(G(s->box[k]), s->b[k], 24);
    }
}
static int arm_defer(Ctx *c);
void nat__ZN8GameAnim20RecalcWorldTransformERK11ycTransformS2_(Ctx *c)
{
    recalcs_total++;
    if (arm_defer(c))
        return;
    if (verify_xform()) {
        static RwtSnap before, mine, theirs;
        static uint32_t checked, bad;
        const uint64_t O = c->x[0];
        rwt_take(O, &before);
        Ctx save = *c;
        recalc_world_transform(c);
        rwt_take(O, &mine);
        rwt_put(O, &before);
        *c = save;
        f_9a65c4_orig(c);
        rwt_take(O, &theirs);
        checked++;
        if (memcmp(&mine, &theirs, sizeof mine) != 0 && bad++ < 10) {
            const uint8_t *a = (const uint8_t *)&mine, *b = (const uint8_t *)&theirs;
            size_t i = 0;
            while (a[i] == b[i]) i++;
            rt_log("[verify] RecalcWorldTransform differs at snapshot byte %#zx (object %#zx)\n", i, i < 0x300 ? i : 0);
        }
        if ((checked & 0x3ffff) == 0)
            rt_log("[verify] RecalcWorldTransform: %u calls checked, %u differ\n", checked, bad);
        return;
    }
    recalc_world_transform(c);
}

/* ---- ycEntity::RefreshWorldTransform ---------------------------------------------------------
 * The entity's world transform (E+0xc0): its local one (E+0x80) as is, or (flag
 * bit 0 of E+0x18) composed under its frame: a provider object (E+0x60, virtual
 * slot 0, returns a transform), else the parent's world (E+0x10), else identity.
 * Then each active component of the pages at E+0x38 gets RecalcWorldTransform
 * (virtual slot 0x48) with the new and the previous world. */
void f_1cb43c_orig(Ctx *c);
/* MINA_CALLERS=n: every n calls of a counted native, the recompiled
 * functions that called it (by the host return address), most frequent first */
const char *guest_func_at_host(const void *pc);
typedef struct CallerHist { const char *what; uint32_t n, every, count[64]; const void *ra[64]; } CallerHist;
static void caller_count(CallerHist *h, const void *ra)
{
    if (!h->every) {
        const char *v = getenv("MINA_CALLERS");
        h->every = v && atoi(v) > 0 ? (uint32_t)atoi(v) : ~0u;
    }
    if (h->every == ~0u)
        return;
    uint32_t i;
    for (i = 0; i < 64 && h->ra[i] && h->ra[i] != ra; i++)
        ;
    if (i < 64) {
        h->ra[i] = ra;
        h->count[i]++;
    }
    if (++h->n % h->every)
        return;
    rt_log("[callers] %s, %u calls:\n", h->what, h->every);
    for (int k = 0; k < 10; k++) {
        uint32_t best = 0;
        for (i = 1; i < 64; i++)
            if (h->count[i] > h->count[best])
                best = i;
        if (!h->count[best])
            break;
        rt_log("[callers]   %6u  %s\n", h->count[best], guest_func_at_host(h->ra[best]));
        h->count[best] = 0;
    }
    memset(h->ra, 0, sizeof h->ra);
    memset(h->count, 0, sizeof h->count);
}

void nat__ZN8GameAnim20RecalcWorldTransformERK11ycTransformS2_(Ctx *c);
static int native_off_rwt;        /* MINA_NO_NATIVES covers GameAnim::RecalcWorldTransform: through its entry */
static void refresh_world_transform(Ctx *c)
{
    GM_BEGIN;
    {
        static int init;
        if (!init) {
            init = 1;
            int rt_native_off(const char *name);
            native_off_rwt = rt_native_off("__ZN8GameAnim20RecalcWorldTransformERK11ycTransformS2_");
        }
    }
    const uint64_t E = c->x[0];
    const uint64_t saved_sp = c->sp, sp = saved_sp - 0xb0;
    c->sp = sp;
    XForm *world = GP(E + 0xc0);
    XForm *P = GP(sp), *prev = GP(sp + 0x50);
    *prev = *world;
    uint64_t flags = lp(E + 0x18);
    if (!(flags & 1)) {
        *world = *(const XForm *)GP(E + 0x80);
    } else {
        const uint64_t prov = lp(E + 0x60);
        if (prov) {
            static const GuestFunc *ic;
            c->x[0] = prov;
            c->x[8] = sp;                        /* the transform comes back at sp */
            RT_CALL_IC(c, lp(lp(prov)), ic);
            flags = lp(E + 0x18);
        } else {
            const uint64_t parent = lp(E + 0x10);
            if (parent) {
                *P = *(const XForm *)GP(parent + 0xc0);
            } else {
                memcpy(P, GP(0x1d48610), 16);       /* identity: the game's constants */
                memcpy((uint8_t *)P + 16, GP(0x1d486d0), 16);
                memset((uint8_t *)P + 32, 0, 8);
            }
        }
        XForm r;
        compose(&r, P, GP(E + 0x80), flags);
        *world = r;
    }
    {   /* MINA_CALLERS: how the world transform changed */
        static uint32_t n, same, tonly, other;
        static int on = -1;
        if (on < 0)
            on = getenv("MINA_CALLERS") != NULL;
        if (on) {
            if (!memcmp(world, prev, sizeof *world))
                same++;
            else if (!memcmp(world, prev, 28))
                tonly++;
            else
                other++;
            if (++n % 20000 == 0) {
                rt_log("[callers] world transform: %u unchanged, %u only moved, %u rotated/scaled\n", same, tonly, other);
                same = tonly = other = 0;
            }
        }
    }
    for (uint64_t page = lp(E + 0x38); page; page = lp(page + 0x8)) {
        for (uint32_t i = 0; i < lp(page + 0x10); i++) {
            const uint64_t comp = lp(page + 0x18 + 8 * (uint64_t)i);
            const uint64_t f = lp(comp + 0x18);
            if (!(f & 1) || (f & 0x3000) == 0x1000)
                continue;
            static const GuestFunc *ic;
            {   /* MINA_CALLERS: which components' RecalcWorldTransform run */
                static CallerHist h = { "component RecalcWorldTransform" };
                static int on = -1;
                if (on < 0)
                    on = getenv("MINA_CALLERS") != NULL;
                if (on) {
                    GuestFn cpu_lookup(uint32_t a);
                    caller_count(&h, (const void *)cpu_lookup((uint32_t)lp(lp(comp) + 0x48)));
                }
            }
            const uint64_t fn = lp(lp(comp) + 0x48);
            if (fn == 0x109eb48)              /* ycComponent::RecalcWorldTransform: an empty function */
                continue;
            c->x[0] = comp;
            c->x[1] = E + 0xc0;
            c->x[2] = sp + 0x50;
            if (fn == 0x19a65c4 && !native_off_rwt)   /* GameAnim::RecalcWorldTransform: the native, directly */
                nat__ZN8GameAnim20RecalcWorldTransformERK11ycTransformS2_(c);
            else
                RT_CALL_IC(c, fn, ic);
        }
    }
    c->sp = saved_sp;
}
void nat__ZN8ycEntity21RefreshWorldTransformEb(Ctx *c)
{
    static CallerHist h = { "ycEntity::RefreshWorldTransform" };
    caller_count(&h, __builtin_return_address(0));
    refresh_world_transform(c);
}

/* ---- IsoAnim::GetIsoDepth --------------------------------------------------------------------
 * x0 component, w1 mode, s0 a, s1 b, s2 c: a depth (s0). Fixed per mode, or
 * from the camera's position against the level's (mode 4 and above 11). */
static inline float hexf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
void f_873480_orig(Ctx *c);
static float iso_depth(uint64_t comp, uint32_t mode, float a, float b, float cc)
{
    GM_BEGIN;
    float s3 = (cc + 10.0f) / 20.0f;
    s3 = fmin_f32(s3, 1.0f);
    s3 = fmax_f32(s3, 0.0f);
    float s0, s2;
    switch (mode) {
    case 0: case 1:
        return cc;
    case 2:
        return -cc;
    case 3:
        s0 = s3 * hexf(0x3eccccc0); s2 = 8.5f; break;
    case 5: case 8:
        s0 = s3 * hexf(0x3e4cccd0); s2 = 3.0f; break;
    case 6: case 9:
        s0 = s3 * hexf(0x3e0f5c30); s2 = hexf(0x40566666); break;
    case 7: case 10:
        s0 = s3 * hexf(0x3db85200); s2 = hexf(0x40333333); break;
    case 11:
        s0 = s3 * hexf(0x3db851e8); s2 = hexf(0x3f4ccccd); break;
    default: {
        const uint64_t lv = lp(lp(lp(comp + 0x10) + 0x50) + 0x16c0);
        const uint64_t q = lv + 0x6a88;
        const uint32_t k = rt_ld32(GP(q + 0x58));
        const uint64_t p = (k != 3 && k != 0) ? q + 0x60 : q;
        const float cam = ldfp(lv + 0x138);
        const float lo = cam + ldfp(p);
        float span = (ldfp(p) - cam) - lo;
        float d = a - lo;
        float m = span > 0.0f ? span : -span;
        m = fmaxnm_f32(m, hexf(0x322bcc77));
        m = span >= 0.0f ? m : -m;
        d = d / m;
        d = fmin_f32(d, 1.0f);
        d = fmax_f32(d, 0.0f);
        d = d * hexf(0x467ffc00);
        d = floorf(d);
        d = s3 + d;
        d = d * hexf(0x38800000);
        s0 = d * hexf(0x40999999);
        s2 = hexf(0x40666666);
        break;
    }
    }
    s0 = s0 + s2;
    const float b1 = b < 0.0f ? b : b + -1.0f;
    s0 = b1 * 10.0f + s0;
    return -s0;
}
static void get_iso_depth(Ctx *c)
{
    c->v[0] = vset_f32(iso_depth(c->x[0], (uint32_t)c->x[1], c->v[0].f[0], c->v[1].f[0], c->v[2].f[0]));
}
void nat__ZN7IsoAnim11GetIsoDepthEP11ycComponentRK6ycVec3fif(Ctx *c)
{
    if (verify_xform()) {
        static uint32_t checked, bad;
        Ctx save = *c;
        get_iso_depth(c);
        const float mine = c->v[0].f[0];
        *c = save;
        f_873480_orig(c);
        checked++;
        if (memcmp(&mine, &c->v[0].f[0], 4) != 0 && bad++ < 10)
            rt_log("[verify] GetIsoDepth differs: %g / %g (mode %u)\n", mine, c->v[0].f[0], (uint32_t)save.x[1]);
        if ((checked & 0xfffff) == 0)
            rt_log("[verify] GetIsoDepth: %u calls checked, %u differ\n", checked, bad);
        return;
    }
    get_iso_depth(c);
}

/* ---- ycRenderPass::Update ---------------------------------------------------------------------
 * For each of the 5 views (camera at RP+0x10+8v): the frustum planes of the
 * camera's two matrices, then every render object of the pages at RP+0x130:
 * the bounds of all of them (RP+0xb0+0x18v), a layer/flag filter, the planes,
 * and the visible ones into the view's lists (objects RP+0x218+8v, sort keys
 * RP+0x1f0+8v), their bounds (RP+0x150+0x18v) and count (RP+0x1c8+8v), sorted
 * by key unless the key function (RP+0x128) is the default one. */
void f_a33c0(Ctx *c);            /* ycMtx4::Inverted */
void f_1e0ca0(Ctx *c);           /* ycRenderPass::SetMaxVisibleObjects */
void f_1e13ac_orig(Ctx *c);

typedef struct { float x, y, z, w; } Plane;
typedef struct { float x, y, z, w, ax, ay, az; } PlaneA;
static inline float fabs_sel(float v) { return v > 0.0f ? v : -v; }
static void make_plane(Plane *p, float x, float y, float z, float w)
{
    float n = x * x;
    n = n + y * y;
    n = n + z * z;
    const float inv = 1.0f / sqrtf(n);
    p->x = x * inv;
    p->y = y * inv;
    p->z = z * inv;
    p->w = w * -inv;
}
/* bounds (center, extent) grown by a box: as the game does it */
static void grow_bounds(float *bc, float *be, const float *box)
{
    float lo_z = bc[2] - be[2];
    const float blo_z = box[2] - box[5];
    lo_z = lo_z < blo_z ? lo_z : blo_z;
    float hi_z = bc[2] + be[2];
    const float bhi_z = box[2] + box[5];
    hi_z = hi_z < bhi_z ? bhi_z : hi_z;
    const float ez = (hi_z - lo_z) * 0.5f;
    const float cz = lo_z + ez;
    float lo[2], hi[2];
    for (int i = 0; i < 2; i++) {
        lo[i] = bc[i] - be[i];
        const float blo = box[i] - box[3 + i];
        lo[i] = blo > lo[i] ? lo[i] : blo;
        hi[i] = bc[i] + be[i];
        const float bhi = box[i] + box[3 + i];
        hi[i] = bhi > hi[i] ? bhi : hi[i];
    }
    for (int i = 0; i < 2; i++) {
        be[i] = (hi[i] - lo[i]) * 0.5f;
        bc[i] = lo[i] + be[i];
    }
    be[2] = ez;
    bc[2] = cz;
}
/* the visible bounds grow the same way, with the additions in another order */
static void grow_bounds_vis(float *bc, float *be, const float *box)
{
    float lo_z = bc[2] - be[2];
    const float blo_z = box[2] - box[5];
    lo_z = lo_z < blo_z ? lo_z : blo_z;
    const float hi_z0 = be[2] + bc[2];
    float hi_z = box[2] + box[5];
    hi_z = hi_z0 < hi_z ? hi_z : hi_z0;
    const float dz = hi_z - lo_z;
    float lo[2], hi[2];
    for (int i = 0; i < 2; i++) {
        lo[i] = bc[i] - be[i];
        const float blo = box[i] - box[3 + i];
        lo[i] = blo > lo[i] ? lo[i] : blo;
        const float h0 = be[i] + bc[i];
        hi[i] = box[i] + box[3 + i];
        hi[i] = hi[i] > h0 ? hi[i] : h0;
    }
    for (int i = 0; i < 2; i++) {
        be[i] = (hi[i] - lo[i]) * 0.5f;
        bc[i] = lo[i] + be[i];
    }
    be[2] = dz * 0.5f;
    bc[2] = lo_z + be[2];
}

/* MINA_RPU_STATS: per 60 calls of ycRenderPass::Update, objects seen, culled,
 * keyed (by a call) and sorted, and the time in the scan and the sort */
static struct { uint64_t calls, objs, culled, keyed, sorted, scan_us, sort_us; } rpu;
/* ycRenderPass::Update's sort: bottom-up merge sort by key, ties taking the
 * right run first, the game's two buffers (the result ends in the first) -
 * done in local arrays (the same merges, so the same order); keys already
 * strictly increasing are left as they are (every merge would take the left) */
static void rpu_sort(uint8_t *gm_, uint64_t RP, uint32_t v, uint32_t n)
{
    const uint64_t o0 = lp(RP + 0x218 + 8 * v), k0 = lp(RP + 0x1f0 + 8 * v);
    const uint32_t *keys = GP(k0);
    uint32_t i;
    for (i = 1; i < n && keys[i - 1] < keys[i]; i++)
        ;
    if (i == n)
        return;
    enum { MAXN = 4096 };
    static uint32_t ka[MAXN], kb[MAXN];
    static uint64_t oa[MAXN], ob[MAXN];
    if (n > MAXN) {           /* (not seen) the game's buffers, as before */
        const uint64_t o1 = lp(RP + 0x248), k1 = lp(RP + 0x240);
        uint64_t so = o0, sk = k0, dob = o1, dk = k1;
        for (uint32_t w = 1;; w *= 2) {
            for (uint32_t s0 = 0; s0 < n;) {
                const uint32_t mid = s0 + w < n ? s0 + w : n, end = s0 + 2 * w < n ? s0 + 2 * w : n;
                uint32_t a = s0, b = mid, d = s0;
                while (d < end) {
                    const int left = a < mid && (b >= end || rt_ld32(GP(sk + 4 * (uint64_t)a)) < rt_ld32(GP(sk + 4 * (uint64_t)b)));
                    const uint32_t from = left ? a++ : b++;
                    memcpy(GP(dob + 8 * (uint64_t)d), GP(so + 8 * (uint64_t)from), 8);
                    rt_st32(GP(dk + 4 * (uint64_t)d), rt_ld32(GP(sk + 4 * (uint64_t)from)));
                    d++;
                }
                s0 = s0 + 2 * w;
            }
            uint64_t t = so; so = dob; dob = t;
            t = sk; sk = dk; dk = t;
            if (2 * w >= n)
                break;
        }
        if (so != o0) {
            memcpy(GP(o0), GP(o1), 8 * (size_t)n);
            memcpy(GP(k0), GP(k1), 4 * (size_t)n);
        }
        return;
    }
    {
        const uint8_t *ob8 = GP(o0);
        for (uint32_t j = 0; j < n; j++) {
            ka[j] = keys[j];
            memcpy(&oa[j], ob8 + 8 * j, 8);
        }
    }
    uint32_t *sk = ka, *dk = kb;
    uint64_t *so = oa, *dob = ob;
    for (uint32_t w = 1;; w *= 2) {
        for (uint32_t s0 = 0; s0 < n; s0 += 2 * w) {
            const uint32_t mid = s0 + w < n ? s0 + w : n, end = s0 + 2 * w < n ? s0 + 2 * w : n;
            uint32_t a = s0, b = mid, d = s0;
            while (a < mid && b < end) {
                if (sk[a] < sk[b]) {
                    dk[d] = sk[a];
                    dob[d++] = so[a++];
                } else {
                    dk[d] = sk[b];
                    dob[d++] = so[b++];
                }
            }
            while (a < mid) {
                dk[d] = sk[a];
                dob[d++] = so[a++];
            }
            while (b < end) {
                dk[d] = sk[b];
                dob[d++] = so[b++];
            }
        }
        uint32_t *tk = sk; sk = dk; dk = tk;
        uint64_t *to = so; so = dob; dob = to;
        if (2 * w >= n)
            break;
    }
    {
        uint32_t *kd = GP(k0);
        uint8_t *od = GP(o0);
        for (uint32_t j = 0; j < n; j++) {
            kd[j] = sk[j];
            memcpy(od + 8 * j, &so[j], 8);
        }
    }
}
static void render_pass_update(Ctx *c)
{
    GM_BEGIN;
    static int stats = -1;
    if (stats < 0)
        stats = getenv("MINA_RPU_STATS") != NULL;
    uint64_t t_scan = stats ? now_us() : 0;
    if (stats)
        rpu.calls++;
    const uint64_t RP = c->x[0];
    if (!*(uint8_t *)GP(RP + 0x258))
        return;
    const uint64_t saved_sp = c->sp, fr = saved_sp - 0x80;    /* campos +0, camdir +0x10, inverse +0x20 */
    c->sp = fr;
    for (uint32_t v = 0; v < 5; v++) {
        const uint64_t cam = lp(RP + 0x10 + 8 * v);
        if (!cam)
            continue;
        const uint32_t mask = *(uint8_t *)GP(RP + 0x9);
        const float *A = GP(cam), *B = GP(cam + 0x40);
        /* camera position: the translation of B's inverse (the game's code) */
        c->x[0] = cam + 0x40;
        c->x[8] = fr + 0x20;
        f_a33c0(c);
        const float *Binv = GP(fr + 0x20);
        float *campos = GP(fr), *camdir = GP(fr + 0x10);
        campos[0] = Binv[12]; campos[1] = Binv[13]; campos[2] = Binv[14];
        memcpy(camdir, GP(cam + 0x10c), 12);
        /* M = B x A, by rows */
        float M[4][4];
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++) {
                float s = A[0 * 4 + k] * B[r * 4 + 0];
                s = s + A[1 * 4 + k] * B[r * 4 + 1];
                s = s + A[2 * 4 + k] * B[r * 4 + 2];
                s = s + A[3 * 4 + k] * B[r * 4 + 3];
                M[r][k] = s;
            }
        /* planes in the order they are tested */
        Plane P[6];
        make_plane(&P[0], M[0][3] - M[0][2], M[1][3] - M[1][2], M[2][3] - M[2][2], M[3][3] - M[3][2]);
        make_plane(&P[1], M[0][2], M[1][2], M[2][2], M[3][2]);
        make_plane(&P[2], M[0][3] + M[0][0], M[1][3] + M[1][0], M[2][3] + M[2][0], M[3][3] + M[3][0]);
        make_plane(&P[3], M[0][3] - M[0][0], M[1][3] - M[1][0], M[2][3] - M[2][0], M[3][3] - M[3][0]);
        make_plane(&P[4], M[0][3] - M[0][1], M[1][3] - M[1][1], M[2][3] - M[2][1], M[3][3] - M[3][1]);
        make_plane(&P[5], M[0][3] + M[0][1], M[1][3] + M[1][1], M[2][3] + M[2][1], M[3][3] + M[3][1]);
        /* the planes that can cull (not at infinity), with their absolute normals:
         * plane_culls's operations, what does not depend on the box done once */
        PlaneA act[6];
        int nact = 0;
        for (int k = 0; k < 6; k++) {
            if (__builtin_fabsf(P[k].w) == __builtin_inff())
                continue;
            act[nact] = (PlaneA){ P[k].x, P[k].y, P[k].z, P[k].w, fabs_sel(P[k].x), fabs_sel(P[k].y), fabs_sel(P[k].z) };
            nact++;
        }
        /* bounds of all objects: empty */
        float *abm = GP(RP + 0xb0 + 0x18 * v);        /* center 3, extent 3 */
        const float neg_max = hexf(0xfe7fffff);
        float ab[6] = { 0.0f, 0.0f, 0.0f, neg_max, neg_max, neg_max };   /* (in registers; stored at the end) */
        float vc[3] = { 0, 0, 0 }, ve[3] = { neg_max, neg_max, neg_max };
        const uint32_t skip_flag = ((1u << v) & mask) ? 8 : 0;
        uint32_t n = 0;
        const uint32_t sortfn = (uint32_t)lp(RP + 0x128);
        const uint32_t npages = (uint32_t)lp(RP + 0x138);
        for (uint32_t pg = 0; pg < npages; pg++) {
            const uint32_t pages = (uint32_t)lp(RP + 0x130);
            uint32_t left = *(uint16_t *)GP(pages + 0x6660 + 2 * pg);
            if (!left)
                continue;
            const uint32_t page = (uint32_t)lp(pages + 8 * pg);
            for (uint32_t j = 0; left; j++) {
                const uint64_t obj = lp(page + 8 * j);
                if (!obj)
                    continue;
                left--;
                if (stats)
                    rpu.objs++;
                const uint32_t boxa = page + 0x4800 + 0x18 * j;
                const float *box = GP(boxa);
                grow_bounds(ab, ab + 3, box);
                const uint8_t layer = *(uint8_t *)GP(page + 8 * j + 0x6001);
                if (!(*(uint8_t *)GP(RP + 0x8) & layer))
                    continue;
                const uint64_t fa = page + 8 * j + 0x6000;
                uint8_t f = *(uint8_t *)GP(fa);
                if (f & skip_flag)
                    continue;
                if (f & 1) {
                    int culled = 0;
                    for (int k = 0; k < nact && !culled; k++) {
                        const PlaneA *q = &act[k];
                        float dd = q->x * box[0];
                        dd = dd + box[1] * q->y;
                        dd = dd + box[2] * q->z;
                        float r = q->ax * box[3];
                        r = r + q->ay * box[4];
                        r = r + q->az * box[5];
                        culled = dd + r < q->w;
                    }
                    if (culled) {
                        if (stats)
                            rpu.culled++;
                        continue;
                    }
                }
                if (n == lp(RP + 0x148)) {
                    c->x[0] = RP;
                    c->x[1] = (uint64_t)n * 2;
                    f_1e0ca0(c);
                    f = *(uint8_t *)GP(fa);
                }
                uint32_t key;
                if (f & 6) {
                    key = (f >> 1 & 1) - 1u;
                } else if (sortfn == 0x11e2844u || sortfn == 0x11e2894u || sortfn == 0x11e28e0u ||
                           sortfn == 0x11e0ae4u) {
                    /* the game's sort keys (ycRenderPass::BackToFrontLook,
                     * FrontToBackLook, HUDSort, NullSort), the same operations */
                    float k;
                    if (sortfn == 0x11e0ae4u) {
                        key = 0;
                        goto keyed;
                    }
                    if (sortfn == 0x11e28e0u) {
                        k = box[2] + 100.0f;
                    } else {
                        const float *cp = GP(fr), *cd = GP(fr + 0x10);
                        k = (box[0] - cp[0]) * cd[0];
                        const float k1 = (box[1] - cp[1]) * cd[1], k2 = (box[2] - cp[2]) * cd[2];
                        k = k + k1;
                        k = k + k2;
                        k = ldfp(fa + 4) + k;
                    }
                    if (sortfn == 0x11e28e0u)
                        k = k + ldfp(fa + 4);
                    k = k > 0.0f ? k : 0.0f;          /* fmaxnm(k, 0) */
                    memcpy(&key, &k, 4);
                    if (sortfn != 0x11e2894u)
                        key = ~key;
                } else {
                    c->x[0] = boxa;
                    c->x[1] = fr;
                    c->x[2] = fr + 0x10;
                    c->x[3] = fa;                    /* the object's flags (ycRenderObjectFlags) */
                    static const GuestFunc *ic;
                    RT_CALL_IC(c, lp(RP + 0x128), ic);
                    key = (uint32_t)c->x[0];
                    if (stats) {
                        rpu.keyed++;
                        static uint32_t seen[16];
                        const uint32_t t = (uint32_t)lp(RP + 0x128);
                        int i = 0;
                        while (i < 16 && seen[i] && seen[i] != t)
                            i++;
                        if (i < 16 && !seen[i]) {
                            seen[i] = t;
                            rt_log("[rpu] key function %#x\n", t);
                        }
                    }
                }
            keyed:
                rt_st32(GP(lp(RP + 0x1f0 + 8 * v) + 4 * (uint64_t)n), key);
                const uint64_t o64 = obj;
                memcpy(GP(lp(RP + 0x218 + 8 * v) + 8 * (uint64_t)n), &o64, 8);
                n++;
                grow_bounds_vis(vc, ve, box);
            }
        }
        memcpy(abm, ab, sizeof ab);
        {   /* MINA_POISON_ALLBOUNDS: is the bounds of all objects read at all? */
            static int poison = -1;
            if (poison < 0)
                poison = getenv("MINA_POISON_ALLBOUNDS") != NULL;
            if (poison)
                for (int k = 0; k < 6; k++)
                    abm[k] = __builtin_nanf("");
        }
        float *vb = GP(RP + 0x150 + 0x18 * v);
        vb[0] = vc[0]; vb[1] = vc[1]; vb[2] = vc[2];
        vb[3] = ve[0]; vb[4] = ve[1]; vb[5] = ve[2];
        uint64_t t_sort = stats ? now_us() : 0;
        if (stats) {
            rpu.scan_us += t_sort - t_scan;
            rpu.sorted += n;
        }
        if (lp(RP + 0x128) != 0x11e0ae4 && n >= 2)
            rpu_sort(gm_, RP, v, n);
        memcpy(GP(RP + 0x1c8 + 8 * v), &(uint64_t){ n }, 8);
        if (stats) {
            t_scan = now_us();
            rpu.sort_us += t_scan - t_sort;
        }
    }
    c->sp = saved_sp;
    if (stats && rpu.calls % 600 == 0) {
        rt_log("[rpu] per call: %.0f objects, %.0f culled, %.0f keyed, %.0f sorted; %.1f us scan, %.1f us sort\n",
               rpu.objs / 600.0, rpu.culled / 600.0, rpu.keyed / 600.0, rpu.sorted / 600.0, rpu.scan_us / 600.0,
               rpu.sort_us / 600.0);
        memset(&rpu, 0, sizeof rpu);
    }
}
void nat__ZN12ycRenderPass6UpdateEv(Ctx *c)
{
    static int off = -1;
    if (off < 0)
        off = getenv("MINA_NO_NATIVE_RPU") != NULL;
    if (off) {
        f_1e13ac_orig(c);
        return;
    }
    if (verify_xform()) {
        /* the pass and the view lists it writes */
        static uint8_t before[0x260], mine[0x260];
        static uint32_t checked, bad;
        const uint64_t RP = c->x[0];
        memcpy(before, G(RP), sizeof before);
        Ctx save = *c;
        render_pass_update(c);
        memcpy(mine, G(RP), sizeof mine);
        uint64_t lists[5][2];
        static uint8_t mo[5][4096 * 8], mk[5][4096 * 4];
        uint32_t cnt[5];
        for (int v = 0; v < 5; v++) {
            cnt[v] = ld_ptr(RP + 0x10 + 8 * v) ? (uint32_t)ld_ptr(RP + 0x1c8 + 8 * v) : 0;
            if (cnt[v] > 4096) cnt[v] = 4096;
            lists[v][0] = ld_ptr(RP + 0x218 + 8 * v);
            lists[v][1] = ld_ptr(RP + 0x1f0 + 8 * v);
            if (cnt[v]) {
                memcpy(mo[v], G(lists[v][0]), 8 * cnt[v]);
                memcpy(mk[v], G(lists[v][1]), 4 * cnt[v]);
            }
        }
        /* the lists grew (reallocated): the game's code would free them again */
        if (memcmp(before + 0x1f0, mine + 0x1f0, 0x60) != 0)
            return;
        memcpy(G(RP), before, sizeof before);
        *c = save;
        f_1e13ac_orig(c);
        checked++;
        int diff = memcmp(mine, G(RP), sizeof mine) != 0;
        for (int v = 0; v < 5 && !diff; v++)
            if (cnt[v] && (memcmp(mo[v], G(ld_ptr(RP + 0x218 + 8 * v)), 8 * cnt[v]) ||
                           memcmp(mk[v], G(ld_ptr(RP + 0x1f0 + 8 * v)), 4 * cnt[v])))
                diff = 2;
        if (diff && bad++ < 10) {
            size_t i = 0;
            while (i < sizeof mine && mine[i] == ((uint8_t *)G(RP))[i])
                i++;
            rt_log("[verify] RenderPass::Update differs (%s, first byte %#zx)\n", diff == 2 ? "lists" : "pass", i);
        }
        if ((checked & 0x3ff) == 0)
            rt_log("[verify] RenderPass::Update: %u calls checked, %u differ\n", checked, bad);
        return;
    }
    render_pass_update(c);
}

/* ---- ycInputState::UpdateDerivedState ---------------------------------------------------------
 * For two arrays of inputs (43 at +0x2f4, 137 at +0x650): the value when just
 * pressed, the time held when just released, the time held; three groups of
 * five (26-30, 31-35, 36-40 of the first) summed up; then this frame's values
 * become the previous ones. s0 = dt. */
void f_a3a00_orig(Ctx *c);
static void input_array(uint8_t *cur, uint8_t *prev, uint8_t *pressed, uint8_t *hold, uint8_t *released, int n,
                        float dt)
{
    for (int i = 0; i < n; i++) {
        float cv, pv, h;
        memcpy(&cv, cur + 4 * i, 4);
        memcpy(&pv, prev + 4 * i, 4);
        memcpy(&h, hold + 4 * i, 4);
        const float p = (pv == 0.0f && cv > 0.0f) ? cv : 0.0f;
        const float r = (pv > 0.0f && !(cv > 0.0f)) ? h : 0.0f;
        const float nh = cv > 0.0f ? h + dt : 0.0f;
        memcpy(pressed + 4 * i, &p, 4);
        memcpy(released + 4 * i, &r, 4);
        memcpy(hold + 4 * i, &nh, 4);
    }
}
static void update_derived_state(Ctx *c)
{
    uint8_t *s = G(c->x[0]);
    const float dt = c->v[0].f[0];
    uint8_t *a = s + 0x2f4;
    input_array(a, a + 0xac, a + 0x158, a + 0x204, a + 0x2b0, 43, dt);
    input_array(s + 0x650, s + 0x874, s + 0xa98, s + 0xcbc, s + 0xee0, 137, dt);
    const float one = 1.0f, zero = 0.0f;
    for (int g = 0; g < 3; g++) {
        memcpy(s + 0x1104 + 4 * g, &zero, 4);
        memcpy(s + 0x111c + 4 * g, &zero, 4);
        float m = 0.0f;
        for (int k = 0; k < 5; k++) {
            const int i = 26 + 5 * g + k;
            float p, h, r;
            memcpy(&p, a + 0x158 + 4 * i, 4);
            memcpy(&h, a + 0x204 + 4 * i, 4);
            memcpy(&r, a + 0x2b0 + 4 * i, 4);
            if (p > 0.0f)
                memcpy(s + 0x1104 + 4 * g, &one, 4);
            m = k == 0 ? fmax_f32(h, 0.0f) : (m > h ? m : h);
            if (r > 0.0f)
                memcpy(s + 0x111c + 4 * g, &one, 4);
        }
        memcpy(s + 0x1110 + 4 * g, &m, 4);
    }
    memcpy(a + 0xac, a, 0xac);
    memcpy(s + 0x874, s + 0x650, 0x224);
}
void nat__ZN12ycInputState18UpdateDerivedStateEf(Ctx *c)
{
    if (verify_xform()) {
        static uint8_t before[0x1130], mine[0x1130];
        static uint32_t checked, bad;
        uint8_t *s = G(c->x[0]);
        memcpy(before, s, sizeof before);
        Ctx save = *c;
        update_derived_state(c);
        memcpy(mine, s, sizeof mine);
        memcpy(s, before, sizeof before);
        *c = save;
        f_a3a00_orig(c);
        checked++;
        if (memcmp(mine, s, sizeof mine) != 0 && bad++ < 10) {
            size_t i = 0;
            while (mine[i] == s[i])
                i++;
            rt_log("[verify] UpdateDerivedState differs at %#zx\n", i);
        }
        if ((checked & 0xfff) == 0)
            rt_log("[verify] UpdateDerivedState: %u calls checked, %u differ\n", checked, bad);
        return;
    }
    update_derived_state(c);
}

/* ---- ycCutterAnimComponent::UpdateFrame -------------------------------------------------------
 * x0 component, s0 dt, w1 whether to update the render frame. The clock (after
 * a start delay at +0x1c4) times the speed (+0x178) moves the times at +0x15c,
 * +0x164, +0x168; the frame index (+0x158) steps over the frame durations of
 * the current animation (+0x17c), forward or backward, looping (+0x170 loops
 * left, +0x16c loops done) or stopping at the end. If the frame changed (+0x174)
 * and w1: UpdateRenderFrame. */
void f_d3b20_orig(Ctx *c);
void f_d3d2c(Ctx *c);            /* ycCutterAnimComponent::UpdateRenderFrame */
static void update_frame(Ctx *c)
{
    GM_BEGIN;
    const uint64_t O = c->x[0];
    float dt = c->v[0].f[0];
    float adv = 0.0f;
    if (!*(uint8_t *)GP(O + 0x1ca)) {
        const float delay = ldfp(O + 0x1c4);
        if (delay == 0.0f) {
            adv = dt;
        } else {
            float left = delay - dt;
            left = (left > 0.0f || left != left) ? left : 0.0f;      /* fcsel ls: <= 0 gives 0 */
            stfp(O + 0x1c4, left);
            adv = fmax_f32(dt - delay, 0.0f);
        }
    }
    const float speed = ldfp(O + 0x178);
    const float step = adv * speed;
    const float a = step + ldfp(O + 0x164);
    float t = step + ldfp(O + 0x168);
    stfp(O + 0x164, a);
    stfp(O + 0x168, t);
    const float prev = ldfp(O + 0x15c);
    stfp(O + 0x160, prev);
    stfp(O + 0x15c, step + prev);
    *(uint8_t *)GP(O + 0x174) = 0;
    uint32_t loops = rt_ld32(GP(O + 0x170));
    int changed = 0;
    if (loops) {
        const uint64_t anims = lp(lp(lp(O + 0x40) + 0x10) + 0x30);
        const uint32_t an = rt_ld32(GP(O + 0x17c));
        const uint64_t A = anims + 0x38 * (uint64_t)an;
        const uint64_t frames0 = lp(A + 0x20);
        uint32_t fi = rt_ld32(GP(O + 0x158));
        uint64_t fr = frames0 + 0x18 * (uint64_t)fi;
        int32_t wraps = 4;
        const float eps = hexf(0xb8d1b717);
        for (;;) {
            int dir;
            const float dur = ldfp(fr + 4);
            if (speed >= 0.0f && t >= dur + eps) {
                t = t - dur;
                dir = 1;
            } else if (speed < 0.0f && t <= -(dur + eps)) {
                t = dur + t;
                dir = -1;
            } else {
                break;
            }
            stfp(O + 0x168, t);
            fi = fi + (uint32_t)dir;
            rt_st32(GP(O + 0x158), fi);
            *(uint8_t *)GP(O + 0x174) = 1;
            changed = 1;
            const uint64_t count = ld_ptr(A + 0x18) >> 34;
            if ((uint64_t)fi < count) {
                fr = frames0 + 0x18 * (uint64_t)fi;
                if (!loops || wraps < 0)
                    break;
                continue;
            }
            /* past the end: one loop less */
            if ((int32_t)(loops - 1) >= 0) {
                loops = loops - 1;
                rt_st32(GP(O + 0x170), loops);
            }
            if (!loops) {
                const uint32_t idx = (speed >= 0.0f || speed != speed) ? (uint32_t)count - 1 : 0u;
                rt_st32(GP(O + 0x158), idx);
                changed = frames0 + 0x18 * (uint64_t)idx != fr;
                *(uint8_t *)GP(O + 0x174) = (uint8_t)changed;
                break;
            }
            fi = speed < 0.0f ? (uint32_t)count - 1 : 0u;
            rt_st32(GP(O + 0x158), fi);
            stfp(O + 0x164, t);
            rt_st32(GP(O + 0x16c), rt_ld32(GP(O + 0x16c)) + 1);
            *(uint8_t *)GP(O + 0x174) = 1;
            if (count == 1)
                break;
            wraps--;
            fr = frames0 + 0x18 * (uint64_t)fi;
            if (wraps < 0)
                break;
        }
    }
    if (changed && (uint32_t)c->x[1] & 0xff) {
        c->x[0] = O;
        f_d3d2c(c);
    }
}
void nat__ZN21ycCutterAnimComponent11UpdateFrameEfb(Ctx *c)
{
    if (verify_xform()) {
        static uint8_t before[0x200], mine[0x200];
        static uint32_t checked, bad;
        uint8_t *s = G(c->x[0]);
        memcpy(before, s, sizeof before);
        Ctx save = *c;
        save.x[1] = 0;                   /* the render frame part is not compared */
        Ctx run = save;
        update_frame(&run);
        memcpy(mine, s, sizeof mine);
        memcpy(s, before, sizeof before);
        run = save;
        f_d3b20_orig(&run);
        checked++;
        if (memcmp(mine, s, sizeof mine) != 0 && bad++ < 10) {
            size_t i = 0;
            while (mine[i] == s[i])
                i++;
            rt_log("[verify] UpdateFrame differs at %#zx\n", i);
        }
        if ((checked & 0xfffff) == 0)
            rt_log("[verify] UpdateFrame: %u calls checked, %u differ\n", checked, bad);
        memcpy(s, before, sizeof before);
        update_frame(c);
        return;
    }
    update_frame(c);
}

/* ---- GameAnim::UpdateFacing: its early out natively ------------------------------------------
 * Nothing to do unless the facing flag (bit 3 of +0x1e4) is set and the facing
 * (from the owner at +0x248 +0x144, or +0x280) differs from the one used
 * (+0x250): then the game's code runs. */
void f_9a7c54_orig(Ctx *c);
void nat__ZN8GameAnim12UpdateFacingEv(Ctx *c)
{
    GM_BEGIN;
    const uint64_t O = c->x[0];
    const uint32_t f = rt_ld32(GP(O + 0x1e4));
    if (!(f & 8) || (int32_t)rt_ld32(GP(O + 0x2d0)) < 0)
        return;
    const uint64_t own = lp(O + 0x248);
    const int from_owner = own && !(f & 0x10000000);
    const uint64_t xy = from_owner ? own + 0x144 : O + 0x280, z = from_owner ? own + 0x14c : O + 0x288;
    if (ldfp(xy) == ldfp(O + 0x250) && ldfp(xy + 4) == ldfp(O + 0x254) && ldfp(z) == ldfp(O + 0x258))
        return;
    f_9a7c54_orig(c);
}

/* ---- ShipIntroBossArm::ApplySpline ------------------------------------------------------------
 * The arm's 32 segments (A+0x510+0x28k) follow its spline, then 4 rounds of
 * relaxation move pairs of them: each move recalculates the segment's world
 * transform (GameAnim::RecalcWorldTransform, ~390 a call), though only the
 * segments' positions (+0x19c) are read until GameAnim::GetFrameBound. That
 * recalculation is a function of its inputs (what it changes for itself is
 * restored), so the segments' recalculations wait: the last one of each runs,
 * in the order of those last calls, before any GetFrameBound and at the end.
 * MINA_VERIFY_XFORM=1 runs every call, and checks that running the last one
 * again changes nothing. MINA_NO_ARM_DEFER=1: no waiting. */
void f_66e660_orig(Ctx *c);
void f_9a6dc4_orig(Ctx *c);
static uint64_t arm_active;
static uint32_t arm_skipped, arm_ran, arm_calls;
/* the segments' components (read when the arm starts), and per segment its
 * pending recalculation: arguments and the order of its last call (0: none) */
static uint64_t arm_seg[32];
/* component -> segment + 1, open addressing (64 slots), made when the arm starts */
static struct { uint32_t comp; uint8_t k; } arm_hash[64];
static inline uint32_t arm_slot(uint32_t comp) { return (comp >> 4) * 2654435761u >> 26; }
static struct { uint64_t a1, a2; uint32_t seq; } arm_pend[32];
static uint32_t arm_seq;
static int arm_npending;          /* segments with a pending recalculation */
static int arm_find(uint32_t comp)
{
    for (uint32_t h = arm_slot(comp);; h = (h + 1) & 63) {
        if (!arm_hash[h].k)
            return -1;
        if (arm_hash[h].comp == comp)
            return arm_hash[h].k - 1;
    }
}

static int arm_defer(Ctx *c)
{
    if (!arm_active)
        return 0;
    const int k = arm_find((uint32_t)c->x[0]);
    if (k < 0)
        return 0;
    if (!arm_pend[k].seq)
        arm_npending++;
    arm_pend[k].a1 = c->x[1];
    arm_pend[k].a2 = c->x[2];
    arm_pend[k].seq = ++arm_seq;
    if (verify_xform())
        return 0;                    /* runs now too; the flush checks it */
    arm_skipped++;
    return 1;
}
static void arm_flush(Ctx *c)
{
    if (!arm_npending)
        return;
    int order[32], n = 0;
    for (int k = 0; k < 32; k++) {   /* pending ones by their last call */
        if (!arm_pend[k].seq)
            continue;
        int i = n++;
        while (i > 0 && arm_pend[order[i - 1]].seq > arm_pend[k].seq) {
            order[i] = order[i - 1];
            i--;
        }
        order[i] = k;
    }
    if (!n)
        return;
    const uint64_t x0 = c->x[0], x1 = c->x[1], x2 = c->x[2];
    const uint64_t active = arm_active;
    arm_active = 0;                  /* these run */
    arm_npending = 0;
    for (int i = 0; i < n; i++) {
        const int k = order[i];
        arm_pend[k].seq = 0;
        c->x[0] = arm_seg[k];
        c->x[1] = arm_pend[k].a1;
        c->x[2] = arm_pend[k].a2;
        if (verify_xform()) {
            static RwtSnap ran, again;
            static uint32_t checked, bad;
            rwt_take(arm_seg[k], &ran);
            recalc_world_transform(c);
            rwt_take(arm_seg[k], &again);
            checked++;
            if (memcmp(&ran, &again, sizeof ran) != 0 && bad++ < 10)
                rt_log("[verify] arm segment %#llx: its last recalculation run again differs\n",
                       (unsigned long long)arm_seg[k]);
            if ((checked & 0xfff) == 0)
                rt_log("[verify] arm segments: %u checked, %u differ\n", checked, bad);
        } else {
            recalc_world_transform(c);
            arm_ran++;
        }
    }
    arm_active = active;
    c->x[0] = x0;
    c->x[1] = x1;
    c->x[2] = x2;
}
/* one segment's bounds: its own pending recalculation runs (the others wait:
 * each is a function of its own inputs); any other object's: all of them */
static void arm_flush_one(Ctx *c, int k)
{
    if (!arm_pend[k].seq || verify_xform()) {
        if (verify_xform())
            arm_flush(c);
        return;
    }
    const uint64_t x0 = c->x[0], x1 = c->x[1], x2 = c->x[2];
    const uint64_t active = arm_active;
    arm_active = 0;
    arm_pend[k].seq = 0;
    arm_npending--;
    c->x[0] = arm_seg[k];
    c->x[1] = arm_pend[k].a1;
    c->x[2] = arm_pend[k].a2;
    recalc_world_transform(c);
    arm_ran++;
    arm_active = active;
    c->x[0] = x0;
    c->x[1] = x1;
    c->x[2] = x2;
}
void nat__ZN8GameAnim13GetFrameBoundEb(Ctx *c)
{
    const int k = arm_active ? arm_find((uint32_t)c->x[0]) : -1;
    if (k >= 0)
        arm_flush_one(c, k);
    else
        arm_flush(c);
    f_9a6dc4_orig(c);
}
void nat__ZN16ShipIntroBossArm11ApplySplineEv(Ctx *c)
{
    static int off = -1;
    if (off < 0)
        off = getenv("MINA_NO_ARM_DEFER") != NULL;
    if (off || arm_active) {
        f_66e660_orig(c);
        return;
    }
    {
        GM_BEGIN;
        memset(arm_hash, 0, sizeof arm_hash);
        for (int k = 0; k < 32; k++) {
            arm_seg[k] = lp(c->x[0] + 0x510 + 0x28 * k);
            if (!arm_seg[k])
                continue;
            uint32_t h = arm_slot((uint32_t)arm_seg[k]);
            while (arm_hash[h].k && arm_hash[h].comp != (uint32_t)arm_seg[k])
                h = (h + 1) & 63;
            if (!arm_hash[h].k) {          /* (a component twice: its first segment, as the scan found) */
                arm_hash[h].comp = (uint32_t)arm_seg[k];
                arm_hash[h].k = (uint8_t)(k + 1);
            }
        }
    }
    arm_active = c->x[0];
    f_66e660_orig(c);
    arm_flush(c);
    arm_active = 0;
    if (++arm_calls % 600 == 0 && getenv("MINA_CALLERS"))
        rt_log("[arm] %u calls: %u segment recalculations waited, %u ran\n", arm_calls, arm_skipped, arm_ran);
}

/* ---- Game::InitFromSave ----------------------------------------------------------------------
 * The saved video options become the renderer's: among them the integer scale
 * of the game's buffers (settings +0x668 -> s_bufferIntScale; 0 picks the
 * biggest that fits: 3, 768x432, on the Vita's screen). The Vita draws them
 * at 1x (256x144, the art's own pixels) unless the player chose a scale:
 * MINA_BUFFER_SCALE=n picks another default (0: the game's). */
void f_99d1a8_orig(Ctx *c);
void nat__ZN4Game12InitFromSaveEv(Ctx *c)
{
    f_99d1a8_orig(c);
#if defined(__vita__)
    GM_BEGIN;
    const char *v = getenv("MINA_BUFFER_SCALE");
    const uint32_t want = v ? (uint32_t)atoi(v) : 1;
    const uint64_t settings = lp(0x6051920);
    rt_log("[video] saved buffer scale %u, pixel mode %u\n", settings ? rt_ld32(GP(settings + 0x668)) : 0xffffffffu,
           settings ? rt_ld32(GP(settings + 0x664)) : 0xffffffffu);
    if (want && settings && rt_ld32(GP(settings + 0x668)) == 0) {
        rt_st32(GP(settings + 0x668), want);
        rt_st32(GP(0x6045fc0), want);           /* (anonymous)::s_bufferIntScale */
        rt_log("[video] game buffers at %ux (%ux%u)\n", want, 256 * want, 144 * want);
    }
#endif
    /* The game steps its world (Game::FixedUpdate) s_targetFPS times a
     * second, by 1/s_targetFPS: 60 or 120 (the Max FPS option), presenting
     * after up to 2 steps. The Vita steps it 30 times (MINA_TARGET_FPS=n
     * another rate, 0 the game's), a step a frame at 30 fps. */
    const char *t = getenv("MINA_TARGET_FPS");
#if defined(__vita__)
    const uint32_t fps = t ? (uint32_t)atoi(t) : 30;
#else
    const uint32_t fps = t ? (uint32_t)atoi(t) : 0;
#endif
    if (fps) {
        GM_BEGIN;
        rt_st32(GP(0x1e14de8), fps);            /* (anonymous)::s_targetFPS */
        rt_log("[video] the world steps %u times a second\n", fps);
    }
}

/* ---- SpawnBase::GetPropertyEquals (MINA_PROPLOG) ---------------------------------------------
 * x1 the property's name, x2 a table of x3 strings: the index of the one the
 * property's value equals (x4 when none). MINA_PROPLOG=1 logs each answer. */
void f_ac2490_orig(Ctx *c);
void nat__ZNK9SpawnBase17GetPropertyEqualsEPKcPS1_yi(Ctx *c)
{
    static int on = -1;
    if (on < 0)
        on = getenv("MINA_PROPLOG") != NULL;
    if (!on) {
        f_ac2490_orig(c);
        return;
    }
    GM_BEGIN;
    const uint64_t key = c->x[1], tab = c->x[2], n = c->x[3], def = c->x[4];
    f_ac2490_orig(c);
    const int64_t r = (int64_t)(int32_t)c->x[0];
    const char *chosen = r >= 0 && (uint64_t)r < n ? (const char *)GP(lp(tab + 8 * (uint64_t)r)) : "-";
    rt_log("[prop] %s of %u (default %d) -> %lld %s\n", (const char *)GP(key), (unsigned)n, (int)def, (long long)r,
           chosen);
}

/* ---- threads on cores ------------------------------------------------------------------
 * The Vita gives the game 3 cores and does not move threads between them by
 * load: the game thread (the frame's bottleneck), the renderer and the audio
 * (a mixer, two Ogg decoders) each get one, so the decoders never take the
 * game thread's core. MINA_NO_PIN: as before (any core). */
#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#endif
static void pin_to_core(int core, const char *what)
{
#if defined(__vita__)
    static int off = -1;
    if (off < 0)
        off = getenv("MINA_NO_PIN") != NULL;
    if (off)
        return;
    const int r = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0 << core);
    rt_log("[thread] %s on core %d (%#x)\n", what, core, r);
#else
    (void)core;
    (void)what;
#endif
}
void f_1ced98_orig(Ctx *c);
void nat__ZN12ycGameThread10ThreadMainEPv(Ctx *c)
{
    pin_to_core(0, "game thread");
    f_1ced98_orig(c);
}
void f_1dfccc_orig(Ctx *c);
void nat__ZN10ycRenderer10ThreadMainEPv(Ctx *c)
{
    pin_to_core(1, "renderer");
    f_1dfccc_orig(c);
}
void f_d033c_orig(Ctx *c);
void nat__ZN7ycAudio5Mixer10ThreadMainEPv(Ctx *c)
{
    pin_to_core(2, "audio mixer");
    f_d033c_orig(c);
}
void f_cb1ac_orig(Ctx *c);
void nat__ZN7ycAudio7Decoder10ThreadMainEPv(Ctx *c)
{
    pin_to_core(2, "audio decoder");
    f_cb1ac_orig(c);
}

/* ---- ycCutterAnimRenderObject::Render ----------------------------------------------------------
 * A sprite's draw call, every frame: the pass's draw call template (RenderCtx
 * +0xb0, 0x220 bytes) with the sprite's material (a hash of its flags looked up
 * in ycMaterials' shader table), its constants (billboard matrix, snapped
 * offset, two colours, palette, the UV rect) in 0x100 bytes of the command
 * list's memory, its stencil state, then ycRenderCmdList::DrawCall. The same
 * operations as the game's code (MINA_NO_NATIVES=ycCutterAnimRenderObject runs it). */
void f_d589c_orig(Ctx *c);
void f_1b6a48(Ctx *c);     /* ycRenderUtils::CalcBillboardMtx */
void f_1b5b10(Ctx *c);     /* ycPaletteTexture::SetOnDrawCall */
void f_1df70c(Ctx *c);     /* ycRenderCmdList::DrawCall */
static inline uint32_t rotr32_(uint32_t v, int n) { return v >> n | v << (32 - n); }
static uint64_t cmdlist_alloc(Ctx *c, uint64_t L);
static int cutter_direct = 1;     /* the draw call made in the command list's entry (0: on the stack, then copied) */
static void cutter_render(Ctx *c)
{
    GM_BEGIN;
    const uint64_t O = c->x[0], CTX = c->x[1];
    const uint8_t *ob = GP(O);
    if (!ob[0xfd])
        return;
    uint32_t w9 = ob[0xdf];
    if (!w9 || !rt_ld32(GP(O + 0xcc)))
        return;
    const uint64_t tex = lp(O + 0xb8);
    if (!tex)
        return;
    const uint64_t saved_sp = c->sp, fr = saved_sp - 0x2b0;
    c->sp = fr;
    const uint64_t cmdlist = lp(CTX + 0x10);
    uint64_t entry = 0, D = fr + 0x40;
    if (cutter_direct) {
        entry = cmdlist_alloc(c, cmdlist);
        D = entry + 8;
    }
    uint8_t *d = GP(D);
    copy_dc(d, GP(CTX + 0xb0));
    if (ob[0xfe])
        rt_st32(d + 0x208, 0x01010101u);
    /* white and opaque, no added colour: 0 (the code reuses the register for
     * those bytes); else 1 */
    if (ob[0xdc] == 0xff && ob[0xdd] == 0xff && ob[0xde] == 0xff && w9 == 0xff && !ob[0xd8] && !ob[0xd9] &&
        !ob[0xda] && !ob[0xdb])
        w9 = 0;
    else
        w9 = 1;
    const uint64_t pal = lp(O + 0xd0);
    if (pal) {
        const int32_t w10 = (int32_t)rt_ld32(GP(pal + 0x30));
        const int lt = lp(0x60210c0) != 0 ? w10 < 0 : 1;
        w9 |= lt ? 8u : 24u;
    }
    {
        const float a = ldfp(O + 0x98), b = ldfp(O + 0x9c);
        if (a < b) {
            const float e = ldfp(O + 0xa0), f = ldfp(O + 0xa4);
            w9 |= e <= f ? 2u : 4u;
        }
    }
    /* the material: the flags hashed into the 256-slot table */
    uint32_t w10 = 0xdeadc42cu, w11 = 0x8ef4c855u, w12 = w9 + w10, w13, w14;
    w12 ^= w11;
    w12 += 0x59bd5389u;
    w10 = w12 ^ 0xdeadc42cu;
    w13 = w10 - rotr32_(w12, 7);
    w11 = w13 ^ w11;
    w14 = rotr32_(w13, 16);
    const uint64_t table = lp(0x60204b0);
    w11 -= w14;
    w12 = w11 ^ w12;
    w12 -= rotr32_(w11, 28);
    w13 = w12 ^ w13;
    w12 = w13 - rotr32_(w12, 18);
    w11 = w12 ^ w11;
    w11 -= w12 >> 8;
    uint32_t slot;
    for (;; w11++) {
        slot = w11 & 0xff;
        if (rt_ld32(GP(table + 8 * (uint64_t)slot)) == w9)
            break;
    }
    const uint32_t mat = rt_ld32(GP(table + 8 * (uint64_t)slot + 4));
    memcpy(d, GP(0x5ea1670 + 8 * (uint64_t)mat), 8);
    rt_st32(d + 0xf8, 0x200);
    memcpy(d + 0x100, &tex, 8);
    d[0x1b8] = ob[0xff] ^ 1;
    d[0x1b9] = 1;
    d[0x1ba] = 0;
    d[0x1bb] = 0x80;
    /* 0x100 bytes of the command list's memory for the constants */
    uint64_t cl = lp(0x6039f88), blk = lp(cl + 0x50), cb = 0;
    int have = 0;
    if (blk) {
        cb = lp(blk + 8);
        have = blk - cb + 0x80000 > 0xff;
    }
    if (!have) {
        const uint64_t alloc = lp(0x6039f60);
        c->x[0] = alloc;
        c->x[1] = 0x80000;
        c->x[2] = 0x100;
        c->x[3] = 0;
        c->x[4] = 0;
        c->x[5] = 0;
        static const GuestFunc *ic;
        RT_CALL_IC(c, lp(lp(alloc)), ic);
        blk = c->x[0];
        memset(GP(blk), 0, 8);
        const uint64_t top = blk + 0x100;
        memcpy(GP(blk + 8), &top, 8);
        memset(GP(blk + 0x10), 0, 8);
        uint64_t link = lp(cl + 0x50);
        if (!link)
            link = cl + 0x48;
        memcpy(GP(link), &blk, 8);
        memcpy(GP(cl + 0x50), &blk, 8);
        cb = lp(blk + 8);
        cl = lp(0x6039f88);
    }
    {
        const uint64_t top = cb + 0x100;
        memcpy(GP(blk + 8), &top, 8);
        const uint64_t cur = lp(cl + 0x50);
        memcpy(d + 0x50, &cur, 8);
        memcpy(d + 0x58, &cb, 8);
        memcpy(d + 0x60, GP(0x1d4e160), 8);
    }
    uint64_t mtx = lp(O + 8);
    if (ob[0xe0]) {           /* a billboard (mode 0 returns the matrix itself) */
        c->x[0] = fr;
        c->x[1] = mtx;
        c->x[2] = lp(CTX + 8);
        c->x[3] = ob[0xe0];
        f_1b6a48(c);
        mtx = c->x[0];
    }
    float *k = GP(cb);
    put_mat(k, GP(mtx));
    {   /* the offset snapped to whole pixels */
        const float s1 = ldfp(0x6039f5c), s0 = ldfp(0x6039f58);
        float v2[2];
        const float ox = ldfp(O + 0xac), oy = ldfp(O + 0xb0);
        if (ox != 0.0f || oy != 0.0f || ox != ox || oy != oy) {
            const float o[2] = { ox, oy };
            for (int i = 0; i < 2; i++) {
                float t = o[i] * s1;
                t = t + 0.5f;
                t = floorf(t);
                t = t * s0;
                t = t - o[i];
                v2[i] = t + k[12 + i];
            }
        } else {
            v2[0] = k[12];
            v2[1] = k[13];
        }
        for (int i = 0; i < 2; i++) {
            float t = v2[i] * s1;
            t = t + 0.5f;
            t = floorf(t);
            k[12 + i] = t * s0;
        }
    }
    for (int i = 0; i < 4; i++) {
        k[16 + i] = (float)ob[0xdc + i] / 255.0f;
        k[20 + i] = (float)ob[0xd8 + i] / 255.0f;
    }
    if (pal) {
        c->x[0] = pal;
        c->x[1] = D;
        c->x[2] = cb + 0x60;
        f_1b5b10(c);
        d = GP(D);
        k = GP(cb);
    }
    const uint64_t pal2 = lp(O + 0xe8);
    if (pal2) {
        rt_st32(d + 0x128, 0x202);
        memcpy(d + 0x130, &pal2, 8);
        k[28] = ldfp(O + 0xf0);
        k[29] = ldfp(O + 0xf0);
    }
    {
        const float a = ldfp(O + 0x98), b = ldfp(O + 0x9c);
        if (a < b) {
            k[28] = a;
            k[29] = ldfp(O + 0x9c);
            const float e = ldfp(O + 0xa0), f = ldfp(O + 0xa4);
            k[30] = e < f ? e : f;
            k[31] = e > f ? e : f;
        }
    }
    d[0x1e9] = 0;
    {
        const uint64_t z = 0, a = lp(0x603a548), b = lp(O + 0xc0), e = lp(0x604dc38);
        memcpy(d + 0x18, &z, 8);
        memcpy(d + 0x20, &a, 8);
        memcpy(d + 0x8, &b, 8);
        memcpy(d + 0x10, &z, 8);
        memcpy(d + 0x28, &e, 8);
    }
    rt_st32(d + 0x210, rt_ld32(GP(O + 0xc8)) * 6u >> 2);
    rt_st32(d + 0x214, rt_ld32(GP(O + 0xcc)) * 6u >> 2);
    rt_st32(d + 0x218, 1);
    rt_st32(d + 0x21c, rt_ld32(d + 0x21c) | 1);
    const uint8_t sa8 = ob[0xa8];
    if (sa8) {
        d[0x206] = 1;
        rt_st16(d + 0x1fe, 2);
        rt_st16(d + 0x202, 2);
        d[0x1f9] = ob[0xaa];
        d[0x1fa] = ob[0xaa];
        d[0x1fb] = sa8;
    }
    const uint8_t sa9 = ob[0xa9];
    if (sa9 != 0xff) {
        d[0x206] = 1;
        d[0x1ff] = 3;
        d[0x203] = 3;
        d[0x1f9] = ob[0xaa];
        d[0x1fa] = ob[0xaa];
        d[0x1fb] = sa9;
    }
    if (entry) {
        memcpy(GP(entry), GP(0x1d4e3b8), 8);
    } else {
        c->x[0] = cmdlist;
        c->x[1] = D;
        f_1df70c(c);
    }
    c->sp = saved_sp;
}
/* ycRenderCmdList::DrawCall: 0x228 bytes of the list's current block (a new
 * block, or the next one already made, when it is full): a header, then the
 * draw call as is */
static uint64_t cmdlist_alloc(Ctx *c, uint64_t L)
{
    GM_BEGIN;
    uint64_t e, left = lp(L + 0x28);
    if (left >= 0x228) {
        e = lp(L + 0x20);
        const uint64_t nx = e + 0x228;
        memcpy(GP(L + 0x20), &nx, 8);
    } else {
        uint64_t cur = lp(L + 0x18), bsize = lp(L + 0x30), next = 0;
        if (cur)
            next = lp(cur + bsize - 8);
        if (cur && next) {
            e = next;
        } else {
            const uint64_t alloc = lp(L + 8);
            const uint64_t x0 = c->x[0], x1 = c->x[1];
            c->x[0] = alloc;
            c->x[1] = bsize;
            c->x[2] = 0x40;
            c->x[3] = 0;
            c->x[4] = 0;
            c->x[5] = 0;
            static const GuestFunc *ic;
            RT_CALL_IC(c, lp(lp(alloc)), ic);
            e = c->x[0];
            c->x[0] = x0;
            c->x[1] = x1;
            bsize = lp(L + 0x30);
            memset(GP(e + bsize - 8), 0, 8);
            cur = lp(L + 0x18);
        }
        if (!e || !cur) {
            if (!cur)
                memcpy(GP(L + 0x10), &e, 8);
        }
        if (cur) {
            const uint64_t at = lp(L + 0x20), z = 0;
            memcpy(GP(at), &z, 8);
            memcpy(GP(at + 8), &e, 8);
            memcpy(GP(cur + bsize - 8), &e, 8);
        }
        const uint64_t nx = e + 0x228;
        memcpy(GP(L + 0x18), &e, 8);
        memcpy(GP(L + 0x20), &nx, 8);
        left = lp(L + 0x38);
    }
    left -= 0x228;
    memcpy(GP(L + 0x28), &left, 8);
    return e;
}
static void cmdlist_draw_call(Ctx *c)
{
    GM_BEGIN;
    const uint64_t L = c->x[0], dc = c->x[1];
    const uint64_t e = cmdlist_alloc(c, L);
    memcpy(GP(e), GP(0x1d4e3b8), 8);
    copy_dc(GP(e + 8), GP(dc));
}

/* MINA_VERIFY_CUTTER: each sprite's draw call made by both, captured at
 * ycRenderCmdList::DrawCall (not drawn), compared (the struct but its constant
 * pointers, and the 0x100 bytes of constants), then the native's is drawn */
void f_1df70c_orig(Ctx *c);
static int cutter_capture;           /* 1, 2: the call that DrawCall records */
static uint8_t cutter_dc[2][0x220], cutter_k[2][0x100];
static uint64_t cutter_list[2];
void nat__ZN15ycRenderCmdList8DrawCallEPK16ycRenderDrawCall(Ctx *c)
{
    if (cutter_capture) {
        const int k = cutter_capture - 1;
        memcpy(cutter_dc[k], G(c->x[1]), 0x220);
        uint64_t kp;
        memcpy(&kp, cutter_dc[k] + 0x58, 8);
        memcpy(cutter_k[k], G(kp), 0x100);
        cutter_list[k] = c->x[0];
        return;
    }
    cmdlist_draw_call(c);
}
void nat__ZN24ycCutterAnimRenderObject6RenderERKN14ycRenderObject9RenderCtxE(Ctx *c)
{
    static int verify = -1;
    if (verify < 0)
        verify = getenv("MINA_VERIFY_CUTTER") != NULL;
    if (!verify) {
        cutter_render(c);
        return;
    }
    static uint32_t checked, bad;
    cutter_direct = 0;
    Ctx save = *c;
    memset(cutter_dc, 0, sizeof cutter_dc);
    cutter_capture = 1;
    cutter_render(c);
    *c = save;
    cutter_capture = 2;
    f_d589c_orig(c);
    cutter_capture = 0;
    checked++;
    uint8_t a[0x220], b[0x220];
    memcpy(a, cutter_dc[0], sizeof a);
    memcpy(b, cutter_dc[1], sizeof b);
    memset(a + 0x50, 0, 16);         /* the constants' block and address */
    memset(b + 0x50, 0, 16);
    memset(cutter_k[0] + 0x68, 0, 8);   /* written by neither (SetOnDrawCall writes 0x60..0x67) */
    memset(cutter_k[1] + 0x68, 0, 8);
    memset(cutter_k[0] + 0x80, 0, 0x80);
    memset(cutter_k[1] + 0x80, 0, 0x80);
    if ((memcmp(a, b, sizeof a) || memcmp(cutter_k[0], cutter_k[1], 0x100) || cutter_list[0] != cutter_list[1]) &&
        bad++ < 10) {
        int i = 0;
        while (i < 0x220 && a[i] == b[i])
            i++;
        int j = 0;
        while (j < 0x100 && cutter_k[0][j] == cutter_k[1][j])
            j++;
        rt_log("[verify] cutter render differs: draw call byte %#x (%#x/%#x), constants byte %#x\n", i,
               i < 0x220 ? a[i] : 0, i < 0x220 ? b[i] : 0, j);
    }
    if ((checked & 0x3ff) == 0)
        rt_log("[verify] cutter render: %u checked, %u differ\n", checked, bad);
    if (cutter_dc[0][0] || cutter_list[0]) {     /* the native's draw call, drawn */
        memcpy(G(save.sp - 0x2b0 + 0x40), cutter_dc[0], 0x220);
        c->x[0] = cutter_list[0];
        c->x[1] = save.sp - 0x2b0 + 0x40;
        const uint64_t sp = c->sp;
        c->sp = save.sp - 0x2b0;
        f_1df70c_orig(c);
        c->sp = sp;
    }
}

/* ---- CombatCore::UpdateCoreAttackData / UpdateCoreDefenseData ----------------------------------
 * A combat core's attack (C+0xd0) / defense (C+0xd8) shape data from the core
 * and its chain of parents (C+0x40, followed while a flag of C+0x210 says the
 * value comes from the parent), then the same for its children (C+0x58 list). */
static uint64_t core_root(uint8_t *gm_, uint64_t c, uint32_t bit)
{
    for (;;) {
        if (!(*(uint8_t *)GP(c + 0x210) & bit))
            return c;
        const uint64_t p = lp(c + 0x40);
        if (!p)
            return c;
        c = p;
    }
}
static void core_copy_floats(uint8_t *gm_, uint64_t C, uint32_t off, uint64_t sd, uint64_t d)
{
    stfp(d + 0x8, ldfp(sd + 0x14));
    sd = lp(C + off);
    stfp(lp(sd + 8) + 0xc, ldfp(sd + 0x18));
    sd = lp(C + off);
    stfp(lp(sd + 8), ldfp(sd + 0x20));
    sd = lp(C + off);
    stfp(lp(sd + 8) + 4, ldfp(sd + 0x24));
}
static void core_attack(uint8_t *gm_, uint64_t C)
{
    const uint64_t sd = lp(C + 0xd0);
    if (sd) {
        const uint64_t d = lp(sd + 8);
        memcpy(GP(d + 0x10), &sd, 8);
        memset(GP(d + 0x20), 0, 8);
        int on = 0;
        const uint64_t P = lp(C + 0x40);
        if ((*(uint8_t *)GP(C + 0x210) & 4) && P) {
            if (*(uint8_t *)GP(core_root(gm_, P, 4) + 0x255)) {
                uint8_t v;
                if (!(*(uint8_t *)GP(P + 0x210) & 4) || !lp(P + 0x40))
                    v = *(uint8_t *)GP(P + 0x256);
                else
                    v = *(uint8_t *)GP(core_root(gm_, lp(P + 0x40), 4) + 0x255);
                on = v != 0;
            }
        } else {
            on = *(uint8_t *)GP(C + 0x256) && *(uint8_t *)GP(C + 0x255);
        }
        if (on) {
            const uint64_t one = 1;
            memcpy(GP(d + 0x20), &one, 8);
        }
        const uint64_t team = rt_ld32(GP(core_root(gm_, C, 1) + 0x20c));
        memcpy(GP(d + 0x18), &team, 8);
        core_copy_floats(gm_, C, 0xd0, sd, d);
    }
    for (uint64_t n = lp(C + 0x58); n; n = lp(n + 0x18))
        core_attack(gm_, lp(n + 8));
}
static void core_defense(uint8_t *gm_, uint64_t C)
{
    const uint64_t sd = lp(C + 0xd8);
    if (sd) {
        const uint64_t d = lp(sd + 8);
        memcpy(GP(d + 0x10), &sd, 8);
        memset(GP(d + 0x20), 0, 8);
        if (*(uint8_t *)GP(core_root(gm_, C, 8) + 0x257)) {
            const uint64_t two = 2;
            memcpy(GP(d + 0x20), &two, 8);
        }
        const uint64_t team = rt_ld32(GP(core_root(gm_, C, 2) + 0x208));
        memcpy(GP(d + 0x18), &team, 8);
        core_copy_floats(gm_, C, 0xd8, sd, d);
    }
    for (uint64_t n = lp(C + 0x58); n; n = lp(n + 0x18))
        core_defense(gm_, lp(n + 8));
}
void nat__ZN10CombatCore20UpdateCoreAttackDataEv(Ctx *c)
{
    GM_BEGIN;
    core_attack(gm_, c->x[0]);
}
void nat__ZN10CombatCore21UpdateCoreDefenseDataEv(Ctx *c)
{
    GM_BEGIN;
    core_defense(gm_, c->x[0]);
}

/* ---- ShadowComponent::SetVisible ---------------------------------------------------------------
 * The shadow's render objects (+0x155: drawn) follow its visibility, the
 * blob (+0xdd), and the owner's sprite (S+0x80). */
void nat__ZN15ShadowComponent10SetVisibleEb(Ctx *c)
{
    GM_BEGIN;
    const uint64_t S = c->x[0];
    const uint32_t vis = (uint32_t)c->x[1];
    uint8_t *s = GP(S);
    s[0xdc] = (uint8_t)vis;
    uint64_t x8 = lp(S + 0x68);
    if (x8)
        *(uint8_t *)GP(x8 + 0x155) = vis && !s[0xdd] ? *(uint8_t *)GP(lp(S + 0x80) + 0x155) != 0 : 0;
    x8 = lp(S + 0x70);
    int lit = 0;                      /* the light (S+0x88) follows the sprite */
    if (!vis) {
        *(uint8_t *)GP(x8 + 0x155) = 0;
        x8 = lp(S + 0x78);
        if (x8)
            *(uint8_t *)GP(x8 + 0x155) = 0;
        x8 = lp(S + 0x88);
        if (!x8)
            return;
    } else {
        const uint8_t blob = s[0xdd];
        *(uint8_t *)GP(x8 + 0x155) = blob;
        x8 = lp(S + 0x78);
        if (x8)
            *(uint8_t *)GP(x8 + 0x155) = blob != 0;
        x8 = lp(S + 0x88);
        if (!x8)
            return;
        lit = s[0xe5] != 0;
    }
    uint8_t w9 = 0;
    if (lit) {
        w9 = *(uint8_t *)GP(lp(S + 0x80) + 0x155);
        *(uint8_t *)GP(x8 + 0x90) = w9;
        w9 = w9 && *(uint8_t *)GP(x8 + 0x91) ? 1 : 0;
    } else {
        *(uint8_t *)GP(x8 + 0x90) = 0;
    }
    *(uint8_t *)GP(lp(x8 + 0x50) + 0x155) = w9;
    *(uint8_t *)GP(lp(x8 + 0x58) + 0x155) = w9;
    *(uint8_t *)GP(lp(lp(x8 + 0x60) + 0x120) + 0x155) = w9;
}

/* ---- ycPaletteTexture::SetOnDrawCall -----------------------------------------------------------
 * A palette's textures on a draw call (units at +0x108 and +0x118: the palette
 * and, for a palette group entry (P+0x30 >= 0, ycMaterials' table at
 * 0x60210c0), the group's), and 1/width of each into the constants. */
static uint64_t pal_group_tex(uint8_t *gm_, uint64_t tab, uint32_t idx)
{
    const uint64_t ent = tab + (uint64_t)idx * 64, t = lp(ent);
    if (t && *(uint8_t *)GP(ent + 0x3e))
        return lp((rt_ld32(GP(ent + 0x30)) & 0x80000000u) ? t + 0x18 : t + 0x20);
    return lp(ent + 8);
}
void nat__ZNK16ycPaletteTexture13SetOnDrawCallEP16ycRenderDrawCallPf(Ctx *c)
{
    GM_BEGIN;
    const uint64_t P = c->x[0], D = c->x[1], out = c->x[2];
    uint64_t t = lp(P);
    if (t && *(uint8_t *)GP(P + 0x3e)) {
        const uint32_t w = rt_ld32(GP(P + 0x30));
        t = lp(!(w & 0x80000000u) && lp(0x60210c0) ? t + 0x20 : t + 0x18);
    } else {
        t = lp(P + 8);
    }
    if (!t)
        return;
    rt_st32(GP(D + 0x108), 0x200);
    memcpy(GP(D + 0x110), &t, 8);
    rt_st32(GP(D + 0x1bc), 0x80000100u);
    uint32_t w = rt_ld32(GP(P + 0x30));
    const uint64_t tab = lp(0x60210c0);
    if (!(w & 0x80000000u) && tab) {
        const uint64_t g = pal_group_tex(gm_, tab, w);
        rt_st32(GP(D + 0x118), 0x200);
        memcpy(GP(D + 0x120), &g, 8);
        rt_st32(GP(D + 0x1c0), 0x80000100u);
        w = rt_ld32(GP(P + 0x30));
    }
    stfp(out, 1.0f / (float)rt_ld32(GP(t + 8)));
    if ((w & 0x80000000u) || !tab)
        return;
    stfp(out + 4, 1.0f / (float)rt_ld32(GP(pal_group_tex(gm_, tab, w) + 8)));
}

/* ---- AnimTile::UpdateLayerProps ----------------------------------------------------------------
 * An animated tile follows its layer (T+0x180 level, T+0x1ac layer, 0x1d8 bytes
 * each): its entity's position (the layer's offset snapped to whole pixels),
 * the layer's render pass, its visibility and visible rect, its palette. The
 * game's operations, in order. */
void f_56701c_orig(Ctx *c);
void f_1e10f4(Ctx *c);     /* ycRenderPass::Unregister */
void f_1e0f7c(Ctx *c);     /* ycRenderPass::Register */
void f_9a7b98(Ctx *c);     /* GameAnim::Update(float) */
void f_1b49d0(Ctx *c);     /* ycPaletteTexture::~ycPaletteTexture */
void nat__ZN8ycEntity21RefreshWorldTransformEb(Ctx *c);
static void anim_tile_layer_props(Ctx *c)
{
    GM_BEGIN;
    const uint64_t T = c->x[0];
    uint32_t li = rt_ld32(GP(T + 0x1ac));
    const uint64_t L = lp(lp(T + 0x180) + 0x40) + (uint64_t)li * 0x1d8;
    const float k0 = ldfp(0x6039f5c), k1 = ldfp(0x6039f58);
    float px[2];
    for (int i = 0; i < 2; i++) {
        float v = ldfp(L + 0x13c + 4 * i) * k0;
        v = v + 0.5f;
        v = floorf(v);
        v = v * k1;
        px[i] = ldfp(T + 0x1a0 + 4 * i) + v;
    }
    const float base = ldfp(T + 0x1a8);
    const float s4 = base + 0.0f;
    float z = base + ldfp(L + 0x144);
    z = z + hexf(0xbba3d70a);
    z = *(uint8_t *)GP(T + 0x1b7) ? s4 : z;
    z = *(uint8_t *)GP(T + 0x1bd) ? z : 0.0f;
    const uint64_t E = lp(T + 0x10);
    if (!(ldfp(E + 0xdc) == px[0]) || !(ldfp(E + 0xe0) == px[1]) || !(ldfp(E + 0xe4) == z)) {
        stfp(E + 0x9c, px[0]);
        stfp(E + 0xa0, px[1]);
        stfp(E + 0xa4, z);
        const uint64_t x1 = c->x[1];
        c->x[0] = E;
        nat__ZN8ycEntity21RefreshWorldTransformEb(c);
        c->x[1] = x1;
        li = rt_ld32(GP(T + 0x1ac));
    }
    /* the layer's render pass */
    if (!((int32_t)li < 0) && *(uint8_t *)GP(T + 0x1b9)) {
        const uint64_t pass = lp(lp(lp(T + 0x180) + 0x40) + (uint64_t)li * 0x1d8 + 0x68);
        const uint64_t ro = lp(T + 0x120), cur = lp(ro + 0xc0);
        if (pass != cur) {
            if (cur) {
                c->x[0] = cur;
                c->x[1] = ro + 0x58;
                f_1e10f4(c);
            }
            c->x[0] = pass;
            c->x[1] = ro + 0x58;
            f_1e0f7c(c);
            li = rt_ld32(GP(T + 0x1ac));
        }
    }
    uint64_t ro = lp(T + 0x120);
    {
        const uint64_t mask = lp(T + 0x198);
        const uint64_t lv = lp(lp(T + 0x180) + 0x40);
        const uint8_t layer_on = *(uint8_t *)GP(lv + (uint64_t)li * 0x1d8 + 0xb4);
        uint32_t w9;
        int hidden = 0;
        if (mask) {
            if (!layer_on || !*(uint8_t *)GP(T + 0x1b4))
                hidden = 1;
            else
                w9 = *(uint16_t *)GP(mask);
        } else {
            if (!layer_on)
                hidden = 1;
            else
                w9 = *(uint8_t *)GP(T + 0x1b4);
        }
        if (hidden) {
            *(uint8_t *)GP(ro + 0x155) = 0;
        } else {
            uint8_t vis = w9 != 0;
            *(uint8_t *)GP(ro + 0x155) = vis;
            if (*(uint8_t *)GP(T + 0x1b8) && vis) {
                const uint64_t le = lv + (uint64_t)li * 0x1d8;
                float s11 = ldfp(le + 0xc0), s10 = ldfp(le + 0xc4), s8 = ldfp(le + 0xc8), s9 = ldfp(le + 0xcc);
                if (!*(uint8_t *)GP(T + 0x1be)) {
                    float r[4];
                    memcpy(r, GP(le + 0x198), 16);
                    const uint64_t saved_sp = c->sp, fr = saved_sp - 0x80;
                    c->sp = fr;
                    c->x[0] = ro;
                    c->x[1] = 0;
                    c->x[8] = fr + 0x18;
                    static const GuestFunc *ic;
                    RT_CALL_IC(c, lp(lp(ro) + 0x90), ic);
                    c->sp = saved_sp;
                    vis = 0;
                    ro = lp(T + 0x120);
                    const float b0 = ldfp(fr + 0x18), b1 = ldfp(fr + 0x1c), b3 = ldfp(fr + 0x24), b4 = ldfp(fr + 0x28);
                    float d = r[0] - b0;
                    d = d > 0.0f ? d : -d;
                    const float e0 = r[2] + b3;
                    if (!(d > e0)) {
                        float d1 = r[1] - b1;
                        d1 = d1 > 0.0f ? d1 : -d1;
                        const float e1 = r[3] + b4;
                        vis = d1 <= e1;
                    }
                    *(uint8_t *)GP(ro + 0x155) = vis;
                    const float lo0 = r[0] - r[2];
                    s11 = s11 > lo0 ? s11 : lo0;
                    const float hi0 = r[2] + r[0];
                    s10 = s10 < hi0 ? s10 : hi0;
                    const float lo1 = r[1] - r[3];
                    s8 = s8 > lo1 ? s8 : lo1;
                    const float hi1 = r[3] + r[1];
                    s9 = s9 < hi1 ? s9 : hi1;
                }
                if (vis) {
                    const int both = ldfp(ro + 0xf8) > ldfp(ro + 0xfc) && s11 > s10;
                    const float a = both ? hexf(0xff7fffff) : s11, b = both ? hexf(0x7f7fffff) : s10;
                    stfp(ro + 0xf0, a);
                    stfp(ro + 0xf4, b);
                    stfp(ro + 0xf8, s8);
                    stfp(ro + 0xfc, s9);
                    if (a > b) {
                        if (s8 > s9)
                            memcpy(GP(ro + 0xf8), GP(0x1d4ec00), 8);
                        memcpy(GP(ro + 0xf0), GP(0x1d4ec00), 8);
                    }
                    c->x[0] = ro;
                    c->v[0] = vset_f32(0.0f);
                    f_9a7b98(c);
                    ro = lp(T + 0x120);
                    c->x[0] = ro;
                    static const GuestFunc *ic2;
                    RT_CALL_IC(c, lp(lp(ro) + 0x78), ic2);
                }
            }
        }
    }
    /* the layer's palette */
    if (!*(uint8_t *)GP(T + 0x1b6))
        return;
    uint64_t r8 = lp(T + 0x120), lv = lp(lp(T + 0x180) + 0x40);
    li = rt_ld32(GP(T + 0x1ac));
    const uint64_t pal = lp(lv + (uint64_t)li * 0x1d8 + 0x98);
    const uint64_t old = lp(r8 + 0x128);
    const uint8_t owned = *(uint8_t *)GP(r8 + 0x1c8);
    *(uint8_t *)GP(r8 + 0x1c8) = 0;
    memcpy(GP(r8 + 0x128), &pal, 8);
    if (owned && old) {
        const uint32_t refs = rt_ld32(GP(old + 0x34)) - 1;
        rt_st32(GP(old + 0x34), refs);
        if (!refs) {
            const uint64_t alloc = lp(0x6053520);
            c->x[0] = old;
            f_1b49d0(c);
            c->x[0] = alloc;
            c->x[1] = old;
            c->x[2] = 0;
            static const GuestFunc *ic3;
            RT_CALL_IC(c, lp(lp(alloc) + 8), ic3);
            r8 = lp(T + 0x120);
            lv = lp(lp(T + 0x180) + 0x40);
            li = rt_ld32(GP(T + 0x1ac));
        }
    }
    rt_st32(GP(r8 + 0x134), rt_ld32(GP(lv + (uint64_t)li * 0x1d8 + 0xb0)));
}
void nat__ZN8AnimTile16UpdateLayerPropsEv(Ctx *c)
{
    anim_tile_layer_props(c);
}

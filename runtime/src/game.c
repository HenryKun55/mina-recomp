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
void f_ae1690(Ctx *c);     /* StatusMenu::Warp(int level): the game's own warp (a transition to that level) */
int mina_level_index(const char *name);
/* (tests) a level named in ux0:data/mina/warp.txt (MINA_WARP_FILE on the Mac) is
 * warped to by the game's StatusMenu::Warp, from the game thread, once per write */
static void warp_requests(Ctx *c)
{
    static uint32_t tick;
    if (++tick % 30)
        return;
#if defined(__vita__)
    const char *path = "ux0:data/mina/warp.txt";
#else
    const char *path = getenv("MINA_WARP_FILE");
    if (!path)
        return;
#endif
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char name[128] = { 0 };
    const int ok = fscanf(f, "%127s", name) == 1;
    fclose(f);
    remove(path);
    if (!ok)
        return;
    const int idx = mina_level_index(name);
    rt_log("[warp] %s -> level %d\n", name, idx);
    if (idx <= 0)
        return;
    const Ctx save = *c;
    c->x[0] = 0;
    c->x[1] = (uint64_t)idx;
    f_ae1690(c);
    *c = save;
}
static void phys_stats_tick(void);
static int g_in_fixed;     /* in Game::FixedUpdate (MINA_RPU_STATS: where the passes update) */
volatile int g_game_stage; /* the watchdog's view: 2 in Game::FixedUpdate, 1 in Game::Update */
void nat__ZN4Game11FixedUpdateEv(Ctx *c)
{
    warp_requests(c);
    uint64_t t0 = now_us();
    uint32_t before = recalcs_total;
    g_fixed_updates++;
    g_in_fixed = 1;
    g_game_stage = 2;
    f_99f430_orig(c);
    g_game_stage = 0;
    g_in_fixed = 0;
    g_fixed_us += now_us() - t0;
    static int heavy;          /* consecutive fixed updates moving thousands of transforms */
    heavy = recalcs_total - before > 1500 ? heavy + 1 : 0;
    if (heavy == 20 && !g_in_play) {
        g_in_play = 1;
        rt_log("[play] gameplay (%u world transforms in a fixed update)\n", recalcs_total - before);
    }
    phys_stats_tick();
}

void f_9a0638_orig(Ctx *c);
void nat__ZN4Game6UpdateEf(Ctx *c)
{
    uint64_t t0 = now_us();
    g_updates++;
    g_game_stage = 1;
    f_9a0638_orig(c);
    g_game_stage = 0;
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
/* ycRenderPass::Update's per-camera work, by the camera's matrices' bytes */
typedef struct { uint32_t key[32]; float inv[16]; PlaneA act[6]; int nact; float soa[7][8]; } CamCache;
static CamCache cam_cache[4];
static int cam_cache_n, cam_cache_next;
static CamCache *cam_cache_find(const void *ab)
{
    for (int i = 0; i < cam_cache_n; i++) {
        const uint32_t *k = cam_cache[i].key, *m = ab;
        uint32_t d = 0;
        for (int j = 0; j < 32; j++)
            d |= k[j] ^ m[j];
        if (!d)
            return &cam_cache[i];
    }
    return NULL;
}
static CamCache *cam_cache_slot(const void *ab)
{
    CamCache *cc = &cam_cache[cam_cache_next];
    cam_cache_next = (cam_cache_next + 1) & 3;
    if (cam_cache_n < 4)
        cam_cache_n++;
    memcpy(cc->key, ab, 128);
    return cc;
}
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

/* grow_bounds and grow_bounds_vis for x, y, z at once: the same operations per
 * axis (blo > lo ? lo : blo is lo < blo ? lo : blo, NaN and signed zeros
 * included; the additions commute), so one NEON lane each; vcgt.f32 itself
 * (GCC turns the intrinsic compare into a VFP compare per lane) */
static inline uint32x4_t vgt4(float32x4_t a, float32x4_t b)
{
    uint32x4_t r;
    __asm__("vcgt.f32 %q0, %q1, %q2" : "=w"(r) : "w"(a), "w"(b));
    return r;
}
static inline void grow4(float32x4_t *bc, float32x4_t *be, float32x4_t bx, float32x4_t bex)
{
    float32x4_t lo = vsubq_f32(*bc, *be);
    const float32x4_t blo = vsubq_f32(bx, bex);
    lo = vbslq_f32(vgt4(blo, lo), lo, blo);
    float32x4_t hi = vaddq_f32(*bc, *be);
    const float32x4_t bhi = vaddq_f32(bx, bex);
    hi = vbslq_f32(vgt4(bhi, hi), bhi, hi);
    *be = vmulq_n_f32(vsubq_f32(hi, lo), 0.5f);
    *bc = vaddq_f32(lo, *be);
}

/* MINA_RPU_STATS: per 60 calls of ycRenderPass::Update, objects seen, culled,
 * keyed (by a call) and sorted, and the time in the scan and the sort */
static struct { uint64_t calls, objs, culled, keyed, sorted, scan_us, sort_us, slots, views, in_fixed; } rpu;
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
    static uint64_t oa[MAXN];
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
    /* The game's merges (bottom-up, a tie taking the right run first) give the
     * order by key, then by position from last to first: any sort by that pair
     * gives the same. (key, ~position) in one 64-bit value: insertion for the
     * usual few objects, merges for more; the objects follow at the end. */
    static uint64_t pa[MAXN], pb[MAXN];
    /* the big passes: the same keys as last time (a still camera), the same order */
    typedef struct { uint64_t rp; uint32_t v, n, cap; uint32_t *keys; uint16_t *order; } SortMemo;
    static SortMemo memo[32];
    SortMemo *sm = NULL;
    if (n > 48) {
        sm = &memo[((uint32_t)RP >> 4 ^ v * 7) & 31];
        if (sm->rp == RP && sm->v == v && sm->n == n && !memcmp(sm->keys, keys, 4 * (size_t)n)) {
            const uint8_t *ob8 = GP(o0);
            for (uint32_t j = 0; j < n; j++)
                memcpy(&oa[j], ob8 + 8 * j, 8);
            uint32_t *kd = GP(k0);
            uint8_t *od = GP(o0);
            for (uint32_t j = 0; j < n; j++) {
                const uint32_t k = sm->order[j];
                kd[j] = sm->keys[k];
                memcpy(od + 8 * j, &oa[k], 8);
            }
            return;
        }
    }
    for (uint32_t j = 0; j < n; j++)
        pa[j] = (uint64_t)keys[j] << 32 | (uint32_t)~j;
    if (sm) {
        if (sm->cap < n) {
            free(sm->keys);
            free(sm->order);
            sm->cap = n + 64;
            sm->keys = malloc(4 * (size_t)sm->cap);
            sm->order = malloc(2 * (size_t)sm->cap);
        }
        memcpy(sm->keys, keys, 4 * (size_t)n);
    }
    uint64_t *srt = pa;
    if (n <= 48) {
        for (uint32_t j = 1; j < n; j++) {
            const uint64_t v = pa[j];
            uint32_t k = j;
            while (k && pa[k - 1] > v) {
                pa[k] = pa[k - 1];
                k--;
            }
            pa[k] = v;
        }
    } else {
        uint64_t *src = pa, *dst = pb;
        for (uint32_t w = 1; w < n; w *= 2) {
            for (uint32_t s0 = 0; s0 < n; s0 += 2 * w) {
                const uint32_t mid = s0 + w < n ? s0 + w : n, end = s0 + 2 * w < n ? s0 + 2 * w : n;
                uint32_t x = s0, y = mid, d = s0;
                while (x < mid && y < end)
                    dst[d++] = src[x] < src[y] ? src[x++] : src[y++];
                while (x < mid)
                    dst[d++] = src[x++];
                while (y < end)
                    dst[d++] = src[y++];
            }
            uint64_t *t = src;
            src = dst;
            dst = t;
        }
        srt = src;
    }
    {
        const uint8_t *ob8 = GP(o0);
        for (uint32_t j = 0; j < n; j++)
            memcpy(&oa[j], ob8 + 8 * j, 8);
        uint32_t *kd = GP(k0);
        uint8_t *od = GP(o0);
        for (uint32_t j = 0; j < n; j++) {
            kd[j] = (uint32_t)(srt[j] >> 32);
            memcpy(od + 8 * j, &oa[~(uint32_t)srt[j]], 8);
        }
    }
    if (sm) {
        for (uint32_t j = 0; j < n; j++)
            sm->order[j] = (uint16_t)~(uint32_t)srt[j];
        sm->rp = RP;
        sm->v = v;
        sm->n = n;
    }
}
static void render_pass_update(Ctx *c)
{
    GM_BEGIN;
    static int stats = -1;
    if (stats < 0)
        stats = getenv("MINA_RPU_STATS") != NULL;
    uint64_t t_scan = stats ? now_us() : 0;
    if (stats) {
        rpu.calls++;
        rpu.in_fixed += g_in_fixed;
    }
    const uint64_t RP = c->x[0];
    if (!*(uint8_t *)GP(RP + 0x258))
        return;
    const uint64_t saved_sp = c->sp, fr = saved_sp - 0x80;    /* campos +0, camdir +0x10, inverse +0x20 */
    c->sp = fr;
    for (uint32_t v = 0; v < 5; v++) {
        const uint64_t cam = lp(RP + 0x10 + 8 * v);
        if (!cam)
            continue;
        if (stats)
            rpu.views++;
        const uint32_t mask = *(uint8_t *)GP(RP + 0x9);
        const float *A = GP(cam), *B = GP(cam + 0x40);
        /* the camera's matrices (A, B: 128 bytes) seen in an earlier call: its
         * inverse and planes again (the passes of a frame share a few cameras) */
        CamCache *cc = cam_cache_find(A);
        PlaneA *act;
        int nact;
        if (cc) {
            memcpy(GP(fr + 0x20), cc->inv, 64);
            act = cc->act;
            nact = cc->nact;
        } else {
        cc = cam_cache_slot(A);
        act = cc->act;
        /* camera position: the translation of B's inverse (the game's code) */
        c->x[0] = cam + 0x40;
        c->x[8] = fr + 0x20;
        f_a33c0(c);
        memcpy(cc->inv, GP(fr + 0x20), 64);
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
        nact = 0;
        /* the side planes first (in a 2D game they cull; the order of the
           tests does not change the result: any plane culls) */
        static const int porder[6] = { 2, 3, 4, 5, 0, 1 };
        for (int kk = 0; kk < 6; kk++) {
            const int k = porder[kk];
            if (__builtin_fabsf(P[k].w) == __builtin_inff())
                continue;
            act[nact] = (PlaneA){ P[k].x, P[k].y, P[k].z, P[k].w, fabs_sel(P[k].x), fabs_sel(P[k].y), fabs_sel(P[k].z) };
            nact++;
        }
        cc->nact = nact;
        /* the planes by component, 4 to a NEON register; the padding never
           culls (0 < -inf is false, and so is NaN < -inf) */
        for (int k = 0; k < 8; k++) {
            const int on = k < nact;
            cc->soa[0][k] = on ? act[k].x : 0.0f;
            cc->soa[1][k] = on ? act[k].y : 0.0f;
            cc->soa[2][k] = on ? act[k].z : 0.0f;
            cc->soa[3][k] = on ? act[k].ax : 0.0f;
            cc->soa[4][k] = on ? act[k].ay : 0.0f;
            cc->soa[5][k] = on ? act[k].az : 0.0f;
            cc->soa[6][k] = on ? act[k].w : -__builtin_inff();
        }
        }
        const float *Binv = GP(fr + 0x20);
        float *campos = GP(fr), *camdir = GP(fr + 0x10);
        campos[0] = Binv[12]; campos[1] = Binv[13]; campos[2] = Binv[14];
        memcpy(camdir, GP(cam + 0x10c), 12);
        /* bounds of all objects: empty */
        float *abm = GP(RP + 0xb0 + 0x18 * v);        /* center 3, extent 3 */
        const float neg_max = hexf(0xfe7fffff);
        /* (in registers; stored at the end) */
        float32x4_t abc = vdupq_n_f32(0.0f), abe = vdupq_n_f32(neg_max);
        float32x4_t vcc = vdupq_n_f32(0.0f), vce = vdupq_n_f32(neg_max);
        const uint32_t skip_flag = ((1u << v) & mask) ? 8 : 0;
        /* the planes in registers for the whole loop */
        float32x4_t pl[2][7];
        for (int q = 0; q < 2; q++)
            for (int m = 0; m < 7; m++)
                pl[q][m] = vld1q_f32(&cc->soa[m][4 * q]);
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
                if (stats)
                    rpu.slots++;
                if (!obj)
                    continue;
                left--;
                if (stats)
                    rpu.objs++;
                const uint32_t boxa = page + 0x4800 + 0x18 * j;
                const float *box = GP(boxa);
                const float32x4_t bx = vld1q_f32(box), bex = vld1q_f32(box + 3);   /* (lane 3: the next float, unused) */
                grow4(&abc, &abe, bx, bex);
                const uint8_t layer = *(uint8_t *)GP(page + 8 * j + 0x6001);
                if (!(*(uint8_t *)GP(RP + 0x8) & layer))
                    continue;
                const uint64_t fa = page + 8 * j + 0x6000;
                uint8_t f = *(uint8_t *)GP(fa);
                if (f & skip_flag)
                    continue;
                if (f & 1) {
                    /* outside any plane: x b0 + b1 y + b2 z + (ax b3 + ay b4 + az b5) < w,
                     * each product rounded, then the additions in that order
                     * (vmul + vadd: no fusing); 4 planes at a time, no
                     * VFP compare-and-transfer per plane */
                    int culled = 0;
                    for (int q4 = 0; q4 < nact; q4 += 4) {
                        const float32x4_t *pq = pl[q4 >> 2];
                        float32x4_t dd = vmulq_n_f32(pq[0], box[0]);
                        dd = vaddq_f32(dd, vmulq_n_f32(pq[1], box[1]));
                        dd = vaddq_f32(dd, vmulq_n_f32(pq[2], box[2]));
                        float32x4_t r = vmulq_n_f32(pq[3], box[3]);
                        r = vaddq_f32(r, vmulq_n_f32(pq[4], box[4]));
                        r = vaddq_f32(r, vmulq_n_f32(pq[5], box[5]));
                        /* vcgt.f32 itself: GCC turns vcltq_f32 into a VFP compare per lane */
                        uint32x4_t lt;
                        const float32x4_t sum = vaddq_f32(dd, r), wv = pq[6];
                        __asm__("vcgt.f32 %q0, %q1, %q2" : "=w"(lt) : "w"(wv), "w"(sum));
                        const uint32x2_t m = vorr_u32(vget_low_u32(lt), vget_high_u32(lt));
                        if (vget_lane_u32(vpmax_u32(m, m), 0)) {
                            culled = 1;
                            break;
                        }
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
                grow4(&vcc, &vce, vld1q_f32(box), vld1q_f32(box + 3));   /* (read again, as the game does) */
            }
        }
        float ab[8];
        vst1q_f32(ab, abc);
        vst1q_f32(ab + 4, abe);
        abm[0] = ab[0]; abm[1] = ab[1]; abm[2] = ab[2];
        abm[3] = ab[4]; abm[4] = ab[5]; abm[5] = ab[6];
        {   /* MINA_POISON_ALLBOUNDS: is the bounds of all objects read at all? */
            static int poison = -1;
            if (poison < 0)
                poison = getenv("MINA_POISON_ALLBOUNDS") != NULL;
            if (poison)
                for (int k = 0; k < 6; k++)
                    abm[k] = __builtin_nanf("");
        }
        float *vb = GP(RP + 0x150 + 0x18 * v);
        {
            float v8[8];
            vst1q_f32(v8, vcc);
            vst1q_f32(v8 + 4, vce);
            vb[0] = v8[0]; vb[1] = v8[1]; vb[2] = v8[2];
            vb[3] = v8[4]; vb[4] = v8[5]; vb[5] = v8[6];
        }
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
        rt_log("[rpu] per call: %.0f objects, %.0f culled, %.0f keyed, %.0f sorted; %.1f us scan, %.1f us sort; %.0f slots, %.2f views, %.0f%% in fixed updates\n",
               rpu.objs / 600.0, rpu.culled / 600.0, rpu.keyed / 600.0, rpu.sorted / 600.0, rpu.scan_us / 600.0,
               rpu.sort_us / 600.0, rpu.slots / 600.0, rpu.views / 600.0, rpu.in_fixed / 6.0);
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
    {
        Ctx *thread_ctx(void);
        Ctx *tc = thread_ctx();
        rt_log("[thread] %s is guest thread %u\n", what, tc ? tc->thread_id : 0);
    }
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

/* ---- AnimTile::PostUpdate ---------------------------------------------------------------------
 * dt = ctx+8 scaled by the world's time scale (entity's world +0x68): an
 * optional virtual update, the layer props, the layer scroll speed, the
 * sprite's animation. */
void nat__ZN8AnimTile10PostUpdateEP20ycUpdateQueueContext(Ctx *c)
{
    GM_BEGIN;
    const uint64_t T = c->x[0], Q = c->x[1];
    if (*(uint8_t *)GP(T + 0x1bb)) {
        const float dt = ldfp(Q + 8) * ldfp(lp(T + 0x10) + 0x68);
        c->x[0] = T;
        c->v[0] = vset_f32(dt);
        static const GuestFunc *ic;
        RT_CALL_IC(c, lp(lp(T) + 0xc0), ic);
    }
    const float s8 = ldfp(Q + 8), s9 = ldfp(lp(T + 0x10) + 0x68);
    c->x[0] = T;
    c->x[1] = Q;
    anim_tile_layer_props(c);
    if (*(uint8_t *)GP(T + 0x1b5)) {
        const float s0 = s8 * s9;
        const float now = ldfp(lp(lp(T + 0x10) + 0x50) + 0x1810), was = ldfp(T + 0x1b0);
        stfp(T + 0x1b0, now);
        float v = 0.0f;
        if (s0 != 0.0f)
            v = (now - was) / s0;
        stfp(lp(T + 0x120) + 0x178, v);
    }
    if (*(uint8_t *)GP(T + 0x1bc)) {
        const uint64_t ro = lp(T + 0x120);
        const float dt = ldfp(Q + 8) * ldfp(lp(ro + 0x10) + 0x68);
        c->x[0] = ro;
        c->v[0] = vset_f32(dt);
        f_9a7b98(c);
    }
}

/* ---- SessionManager::StartActiveSaveSlot -------------------------------------------------------
 * MINA_START_LEVEL (tests): the game, once its save slot starts, goes to that
 * level instead (the level index it was about to warp to, 0x1e48120) */
void f_a907dc_orig(Ctx *c);
int mina_start_level_index(void);
void nat__ZN14SessionManager19StartActiveSaveSlotEv(Ctx *c)
{
    f_a907dc_orig(c);
    const int idx = mina_start_level_index();
    if (idx >= 0) {
        rt_log("[levels] the save slot starts in level %d (was %d)\n", idx, (int)(int32_t)rt_ld32(G(0x1e48120)));
        rt_st32(G(0x1e48120), (uint32_t)idx);
    }
}

/* ---- PhysicsManager::Overlap -------------------------------------------------------------------
 * Which physics objects and tile layers a set of query shapes (PhysicsQueryShape,
 * 40 bytes: mask, flags, shape) at pos overlaps; the listener (L) gets each hit.
 * The recompiled scan reloads its spilled locals for every slot of the 12 buckets
 * of 64-slot pools; here the occupied slots come from the free bitmap's zeros.
 * Pool slot (0x40): +0 half x/y, +8 offset x/y, +0x10 pos x/y/z, +0x20 owner,
 * +0x28/+0x30 masks, +0x3a flags, +0x3c z. Same memory reads and calls, in the
 * same order, as the game's code (callbacks may change what follows).
 * MINA_VERIFY_PHYS=1: each call runs the game's code, and the shapes this one
 * would test (its ycGeo::Intersects and TileLayerOverlap calls, made without
 * calling them) are compared with the game's calls. MINA_PHYS_STATS=1: counts. */
void f_1abdf4_orig(Ctx *c);     /* ycGeo::Intersects */
void f_7bdfe4_orig(Ctx *c);     /* TileLevelCollision::TileLayerOverlap */
void f_9748c0_orig(Ctx *c);     /* PhysicsManager::Overlap */

typedef struct { uint32_t a, b, c, d; } PhysRec;
static PhysRec phys_rec_buf[2][512];
static int phys_rec_n[2], phys_rec_on = -1;   /* [0]: the game's calls, [1]: this one's */
static uint32_t phys_q, phys_slots, phys_cand, phys_tn[3];
static int phys_stats_on = -1;
static void phys_stats_tick(void)
{
    static int n;
    if (phys_stats_on < 0)
        phys_stats_on = getenv("MINA_PHYS_STATS") != NULL;
    if (!phys_stats_on || ++n < 300)
        return;
    rt_log("[phys] per fixed update: %.1f queries, %.0f slots scanned, %.1f shape tests; overlap tiles: %.0f entities, %.0f components, %.0f layers\n",
           phys_q / 300.0, phys_slots / 300.0, phys_cand / 300.0, phys_tn[0] / 300.0, phys_tn[1] / 300.0, phys_tn[2] / 300.0);
    memset(phys_tn, 0, sizeof phys_tn);
    n = 0;
    phys_q = phys_slots = phys_cand = 0;
}
static inline void phys_rec(int w, uint32_t a, uint32_t b, uint32_t c_, uint32_t d)
{
    if (phys_rec_n[w] < 512)
        phys_rec_buf[w][phys_rec_n[w]] = (PhysRec){a, b, c_, d};
    phys_rec_n[w]++;
}
void nat__ZN5ycGeo10IntersectsEjRK7ycShapeRK6ycVec3jS2_S5_j(Ctx *c)
{
    if (phys_rec_on > 0)
        phys_rec(0, (uint32_t)c->x[1], (uint32_t)c->x[3], (uint32_t)c->x[4], (uint32_t)c->x[5]);
    f_1abdf4_orig(c);
}
static void tile_layer_overlap(Ctx *c, int dry);
static int tile_rec_on = -1, tile_rec_stop;
/* MINA_VERIFY_TILE=1: the tiles TileLayerOverlap hands to the callback, the
 * game's (from the callback, CheckOverlap) and this one's (without calling it) */
void f_7bb798_orig(Ctx *c);
void nat__ZN33PhysicsTileLevelCollisionCallback12CheckOverlapERKN26TileLevelCollisionCallback11CollideInfoEbRK6ycVec39ShapeTypePK7ycShape(Ctx *c)
{
    if (tile_rec_on > 0 && !tile_rec_stop) {
        GM_BEGIN;
        const uint64_t ci = c->x[1];
        phys_rec(0, rt_ld32(GP(ci + 8)), rt_ld32(GP(ci + 0xc)), rt_ld32(GP(ci + 0x20)) & 0xffffff,
                 ((uint32_t)c->x[2] & 1) | (uint32_t)*(uint8_t *)GP(ci + 0x24) << 8);
    }
    f_7bb798_orig(c);
    if (tile_rec_on > 0 && ((uint32_t)c->x[0] & 1))
        tile_rec_stop = 1;
}
void nat__ZN18TileLevelCollision16TileLayerOverlapEPN26TileLevelCollisionCallback11CollideInfoERKNS_11CommonParamE9ShapeTypePK7ycShapeRK6ycVec3(Ctx *c)
{
    if (phys_rec_on > 0) {
        GM_BEGIN;
        phys_rec(0, (uint32_t)c->x[0] | 0x80000000u, (uint32_t)c->x[4], rt_ld32(GP(c->x[1] + 0x18)), rt_ld32(GP(c->x[1] + 0x1c)));
        f_7bdfe4_orig(c);
        return;
    }
    if (tile_rec_on < 0)
        tile_rec_on = getenv("MINA_VERIFY_TILE") != NULL;
    if (tile_rec_on > 0) {
        static uint32_t calls, bad;
        Ctx save = *c;
        phys_rec_n[1] = 0;
        tile_layer_overlap(c, 1);
        *c = save;
        phys_rec_n[0] = 0;
        tile_rec_stop = 0;
        f_7bdfe4_orig(c);
        calls++;
        const int n0 = phys_rec_n[0], n1 = phys_rec_n[1];
        const int ok = (tile_rec_stop ? n1 >= n0 : n0 == n1) &&
                       !memcmp(phys_rec_buf[0], phys_rec_buf[1], sizeof(PhysRec) * (n0 < 512 ? n0 : 512));
        if (!ok && bad++ < 20) {
            rt_log("[verify] TileLayerOverlap differs (%u of %u): game %d tiles, mine %d%s\n", bad, calls, n0, n1,
                   tile_rec_stop ? " (stopped)" : "");
            for (int i = 0; i < 4 && (i < n0 || i < n1); i++) {
                const PhysRec *a = &phys_rec_buf[0][i], *b = &phys_rec_buf[1][i];
                rt_log("  %d: %08x %08x %08x %08x | %08x %08x %08x %08x\n", i, i < n0 ? a->a : 0, i < n0 ? a->b : 0,
                       i < n0 ? a->c : 0, i < n0 ? a->d : 0, i < n1 ? b->a : 0, i < n1 ? b->b : 0, i < n1 ? b->c : 0,
                       i < n1 ? b->d : 0);
            }
        }
        if ((calls & 0xfff) == 0)
            rt_log("[verify] TileLayerOverlap: %u calls checked, %u differ\n", calls, bad);
        return;
    }
    tile_layer_overlap(c, 0);
}

static inline uint64_t ld64g(uint8_t *gm_, uint64_t a) { return rt_ld64(GP(a)); }
#define L64(a) ld64g(gm_, (a))
#define L32(a) rt_ld32(GP(a))
#define L8(a) (*(uint8_t *)GP(a))
/* the game's lane ops: fcmgt + bsl/bit */
static inline float gt_sel(float a, float b, float t, float f) { return a > b ? t : f; }

static void phys_verify(Ctx *c, void (*nat)(Ctx *, int), void (*orig)(Ctx *), const char *name, int kreg);
static void phys_overlap(Ctx *c, int dry)
{
    GM_BEGIN;
    const uint64_t T = c->x[0], S = c->x[2], L = c->x[4];
    const uint32_t n = (uint32_t)c->x[1], kind = (uint32_t)c->x[5];
    const uint32_t z = (uint32_t)c->x[3];
    const float px = c->v[0].f[0], py = c->v[1].f[0], pz = c->v[2].f[0];
    const uint64_t saved_sp = c->sp, saved_fp = c->x[29];
    const uint64_t F = saved_sp - 0x190;
    stfp(F + 0xc0, px);
    stfp(F + 0xc4, py);
    stfp(F + 0xc8, pz);
    uint64_t ret = 0;
    if (n == 0)
        goto done;
    if (!dry)
        phys_q++;
    /* the shapes' bounds (the game's registers: types it has no case for reuse
       the previous shape's box, the first one the caller's v6/v7) */
    float h6[2] = {c->v[6].f[0], c->v[6].f[1]}, c7[2] = {c->v[7].f[0], c->v[7].f[1]};
    float ac[2] = {0.0f, 0.0f}, ah[2];
    {
        float m = hexf(0xfe7fffffu);
        ah[0] = ah[1] = m;
    }
    uint64_t mask = 0;
    const uint64_t cfg = L + 0x28dc;
    for (uint32_t i = 0; i < n; i++) {
        const uint64_t e = S + (uint64_t)i * 40;
        const uint64_t m = L64(e);
        const uint64_t sh = L64(e + 0x10);
        const uint32_t ty = L32(sh) - 1;
        if (ty <= 8) {
            const uint64_t d = L64(sh + 8);
            if (ty == 0 || ty == 8) {
                float mn[2], mx[2], v7[2];
                for (int k = 0; k < 2; k++) {
                    const float a = ldfp(d + 4 * k);
                    if (ty == 0) {
                        const float q = a + ldfp(d + 0xc + 4 * k);
                        mn[k] = gt_sel(q, a, a, q);
                        mx[k] = a;
                        v7[k] = q;
                    } else {
                        const float b = ldfp(d + 0x18 + 4 * k), mm = ldfp(d + 0xc + 4 * k);
                        float lo = gt_sel(b, a, a, b);
                        mx[k] = gt_sel(a, b, a, b);
                        mn[k] = gt_sel(lo, mm, mm, lo);
                        v7[k] = mm;
                    }
                    const float x6 = mx[k] + 0.0f;
                    const float hi = gt_sel(v7[k], x6, v7[k], x6);
                    h6[k] = (hi - mn[k]) * 0.5f;
                    c7[k] = mn[k] + h6[k];
                }
            } else if (ty == 3) {
                c7[0] = ldfp(d);
                c7[1] = ldfp(d + 4);
                h6[0] = ldfp(d + 0xc);
                h6[1] = ldfp(d + 0x10);
            } else if (ty == 7) {
                c7[0] = ldfp(d);
                c7[1] = ldfp(d + 4);
                h6[0] = h6[1] = ldfp(d + 0xc);
            }
        }
        mask |= m;
        for (int k = 0; k < 2; k++) {
            const float lo = ac[k] - ah[k], hi = ac[k] + ah[k];
            float nlo = c7[k] - h6[k];
            nlo = gt_sel(nlo, lo, lo, nlo);
            float nhi = c7[k] + h6[k];
            nhi = gt_sel(nhi, hi, nhi, hi);
            ah[k] = (nhi - nlo) * 0.5f;
            ac[k] = nlo + ah[k];
        }
    }
    if (!mask)
        goto done;
    const uint32_t zz = L8(cfg + 0xd) ? z : 0;
    const uint64_t ign = L64(L + 0x60);
    const uint32_t moff = kind == 0 ? 0x30 : 0x28, coff = kind == 0 ? 0x10 : 0x8;
    const uint32_t mlo = (uint32_t)mask, mhi = (uint32_t)(mask >> 32);
    const int stats_on = !dry && phys_stats_on > 0;
    const float cx = ac[0] + px, cy = ac[1] + py;
    const float hx = ah[0] + hexf(0x3f4ccccdu), hy = ah[1] + hexf(0x3f4ccccdu);
    const float s9 = cx - hx, s10 = hx + cx, s11 = hy + cy, s12 = cy - hy;
    c->sp = F;
    c->x[29] = F + 0x180;
    for (uint32_t b = 0; b < 12; b++) {
        const uint64_t bk = T + (uint64_t)b * 32;
        const uint64_t cnt = L64(bk + 0x168);
        if (!cnt || !L32(T + 0x2e4))
            continue;
        const uint64_t arr = L64(bk + 0x160);
        const uint64_t end = (uint32_t)(arr + cnt * 8);
        uint32_t w9 = 1;
        for (uint64_t pp = (uint32_t)arr; pp != end; pp = (uint32_t)(pp + 8)) {
            if (!w9)
                continue;
            const uint64_t pool = L64(pp);
            uint64_t O0 = L64(pool + 0x20);
            uint32_t w = 0, nw;
            for (;;) {
                const uint64_t bits = L64(L64(pool) + (uint64_t)w * 8);
                if (bits != ~0ull) {
                    if (stats_on)
                        phys_slots += __builtin_popcountll(~bits);
                    /* by 32-bit halves: one word of live slots in a register */
                    for (int hf = 0; hf < 2; hf++) {
                    uint32_t live = ~(uint32_t)(bits >> (hf * 32));
                    const uint32_t ob = (uint32_t)(O0 + hf * 0x800);
                    const uint8_t *mb = gm_ + (uint32_t)(ob + moff);
                    while (live) {
                        const uint32_t j = __builtin_ctz(live);
                        live &= live - 1;
                        const uint8_t *mp = mb + (j << 6);
                        if (!((rt_ld32(mp) & mlo) | (rt_ld32(mp + 4) & mhi)))
                            continue;
                        const uint64_t O = (uint32_t)(ob + (j << 6));
                        const uint64_t own = L64(O + 0x20);
                        if (own == ign)
                            continue;
                        if (zz) {
                            uint32_t dz = zz - L32(O + 0x3c);
                            if ((int32_t)dz < 0)
                                dz = -dz;
                            if (dz >= 2 && !(L8(O + 0x3a) & 0x80) && L8(cfg + 0xd))
                                continue;
                        }
                        {
                            const float t = ldfp(O + 0x10) + ldfp(O + 0x8), hw = ldfp(O);
                            const float lo = t - hw, hi = hw + t;
                            if (s9 > hi || lo > s10)
                                continue;
                        }
                        {
                            const float hh = ldfp(O + 0x4), t = ldfp(O + 0x14) + ldfp(O + 0xc);
                            const float lo = t - hh, hi = hh + t;
                            if (s12 > hi || lo > s11)
                                continue;
                        }
                        for (uint64_t nd = L64(own + 0x128); nd; nd = L64(nd + 0x18)) {
                            const uint64_t cp = L64(nd + 8);
                            for (uint32_t k = 0; k < (n > 1 ? n : 1); k++) {
                                const uint64_t e = S + (uint64_t)k * 40;
                                if (!(L64(cp + coff) & L64(e)))
                                    continue;
                                const uint32_t fl = L32(e + 8);
                                if (!(L32(cp + 0x84) & fl & 0x1e00))
                                    continue;
                                const uint64_t sh = L64(e + 0x10);
                                const uint32_t ty = L32(sh);
                                const uint64_t sd = L64(sh + 8);
                                const uint32_t ct = L32(cp + 0x18);
                                const uint64_t cd = L64(cp + 0x20);
                                if (fl & 0xa00) {
                                    if (!(pz == 0.0f))
                                        continue;
                                    if (!(ldfp(O + 0x18) == 0.0f))
                                        continue;
                                }
                                if (dry) {
                                    phys_rec(1, (uint32_t)sd, ct, (uint32_t)cd, (uint32_t)(O + 0x10));
                                    continue;
                                }
                                phys_cand++;
                                c->x[0] = ty;
                                c->x[1] = sd;
                                c->x[2] = F + 0xc0;
                                c->x[3] = ct;
                                c->x[4] = cd;
                                c->x[5] = O + 0x10;
                                f_1abdf4_orig(c);
                                if (!(uint32_t)c->x[0])
                                    continue;
                                rt_st64(GP(L + 0x8), T);
                                rt_st64(GP(L + 0x18), S);
                                rt_st64(GP(L + 0x3c), L64(F + 0xc0));
                                rt_st32(GP(L + 0x44), L32(F + 0xc8));
                                rt_st64(GP(L + 0x48), 0);
                                rt_st32(GP(L + 0x50), 0);
                                c->x[0] = L;
                                c->x[1] = L64(O + 0x20);
                                c->x[2] = cp;
                                c->x[3] = k;
                                rt_call(c, L64(L64(L) + 8));
                            }
                        }
                    }
                    }
                    O0 = (uint32_t)(O0 + 0x1000);
                } else {
                    O0 = (uint32_t)(O0 + 0x1000);
                }
                w++;
                nw = L32(T + 0x2e4);
                if (w == nw)
                    break;
            }
            w9 = nw;
        }
    }
    if (kind == 1) {
        if (!dry) {
            rt_st64(GP(L + 0x8), T);
            rt_st64(GP(L + 0x18), S);
            rt_st64(GP(L + 0x3c), L64(F + 0xc0));
            rt_st32(GP(L + 0x44), L32(F + 0xc8));
            rt_st64(GP(L + 0x48), 0);
            rt_st32(GP(L + 0x50), 0);
        }
        rt_st64(GP(F + 0x98), GUEST_IMAGE_BASE + 0xde6970);
        rt_st64(GP(F + 0xa0), L);
        float s0 = 1.0f;
        if (!L8(cfg))
            s0 = ldfp(L + 0x28d4);
        stfp(F + 0xa8, s0);
        rt_st64(GP(F + 0x88), F + 0x98);
        rt_st64(GP(F + 0x90), F + 0xa8);
        for (uint32_t i = 0; i < (n > 1 ? n : 1); i++) {
            const uint64_t e = S + (uint64_t)i * 40;
            rt_st64(GP(F + 0xb8), L64(e));
            rt_st32(GP(F + 0xb4), i);
            if (!L64(L64(L64(T + 0x10) + 0x50) + 0xb0))
                continue;
            for (uint64_t nd = L64(L64(L64(T + 0x10) + 0x50) + 0xb0); nd; nd = L64(nd + 0x18)) {
                uint64_t r = L64(nd + 8), q;
                if (stats_on)
                    phys_tn[0]++;
                while ((q = L64(r + 0x450)) != 0)
                    r = q;
                for (uint64_t n2 = L64(r + 0x438); n2; n2 = L64(n2 + 0x18)) {
                    const uint64_t cp = L64(n2 + 8);
                    if (stats_on)
                        phys_tn[1]++;
                    if (L32(cp + 0x370) != 5)
                        continue;
                    rt_st32(GP(F + 0xac), L32(e + 8));
                    const uint64_t X = L64(cp + 0x398);
                    const uint64_t sh = L64(e + 0x10);
                    const uint32_t ty = L32(sh);
                    const uint64_t sd = L64(sh + 8);
                    rt_st64(GP(F + 0xd0), X);
                    uint64_t x8 = L64(X), x9 = L64(x8 + 0x10);
                    if (!(L64(x9 + 0x38) >> 34))
                        continue;
                    for (uint64_t l = 0; l < (L64(x9 + 0x38) >> 34); l = (uint32_t)(l + 1)) {
                        const uint64_t lay = L64(x9 + 0x40);
                        if (stats_on)
                            phys_tn[2]++;
                        const uint64_t a = L64(lay + l * 168 + 0x60);
                        if (a) {
                            const uint64_t x12 = a & ~L64(x8 + 0xb0);
                            const uint32_t pass = (x12 & ~1ull) == 0 ? !(a & 1) : (uint32_t)a;
                            if (!(pass & 1))
                                continue;
                        }
                        if (!L8(L64(X + 8) + l))
                            continue;
                        const uint32_t lz = L32(lay + l * 168 + 0x88);
                        if (!lz)
                            continue;
                        if (zz) {
                            uint32_t dz = lz - zz;
                            if ((int32_t)dz < 0)
                                dz = -dz;
                            if (dz > 1)
                                continue;
                        }
                        rt_st32(GP(F + 0xe8), lz);
                        rt_st32(GP(F + 0xec), (uint32_t)l);
                        if (dry) {
                            phys_rec(1, (uint32_t)X | 0x80000000u, (uint32_t)sd, lz, (uint32_t)l);
                            continue;
                        }
                        c->x[0] = X;
                        c->x[1] = F + 0xd0;
                        c->x[2] = F + 0x88;
                        c->x[3] = ty;
                        c->x[4] = sd;
                        c->x[5] = F + 0xc0;
                        nat__ZN18TileLevelCollision16TileLayerOverlapEPN26TileLevelCollisionCallback11CollideInfoERKNS_11CommonParamE9ShapeTypePK7ycShapeRK6ycVec3(c);
                        x8 = L64(X);
                        x9 = L64(x8 + 0x10);
                    }
                }
            }
        }
    }
    c->sp = saved_sp;
    c->x[29] = saved_fp;
done:
    ret = L64(L + 0x1480) != 0;
    c->x[0] = ret;
}

void nat__ZN14PhysicsManager7OverlapEjP17PhysicsQueryShapei6ycVec3P20PhysicsQueryListenerj(Ctx *c)
{
    if (phys_rec_on < 0)
        phys_rec_on = getenv("MINA_VERIFY_PHYS") != NULL;
    if (phys_rec_on > 0) {
        phys_verify(c, phys_overlap, f_9748c0_orig, "Overlap", 5);
        return;
    }
    phys_overlap(c, 0);
}

/* ---- PhysicsManager::Sweep ---------------------------------------------------------------------
 * Overlap's scan for a moving query: the shapes' box swept along the segment
 * (ycLineSeg: start +0, delta +0xc), hits from ycGeo::Sweep (time of impact,
 * normal) and TileLevelCollision::TileLayerSweep. L+0x2c: the unit direction,
 * +0x38 its length, +0x3c the segment. Same verification as Overlap. */
void f_1ad96c_orig(Ctx *c);     /* ycGeo::Sweep */
void f_7bd5bc_orig(Ctx *c);     /* TileLevelCollision::TileLayerSweep */
void f_974fe0_orig(Ctx *c);     /* PhysicsManager::Sweep */
void nat__ZN5ycGeo5SweepEPfP6ycVec3jRK7ycShapeRK9ycLineSegjS5_S8_j(Ctx *c)
{
    if (phys_rec_on > 0)
        phys_rec(0, (uint32_t)c->x[3], (uint32_t)c->x[5], (uint32_t)c->x[6], (uint32_t)c->x[4]);
    f_1ad96c_orig(c);
}
void nat__ZN18TileLevelCollision14TileLayerSweepEPN26TileLevelCollisionCallback11CollideInfoERKNS_11CommonParamE9ShapeTypePK7ycShapeRK6ycVec3SC_(Ctx *c)
{
    if (phys_rec_on > 0) {
        GM_BEGIN;
        phys_rec(0, (uint32_t)c->x[0] | 0x80000000u, (uint32_t)c->x[4], rt_ld32(GP(c->x[1] + 0x18)), rt_ld32(GP(c->x[1] + 0x1c)));
    }
    f_7bd5bc_orig(c);
}

static void phys_sweep(Ctx *c, int dry)
{
    GM_BEGIN;
    const uint64_t T = c->x[0], S = c->x[2], G = c->x[4], L = c->x[5];
    const uint32_t n = (uint32_t)c->x[1], kind = (uint32_t)c->x[6];
    const uint32_t z = (uint32_t)c->x[3];
    const uint64_t saved_sp = c->sp, saved_fp = c->x[29];
    const uint64_t F = saved_sp - 0x1b0;
    if (n == 0)
        goto done;
    if (!dry)
        phys_q++;
    float h4[2] = {c->v[4].f[0], c->v[4].f[1]}, c5[2] = {c->v[5].f[0], c->v[5].f[1]};
    float ac[2] = {0.0f, 0.0f}, ah[2];
    ah[0] = ah[1] = hexf(0xfe7fffffu);
    uint64_t mask = 0;
    const uint64_t cfg = L + 0x28dc;
    for (uint32_t i = 0; i < n; i++) {
        const uint64_t e = S + (uint64_t)i * 40;
        const uint64_t m = L64(e);
        const uint64_t sh = L64(e + 0x10);
        const uint32_t ty = L32(sh) - 1;
        if (ty <= 8) {
            const uint64_t d = L64(sh + 8);
            if (ty == 0 || ty == 8) {
                for (int k = 0; k < 2; k++) {
                    const float a = ldfp(d + 4 * k);
                    float mn, x4, v5;
                    if (ty == 0) {
                        const float q = a + ldfp(d + 0xc + 4 * k);
                        mn = gt_sel(q, a, a, q);
                        x4 = a + 0.0f;
                        v5 = q;
                    } else {        /* no +0 on this path (the jump skips it) */
                        const float b = ldfp(d + 0x18 + 4 * k), mm = ldfp(d + 0xc + 4 * k);
                        const float lo = gt_sel(b, a, a, b);
                        x4 = gt_sel(a, b, a, b);
                        mn = gt_sel(lo, mm, mm, lo);
                        v5 = mm;
                    }
                    const float hi = gt_sel(v5, x4, v5, x4);
                    h4[k] = (hi - mn) * 0.5f;
                    c5[k] = mn + h4[k];
                }
            } else if (ty == 3) {
                c5[0] = ldfp(d);
                c5[1] = ldfp(d + 4);
                h4[0] = ldfp(d + 0xc);
                h4[1] = ldfp(d + 0x10);
            } else if (ty == 7) {
                c5[0] = ldfp(d);
                c5[1] = ldfp(d + 4);
                h4[0] = h4[1] = ldfp(d + 0xc);
            }
        }
        mask |= m;
        for (int k = 0; k < 2; k++) {
            const float lo = ac[k] - ah[k], hi = ac[k] + ah[k];
            float nlo = c5[k] - h4[k];
            nlo = gt_sel(nlo, lo, lo, nlo);
            float nhi = c5[k] + h4[k];
            nhi = gt_sel(nhi, hi, nhi, hi);
            ah[k] = (nhi - nlo) * 0.5f;
            ac[k] = nlo + ah[k];
        }
    }
    if (!mask)
        goto done;
    const uint32_t zz = L8(cfg + 0xd) ? z : 0;
    rt_st64(GP(F + 0xe8), 0);
    rt_st64(GP(F + 0xf0), 0);
    rt_st64(GP(F + 0xf8), 0);
    if (!dry) {
        uint8_t seg[24];
        memcpy(seg, GP(G), 24);
        memcpy(GP(L + 0x3c), seg, 24);
        rt_st64(GP(L + 0x2c), L64(G + 0xc));
        rt_st32(GP(L + 0x34), L32(G + 0x14));
        const float dx = ldfp(L + 0x2c), dy = ldfp(L + 0x30), dz = ldfp(L + 0x34);
        float len = dx * dx + dy * dy + dz * dz;
        if (len == 0.0f) {
            len = 0.0f;
        } else {
            len = sqrtf(len);
            const float inv = 1.0f / len;
            const float ux = ldfp(L + 0x2c) * inv, uy = ldfp(L + 0x30) * inv;
            stfp(L + 0x2c, ux);
            stfp(L + 0x30, uy);
            stfp(L + 0x34, inv * dz);
        }
        stfp(L + 0x38, len);
    }
    const uint64_t ign = L64(L + 0x60);
    const uint32_t moff = kind == 0 ? 0x30 : 0x28, coff = kind == 0 ? 0x10 : 0x8;
    const uint32_t mlo = (uint32_t)mask, mhi = (uint32_t)(mask >> 32);
    const int stats_on = !dry && phys_stats_on > 0;
    float lo[2], hi[2];
    {
        const float e8 = hexf(0x3f4ccccdu);
        const float h0[2] = {ah[0] + e8, ah[1] + e8};
        for (int k = 0; k < 2; k++) {
            const float p = ldfp(G + 4 * k), d = ldfp(G + 0xc + 4 * k);
            const float v3 = ac[k] + p;
            const float v1 = ac[k] + (p + d);
            const float l2 = v3 - h0[k], l4 = v1 - h0[k];
            lo[k] = l2 > l4 ? l4 : l2;
            const float u3 = h0[k] + v3, u0 = h0[k] + v1;
            hi[k] = u0 > u3 ? u0 : u3;
        }
    }
    const float hx = (hi[0] - lo[0]) * 0.5f, hy = (hi[1] - lo[1]) * 0.5f;
    const float cx = lo[0] + hx, cy = lo[1] + hy;
    const float s8 = cx - hx, s9 = hx + cx, s10 = hy + cy, s11 = cy - hy;
    c->sp = F;
    c->x[29] = F + 0x1a0;
    for (uint32_t b = 0; b < 12; b++) {
        const uint64_t bk = T + (uint64_t)b * 32;
        const uint64_t cnt = L64(bk + 0x168);
        if (!cnt || !L32(T + 0x2e4))
            continue;
        const uint64_t arr = L64(bk + 0x160);
        const uint64_t end = (uint32_t)(arr + cnt * 8);
        uint32_t w9 = 1;
        for (uint64_t pp = (uint32_t)arr; pp != end; pp = (uint32_t)(pp + 8)) {
            if (!w9)
                continue;
            const uint64_t pool = L64(pp);
            uint64_t O0 = L64(pool + 0x20);
            uint32_t w = 0, nw;
            for (;;) {
                const uint64_t bits = L64(L64(pool) + (uint64_t)w * 8);
                if (stats_on)
                    phys_slots += __builtin_popcountll(~bits);
                /* by 32-bit halves: one word of live slots in a register */
                for (int hf = 0; hf < 2; hf++) {
                uint32_t live = ~(uint32_t)(bits >> (hf * 32));
                const uint32_t ob = (uint32_t)(O0 + hf * 0x800);
                const uint8_t *mb = gm_ + (uint32_t)(ob + moff);
                while (live) {
                    const uint32_t j = __builtin_ctz(live);
                    live &= live - 1;
                    const uint8_t *mp = mb + (j << 6);
                    if (!((rt_ld32(mp) & mlo) | (rt_ld32(mp + 4) & mhi)))
                        continue;
                    const uint64_t O = (uint32_t)(ob + (j << 6));
                    const uint64_t own = L64(O + 0x20);
                    if (own == ign)
                        continue;
                    if (zz) {
                        uint32_t dz = zz - L32(O + 0x3c);
                        if ((int32_t)dz < 0)
                            dz = -dz;
                        if (dz >= 2 && !(L8(O + 0x3a) & 0x80) && L8(cfg + 0xd))
                            continue;
                    }
                    {
                        const float t = ldfp(O + 0x10) + ldfp(O + 0x8), hw = ldfp(O);
                        const float l = t - hw, h = hw + t;
                        if (s8 > h || l > s9)
                            continue;
                    }
                    {
                        const float hh = ldfp(O + 0x4), t = ldfp(O + 0x14) + ldfp(O + 0xc);
                        const float l = t - hh, h = hh + t;
                        if (s11 > h || l > s10)
                            continue;
                    }
                    for (uint64_t nd = L64(own + 0x128); nd; nd = L64(nd + 0x18)) {
                        const uint64_t cp = L64(nd + 8);
                        for (uint32_t k = 0; k < (n > 1 ? n : 1); k++) {
                            const uint64_t e = S + (uint64_t)k * 40;
                            if (!(L64(cp + coff) & L64(e)))
                                continue;
                            if (!(L32(cp + 0x84) & L32(e + 8) & 0x1e00))
                                continue;
                            const uint64_t ow = L64(O + 0x20);
                            rt_st64(GP(F + 0xe8), L64(ow + 0x140));
                            rt_st32(GP(F + 0xf0), L32(ow + 0x148));
                            const uint64_t sh = L64(e + 0x10);
                            const uint32_t ty = L32(sh);
                            const uint64_t sd = L64(sh + 8);
                            const uint32_t ct = L32(cp + 0x18);
                            const uint64_t cd = L64(cp + 0x20);
                            if (dry) {
                                phys_rec(1, (uint32_t)sd, ct, (uint32_t)cd, (uint32_t)G);
                                continue;
                            }
                            phys_cand++;
                            c->x[0] = F + 0xc0;
                            c->x[1] = F + 0x100;
                            c->x[2] = ty;
                            c->x[3] = sd;
                            c->x[4] = G;
                            c->x[5] = ct;
                            c->x[6] = cd;
                            c->x[7] = F + 0xe8;
                            f_1ad96c_orig(c);
                            if (!(uint32_t)c->x[0])
                                continue;
                            rt_st64(GP(L + 0x8), T);
                            rt_st64(GP(L + 0x18), S);
                            {
                                uint8_t seg[24];
                                memcpy(seg, GP(G), 24);
                                memcpy(GP(L + 0x3c), seg, 24);
                            }
                            c->x[0] = L;
                            c->x[1] = L64(O + 0x20);
                            c->x[2] = L64(nd + 8);
                            memset(&c->v[0], 0, 16);
                            c->v[0].f[0] = ldfp(F + 0xc0);
                            c->x[3] = F + 0x100;
                            c->x[4] = k;
                            rt_call(c, L64(L64(L)));
                        }
                    }
                }
                }
                O0 = (uint32_t)(O0 + 0x1000);
                w++;
                nw = L32(T + 0x2e4);
                if (w == nw)
                    break;
            }
            w9 = nw;
        }
    }
    if (kind == 1) {
        if (!dry) {
            rt_st64(GP(L + 0x8), T);
            rt_st64(GP(L + 0x18), S);
            uint8_t seg[24];
            memcpy(seg, GP(G), 24);
            memcpy(GP(L + 0x3c), seg, 24);
        }
        rt_st64(GP(F + 0xc0), GUEST_IMAGE_BASE + 0xde6970);
        rt_st64(GP(F + 0xc8), L);
        float s0 = 1.0f;
        if (!L8(cfg))
            s0 = ldfp(L + 0x28d4);
        stfp(F + 0xd0, s0);
        rt_st64(GP(F + 0xb0), F + 0xc0);
        rt_st64(GP(F + 0xb8), F + 0xd0);
        for (uint32_t i = 0; i < (n > 1 ? n : 1); i++) {
            const uint64_t e = S + (uint64_t)i * 40;
            rt_st32(GP(F + 0xdc), i);
            rt_st64(GP(F + 0xe0), L64(e));
            if (!L64(L64(L64(T + 0x10) + 0x50) + 0xb0))
                continue;
            for (uint64_t nd = L64(L64(L64(T + 0x10) + 0x50) + 0xb0); nd; nd = L64(nd + 0x18)) {
                uint64_t r = L64(nd + 8), q;
                while ((q = L64(r + 0x450)) != 0)
                    r = q;
                for (uint64_t n2 = L64(r + 0x438); n2; n2 = L64(n2 + 0x18)) {
                    const uint64_t cp = L64(n2 + 8);
                    if (L32(cp + 0x370) != 5)
                        continue;
                    rt_st32(GP(F + 0xd4), L32(e + 8));
                    const uint64_t X = L64(cp + 0x398);
                    const uint64_t sh = L64(e + 0x10);
                    const uint32_t ty = L32(sh);
                    const uint64_t sd = L64(sh + 8);
                    {
                        const float pz = ldfp(G + 8);
                        stfp(F + 0xa8, pz);
                        const float ez = pz + ldfp(G + 0x14);
                        rt_st64(GP(F + 0xa0), L64(G));
                        const float ex = ldfp(G) + ldfp(G + 0xc), ey = ldfp(G + 4) + ldfp(G + 0x10);
                        stfp(F + 0x90, ex);
                        stfp(F + 0x94, ey);
                        stfp(F + 0x98, ez);
                    }
                    rt_st64(GP(F + 0x100), X);
                    uint64_t x8 = L64(X), x9 = L64(x8 + 0x10);
                    if (!(L64(x9 + 0x38) >> 34))
                        continue;
                    for (uint64_t l = 0; l < (L64(x9 + 0x38) >> 34); l = (uint32_t)(l + 1)) {
                        const uint64_t lay = L64(x9 + 0x40);
                        const uint64_t a = L64(lay + l * 168 + 0x60);
                        if (a) {
                            const uint64_t x12 = a & ~L64(x8 + 0xb0);
                            const uint32_t pass = (x12 & ~1ull) == 0 ? !(a & 1) : (uint32_t)a;
                            if (!(pass & 1))
                                continue;
                        }
                        if (!L8(L64(X + 8) + l))
                            continue;
                        const uint32_t lz = L32(lay + l * 168 + 0x88);
                        if (!lz)
                            continue;
                        if (zz) {
                            uint32_t dz = lz - zz;
                            if ((int32_t)dz < 0)
                                dz = -dz;
                            if (dz > 1)
                                continue;
                        }
                        rt_st32(GP(F + 0x118), lz);
                        rt_st32(GP(F + 0x11c), (uint32_t)l);
                        if (dry) {
                            phys_rec(1, (uint32_t)X | 0x80000000u, (uint32_t)sd, lz, (uint32_t)l);
                            continue;
                        }
                        c->x[0] = X;
                        c->x[1] = F + 0x100;
                        c->x[2] = F + 0xb0;
                        c->x[3] = ty;
                        c->x[4] = sd;
                        c->x[5] = F + 0xa0;
                        c->x[6] = F + 0x90;
                        f_7bd5bc_orig(c);
                        x8 = L64(X);
                        x9 = L64(x8 + 0x10);
                    }
                }
            }
        }
    }
    c->sp = saved_sp;
    c->x[29] = saved_fp;
done:
    c->x[0] = L64(L + 0x1480) != 0;
}

static void phys_verify(Ctx *c, void (*nat)(Ctx *, int), void (*orig)(Ctx *), const char *name, int kreg)
{
    static uint32_t calls[2], bad[2];
    const int w = kreg == 6;
    Ctx save = *c;
    phys_rec_n[1] = 0;
    nat(c, 1);
    *c = save;
    phys_rec_n[0] = 0;
    orig(c);
    calls[w]++;
    const int n0 = phys_rec_n[0], n1 = phys_rec_n[1];
    if ((n0 != n1 || memcmp(phys_rec_buf[0], phys_rec_buf[1], sizeof(PhysRec) * (n0 < 512 ? n0 : 512))) && bad[w]++ < 20) {
        rt_log("[verify] %s differs (%u of %u): game %d calls, mine %d (kind %u n %u)\n", name, bad[w], calls[w], n0, n1,
               (unsigned)save.x[kreg], (unsigned)save.x[1]);
        for (int i = 0; i < 6 && (i < n0 || i < n1); i++) {
            const PhysRec *a = &phys_rec_buf[0][i], *b = &phys_rec_buf[1][i];
            rt_log("  %d: %08x %08x %08x %08x | %08x %08x %08x %08x\n", i, i < n0 ? a->a : 0, i < n0 ? a->b : 0,
                   i < n0 ? a->c : 0, i < n0 ? a->d : 0, i < n1 ? b->a : 0, i < n1 ? b->b : 0, i < n1 ? b->c : 0,
                   i < n1 ? b->d : 0);
        }
    }
    if ((calls[w] & 0x7fff) == 0)
        rt_log("[verify] %s: %u calls checked, %u differ\n", name, calls[w], bad[w]);
}

void nat__ZN14PhysicsManager5SweepEjP17PhysicsQueryShapeiRK9ycLineSegP20PhysicsQueryListenerj(Ctx *c)
{
    if (phys_rec_on < 0)
        phys_rec_on = getenv("MINA_VERIFY_PHYS") != NULL;
    if (phys_rec_on > 0)
        phys_verify(c, phys_sweep, f_974fe0_orig, "Sweep", 6);
    else
        phys_sweep(c, 0);
}

/* ---- ycBezier::GetLengthAppox ------------------------------------------------------------------
 * The length of a cubic Bezier (p0..p3 at +0, +0xc, +0x18, +0x24) from 0 to t:
 * 5-point Gauss-Legendre over |B'(u)|, B'(u) = c + u (b + u a) per axis.
 * The game's operations and groupings (its NEON pairs as scalars); the
 * recompiled vector code is slow on the Vita. MINA_VERIFY_XFORM: checked. */
void f_d1db4_orig(Ctx *c);
static float bez_len(const float *B, float t)
{
    const float p0x = B[0], p0y = B[1], p0z = B[2], p1x = B[3], p1y = B[4], p1z = B[5];
    const float p2x = B[6], p2y = B[7], p2z = B[8], p3x = B[9], p3y = B[10], p3z = B[11];
    const float h = t * 0.5f;
    /* y */
    float s6 = (p3y - p2y) * 3.0f;
    const float s4 = (p3y - p0y) * 6.0f;
    const float s3 = s6 * 3.0f;
    s6 = s6 + s6;
    const float cy = (p1y - p0y) * 3.0f;
    const float ay = (3.0f * cy - s4) + s3;
    const float by = (s4 + -4.0f * cy) - s6;
    /* x, z */
    const float cx = (p1x - p0x) * 3.0f, cz = (p1z - p0z) * 3.0f;
    const float ex = (p3x - p2x) * 3.0f, ez = (p3z - p2z) * 3.0f;
    const float fx = (p3x - p0x) * 6.0f, fz = (p3z - p0z) * 6.0f;
    const float ax = (cx * 3.0f - fx) + ex * 3.0f, az = (cz * 3.0f - fz) + ez * 3.0f;
    const float bx = (fx + cx * -4.0f) - (ex + ex), bz = (fz + cz * -4.0f) - (ez + ez);
#define BEZ_D(a, b, cc, u) (((b) + (a) * (u)) * (u) + (cc))
    float sum;
    {
        const float y = BEZ_D(ay, by, cy, h), x = BEZ_D(ax, bx, cx, h), z = BEZ_D(az, bz, cz, h);
        sum = sqrtf(z * z + (x * x + y * y)) * hexf(0x3f11a2b4u) + 0.0f;
    }
    static const uint32_t nodes[2][2] = { { 0x3eec4dc0u, 0x3fc4ec90u }, { 0x3dc024c8u, 0x3ff3fdb4u } };
    static const uint32_t weights[2] = { 0x3ef50ed1u, 0x3e729cf6u };
    for (int k = 0; k < 2; k++) {
        float r[2];
        for (int i = 0; i < 2; i++) {
            const float u = hexf(nodes[k][i]) * h;
            const float y = BEZ_D(ay, by, cy, u), x = BEZ_D(ax, bx, cx, u), z = BEZ_D(az, bz, cz, u);
            r[i] = sqrtf(z * z + (x * x + y * y)) * hexf(weights[k]);
        }
        sum = sum + r[0];
        sum = sum + r[1];
    }
#undef BEZ_D
    return sum * h;
}
void nat__ZNK8ycBezier14GetLengthAppoxEf(Ctx *c)
{
    GM_BEGIN;
    float B[12];
    memcpy(B, GP(c->x[0]), 48);
    const float t = c->v[0].f[0];
    const float r = bez_len(B, t);
    if (verify_xform()) {
        static uint32_t checked, bad;
        Ctx save = *c;
        f_d1db4_orig(c);
        const float g = c->v[0].f[0];
        checked++;
        if (memcmp(&g, &r, 4) && bad++ < 10)
            rt_log("[verify] Bezier::GetLengthAppox differs (%u of %u): %a / %a (t %a)\n", bad, checked, r, g, t);
        if ((checked & 0xffff) == 0)
            rt_log("[verify] Bezier::GetLengthAppox: %u calls checked, %u differ\n", checked, bad);
        *c = save;
    }
    memset(&c->v[0], 0, 16);
    c->v[0].f[0] = r;
}

/* ---- CombatManager::ProcessAttackList ----------------------------------------------------------
 * Attack shapes (pool slots of 0x28: +0 half x/y, +8 center x/y, +0x10 the
 * CombatShape, +0x18 owner, +0x20 flags: 1 attack, 2 defense) against the
 * defense shapes of bucket idx (this+0x80+32 idx: pool list, count); pools of
 * 64 slots with a free bitmap, this+0xe4 words. Every overlapping pair: each
 * attack shape i of the attacker against each defense shape j of the defender
 * (AttackShape::CheckAttackOverlap), a hit assigning the damage. The game's
 * order, reads and calls; its stack frame (0x1b0) for the shape containers
 * passed by value and the stack arguments. The scans were the cost: the
 * recompiled code reloads its spills for every slot.
 * MINA_VERIFY_COMBAT=1: the game's code runs, and the CheckAttackOverlap calls
 * this one would make (before the first hit) are compared with the game's. */
void f_986898_orig(Ctx *c);     /* CombatManager::ProcessAttackList */
void f_987548(Ctx *c);          /* CombatShape::GetShape */
void f_98944c(Ctx *c);          /* CombatShape::GetModifiedShape */
void f_5721dc_orig(Ctx *c);     /* AttackShape::CheckAttackOverlap */
void f_98375c(Ctx *c);          /* CombatCore::AssignDamage */
static int combat_rec_on = -1, combat_rec_hit;
void nat__ZN11AttackShape18CheckAttackOverlapE16ycShapeContainerjjS0_jP12DefenseShapejb(Ctx *c)
{
    if (combat_rec_on > 0 && !combat_rec_hit) {
        GM_BEGIN;
        phys_rec(0, (uint32_t)c->x[0], (uint32_t)c->x[4], (uint32_t)L64(c->sp), L32(c->sp + 8));
    }
    f_5721dc_orig(c);
    if (combat_rec_on > 0 && (uint32_t)c->x[0])
        combat_rec_hit = 1;
}

static uint64_t combat_root(uint8_t *gm_, uint64_t r, uint32_t off, uint32_t bit)
{
    for (;;) {
        const uint64_t x9 = r;
        if (!(L8(r + off) & bit))
            return x9;
        r = L64(x9 + 0x40);
        if (!r)
            return x9;
    }
}
static inline uint32_t combat_count(uint8_t *gm_, uint64_t s)
{
    return L32(s + (L8(s + 0xbe) ? 0x54 : 0x50));
}

static void combat_tail(Ctx *c, uint8_t *gm_, uint64_t F, uint64_t A, uint64_t D, int dry)
{
    const uint64_t fp = F + 0x1a0;      /* x29 */
    const uint64_t x20 = L64(A + 0x10);
    if (!combat_count(gm_, x20))
        return;
    const uint64_t x22 = L64(D + 0x10);
    for (uint64_t i = 0; i < combat_count(gm_, x20); i++) {
        c->x[0] = x20;
        c->x[1] = i;
        f_987548(c);
        uint32_t w0 = (uint32_t)c->x[0];
        uint64_t x3;
        if (w0 <= 8 && ((1u << (w0 & 31)) & 0x112)) {
            rt_st32(GP(fp - 0xa0), w0);
            x3 = fp - 0x90;
            rt_st64(GP(fp - 0x98), x3);
        } else {
            x3 = L64(fp - 0x98);
        }
        c->x[0] = x20;
        c->x[1] = i;
        c->x[2] = L64(fp - 0xa0);
        c->x[3] = x3;
        f_98944c(c);
        const uint64_t sc0 = c->x[0], x25 = c->x[1];
        rt_st64(GP(F + 0xd0), sc0);
        if (!x25)
            continue;
        uint64_t p8 = x20 + 0x198;
        if (L64(x20 + 0x180) > i)
            p8 = L64(x20 + 0x178) + (i << 5) + 0x10;
        const uint32_t w9 = L32(p8);
        const uint32_t fl = (uint32_t)((L64(x20 + 0x1f8) >> 34) & 0x10) | w9;
        rt_st32(GP(F + 0xb4), w9);
        rt_st32(GP(F + 0xcc), fl);
        const uint64_t a38 = L64(x20 + 0x38);
        const uint32_t teamA = L32(combat_root(gm_, a38, 0x210, 1) + 0x20c);
        const uint64_t d38 = L64(x22 + 0x38);
        if (!(L32(combat_root(gm_, d38, 0x210, 2) + 0x208) & teamA))
            continue;
        if (!L8(combat_root(gm_, d38, 0x210, 8) + 0x257))
            continue;
        if (a38 && d38) {
            if (a38 == d38)
                continue;
            if (L64(a38 + 0x40) == d38 || L64(d38 + 0x40) == a38) {
                if (!(L8(d38 + 0x1d2) & 1) && !(L8(a38 + 0x1d2) & 1))
                    continue;
            }
        }
        if (!combat_count(gm_, x22))
            continue;
        for (uint64_t j = 0; j < combat_count(gm_, x22); j++) {
            c->x[0] = x20;              /* (the attacker's shape j: the game's code) */
            c->x[1] = j;
            f_987548(c);
            w0 = (uint32_t)c->x[0];
            uint64_t y3;
            if (w0 <= 8 && ((1u << (w0 & 31)) & 0x112)) {
                rt_st32(GP(fp - 0xc8), w0);
                y3 = fp - 0xb8;
                rt_st64(GP(fp - 0xc0), y3);
            } else {
                y3 = L64(fp - 0xc0);
            }
            c->x[0] = x22;
            c->x[1] = j;
            c->x[2] = L64(fp - 0xc8);
            c->x[3] = y3;
            f_98944c(c);
            const uint64_t x5 = c->x[0], x6 = c->x[1];
            uint64_t q8 = x22 + 0x198;
            if (L64(x22 + 0x180) > j)
                q8 = L64(x22 + 0x178) + (j << 5) + 0x10;
            const uint32_t w7 = L32(q8);
            rt_st32(GP(F + 8), (uint32_t)j);
            rt_st64(GP(F), x22);
            if (dry) {
                phys_rec(1, (uint32_t)x20, (uint32_t)i, (uint32_t)x22, (uint32_t)j);
                continue;
            }
            c->x[0] = x20;
            c->x[1] = L64(F + 0xd0);
            c->x[2] = x25;
            c->x[3] = L32(F + 0xcc);
            c->x[4] = i;
            c->x[5] = x5;
            c->x[6] = x6;
            c->x[7] = w7;
            c->sp = F;                      /* (its stack arguments are at sp: a callee above left it lower) */
            f_5721dc_orig(c);
            if (!(uint32_t)c->x[0])
                continue;
            /* a hit */
            const uint32_t saved = L32(x20 + 0x1ec);
            if (L32(F + 0xb4) & (1u << 11))
                rt_st32(GP(x20 + 0x1ec), 1);
            const uint64_t o = L64(x22 + 0x38);
            rt_st64(GP(x20 + 0x1c8), o);
            rt_st64(GP(x20 + 0x238), x22);
            rt_st32(GP(x20 + 0x240), (uint32_t)j);
            rt_st32(GP(x20 + 0x244), (uint32_t)i);
            const uint64_t dd = x20 + 0x1c0;
            if (!(L8(x20 + 0x1f8) & 8)) {
                c->x[0] = o;
                c->x[1] = dd;
                c->x[2] = j;
                c->sp = F;
                f_98375c(c);
            } else {
                rt_st32(GP(o + 0x204), (uint32_t)j);
                const uint64_t r = combat_root(gm_, o, 0x211, 2);
                const uint32_t n = L32(r + 0x1c8);
                int skip = 0;
                if (n) {
                    const uint32_t id = L32(x20 + 0x214);
                    if (L32(r + 0x10c) == id) {
                        skip = 1;
                    } else {
                        uint64_t k = 0, p = r + 0x114;
                        for (;;) {
                            if ((uint64_t)n - 1 == k)
                                break;
                            const uint32_t v = L32(p);
                            p += 8;
                            k++;
                            if (v == id) {
                                skip = k < n;
                                break;
                            }
                        }
                    }
                }
                if (!skip) {
                    c->x[0] = o;
                    c->x[1] = dd;
                    rt_call(c, L64(L64(o) + 0x68));
                    const uint64_t res = c->x[0];
                    const uint64_t x24 = res ? L64(x20 + 0x38) : 0;
                    if (res && x24) {
                        rt_st64(GP(x20 + 0x250), res);
                        const int32_t k = (int32_t)L32(x20 + 0x248);
                        if (k >= 0) {
                            const uint64_t obj = L64(GUEST_IMAGE_BASE + 0x5051d28u + (uint64_t)(int64_t)k * 8 + 8);
                            c->x[0] = obj;
                            c->x[1] = dd;
                            rt_call(c, L64(L64(obj) + 0x1a8));
                        }
                        const uint64_t x0b = L64(x24 + 0x38);
                        if (x0b) {
                            c->x[0] = x0b;
                            c->x[1] = dd;
                            rt_call(c, L64(L64(x0b) + 0x50));
                        }
                        const uint64_t f250 = L64(x20 + 0x250);
                        if (!(f250 & 4)) {
                            rt_st64(GP(x24 + 0x1d8), f250);
                            rt_st32(GP(x24 + 0x200), (uint32_t)j);
                        }
                    }
                }
            }
            rt_st64(GP(x20 + 0x1c8), 0);
            rt_st64(GP(x20 + 0x238), 0);
            rt_st64(GP(x20 + 0x240), 0);
            rt_st32(GP(x20 + 0x1ec), saved);
        }
    }
}

static void process_attack_list(Ctx *c, int dry)
{
    GM_BEGIN;
    const uint64_t T = c->x[0];
    const uint32_t idx = (uint32_t)c->x[1];
    const uint64_t cnt = L64(T + 0x88);
    if (!cnt || !L32(T + 0xe4))
        return;
    const uint64_t saved_sp = c->sp, saved_fp = c->x[29];
    const uint64_t F = saved_sp - 0x1b0;
    c->sp = F;
    c->x[29] = F + 0x1a0;
    const uint64_t arr = L64(T + 0x80);
    const uint64_t end = (uint32_t)(arr + cnt * 8);
    const uint64_t B = T + (uint64_t)idx * 32 + 0x80;
    uint32_t w9 = 1;
    for (uint64_t cur = (uint32_t)arr; cur != end; cur = (uint32_t)(cur + 8)) {
        if (!w9)
            continue;
        const uint64_t pool = L64(cur);
        uint64_t abase = L64(pool + 0x20);
        uint32_t w = 0, nw;
        for (;;) {
            const uint64_t bits = L64(L64(pool) + (uint64_t)w * 8);
            if (bits != ~0ull) {
                for (int hf = 0; hf < 2; hf++) {
                    uint32_t live = ~(uint32_t)(bits >> (hf * 32));
                    const uint32_t ab = (uint32_t)(abase + hf * 32 * 0x28);
                    while (live) {
                        const uint32_t j = __builtin_ctz(live);
                        live &= live - 1;
                        const uint64_t A = ab + j * 0x28;
                        if (!L64(A + 0x18) || !(L8(A + 0x20) & 1))
                            continue;
                        const uint64_t bcnt = L64(B + 8);
                        if (!bcnt || !L32(T + 0xe4))
                            continue;
                        const float e8 = hexf(0x3f4ccccdu);
                        const float hx = ldfp(A) + e8, hy = ldfp(A + 4) + e8;
                        const float cx = ldfp(A + 8), cy = ldfp(A + 0xc);
                        const float s8 = cx - hx, s9 = hx + cx, s10 = hy + cy, s11 = cy - hy;
                        const uint64_t barr = L64(B);
                        const uint64_t bend = (uint32_t)(barr + bcnt * 8);
                        uint32_t v9 = L32(T + 0xe4);
                        for (uint64_t pp = (uint32_t)barr; pp != bend; pp = (uint32_t)(pp + 8)) {
                            if (!v9)
                                continue;
                            const uint64_t dpool = L64(pp);
                            uint64_t dbase0 = L64(dpool + 0x20);
                            uint32_t w2 = 0, nw2;
                            for (;;) {
                                const uint64_t db = L64(L64(dpool) + (uint64_t)w2 * 8);
                                if (db != ~0ull) {
                                    for (int h2 = 0; h2 < 2; h2++) {
                                        uint32_t lv = ~(uint32_t)(db >> (h2 * 32));
                                        const uint32_t dbase = (uint32_t)(dbase0 + h2 * 32 * 0x28);
                                        while (lv) {
                                            const uint32_t k = __builtin_ctz(lv);
                                            lv &= lv - 1;
                                            const uint64_t D = dbase + k * 0x28;
                                            const uint8_t *dp = gm_ + (uint32_t)D;
                                            if (!(dp[0x20] & 2) || !(rt_ld32(dp + 0x18) | rt_ld32(dp + 0x1c)))
                                                continue;
                                            {
                                                const float t = ldfp(D + 8), h = ldfp(D);
                                                const float lo = t - h, hi = t + h;
                                                if (s8 > hi || lo > s9)
                                                    continue;
                                            }
                                            {
                                                const float h = ldfp(D + 4), t = ldfp(D + 0xc);
                                                const float lo = t - h, hi = h + t;
                                                if (s11 > hi || lo > s10)
                                                    continue;
                                            }
                                            combat_tail(c, gm_, F, A, D, dry);
                                        }
                                    }
                                }
                                dbase0 = (uint32_t)(dbase0 + 0xa00);
                                w2++;
                                nw2 = L32(T + 0xe4);
                                if (w2 == nw2)
                                    break;
                            }
                            v9 = nw2;
                        }
                    }
                }
            }
            abase = (uint32_t)(abase + 0xa00);
            w++;
            nw = L32(T + 0xe4);
            if (w == nw)
                break;
        }
        w9 = nw;
    }
    c->sp = saved_sp;
    c->x[29] = saved_fp;
}

void nat__ZN13CombatManager17ProcessAttackListEi(Ctx *c)
{
    if (combat_rec_on < 0)
        combat_rec_on = getenv("MINA_VERIFY_COMBAT") != NULL;
    if (combat_rec_on > 0) {
        static uint32_t calls, bad;
        Ctx save = *c;
        phys_rec_n[1] = 0;
        process_attack_list(c, 1);
        *c = save;
        phys_rec_n[0] = 0;
        combat_rec_hit = 0;
        f_986898_orig(c);
        calls++;
        const int n0 = phys_rec_n[0], n1 = phys_rec_n[1];
        /* up to the game's first hit (a hit changes what follows) */
        const int cmp = combat_rec_hit ? n0 : (n0 > n1 ? n0 : n1);
        const int same = (combat_rec_hit ? n1 >= n0 : n0 == n1) &&
                         !memcmp(phys_rec_buf[0], phys_rec_buf[1], sizeof(PhysRec) * (cmp < 512 ? cmp : 512));
        if (!same && bad++ < 20)
            rt_log("[verify] ProcessAttackList differs (%u of %u): game %d checks, mine %d%s\n", bad, calls, n0, n1,
                   combat_rec_hit ? " (game hit)" : "");
        if ((calls & 0x3ff) == 0)
            rt_log("[verify] ProcessAttackList: %u calls checked, %u differ\n", calls, bad);
        return;
    }
    process_attack_list(c, 0);
}

/* ---- ycSpline::GetPos --------------------------------------------------------------------------
 * The point at t (0..1 of the spline's length): the segment from the length
 * table (data+0x28 pairs (length, start), data+0x38 index by length/0x40,
 * data+0x44 the total), then the parameter in it: linear (this+0x78 == 1, or
 * control points on the line at 1/3 and 2/3 within 1e-4) or a bisection on
 * ycBezier::GetLengthAppox to 1e-4; the cubic (segments of 3 points, 0x24
 * bytes, at data+8). The game's operations; the bisection's lengths from
 * bez_len directly (it was ~600 calls through the context a fixed update).
 * MINA_VERIFY_XFORM: checked against the game's code. */
void f_1bdc18_orig(Ctx *c);
static inline uint32_t fcvtzu32(float x)
{
    if (!(x >= 0.0f))
        return 0;
    if (x >= 4294967296.0f)
        return 0xffffffffu;
    return (uint32_t)x;
}
static inline float fabs_g(float v) { return v > 0.0f ? v : -v; }   /* fneg + fcsel gt */
static void spline_pos(uint8_t *gm_, uint64_t S, float t, float out[4])
{
    uint64_t data = S + 0x10;
    const uint64_t p68 = L64(S + 0x68);
    if (p68) {
        const uint64_t d = L64(p68 + 8);
        if (d)
            data = d;
    }
    const uint32_t mode = L8(S + 0x78);
    float u = 1.0f;
    int32_t seg;
    const float eps = hexf(0x38d1b717u), third = hexf(0x3eaaaaabu), two3 = hexf(0x3f2aaaabu);
    float p0x = 0, p0y = 0, p0z = 0, p3x = 0, p3y = 0, p3z = 0;
    uint64_t B = 0;
    int have_pts = 0;
    if (t >= 1.0f) {
        const uint64_t n = L64(data) >> 34;
        seg = n ? (int32_t)((uint32_t)(n - 1) / 3u) - 1 : -1;
        goto eval;
    }
    {
        const uint64_t tab = L64(data + 0x28);
        uint32_t w9 = (uint32_t)(L64(data + 0x30) >> 34) - 1;
        const float unit = ldfp(data + 0x40), total = ldfp(data + 0x44);
        const float target = total * t;
        const uint32_t w10 = fcvtzu32(target / unit);
        const int lo_ = w9 < w10;
        w9 = lo_ ? w9 : w10;
        const float s2 = lo_ ? total : target;
        w9 = L32(L64(data + 0x38) + (uint64_t)w9 * 4);
        float s1, s0;
        for (;;) {
            const uint64_t e = tab + (uint64_t)w9 * 8;
            s1 = ldfp(e);
            s0 = s2 - ldfp(e + 4);
            w9++;
            if (!(s0 > s1))
                break;
        }
        seg = (int32_t)(w9 - 1);
        if (mode != 1) {
            B = L64(data + 8) + (int64_t)seg * 36;
            p3z = ldfp(B + 0x2c);
            p3x = ldfp(B + 0x24);
            p3y = ldfp(B + 0x28);
            p0x = ldfp(B);
            p0y = ldfp(B + 4);
            p0z = ldfp(B + 8);
            have_pts = 1;
            const float dx = p3x - p0x, dy = p3y - p0y;
            int straight = 0;
            if (fabs_g((p0x + dx * third) - ldfp(B + 0xc)) < eps &&
                fabs_g((p0y + dy * third) - ldfp(B + 0x10)) < eps) {
                const float dz = p3z - p0z;
                if (fabs_g((p0z + dz * third) - ldfp(B + 0x14)) < eps &&
                    fabs_g((p0x + dx * two3) - ldfp(B + 0x18)) < eps &&
                    fabs_g((p0y + dy * two3) - ldfp(B + 0x1c)) < eps &&
                    fabs_g((p0z + dz * two3) - ldfp(B + 0x20)) < eps)
                    straight = 1;
            }
            if (!straight) {
                const float s13 = s0 + 0.0f;
                if (!(s13 > 0.0f) && !(s13 != s13)) {     /* b.ls: <= 0 (not NaN) */
                    u = 0.0f;
                    goto cubic;
                }
                float Bf[12];
                memcpy(Bf, GP(B), 48);
                float s8 = bez_len(Bf, 1.0f);
                if (!(s8 > s13) && !(s8 != s8 || s13 != s13)) {
                    u = 1.0f;
                    goto cubic;
                }
                float lo = 0.0f, best = 1.0f, bestlen = hexf(0x7f7fffffu), hi = 1.0f;
                int w8 = 1;
                for (;;) {
                    if (w8 && s8 < bestlen) {
                        const float d = fabs_g(s8 - s13);
                        best = hi;
                        bestlen = s8;
                        if (d < eps) {
                            u = hi;
                            goto eval_pts;
                        }
                    }
                    if (lo >= hi + hexf(0xb8d1b717u)) {
                        u = best;
                        goto eval_pts;
                    }
                    const float mid = lo + (hi - lo) * 0.5f;
                    const float len = bez_len(Bf, mid);
                    const int less = len < s13;          /* mi */
                    w8 = !less;                           /* pl (>= or NaN) */
                    s8 = less ? s8 : len;
                    hi = less ? hi : mid;
                    lo = less ? mid : lo;
                }
            }
        }
        /* linear */
        if (s1 == 0.0f)
            u = 0.0f;
        else
            u = s0 / s1;
    }
eval:
eval_pts:
    B = L64(data + 8) + (int64_t)seg * 36;
    if (mode == 1) {
        const float z0 = ldfp(B + 8);
        const float s2 = u * (ldfp(B + 0x2c) - z0);
        const float x0 = ldfp(B), y0 = ldfp(B + 4);
        out[0] = x0 + (ldfp(B + 0x24) - x0) * u;
        out[1] = y0 + (ldfp(B + 0x28) - y0) * u;
        out[2] = z0 + s2;
        return;
    }
    p0z = ldfp(B + 8);
    p3z = ldfp(B + 0x2c);
    p0x = ldfp(B);
    p0y = ldfp(B + 4);
    p3x = ldfp(B + 0x24);
    p3y = ldfp(B + 0x28);
    have_pts = 1;
cubic:
    (void)have_pts;
    {
        const float v = 1.0f - u;
        const float a = v * (v * v);
        const float s2a = v * 3.0f;
        const float b = (v * s2a) * u;
        const float cc = (s2a * u) * u;
        const float d = (u * u) * u;
        float z = a * p0z;
        z = z + b * ldfp(B + 0x14);
        z = z + cc * ldfp(B + 0x20);
        const float z3 = d * p3z;
        float x = p0x * a, y = p0y * a;
        x = x + ldfp(B + 0xc) * b;
        y = y + ldfp(B + 0x10) * b;
        x = x + ldfp(B + 0x18) * cc;
        y = y + ldfp(B + 0x1c) * cc;
        x = x + p3x * d;
        y = y + p3y * d;
        out[0] = x;
        out[1] = y;
        out[2] = z + z3;
    }
}
void nat__ZNK8ycSpline6GetPosEfb(Ctx *c)
{
    GM_BEGIN;
    float o[4] = { 0, 0, 0, 0 };
    const float t = c->v[0].f[0];
    spline_pos(gm_, c->x[0], t, o);
    if (verify_xform()) {
        static uint32_t checked, bad;
        Ctx save = *c;
        f_1bdc18_orig(c);
        checked++;
        if ((memcmp(&c->v[0].f[0], &o[0], 4) || memcmp(&c->v[1].f[0], &o[1], 4) || memcmp(&c->v[2].f[0], &o[2], 4)) &&
            bad++ < 10)
            rt_log("[verify] Spline::GetPos differs (%u of %u): %a %a %a / %a %a %a (t %a)\n", bad, checked, o[0], o[1],
                   o[2], c->v[0].f[0], c->v[1].f[0], c->v[2].f[0], t);
        if ((checked & 0xfff) == 0)
            rt_log("[verify] Spline::GetPos: %u calls checked, %u differ\n", checked, bad);
        *c = save;
    }
    memset(&c->v[0], 0, 48);
    c->v[0].f[0] = o[0];
    c->v[0].f[1] = o[1];
    c->v[1].f[0] = o[1];
    c->v[2].f[0] = o[2];
}

/* ---- TileLevelCollision::TileLayerOverlap ------------------------------------------------------
 * The tiles of layer ci+0x1c under a shape's box: the box (by shape type, as
 * in PhysicsManager::Overlap; types without a case keep the registers' values)
 * in the layer's tiles (layer offset L->0x40[l]+0x130, scroll layers[l]+0x68,
 * the world scale at 0x100e14e28, y down), clamped to the layer (+0x80 w,
 * +0x84 h); each non-empty tile (direct, +0x128 of the transform, or by
 * chunk when the layer has them) fills ci (+8 center, +0x20 kind, +0x22/+0x23
 * flips, +0x24 solid) and goes to the callback (cp->0 vtable +0x20, with
 * whether the tile is inside the range's border), which can stop the scan.
 * The game's operations. Verified through Overlap's MINA_VERIFY_PHYS (calls
 * of the callback) and MINA_VERIFY_XFORM (ci compared). */
static inline int32_t fcvtms32(float x)
{
    if (x != x)
        return 0;
    const float f = floorf(x);
    if (f >= 2147483648.0f)
        return 0x7fffffff;
    if (f < -2147483648.0f)
        return (int32_t)0x80000000;
    return (int32_t)f;
}
static inline uint32_t udiv0(uint32_t a, uint32_t b) { return b ? a / b : 0; }   /* arm64 udiv: 0 for /0 */
static void tile_layer_overlap(Ctx *c, int dry)
{
    GM_BEGIN;
    const uint64_t X = c->x[0], CI = c->x[1], CP = c->x[2], SH = c->x[4], POS = c->x[5];
    const uint32_t ty = (uint32_t)c->x[3];
    const uint64_t KA = GUEST_IMAGE_BASE + 0xe14e28u;
    float K = ldfp(KA);
    uint64_t M = L64(L64(X) + 0x10);
    const float tw = (float)L32(M + 8), th = (float)L32(M + 0xc);
    float v2[2] = { (tw * 0.5f) * K, (th * 0.5f) * K };
    stfp(CI + 0x10, v2[0]);
    stfp(CI + 0x14, v2[1]);
    float v3[2] = { c->v[3].f[0], c->v[3].f[1] };
    const uint32_t t1 = ty - 1;
    if (t1 <= 8) {
        if (t1 == 0 || t1 == 8) {
            for (int k = 0; k < 2; k++) {
                float a = ldfp(SH + 4 * k), mn, x2, q;
                if (t1 == 0) {
                    q = a + ldfp(SH + 0xc + 4 * k);
                    mn = gt_sel(q, a, a, q);
                    x2 = a + 0.0f;
                } else {
                    const float b = a;                       /* d3 = [shp] */
                    const float d2 = ldfp(SH + 0x18 + 4 * k);
                    const float lo = gt_sel(d2, b, b, d2);   /* v4 = d2 > b ? b : d2 */
                    x2 = gt_sel(b, d2, b, d2);               /* v2 = b > d2 ? b : d2 */
                    q = ldfp(SH + 0xc + 4 * k);
                    mn = gt_sel(lo, q, q, lo);
                }
                const float hi = gt_sel(q, x2, q, x2);
                v3[k] = (hi - mn) * 0.5f;
                v2[k] = mn + v3[k];
            }
        } else if (t1 == 3) {
            v2[0] = ldfp(SH); v2[1] = ldfp(SH + 4);
            v3[0] = ldfp(SH + 0xc); v3[1] = ldfp(SH + 0x10);
        } else if (t1 == 7) {
            v2[0] = ldfp(SH); v2[1] = ldfp(SH + 4);
            v3[0] = v3[1] = ldfp(SH + 0xc);
        }
    }
    const uint32_t l = L32(CI + 0x1c);
    const uint64_t L0 = L64(X);
    const float px = ldfp(POS), py = ldfp(POS + 4);
    const float lox = (v2[0] - v3[0]) + px, loy = (v2[1] - v3[1]) + py;
    const uint64_t T = L64(L0 + 0x40) + (uint64_t)l * 0x1d8;
    const uint64_t LY = L64(L64(L0 + 0x10) + 0x40) + (uint64_t)l * 0xa8;
    const float ox = ldfp(T + 0x130), oy = ldfp(T + 0x134), sx = ldfp(LY + 0x68), sy = ldfp(LY + 0x6c);
    const float nK = -K;
    const float a4 = (lox - ox) / K - sx, a6 = (loy - oy) / nK - sy;
    const float hix = (v2[0] + v3[0]) + px, hiy = (v2[1] + v3[1]) + py;
    const float a1 = (hix - ox) / K - sx, a2 = (hiy - oy) / nK - sy;
    const int32_t w9 = fcvtms32(a4 / tw), w10 = fcvtms32(a2 / th), w11 = fcvtms32(a1 / tw), w12 = fcvtms32(a6 / th);
    const int32_t lw0 = (int32_t)L32(LY + 0x80), lh0 = (int32_t)L32(LY + 0x84);
    const int32_t x0 = w9 > 0 ? w9 : 0, y0 = w10 > 0 ? w10 : 0;
    const int32_t xm = lw0 > w11 ? w11 : lw0 - 1, ym = lh0 > w12 ? w12 : lh0 - 1;
    if (y0 > ym || x0 > xm)
        return;
    const uint64_t saved_sp = c->sp;
    for (uint32_t y = (uint32_t)y0;; y++) {
        for (uint32_t x = (uint32_t)x0; x != (uint32_t)xm + 1; x++) {
            const uint64_t L1 = L64(X);
            const uint32_t li = L32(CI + 0x1c);
            const uint64_t M1 = L64(L1 + 0x10);
            const uint64_t LY1 = L64(M1 + 0x40) + (uint64_t)li * 0xa8;
            const uint32_t lw = L32(LY1 + 0x80), lh = L32(LY1 + 0x84);
            if (!(lw > x && lh > y))
                continue;
            const uint64_t T1 = L64(L1 + 0x40) + (uint64_t)li * 0x1d8;
            uint64_t tp;
            if (!(L64(LY1 + 0x50) >> 34)) {
                tp = L64(T1 + 0x128) + (uint64_t)(uint32_t)(y * lw + x) * 2;
            } else {
                const uint32_t cw = L32(M1 + 0x10), ch = L32(M1 + 0x14);
                const uint32_t ci = udiv0(cw - 1 + lw, cw) * udiv0(y, ch) + udiv0(x, cw);
                const uint32_t chunk = L32(L64(LY1 + 0x58) + (uint64_t)ci * 4);
                if (chunk == 0xffffffffu)
                    continue;
                const uint32_t off = ((ch - 1) & y) * cw + ((cw - 1) & x);
                tp = L64(T1 + 0x128) + (uint64_t)(uint32_t)(off + chunk) * 2;
            }
            if (!tp)
                continue;
            const uint32_t tile = rt_ld16(GP(tp));
            if (!tile)
                continue;
            const float ftw = (float)L32(M1 + 8), fth = (float)L32(M1 + 0xc);
            float cx = 0.5f * ftw + (float)(int32_t)x * ftw;
            float cy = (float)(int32_t)y * fth + 0.5f * fth;
            stfp(CI + 8, cx);
            stfp(CI + 0xc, cy);
            const uint64_t LY2 = L64(L64(L64(X) + 0x10) + 0x40) + (uint64_t)li * 0xa8;
            const float K2 = ldfp(KA);
            cx = (cx + ldfp(LY2 + 0x68)) * K2;
            cy = (cy + ldfp(LY2 + 0x6c)) * -K2;
            const uint64_t T2 = L64(L64(X) + 0x40) + (uint64_t)li * 0x1d8;
            cx = cx + ldfp(T2 + 0x130);
            cy = cy + ldfp(T2 + 0x134);
            stfp(CI + 8, cx);
            stfp(CI + 0xc, cy);
            *(uint8_t *)GP(CI + 0x22) = (tile >> 14) & 1;
            *(uint8_t *)GP(CI + 0x23) = (uint8_t)(tile >> 15);
            const uint64_t def = L64(M1 + 0x80) + (uint64_t)(tile & 0x3fff) * 6;
            rt_st16(GP(CI + 0x20), rt_ld16(GP(def - 2)));
            *(uint8_t *)GP(CI + 0x24) = (int16_t)rt_ld16(GP(def - 6)) == -1;
            const int inner = x > (uint32_t)x0 && (int32_t)x < xm && y > (uint32_t)y0 && (int32_t)y < ym;
            if (dry) {
                phys_rec(1, L32(CI + 8), L32(CI + 0xc), L32(CI + 0x20) & 0xffffff, (uint32_t)inner | (uint32_t)L8(CI + 0x24) << 8);
                continue;
            }
            const uint64_t cb = L64(CP);
            c->x[0] = cb;
            c->x[1] = CI;
            c->x[2] = (uint32_t)inner;
            c->x[3] = POS;
            c->x[4] = ty;
            c->x[5] = SH;
            c->sp = saved_sp - 0xa0;
            rt_call(c, L64(L64(cb) + 0x20));
            c->sp = saved_sp;
            if ((uint32_t)c->x[0] & 1)
                return;
        }
        if ((int32_t)y == ym)
            return;
    }
}

/* ---- World::UpdateSpawnManager ----------------------------------------------------------------
 * Every spawn manager of the level's tile components: the camera's region
 * (SM+0xa4.., active area A/B, camera margins D), then each spawn record
 * (pool slots: records of 0x40 at P+0x36080, spawn points of 0xd8 at P+0x80,
 * 64 to a word of the free bitmap at P): by state (0 waiting, 2, 3) and kind,
 * distance tests against the active area and the camera, then IsEventReady
 * and Spawn, or the kill of what it spawned. ~2900 records a fixed update in
 * astralOrrery; the recompiled code spent ~190 instructions on each. The
 * game's operations, writes and calls in order (w1 of Spawn is what the last
 * call left, as in the game).
 * MINA_VERIFY_SPAWN=1: the game's code runs with its calls recorded (arguments
 * and results); the records go back to before it, this one runs on them with
 * the recorded results instead of the calls, and both outcomes are compared
 * (slots handed to a call aside: the call's own effects are not replayed);
 * the game's outcome stays. */
void f_b66874_orig(Ctx *c);
void f_ac3554_orig(Ctx *c); /* SpawnManager::SpawnPersistentEntities */
void f_a7692c_orig(Ctx *c); /* SaveSlot::UpdateMode */
void f_ac4b18_orig(Ctx *c); /* SpawnPoint::IsEventReady */
void f_ac4f3c_orig(Ctx *c); /* SpawnPoint::Spawn */
void f_9ae200_orig(Ctx *c); /* GameComponent::Kill */
void f_ac5a70_orig(Ctx *c); /* SpawnPoint::DisownParent */
void f_ac52b8_orig(Ctx *c); /* SpawnPoint::IsEntityPermanentKilled */
void f_615680_orig(Ctx *c); /* PropUtil::GetPropertyExists */
enum { SPC_READY = 1, SPC_SPAWN, SPC_KILL, SPC_DISOWN, SPC_PERMKILLED, SPC_PROP, SPC_PERSIST, SPC_MODE, SPC_VCALL };
typedef struct { uint32_t id; uint64_t a0, a1, r0, r1; } SpCall;
static SpCall sp_trace[2048];
static int sp_n, sp_pos, sp_mode, sp_bad, sp_unverifiable;   /* sp_mode: 1 recording (the game's code), 2 replaying */
static int sp_depth;        /* only the calls the function itself makes (not those inside them) */
#define SP_WRAP(name, orig, id)                                    \
    void name(Ctx *c)                                              \
    {                                                              \
        if (sp_mode != 1 || sp_depth) {                            \
            orig(c);                                               \
            return;                                                \
        }                                                          \
        const int k_ = sp_n < 2048 ? sp_n++ : -1;                  \
        if (k_ < 0)                                                \
            sp_unverifiable = 1;                                   \
        else                                                       \
            sp_trace[k_] = (SpCall){ id, c->x[0], c->x[1], 0, 0 }; \
        sp_depth++;                                                \
        orig(c);                                                   \
        sp_depth--;                                                \
        if (k_ >= 0) {                                             \
            sp_trace[k_].r0 = c->x[0];                             \
            sp_trace[k_].r1 = c->x[1];                             \
        }                                                          \
    }
SP_WRAP(nat__ZN10SpawnPoint12IsEventReadyEfPNS_8CoreDataEPS_P11WorldRegion, f_ac4b18_orig, SPC_READY)
SP_WRAP(nat__ZN10SpawnPoint5SpawnEb, f_ac4f3c_orig, SPC_SPAWN)
SP_WRAP(nat__ZN13GameComponent4KillEv, f_9ae200_orig, SPC_KILL)
SP_WRAP(nat__ZN10SpawnPoint12DisownParentEv, f_ac5a70_orig, SPC_DISOWN)
SP_WRAP(nat__ZN10SpawnPoint23IsEntityPermanentKilledEb, f_ac52b8_orig, SPC_PERMKILLED)
SP_WRAP(nat__ZN8PropUtil17GetPropertyExistsEPK11ycDataArrayI20ycTileLevel2PropertyEPKc, f_615680_orig, SPC_PROP)
SP_WRAP(nat__ZN12SpawnManager23SpawnPersistentEntitiesEv, f_ac3554_orig, SPC_PERSIST)
SP_WRAP(nat__ZN8SaveSlot10UpdateModeE20ycTileLevel2ModeFlagb, f_a7692c_orig, SPC_MODE)
static void sp_dbg_dump(uint64_t R);
/* a call of the native: the function, or the recorded result when replaying */
static void sp_call(Ctx *c, uint32_t id, void (*fn)(Ctx *))
{
    if (sp_mode != 2) {
        fn(c);
        return;
    }
    if (sp_unverifiable) {      /* past a call whose effects are not replayed: nothing more to compare */
        c->x[0] = 0;
        return;
    }
    if (sp_pos < sp_n && sp_trace[sp_pos].id == id && sp_trace[sp_pos].a0 == c->x[0] &&
        (id == SPC_SPAWN || id == SPC_KILL || id == SPC_DISOWN || id == SPC_PERSIST || sp_trace[sp_pos].a1 == c->x[1])) {
        c->x[0] = sp_trace[sp_pos].r0;
        c->x[1] = sp_trace[sp_pos].r1;
        sp_pos++;
        /* what Spawn, Kill, DisownParent and the persistent spawns change is not replayed */
        if (id == SPC_SPAWN || id == SPC_KILL || id == SPC_DISOWN || id == SPC_PERSIST)
            sp_unverifiable = 1;
        return;
    }
    if (!sp_bad++) {
        rt_log("[verify] UpdateSpawnManager: call %d (id %u x0 %#llx) where the game made %d (id %u x0 %#llx)\n", sp_pos,
               id, (unsigned long long)c->x[0], sp_pos, sp_pos < sp_n ? sp_trace[sp_pos].id : 0,
               (unsigned long long)(sp_pos < sp_n ? sp_trace[sp_pos].a0 : 0));
        for (int k = 0; k < sp_n && k < 8; k++)
            rt_log("  game %d: id %u x0 %#llx x1 %#llx -> %#llx\n", k, sp_trace[k].id, (unsigned long long)sp_trace[k].a0,
                   (unsigned long long)sp_trace[k].a1, (unsigned long long)sp_trace[k].r0);
        if (id == SPC_READY)
            sp_dbg_dump(c->x[0]);
    }
    c->x[0] = 0;
}
#define SPCALL(id, fn) sp_call(c, (id), (fn))

typedef struct {
    float Ax, Ay, Bx, By, Dx, Dy, Z, camx, camy, e0, e4;
    const float *lofs;      /* the game's array on its stack (an index past what was copied reads what is there) */
} SpRegion;
static inline float sp_abs(float v) { return v > 0.0f ? v : -v; }
static const void *sp_dbg_region;
static void sp_dbg_dump(uint64_t R)
{
    uint8_t *gm_ = g_mem;
    rt_log("  record: state %u kind %u mask %#llx li %u pos %a %a r1c %a r20 %a r24 %a r28 %a\n", L32(R), L8(R + 4),
           (unsigned long long)L64(R + 8), L8(R + 0x2c), ldfp(R + 0x10), ldfp(R + 0x14), ldfp(R + 0x1c), ldfp(R + 0x20),
           ldfp(R + 0x24), ldfp(R + 0x28));
    const SpRegion *g = sp_dbg_region;
    if (g)
        rt_log("  region: A %a %a B %a %a D %a %a cam %a %a e %a %a\n", g->Ax, g->Ay, g->Bx, g->By, g->Dx, g->Dy, g->camx,
               g->camy, g->e0, g->e4);
}
/* the record's position (+ its layer's offset) in the active area A/B */
static inline int sp_in_area(uint8_t *gm_, uint64_t R, const SpRegion *g)
{
    float x = ldfp(R + 0x10), y = ldfp(R + 0x14);
    const uint32_t li = L8(R + 0x2c);
    if (li != 0xff) {
        x = x + g->lofs[2 * li];
        y = y + g->lofs[2 * li + 1];
    }
    if (sp_abs((x + ldfp(R + 0x1c)) - g->Ax) > ldfp(R + 0x24) + g->Bx)
        return 0;
    if (sp_abs((y + ldfp(R + 0x20)) - g->Ay) > g->By + ldfp(R + 0x28))
        return 0;
    return 1;
}
/* bd0: A/B, then the camera with margins D */
static inline int sp_visible_bd0(uint8_t *gm_, uint64_t R, const SpRegion *g)
{
    const float x0 = ldfp(R + 0x10), y0 = ldfp(R + 0x14);
    const uint32_t li = L8(R + 0x2c);
    float x = x0, y = y0;
    if (li != 0xff) {
        x = x0 + g->lofs[2 * li];
        y = y0 + g->lofs[2 * li + 1];
    }
    const float r1c = ldfp(R + 0x1c), r24 = ldfp(R + 0x24), r20 = ldfp(R + 0x20), r28 = ldfp(R + 0x28);
    if (sp_abs((x + r1c) - g->Ax) > r24 + g->Bx)
        return 0;
    if (sp_abs((y + r20) - g->Ay) > g->By + r28)
        return 0;
    float x2 = x0, y2 = y0;
    if (li != 0xff) {
        x2 = x0 + g->lofs[2 * li];
        y2 = y0 + g->lofs[2 * li + 1];
    }
    if (sp_abs((x2 + r1c) - g->camx) > r24 + g->Dx)
        return 0;
    if (sp_abs((y2 + r20) - g->camy) > g->Dy + r28)
        return 0;
    return 1;
}
/* cec / ec8: the camera with margins e0/e4, then A/B */
static inline int sp_near_cam(uint8_t *gm_, uint64_t R, const SpRegion *g)
{
    const float x0 = ldfp(R + 0x10), y0 = ldfp(R + 0x14);
    const uint32_t li = L8(R + 0x2c);
    float x = x0, y = y0;
    if (li != 0xff) {
        x = x0 + g->lofs[2 * li];
        y = y0 + g->lofs[2 * li + 1];
    }
    const float r1c = ldfp(R + 0x1c), r24 = ldfp(R + 0x24), r20 = ldfp(R + 0x20), r28 = ldfp(R + 0x28);
    if (sp_abs((x + r1c) - g->camx) > r24 + g->e0)
        return 0;
    if (sp_abs((y + r20) - g->camy) > g->e4 + r28)
        return 0;
    float x2 = x0, y2 = y0;
    if (li != 0xff) {
        x2 = x0 + g->lofs[2 * li];
        y2 = y0 + g->lofs[2 * li + 1];
    }
    if (sp_abs((x2 + r1c) - g->Ax) > r24 + g->Bx)
        return 0;
    if (sp_abs((y2 + r20) - g->Ay) > g->By + r28)
        return 0;
    return 1;
}

/* the kill path (7054) */
static void sp_kill(Ctx *c, uint8_t *gm_, uint64_t R)
{
    rt_st32(GP(R + 0x38), 0);
    if (!(L8(R + 0x2d) & 8))
        return;
    const uint64_t comp = L64(R + 0x30);
    if (!comp)
        return;
    c->x[0] = comp;
    SPCALL(SPC_KILL, f_9ae200_orig);
    rt_st64(GP(R + 0x30), 0);
}
/* e78: ready -> spawn */
static void sp_ready_spawn(Ctx *c, uint8_t *gm_, uint64_t R, uint64_t SPt, uint64_t SM, float dts)
{
    c->x[0] = R;
    c->x[1] = SPt;
    c->x[2] = L64(SM + 0x88);
    memset(&c->v[0], 0, 16);
    c->v[0].f[0] = dts;
    SPCALL(SPC_READY, f_ac4b18_orig);
    if (!(uint32_t)c->x[0])
        return;
    c->x[0] = SPt;
    SPCALL(SPC_SPAWN, f_ac4f3c_orig);
}
/* 7230: the spawned entity takes the parent's value */
static void sp_after_spawn(Ctx *c, uint8_t *gm_, uint64_t SPt, uint64_t x25)
{
    const uint64_t par = L64(SPt + 0x40);
    if (!par)
        return;
    const uint32_t v = L32(par + 0xc8);
    rt_st32(GP(x25 + 0x15c), v);
    const uint64_t o = L64(x25 + 0xb0);
    if (!o)
        return;
    c->x[0] = o;
    c->x[1] = 0x9e39a155e35e877eull;
    if (sp_mode == 2) {
        sp_unverifiable = 1;
        return;
    }
    rt_call(c, L64(L64(o) + 0x18));
    if (!(uint32_t)c->x[0])
        return;
    const uint64_t o2 = L64(x25 + 0xb0);
    if (!o2)
        return;
    rt_st32(GP(o2 + 0x1e8), v);
    rt_st32(GP(L64(o2 + 0xf8) + 0x3c), v);
}
/* 7130 / 71ec: spawned at the parent's offset (the position put back after) */
static void sp_spawn_offset(Ctx *c, uint8_t *gm_, uint64_t SPt, uint64_t par)
{
    const uint64_t r = L64(SPt + 0x30);
    const float z = ldfp(r + 0x18);
    const float nz = z + ldfp(par + 0x90);
    const uint64_t xy = L64(r + 0x10);
    float x, y;
    memcpy(&x, &xy, 4);
    memcpy(&y, (const uint8_t *)&xy + 4, 4);
    stfp(r + 0x10, x + ldfp(par + 0x88));
    stfp(r + 0x14, y + ldfp(par + 0x8c));
    stfp(r + 0x18, nz);
    c->x[0] = SPt;
    SPCALL(SPC_SPAWN, f_ac4f3c_orig);
    const uint64_t r2 = L64(SPt + 0x30);
    rt_st64(GP(r2 + 0x10), xy);
    stfp(r2 + 0x18, z);
}

static void sp_slot(Ctx *c, uint8_t *gm_, uint64_t R, uint64_t SPt, uint64_t SM, uint64_t mode, const SpRegion *g, float dts)
{
    const uint32_t st = L32(R);
    const uint32_t kind = L8(R + 4);
    if (st == 3) {
        if (kind == 8)
            goto zero;
        if (L8(SPt + 0xbb) && kind == 7)
            goto zero;
        if (!(kind <= 9 && ((1u << (kind & 31)) & 0x261)))
            return;
        if (!sp_near_cam(gm_, R, g))
            goto zero;
        return;
    }
    if (st == 2) {
        if (kind - 8 < 4 || kind == 6)
            goto zero;
        if (L8(SM + 0xfe)) {
            if (kind - 3 < 3 || kind == 0)
                goto ec8;
            if (kind == 7)
                goto zero;
            goto fb0;
        }
        if (L8(SPt + 0xbb) && kind == 7)
            goto zero;
        if (kind > 5 || !((1u << kind) & 0x39))
            goto fb0;
    ec8:
        if (!sp_near_cam(gm_, R, g))
            goto zero;
    fb0: {
            const uint64_t par = L64(SPt + 0x40);
            if (!par)
                return;
            const uint64_t pr = L64(par + 0x30);
            if (!(L8(pr + 0x2d) & 1))
                return;
            if (L32(pr) == 1)
                goto zero;
            return;
        }
    }
    if (st != 0)
        return;
    {
        const uint64_t m = L64(R + 8);
        if (m) {
            const uint64_t x10 = m & ~mode;
            const uint32_t pass = (x10 & ~1ull) == 0 ? !(m & 1) : (uint32_t)m;
            if (!(pass & 1))
                return;
        }
    }
    switch (kind) {
    case 1: case 2:
        sp_ready_spawn(c, gm_, R, SPt, SM, dts);
        return;
    case 3: case 9:
        if (!sp_in_area(gm_, R, g))
            goto kill;
        sp_ready_spawn(c, gm_, R, SPt, SM, dts);
        return;
    case 12: case 13:
        return;
    case 14: {
        *(uint8_t *)GP(SPt + 0xb9) = 1;
        rt_st32(GP(L64(SPt + 0x30)), 3);
        rt_st64(GP(SPt + 0x38), 0);
        return;
    }
    case 8: {
        const uint64_t par = L64(SPt + 0x40);
        if (!par)
            goto kill;
        const uint64_t R2 = L64(par + 0x30);
        float x = ldfp(R2 + 0x10), y = ldfp(R2 + 0x14);
        const uint32_t li = L8(R2 + 0x2c);
        if (li != 0xff) {
            x = x + g->lofs[2 * li];
            y = y + g->lofs[2 * li + 1];
        }
        if (sp_abs((x + ldfp(R2 + 0x1c)) - g->Ax) > ldfp(R2 + 0x24) + g->Bx)
            goto kill;
        {
            const float d = sp_abs((y + ldfp(R2 + 0x20)) - g->Ay), lim = g->By + ldfp(R2 + 0x28);
            if (d > lim)                 /* (b.le: NaN goes on) */
                goto kill;
        }
        const uint32_t t = L32(SPt + 0xa4);
        if (t == 0x1f8 || t == 0x20a || t == 0x146 || t == 0x175 || t == 0xaa || t == 0x11f || t == 0x2f || t == 0x45) {
            sp_ready_spawn(c, gm_, R, SPt, SM, dts);
            return;
        }
        c->x[0] = SPt;
        c->x[1] = 1;
        SPCALL(SPC_PERMKILLED, f_ac52b8_orig);
        if ((uint32_t)c->x[0] & 1)
            goto kill;
        sp_ready_spawn(c, gm_, R, SPt, SM, dts);
        return;
    }
    case 7: {
        const uint64_t par = L64(SPt + 0x40);
        if (!par) {
            c->x[0] = SPt;
            SPCALL(SPC_SPAWN, f_ac4f3c_orig);
            c->x[0] = SPt;
            SPCALL(SPC_DISOWN, f_ac5a70_orig);
            return;
        }
        int go710c = 0;
        const uint64_t pr = L64(par + 0x30);
        if (L32(pr) == 1) {
            if (L8(pr + 0x2d) & 1) {
                go710c = 1;
            } else if (L32(par + 0xa4) == 0x16d) {
                const uint32_t w = L32(SPt + 0xa4);
                if (w == 0x30 || w == 0x2d) {
                    go710c = 1;
                } else if (w == 0x24) {
                    uint64_t props = L64(SPt);
                    if (props) {
                        c->x[0] = props;
                        c->x[1] = GUEST_IMAGE_BASE + 0xd03b10u;     /* "Buried" */
                        SPCALL(SPC_PROP, f_615680_orig);
                        if ((uint32_t)c->x[0] & 1) {
                            go710c = 1;
                        } else {
                            props = L64(SPt);
                            if (props) {
                                c->x[0] = props;
                                c->x[1] = GUEST_IMAGE_BASE + 0xd03b17u; /* "BuriedGlass" */
                                SPCALL(SPC_PROP, f_615680_orig);
                                go710c = (uint32_t)c->x[0] != 0;
                            }
                        }
                    }
                }
            }
        }
        if (go710c) {
            const uint64_t p2 = L64(SPt + 0x40);
            if (L8(L64(p2 + 0x30) + 0x2d) & 0x10) {
                sp_spawn_offset(c, gm_, SPt, p2);
            } else {
                c->x[0] = SPt;
                SPCALL(SPC_SPAWN, f_ac4f3c_orig);
            }
            const uint64_t x25 = L64(SPt + 0x38);
            if (x25)
                sp_after_spawn(c, gm_, SPt, x25);
            return;
        }
        {   /* 7130 */
            const uint64_t p2 = L64(SPt + 0x40);
            const uint64_t pr2 = L64(p2 + 0x30);
            if (L32(pr2) != 3 || !(L8(pr2 + 0x2d) & 2))
                return;
            sp_spawn_offset(c, gm_, SPt, p2);
            const uint64_t p3 = L64(SPt + 0x40);
            if (!p3 || L8(p3 + 0xb9)) {
                rt_st64(GP(SPt + 0x40), 0);
                c->x[0] = SPt;
                SPCALL(SPC_DISOWN, f_ac5a70_orig);
            }
            const uint64_t x25 = L64(SPt + 0x38);
            if (x25)
                sp_after_spawn(c, gm_, SPt, x25);
            return;
        }
    }
    default:        /* 0, 4, 5, 6, 10, 11 and anything above 14 */
        if (!sp_visible_bd0(gm_, R, g))
            goto kill;
        sp_ready_spawn(c, gm_, R, SPt, SM, dts);
        return;
    }
kill:
    sp_kill(c, gm_, R);
    return;
zero:
    rt_st32(GP(R), 0);
}

static void update_spawn_manager(Ctx *c)
{
    GM_BEGIN;
    const uint64_t W = c->x[0], CTX = c->x[1];
    const uint64_t saved_sp = c->sp;
    c->sp = saved_sp - 0x930;
    float *const lofs = GP(saved_sp - 0x930 + 0x88);
    for (uint64_t nd = L64(W + 0xb0); nd; nd = L64(nd + 0x18)) {
        const uint64_t comp = L64(nd + 8);
        if (L32(comp + 0x370) != 5)
            continue;
        for (uint64_t n2 = L64(comp + 0x438); n2; n2 = L64(n2 + 0x18)) {
            const uint64_t SM = L64(L64(n2 + 8) + 0x3a0);
            if (!L8(SM + 0xfc))
                continue;
            const uint64_t WR = L64(SM + 0x88);
            const uint64_t lt = L64(WR + 0x390);
            if (lt) {
                const uint64_t cnt = L64(L64(lt + 0x10) + 0x38) >> 34;
                uint64_t src = L64(lt + 0x40) + 0x130;
                for (uint64_t i = 0; i < cnt; i++, src += 0x1d8)
                    rt_st64(GP(saved_sp - 0x930 + 0x88 + 8 * i), L64(src));
            }
            SpRegion g;
            g.lofs = lofs;
            sp_dbg_region = &g;
            float dts = ldfp(CTX + 8);
            const float scale = ldfp(L64(L64(SM + 0x108) + 0x10) + 0x68);
            const uint64_t CAM = L64(L64(WR + 0x10) + 0x16c0);
            const uint64_t CV = CAM + 0x6a24;
            g.camx = ldfp(CV + 0x88);
            const float cy0 = ldfp(CV + 0x8c), cz = ldfp(CV + 0x90);
            g.camy = cy0 + (ldfp(CAM + 0x1bc) - ldfp(CAM + 0x1c0)) * 0.5f;
            stfp(SM + 0xa4, g.camx);
            stfp(SM + 0xa8, g.camy);
            stfp(SM + 0xac, cz);
            const float s0 = ldfp(SM + 0xdc) + ldfp(SM + 0xc4);
            g.Dx = ldfp(SM + 0xd4) + ldfp(SM + 0xbc);
            g.Dy = ldfp(SM + 0xd8) + ldfp(SM + 0xc0);
            stfp(SM + 0xb0, g.Dx);
            stfp(SM + 0xb4, g.Dy);
            stfp(SM + 0xb8, s0);
            if (L8(CV + 0xc)) {
                g.Ax = ldfp(CAM + 0x6a18);
                g.Ay = ldfp(CAM + 0x6a1c);
                const float e4 = hexf(0x3ecccccdu);
                g.Bx = (ldfp(CV) + e4) + ldfp(SM + 0xc8);
                g.By = (ldfp(CV + 4) + e4) + ldfp(SM + 0xcc);
                g.Z = (ldfp(CV + 8) + 0.0f) + ldfp(SM + 0xd0);
            } else {
                g.Ax = g.Ay = 0.0f;
                g.Bx = g.By = g.Z = hexf(0xfe7fffffu);       /* mov w8, #-0x1800001 */
            }
            g.e0 = ldfp(SM + 0xe0);
            g.e4 = ldfp(SM + 0xe4);
            if (L8(SM + 0x101)) {
                c->x[0] = SM;
                SPCALL(SPC_PERSIST, f_ac3554_orig);
                *(uint8_t *)GP(SM + 0x101) = 0;
            }
            c->x[0] = L64(GUEST_IMAGE_BASE + 0x5051910u);
            c->x[1] = 0;
            SPCALL(SPC_MODE, f_a7692c_orig);
            const uint64_t mode = c->x[0];
            const uint64_t pcnt = L64(SM + 0x20);
            if (pcnt && L32(SM + 0x14)) {
                dts = dts * scale;
                const uint64_t arr = L64(SM + 0x18);
                const uint64_t end = (uint32_t)(arr + pcnt * 8);
                uint32_t w9 = 1;
                for (uint64_t cur = (uint32_t)arr; cur != end; cur = (uint32_t)(cur + 8)) {
                    if (!w9)
                        continue;
                    const uint64_t P = L64(cur);
                    uint32_t w = 0, nw;
                    for (;;) {
                        const uint64_t bits = L64(P + (uint64_t)w * 8);
                        if (bits != ~0ull) {
                            const uint64_t R0 = P + 0x36080 + (uint64_t)w * 0x1000;
                            const uint64_t S0 = P + 0x80 + (uint64_t)w * 0x3600;
                            for (int hf = 0; hf < 2; hf++) {
                                uint32_t live = ~(uint32_t)(bits >> (32 * hf));
                                while (live) {
                                    const uint32_t j = __builtin_ctz(live) + 32 * hf;
                                    live &= live - 1;
                                    sp_slot(c, gm_, (uint32_t)(R0 + j * 0x40), (uint32_t)(S0 + j * 0xd8), SM, mode, &g, dts);
                                }
                            }
                        }
                        w++;
                        nw = L32(SM + 0x14);
                        if (w == nw)
                            break;
                    }
                    w9 = nw;
                }
            }
            /* after: the area set up once both halves are not negative */
            if (!(g.Bx < 0.0f) && !(g.By < 0.0f) && !(g.Z < 0.0f))
                *(uint8_t *)GP(SM + 0x100) = 1;
        }
    }
    c->sp = saved_sp;
}

/* MINA_VERIFY_SPAWN: the regions the function writes (managers' heads, pools) */
typedef struct { uint64_t addr; uint32_t len; uint8_t *pre, *game; } SpSnap;
static int sp_snap(uint8_t *gm_, uint64_t W, SpSnap *sn, int max)
{
    int n = 0;
    for (uint64_t nd = L64(W + 0xb0); nd; nd = L64(nd + 0x18)) {
        const uint64_t comp = L64(nd + 8);
        if (L32(comp + 0x370) != 5)
            continue;
        for (uint64_t n2 = L64(comp + 0x438); n2; n2 = L64(n2 + 0x18)) {
            const uint64_t SM = L64(L64(n2 + 8) + 0x3a0);
            if (!L8(SM + 0xfc) || n >= max)
                continue;
            sn[n++] = (SpSnap){ SM, 0x110, NULL, NULL };
            const uint64_t cnt = L64(SM + 0x20), arr = L64(SM + 0x18);
            for (uint64_t k = 0; k < cnt && n < max; k++)
                sn[n++] = (SpSnap){ L64(arr + 8 * k), 0x36080 + 0x10000, NULL, NULL };
        }
    }
    return n;
}
void nat__ZN5World18UpdateSpawnManagerEP20ycUpdateQueueContext(Ctx *c)
{
    static int verify = -1;
    if (verify < 0)
        verify = getenv("MINA_VERIFY_SPAWN") != NULL;
    if (!verify) {
        update_spawn_manager(c);
        return;
    }
    GM_BEGIN;
    static uint32_t calls, bad, skipped;
    SpSnap sn[16];
    const int ns = sp_snap(gm_, c->x[0], sn, 16);
    for (int i = 0; i < ns; i++) {
        sn[i].pre = malloc(sn[i].len);
        sn[i].game = malloc(sn[i].len);
        memcpy(sn[i].pre, GP(sn[i].addr), sn[i].len);
    }
    const Ctx save = *c;
    sp_n = 0;
    sp_depth = 0;
    sp_unverifiable = 0;
    sp_mode = 1;
    f_b66874_orig(c);
    sp_mode = 0;
    const Ctx after = *c;
    for (int i = 0; i < ns; i++) {
        memcpy(sn[i].game, GP(sn[i].addr), sn[i].len);
        memcpy(GP(sn[i].addr), sn[i].pre, sn[i].len);
    }
    *c = save;
    sp_pos = 0;
    sp_bad = 0;
    sp_mode = 2;
    update_spawn_manager(c);
    sp_mode = 0;
    calls++;
    if (sp_unverifiable && !sp_bad) {
        skipped++;
    } else {
        int diff = sp_bad || sp_pos != sp_n;
        for (int i = 0; i < ns && !diff; i++) {
            const uint8_t *mine = GP(sn[i].addr);
            for (uint32_t o = 0; o < sn[i].len; o++) {
                if (mine[o] == sn[i].game[o])
                    continue;
                /* a byte of a record or spawn point handed to a call: the call's effects are not replayed */
                const uint64_t a = sn[i].addr + o;
                int called = 0;
                for (int k = 0; k < sp_n && !called; k++) {
                    const uint64_t t = sp_trace[k].a0;
                    called = (a >= t && a < t + 0xd8) || (a >= (t & ~0x3full) && a < (t & ~0x3full) + 0x40);
                }
                if (called)
                    continue;
                if (bad < 20)
                    rt_log("[verify] UpdateSpawnManager differs at %#llx (+%#x of %#llx): game %02x, mine %02x\n",
                           (unsigned long long)a, o, (unsigned long long)sn[i].addr, sn[i].game[o], mine[o]);
                diff = 1;
                break;
            }
        }
        if (diff && bad++ < 20)
            rt_log("[verify] UpdateSpawnManager differs (%u of %u): %d calls replayed of %d\n", bad, calls, sp_pos, sp_n);
    }
    /* the game's outcome stays */
    for (int i = 0; i < ns; i++) {
        memcpy(GP(sn[i].addr), sn[i].game, sn[i].len);
        free(sn[i].pre);
        free(sn[i].game);
    }
    *c = after;
    if ((calls & 0x3ff) == 0)
        rt_log("[verify] UpdateSpawnManager: %u calls checked, %u differ, %u compared up to a call with effects\n", calls,
               bad, skipped);
}

/* ---- ycTexture::AllocStagingBuf ---------------------------------------------------------
 * A texture upload's staging record (32 bytes from the game's TLSF heap):
 * [0] mip, [8] the buffer's contents, [16] the buffer, [24] 0. The game
 * stores the new buffer into [16] with objc_storeStrong, releasing what the
 * block held there: a reused block's old bytes. On macOS that is nil (the
 * game works); here it was a live object's address, so a sampler, a texture
 * or a depth/stencil state got one release too many and died while bound
 * (the scene drawn without its tiles or its depth test). The same steps,
 * with [16] empty first. */
void f_ca5c80(Ctx *c);    /* _objc_msgSend$newBufferWithLength:options: */
void f_ca4860(Ctx *c);    /* _objc_msgSend$contents */
void f_1e4bcc(Ctx *c);    /* ycTexture::CalcLinearMipChainSize */
void nat__ZN9ycTexture15AllocStagingBufEj(Ctx *c)
{
    GM_BEGIN;
    const uint64_t T = c->x[0];
    const uint32_t mip = (uint32_t)c->x[1];
    const uint64_t heap = L64(0x6039f60);              /* the game's allocator */
    c->x[0] = heap;
    c->x[1] = 32;
    c->x[2] = 8;
    c->x[3] = 0;
    c->x[4] = 0;
    c->x[5] = 0;
    rt_call(c, L64(L64(heap)));
    const uint64_t S = c->x[0];
    rt_st32(GP(S), mip);
    rt_st64(GP(S + 16), 0);
    const uint32_t fmt = L32(T + 0x18), w = L32(T + 0x8), h = L32(T + 0xc);
    uint32_t size;
    if (mip != 0xffffffffu) {
        const uint64_t e = 0x1d33ab4 + (uint64_t)fmt * 12;   /* block width, height, bytes */
        size = udiv0(w, L32(e)) * udiv0(h, L32(e + 4)) * L32(e + 8);
    } else {
        c->x[0] = fmt;
        c->x[1] = w;
        c->x[2] = h;
        c->x[3] = L32(T + 0x14);
        f_1e4bcc(c);
        size = (uint32_t)c->x[0];
    }
    const uint32_t len = (L32(T + 0x1c) & 4) ? size * 6 : size;
    c->x[0] = L64(0x603ad38);                           /* the device */
    c->x[2] = len;
    c->x[3] = 0;
    f_ca5c80(c);
    rt_st64(GP(S + 16), c->x[0]);                       /* (what was there: nothing) */
    c->x[0] = L64(S + 16);
    f_ca4860(c);
    rt_st64(GP(S + 8), c->x[0]);
    rt_st64(GP(S + 24), 0);
    c->x[0] = S;
}

/* ---- ycPaletteTexture color lookups (linear searches, ~15% of the game thread on
 * hub_overworld_south: recompiled, each color compare is four byte loads) ----
 * A palette's colors: its own (+0x18, count +0x28), or with a source (+0, flag +0x3e)
 * the source's (+0x10 -> +0x10 colors, +4 count). MINA_PAL_CHECK=1 runs the original
 * too and reports differences. */
void f_1b5f8c_orig(Ctx *c);
void f_1b4758_orig(Ctx *c);
static int pal_check = -1;
static inline void pal_colors(uint8_t *gm_, uint64_t P, uint32_t *n, uint32_t *colors)
{
    const uint64_t src = L64(P);
    if (src && L8(P + 0x3e)) {
        const uint64_t d = L64(src + 0x10);
        *n = L32(d + 4);
        *colors = (uint32_t)L64(d + 0x10);
    } else {
        *n = L32(P + 0x28);
        *colors = (uint32_t)L64(P + 0x18);
    }
}
static uint32_t pal_find_from_base(uint8_t *gm_, uint64_t P, uint64_t color, uint64_t B)
{
    uint32_t n, cols, bn, bcols;
    pal_colors(gm_, P, &n, &cols);
    pal_colors(gm_, B, &bn, &bcols);
    const uint32_t want = L32(color);
    const uint8_t *bc = GP(bcols);
    for (uint32_t i = 0; i < bn; i++) {
        uint32_t v;
        memcpy(&v, bc + 4 * i, 4);
        if (v != want)
            continue;
        if (i < n)
            return L32(cols + 4 * i);
        /* past this palette's end: wrapped into 1..n-1 (udiv by 0 gives 0 on arm64) */
        const uint32_t m = n - 1;
        const uint32_t q = m ? i / m : 0;
        return L32(cols + 4 * (i - q * m + 1));
    }
    return want;
}
static uint32_t pal_get_index(uint8_t *gm_, uint64_t P, uint32_t color, uint32_t fuzzy)
{
    uint32_t n, cols;
    pal_colors(gm_, P, &n, &cols);
    const uint8_t *pc = GP(cols);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v;
        memcpy(&v, pc + 4 * i, 4);
        if (v == color)
            return i;
    }
    if (!fuzzy || (color >> 24) == 0xff)
        return 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v;
        memcpy(&v, pc + 4 * i, 4);
        if (((v ^ color) & 0xffffff) == 0)
            return i;
    }
    return 0;
}
static void pal_report(const char *what, uint32_t got, uint32_t want)
{
    static unsigned calls, bad;
    calls++;
    if (got != want && bad++ < 16)
        rt_log("[verify] %s: native %#x, original %#x\n", what, got, want);
    if ((calls & 0xffff) == 0)
        rt_log("[verify] palette lookups: %u checked, %u differ\n", calls, bad);
}
void nat__ZNK16ycPaletteTexture12FindFromBaseERK7ycColorPKS_(Ctx *c)
{
    GM_BEGIN;
    const uint32_t r = pal_find_from_base(gm_, c->x[0], c->x[1], c->x[2]);
    if (pal_check < 0)
        pal_check = getenv("MINA_PAL_CHECK") != NULL;
    if (pal_check) {
        f_1b5f8c_orig(c);
        pal_report("FindFromBase", r, (uint32_t)c->x[0]);
    }
    c->x[0] = r;
}
void nat__ZNK16ycPaletteTexture8GetIndexE7ycColorb(Ctx *c)
{
    GM_BEGIN;
    const uint32_t r = pal_get_index(gm_, c->x[0], (uint32_t)c->x[1], (uint32_t)c->x[2]);
    if (pal_check < 0)
        pal_check = getenv("MINA_PAL_CHECK") != NULL;
    if (pal_check) {
        f_1b4758_orig(c);
        pal_report("GetIndex", r, (uint32_t)c->x[0]);
    }
    c->x[0] = r;
}

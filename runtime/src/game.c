/* Natives for the game's own hot code (runtime/natives.txt): measured first,
 * then replaced where the recompiled code is too slow for the Vita. */
#include "runtime.h"

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

/* MINA_FPS: fixed updates (Game::FixedUpdate) and updates per presented frame,
 * and the time spent in them */
uint32_t g_fixed_updates, g_updates;
uint64_t g_fixed_us, g_update_us;
uint32_t g_composites, g_bounds;        /* calls (MINA_FPS) */
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
    if (!(f & 0x20)) {
        if (!(f & 0x200)) {
            const float ox = o->q[0], oy = o->q[1], oz = o->q[2], ow = o->q[3];
            const float ax = a->q[0], ay = a->q[1], az = a->q[2], aw = a->q[3];
            const float w = ow * aw - ((ox * ax + oy * ay) + oz * az);
            const float zz = aw * oz + ow * az;
            o->q[0] = (ox * aw + ax * ow) + (oz * ay - oy * az);
            o->q[1] = (oy * aw + ay * ow) + (ox * az - oz * ax);
            o->q[2] = (oy * ax - ox * ay) + zz;
            o->q[3] = w;
        }
        if (!(f & 0x108)) {
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
    const uint64_t self = c->x[0];
    const float *m = G(ld_ptr(self + 0x60));
    float box[6];
    uint64_t src = ld_ptr(self + 0x40) ? ld_ptr(self + 0x48) : 0;
    const float *b = src ? G(src + 0x30) : NULL;
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
    memcpy(G(ld_ptr(self + 0x80)), box, sizeof box);
    for (int k = 1; k < 4; k++) {
        uint64_t p = ld_ptr(self + 0x80 + 8 * k);
        if (!p)
            break;
        memcpy(G(p), box, sizeof box);
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
    const uint32_t mode = w5 ? rt_ld32(G(pa + 0xc)) : 0;
    if (!mode) {
        compose(o, ap, bp, flags);
        return;
    }
    const float *p = G(pa);
    XForm b = *bp;
    const float bz = b.t[2];
    const float ty = w6 ? b.t[1] : -0.0f;
    /* GetIsoDepth(x0 = P+0x18, x1 = mode, s0 = A.t[1] + P[0] + (B.t[1] or -0), s1 = P[4], s2 = P[2]) */
    const float depth = iso_depth(ld_ptr(pa + 0x18), mode, (ap->t[1] + p[0]) + ty, p[4], p[2]);
    b.t[2] = depth - p[1];
    XForm a = *ap;
    const uint32_t pflags = rt_ld32(G(pa + 0x14));
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
void f_9a65c4_orig(Ctx *c);
static void recalc_world_transform(Ctx *c)
{
    const uint64_t O = c->x[0];
    const XForm *W = G(c->x[1]);
    const uint64_t saved_sp = c->sp;
    const uint64_t sp = saved_sp - 0xf0;
    c->sp = sp;
    XForm *L = G(sp + 0x8), *Wc = G(sp + 0x30), *R = G(sp + 0x60);

    const uint64_t par = ld_ptr(O + 0x200);
    if (par && !(*(uint8_t *)G(O + 0x1f8) & 2)) {
        rt_st32(G(O + 0x1dc), rt_ld32(G(par + 0xbc)));
        rt_st32(G(O + 0x1e4), rt_ld32(G(O + 0x1e4)) | 0x20);
    }
    const float s8 = ldf(O + 0x1d8);
    uint32_t w22 = rt_ld32(G(O + 0x1e4));
    float s11 = s8;
    if ((w22 & (1u << 20)) && rt_ld32(G(O + 0x1dc)) != 4) {
        s11 = -s8;
        stf(O + 0x1d8, s11);
    }
    const float s9 = ldf(O + 0x1d0), s10 = ldf(O + 0x1e0);

    /* the layer: O+0x1e0 (when 0) from the depth of the parent's or own sprite */
    int layer = 0;            /* 0: keep, 1: from x8 below, 2: 1.0 */
    uint64_t x8 = 0;
    if (par) {
        compose(R, G(par + 0x78), G(O + 0x180), ld_ptr(O + 0x18));
        const uint32_t f = rt_ld32(G(O + 0x1f8));
        if (!(f & 1))
            stf(O + 0x1d0, ldf(par + 0xc0) + s9);
        if (!(f & 2)) {
            rt_st32(G(O + 0x1dc), rt_ld32(G(par + 0xbc)));
            w22 |= 0x20;
            rt_st32(G(O + 0x1e4), w22);
            stf(O + 0x1d8, ldf(par + 0xc4) + s11);
        }
        if (f & 0x20) {
            if (s10 == 0.0f) {
                x8 = ld_ptr(O + 0x248);
                layer = x8 && !(*(uint8_t *)G(O + 0x1f8) & 0x40) ? 1 : 2;
                if (!x8)
                    layer = 3;               /* 1.0 stored as is */
                else if (layer == 2)
                    layer = 3;
            }
        } else {
            x8 = ld_ptr(par + 0x50);
            layer = x8 ? 1 : 2;
        }
    } else {
        *R = *(const XForm *)G(O + 0x180);
        if (s10 == 0.0f) {
            x8 = ld_ptr(O + 0x248);
            layer = x8 && !(*(uint8_t *)G(O + 0x1f8) & 0x40) ? 1 : 3;
        }
    }
    if (layer) {
        float v = 1.0f;
        if (layer == 1) {
            int32_t d = (int32_t)rt_ld32(G(x8 + 0x15c));
            if (d > 10)
                d -= 10;
            d += 2;
            v = (float)(int32_t)((float)d * 0.5f);
        }
        stf(O + 0x1e0, layer == 3 ? v : s10 + v);
    }

    *Wc = *W;
    if (*(uint8_t *)G(O + 0x1cb)) {
        /* whole pixels: floor(t * k0 + 0.5) * k1 */
        const float k0 = ldf(0x6039f5c), k1 = ldf(0x6039f58);
        for (int i = 0; i < 2; i++) {
            Wc->t[i] = floorf(Wc->t[i] * k0 + 0.5f) * k1;
            R->t[i] = floorf(R->t[i] * k0 + 0.5f) * k1;
        }
    }
    memcpy(G(O + 0x274), R->t, 12);
    calc_transform_p(c, L, O + 0x1d0, Wc, R, ld_ptr(O + 0x18), w22 >> 4 & 1, w22 >> 2 & 1);
    memcpy(G(O + 0x25c), L->t, 12);
    stf(sp + 4, 0.0f);
    float hook = 0.0f;
    const uint64_t h = ld_ptr(O + 0x2b8);
    if (h) {
        *(uint8_t *)G(sp + 3) = 0;
        c->v[0] = vset_f32((rt_ld32(G(O + 0x1e4)) & 0x1000000) ? -0.8f : 0.0f);
        c->x[0] = h;
        c->x[1] = O;
        c->x[2] = sp + 0x8 + 0x1c;
        c->x[3] = sp + 0x8 + 0x10;
        c->x[4] = sp + 4;
        c->x[5] = sp + 3;
        rt_call(c, ld_ptr(ld_ptr(h) + 0x60));
        if (!(*(uint8_t *)G(O + 0x1e6) & 0x20)) {
            const float step = *(uint8_t *)G(sp + 3) ? 10.0f : -10.0f;
            if (ldf(O + 0x1e0) == 1.0f)
                L->t[2] = L->t[2] + step;
        }
        hook = ldf(sp + 4);
    }
    memcpy(G(O + 0x268), L->t, 12);

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
    stf(ld_ptr(O + 0xa0) + 4, hook);
    for (int k = 1; k < 4; k++) {
        uint64_t p = ld_ptr(O + 0xa0 + 8 * k);
        if (!p)
            break;
        stf(p + 4, hook);
    }
    memcpy(G(ld_ptr(O + 0x60)), m, sizeof m);
    for (int k = 1; k < 4; k++) {
        uint64_t p = ld_ptr(O + 0x60 + 8 * k);
        if (!p)
            break;
        memcpy(G(p), m, sizeof m);
    }
    c->x[0] = O;
    update_bounds(c);
    stf(O + 0x1e0, s10);
    stf(O + 0x1d0, s9);
    stf(O + 0x1d8, s8);
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
void nat__ZN8GameAnim20RecalcWorldTransformERK11ycTransformS2_(Ctx *c)
{
    recalcs_total++;
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
static void refresh_world_transform(Ctx *c)
{
    const uint64_t E = c->x[0];
    const uint64_t saved_sp = c->sp, sp = saved_sp - 0xb0;
    c->sp = sp;
    XForm *world = G(E + 0xc0);
    XForm *P = G(sp), *prev = G(sp + 0x50);
    *prev = *world;
    uint64_t flags = ld_ptr(E + 0x18);
    if (!(flags & 1)) {
        *world = *(const XForm *)G(E + 0x80);
    } else {
        const uint64_t prov = ld_ptr(E + 0x60);
        if (prov) {
            static const GuestFunc *ic;
            c->x[0] = prov;
            c->x[8] = sp;                        /* the transform comes back at sp */
            RT_CALL_IC(c, ld_ptr(ld_ptr(prov)), ic);
            flags = ld_ptr(E + 0x18);
        } else {
            const uint64_t parent = ld_ptr(E + 0x10);
            if (parent) {
                *P = *(const XForm *)G(parent + 0xc0);
            } else {
                memcpy(P, G(0x1d48610), 16);       /* identity: the game's constants */
                memcpy((uint8_t *)P + 16, G(0x1d486d0), 16);
                memset((uint8_t *)P + 32, 0, 8);
            }
        }
        XForm r;
        compose(&r, P, G(E + 0x80), flags);
        *world = r;
    }
    for (uint64_t page = ld_ptr(E + 0x38); page; page = ld_ptr(page + 0x8)) {
        for (uint32_t i = 0; i < ld_ptr(page + 0x10); i++) {
            const uint64_t comp = ld_ptr(page + 0x18 + 8 * (uint64_t)i);
            const uint64_t f = ld_ptr(comp + 0x18);
            if (!(f & 1) || (f & 0x3000) == 0x1000)
                continue;
            static const GuestFunc *ic;
            c->x[0] = comp;
            c->x[1] = E + 0xc0;
            c->x[2] = sp + 0x50;
            RT_CALL_IC(c, ld_ptr(ld_ptr(comp) + 0x48), ic);
        }
    }
    c->sp = saved_sp;
}
void nat__ZN8ycEntity21RefreshWorldTransformEb(Ctx *c)
{
    refresh_world_transform(c);
}

/* ---- IsoAnim::GetIsoDepth --------------------------------------------------------------------
 * x0 component, w1 mode, s0 a, s1 b, s2 c: a depth (s0). Fixed per mode, or
 * from the camera's position against the level's (mode 4 and above 11). */
static inline float hexf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
void f_873480_orig(Ctx *c);
static float iso_depth(uint64_t comp, uint32_t mode, float a, float b, float cc)
{
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
        const uint64_t lv = ld_ptr(ld_ptr(ld_ptr(comp + 0x10) + 0x50) + 0x16c0);
        const uint64_t q = lv + 0x6a88;
        const uint32_t k = rt_ld32(G(q + 0x58));
        const uint64_t p = (k != 3 && k != 0) ? q + 0x60 : q;
        const float cam = ldf(lv + 0x138);
        const float lo = cam + ldf(p);
        float span = (ldf(p) - cam) - lo;
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
static int plane_culls(const Plane *p, const float *box)
{
    if (__builtin_fabsf(p->w) == __builtin_inff())
        return 0;
    const float ax = fabs_sel(p->x), ay = fabs_sel(p->y), az = fabs_sel(p->z);
    float d = p->x * box[0];
    d = d + box[1] * p->y;
    d = d + box[2] * p->z;
    float r = ax * box[3];
    r = r + ay * box[4];
    r = r + az * box[5];
    return d + r < p->w;
}

static void render_pass_update(Ctx *c)
{
    const uint64_t RP = c->x[0];
    if (!*(uint8_t *)G(RP + 0x258))
        return;
    const uint64_t saved_sp = c->sp, fr = saved_sp - 0x80;    /* campos +0, camdir +0x10, inverse +0x20 */
    c->sp = fr;
    for (uint32_t v = 0; v < 5; v++) {
        const uint64_t cam = ld_ptr(RP + 0x10 + 8 * v);
        if (!cam)
            continue;
        const uint32_t mask = *(uint8_t *)G(RP + 0x9);
        const float *A = G(cam), *B = G(cam + 0x40);
        /* camera position: the translation of B's inverse (the game's code) */
        c->x[0] = cam + 0x40;
        c->x[8] = fr + 0x20;
        f_a33c0(c);
        const float *Binv = G(fr + 0x20);
        float *campos = G(fr), *camdir = G(fr + 0x10);
        campos[0] = Binv[12]; campos[1] = Binv[13]; campos[2] = Binv[14];
        memcpy(camdir, G(cam + 0x10c), 12);
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
        /* bounds of all objects: empty */
        float *ab = G(RP + 0xb0 + 0x18 * v);         /* center 3, extent 3 */
        const float neg_max = hexf(0xfe7fffff);
        ab[0] = ab[1] = ab[2] = 0.0f;
        ab[3] = ab[4] = ab[5] = neg_max;
        float vc[3] = { 0, 0, 0 }, ve[3] = { neg_max, neg_max, neg_max };
        const uint32_t skip_flag = ((1u << v) & mask) ? 8 : 0;
        uint32_t n = 0;
        const uint64_t npages = ld_ptr(RP + 0x138);
        for (uint64_t pg = 0; pg < npages; pg++) {
            const uint64_t pages = ld_ptr(RP + 0x130);
            uint32_t left = *(uint16_t *)G(pages + 0x6660 + 2 * pg);
            if (!left)
                continue;
            const uint64_t page = ld_ptr(pages + 8 * pg);
            for (uint64_t j = 0; left; j++) {
                const uint64_t obj = ld_ptr(page + 8 * j);
                if (!obj)
                    continue;
                left--;
                const uint64_t boxa = page + 0x4800 + 0x18 * j;
                const float *box = G(boxa);
                grow_bounds(ab, ab + 3, box);
                const uint8_t layer = *(uint8_t *)G(page + 8 * j + 0x6001);
                if (!(*(uint8_t *)G(RP + 0x8) & layer))
                    continue;
                const uint64_t fa = page + 8 * j + 0x6000;
                uint8_t f = *(uint8_t *)G(fa);
                if (f & skip_flag)
                    continue;
                if (f & 1) {
                    int culled = 0;
                    for (int k = 0; k < 6 && !culled; k++)
                        culled = plane_culls(&P[k], box);
                    if (culled)
                        continue;
                }
                if (n == ld_ptr(RP + 0x148)) {
                    c->x[0] = RP;
                    c->x[1] = (uint64_t)n * 2;
                    f_1e0ca0(c);
                    f = *(uint8_t *)G(fa);
                }
                uint32_t key;
                if (f & 6) {
                    key = (f >> 1 & 1) - 1u;
                } else {
                    c->x[0] = boxa;
                    c->x[1] = fr;
                    c->x[2] = fr + 0x10;
                    c->x[3] = fa;                    /* the object's flags (ycRenderObjectFlags) */
                    static const GuestFunc *ic;
                    RT_CALL_IC(c, ld_ptr(RP + 0x128), ic);
                    key = (uint32_t)c->x[0];
                }
                rt_st32(G(ld_ptr(RP + 0x1f0 + 8 * v) + 4 * (uint64_t)n), key);
                const uint64_t o64 = obj;
                memcpy(G(ld_ptr(RP + 0x218 + 8 * v) + 8 * (uint64_t)n), &o64, 8);
                n++;
                grow_bounds_vis(vc, ve, box);
            }
        }
        float *vb = G(RP + 0x150 + 0x18 * v);
        vb[0] = vc[0]; vb[1] = vc[1]; vb[2] = vc[2];
        vb[3] = ve[0]; vb[4] = ve[1]; vb[5] = ve[2];
        if (ld_ptr(RP + 0x128) != 0x11e0ae4 && n >= 2) {
            /* bottom-up merge sort by key (ties: the right run first), two buffers */
            const uint64_t o0 = ld_ptr(RP + 0x218 + 8 * v), k0 = ld_ptr(RP + 0x1f0 + 8 * v);
            const uint64_t o1 = ld_ptr(RP + 0x248), k1 = ld_ptr(RP + 0x240);
            uint64_t so = o0, sk = k0, dob = o1, dk = k1;
            for (uint32_t w = 1;; w *= 2) {
                for (uint32_t i = 0; i < n;) {
                    const uint32_t mid = i + w < n ? i + w : n, end = i + 2 * w < n ? i + 2 * w : n;
                    uint32_t a = i, b = mid, d = i;
                    while (d < end) {
                        int take_left;
                        uint32_t ka = 0, kb = 0;
                        if (a < mid) {
                            ka = rt_ld32(G(sk + 4 * (uint64_t)a));
                            take_left = b < end ? (kb = rt_ld32(G(sk + 4 * (uint64_t)b)), ka < kb) : 1;
                        } else {
                            take_left = 0;
                        }
                        if (take_left) {
                            memcpy(G(dob + 8 * (uint64_t)d), G(so + 8 * (uint64_t)a), 8);
                            rt_st32(G(dk + 4 * (uint64_t)d), ka);
                            a++;
                        } else {
                            kb = rt_ld32(G(sk + 4 * (uint64_t)b));
                            memcpy(G(dob + 8 * (uint64_t)d), G(so + 8 * (uint64_t)b), 8);
                            rt_st32(G(dk + 4 * (uint64_t)d), kb);
                            b++;
                        }
                        d++;
                    }
                    i = i + 2 * w;
                }
                uint64_t t;
                t = so; so = dob; dob = t;
                t = sk; sk = dk; dk = t;
                if (2 * w >= n)
                    break;
            }
            if (so != o0) {
                memcpy(G(o0), G(o1), 8 * (size_t)n);
                memcpy(G(k0), G(k1), 4 * (size_t)n);
            }
        }
        memcpy(G(RP + 0x1c8 + 8 * v), &(uint64_t){ n }, 8);
    }
    c->sp = saved_sp;
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

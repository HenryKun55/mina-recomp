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
static uint32_t composites_total;

void f_99f430_orig(Ctx *c);
void nat__ZN4Game11FixedUpdateEv(Ctx *c)
{
    uint64_t t0 = now_us();
    uint32_t before = composites_total;
    g_fixed_updates++;
    f_99f430_orig(c);
    g_fixed_us += now_us() - t0;
    static int heavy;          /* consecutive fixed updates moving thousands of transforms */
    heavy = composites_total - before > 4000 ? heavy + 1 : 0;
    if (heavy == 60 && !g_in_play) {
        g_in_play = 1;
        rt_log("[play] gameplay (%u transforms in a fixed update)\n", composites_total - before);
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

static void composite(Ctx *c)
{
    XForm *o = G(c->x[0]);
    const XForm *a = G(c->x[1]);
    const uint64_t f = c->x[3];
    const XForm b = *(const XForm *)G(c->x[2]);     /* a struct copy: no library call */
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

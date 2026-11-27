/* Natives for the game's own hot code (runtime/natives.txt): measured first,
 * then replaced where the recompiled code is too slow for the Vita. */
#include "runtime.h"

#include <arm_neon.h>
#include <pthread.h>
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
/* MINA_CALLS=1: calls per fixed update of a few hot functions in the [fps] lines (the counts
 * always kept: one add each) */
uint32_t g_calls[24];
const char *const g_call_name[24] = { "RecalcWorld", "DerivedState", "UpdateFrame", "RenderFrame", "RenderFrameFast",
                                      "Shapes", "ShapesFast", "AttackList", "PalComp", "PalAnimEntry", "GetIndex",
                                      "UQItems", "PalSame", "PalNoEntry", "PalNoEntryUs", "PalEntryUs",
                                      "PalOff", "PalSkipped", NULL };
#define CALLS(k) (g_calls[k]++)
static int g_calls_on;           /* MINA_CALLS set (read once at the first palette update) */
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
    /* (tests) only with MINA_WARP=1: the console's game thread opened the file twice a second,
     * and an open behind the card's other reads (the music banks) held it 100-260 ms (the
     * [spike] lines all in Game::FixedUpdate itself) */
    static int on = -1;
    if (on < 0)
        on = getenv("MINA_WARP") != NULL;
    if (!on)
        return;
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
    if (getenv("MINA_CJK_TEST")) {      /* (tests) the on-demand load, mid-game */
        void cjk_fonts_load_test(Ctx *c);
        cjk_fonts_load_test(c);
    }
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
/* MINA_FPS: a game update longer than 40 ms logs where its time went: files
 * (the last one opened), texture uploads, waiting for the GPU lock */
extern uint64_t g_spk_io_us[32], g_spk_lock_us[32], g_spk_tex_us[32];
extern uint32_t g_spk_io_n[32];
extern char g_spk_io_file[32][64];
typedef struct { uint64_t t, io, lock, tex; uint32_t n; } Spk;
static int spk_on(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("MINA_FPS") != NULL;
    return on;
}
static void spk_take(Ctx *c, Spk *k)
{
    const uint32_t id = c->thread_id < 32 ? c->thread_id : 0;
    k->t = now_us();
    k->io = g_spk_io_us[id];
    k->n = g_spk_io_n[id];
    k->lock = g_spk_lock_us[id];
    k->tex = g_spk_tex_us[id];
}
static void spk_check(Ctx *c, const Spk *k, const char *what)
{
    const uint64_t dt = now_us() - k->t;
    if (dt < 40000)
        return;
    const uint32_t id = c->thread_id < 32 ? c->thread_id : 0;
    rt_log("[spike] %s %.1f ms: files %.1f ms (%u calls, last %s), texture uploads %.1f ms, GPU lock %.1f ms\n", what,
           dt / 1000.0, (g_spk_io_us[id] - k->io) / 1000.0, g_spk_io_n[id] - k->n,
           g_spk_io_n[id] != k->n ? g_spk_io_file[id] : "-", (g_spk_tex_us[id] - k->tex) / 1000.0,
           (g_spk_lock_us[id] - k->lock) / 1000.0);
    void rt_profile_spike(uint64_t t0, uint64_t t1);
    rt_profile_spike(k->t, now_us());
}
/* ---- The world's rate (Vita): s_targetFPS, which ycGameThread::ThreadMain reads for every frame
 * (the Max FPS option changes it while the game runs). MINA_TARGET_FPS=n fixes it (0: the game's);
 * unset, it adapts: 60 steps a second while a step and the frame's update fit a 60 fps frame, 30
 * when they don't (the frame falls behind: frames with 2 steps while steps are long). At 30 it
 * tries 60 again after a wait (windows of 30 steps, 1 s) that doubles while tries fail (up to
 * 8 s; 1 s after a drop caused by a hitch such as a load); measuring at 30 can't tell: a step
 * there does ~1.7x the work of one at 60.
 * MINA_RATE_LOG=1 logs the windows. */
static int g_rate_auto;              /* set by Game::InitFromSave */
static uint32_t rate_fixed_since;    /* steps since the last Game::Update */
static uint64_t rate_step_us;        /* the last step's time */
static void rate_set(uint32_t fps, const char *why, double avg_ms, double max_ms)
{
    GM_BEGIN;
    rt_st32(GP(0x1e14de8), fps);
    rt_log("[rate] the world steps %u times a second (%s: step+update %.1f ms average, %.1f worst)\n", fps, why,
           avg_ms, max_ms);
}
static void rate_frame(uint64_t update_us)
{
    static uint32_t frames, late, held, wait = 2, waited, since_fail;
    static double tried_at, fail_ref;     /* the 30 Hz cost when 60 was tried; that of a failed try */
    static uint64_t sum_us, max_us;
    static int log = -1;
    if (log < 0)
        log = getenv("MINA_RATE_LOG") != NULL;
    const uint32_t steps = rate_fixed_since;
    rate_fixed_since = 0;
    if (!steps)
        return;                      /* a frame without a step (30 Hz): no information */
    GM_BEGIN;
    const uint32_t fps = rt_ld32(GP(0x1e14de8));
    const uint64_t cost = rate_step_us + update_us;
    frames++;
    late += steps > 1;
    sum_us += cost;
    if (cost > max_us)
        max_us = cost;
    if (frames < 30)
        return;
    const double avg = sum_us / 1000.0 / frames, worst = max_us / 1000.0;
    if (log)
        rt_log("[rate] %u Hz: %u frames, %u late, step+update %.1f ms average, %.1f worst; next try in %u/%u\n", fps,
               frames, late, avg, worst, waited, wait);
    if (fps >= 60) {
        /* behind: frames with 2 steps while the steps are long (a load's single long
         * step makes frames late too, with short steps around it) */
        if ((late >= 4 && avg > 10.0) || avg > 15.0) {
            /* a window with a long hitch (a load, a room) says little: soon again; else the
             * wait doubles while tries fail (a failed try is half a second of 2-step frames) */
            if (worst > 50.0 || held >= 10) {
                wait = 1;
                fail_ref = 0;
            } else {
                wait = wait * 2 > 8 ? 8 : (wait < 2 ? 2 : wait * 2);
                fail_ref = tried_at;
                since_fail = 0;
            }
            rate_set(30, "falling behind", avg, worst);
            held = waited = 0;
        } else if (++held == 10)
            wait = 2;                /* 60 held for 10 windows: the next drop tries again soon */
    } else if (++since_fail, ++waited >= wait) {
        /* a step at 30 does more work than one at 60 (about 1.7x): hopeless when even
         * half of it would not fit a 60 fps frame; after a failed try, not again until the
         * scene got lighter (15% under the cost then) or 30 s went by */
        waited = 0;
        if (avg * 0.5 >= 14.0)
            wait = wait * 2 > 8 ? 8 : wait * 2;
        else if (fail_ref > 0 && avg > fail_ref * 0.85 && since_fail < 30)
            ;
        else {
            tried_at = avg;
            rate_set(60, "trying", avg, worst);
        }
    }
    frames = late = 0;
    sum_us = max_us = 0;
}

void nat__ZN4Game11FixedUpdateEv(Ctx *c)
{
    Spk spk;
    const int spk_ = spk_on();
    if (spk_)
        spk_take(c, &spk);
    warp_requests(c);
    uint64_t t0 = now_us();
    uint32_t before = recalcs_total;
    g_fixed_updates++;
    g_in_fixed = 1;
    g_game_stage = 2;
    f_99f430_orig(c);
    g_game_stage = 0;
    g_in_fixed = 0;
    rate_step_us = now_us() - t0;
    rate_fixed_since++;
    g_fixed_us += rate_step_us;
    if (spk_)
        spk_check(c, &spk, "fixed update");
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
    Spk spk;
    const int spk_ = spk_on();
    if (spk_)
        spk_take(c, &spk);
    g_updates++;
    g_game_stage = 1;
    f_9a0638_orig(c);
    g_game_stage = 0;
    g_update_us += now_us() - t0;
    if (g_rate_auto)
        rate_frame(now_us() - t0);
    if (spk_)
        spk_check(c, &spk, "update (frame, with its fixed updates)");
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
    CALLS(0);
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
static void caller_count_(CallerHist *h, const void *ra);
/* off (no MINA_CALLERS): one test, no call */
static inline __attribute__((always_inline)) void caller_count(CallerHist *h, const void *ra)
{
    if (__builtin_expect(h->every == ~0u, 1))
        return;
    caller_count_(h, ra);
}
static void caller_count_(CallerHist *h, const void *ra)
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
extern uint64_t g_zone_us[8];
static inline int zones_on(void);
void nat__ZN8ycEntity21RefreshWorldTransformEb(Ctx *c)
{
    static CallerHist h = { "ycEntity::RefreshWorldTransform" };
    caller_count(&h, __builtin_return_address(0));
    static int depth;
    if (depth || !zones_on()) {
        refresh_world_transform(c);
        return;
    }
    depth = 1;
    const uint64_t t = now_us();
    refresh_world_transform(c);
    g_zone_us[6] += now_us() - t;
    depth = 0;
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

/* ---- render pass memo (MINA_NO_RPU_MEMO=1: off): a pass whose inputs are byte for byte the
 * ones of its last update gets that update's outputs again (about a third of the passes on
 * the deck and on boneBeachA, frame after frame). The inputs: the pass's fields the scan
 * reads, its cameras' matrices and direction, and for every page the object slots, boxes and
 * flag blocks up to the last object. The outputs: per view the count, keys, objects and the
 * two bounds. Passes whose sort calls the game (unknown key functions) or whose lists grew
 * are not kept. */
typedef struct RpuMemo {
    uint32_t rp, in_len, in_cap, out_len, out_cap;
    uint8_t *in, *out;
} RpuMemo;
static RpuMemo rpu_memo[64];
static uint8_t *rpu_gbuf;
static uint32_t rpu_gcap;
static uint32_t rpu_hits, rpu_misses;
static inline void rpu_put(uint32_t *len, const void *src, uint32_t n)
{
    if (*len + n > rpu_gcap) {
        rpu_gcap = (*len + n) * 2 + 4096;
        rpu_gbuf = realloc(rpu_gbuf, rpu_gcap);
    }
    memcpy(rpu_gbuf + *len, src, n);
    *len += n;
}
static int rpu_known_sort(uint32_t f)
{
    return f == 0x11e2844u || f == 0x11e2894u || f == 0x11e28e0u || f == 0x11e0ae4u;
}
/* the inputs into rpu_gbuf; 0 when the pass cannot be kept */
static uint32_t rpu_gather(uint8_t *gm_, uint64_t RP)
{
    uint32_t len = 0;
    if (!rpu_known_sort((uint32_t)lp(RP + 0x128)))
        return 0;
    rpu_put(&len, GP(RP + 0x8), 2);                  /* layers, views that skip flag 8 */
    rpu_put(&len, GP(RP + 0x10), 5 * 8);              /* cameras */
    rpu_put(&len, GP(RP + 0x128), 0x28);              /* sort function, pages, page count, ..., capacity */
    rpu_put(&len, GP(RP + 0x1f0), 0x50);              /* the key and object lists */
    for (uint32_t v = 0; v < 5; v++) {
        const uint64_t cam = lp(RP + 0x10 + 8 * v);
        if (!cam)
            continue;
        rpu_put(&len, GP(cam), 0x80);
        rpu_put(&len, GP(cam + 0x10c), 12);
    }
    const uint32_t pages = (uint32_t)lp(RP + 0x130), np = (uint32_t)lp(RP + 0x138);
    for (uint32_t pg = 0; pg < np; pg++) {
        uint32_t left = *(uint16_t *)GP(pages + 0x6660 + 2 * pg);
        rpu_put(&len, &left, 4);
        if (!left)
            continue;
        const uint32_t page = (uint32_t)lp(pages + 8 * pg);
        rpu_put(&len, &page, 4);
        uint32_t j = 0;
        for (; left; j++)
            if (lp(page + 8 * j))
                left--;
        rpu_put(&len, GP(page), 8 * j);
        rpu_put(&len, GP(page + 0x4800), 0x18 * j);
        rpu_put(&len, GP(page + 0x6000), 8 * j);
    }
    return len;
}
static RpuMemo *rpu_slot(uint64_t RP) { return &rpu_memo[((uint32_t)RP >> 4 ^ (uint32_t)RP >> 10) & 63]; }
/* outputs: per active view n, keys, objects, both bounds */
static void rpu_save(uint8_t *gm_, uint64_t RP, uint32_t in_len)
{
    RpuMemo *m = rpu_slot(RP);
    if (m->in_cap < in_len) {
        m->in_cap = in_len + 1024;
        m->in = realloc(m->in, m->in_cap);
    }
    memcpy(m->in, rpu_gbuf, in_len);
    m->in_len = in_len;
    uint32_t len = 0;
    for (uint32_t v = 0; v < 5; v++) {
        if (!lp(RP + 0x10 + 8 * v))
            continue;
        const uint32_t n = (uint32_t)lp(RP + 0x1c8 + 8 * v);
        len += 4 + 48 + 12 * n;
    }
    if (m->out_cap < len) {
        m->out_cap = len + 1024;
        m->out = realloc(m->out, m->out_cap);
    }
    uint8_t *o = m->out;
    for (uint32_t v = 0; v < 5; v++) {
        if (!lp(RP + 0x10 + 8 * v))
            continue;
        const uint32_t n = (uint32_t)lp(RP + 0x1c8 + 8 * v);
        memcpy(o, &n, 4); o += 4;
        memcpy(o, GP(RP + 0xb0 + 0x18 * v), 24); o += 24;
        memcpy(o, GP(RP + 0x150 + 0x18 * v), 24); o += 24;
        memcpy(o, GP(lp(RP + 0x1f0 + 8 * v)), 4 * n); o += 4 * n;
        memcpy(o, GP(lp(RP + 0x218 + 8 * v)), 8 * n); o += 8 * n;
    }
    m->out_len = len;
    m->rp = (uint32_t)RP;
}
static int rpu_restore(uint8_t *gm_, uint64_t RP, uint32_t in_len)
{
    RpuMemo *m = rpu_slot(RP);
    if (m->rp != (uint32_t)RP || m->in_len != in_len || memcmp(m->in, rpu_gbuf, in_len))
        return 0;
    const uint8_t *o = m->out;
    for (uint32_t v = 0; v < 5; v++) {
        if (!lp(RP + 0x10 + 8 * v))
            continue;
        uint32_t n;
        memcpy(&n, o, 4); o += 4;
        memcpy(GP(RP + 0xb0 + 0x18 * v), o, 24); o += 24;
        memcpy(GP(RP + 0x150 + 0x18 * v), o, 24); o += 24;
        memcpy(GP(lp(RP + 0x1f0 + 8 * v)), o, 4 * n); o += 4 * n;
        memcpy(GP(lp(RP + 0x218 + 8 * v)), o, 8 * n); o += 8 * n;
        const uint64_t n64 = n;
        memcpy(GP(RP + 0x1c8 + 8 * v), &n64, 8);
    }
    return 1;
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
    if (stats) {   /* how many passes share their object pages and camera (per 600 calls) */
        static uint32_t keys[600], nk, ndist;
        const uint32_t k = (uint32_t)lp(RP + 0x130) * 31u ^ (uint32_t)lp(RP + 0x10);
        int seen = 0;
        for (uint32_t i = 0; i < nk; i++)
            if (keys[i] == k) {
                seen = 1;
                break;
            }
        if (!seen && nk < 600)
            keys[nk++] = k;
        {   /* would last frame's result do? a hash of what the scan reads */
            static struct { uint32_t rp, h; } last[64];
            static uint32_t same, total;
            uint32_t h = 2166136261u;
            const uint32_t pages = (uint32_t)lp(RP + 0x130), np = (uint32_t)lp(RP + 0x138);
            for (uint32_t pg = 0; pg < np; pg++) {
                const uint32_t left = *(uint16_t *)GP(pages + 0x6660 + 2 * pg);
                if (!left)
                    continue;
                const uint32_t page = (uint32_t)lp(pages + 8 * pg);
                const uint32_t *a = GP(page), *b = GP(page + 0x4800), *f = GP(page + 0x6000);
                for (uint32_t j = 0; j < 2 * left; j++)
                    h = (h ^ a[j]) * 16777619u;
                for (uint32_t j = 0; j < 6 * left; j++)
                    h = (h ^ b[j]) * 16777619u;
                for (uint32_t j = 0; j < 2 * left; j++)
                    h = (h ^ f[j]) * 16777619u;
            }
            const float *cm = GP(lp(RP + 0x10));
            const uint32_t *cmu = (const uint32_t *)cm;
            for (int j = 0; j < 32; j++)
                h = (h ^ cmu[j]) * 16777619u;
            const uint32_t slot = ((uint32_t)RP >> 4) & 63;
            total++;
            if (last[slot].rp == (uint32_t)RP && last[slot].h == h)
                same++;
            last[slot].rp = (uint32_t)RP;
            last[slot].h = h;
            if (total == 600) {
                rt_log("[rpu] %u of 600 passes read the same as last time\n", same);
                same = total = 0;
            }
        }
        if (rpu.calls % 600 == 0) {
            ndist = nk;
            rt_log("[rpu] %u distinct (pages, camera) in 600 calls; pages %#x mask %#x layer %#x\n", ndist,
                   (uint32_t)lp(RP + 0x130), *(uint8_t *)GP(RP + 0x9), *(uint8_t *)GP(RP + 0x8));
            nk = 0;
        }
    }
    static int memo_off = -1;
    if (memo_off < 0)
        memo_off = getenv("MINA_NO_RPU_MEMO") != NULL;
    const uint32_t in_len = memo_off ? 0 : rpu_gather(gm_, RP);
    if (in_len && rpu_restore(gm_, RP, in_len)) {
        rpu_hits++;
        return;
    }
    rpu_misses++;
    const uint64_t lists_before = lp(RP + 0x1f0) ^ lp(RP + 0x218) * 3u ^ lp(RP + 0x148) * 7u;
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
        /* what the loop reads of the pass and the camera, in locals (with -fno-strict-aliasing
         * every store to guest memory made GCC load them again): read again after the calls
         * into the game (the list growing, a key function) */
        uint32_t layers = *(uint8_t *)GP(RP + 0x8), cap = (uint32_t)lp(RP + 0x148);
        uint32_t *keyl = GP(lp(RP + 0x1f0 + 8 * v));
        uint8_t *objl = GP(lp(RP + 0x218 + 8 * v));
        float cp0 = ((const float *)GP(fr))[0], cp1 = ((const float *)GP(fr))[1], cp2 = ((const float *)GP(fr))[2];
        float cd0 = ((const float *)GP(fr + 0x10))[0], cd1 = ((const float *)GP(fr + 0x10))[1],
              cd2 = ((const float *)GP(fr + 0x10))[2];
#define RPU_RELOAD()                                                                                  \
        do {                                                                                          \
            layers = *(uint8_t *)GP(RP + 0x8);                                                        \
            cap = (uint32_t)lp(RP + 0x148);                                                           \
            keyl = GP(lp(RP + 0x1f0 + 8 * v));                                                        \
            objl = GP(lp(RP + 0x218 + 8 * v));                                                        \
            cp0 = ((const float *)GP(fr))[0], cp1 = ((const float *)GP(fr))[1], cp2 = ((const float *)GP(fr))[2]; \
            cd0 = ((const float *)GP(fr + 0x10))[0], cd1 = ((const float *)GP(fr + 0x10))[1];                    \
            cd2 = ((const float *)GP(fr + 0x10))[2];                                                  \
        } while (0)
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
                if (!(layers & layer))
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
                if (n == cap) {
                    c->x[0] = RP;
                    c->x[1] = (uint64_t)n * 2;
                    f_1e0ca0(c);
                    f = *(uint8_t *)GP(fa);
                    RPU_RELOAD();
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
                        k = (box[0] - cp0) * cd0;
                        const float k1 = (box[1] - cp1) * cd1, k2 = (box[2] - cp2) * cd2;
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
                    RPU_RELOAD();
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
                memcpy((uint8_t *)keyl + 4 * (size_t)n, &key, 4);
                const uint64_t o64 = obj;
                memcpy(objl + 8 * (size_t)n, &o64, 8);
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
    /* kept unless a list grew during the scan (the inputs gathered before no longer hold) */
    if (in_len && (lp(RP + 0x1f0) ^ lp(RP + 0x218) * 3u ^ lp(RP + 0x148) * 7u) == lists_before)
        rpu_save(gm_, RP, in_len);
    if (stats && (rpu_hits + rpu_misses) >= 6000) {
        rt_log("[rpu] memo: %u of %u passes from last time\n", rpu_hits, rpu_hits + rpu_misses);
        rpu_hits = rpu_misses = 0;
    }
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
            rt_log("[verify] RenderPass::Update differs (%s, first byte %#x)\n", diff == 2 ? "lists" : "pass", (unsigned)i);
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
    CALLS(1);
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
    CALLS(2);
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

/* ---- ycCutterAnimComponent::UpdateRenderFrame: its indices checked ------------------------------
 * The render frame is the anim's (O+0x40) frame (+0x28, 0x58 each, +0x18 of them) that the
 * current animation's (data +0x10: list +0x30, 0x38 each, count +0x28 >> 34) track (+0x20,
 * 0x18 each, count +0x18 >> 34) names at the frame index (O+0x158). The console crashed in a
 * long play (ycCutterAnimFrame::InitTexture on a frame 0x5_8e8d4b20: an index far out of its
 * anim's frames, on the game thread). An index or pointer out of range keeps the frame
 * shown (logged, with the component) instead. */
void f_d3d2c_orig(Ctx *c);
void nat__ZN21ycCutterAnimComponent17UpdateRenderFrameEv(Ctx *c)
{
    CALLS(3);
    GM_BEGIN;
    const uint64_t O = c->x[0];
    const uint32_t an = rt_ld32(GP(O + 0x17c));
    if ((int32_t)an < 0) {
        f_d3d2c_orig(c);
        return;
    }
    const uint64_t anim = lp(O + 0x40);
    const char *why = NULL;
    uint64_t nan = 0, nfr = 0, nf = 0;
    uint32_t fi = rt_ld32(GP(O + 0x158)), f = 0;
    uint64_t data = 0, lst = 0, A = 0, trk = 0, frames = 0;
    if (!guest_ptr_ok((uint32_t)anim) || !guest_ptr_ok((uint32_t)(data = lp(anim + 0x10))))
        why = "anim";
    else if (an >= (nan = rt_ld64(GP(data + 0x28)) >> 34) || !guest_ptr_ok((uint32_t)(lst = lp(data + 0x30))))
        why = "animation";
    else if (A = lst + 0x38ull * an, fi >= (nfr = rt_ld64(GP(A + 0x18)) >> 34) || !guest_ptr_ok((uint32_t)(trk = lp(A + 0x20))))
        why = "frame index";
    else if (f = rt_ld32(GP(trk + 0x18ull * fi)), f >= (nf = rt_ld32(GP(anim + 0x18))) ||
             !guest_ptr_ok((uint32_t)(frames = lp(anim + 0x28))))
        why = "frame";
    if (!why) {
        /* the usual case natively: the frame's texture set up (InitTexture returns at once on
         * its flag 4) and the same frame and texture as shown (the original's early return) */
        const uint64_t F = frames + 0x58ull * f;
        if ((rt_ld32(GP(F + 0x40)) & 4) && lp(O + 0x48) == F && rt_ld64(GP(O + 0x110)) == rt_ld64(GP(F + 0x10))) {
            c->x[0] = O;
            CALLS(4);
            return;
        }
        f_d3d2c_orig(c);
        return;
    }
    static int told;
    if (told++ < 20)
        rt_log("[anim] component %#llx (vtable %#x, entity %#x): %s out of range (anim %#llx, animation %u of %llu, "
               "index %u of %llu, frame %u of %llu); frame kept\n",
               (unsigned long long)O, rt_ld32(GP(O)), rt_ld32(GP(O + 0x10)), why, (unsigned long long)anim, an,
               (unsigned long long)nan, fi, (unsigned long long)nfr, f, (unsigned long long)nf);
    c->x[0] = O;
}

/* ---- CombatShape::UpdateShapes(dt, names, n): its "nothing changed" return natively ----------
 * Most calls find the owner's anim (S+0x58) at the same position, animation, frame, scale as
 * the last time (and no rotation to compare, S+0x40 clear) and nothing moved: the timers
 * (+0x44, +0x48) advance, the motion (+0x90..0x98) is stored, the bounds' offset (+0xc0, +0xc8)
 * cleared, 0 returned. That decided from reads only; anything else runs the original untouched.
 * The arithmetic in the original's order (fp-contract off). MINA_NO_NATIVES=ShapesFast. */
void f_987cac_orig(Ctx *c);
static int shapes_fast(Ctx *c);
void nat__ZN11CombatShape12UpdateShapesEfPPKcj(Ctx *c)
{
    CALLS(5);
    static int verify = -1;
    if (verify < 0)
        verify = getenv("MINA_VERIFY_SHAPES") != NULL;
    if (!verify) {
        if (!shapes_fast(c))
            f_987cac_orig(c);
        else
            CALLS(6);
        return;
    }
    /* MINA_VERIFY_SHAPES=1: a fast return checked against the original on the same object */
    GM_BEGIN;
    static uint32_t checked, bad;
    static uint8_t s0[0x100], s1[0x100];
    const uint64_t S = c->x[0];
    memcpy(s0, GP(S), sizeof s0);
    const Ctx save = *c;
    if (!shapes_fast(c)) {
        f_987cac_orig(c);
        return;
    }
    const uint64_t mine = c->x[0];
    memcpy(s1, GP(S), sizeof s1);
    memcpy(GP(S), s0, sizeof s0);
    *c = save;
    f_987cac_orig(c);
    checked++;
    if ((memcmp(s1, GP(S), sizeof s1) || mine != c->x[0]) && bad++ < 10) {
        unsigned i = 0;
        while (i < sizeof s1 && s1[i] == ((uint8_t *)GP(S))[i]) i++;
        rt_log("[verify] UpdateShapes differs at +%#x: %02x / %02x, returns %llu / %llu\n", i, i < sizeof s1 ? s1[i] : 0,
               i < sizeof s1 ? ((uint8_t *)GP(S))[i] : 0, (unsigned long long)mine, (unsigned long long)c->x[0]);
    }
    if ((checked & 0xfff) == 0)
        rt_log("[verify] UpdateShapes: %u fast returns checked, %u differ\n", checked, bad);
}
#define LB8(a) (*(uint8_t *)GP(a))
static int shapes_fast(Ctx *c)
{
    static int off = -1;
    if (off < 0)
        off = rt_native_off("ShapesFast");
    if (off)
        return 0;
    GM_BEGIN;
    const uint64_t S = c->x[0];
    const float dt = c->v[0].f[0];
    const uint32_t st = rt_ld32(GP(S + 0xb4));
    if (st & 8) {
        stfp(S + 0x44, ldfp(S + 0x44) + dt);
        stfp(S + 0x48, ldfp(S + 0x48) + dt);
        rt_st64(GP(S + 0xc0), 0);
        rt_st32(GP(S + 0xc8), 0);
        c->x[0] = 0;
        return 1;
    }
    const uint64_t A = lp(S + 0x58);
    if (((st >> 3) & 2) || !A || rt_ld64(GP(S + 0x60)) != A || LB8(S + 0xbd) || LB8(S + 0x40))
        goto orig;
    {
        const uint64_t E = lp(S + 0x38);
        if (LB8(E + 0x254) || rt_ld32(GP(S + 0x68)) != rt_ld32(GP(A + 0x17c)) ||
            rt_ld32(GP(S + 0xb0)) != rt_ld32(GP(A + 0x158)) || !(ldfp(S + 0x9c) == ldfp(A + 0x190)) ||
            !(ldfp(S + 0xa0) == ldfp(A + 0x194)) || !(ldfp(S + 0xa4) == ldfp(A + 0x198)))
            goto orig;
        float s1 = ldfp(A + 0x25c), s2 = ldfp(A + 0x260);
        const float s0 = ldfp(A + 0x264);
        const uint64_t P = rt_ld64(GP(A + 0x200));
        if (P) {
            const uint64_t p = (uint32_t)P;
            const float s3 = ldfp(p + 0x94) - ldfp(p + 0xa0);
            const float d0 = ldfp(p + 0x98) - ldfp(p + 0xa4), d1 = ldfp(p + 0x9c) - ldfp(p + 0xa8);
            s1 = s1 - s3;
            s2 = s2 - d0;
            s2 = s2 - d1;
        }
        const float s8 = s1 + ldfp(S + 0xd8);
        const float s9 = s0 + 0.0f;
        const float s10 = (s2 + ldfp(S + 0xdc)) + ldfp(S + 0xe0);
        const float s12 = s8 - ldfp(S + 0x6c), s11 = s10 - ldfp(S + 0x70), m = s9 - ldfp(S + 0x74);
        if (!(s12 == 0.0f) || !(s11 == 0.0f))
            goto orig;              /* moved */
        int stop = 0;
        if (LB8(S + 0xbf)) {
            const float lim = ldfp(S + 0xe4);
            float a = s12 - (ldfp(S + 0xc0) + ldfp(S + 0xcc));
            a = a > 0.0f ? a : -a;
            if (a > lim)
                stop = 1;
            else {
                float b = s11 - (ldfp(S + 0xc4) + ldfp(S + 0xd0));
                b = b > 0.0f ? b : -b;
                stop = !(b <= lim);
            }
        }
        stfp(S + 0x44, ldfp(S + 0x44) + dt);
        stfp(S + 0x48, ldfp(S + 0x48) + dt);
        rt_st32(GP(S + 0xb4), 0);
        stfp(S + 0x90, s12);
        stfp(S + 0x94, s11);
        stfp(S + 0x98, m);
        if (stop)
            rt_st32(GP(S + 0x48), 0);
        rt_st64(GP(S + 0xc0), 0);
        rt_st32(GP(S + 0xc8), 0);
        c->x[0] = 0;
        return 1;
    }
orig:
    return 0;
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

/* ---- GameAnim::Update / PostUpdate (ycUpdateQueueContext): the update queue's most called
 * callbacks (~700 GameAnims each per update in hub_overworld_south), natively: the facing, the
 * frame and the render frame through their natives' early outs, the world transform (vtable
 * +0x80) when flagged (+0x1e4 bit 5, or bit 4 with mode 4 at +0x1dc), then bit 5 cleared.
 * PostUpdate taking itself off its queue (bit 27 without 26) runs the game's code.
 * MINA_NO_NATIVES=GameAnimUQ. */
void f_9a7af0_orig(Ctx *c);
void f_9a7b04_orig(Ctx *c);
static int gameanim_uq_off = -1;
static inline void gameanim_xform(Ctx *c, uint8_t *gm_, uint64_t O)
{
    uint32_t f = rt_ld32(GP(O + 0x1e4));
    if ((f & 0x20) || ((f & 0x10) && rt_ld32(GP(O + 0x1dc)) == 4)) {
        c->x[0] = O;
        static const GuestFunc *ic;
        RT_CALL_IC(c, rt_ld64(GP(lp(O) + 0x80)), ic);
        f = rt_ld32(GP(O + 0x1e4));
    }
    rt_st32(GP(O + 0x1e4), f & ~0x20u);
}
void nat__ZN8GameAnim6UpdateEP20ycUpdateQueueContext(Ctx *c)
{
    if (gameanim_uq_off < 0)
        gameanim_uq_off = rt_native_off("GameAnimUQ");
    if (gameanim_uq_off) {
        f_9a7af0_orig(c);
        return;
    }
    GM_BEGIN;
    const uint64_t O = c->x[0];
    const float dt = ldfp(c->x[1] + 8) * ldfp(lp(O + 0x10) + 0x68);
    nat__ZN8GameAnim12UpdateFacingEv(c);
    if ((int32_t)rt_ld32(GP(O + 0x17c)) < 0) {
        rt_st32(GP(O + 0x124), 0);
    } else {
        if (!*(uint8_t *)GP(O + 0x1c9)) {
            c->x[0] = O;
            c->x[1] = 1;
            c->v[0] = vset_f32(dt);
            nat__ZN21ycCutterAnimComponent11UpdateFrameEfb(c);
        }
        c->x[0] = O;
        nat__ZN21ycCutterAnimComponent17UpdateRenderFrameEv(c);
        if (*(uint8_t *)GP(O + 0x1c9)) {
            c->x[0] = O;
            c->x[1] = 0;
            c->v[0] = vset_f32(dt);
            nat__ZN21ycCutterAnimComponent11UpdateFrameEfb(c);
        }
    }
    if (rt_ld64(GP(O + 0x3d8)) || (rt_ld32(GP(O + 0x1e4)) & (1u << 26)))
        return;
    gameanim_xform(c, gm_, O);
}
void nat__ZN8GameAnim10PostUpdateEP20ycUpdateQueueContext(Ctx *c)
{
    if (gameanim_uq_off < 0)
        gameanim_uq_off = rt_native_off("GameAnimUQ");
    GM_BEGIN;
    const uint64_t O = c->x[0];
    const uint32_t f = rt_ld32(GP(O + 0x1e4));
    if (gameanim_uq_off || ((f & (1u << 27)) && !(f & (1u << 26)))) {
        f_9a7b04_orig(c);
        return;
    }
    gameanim_xform(c, gm_, O);
}

/* ---- CombatCore::Update (ycUpdateQueueContext), ~240 a world update: the timers (+0x268..0x288
 * counted down while positive; the list at +0x108, count +0x1c8, entries dropped when done), then
 * its defense (+0xd8) and attack (+0xd0) shapes' UpdateShapes (natives first: a "nothing changed"
 * 0 skips the box comparison) and, when a box moved by 1e-6 or more, the core's data again;
 * UpdateDamageDesc with an attack shape. The game's order and arithmetic. MINA_NO_NATIVES=CoreUQ. */
void f_981fc4_orig(Ctx *c);
void f_982398(Ctx *c);          /* CombatCore::UpdateCoreDefenseData */
void f_98245c(Ctx *c);          /* CombatCore::UpdateCoreAttackData */
void f_982588(Ctx *c);          /* CombatCore::UpdateDamageDesc */
static int core_shape(Ctx *c, uint8_t *gm_, uint64_t O, uint32_t so, uint32_t names, uint32_t n, float dt)
{
    const uint64_t sh = lp(O + so);
    float old[6];
    memcpy(old, GP(sh + 0x14), sizeof old);
    c->x[0] = sh;
    c->x[1] = rt_ld64(GP(O + names));
    c->x[2] = rt_ld32(GP(O + n));
    c->v[0] = vset_f32(dt);
    nat__ZN11CombatShape12UpdateShapesEfPPKcj(c);
    if (!(uint32_t)c->x[0])
        return 0;
    const float eps = hexf(0x358637bdu);
    const uint64_t sh2 = lp(O + so);      /* (read again, as the game does) */
    for (int k = 0; k < 6; k++) {
        float d = ldfp(sh2 + 0x14 + 4 * k) - old[k];
        d = d > 0.0f ? d : -d;
        if (!(d < eps))
            return 1;
    }
    return 0;
}
static void core_update(Ctx *c);
void nat__ZN10CombatCore6UpdateEP20ycUpdateQueueContext(Ctx *c)
{
    static int off = -1, verify = -1;
    if (off < 0) {
        off = rt_native_off("CoreUQ");
        verify = getenv("MINA_VERIFY_CORE") != NULL;
    }
    if (off) {
        f_981fc4_orig(c);
        return;
    }
    if (!verify) {
        core_update(c);
        return;
    }
    /* MINA_VERIFY_CORE=1: the core and its two shapes after this one and after the game's code
     * (a pointer set from 0 at the attack shape's +0x100 differs: each run allocates its block) */
    GM_BEGIN;
    static uint32_t checked, bad;
    static uint8_t b[3][0x300], m[3][0x300];
    const uint64_t O = c->x[0], ob[3] = { O, lp(O + 0xd8), lp(O + 0xd0) };
    for (int k = 0; k < 3; k++)
        if (ob[k])
            memcpy(b[k], GP(ob[k]), 0x300);
    const Ctx save = *c;
    core_update(c);
    for (int k = 0; k < 3; k++)
        if (ob[k])
            memcpy(m[k], GP(ob[k]), 0x300), memcpy(GP(ob[k]), b[k], 0x300);
    *c = save;
    f_981fc4_orig(c);
    checked++;
    for (int k = 0; k < 3; k++)
        if (ob[k] && memcmp(m[k], GP(ob[k]), 0x300) && bad++ < 10) {
            unsigned i = 0;
            while (m[k][i] == ((uint8_t *)GP(ob[k]))[i]) i++;
            {
                uint64_t a, g;
                memcpy(&a, m[k] + (i & ~7u), 8);
                memcpy(&g, GP(ob[k] + (i & ~7u)), 8);
                rt_log("[verify] CombatCore::Update differs: object %d at +%#x: mine %#llx, game %#llx (was %#llx)\n", k, i,
                       (unsigned long long)a, (unsigned long long)g,
                       (unsigned long long)({ uint64_t w_; memcpy(&w_, b[k] + (i & ~7u), 8); w_; }));
            }
        }
    if ((checked & 0xfff) == 0)
        rt_log("[verify] CombatCore::Update: %u checked, %u differ\n", checked, bad);
}
static void core_update(Ctx *c)
{
    GM_BEGIN;
    const uint64_t O = c->x[0];
    const float dt = ldfp(c->x[1] + 8) * ldfp(lp(O + 0x10) + 0x68);
    stfp(O + 0x1e4, ldfp(O + 0x1e0));
    for (uint32_t o = 0x268; o <= 0x288; o += 4) {
        const float v = ldfp(O + o);
        if (v > 0.0f)
            stfp(O + o, v - dt);
    }
    for (uint32_t i = 0; i < rt_ld32(GP(O + 0x1c8)); i++) {
        const uint64_t e = O + 0x108 + 8ull * i;
        const float v = ldfp(e) - dt;
        stfp(e, v);
        if (v > 0.0f)
            continue;
        const uint32_t n = rt_ld32(GP(O + 0x1c8));
        rt_st64(GP(e), rt_ld64(GP(O + 0x108 + 8ull * (n - 1))));
        rt_st32(GP(O + 0x1c8), n - 1);
        i--;
    }
    const uint64_t D = lp(O + 0xd8);
    if (D && core_shape(c, gm_, O, 0xd8, 0xc0, 0xcc, dt)) {
        c->x[0] = O;
        f_982398(c);
    }
    const uint64_t A = lp(O + 0xd0);
    if (!A)
        return;
    if (core_shape(c, gm_, O, 0xd0, 0xb8, 0xc8, dt)) {
        c->x[0] = O;
        f_98245c(c);
    }
    c->x[0] = O;
    f_982588(c);
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
     * after up to 2 steps. The Vita starts at 30 and adapts (rate_frame);
     * MINA_TARGET_FPS=n fixes another rate, 0 the game's. */
    const char *t = getenv("MINA_TARGET_FPS");
#if defined(__vita__)
    g_rate_auto = !t || strcmp(t, "auto") == 0;
    const uint32_t fps = t && !g_rate_auto ? (uint32_t)atoi(t) : 30;
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
    {
        extern uint32_t g_game_tid;
        g_game_tid = c->thread_id;
    }
    {
        /* The frame semaphore (signalled when the renderer commits a frame,
         * waited on by this loop before each one) starts at 1 on the Vita: the
         * game builds its next frame while the renderer draws the last one,
         * instead of game, renderer, game... in turn (the deck: 54-58 fps,
         * frames with a world step 28 ms; 60.0, worst 21 ms). The command lists
         * come from a pool; uploads to textures still drawn may show a frame
         * early. MINA_FRAME_SLACK=n: n frames (0: the game's lockstep) */
        const char *v = getenv("MINA_FRAME_SLACK");
#if defined(__vita__)
        const int n = v ? atoi(v) : 1;
#else
        const int n = v ? atoi(v) : 0;
#endif
        void rt_dispatch_semaphore_signal(uint64_t sem);
        const uint64_t sem = rt_ld64(G(0x6033bf8));
        for (int i = 0; i < n && sem; i++)
            rt_dispatch_semaphore_signal(sem);
        if (n)
            rt_log("[thread] frame semaphore %#llx starts at %d\n", (unsigned long long)sem, n);
    }
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

/* ProcessAttackList's defenders: those of B's pools with flag 2 and a shape, their boxes as
 * the loop computes them (n -1: too many, the loop reads them itself) */
typedef struct { float xlo, xhi, ylo, yhi; } DefBox;
#define DCACHE_MAX 2048
static DefBox dcache[DCACHE_MAX];
static int dcache_n = -1, dcache_dirty = 1;
static void dcache_build(uint8_t *gm_, uint64_t T, uint64_t B)
{
    dcache_dirty = 0;
    dcache_n = 0;
    const uint64_t bcnt = L64(B + 8);
    if (!bcnt || !L32(T + 0xe4))
        return;
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
                        if (dcache_n == DCACHE_MAX) {
                            dcache_n = -1;
                            return;
                        }
                        DefBox *d = &dcache[dcache_n++];
                        {
                            const float t = ldfp(D + 8), h = ldfp(D);
                            d->xlo = t - h;
                            d->xhi = t + h;
                        }
                        {
                            const float h = ldfp(D + 4), t = ldfp(D + 0xc);
                            d->ylo = t - h;
                            d->yhi = h + t;
                        }
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
static void process_attack_list(Ctx *c, int dry)
{
    GM_BEGIN;
    dcache_dirty = 1;
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
                        /* the defenders once (their flags and boxes), until a hit runs game code:
                         * an attacker that touches none of them does nothing in the game's loop */
                        if (dcache_dirty)
                            dcache_build(gm_, T, B);
                        if (dcache_n >= 0) {
                            int any = 0;
                            for (int q = 0; q < dcache_n && !any; q++) {
                                const DefBox *d = &dcache[q];
                                any = !(s8 > d->xhi || d->xlo > s9) && !(s11 > d->yhi || d->ylo > s10);
                            }
                            if (!any)
                                continue;
                            dcache_dirty = 1;
                        }
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
    CALLS(7);
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
extern uint32_t g_stage_made, g_stage_kept, g_stage_gone;
void nat__ZN9ycTexture15AllocStagingBufEj(Ctx *c)
{
    GM_BEGIN;
    if (getenv("MINA_STAGE_STATS") && (__atomic_add_fetch(&g_stage_made, 1, __ATOMIC_RELAXED) % 600) == 0)
        rt_log("[stage] records made %u, kept for reuse %u, let go %u\n", g_stage_made, g_stage_kept, g_stage_gone);
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
/* the first i < n with a[i] == x (n when none): 8 colors a step with NEON (the
 * byte-wise compares of the recompiled code, then a word loop, were ~19% of the
 * game thread on hub_overworld_south: palettes of ~85 colors, found after ~40) */
static inline uint32_t find_u32(const uint8_t *a, uint32_t n, uint32_t x)
{
    uint32_t i = 0;
    const uint32x4_t vx = vdupq_n_u32(x);
    for (; i + 8 <= n; i += 8) {
        const uint32x4_t e0 = vceqq_u32(vreinterpretq_u32_u8(vld1q_u8(a + 4 * i)), vx);
        const uint32x4_t e1 = vceqq_u32(vreinterpretq_u32_u8(vld1q_u8(a + 4 * i + 16)), vx);
        const uint32x4_t o = vorrq_u32(e0, e1);
        const uint32x2_t r = vorr_u32(vget_low_u32(o), vget_high_u32(o));
        if (vget_lane_u32(vpmax_u32(r, r), 0))
            break;
    }
    for (; i < n; i++) {
        uint32_t v;
        memcpy(&v, a + 4 * i, 4);
        if (v == x)
            return i;
    }
    return n;
}
/* Inside a ycPaletteComponent::Update (pal_scope) the palettes searched (a group's, a base) don't
 * change: only the component's own palette (pal_avoid, its colors) is written. There a palette
 * searched again and again (per color of an animation) gets a table of the first index of each
 * of its colors, built at its third search in the scope; outside, or for the component's own
 * palette, the scan. */
static int pal_scope;
static uint32_t pal_gen;
static uint32_t pal_avoid;
/* a table per palette (8, by its colors' address), kept across scopes with a copy of the colors
 * it was built from: the first search of a scope compares them (one memcmp), rebuilt if changed */
#define PAL_SLOTS 8
typedef struct { uint32_t cols, n, gen, used; uint32_t snap[256]; uint16_t tab[512]; } PalHash;
static PalHash pal_h[PAL_SLOTS];
static uint32_t pal_use;
static inline uint32_t pal_hash(uint32_t x) { return (x * 2654435761u) >> 23; }
static PalHash *pal_table(uint8_t *gm_, uint32_t cols, uint32_t n);
static inline uint32_t pal_lookup(const PalHash *h, uint32_t x)
{
    uint32_t i = pal_hash(x);
    while (h->tab[i] != 0xffff) {
        if (h->snap[h->tab[i]] == x)
            return h->tab[i];
        i = (i + 1) & 511;
    }
    return h->n;
}
static uint32_t find_first(uint8_t *gm_, uint32_t cols, uint32_t n, uint32_t x)
{
    PalHash *h = pal_table(gm_, cols, n);
    return h ? pal_lookup(h, x) : find_u32(GP(cols), n, x);
}
/* the table of a palette's colors in this scope, or null (out of a scope, small or big palettes,
 * the component's own) */
static PalHash *pal_table(uint8_t *gm_, uint32_t cols, uint32_t n)
{
    const uint8_t *a = GP(cols);
    if (!pal_scope || n < 16 || n > 256 || cols == pal_avoid)
        return NULL;
    int k = 0;
    while (k < PAL_SLOTS && !(pal_h[k].cols == cols && pal_h[k].n == n))
        k++;
    PalHash *h;
    if (k < PAL_SLOTS) {
        h = &pal_h[k];
        if (h->gen != pal_gen) {
            if (memcmp(h->snap, a, n * 4))
                h->cols = 0;          /* changed: rebuilt below */
            else
                h->gen = pal_gen;
        }
    } else {
        int lru = 0;
        for (int j = 1; j < PAL_SLOTS; j++)
            if (pal_h[j].used < pal_h[lru].used)
                lru = j;
        h = &pal_h[lru];
        h->cols = 0;
    }
    if (!h->cols) {
        h->cols = cols;
        h->n = n;
        h->gen = pal_gen;
        memcpy(h->snap, a, n * 4);
        memset(h->tab, 0xff, sizeof h->tab);
        for (uint32_t j = 0; j < n; j++) {
            const uint32_t v = h->snap[j];
            uint32_t i = pal_hash(v);
            while (h->tab[i] != 0xffff && h->snap[h->tab[i]] != v)
                i = (i + 1) & 511;
            if (h->tab[i] == 0xffff)
                h->tab[i] = (uint16_t)j;
        }
    }
    h->used = ++pal_use;
    return h;
}
static uint32_t pal_find_from_base(uint8_t *gm_, uint64_t P, uint64_t color, uint64_t B)
{
    uint32_t n, cols, bn, bcols;
    pal_colors(gm_, P, &n, &cols);
    pal_colors(gm_, B, &bn, &bcols);
    const uint32_t want = L32(color);
    const uint32_t i = find_first(gm_, bcols, bn, want);
    if (i == bn)
        return want;
    if (i < n)
        return L32(cols + 4 * i);
    /* past this palette's end: wrapped into 1..n-1 (udiv by 0 gives 0 on arm64) */
    const uint32_t m = n - 1;
    const uint32_t q = m ? i / m : 0;
    return L32(cols + 4 * (i - q * m + 1));
}
static uint32_t pal_get_index(uint8_t *gm_, uint64_t P, uint32_t color, uint32_t fuzzy)
{
    uint32_t n, cols;
    pal_colors(gm_, P, &n, &cols);
    const uint8_t *pc = GP(cols);
    const uint32_t i = find_first(gm_, cols, n, color);
    if (i < n)
        return i;
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
    {
        static CallerHist h = { .what = "ycPaletteTexture::FindFromBase" };
        caller_count(&h, __builtin_return_address(0));
    }
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
    CALLS(10);
    GM_BEGIN;
    {
        static CallerHist h = { .what = "ycPaletteTexture::GetIndex" };
        caller_count(&h, __builtin_return_address(0));
    }
    const uint32_t r = pal_get_index(gm_, c->x[0], (uint32_t)c->x[1], (uint32_t)c->x[2]);
    if (pal_check < 0)
        pal_check = getenv("MINA_PAL_CHECK") != NULL;
    if (pal_check) {
        f_1b4758_orig(c);
        pal_report("GetIndex", r, (uint32_t)c->x[0]);
    }
    c->x[0] = r;
}

/* ---- ycUpdateQueue::Update: MINA_UQ_STATS=1 times each update callback (by function), the
 * game's loop redone here; otherwise the original runs. The queue's pages (+0x28, linked by
 * +0x8, count +0x10) hold items of 0x28 bytes from +0x18: object, member function pointer
 * (address or vtable offset), adjustment (bit 0: virtual), removed. The callbacks get the
 * context the loop keeps in its frame (queue +0x38); removals they ask for are done after. */
void f_1e5c84_orig(Ctx *c);
extern uint64_t g_zone_us[8];
static inline int zones_on(void);
void f_1e5f8c(Ctx *c);
void f_1e5e34(Ctx *c);
typedef struct { uint32_t fn; uint32_t calls; uint64_t us; } UqStat;
static int uq_depth;
static UqStat uq_stat[256];
static uint32_t uq_runs;
static uint64_t uq_total_us;
static void uq_note(uint32_t fn, uint64_t us)
{
    fn |= (uint32_t)(uq_depth > 3 ? 3 : uq_depth) << 30;      /* the nesting level in the top bits */
    uint32_t h = (fn >> 2) * 2654435761u >> 24;
    for (int k = 0; k < 256; k++, h = (h + 1) & 255) {
        UqStat *s = &uq_stat[h];
        if (s->fn == fn || !s->fn) {
            s->fn = fn;
            s->calls++;
            s->us += us;
            return;
        }
    }
}
static void uq_report(void)
{
    rt_log("[uq] %u outer updates, %.2f ms each; by callback (nesting level, ms and calls per outer update):\n",
           uq_runs, uq_total_us / 1000.0 / uq_runs);
    for (int r = 0; r < 30; r++) {
        int best = -1;
        for (int k = 0; k < 256; k++)
            if (uq_stat[k].fn && (best < 0 || uq_stat[k].us > uq_stat[best].us))
                best = k;
        if (best < 0 || !uq_stat[best].us)
            break;
        rt_log("[uq]   %u 0x1%08x  %6.3f ms  %6.1f\n", uq_stat[best].fn >> 30, (uq_stat[best].fn & 0x3fffffffu) - 0x1000000u,
               uq_stat[best].us / 1000.0 / uq_runs, (double)uq_stat[best].calls / uq_runs);
        uq_stat[best].us = 0;
    }
    memset(uq_stat, 0, sizeof uq_stat);
    uq_runs = 0;
    uq_total_us = 0;
}
void nat__ZN8GameAnim6UpdateEP20ycUpdateQueueContext(Ctx *c);
void nat__ZN8GameAnim10PostUpdateEP20ycUpdateQueueContext(Ctx *c);
void nat__ZN10CombatCore6UpdateEP20ycUpdateQueueContext(Ctx *c);
void nat__ZN13ycUpdateQueue6UpdateEf(Ctx *c)
{
    static int on = -1;
    if (on < 0)
        on = getenv("MINA_UQ_STATS") != NULL;
    static int orig = -1;
    if (orig < 0)
        orig = rt_native_off("__ZN13ycUpdateQueue6UpdateEf");
    if (!on) {
        static int zdepth;
        if (zones_on() && !zdepth) {
            zdepth = 1;
            const uint64_t t = now_us();
            f_1e5c84_orig(c);
            g_zone_us[5] += now_us() - t;
            zdepth = 0;
            return;
        }
        if (orig) {
            f_1e5c84_orig(c);
            return;
        }
    }
    GM_BEGIN;
    const uint64_t t0 = on ? now_us() : 0;
    const uint64_t Q = c->x[0];
    float s8;
    memcpy(&s8, &c->v[0], 4);
    const uint64_t saved_sp = c->sp, F = saved_sp - 0xc0;
    c->sp = F;
    uint8_t *f = GP(F);
    memset(f + 0x10, 0, 0x30);
    const uint64_t g = L64(0x6053520);
    memset(f + 0x40, 0, 16);
    memset(f + 0x50, 0, 8);
    memcpy(f + 0x58, &g, 8);
    memset(f + 0x60, 0, 8);
    memcpy(f + 0x68, &g, 8);
    int stored = 1;
    if (g) {
        c->x[0] = g;
        rt_call(c, L64(L64(g) + 0x28));
        if ((uint32_t)c->x[0]) {
            c->x[0] = g;
            c->x[1] = g;
            rt_call(c, L64(L64(g) + 0x38));
            stored = 0;
        }
    }
    if (stored)
        memcpy(f + 0x58, &g, 8);
    f = GP(F);
    memcpy(f + 0x10, GP(0x1d4a0c0), 16);
    memcpy(f + 0x20, GP(0x1d4a0e0), 16);
    memcpy(f + 0x30, GP(0x1d4a0d0), 16);
    memset(f + 0x60, 0, 8);
    memcpy(f + 8, &s8, 4);
    memcpy(f, &Q, 8);
    memcpy(GP(Q + 0x38), &F, 8);
    for (uint64_t page = L64(Q + 0x28); page; page = L64(page + 8)) {
        for (uint64_t i = 0; i < L64(page + 0x10); i++) {
            const uint64_t it = page + 0x18 + 0x28 * i;
            if (L64(it + 0x18))
                continue;
            const uint64_t obj = L64(it), mp = L64(it + 8), adj = L64(it + 0x10);
            const uint64_t x0 = obj + (uint64_t)((int64_t)adj >> 1);
            const uint64_t fn = (adj & 1) ? L64(L64(x0) + (uint32_t)mp) : mp;
            c->x[0] = x0;
            c->x[1] = L64(Q + 0x38);
            c->x[8] = fn;
            c->sp = F;
            if (!on) {
                /* the callback through a cache of the last target per hash of its address */
                static const GuestFunc *ic[256];
                CALLS(11);
                /* the most called callbacks straight to their natives */
                if (fn == 0x19a7af0ull)
                    nat__ZN8GameAnim6UpdateEP20ycUpdateQueueContext(c);
                else if (fn == 0x19a7b04ull)
                    nat__ZN8GameAnim10PostUpdateEP20ycUpdateQueueContext(c);
                else if (fn == 0x1981fc4ull)
                    nat__ZN10CombatCore6UpdateEP20ycUpdateQueueContext(c);
                else
                    RT_CALL_IC(c, fn, ic[(uint32_t)fn >> 2 & 255]);
                continue;
            }
            const uint64_t t = now_us();
            uq_depth++;
            rt_call(c, fn);
            uq_depth--;
            uq_note((uint32_t)fn, now_us() - t);
        }
    }
    c->sp = F;
    for (uint64_t pi = 0, ii = 0;;) {
        if (pi >= L64(F + 0x60))
            break;
        const uint64_t pages = L64(F + 0x40);
        const uint64_t item = L64(pages + 8 * pi) + L64(F + 0x10) + L64(F + 0x18) * ii;
        c->x[0] = Q;
        c->x[1] = L64(item);
        c->x[2] = L64(item + 8);
        c->sp = F;
        f_1e5f8c(c);
        const uint64_t cnt = L64(L64(L64(F + 0x40) + 8 * pi));
        if (ii + 1 == cnt) {
            ii = 0;
            pi++;
        } else
            ii++;
    }
    memset(GP(Q + 0x38), 0, 8);
    c->x[0] = F + 0x10;
    c->sp = F;
    f_1e5e34(c);
    c->sp = saved_sp;
    if (on && uq_depth == 0) {
        uq_total_us += now_us() - t0;
        if (++uq_runs == 300)
            uq_report();
    }
}

/* ---- MINA_ZONES=1: time spent in a few of the game's functions (ms per frame in the [fps] lines) */
uint64_t g_zone_us[8];
const char *const g_zone_name[8] = { "PaletteComp", "LayerTransform", "TileRenderObj", "MessagePrompt",
                                     "HUDCamera", "UpdateQueue", "RefreshWorldTransform", "GameRender" };
int g_zones = -1;
static inline int zones_on(void)
{
    if (g_zones < 0)
        g_zones = getenv("MINA_ZONES") != NULL;
    return g_zones;
}
#define ZONE_NATIVE(nat, orig, k)                                                                        \
    void orig(Ctx *c);                                                                                    \
    void nat(Ctx *c)                                                                                      \
    {                                                                                                     \
        if (!zones_on()) {                                                                                \
            orig(c);                                                                                      \
            return;                                                                                       \
        }                                                                                                 \
        const uint64_t t = now_us();                                                                      \
        orig(c);                                                                                          \
        g_zone_us[k] += now_us() - t;                                                                     \
    }
void f_1b156c_orig(Ctx *c);
static int pm_try(Ctx *c, float dt);
static void pm_after(Ctx *c, uint64_t comp);
static void pm_verify(Ctx *c, uint64_t comp, const uint8_t *ents, uint64_t ne);
void nat__ZN18ycPaletteComponent6UpdateEf(Ctx *c)
{
    CALLS(8);
    static int skip = -1;               /* (measurement) */
    if (skip < 0)
        skip = getenv("MINA_SKIP_PAL") != NULL;
    if (skip)
        return;
    static int off = -1;
    if (off < 0)
        off = rt_native_off("PaletteScope");
    const uint64_t t = zones_on() ? now_us() : 0;
    const int was = pal_scope;
    const uint32_t avoid = pal_avoid;
    if (!off) {
        GM_BEGIN;
        pal_scope = 1;
        pal_gen++;
        pal_avoid = (uint32_t)lp(lp(c->x[0] + 0x88) + 0x18);
    }
    const uint64_t comp = c->x[0];
    {
        /* a step that would give the same palette: only the entries' times advance (pm_try) */
        static int verify = -1;
        if (verify < 0)
            verify = getenv("MINA_VERIFY_PALSKIP") != NULL;
        GM_BEGIN;
        const uint64_t x0 = c->x[0];
        const V128 v0 = c->v[0];
        static uint8_t ents[16 * 0x58];
        const uint64_t vec = lp(comp + 0x198), ne = rt_ld64(GP(comp + 0x1a0));
        if (verify && ne <= 16)
            memcpy(ents, GP(vec), ne * 0x58);
        const int skip = pm_try(c, v0.f[0]);
        c->x[0] = x0;
        c->v[0] = v0;
        if (skip) {
            CALLS(17);
            if (verify && ne <= 16)
                pm_verify(c, comp, ents, ne);
            pal_scope = was;
            pal_avoid = avoid;
            pal_gen++;
            return;
        }
    }
    static int calls_on = -1;
    if (calls_on < 0)
        g_calls_on = calls_on = getenv("MINA_CALLS") != NULL;
    uint64_t t0 = 0;
    int ne = 0;
    if (calls_on) {
        GM_BEGIN;
        ne = rt_ld64(GP(comp + 0x1a0)) == 0;
        if (!*(uint8_t *)GP(comp + 0xd0) || !*(uint8_t *)GP(comp + 0xd1))
            CALLS(16);
        if (ne)
            CALLS(13);
        t0 = now_us();
    }
    f_1b156c_orig(c);
    if (calls_on)
        g_calls[ne ? 14 : 15] += (uint32_t)(now_us() - t0);
    pm_after(c, comp);
    if (calls_on) {
        /* (MINA_CALLS) the component's palette after it: the same as the last time? */
        GM_BEGIN;
        const uint64_t P = lp(comp + 0x88);
        uint32_t h = 2166136261u;
        if (P) {
            const uint32_t n = rt_ld32(GP(P + 0x28)) & 0x3ff;
            const uint8_t *cols = GP(lp(P + 0x18)), *idx = GP(lp(P + 0x10));
            for (uint32_t i = 0; i < n * 4; i++)
                h = (h ^ cols[i]) * 16777619u;
            if (lp(P + 0x10))
                for (uint32_t i = 0; i < n; i++)
                    h = (h ^ idx[i]) * 16777619u;
            h ^= (uint32_t)rt_ld64(GP(P));
        }
        static uint32_t key[1024], val[1024];
        const uint32_t k = ((uint32_t)comp >> 3) * 2654435761u >> 22;
        if (key[k] == (uint32_t)comp && val[k] == h)
            CALLS(12);
        key[k] = (uint32_t)comp;
        val[k] = h;
    }
    pal_scope = was;
    pal_avoid = avoid;
    pal_gen++;
    if (t)
        g_zone_us[0] += now_us() - t;
}
ZONE_NATIVE(nat__ZN18TileLevelComponent20UpdateLayerTransformEj, f_7beff4_orig, 1)
ZONE_NATIVE(nat__ZN18TileLevelComponent27UpdateTileLevelRenderObjectEj, f_7bf3b8_orig, 2)
ZONE_NATIVE(nat__ZN20MessagePromptManager6UpdateEf, f_439148_orig, 3)
ZONE_NATIVE(nat__ZN9HUDCamera6UpdateEv, f_9b30f0_orig, 4)
ZONE_NATIVE(nat__ZN4Game6RenderEv, f_9a0b2c_orig, 7)

/* ---- TileLevelRenderObject::Render (x0 the object O, x1 the render context): a draw call
 * on the guest stack, a copy of the context's template (+0xb0, 0x220 bytes); its pipeline
 * from a table by a few flags (Jenkins' final mix, open addressing); 256 bytes of constants
 * from the command list's allocator: the object's matrix (translation snapped to whole
 * pixels), its color / 255, its depth range; the palette (SetOnDrawCall); then one draw per
 * index range (+0xd0/+0xd8, +0xe0/+0xe8). */
void f_1b5b10(Ctx *c);
void f_1df70c(Ctx *c);
void f_701d88_orig(Ctx *c);
static inline uint32_t rol32(uint32_t v, int r) { return v << r | v >> (32 - r); }
void nat__ZN21TileLevelRenderObject6RenderERKN14ycRenderObject9RenderCtxE(Ctx *c)
{
    GM_BEGIN;
    const uint64_t O = c->x[0], T = c->x[1] + 0xb0;
    if (!L8(O + 0xb4))
        return;
    {
        const uint64_t s = L64(O + 0x28);
        if (ldfp(s + 0xc) < 0.0f || ldfp(s + 0x10) < 0.0f || ldfp(s + 0x14) < 0.0f)
            return;
    }
    const uint64_t list = L64(c->x[1] + 0x10);
    const uint64_t saved_sp = c->sp, D = saved_sp - 0x270;
    memcpy(GP(D), GP(T), 0x220);
    /* the pipeline */
    {
        const uint64_t pal = L64(O + 0x98);
        const int lt = L64(0x60210c0) == 0 || (int32_t)L32(pal + 0x30) < 0;
        uint32_t key = lt ? 8 : 0x18;
        if (!(L8(O + 0xb0) == 0xff && L8(O + 0xb1) == 0xff && L8(O + 0xb2) == 0xff && L8(O + 0xb3) == 0xff))
            key |= 1;
        if (ldfp(O + 0xc0) < ldfp(O + 0xc4))
            key |= ldfp(O + 0xc8) > ldfp(O + 0xcc) ? 4 : 2;
        uint32_t a = 0xdeadc42cu, b = 0x8ef4c855u;
        uint32_t w11 = ((key + a) ^ b) + 0x59bd5389u;
        uint32_t w9 = w11 ^ a;
        uint32_t w12 = w9 - rol32(w11, 25);          /* ror 7 */
        uint32_t w10 = (w12 ^ b) - rol32(w12, 16);
        w11 = (w10 ^ w11) - rol32(w10, 4);           /* ror 28 */
        w12 = w11 ^ w12;
        w11 = w12 - rol32(w11, 14);                  /* ror 18 */
        w10 = (w11 ^ w10) - (w11 >> 8);
        const uint64_t tab = L64(0x60204b0);
        uint64_t h = w10, idx;
        int probes = 0;
        do {
            idx = h & 0xff;
            h++;
            if (++probes > 256) {
                /* not in the table: the original would spin here */
                static int told;
                if (told++ < 8)
                    rt_log("[tile] pipeline key %#x not in table %#llx (hash %#x, O %#llx, pal %#llx +0x30 %#x, "
                           "50210c0 %#llx, color %#x, c0..cc %f %f %f %f)\n", key, (unsigned long long)tab, w10,
                           (unsigned long long)O, (unsigned long long)pal, L32(pal + 0x30),
                           (unsigned long long)L64(0x60210c0), L32(O + 0xb0), ldfp(O + 0xc0), ldfp(O + 0xc4),
                           ldfp(O + 0xc8), ldfp(O + 0xcc));
                c->x[0] = O;
                c->x[1] = T - 0xb0;
                f_701d88_orig(c);
                return;
            }
        } while (L32(tab + idx * 8) != key);
        const uint32_t pi = L32(tab + idx * 8 + 4);
        const uint64_t pipe = L64(0x5ea1670 + 8 * (uint64_t)pi);
        memcpy(GP(D), &pipe, 8);
    }
    /* 256 bytes of constants */
    uint64_t al = L64(0x6039f88), blk = L64(al + 0x50), cur;
    if (blk && (blk - L64(blk + 8)) + 0x80000 > 0xff) {
        cur = L64(blk + 8);
    } else {
        const uint64_t heap = L64(0x6039f60);
        c->x[0] = heap;
        c->x[1] = 0x80000;
        c->x[2] = 0x100;
        c->x[3] = 0;
        c->x[4] = 0;
        c->x[5] = 0;
        c->sp = D;
        rt_call(c, L64(L64(heap)));
        blk = c->x[0];
        const uint64_t z = 0, end = blk + 0x100;
        memcpy(GP(blk), &z, 8);
        memcpy(GP(blk + 8), &end, 8);
        memcpy(GP(blk + 0x10), &z, 8);
        const uint64_t tail = L64(al + 0x50);
        memcpy(GP(tail ? tail : al + 0x48), &blk, 8);
        memcpy(GP(al + 0x50), &blk, 8);
        cur = L64(blk + 8);
        al = L64(0x6039f88);
    }
    {
        const uint64_t nx = cur + 0x100, blk2 = L64(al + 0x50);
        memcpy(GP(blk + 8), &nx, 8);
        memcpy(GP(D + 0x50), &blk2, 8);
        memcpy(GP(D + 0x58), &cur, 8);
        memcpy(GP(D + 0x60), GP(0x1d4e160), 8);
    }
    {
        float *k = GP(cur);
        memcpy(k, GP(L64(O + 8)), 64);
        const float s0 = ldfp(0x6039f5c), s1 = ldfp(0x6039f58);
        k[12] = floorf(k[12] * s0 + 0.5f) * s1;
        k[13] = floorf(k[13] * s0 + 0.5f) * s1;
        for (int i = 0; i < 4; i++)
            k[16 + i] = (float)L8(O + 0xb0 + i) / 255.0f;
        memset(&k[20], 0, 16);
        const float c0 = ldfp(O + 0xc0), c4 = ldfp(O + 0xc4);
        if (c0 < c4) {
            k[28] = c0;
            k[29] = c4;
            const float c8 = ldfp(O + 0xc8), cc = ldfp(O + 0xcc);
            k[30] = c8 < cc ? c8 : cc;
            k[31] = c8 > cc ? c8 : cc;
        }
    }
    c->x[0] = L64(O + 0x98);
    c->x[1] = D;
    c->x[2] = cur + 0x60;
    c->sp = D;
    f_1b5b10(c);
    uint8_t *d = GP(D);
    d[0x1e9] = 0;
    {
        const uint64_t z = 0, a8 = L64(0x603a548), a9 = L64(O + 0xa8), a28 = L64(0x604dc38);
        memcpy(d + 0x18, &z, 8);
        memcpy(d + 0x20, &a8, 8);
        memcpy(d + 0x8, &a9, 8);
        memcpy(d + 0x10, &z, 8);
        memcpy(d + 0x28, &a28, 8);
    }
    const uint8_t stencil = L8(O + 0xbc);
    if (stencil) {
        const uint16_t two = 2, ff = 0xffff;
        d[0x206] = 1;
        memcpy(d + 0x1fe, &two, 2);
        memcpy(d + 0x202, &two, 2);
        memcpy(d + 0x1f9, &ff, 2);
        d[0x1fb] = stencil;
    }
    for (int r = 0; r < 2; r++) {
        const uint64_t base = O + (r ? 0xe0 : 0xd0);
        const uint32_t cnt = *(uint16_t *)GP(base + 8);
        if (!cnt)
            break;
        d = GP(D);
        const uint32_t w200 = 0x200, one = 1, first = *(uint16_t *)GP(base + 0xa);
        const uint64_t idxbuf = L64(base);
        const uint16_t h1 = 1;
        memcpy(d + 0xf8, &w200, 4);
        memcpy(d + 0x100, &idxbuf, 8);
        d[0x1b8] = L8(O + 0xbd) ^ 1;
        memcpy(d + 0x1b9, &h1, 2);
        d[0x1bb] = 0x80;
        memcpy(d + 0x210, &first, 4);
        memcpy(d + 0x214, &cnt, 4);
        memcpy(d + 0x218, &one, 4);
        uint32_t fl;
        memcpy(&fl, d + 0x21c, 4);
        fl |= 1;
        memcpy(d + 0x21c, &fl, 4);
        c->x[0] = list;
        c->x[1] = D;
        c->sp = D;
        f_1df70c(c);
    }
    c->sp = saved_sp;
}

/* ---- ycSysAllocator::Alloc / Free ------------------------------------------------------------
 * ycCutterAnimRenderObject::Render and ycDrawUtil::Render take 512 KiB of scratch
 * (align 256) from the system allocator for each render and give it back: ~25 such
 * blocks a second through newlib's heap, which cut its free space into thousands of
 * pieces until a 1.25 MiB block could not be found (Vita3K, a tour of 12 levels at
 * the console's speed; the console's crashes). Those blocks stay in a pool instead:
 * up to SCRATCH_POOL of them, made by the game's Alloc (its header intact), handed
 * out again by Alloc and taken back by Free. MINA_BIGLOG: blocks of 256 KiB or more. */
void f_1e4b08_orig(Ctx *c);
void f_1e4b5c_orig(Ctx *c);
#define SCRATCH_POOL 16
static uint32_t scratch_known[SCRATCH_POOL], scratch_free[SCRATCH_POOL];
static int nscratch_known, nscratch_free;
static pthread_mutex_t scratch_lock = PTHREAD_MUTEX_INITIALIZER;
void nat__ZN14ycSysAllocator5AllocEyy11ycAllocInfoPKcjy(Ctx *c)
{
    static int on = -1;
    if (on < 0)
        on = getenv("MINA_BIGLOG") != NULL;
    if (on && c->x[1] >= (256u << 10)) {
        static int told;
        if (told++ < 60)
            rt_log("[big] ycSysAllocator::Alloc %llu align %llu\n", (unsigned long long)c->x[1],
                   (unsigned long long)c->x[2]);
    }
    const int scratch = c->x[1] == 0x80000 && c->x[2] == 0x100;
    if (scratch) {
        pthread_mutex_lock(&scratch_lock);
        const uint32_t a = nscratch_free ? scratch_free[--nscratch_free] : 0;
        pthread_mutex_unlock(&scratch_lock);
        if (a) {
            c->x[0] = a;
            return;
        }
    }
    f_1e4b08_orig(c);
    if (scratch && c->x[0]) {
        pthread_mutex_lock(&scratch_lock);
        if (nscratch_known < SCRATCH_POOL)
            scratch_known[nscratch_known++] = (uint32_t)c->x[0];
        pthread_mutex_unlock(&scratch_lock);
    }
}
void nat__ZN14ycSysAllocator4FreeEPvy(Ctx *c)
{
    const uint32_t a = (uint32_t)c->x[1];
    if (a) {
        pthread_mutex_lock(&scratch_lock);
        for (int i = 0; i < nscratch_known; i++)
            if (scratch_known[i] == a) {
                scratch_free[nscratch_free++] = a;
                pthread_mutex_unlock(&scratch_lock);
                return;
            }
        pthread_mutex_unlock(&scratch_lock);
    }
    f_1e4b5c_orig(c);
}

/* ---- Shop::IsOutOfStock (x0 the ShopDef, x1 its OutInfo out, 0x18 bytes) --------------------
 * NewShopSign::Update asks it every step for its shop (whether there is something new to buy):
 * ~2 ms each at 240 MHz (every item through linear searches of the 361 item definitions and
 * string hashes), 2 of the hub's 14 ms. For the signs the answer is kept per shop and asked
 * again every 30 steps (half a second at 60); any other caller (shop menus, merchants, buying)
 * gets the game's answer and drops the kept ones. MINA_NO_NATIVES=IsOutOfStock: always asked. */
void f_aa0ec4_orig(Ctx *c);
void f_39277c_orig(Ctx *c);
static int in_shop_sign;
typedef struct { uint64_t def, ret; uint8_t out[0x18]; uint32_t age; } ShopMemo;
static ShopMemo shop_memo[8];
static int shop_memo_n;
void nat__ZN11NewShopSign6UpdateEf(Ctx *c)
{
    in_shop_sign++;
    f_39277c_orig(c);
    in_shop_sign--;
}
void nat__ZN4Shop12IsOutOfStockEPK7ShopDefRNS_7OutInfoE(Ctx *c)
{
    static int off = -1;
    if (off < 0)
        off = rt_native_off("__ZN4Shop12IsOutOfStockEPK7ShopDefRNS_7OutInfoE");
    if (off || !in_shop_sign) {
        shop_memo_n = 0;
        f_aa0ec4_orig(c);
        return;
    }
    const uint64_t def = c->x[0], out = c->x[1];
    for (int i = 0; i < shop_memo_n; i++) {
        ShopMemo *m = &shop_memo[i];
        if (m->def == def && ++m->age < 30) {
            memcpy(G(out), m->out, sizeof m->out);
            c->x[0] = m->ret;
            return;
        }
    }
    f_aa0ec4_orig(c);
    int i = 0;
    while (i < shop_memo_n && shop_memo[i].def != def)
        i++;
    if (i == shop_memo_n) {
        if (shop_memo_n == 8)
            i = 0;
        else
            shop_memo_n++;
    }
    shop_memo[i].def = def;
    shop_memo[i].ret = (uint32_t)c->x[0];
    shop_memo[i].age = 0;
    memcpy(shop_memo[i].out, G(out), sizeof shop_memo[i].out);
}

/* ---- DarknessManager::Update / DarknessLight::GetPos --------------------------------------------
 * The darkness grid (mansion): Update recomputes every tile (CalcTile), and each tile asks every
 * light its position (GetPos: the light's or its entity's position, offsets, two fmodf to snap
 * it), tiles x lights times a step. Nothing moves between those calls (Update refreshes its own
 * entity first; CalcTile writes only the grid), so during an Update each light's answer (s0-s2)
 * is kept after its first call. MINA_NO_NATIVES=GetPos: always asked. */
void f_585794_orig(Ctx *c);
void f_5852e0_orig(Ctx *c);
typedef struct { uint64_t light; V128 v[3]; } LightPos;
static LightPos dm_pos[64];
static int dm_batch, dm_n;
static uint32_t dm_gen;          /* one per DarknessManager::Update: what CalcTile kept is for it */
static void darkness_update(Ctx *c);
void nat__ZN15DarknessManager6UpdateEP20ycUpdateQueueContext(Ctx *c)
{
    static int off = -1, verify = -1;
    if (off < 0) {
        off = rt_native_off("__ZN15DarknessManager6UpdateEP20ycUpdateQueueContext");
        verify = getenv("MINA_VERIFY_DARK") != NULL;
    }
    dm_batch = 1;
    dm_n = 0;
    dm_gen++;
    if (off) {
        f_585794_orig(c);
    } else if (!verify) {
        darkness_update(c);
    } else {
        /* the grid, the sprites, the manager and its entity after each, compared */
        GM_BEGIN;
        static uint32_t checked, bad;
        const uint64_t M = c->x[0], E = lp(M + 0x10);
        const uint32_t n = rt_ld32(GP(M + 0x1ac));
        const uint64_t T = lp(M + 0x170), S = lp(M + 0x178);
        const size_t tn = T ? (size_t)n * 12 : 0, sn = S ? (size_t)n * 0x3e0 : 0;
        uint8_t *b = malloc(2 * (tn + sn + 0x1c0 + 0x100));
        if (!b) {
            f_585794_orig(c);
            dm_batch = 0;
            return;
        }
        uint8_t *mine = b + tn + sn + 0x1c0 + 0x100;
#define DK_TAKE(dst) do { uint8_t *p_ = (dst); memcpy(p_, GP(T), tn); memcpy(p_ + tn, GP(S), sn); \
        memcpy(p_ + tn + sn, GP(M), 0x1c0); memcpy(p_ + tn + sn + 0x1c0, GP(E), 0x100); } while (0)
#define DK_PUT(src) do { const uint8_t *p_ = (src); memcpy(GP(T), p_, tn); memcpy(GP(S), p_ + tn, sn); \
        memcpy(GP(M), p_ + tn + sn, 0x1c0); memcpy(GP(E), p_ + tn + sn + 0x1c0, 0x100); } while (0)
        DK_TAKE(b);
        const Ctx save = *c;
        darkness_update(c);
        DK_TAKE(mine);
        DK_PUT(b);
        *c = save;
        dm_n = 0;
        dm_gen++;
        f_585794_orig(c);
        DK_TAKE(b);
        checked++;
        const size_t all = tn + sn + 0x1c0 + 0x100;
        if (memcmp(b, mine, all) && bad++ < 10) {
            size_t i = 0;
            while (b[i] == mine[i]) i++;
            rt_log("[verify] DarknessManager::Update differs at %zu (grid %zu, sprites %zu, manager 0x1c0, entity): "
                   "%02x / %02x\n", i, tn, sn, mine[i], b[i]);
        }
        if ((checked & 0x3ff) == 0)
            rt_log("[verify] DarknessManager::Update: %u checked, %u differ\n", checked, bad);
        free(b);
    }
    dm_batch = 0;
}
void nat__ZNK13DarknessLight6GetPosEv(Ctx *c)
{
    static int off = -1;
    if (off < 0)
        off = rt_native_off("__ZNK13DarknessLight6GetPosEv");
    if (!dm_batch || off) {
        f_5852e0_orig(c);
        return;
    }
    const uint64_t L = c->x[0];
    for (int i = 0; i < dm_n; i++)
        if (dm_pos[i].light == L) {
            c->v[0] = dm_pos[i].v[0];
            c->v[1] = dm_pos[i].v[1];
            c->v[2] = dm_pos[i].v[2];
            return;
        }
    f_5852e0_orig(c);
    if (dm_n < 64) {
        dm_pos[dm_n].light = L;
        dm_pos[dm_n].v[0] = c->v[0];
        dm_pos[dm_n].v[1] = c->v[1];
        dm_pos[dm_n].v[2] = c->v[2];
        dm_n++;
    }
}

/* ---- DarknessManager::CalcTile (x0 the manager M, w1 the tile, w2 force) -------------------------
 * The tile's center (its column and row - 1, x 0.8, + 0.4, from M's entity position) against
 * each light of M+0x180 (the brightest of: 1 inside its inner box, 0.5 inside its outer one in
 * x or y) and the first darkness area of the room (entity +0x50, +0x778) holding it (its
 * values into M+0x198; outside every area the tile is lit). The level (0 dark, 1 half, 2 lit)
 * into the grid M+0x170 (12 bytes a tile: target, level, fade). The game's comparisons, in
 * order (NaN goes where the game's flags send it). MINA_VERIFY_DARK=1 runs both, compares. */
void f_585f84_orig(Ctx *c);
void f_584ec8(Ctx *c);           /* DarknessArea::GetDarknessAABB */
static void calc_tile(Ctx *c)
{
    GM_BEGIN;
    const uint64_t M = c->x[0];
    const int32_t i = (int32_t)c->x[1];
    const uint32_t force = (uint32_t)c->x[2] & 1;
    const int32_t w = (int32_t)rt_ld32(GP(M + 0x1a4));
    const int32_t q = w ? i / w : 0;
    const float k8 = hexf(0x3f4ccccd), k4 = hexf(0x3ecccccd);
    const float s0 = (float)(int32_t)(i - q * w - 1) * k8 + k4;
    const float s1 = (float)(q - 1) * k8 + k4;
    const uint64_t E = lp(M + 0x10);
    const float s8 = ldfp(E + 0xdc) + s0, s9 = ldfp(E + 0xe0) + s1;
    float s11 = ldfp(E + 0xe4);
    uint32_t w24 = 0, w23 = 0;
    float s10 = 0.0f;
    /* in an Update: the lights' boxes, worked out once for all the tiles */
    static struct { uint64_t M; uint32_t gen; int n, ok; uint32_t w23, w24; uint8_t on[64]; float b[64][6]; } lc;
    if (dm_batch && (lc.M != M || lc.gen != dm_gen)) {
        lc.M = M;
        lc.gen = dm_gen;
        lc.n = 0;
        lc.ok = 1;
        lc.w23 = lc.w24 = 0;
        for (uint64_t node = lp(M + 0x180); node; node = lp(node + 0x18)) {
            if (lc.n == 64) {
                lc.ok = 0;
                break;
            }
            const uint64_t L = lp(node + 8);
            if (*(uint8_t *)GP(L + 0x98)) {
                lc.w24 = rt_ld32(GP(L + 0x9c));
                lc.w23 = 1;
            }
            const int k = lc.n++;
            lc.on[k] = *(uint8_t *)GP(L + 0x84) != 0;
            if (!lc.on[k])
                continue;
            c->x[0] = L;
            nat__ZNK13DarknessLight6GetPosEv(c);
            float s2 = ldfp(L + 0x6c), s3 = ldfp(L + 0x70), s4, s5;
            if (*(uint8_t *)GP(L + 0x87)) {
                const float a = ldfp(L + 0x78), b = ldfp(L + 0x7c);
                s2 = s2 - a;
                s3 = s3 - b;
                s5 = a + s2;
                s4 = b + s3;
            } else {
                const float kk = hexf(0xbba3d70a);
                s2 = s2 * 0.5f + kk;
                s3 = s3 * 0.5f + kk;
                s5 = (s2 + ldfp(L + 0x78)) + kk;
                s4 = (s3 + ldfp(L + 0x7c)) + kk;
            }
            float *b = lc.b[k];
            b[0] = c->v[0].f[0];
            b[1] = c->v[1].f[0];
            b[2] = s2;
            b[3] = s3;
            b[4] = s4;
            b[5] = s5;
        }
    }
    if (dm_batch && lc.ok) {
        w23 = lc.w23;
        w24 = lc.w24;
        /* s10 only grows, to at most 1 (s6 is 0, 0.5 or 1): once 1, the rest can't change it */
        for (int k = 0; k < lc.n && s10 != 1.0f; k++) {
            float s6 = 0.0f;
            if (lc.on[k]) {
                const float *b = lc.b[k];
                float d0 = b[0] - s8, d1 = b[1] - s9;
                if (d0 < 0.0f) d0 = -d0;
                if (d1 < 0.0f) d1 = -d1;
                if (d0 < b[2] && d1 < b[3])
                    s6 = 1.0f;
                else if (d0 <= b[5] && d1 <= b[3])
                    s6 = 0.5f;
                else
                    s6 = d0 <= b[2] && d1 <= b[4] ? 0.5f : 0.0f;
            }
            s10 = s10 > s6 ? s10 : s6;
        }
    } else
    for (uint64_t node = lp(M + 0x180); node; node = lp(node + 0x18)) {
        const uint64_t L = lp(node + 8);
        if (*(uint8_t *)GP(L + 0x98)) {
            w24 = rt_ld32(GP(L + 0x9c));
            w23 = 1;
        }
        float s6 = 0.0f;
        if (*(uint8_t *)GP(L + 0x84)) {
            c->x[0] = L;
            nat__ZNK13DarknessLight6GetPosEv(c);
            float d0 = c->v[0].f[0] - s8, d1 = c->v[1].f[0] - s9;
            if (d0 < 0.0f) d0 = -d0;
            if (d1 < 0.0f) d1 = -d1;
            float s2 = ldfp(L + 0x6c), s3 = ldfp(L + 0x70), s4, s5;
            if (*(uint8_t *)GP(L + 0x87)) {
                const float a = ldfp(L + 0x78), b = ldfp(L + 0x7c);
                s2 = s2 - a;
                s3 = s3 - b;
                s5 = a + s2;
                s4 = b + s3;
            } else {
                const float k = hexf(0xbba3d70a);
                s2 = s2 * 0.5f + k;
                s3 = s3 * 0.5f + k;
                s5 = (s2 + ldfp(L + 0x78)) + k;
                s4 = (s3 + ldfp(L + 0x7c)) + k;
            }
            if (d0 < s2 && d1 < s3)
                s6 = 1.0f;
            else if (d0 <= s5 && d1 <= s3)
                s6 = 0.5f;
            else
                s6 = d0 <= s2 && d1 <= s4 ? 0.5f : 0.0f;
        }
        s10 = s10 > s6 ? s10 : s6;
    }
    if (!lp(M + 0x180))
        s10 = 0.0f;
    float r = 1.0f;
    uint64_t an = lp(lp(E + 0x50) + 0x778);
    if (an) {
        s11 = s11 + 0.0f;
        const uint64_t saved_sp = c->sp, fr = saved_sp - 0x40;
        for (; an; an = lp(an + 0x18)) {
            const uint64_t A = lp(an + 8);
            if (!A || !*(uint8_t *)GP(A + 0x171))
                continue;
            /* its box: GetDarknessAABB keeps it itself (from the area's position), so in an
             * Update its first answer is every later one */
            static struct { uint64_t A; uint32_t gen; float b[6]; } ab[16];
            float bx[6];
            int k = 0;
            while (k < 16 && !(ab[k].A == A && ab[k].gen == dm_gen))
                k++;
            if (dm_batch && k < 16) {
                memcpy(bx, ab[k].b, sizeof bx);
            } else {
                c->sp = fr;
                c->x[0] = fr + 8;
                c->x[1] = A;
                f_584ec8(c);
                c->sp = saved_sp;
                memcpy(bx, GP(fr + 8), sizeof bx);
                if (dm_batch) {
                    k = (int)((A >> 4) & 15);
                    ab[k].A = A;
                    ab[k].gen = dm_gen;
                    memcpy(ab[k].b, bx, sizeof bx);
                }
            }
            const float c0 = bx[0], c1 = bx[1], c2 = bx[2];
            const float h0 = bx[3], h1 = bx[4], h2 = bx[5];
            if (s8 < c0 - h0 || s9 < c1 - h1)
                continue;
            if (!(s11 < c2 - h2) && s8 <= c0 + h0 && s9 <= c1 + h1 && !(s11 > c2 + h2)) {
                memcpy(GP(M + 0x198), GP(A + 0x174), 8);
                memcpy(GP(M + 0x1a0), GP(A + 0x17c), 4);
                r = s10;
                break;
            }
        }
        c->sp = saved_sp;
    }
    const uint32_t w11 = w23 & (w24 == 1);
    if (w11)
        r = 1.0f;
    const uint32_t lvl = (r < 1.0f || r != r) ? (r >= 0.5f) : 2;
    const uint64_t T = lp(M + 0x170) + (uint64_t)(int64_t)i * 12;
    if (force || w11) {
        rt_st32(GP(T), lvl);
        rt_st32(GP(T + 4), lvl);
        stfp(T + 8, 0.0f);
    } else if (rt_ld32(GP(T + 4)) != lvl) {
        rt_st32(GP(T + 4), lvl);
        rt_st32(GP(T + 8), 0x3d088889u);
    }
    if (w23 && w24 == 2)
        rt_st32(GP(T + 8), 0);
}
void nat__ZN15DarknessManager8CalcTileEib(Ctx *c)
{
    static int off = -1, verify = -1;
    if (off < 0) {
        off = rt_native_off("__ZN15DarknessManager8CalcTileEib");
        verify = getenv("MINA_VERIFY_DARK") != NULL;
    }
    if (off) {
        f_585f84_orig(c);
        return;
    }
    if (!verify) {
        calc_tile(c);
        return;
    }
    GM_BEGIN;
    static uint32_t checked, bad;
    const uint64_t M = c->x[0];
    const uint64_t T = lp(M + 0x170) + (uint64_t)(int64_t)(int32_t)c->x[1] * 12;
    uint8_t t0[12], m0[12], t1[12], m1[12];
    memcpy(t0, GP(T), 12);
    memcpy(m0, GP(M + 0x198), 12);
    const Ctx save = *c;
    calc_tile(c);
    memcpy(t1, GP(T), 12);
    memcpy(m1, GP(M + 0x198), 12);
    memcpy(GP(T), t0, 12);
    memcpy(GP(M + 0x198), m0, 12);
    *c = save;
    f_585f84_orig(c);
    checked++;
    if ((memcmp(t1, GP(T), 12) || memcmp(m1, GP(M + 0x198), 12)) && bad++ < 10) {
        const uint32_t *a = (const uint32_t *)t1, *b = (const uint32_t *)GP(T);
        rt_log("[verify] CalcTile %d differs: %x %x %x / %x %x %x\n", (int32_t)save.x[1], a[0], a[1], a[2], b[0], b[1],
               b[2]);
    }
    if ((checked & 0xffff) == 0)
        rt_log("[verify] CalcTile: %u checked, %u differ\n", checked, bad);
}

/* ---- hashlittle (Bob Jenkins' lookup3; the game's ignores its initval: 1337 built in) ------------
 * Strings hashed everywhere (names of animations, properties, files): the byte-wise lookup3,
 * which gives the word-wise result on a little-endian machine. MINA_VERIFY_HASH=1 compares. */
void f_1b7b4c_orig(Ctx *c);
#define HL_ROT(x, k) (((x) << (k)) | ((x) >> (32 - (k))))
#define HL_MIX(a, b, c)                                                                                  \
    {                                                                                                    \
        a -= c; a ^= HL_ROT(c, 4); c += b;                                                               \
        b -= a; b ^= HL_ROT(a, 6); a += c;                                                               \
        c -= b; c ^= HL_ROT(b, 8); b += a;                                                               \
        a -= c; a ^= HL_ROT(c, 16); c += b;                                                              \
        b -= a; b ^= HL_ROT(a, 19); a += c;                                                              \
        c -= b; c ^= HL_ROT(b, 4); b += a;                                                               \
    }
#define HL_FINAL(a, b, c)                                                                                \
    {                                                                                                    \
        c ^= b; c -= HL_ROT(b, 14);                                                                      \
        a ^= c; a -= HL_ROT(c, 11);                                                                      \
        b ^= a; b -= HL_ROT(a, 25);                                                                      \
        c ^= b; c -= HL_ROT(b, 16);                                                                      \
        a ^= c; a -= HL_ROT(c, 4);                                                                       \
        b ^= a; b -= HL_ROT(a, 14);                                                                      \
        c ^= b; c -= HL_ROT(b, 24);                                                                      \
    }
static uint32_t hash_little(const uint8_t *k, uint32_t length)
{
    uint32_t a, b, c;
    a = b = c = 0xdeadbeefu + length + 1337u;
    while (length > 12) {
        uint32_t w[3];
        memcpy(w, k, 12);
        a += w[0];
        b += w[1];
        c += w[2];
        HL_MIX(a, b, c);
        length -= 12;
        k += 12;
    }
    switch (length) {
    case 12: c += (uint32_t)k[11] << 24; /* fall through */
    case 11: c += (uint32_t)k[10] << 16; /* fall through */
    case 10: c += (uint32_t)k[9] << 8;   /* fall through */
    case 9: c += k[8];                   /* fall through */
    case 8: b += (uint32_t)k[7] << 24;   /* fall through */
    case 7: b += (uint32_t)k[6] << 16;   /* fall through */
    case 6: b += (uint32_t)k[5] << 8;    /* fall through */
    case 5: b += k[4];                   /* fall through */
    case 4: a += (uint32_t)k[3] << 24;   /* fall through */
    case 3: a += (uint32_t)k[2] << 16;   /* fall through */
    case 2: a += (uint32_t)k[1] << 8;    /* fall through */
    case 1: a += k[0]; break;
    case 0: return c;
    }
    HL_FINAL(a, b, c);
    return c;
}
void nat__Z10hashlittlePKvmj(Ctx *c)
{
    static int verify = -1, off = -1;
    if (off < 0) {
        off = rt_native_off("__Z10hashlittlePKvmj");
        verify = getenv("MINA_VERIFY_HASH") != NULL;
    }
    if (off || c->x[1] >> 31) {
        f_1b7b4c_orig(c);
        return;
    }
    const uint32_t h = hash_little(G(c->x[0]), (uint32_t)c->x[1]);
    if (verify) {
        static uint32_t checked, bad;
        f_1b7b4c_orig(c);
        checked++;
        if ((uint32_t)c->x[0] != h && bad++ < 10)
            rt_log("[verify] hashlittle differs: %#x / %#x (length %u)\n", h, (uint32_t)c->x[0], (uint32_t)c->x[1]);
        if ((checked & 0xffff) == 0)
            rt_log("[verify] hashlittle: %u checked, %u differ\n", checked, bad);
        return;
    }
    c->x[0] = h;
}

/* ---- DarknessManager::Update (x0 the manager M, x1 the update context) ------------------------------
 * Its entity follows the camera in whole tiles of 0.8 (snapped toward the room's corner); when it
 * moves the grid (M+0x170, 12 bytes a tile) is shifted by whole rows/columns (that step runs the
 * game's code); every tile is then recomputed
 * (CalcTile) and fades toward its level by dt; each tile's sprite (M+0x178, 0x3e0 apart) shows
 * animation "0".."3" for (target, level), found by name hash in its animations. The game's code,
 * in order; MINA_VERIFY_DARK=1 compares the grid, sprites, manager and entity. */
void f_d3d2c(Ctx *c);            /* ycCutterAnimComponent::UpdateRenderFrame */
static void dk_calc(Ctx *c, uint64_t M, uint32_t i, uint32_t force)
{
    c->x[0] = M;
    c->x[1] = i;
    c->x[2] = force;
    nat__ZN15DarknessManager8CalcTileEib(c);
}
/* the inputs of every CalcTile of M (see darkness_update); 0 when they don't fit or a light's
 * mode makes CalcTile write whatever it finds */
static int dk_sig(uint8_t *gm_, uint64_t M, uint64_t E, uint8_t *buf, uint32_t *len)
{
    uint32_t n = 0;
    int ok = 1;
    uint32_t w23 = 0, w24 = 0;
#define DK_SIG(addr, l) do { if (n + (l) > 4096) ok = 0; else { memcpy(buf + n, GP(addr), (l)); n += (l); } } while (0)
    DK_SIG(M + 0x170, 0x40);                 /* grid, sprites, lists, 0x198 values, W, H, count */
    DK_SIG(E + 0xdc, 12);
    for (uint64_t node = lp(M + 0x180); node && ok; node = lp(node + 0x18)) {
        const uint64_t L = lp(node + 8);
        DK_SIG(node + 8, 4);
        DK_SIG(L + 0x58, 0x48);              /* owner, offsets, flags, boxes, 0x98/0x9c */
        if (*(uint8_t *)GP(L + 0x98)) {
            w23 = 1;
            w24 = rt_ld32(GP(L + 0x9c));
        }
        const uint64_t o = lp(L + 0x58);
        DK_SIG(o + 0x144, 4);
        DK_SIG(lp(o + 0x10) + 0xdc, 12);
    }
    const uint64_t E50 = lp(E + 0x50);
    DK_SIG(E50 + 0x778, 4);
    for (uint64_t an = lp(E50 + 0x778); an && ok; an = lp(an + 0x18)) {
        const uint64_t A = lp(an + 8);
        DK_SIG(an + 8, 4);
        if (!A)
            continue;
        DK_SIG(A + 0x170, 0x38);             /* flags, values, its box and the position it is for */
        DK_SIG(lp(A + 0x10) + 0xd0, 0x18);
    }
    DK_SIG(0x1e14e28, 4);
#undef DK_SIG
    *len = n;
    return ok && !(w23 && (w24 == 1 || w24 == 2));
}
static void darkness_update(Ctx *c)
{
    GM_BEGIN;
    const uint64_t M = c->x[0], Q = c->x[1];
    if (!*(uint8_t *)GP(M + 0x1b4))
        return;
    float s8 = ldfp(Q + 8);
    const uint64_t E = lp(M + 0x10);
    const float s9 = ldfp(E + 0x68);
    const uint64_t cam = lp(lp(E + 0x50) + 0x16c0);
    const float g = ldfp(0x1e14e28), k8 = hexf(0x3f4ccccd);
    float nx, ny;
    {
        const float c0x = ldfp(0x1d4f880), c0y = ldfp(0x1d4f884);
        float v0[2], v2[2];
        for (int k = 0; k < 2; k++) {
            v2[k] = ldfp(cam + 0x6a84 + 4 * k) - ldfp(cam + 0x6a74 + 4 * k);
            v0[k] = ldfp(cam + 0x6a18 + 4 * k) - ldfp(cam + 0x6a24 + 4 * k);
        }
        float v1[2] = { c0x * g, c0y * g };
        float r[2];
        for (int k = 0; k < 2; k++) {
            float t = v1[k] * 0.5f;
            t = v2[k] + t;
            t = t - v0[k];
            t = t / k8;
            t = floorf(t);
            t = t * k8;
            r[k] = v0[k] + t;
        }
        nx = r[0];
        ny = r[1];
    }
    const float ox = ldfp(E + 0xdc), oy = ldfp(E + 0xe0), oz = ldfp(E + 0xe4);
    /* the grid shifts (the camera crossed a tile): the game's code for that step (the shift
     * isn't exercised by the unattended checks, which don't move) */
    if (*(uint8_t *)GP(M + 0x1b5) && (!(nx == ox && ny == oy) || oz != 0.0f)) {
        c->x[0] = M;
        c->x[1] = Q;
        f_585794_orig(c);
        return;
    }
    stfp(E + 0x9c, nx);
    stfp(E + 0xa0, ny);
    rt_st32(GP(E + 0xa4), 0);
    c->x[0] = E;
    c->x[1] = Q;
    nat__ZN8ycEntity21RefreshWorldTransformEb(c);
    if (!*(uint8_t *)GP(M + 0x1b5)) {
        for (uint32_t i = 0; (int32_t)i < (int32_t)rt_ld32(GP(M + 0x1ac)); i++)
            dk_calc(c, M, i, 1);
        *(uint8_t *)GP(M + 0x1b5) = 1;
    } else {
        /* what every CalcTile(i, false) reads: when it's all as at the last Update and the
         * grid's levels are the ones they wrote, they would write nothing */
        /* what every CalcTile(i, false) reads: when it's all as after the last Update's and the
         * grid's levels are the ones they wrote, they would write nothing (unless a light's mode
         * makes them write anyway: w23 with w24 1 or 2) */
        static uint8_t sig_old[4096], sig_cur[4096], lvl[4096];
        static uint32_t siglen, lvl_n;
        static uint64_t sigM;
        static int have;
        uint32_t n = 0;
        int ok = dk_sig(gm_, M, E, sig_cur, &n);
        const uint32_t cnt = rt_ld32(GP(M + 0x1ac));
        const uint64_t T = lp(M + 0x170);
        int same = ok && have && sigM == M && siglen == n && lvl_n == cnt && !memcmp(sig_cur, sig_old, n);
        for (uint32_t i = 0; same && i < cnt; i++)
            same = rt_ld32(GP(T + (uint64_t)i * 12 + 4)) == lvl[i];
        static int noskip = -1;
        if (noskip < 0)
            noskip = getenv("MINA_DARK_NOSKIP") != NULL;
        if (!same || noskip) {
            for (uint32_t i = 0; (int32_t)i < (int32_t)rt_ld32(GP(M + 0x1ac)); i++)
                dk_calc(c, M, i, 0);
            have = 0;
            const uint32_t cnt2 = rt_ld32(GP(M + 0x1ac));
            const uint64_t T2 = lp(M + 0x170);
            uint32_t m = 0;
            if (cnt2 <= sizeof lvl && dk_sig(gm_, M, E, sig_old, &m)) {
                for (uint32_t i = 0; i < cnt2; i++)
                    lvl[i] = (uint8_t)rt_ld32(GP(T2 + (uint64_t)i * 12 + 4));
                int fits = 1;
                for (uint32_t i = 0; i < cnt2; i++)
                    fits &= rt_ld32(GP(T2 + (uint64_t)i * 12 + 4)) == lvl[i];
                siglen = m;
                sigM = M;
                lvl_n = cnt2;
                have = fits;
            }
        }
    }
    /* each tile's fade and sprite */
    static uint32_t hname[4];
    if (!hname[0]) {
        static const char nm[4][2] = { "0", "1", "2", "3" };
        for (int k = 0; k < 4; k++)
            hname[k] = hash_little((const uint8_t *)nm[k], 1);
    }
    /* (target, level) -> animation, -1 none: the game's jump table */
    static const int8_t pick[22] = { 0, 1, 2, -1, -1, -1, -1, -1, -1, -1, 1, 2, 3, -1, -1, -1, -1, -1, -1, -1, 2, 3 };
    s8 = s8 * s9;
    for (uint64_t i = 0; (int64_t)i < (int64_t)(int32_t)rt_ld32(GP(M + 0x1ac)); i++) {
        const uint64_t T = lp(M + 0x170), S = lp(M + 0x178);
        const uint64_t A = S + i * 0x3e0;
        uint32_t w25 = 0;
        const uint64_t gs = lp(0x6051d08);
        if (!(*(uint8_t *)GP(gs + 1) && (*(uint8_t *)GP(lp(gs + 0x20) + 0x16) & 2))) {
            const uint64_t e = T + i * 12;
            float f = ldfp(e + 8);
            if (!(f < 0.0f || f != f)) {
                f = f - s8;
                stfp(e + 8, f);
                if (!(f > 0.0f || f != f))
                    rt_st32(GP(e), rt_ld32(GP(e + 4)));
            }
            const uint32_t idx = rt_ld32(GP(e)) * 10 + rt_ld32(GP(e + 4));
            if (idx <= 0x15 && pick[idx] >= 0) {
                const uint32_t h = hname[pick[idx]];
                const uint64_t lst = lp(lp(A + 0x40) + 0x10);
                const uint64_t nn = L64(lst + 0x28) >> 34;
                w25 = 1;
                if (nn) {
                    const uint64_t arr = lp(lst + 0x30);
                    uint32_t j = 0;
                    while ((uint64_t)j < nn && rt_ld32(GP(arr + (uint64_t)j * 0x38)) != h)
                        j++;
                    if ((uint64_t)j < nn && j != 0xffffffffu && rt_ld32(GP(A + 0x17c)) != j) {
                        rt_st32(GP(A + 0x17c), j);
                        memset(GP(A + 0x158), 0, 0x18);
                        rt_st32(GP(A + 0x170), 0xffffffffu);
                        rt_st32(GP(A + 0x178), 0x3f800000u);
                        *(uint8_t *)GP(A + 0x174) = 1;
                        c->x[0] = A;
                        f_d3d2c(c);
                    }
                }
            }
        }
        *(uint8_t *)GP(A + 0x28c) = (uint8_t)w25;
        uint8_t vis = (uint8_t)w25;
        const uint64_t par = lp(A + 0x200);
        if (par) {
            const uint32_t fl = rt_ld32(GP(A + 0x1f8));
            if (!(fl & 8) && w25)
                vis = *(uint8_t *)GP(par + 0xcc) != 0;
            else
                vis = (uint8_t)(w25 & (fl >> 3));
        }
        *(uint8_t *)GP(A + 0x155) = vis;
    }
}

/* ---- ycPaletteTexture::WriteFromBase (x0 P, x1 the source S, x2 the base B or null: P's group's) ---
 * P's colors from S by way of B: each of P's colors found in B (first match) takes S's color at
 * that index (wrapped into 1..n-1 past S's end); or, when B is P's group palette, by P's index
 * bytes; or, when S and B share P's source resource, P's own colors from it. Changed colors are
 * marked dirty and the texture queued for upload (once); then the dirty ones get their group
 * index (GetIndex). The game's order of reads and writes; the base's first-match search through a
 * small hash table instead of a scan per color (palettes of ~85 colors on hub_overworld_south:
 * ~3 ms a step). MINA_VERIFY_PAL=1 compares P's arrays and header with the game's. */
void f_1b5590_orig(Ctx *c);
void f_1b4d58(Ctx *c);           /* ycPaletteTexture::WriteSetup */
void f_a5580(Ctx *c);            /* ycRenderUpdateQueue::Add */
static inline uint32_t pal_count(uint8_t *gm_, uint64_t X)
{
    const uint64_t r = lp(X);
    if (r && *(uint8_t *)GP(X + 0x3e))
        return rt_ld32(GP(lp(r + 0x10) + 4));
    return rt_ld32(GP(X + 0x28));
}
static inline uint64_t pal_cols(uint8_t *gm_, uint64_t X)
{
    const uint64_t r = lp(X);
    if (r && *(uint8_t *)GP(X + 0x3e))
        return lp(lp(r + 0x10) + 0x10);
    return lp(X + 0x18);
}
static void pal_set(Ctx *c, uint8_t *gm_, uint64_t P, uint32_t i, uint32_t color)
{
    const uint64_t at = lp(P + 0x18) + (uint64_t)(int64_t)(int32_t)i * 4;
    if (rt_ld32(GP(at)) == color)
        return;
    *(uint8_t *)GP(lp(P + 0x20) + (uint64_t)(int64_t)(int32_t)i) = 1;
    rt_st32(GP(at), color);
    if (!*(uint8_t *)GP(P + 0x3c)) {
        *(uint8_t *)GP(P + 0x3c) = 1;
        const uint64_t saved_sp = c->sp, fr = saved_sp - 0x90;
        uint8_t *f = GP(fr);
        memcpy(f + 8, &P, 8);
        const uint64_t fn = 0x11b463c;
        memcpy(f + 0x10, &fn, 8);
        memset(f + 0x18, 0, 16);
        rt_st32(f + 0x28, 0x40);
        c->sp = fr;
        c->x[0] = fr + 8;
        f_a5580(c);
        c->sp = saved_sp;
    }
    *(uint8_t *)GP(P + 0x3d) = 1;
}
static void write_from_base(Ctx *c)
{
    GM_BEGIN;
    const uint64_t P = c->x[0], S = c->x[1];
    uint64_t B = c->x[2];
    const uint64_t tab = lp(0x60210b8);
    if (!B) {
        const int32_t g = (int32_t)rt_ld32(GP(P + 0x30));
        B = g < 0 ? 0 : tab + (uint64_t)(int64_t)g * 64;
    }
    c->x[0] = P;
    f_1b4d58(c);
    uint32_t nP = pal_count(gm_, P);
    const uint32_t nS = pal_count(gm_, S), nB = pal_count(gm_, B);
    const uint64_t cP = pal_cols(gm_, P), cB = pal_cols(gm_, B), cS = pal_cols(gm_, S);
    const uint64_t rS = lp(S), rB = lp(B);
    if (rS && *(uint8_t *)GP(S + 0x3e) && *(uint8_t *)GP(B + 0x3e) && rB == rS) {
        if ((int32_t)nP < 0)
            nP = rt_ld32(GP(lp(lp(P) + 0x10) + 4));
        for (uint32_t i = 0; (int32_t)i < (int32_t)nP; i++)
            pal_set(c, gm_, P, i, rt_ld32(GP(cP + (uint64_t)i * 4)));
    } else {
        const int32_t g = (int32_t)rt_ld32(GP(P + 0x30));
        const uint64_t T = g < 0 ? 0 : lp(0x60210b8) + (uint64_t)(int64_t)g * 64;
        if (B == T) {
            uint64_t ib = lp(P + 0x10);
            if (lp(P) && *(uint8_t *)GP(P + 0x3e))
                ib = lp(lp(lp(P) + 0x10) + 0x20);
            for (uint32_t i = 0; i != nP; i++) {
                const uint32_t k = *(uint8_t *)GP(ib + i);
                const uint32_t m = nS - 1;
                const uint32_t idx = nS > k ? k : (m ? k % m : k) + 1;
                pal_set(c, gm_, P, i, rt_ld32(GP(cS + (uint64_t)idx * 4)));
            }
        } else if (nP) {
            /* B's colors: the first index of each */
            static uint32_t hk[1024], hv[1024], hgen[1024], gen;
            const int hashed = nB <= 512;
            if (hashed) {
                gen++;
                for (uint32_t j = 0; j < nB; j++) {
                    const uint32_t col = rt_ld32(GP(cB + (uint64_t)j * 4));
                    uint32_t h = (col * 2654435761u) >> 22;
                    while (hgen[h] == gen && hk[h] != col)
                        h = (h + 1) & 1023;
                    if (hgen[h] != gen) {
                        hgen[h] = gen;
                        hk[h] = col;
                        hv[h] = j;
                    }
                }
            }
            for (uint32_t i = 0; i != nP; i++) {
                uint32_t col = rt_ld32(GP(cP + (uint64_t)i * 4));
                uint32_t j = nB;
                if (hashed) {
                    uint32_t h = (col * 2654435761u) >> 22;
                    while (hgen[h] == gen && hk[h] != col)
                        h = (h + 1) & 1023;
                    if (hgen[h] == gen)
                        j = hv[h];
                } else {
                    j = find_u32(GP(cB), nB, col);
                }
                if (j < nB) {
                    const uint32_t m = nS - 1;
                    const uint32_t idx = nS > j ? j : (m ? j % m : j) + 1;
                    col = rt_ld32(GP(cS + (uint64_t)idx * 4));
                }
                pal_set(c, gm_, P, i, col);
            }
        }
    }
    *(uint8_t *)GP(P + 0x3d) = 1;
    if (*(uint8_t *)GP(P + 0x3f))
        return;
    if ((int32_t)rt_ld32(GP(P + 0x30)) >= 0 && lp(0x60210c0)) {
        *(uint8_t *)GP(P + 0x3d) = 0;
        if (lp(P + 0x10)) {
            uint32_t n = rt_ld32(GP(P + 0x28));
            const uint64_t dirty = lp(P + 0x20);
            for (uint64_t i = 0; i < n; i++) {
                if (!*(uint8_t *)GP(dirty + i))
                    continue;
                const int32_t g = (int32_t)rt_ld32(GP(P + 0x30));
                const uint64_t G2 = g < 0 ? 0 : lp(0x60210b8) + (uint64_t)(int64_t)g * 64;
                const uint32_t col = rt_ld32(GP(lp(P + 0x18) + i * 4));
                const uint32_t r = pal_get_index(gm_, G2, col, 0);
                const uint8_t v = ((col >> 24) == 0 && (int32_t)r < 0) ? 0 : (uint8_t)r;
                *(uint8_t *)GP(lp(P + 0x10) + i) = v;
                *(uint8_t *)GP(lp(P + 0x20) + i) = 0;
                n = rt_ld32(GP(P + 0x28));
            }
        }
    }
    *(uint8_t *)GP(P + 0x3f) = 1;
}
void nat__ZN16ycPaletteTexture13WriteFromBaseEPKS_S1_(Ctx *c)
{
    static int off = -1, verify = -1;
    if (off < 0) {
        off = rt_native_off("__ZN16ycPaletteTexture13WriteFromBaseEPKS_S1_");
        verify = getenv("MINA_VERIFY_PAL") != NULL;
    }
    if (off) {
        f_1b5590_orig(c);
        return;
    }
    if (!verify) {
        write_from_base(c);
        return;
    }
    GM_BEGIN;
    static uint32_t checked, bad;
    const uint64_t P = c->x[0];
    const uint32_t n = rt_ld32(GP(P + 0x28)) & 0xfff;
    static uint8_t b0[0x40 + 6 * 4096], b1[0x40 + 6 * 4096];
#define PV_TAKE(b) do { memcpy(b, GP(P), 0x40); if (lp(P + 0x18)) memcpy(b + 0x40, GP(lp(P + 0x18)), n * 4); \
        if (lp(P + 0x20)) memcpy(b + 0x40 + 4 * 4096, GP(lp(P + 0x20)), n); \
        if (lp(P + 0x10)) memcpy(b + 0x40 + 5 * 4096, GP(lp(P + 0x10)), n); } while (0)
#define PV_PUT(b) do { memcpy(GP(P), b, 0x40); if (lp(P + 0x18)) memcpy(GP(lp(P + 0x18)), b + 0x40, n * 4); \
        if (lp(P + 0x20)) memcpy(GP(lp(P + 0x20)), b + 0x40 + 4 * 4096, n); \
        if (lp(P + 0x10)) memcpy(GP(lp(P + 0x10)), b + 0x40 + 5 * 4096, n); } while (0)
    memset(b0, 0, sizeof b0);
    memset(b1, 0, sizeof b1);
    PV_TAKE(b0);
    const Ctx save = *c;
    write_from_base(c);
    static uint8_t mine[sizeof b0];
    memset(mine, 0, sizeof mine);
    PV_TAKE(mine);
    PV_PUT(b0);
    *c = save;
    f_1b5590_orig(c);
    PV_TAKE(b1);
    /* P+8..0x27: WriteSetup's buffers (it ran in both: new ones); their contents are compared */
    memset(mine + 8, 0, 0x20);
    memset(b1 + 8, 0, 0x20);
    checked++;
    if (memcmp(mine, b1, sizeof b1) && bad++ < 10) {
        size_t i = 0;
        while (mine[i] == b1[i]) i++;
        rt_log("[verify] WriteFromBase differs at %u (%u colors): %02x / %02x\n", (unsigned)i, n, mine[i], b1[i]);
    }
    if ((checked & 0xff) == 0)
        rt_log("[verify] WriteFromBase: %u checked, %u differ\n", checked, bad);
}

/* ---- ycPaletteAnimEntry::Update (x0 the entry E, s0 dt, x1 the UpdateInfo I) -------------------------
 * E: +0 its ycAnimDataLookup, +8 its resource, +0x10 a palette to write from, +0x18 a level,
 * +0x1c a mask of its sub-animations, +0x20 time, +0x24 duration, +0x28 rate, +0x2c loops left.
 * I: +0 the component's level, +8 a scratch palette, +0x10 the target palette, +0x18 a base,
 * +0x20 "target set up". The time advances (and loops); each sub-animation (0x48 bytes) of the
 * mask takes a frame (a palette resource) into the scratch palette, then copies a range of
 * colors (cycled by time) from it into the target, through FindFromBase against the base when
 * asked. Fields come through AnimateScalar (the game's). Returns 1 when its loops ran out.
 * The game's operations in order; the per-color loop is where the time went (~190 colors an
 * entry, ~5000 a step on hub_overworld_south). MINA_NO_NATIVES=PaletteAnimEntry: the game's. */
void f_1b18e8_orig(Ctx *c);
void f_a6c18(Ctx *c);            /* ycAnimDataLookup::AnimateScalar */
void f_a5ac8(Ctx *c);            /* ycRenderUpdateQueue::Remove */
void f_1b4294(Ctx *c);           /* ycPaletteResourceCache::ReleaseRef */
void f_1b4b5c(Ctx *c);           /* ycPaletteTexture::Write */
static inline int32_t fcvtzs32(float f)
{
    if (f != f)
        return 0;
    if (f >= 2147483648.0f)
        return INT32_MAX;
    if (f <= -2147483648.0f)
        return INT32_MIN;
    return (int32_t)f;
}
static inline int32_t posmod32(int32_t a, int32_t m)
{
    if (!m)
        return a;                                  /* sdiv by 0 is 0 on arm64 */
    if (m == -1)
        return 0;
    const int32_t r = a - (a / m) * m;
    return r + (m & (r >> 31));
}
typedef struct { Ctx *c; uint8_t *gm; uint64_t fr; } PaeCtx;
static uint64_t pae_scalar(PaeCtx *k, uint64_t look, uint64_t field, float t)
{
    Ctx *c = k->c;
    const uint64_t saved = c->sp;
    c->x[0] = look;
    c->x[1] = field;
    c->x[2] = k->fr + 0x58;
    c->v[0] = vset_f32(t);
    c->sp = k->fr;
    f_a6c18(c);
    c->sp = saved;
    return (uint32_t)c->x[0] ? k->fr + 0x58 : field;
}
static void pae_queue(PaeCtx *k, uint64_t P, void (*fn)(Ctx *))
{
    Ctx *c = k->c;
    uint8_t *gm_ = k->gm;
    uint8_t *f = GP(k->fr);
    const uint64_t cb = 0x11b463c;
    memcpy(f + 0x58, &P, 8);
    memcpy(f + 0x60, &cb, 8);
    memset(f + 0x68, 0, 16);
    rt_st32(f + 0x78, 0x40);
    const uint64_t saved = c->sp;
    c->sp = k->fr;
    c->x[0] = k->fr + 0x58;
    fn(c);
    c->sp = saved;
}
/* the scratch palette T takes resource R */
static int pae_released;          /* MINA_VERIFY_PAL: a resource let go (the run can't be undone) */
static uint64_t pae_ref_at[16];
static uint32_t pae_ref_was[16], pae_nref;
static void pae_take(PaeCtx *k, uint64_t T, uint64_t R)
{
    uint8_t *gm_ = k->gm;
    *(uint8_t *)GP(T + 0x3e) = 1;
    if (*(uint8_t *)GP(T + 0x3c)) {
        rt_st16(GP(T + 0x3c), 0);
        pae_queue(k, T, f_a5ac8);
    }
    const uint64_t old = L64(T);
    if (old == R)
        return;
    if (pae_nref < 16) {
        pae_ref_at[pae_nref] = R + 4;
        pae_ref_was[pae_nref++] = rt_ld32(GP(R + 4));
    }
    rt_st32(GP(R + 4), rt_ld32(GP(R + 4)) + 1);
    memcpy(GP(T), &R, 8);
    if (old) {
        pae_released = 1;
        const uint64_t saved = k->c->sp;
        k->c->sp = k->fr;
        k->c->x[0] = old;
        f_1b4294(k->c);
        k->c->sp = saved;
    }
    rt_st32(GP(T + 0x38), rt_ld32(GP(T + 0x38)) + 1);
}
/* color i of the target X (its own array) becomes v */
static void pae_put(PaeCtx *k, uint64_t X, uint64_t cols, int64_t i, uint32_t v)
{
    uint8_t *gm_ = k->gm;
    if (rt_ld32(GP(cols + (uint64_t)i * 4)) == v)
        return;
    *(uint8_t *)GP(lp(X + 0x20) + (uint64_t)i) = 1;
    rt_st32(GP(cols + (uint64_t)i * 4), v);
    if (!*(uint8_t *)GP(X + 0x3c)) {
        *(uint8_t *)GP(X + 0x3c) = 1;
        pae_queue(k, X, f_a5580);
    }
    *(uint8_t *)GP(X + 0x3d) = 1;
}
static void pae_setup(PaeCtx *k, uint64_t I)
{
    uint8_t *gm_ = k->gm;
    if (*(uint8_t *)GP(I + 0x20))
        return;
    *(uint8_t *)GP(I + 0x20) = 1;
    const uint64_t saved = k->c->sp;
    k->c->sp = k->fr;
    k->c->x[0] = lp(I + 0x10);
    f_1b4d58(k->c);
    k->c->sp = saved;
}
static uint32_t pal_anim_entry(Ctx *c)
{
    CALLS(9);
    GM_BEGIN;
    const uint64_t E = c->x[0], I = c->x[1];
    float dt = c->v[0].f[0];
    PaeCtx k = { c, gm_, c->sp - 0xf0 };
    const uint64_t look = L64(E), res = L64(E + 8);
    const uint64_t subv = lp(res + 0x10);
    uint32_t ret = 0;
    float t;
    int32_t loops = (int32_t)rt_ld32(GP(E + 0x2c));
    if (!loops) {
        t = ldfp(E + 0x20);
    } else {
        const float rate = ldfp(E + 0x28), dur = ldfp(E + 0x24);
        t = ldfp(E + 0x20) + rate * dt;
        stfp(E + 0x20, t);
        if (!(dur > 0.0f)) {
            /* b.le (or NaN): as is */
        } else if (rate >= 0.0f) {
            const float lim = dur + hexf(0xba83126fu);
            if (t >= lim) {
                for (;;) {
                    const int32_t l1 = loops - 1;
                    if (l1 >= 0) {
                        rt_st32(GP(E + 0x2c), (uint32_t)l1);
                        loops = l1;
                        if (l1 == 0) {
                            ret = 1;
                            stfp(E + 0x20, lim);
                            t = lim;
                            break;
                        }
                    }
                    t = t - dur;
                    if (t < lim || t != t) {
                        stfp(E + 0x20, t);
                        break;
                    }
                }
            }
        } else {
            const float lim = hexf(0x3a83126fu);
            if (!(t > lim || t != t)) {
                for (;;) {
                    const int32_t l1 = loops - 1;
                    if (l1 >= 0) {
                        rt_st32(GP(E + 0x2c), (uint32_t)l1);
                        loops = l1;
                        if (l1 == 0) {
                            ret = 1;
                            stfp(E + 0x20, 0.0f);
                            t = 0.0f;
                            break;
                        }
                    }
                    t = dur + t;
                    if (t > lim || t != t) {
                        stfp(E + 0x20, t);
                        break;
                    }
                }
            }
        }
    }
    if (!(L64(subv) >> 34))
        return ret;
    if ((int32_t)rt_ld32(GP(E + 0x18)) < (int32_t)rt_ld32(GP(I)))
        return ret;
    const uint64_t subs = L64(subv + 8);
    uint64_t acc = 0;
    for (uint64_t si = 0; si < (L64(subv) >> 34); si++) {
        const uint64_t sub = subs + si * 0x48;
        const uint64_t m = L64(sub + 0x10) >> 34;
        if (!((rt_ld32(GP(E + 0x1c)) >> ((uint32_t)si & 31)) & 1)) {
            acc += m;
            continue;
        }
        const int32_t first_frame = (int32_t)rt_ld32(GP(pae_scalar(&k, look, sub + 0x24, t)));
        const float fr_rate = ldfp(pae_scalar(&k, look, sub + 0x28, t));
        const int32_t fi = posmod32(first_frame + fcvtzs32(t * fr_rate), (int32_t)(uint32_t)m);
        const uint64_t R = L64(lp(L64(E + 8) + 0x18) + (acc + (uint64_t)(int64_t)fi) * 8);
        const uint32_t base_on = *(uint8_t *)GP(pae_scalar(&k, look, sub + 0x20, t));
        const uint64_t T = L64(I + 8);
        if (R) {
            pae_take(&k, T, R);
        } else if (base_on) {
            acc += m;
            continue;
        } else {
            const uint64_t X = L64(I + 0x10), RX = L64(X);
            if (RX && *(uint8_t *)GP(X + 0x3e)) {
                pae_take(&k, T, RX);
            } else {
                const uint64_t saved = c->sp;
                c->sp = k.fr;
                c->x[0] = T;
                c->x[1] = X;
                f_1b4b5c(c);
                c->sp = saved;
            }
        }
        const uint64_t wfb = L64(E + 0x10);
        if (wfb) {
            const uint64_t saved = c->sp;
            c->sp = k.fr;
            c->x[0] = L64(I + 8);
            c->x[1] = wfb;
            c->x[2] = 0;
            nat__ZN16ycPaletteTexture13WriteFromBaseEPKS_S1_(c);
            c->sp = saved;
        }
        const int32_t start = (int32_t)rt_ld32(GP(pae_scalar(&k, look, sub + 0x34, t)));
        const int32_t len = (int32_t)rt_ld32(GP(pae_scalar(&k, look, sub + 0x38, t)));
        int64_t i = (int64_t)(int32_t)rt_ld32(GP(pae_scalar(&k, look, sub + 0x2c, t)));
        const int32_t count = (int32_t)rt_ld32(GP(pae_scalar(&k, look, sub + 0x30, t)));
        const float off = ldfp(pae_scalar(&k, look, sub + 0x3c, t));
        const float speed = ldfp(pae_scalar(&k, look, sub + 0x40, t));
        const int use_base = base_on ? L64(I + 0x18) != 0 : 0;
        int32_t span = (int32_t)pal_count(gm_, L64(I + 8)) - start;
        span = (len < span && len != 0) ? len : span;
        int32_t end = (int32_t)pal_count(gm_, L64(I + 0x10));
        if (count >= 1) {
            const int32_t e2 = count + (int32_t)i;
            end = e2 < end ? e2 : end;
        }
        if ((int32_t)i >= end) {
            acc += m;
            continue;
        }
        int32_t pos = fcvtzs32(off + t * speed);
        uint32_t left = (uint32_t)(end - (int32_t)i);
        /* FindFromBase(scratch, color, base): both fixed through the loop (only the target is
         * written: checked per color against their colors) */
        PalHash *fb_h = NULL;
        uint32_t fb_n = 0, fb_cols = 0, fb_bn = 0, fb_bcols = 0;
        if (use_base) {
            pal_colors(gm_, L64(I + 8), &fb_n, &fb_cols);
            pal_colors(gm_, L64(I + 0x18), &fb_bn, &fb_bcols);
            fb_h = pal_table(gm_, fb_bcols, fb_bn);
        }
        for (; left; left--, i++, pos++) {
            uint64_t X = L64(I + 0x10);
            if (i >= (int64_t)(int32_t)pal_count(gm_, X))
                break;
            const int32_t idx = posmod32(pos, span) + start;
            if (use_base) {
                const uint64_t col = rt_ld32(GP(pal_cols(gm_, X) + (uint64_t)(int64_t)idx * 4));
                uint64_t own = lp(X + 0x18);
                uint32_t r;
                if (fb_h && own != fb_cols && own != fb_bcols) {
                    /* FindFromBase with the base's table resolved before the loop */
                    const uint32_t j = pal_lookup(fb_h, (uint32_t)col);
                    if (j == fb_bn)
                        r = (uint32_t)col;
                    else if (j < fb_n)
                        r = rt_ld32(GP(fb_cols + 4 * j));
                    else {
                        const uint32_t mm = fb_n - 1, q = mm ? j / mm : 0;
                        r = rt_ld32(GP(fb_cols + 4 * (j - q * mm + 1)));
                    }
                } else {
                    rt_st32(GP(k.fr + 0x50), (uint32_t)col);
                    r = pal_find_from_base(gm_, L64(I + 8), k.fr + 0x50, L64(I + 0x18));
                }
                if (!*(uint8_t *)GP(I + 0x20)) {
                    pae_setup(&k, I);
                    X = L64(I + 0x10);
                    own = lp(X + 0x18);
                }
                pae_put(&k, X, own, i, r);
            } else {
                const uint32_t col = rt_ld32(GP(pal_cols(gm_, L64(I + 8)) + (uint64_t)(int64_t)idx * 4));
                if (!*(uint8_t *)GP(I + 0x20)) {
                    pae_setup(&k, I);
                    X = L64(I + 0x10);
                }
                pae_put(&k, X, lp(X + 0x18), i, col);
            }
        }
        acc += m;
    }
    return ret;
}
void nat__ZN18ycPaletteAnimEntry6UpdateEfPNS_10UpdateInfoE(Ctx *c)
{
    static int off = -1;
    if (off < 0)
        off = rt_native_off("__ZN18ycPaletteAnimEntry6UpdateEfPNS_10UpdateInfoE");
    if (off) {
        f_1b18e8_orig(c);
        return;
    }
    static int verify = -1;
    if (verify < 0)
        verify = getenv("MINA_VERIFY_PAL") != NULL;
    const uint64_t saved = c->sp;
    if (!verify) {
        const uint32_t r = pal_anim_entry(c);
        c->sp = saved;
        c->x[0] = r;
        return;
    }
    /* the entry, the info, the scratch and target palettes (headers, colors, dirty flags,
     * indices) after each; WriteSetup's buffers (P+8..0x27) by content */
    GM_BEGIN;
    static uint32_t checked, bad, skipped;
    const uint64_t E = c->x[0], I = c->x[1];
    const uint64_t T = L64(I + 8), X = L64(I + 0x10);
    static uint8_t s0[0x2000], s1[0x2000], s2[0x2000];
#define PAE_TAKE(b) do { uint8_t *p_ = (b); size_t o_ = 0; memset(p_, 0, sizeof s0); \
        memcpy(p_ + o_, GP(E), 0x58); o_ += 0x58; memcpy(p_ + o_, GP(I), 0x28); o_ += 0x28; \
        const uint64_t pp_[2] = { T, X }; \
        for (int q_ = 0; q_ < 2; q_++) { const uint64_t P_ = pp_[q_]; if (!P_) continue; \
            memcpy(p_ + o_, GP(P_), 0x40); o_ += 0x40; const uint32_t n_ = rt_ld32(GP(P_ + 0x28)) & 0x3ff; \
            if (lp(P_ + 0x18)) memcpy(p_ + o_, GP(lp(P_ + 0x18)), n_ * 4); o_ += 0x1000 / 2; \
            if (lp(P_ + 0x20)) memcpy(p_ + o_, GP(lp(P_ + 0x20)), n_); o_ += 0x400 / 2; \
            if (lp(P_ + 0x10)) memcpy(p_ + o_, GP(lp(P_ + 0x10)), n_); o_ += 0x400 / 2; } } while (0)
#define PAE_PUT(b) do { const uint8_t *p_ = (b); size_t o_ = 0; \
        memcpy(GP(E), p_ + o_, 0x58); o_ += 0x58; memcpy(GP(I), p_ + o_, 0x28); o_ += 0x28; \
        const uint64_t pp_[2] = { T, X }; \
        for (int q_ = 0; q_ < 2; q_++) { const uint64_t P_ = pp_[q_]; if (!P_) continue; \
            memcpy(GP(P_), p_ + o_, 0x40); o_ += 0x40; const uint32_t n_ = rt_ld32(GP(P_ + 0x28)) & 0x3ff; \
            if (lp(P_ + 0x18)) memcpy(GP(lp(P_ + 0x18)), p_ + o_, n_ * 4); o_ += 0x1000 / 2; \
            if (lp(P_ + 0x20)) memcpy(GP(lp(P_ + 0x20)), p_ + o_, n_); o_ += 0x400 / 2; \
            if (lp(P_ + 0x10)) memcpy(GP(lp(P_ + 0x10)), p_ + o_, n_); o_ += 0x400 / 2; } } while (0)
    PAE_TAKE(s0);
    const Ctx save = *c;
    pae_released = 0;
    pae_nref = 0;
    const uint32_t mine = pal_anim_entry(c);
    c->sp = saved;
    if (pae_released) {
        skipped++;
        c->x[0] = mine;
        return;
    }
    PAE_TAKE(s1);
    PAE_PUT(s0);
    for (uint32_t q = 0; q < pae_nref; q++)
        rt_st32(GP(pae_ref_at[q]), pae_ref_was[q]);
    *c = save;
    f_1b18e8_orig(c);
    PAE_TAKE(s2);
    const size_t hp = 0x58 + 0x28;
    for (int q = 0; q < 2; q++) {
        memset(s1 + hp + q * 0xc40 + 8, 0, 0x20);
        memset(s2 + hp + q * 0xc40 + 8, 0, 0x20);
    }
    checked++;
    if ((memcmp(s1, s2, sizeof s1) || mine != (uint32_t)c->x[0]) && bad++ < 10) {
        size_t i = 0;
        while (i < sizeof s1 && s1[i] == s2[i]) i++;
        rt_log("[verify] PaletteAnimEntry differs at %u: %02x / %02x, returns %u / %u\n", (unsigned)i,
               i < sizeof s1 ? s1[i] : 0, i < sizeof s1 ? s2[i] : 0, mine, (uint32_t)c->x[0]);
    }
    if ((checked & 0x3ff) == 0)
        rt_log("[verify] PaletteAnimEntry: %u checked, %u differ, %u skipped (a resource let go)\n", checked, bad,
               skipped);
}

/* ---- ycPaletteComponent::Update: a step that would give the same palette is skipped ----------
 * Each step the game resets the component's palette (x28 = comp+0x88) to its base (the resource
 * at comp+0x90 taken, or the palette at comp+0x90 written in), runs its animation entries (a frame
 * palette and a cycled range of colors each, ycPaletteAnimEntry::Update) over it, maps the colors
 * that changed to the global palette's indices and queues the texture's upload: ~150 us for each
 * of the 26 animated components on hub_overworld_south, where 92% end the step as they began it.
 * The result is a function of: the palette's state (header but its upload flags +0x3c/+0x3d,
 * colors, indices), the global palette (the table's entry +0x30) and the base, and per entry its
 * fields and what its time selects (frame, flags, range, offset of the cycle: the values
 * AnimateScalar gives at the new time). When all of them are as at the end of the last step that
 * ran (no color left dirty), that step's result is already there: only the entries' times (and
 * loop counts) advance, as the game's code would. Anything else (an entry running out of loops,
 * a slot or signature full) runs the game's code, which is then recorded.
 * MINA_NO_NATIVES=PalSkip. MINA_VERIFY_PALSKIP=1: a skip is checked against the game's code. */
#define PM_SLOTS 40
#define PM_SIG 384
#define PM_COLS 512
typedef struct {
    uint32_t comp, used, valid, nsig, n, gn, bn, dver;
    uint32_t sig[PM_SIG];
    uint8_t hdr[0x40], ghdr[0x40], bhdr[0x40];
    uint32_t cols[PM_COLS], gcols[PM_COLS], bcols[PM_COLS];
    uint8_t idx[PM_COLS];
} PalMemo;
static PalMemo pm_slot[PM_SLOTS];
static uint32_t pm_use;
static PalMemo *pm_find(uint32_t comp, int make)
{
    int lru = 0;
    for (int k = 0; k < PM_SLOTS; k++) {
        if (pm_slot[k].comp == comp)
            return &pm_slot[k];
        if (pm_slot[k].used < pm_slot[lru].used)
            lru = k;
    }
    if (!make)
        return NULL;
    PalMemo *m = &pm_slot[lru];
    m->comp = comp;
    m->valid = 0;
    return m;
}
static inline uint64_t pm_global(uint8_t *gm_, uint64_t P)
{
    const int32_t i = (int32_t)rt_ld32(GP(P + 0x30));
    return i < 0 ? 0 : rt_ld64(GP(0x60210b8)) + 0x40ull * (uint32_t)i;
}
/* the state compared (and recorded): x28's header (upload flags zeroed), own colors and indices;
 * the global palette's and the base's headers and colors. 0: doesn't fit. */
typedef struct { uint8_t hdr[0x40], ghdr[0x40], bhdr[0x40]; uint32_t n, gn, bn, ncol, gcol, bcol, idx; } PmState;
static int pm_state(uint8_t *gm_, uint64_t comp, PmState *s)
{
    const uint64_t P = lp(comp + 0x88), G = pm_global(gm_, P), B = comp + 0x90;
    memcpy(s->hdr, GP(P), 0x40);
    s->hdr[0x3c] = s->hdr[0x3d] = 0;
    memset(s->hdr + 0x38, 0, 4);          /* the version: advanced by the last step's delta instead */
    s->n = rt_ld32(GP(P + 0x28));
    s->ncol = (uint32_t)lp(P + 0x18);
    s->idx = (uint32_t)lp(P + 0x10);
    if (s->n > PM_COLS || (s->n && !s->ncol))
        return 0;
    memset(s->ghdr, 0, 0x40);
    s->gn = 0;
    s->gcol = 0;
    if (G) {
        memcpy(s->ghdr, GP(G), 0x40);
        s->ghdr[0x3c] = s->ghdr[0x3d] = 0;
        pal_colors(gm_, G, &s->gn, &s->gcol);
        if (s->gn > PM_COLS)
            return 0;
    }
    memcpy(s->bhdr, GP(B), 0x40);
    s->bhdr[0x3c] = s->bhdr[0x3d] = 0;
    pal_colors(gm_, B, &s->bn, &s->bcol);
    return s->bn <= PM_COLS;
}
/* the entries' signature at their new times (into sig, its length; -1: no skip possible) and the
 * times and loop counts they'd store */
typedef struct { float t; int32_t loops; } PmTime;
static int pm_signature(Ctx *c, uint8_t *gm_, uint64_t comp, float dt, uint32_t *sig, PmTime *tm, int maxe)
{
    const uint64_t vec = lp(comp + 0x198), ne = rt_ld64(GP(comp + 0x1a0));
    if (!ne || ne > (uint64_t)maxe)
        return -1;
    int32_t w26 = INT32_MIN;
    for (uint64_t j = 0; j < ne; j++) {
        const int32_t p = (int32_t)rt_ld32(GP(vec + 0x58 * j + 0x18));
        w26 = w26 > p ? w26 : p;
    }
    PaeCtx k = { c, gm_, c->sp - 0x100 };
    int n = 0;
#define PM_PUSH(v) do { if (n >= PM_SIG) return -1; sig[n++] = (uint32_t)(v); } while (0)
    for (uint64_t j = 0; j < ne; j++) {
        const uint64_t E = vec + 0x58 * j;
        /* the time, as pal_anim_entry advances it */
        float t;
        int32_t loops = (int32_t)rt_ld32(GP(E + 0x2c));
        if (!loops) {
            t = ldfp(E + 0x20);
        } else {
            const float rate = ldfp(E + 0x28), dur = ldfp(E + 0x24);
            t = ldfp(E + 0x20) + rate * dt;
            if (!(dur > 0.0f)) {
            } else if (rate >= 0.0f) {
                const float lim = dur + hexf(0xba83126fu);
                if (t >= lim) {
                    for (;;) {
                        const int32_t l1 = loops - 1;
                        if (l1 >= 0) {
                            loops = l1;
                            if (l1 == 0)
                                return -1;          /* runs out: the entry goes */
                        }
                        t = t - dur;
                        if (t < lim || t != t)
                            break;
                    }
                }
            } else {
                const float lim = hexf(0x3a83126fu);
                if (!(t > lim || t != t)) {
                    for (;;) {
                        const int32_t l1 = loops - 1;
                        if (l1 >= 0) {
                            loops = l1;
                            if (l1 == 0)
                                return -1;
                        }
                        t = dur + t;
                        if (t > lim || t != t)
                            break;
                    }
                }
            }
        }
        tm[j].t = t;
        tm[j].loops = loops;
        const uint64_t look = L64(E), res = L64(E + 8), subv = lp(res + 0x10), wfb = L64(E + 0x10);
        PM_PUSH(E);
        PM_PUSH(look);
        PM_PUSH(res);
        PM_PUSH(wfb);
        PM_PUSH(rt_ld32(GP(E + 0x18)));
        PM_PUSH(rt_ld32(GP(E + 0x1c)));
        PM_PUSH(*(uint8_t *)GP(E + 0x50));
        if (wfb) {
            /* the palette written from: its header and colors */
            uint32_t wn, wc;
            pal_colors(gm_, wfb, &wn, &wc);
            if (wn > PM_COLS)
                return -1;
            uint64_t h = 1469598103934665603ull;
            const uint8_t *p = GP(wfb);
            for (int q = 0; q < 0x40; q++)
                if (q != 0x3c && q != 0x3d)
                    h = (h ^ p[q]) * 1099511628211ull;
            p = GP(wc);
            for (uint32_t q = 0; q < wn * 4; q++)
                h = (h ^ p[q]) * 1099511628211ull;
            PM_PUSH(h);
            PM_PUSH(h >> 32);
        }
        const uint64_t nsub = L64(subv) >> 34;
        PM_PUSH(nsub);
        if (!nsub || (int32_t)rt_ld32(GP(E + 0x18)) < w26)
            continue;
        const uint64_t subs = L64(subv + 8);
        uint64_t acc = 0;
        for (uint64_t si = 0; si < nsub; si++) {
            const uint64_t sub = subs + si * 0x48;
            const uint64_t m = L64(sub + 0x10) >> 34;
            if (!((rt_ld32(GP(E + 0x1c)) >> ((uint32_t)si & 31)) & 1)) {
                acc += m;
                continue;
            }
            const int32_t first_frame = (int32_t)rt_ld32(GP(pae_scalar(&k, look, sub + 0x24, t)));
            const float fr_rate = ldfp(pae_scalar(&k, look, sub + 0x28, t));
            const int32_t fi = posmod32(first_frame + fcvtzs32(t * fr_rate), (int32_t)(uint32_t)m);
            const uint64_t R = L64(lp(res + 0x18) + (acc + (uint64_t)(int64_t)fi) * 8);
            PM_PUSH(si);
            PM_PUSH(R);
            PM_PUSH(R >> 32);
            PM_PUSH(*(uint8_t *)GP(pae_scalar(&k, look, sub + 0x20, t)));
            PM_PUSH(rt_ld32(GP(pae_scalar(&k, look, sub + 0x34, t))));
            PM_PUSH(rt_ld32(GP(pae_scalar(&k, look, sub + 0x38, t))));
            PM_PUSH(rt_ld32(GP(pae_scalar(&k, look, sub + 0x2c, t))));
            PM_PUSH(rt_ld32(GP(pae_scalar(&k, look, sub + 0x30, t))));
            const float off = ldfp(pae_scalar(&k, look, sub + 0x3c, t));
            const float speed = ldfp(pae_scalar(&k, look, sub + 0x40, t));
            PM_PUSH(fcvtzs32(off + t * speed));
            acc += m;
        }
    }
#undef PM_PUSH
    return n;
}
static uint32_t pm_why[8];
static int pm_same(uint8_t *gm_, const PalMemo *m, const PmState *s, uint64_t comp)
{
    if (memcmp(m->hdr, s->hdr, 0x40)) {
        pm_why[0]++;
        return 0;
    }
    if (memcmp(m->ghdr, s->ghdr, 0x40) || m->gn != s->gn) {
        pm_why[1]++;
        return 0;
    }
    if (memcmp(m->bhdr, s->bhdr, 0x40) || m->n != s->n || m->bn != s->bn) {
        pm_why[2]++;
        return 0;
    }
    const uint64_t P = lp(comp + 0x88);
    if (s->n) {
        if (memcmp(m->cols, GP(s->ncol), s->n * 4)) {
            pm_why[3]++;
            return 0;
        }
        if (s->idx && memcmp(m->idx, GP(s->idx), s->n)) {
            pm_why[4]++;
            return 0;
        }
        const uint64_t dirty = lp(P + 0x20);
        if (dirty) {
            const uint8_t *d = GP(dirty);
            for (uint32_t i = 0; i < s->n; i++)
                if (d[i]) {
                    pm_why[5]++;
                    return 0;
                }
        }
    }
    if (s->gn && memcmp(m->gcols, GP(s->gcol), s->gn * 4)) {
        pm_why[6]++;
        return 0;
    }
    if (s->bn && memcmp(m->bcols, GP(s->bcol), s->bn * 4))
        return 0;
    return 1;
}
static void pm_record(uint8_t *gm_, PalMemo *m, const PmState *s)
{
    memcpy(m->hdr, s->hdr, 0x40);
    memcpy(m->ghdr, s->ghdr, 0x40);
    memcpy(m->bhdr, s->bhdr, 0x40);
    m->n = s->n;
    m->gn = s->gn;
    m->bn = s->bn;
    if (s->n) {
        memcpy(m->cols, GP(s->ncol), s->n * 4);
        if (s->idx)
            memcpy(m->idx, GP(s->idx), s->n);
    }
    if (s->gn)
        memcpy(m->gcols, GP(s->gcol), s->gn * 4);
    if (s->bn)
        memcpy(m->bcols, GP(s->bcol), s->bn * 4);
}
/* 1: skipped (the entries' times advanced); 0: run the game's code and call pm_after */
static uint32_t pm_sig_tmp[PM_SIG];
static PmTime pm_tm[16];
static int pm_pending_sig = -1;
static PalMemo *pm_pending;
static int pm_try(Ctx *c, float dt)
{
    static int off = -1;
    if (off < 0)
        off = rt_native_off("PalSkip");
    pm_pending = NULL;
    if (off)
        return 0;
    GM_BEGIN;
    const uint64_t comp = c->x[0];
    if (!*(uint8_t *)GP(comp + 0xd0) || !*(uint8_t *)GP(comp + 0xd1) || !lp(comp + 0x88))
        return 0;
    const int n = pm_signature(c, gm_, comp, dt, pm_sig_tmp, pm_tm, 16);
    if (n < 0)
        return 0;
    PalMemo *m = pm_find((uint32_t)comp, 1);
    m->used = ++pm_use;
    PmState s;
    if (m->valid && !((int)m->nsig == n && !memcmp(m->sig, pm_sig_tmp, n * 4)))
        pm_why[7]++;
    if (g_calls_on && (pm_use & 0x3fff) == 0)
        rt_log("[pm] not skipped: header %u, global %u, base %u, colors %u, indices %u, dirty %u, global colors %u, signature %u\n",
               pm_why[0], pm_why[1], pm_why[2], pm_why[3], pm_why[4], pm_why[5], pm_why[6], pm_why[7]);
    if (m->valid && (int)m->nsig == n && !memcmp(m->sig, pm_sig_tmp, n * 4) && pm_state(gm_, comp, &s) &&
        pm_same(gm_, m, &s, comp)) {
        const uint64_t P = lp(comp + 0x88);
        rt_st32(GP(P + 0x38), rt_ld32(GP(P + 0x38)) + m->dver);
        const uint64_t vec = lp(comp + 0x198), ne = rt_ld64(GP(comp + 0x1a0));
        for (uint64_t j = 0; j < ne; j++) {
            const uint64_t E = vec + 0x58 * j;
            if (rt_ld32(GP(E + 0x2c))) {
                stfp(E + 0x20, pm_tm[j].t);
                rt_st32(GP(E + 0x2c), (uint32_t)pm_tm[j].loops);
            }
        }
        return 1;
    }
    m->valid = 0;
    memcpy(m->sig, pm_sig_tmp, n * 4);
    m->dver = rt_ld32(GP(lp(comp + 0x88) + 0x38));      /* (the version before: pm_after takes the delta) */
    pm_pending = m;
    pm_pending_sig = n;
    return 0;
}
/* after the game's code ran on a component pm_try let through: its result recorded */
static void pm_after(Ctx *c, uint64_t comp)
{
    PalMemo *m = pm_pending;
    pm_pending = NULL;
    if (!m || m->comp != (uint32_t)comp)
        return;
    GM_BEGIN;
    PmState s;
    if (!pm_state(gm_, comp, &s))
        return;
    pm_record(gm_, m, &s);
    m->dver = rt_ld32(GP(lp(comp + 0x88) + 0x38)) - m->dver;
    m->nsig = (uint32_t)pm_pending_sig;
    m->valid = 1;
}

/* MINA_VERIFY_PALSKIP: after a skip, the palette (header but upload flags, colors, indices) and
 * the entries as they are, against the game's code run from the entries as they were */
static void pm_verify(Ctx *c, uint64_t comp, const uint8_t *ents, uint64_t ne)
{
    GM_BEGIN;
    static uint32_t checked, bad;
    static uint8_t mine_e[16 * 0x58], mine_c[PM_COLS * 4], mine_i[PM_COLS], hdr[0x40];
    const uint64_t P = lp(comp + 0x88), vec = lp(comp + 0x198);
    const uint32_t n = rt_ld32(GP(P + 0x28));
    const uint64_t cols = lp(P + 0x18), idx = lp(P + 0x10);
    if (n > PM_COLS)
        return;
    memcpy(mine_e, GP(vec), ne * 0x58);
    memcpy(hdr, GP(P), 0x40);
    if (n && cols)
        memcpy(mine_c, GP(cols), n * 4);
    if (n && idx)
        memcpy(mine_i, GP(idx), n);
    memcpy(GP(vec), ents, ne * 0x58);
    const uint64_t sp = c->sp;
    f_1b156c_orig(c);
    c->sp = sp;
    checked++;
    const char *what = NULL;
    uint8_t h2[0x40];
    memcpy(h2, GP(P), 0x40);
    hdr[0x3c] = hdr[0x3d] = h2[0x3c] = h2[0x3d] = 0;
    if (rt_ld64(GP(comp + 0x1a0)) != ne || memcmp(mine_e, GP(vec), ne * 0x58))
        what = "entries";
    else if (memcmp(hdr, h2, 0x40))
        what = "header";
    else if (n && cols && memcmp(mine_c, GP(cols), n * 4))
        what = "colors";
    else if (n && idx && memcmp(mine_i, GP(idx), n))
        what = "indices";
    if (what && bad++ < 20)
        rt_log("[verify] palette skip of %#llx differs: %s\n", (unsigned long long)comp, what);
    if ((checked & 0x3ff) == 0)
        rt_log("[verify] palette skips: %u checked, %u differ\n", checked, bad);
}

#if defined(__vita__)
#include <malloc.h>
#endif
/* ---- ycTextFont::GetFont: the Chinese, Japanese and Korean fonts on demand ----
 * Game::LoadUpdate loads all 14 fonts of its table (Mach-O 0x100e18f68: name, size, font;
 * 0x18 each) at boot: the six of fonts/japanese, korean, chineseSimplified and
 * chineseTraditional took 26 MiB of the heap for good (Unifontexmono alone ~20), in any
 * language. Their glyphs go into the per-size glyph caches the text draws from; nothing
 * reads their table entries. In another language they wait: loaded (into their entries)
 * when the language becomes one of them (ycTextLocalizer::ReloadLanguage) or the language
 * list opens (OptionsMenuLanguagePopup: the languages' names in their own scripts).
 * MINA_CJK_FONTS=1: all at boot, as the game does. MINA_FONTLOG=1: the fonts and their cost. */
void f_1c12c8_orig(Ctx *c);
#define FONT_TABLE 0x1e18f68u          /* guest address of the table */
#define S_LANGUAGE 0x5ea15d8u          /* (anonymous)::s_language */
static int cjk_waiting[16], ncjk_waiting, cjk_loaded;
static int font_is_cjk(const char *n)
{
    return strstr(n, "fonts/japanese/") || strstr(n, "fonts/korean/") || strstr(n, "fonts/chinese");
}
static int language_is_cjk(void)
{
    const uint32_t l = rt_ld32(G(S_LANGUAGE));
    return l == 5 || l == 6 || l == 9 || l == 10 || l == 11;
}
static void font_get(Ctx *c, uint64_t name, uint32_t size)
{
    c->x[0] = name;
    c->x[1] = size;
#if defined(__vita__)
    struct mallinfo mi0 = mallinfo();
    f_1c12c8_orig(c);
    struct mallinfo mi1 = mallinfo();
    const int d = mi1.uordblks - mi0.uordblks;
#else
    f_1c12c8_orig(c);
    const int d = 0;
#endif
    if (getenv("MINA_FONTLOG"))
        rt_log("[font] %s (%u): %+.1f MiB\n", name ? (const char *)G((uint32_t)name) : "-", size, d / 1048576.0);
}
/* the fonts left waiting, into their table entries (on the game thread) */
static void cjk_fonts_load(Ctx *c, const char *why)
{
    if (cjk_loaded || !ncjk_waiting)
        return;
    cjk_loaded = 1;
    const Ctx save = *c;
    for (int k = 0; k < ncjk_waiting; k++) {
        const uint32_t e = FONT_TABLE + 0x18u * (uint32_t)cjk_waiting[k];
        font_get(c, rt_ld64(G(e)), rt_ld32(G(e + 8)));
        rt_st64(G(e + 0x10), c->x[0]);
    }
    rt_log("[font] %d Chinese, Japanese and Korean fonts loaded (%s)\n", ncjk_waiting, why);
    *c = save;
}
void nat__ZN10ycTextFont7GetFontEPKcj(Ctx *c)
{
    const uint64_t name = c->x[0];
    const uint32_t size = (uint32_t)c->x[1];
    static int all = -1;
    if (all < 0)
        all = getenv("MINA_CJK_FONTS") != NULL;
    if (name && !all && !cjk_loaded && !language_is_cjk() && font_is_cjk((const char *)G((uint32_t)name))) {
        for (int i = 0; i < 14; i++)
            if (rt_ld64(G(FONT_TABLE + 0x18u * (uint32_t)i)) == name && ncjk_waiting < 16) {
                cjk_waiting[ncjk_waiting++] = i;
                c->x[0] = 0;
                return;
            }
    }
    font_get(c, name, size);
}
void f_1c2190_orig(Ctx *c);        /* ycTextLocalizer::ReloadLanguage */
void nat__ZN15ycTextLocalizer14ReloadLanguageEv(Ctx *c)
{
    if (language_is_cjk())
        cjk_fonts_load(c, "the language");
    f_1c2190_orig(c);
}
void f_77c45c_orig(Ctx *c);        /* OptionsMenuLanguagePopup::UpdateState */
void nat__ZN24OptionsMenuLanguagePopup11UpdateStateEv(Ctx *c)
{
    cjk_fonts_load(c, "the language list");
    f_77c45c_orig(c);
}
void cjk_fonts_load_test(Ctx *c) { cjk_fonts_load(c, "test"); }

/* (MINA_STAGE_STATS) a texture upload's staging record: made (AllocStagingBuf), kept for
 * reuse by its texture ($_0) or let go ($_1) after the upload, on the render GC thread */
uint32_t g_stage_made, g_stage_kept, g_stage_gone;
void f_1eccbc_orig(Ctx *c);
void f_1ecd38_orig(Ctx *c);
void nat__ZZN9ycTexture6UploadEPvEN3__08__invokeES0_S0_(Ctx *c) { __atomic_add_fetch(&g_stage_kept, 1, __ATOMIC_RELAXED); f_1eccbc_orig(c); }
void nat__ZZN9ycTexture6UploadEPvEN3__18__invokeES0_S0_(Ctx *c) { __atomic_add_fetch(&g_stage_gone, 1, __ATOMIC_RELAXED); f_1ecd38_orig(c); }

/* ---- ycGpuBuffer::Upload ----------------------------------------------------------------
 * After the copy, a buffer with flag 2 (+0x14) has its upload record (and its 16 KiB staging
 * MTLBuffer) pushed onto a list of its own (+0x20) by the GC thread; nothing ever takes them
 * off it (ycGpuBuffer::Update makes a new one each time). ycDrawUtil's page buffers have it,
 * and here one commits every frame (the black borders around the picture): 60 records and
 * buffers a second, ~50 MiB an hour, the console's slow leak. Their upload goes the other way:
 * record and buffer let go once the copy is done. MINA_GPUBUF_KEEP=1: as the game does.
 * (MINA_STAGE_STATS: the uploads, by flags and size.) */
void f_1e833c_orig(Ctx *c);
void nat__ZN11ycGpuBuffer6UploadEPv(Ctx *c)
{
    static int on = -1, keep = -1;
    if (on < 0) {
        on = getenv("MINA_STAGE_STATS") != NULL;
        keep = getenv("MINA_GPUBUF_KEEP") != NULL;
    }
    const uint32_t fa = (uint32_t)c->x[0] + 0x14;
    const uint8_t f0 = *(uint8_t *)G(fa);
    if (!keep && (f0 & 2)) {
        *(uint8_t *)G(fa) = f0 & ~2;
        f_1e833c_orig(c);
        *(uint8_t *)G(fa) = f0;
        return;
    }
    if (on) {
        static uint32_t n, kept;
        const uint64_t B = c->x[0];
        const uint8_t fl = *(uint8_t *)G((uint32_t)B + 0x14);
        kept += (fl >> 1) & 1;
        if ((++n % 600) == 0)
            rt_log("[gpubuf] %u uploads, %u kept for reuse (last: flags %#x, %u bytes, buffer %#llx)\n", n, kept, fl,
                   rt_ld32(G((uint32_t)B + 0x10)), (unsigned long long)B);
    }
    f_1e833c_orig(c);
}

/* ---- ycUpdateQueue::Remove(ycUpdateQueueItemInfo const&) (x0 the queue Q, x1 its page P, x2 the index) -----
 * The page's items after the index move down one (40 bytes each) and every moved item's info
 * (its page index) goes down by one: found through Q+0x38's paged table when the item has a
 * handle (+0x18, 1-based), else through Q's open hash by the item's key (+0): 64-bit mix,
 * linear probing up to Q+0x50 + 1 slots, values Q+0x70 (16 bytes, the index at +8). The count
 * (P+0x10) goes down; an emptied page is unlinked and freed: that case runs the game's code
 * (nothing moves then). The console's profile: this function in 195 of 459 game-update spikes
 * (a room's entities going at once: every removal moves the whole rest of the page). */
void f_1e5f8c_orig(Ctx *c);
void nat__ZN13ycUpdateQueue6RemoveERK21ycUpdateQueueItemInfo(Ctx *c)
{
    static int off = -1;
    if (off < 0)
        off = rt_native_off("__ZN13ycUpdateQueue6RemoveERK21ycUpdateQueueItemInfo");
    GM_BEGIN;
    const uint64_t Q = c->x[0], P = c->x[1];
    const uint64_t cnt = L64(P + 0x10);
    if (off || cnt <= 1) {
        f_1e5f8c_orig(c);
        return;
    }
    uint64_t idx = c->x[2];
    if (idx + 1 < cnt) {
        uint8_t *items = GP(P + 0x18);
        memmove(items + idx * 40, items + (idx + 1) * 40, (size_t)(cnt - idx - 1) * 40);
        const uint64_t T = L64(Q + 0x38);
        const uint64_t tbl = L64(Q + 0x40), cap = L64(Q + 0x48), probes = L64(Q + 0x50) + 1, vals = L64(Q + 0x70);
        const uint64_t mask = cap - 1, C = 0xd6e8feb86659fd93ull;
        /* the handle table's shape, read once (a 64-bit division per item was a library call
         * on the console's 32-bit CPU) */
        const uint64_t per64 = T ? L64(T + 0x28) : 0, pages = T ? L64(T + 0x40) : 0;
        const uint64_t base = T ? L64(T + 0x10) : 0, stride = T ? L64(T + 0x18) : 0;
        const int small = per64 && per64 <= 0xffffffffu;
        const uint32_t per = (uint32_t)per64;
        const int pow2 = small && !(per & (per - 1));
        const int sh = pow2 ? __builtin_ctz(per) : 0;
        for (; idx + 1 < cnt; idx++) {
            const uint8_t *it = items + idx * 40;
            const uint64_t k = rt_ld64(it + 0x18);
            uint64_t at;
            if (k) {
                uint64_t pg, o;
                const uint64_t j = k - 1;
                if (pow2 && j <= 0xffffffffu) {
                    pg = (uint32_t)j >> sh;
                    o = (uint32_t)j & (per - 1);
                } else if (small && j <= 0xffffffffu) {
                    pg = (uint32_t)j / per;
                    o = (uint32_t)j - (uint32_t)pg * per;
                } else {
                    pg = j / per64;
                    o = j - pg * per64;
                }
                at = L64(pages + pg * 8) + base + stride * o + 8;
            } else {
                const uint64_t key = rt_ld64(it);
                uint64_t h = key ^ (key >> 32);
                h *= C;
                h ^= h >> 32;
                h *= C;
                h ^= h >> 32;
                h &= mask;
                if (tbl)
                    for (uint64_t n = probes;;) {
                        const uint64_t v = L64(tbl + h * 8);
                        if (!v || v == key)
                            break;
                        h = (h + 1) & mask;
                        if (!--n)
                            break;
                    }
                at = vals + h * 16 + 8;
            }
            rt_st64(GP(at), L64(at) - 1);
        }
    }
    rt_st64(GP(P + 0x10), cnt - 1);
}

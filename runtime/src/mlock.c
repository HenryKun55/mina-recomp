/*
 * newlib's malloc lock, wrapped (ld --wrap=__malloc_lock/__malloc_unlock):
 * MINA_MLOCK=1 measures, per guest thread, the time spent waiting for it and holding
 * it (holds and waits over 2 ms), for the [mlock] lines of the [fps] report:
 * on the console, mallocs of 64 bytes took up to 38 ms.
 */
#if defined(__vita__)
#include "runtime.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdlib.h>
#include <string.h>

struct _reent;

void __real___malloc_lock(struct _reent *r);
void __real___malloc_unlock(struct _reent *r);
static int mlock_depth;               /* only the holder touches these */
static uint64_t mlock_t0;
static uint32_t mlock_holder;
static int mlock_measure;            /* MINA_MLOCK, set by mlock_configure (getenv may malloc) */

/* by guest thread id (0: not a guest thread) */
uint64_t g_mlock_wait_us[32], g_mlock_hold_us[32], g_mlock_wait_max[32], g_mlock_hold_max[32];
uint32_t g_mlock_long_holds[32], g_mlock_long_waits[32];

static uint32_t tid_now(void)
{
    Ctx *thread_ctx(void);
    Ctx *c = thread_ctx();
    return c && c->thread_id < 32 ? c->thread_id : 0;
}

void __wrap___malloc_lock(struct _reent *r)
{
    if (!mlock_measure) {
        __real___malloc_lock(r);
        return;
    }
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    __real___malloc_lock(r);
    if (mlock_depth++ == 0) {
        const uint64_t t1 = sceKernelGetProcessTimeWide();
        const uint32_t id = tid_now();
        const uint64_t w = t1 - t0;
        g_mlock_wait_us[id] += w;
        if (w > g_mlock_wait_max[id])
            g_mlock_wait_max[id] = w;
        if (w > 2000)
            g_mlock_long_waits[id]++;
        mlock_t0 = t1;
        mlock_holder = id;
    }
}

void __wrap___malloc_unlock(struct _reent *r)
{
    if (mlock_measure && --mlock_depth == 0) {
        const uint64_t h = sceKernelGetProcessTimeWide() - mlock_t0;
        const uint32_t id = mlock_holder;
        g_mlock_hold_us[id] += h;
        if (h > g_mlock_hold_max[id])
            g_mlock_hold_max[id] = h;
        if (h > 2000)
            g_mlock_long_holds[id]++;
    }
    __real___malloc_unlock(r);
}

/* after the environment is loaded */
void mlock_configure(void)
{
    mlock_measure = getenv("MINA_MLOCK") != NULL;
}

/* the [fps] report: per thread, waits and holds (ms per frame, longest) */
void mlock_report(int frames)
{
    if (mlock_measure <= 0 || frames <= 0)
        return;
    char b[768];
    int n = 0;
    for (int i = 0; i < 32; i++) {
        if (g_mlock_wait_us[i] / frames < 50 && g_mlock_hold_us[i] / frames < 50 && !g_mlock_long_holds[i] &&
            !g_mlock_long_waits[i])
            continue;
        if (n < 680)
            n += snprintf(b + n, sizeof b - (size_t)n, " t%d wait %.2f (max %.1f, %u long) hold %.2f (max %.1f, %u long);",
                          i, g_mlock_wait_us[i] / 1000.0 / frames, g_mlock_wait_max[i] / 1000.0, g_mlock_long_waits[i],
                          g_mlock_hold_us[i] / 1000.0 / frames, g_mlock_hold_max[i] / 1000.0, g_mlock_long_holds[i]);
        g_mlock_wait_us[i] = g_mlock_hold_us[i] = g_mlock_wait_max[i] = g_mlock_hold_max[i] = 0;
        g_mlock_long_holds[i] = g_mlock_long_waits[i] = 0;
    }
    if (n)
        rt_log("[mlock] ms per frame:%s\n", b);
}
#else
void mlock_configure(void) {}
void mlock_report(int frames) { (void)frames; }
#endif

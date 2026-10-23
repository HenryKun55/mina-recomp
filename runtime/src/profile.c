/*
 * Sampling profiler for builds made with -DMINA_PROFILE=ON (gen.py --profile):
 * every recompiled function keeps its index in its thread's Ctx.cur_fn while
 * it runs. With MINA_PROFILE=1 at run time, a thread looks at every guest
 * thread's cur_fn each millisecond and counts it; every 5 s the log gets, per
 * busy thread, the functions it was found in most (their own time, natives
 * included: a native runs under the function it replaces).
 * Unlike timing calls, it costs the measured code two stores per call, so the
 * shares hold on the device too.
 */
#include "runtime.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#endif

#if defined(MINA_PROFILE)

int rt_all_ctx(Ctx ***out);
void rt_log_flush(void);

#define SLOTS 8              /* threads profiled: the first ones seen working */
#define MAXT 128
#define PERIOD_US 1000
#define REPORT_US 5000000
#define TOP 25

static uint16_t *counts;     /* [SLOTS][g_guest_func_count] */
static uint32_t totals[SLOTS];
static int slot_of[MAXT];    /* thread (index in rt_all_ctx) -> slot + 1 (0: none yet) */
static int slot_thread[SLOTS], nslots;
static uint32_t last_fn[MAXT], changes[MAXT];

static void report(void)
{
    Ctx **ctx;
    rt_all_ctx(&ctx);
    for (int s = 0; s < nslots; s++) {
        if (totals[s] < 200)
            continue;
        uint16_t *cnt = counts + (size_t)s * g_guest_func_count;
        rt_log("[prof] thread %u: %u samples\n", ctx[slot_thread[s]]->thread_id, totals[s]);
        for (int k = 0; k < TOP; k++) {
            uint32_t best = 0;
            for (uint32_t f = 1; f < g_guest_func_count; f++)
                if (cnt[f] > cnt[best])
                    best = f;
            if (!cnt[best])
                break;
            rt_log("[prof]   %5.1f%%  %s\n", 100.0 * cnt[best] / totals[s], g_guest_func_names[best]);
            cnt[best] = 0;
        }
        memset(cnt, 0, g_guest_func_count * sizeof *cnt);
        totals[s] = 0;
    }
    rt_log_flush();
}

static void sample_loop(void)
{
    uint32_t since = 0;
    for (;;) {
#if defined(__vita__)
        sceKernelDelayThread(PERIOD_US);
#else
        usleep(PERIOD_US);
#endif
        Ctx **ctx;
        int n = rt_all_ctx(&ctx);
        for (int t = 0; t < n && t < MAXT; t++) {
            const uint32_t f = __atomic_load_n(&ctx[t]->cur_fn, __ATOMIC_RELAXED);
            if (f != last_fn[t]) {           /* working: what it runs changes */
                last_fn[t] = f;
                if (++changes[t] == 50 && !slot_of[t] && nslots < SLOTS) {
                    slot_thread[nslots] = t;
                    slot_of[t] = ++nslots;
                }
            }
            const int s = slot_of[t] - 1;
            if (s >= 0 && f < g_guest_func_count) {
                uint16_t *c = &counts[(size_t)s * g_guest_func_count + f];
                if (*c < 0xffff)
                    (*c)++;
                totals[s]++;
            }
        }
        since += PERIOD_US;
        if (since >= REPORT_US) {
            since = 0;
            report();
        }
    }
}
#if defined(__vita__)
/* a kernel thread of the lowest priority: the Vita does not share a core
 * between threads of one priority, so it must never stand in the game's way */
static int sampler_entry(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    sample_loop();
    return 0;
}
#else
static void *sampler(void *arg)
{
    (void)arg;
    sample_loop();
    return NULL;
}
#endif

void rt_profile_start(void)
{
    if (!getenv("MINA_PROFILE"))
        return;
    counts = calloc((size_t)SLOTS * g_guest_func_count, sizeof *counts);
    if (!counts)
        return;
#if defined(__vita__)
    SceUID t = sceKernelCreateThread("mina_profiler", sampler_entry, 191, 0x10000, 0, SCE_KERNEL_CPU_MASK_USER_ALL, NULL);
    if (t >= 0)
        sceKernelStartThread(t, 0, NULL);
#else
    pthread_t t;
    pthread_create(&t, NULL, sampler, NULL);
#endif
    rt_log("[prof] sampling every %d us, reports every %d s\n", PERIOD_US, REPORT_US / 1000000);
}

#else
void rt_profile_start(void) {}
#endif

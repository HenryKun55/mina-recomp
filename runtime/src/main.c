/* Entry point: loads the game's executable and runs its main(). */
#include "objc_rt.h"

#include <string.h>
#include <unistd.h>

Ctx *threads_main_ctx(uint32_t stack_size);
void metal_init(void);
void run_atexit(Ctx *c);

static uint32_t guest_strv(int n, char **v)
{
    uint32_t arr = gcalloc((uint64_t)n + 1, 8);
    for (int k = 0; k < n; k++)
        rt_st64(G(arr + 8 * (uint32_t)k), gstrdup(v[k]));
    return arr;
}

#if defined(__vita__)
/* the recompiled code keeps its frames on the host stack: give it room */
int sceUserMainThreadStackSize = 8 * 1024 * 1024;
#endif

#if defined(__vita__)
/* No environment on the Vita: ux0:data/mina/env.txt holds KEY=VALUE lines
 * (MINA_TRACE, MINA_MEMSTAT, ...). */
static void load_env_file(void)
{
    FILE *f = fopen("ux0:data/mina/env.txt", "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq)
            continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
    }
    fclose(f);
}
#endif

int main(int argc, char **argv)
{
#if defined(__vita__)
    load_env_file();
#if !defined(MINA_NULL_GL)
    void metal_vita_init(void);
    metal_vita_init();
#endif
#endif
    const char *exe = getenv("MINA_EXE");
    if (!exe)
#if defined(__vita__)
        exe = "ux0:data/mina/Contents/MacOS/MinaTheHollower";
#else
        exe = "gamedata/app/Contents/MacOS/MinaTheHollower";
#endif
    if (getenv("MINA_TRACE"))
        g_trace_imports = 1;

    mem_init();
    imports_init();
    loader_load(exe);
    cpu_init();
    metal_init();
    objc_init();

    Ctx *c = threads_main_ctx(8u << 20);
    void watchdog_start(void);
    watchdog_start();
    if (g_trace_imports) {
        void *probe = malloc(16);
        rt_log("[mem] g_mem %p, heap probe %p, main host sp %p, main ctx %p, guest stack %#x..%#x\n",
               (void *)g_mem, probe, (void *)&c, (void *)c, c->stack_lo, c->stack_hi);
        free(probe);
    }

    /* guest argv[0] is the executable inside the bundle, as on macOS */
    char *gargs[16];
    int gargc = 0;
    gargs[gargc++] = (char *)exe;
    for (int k = 1; k < argc && gargc < 15; k++)
        gargs[gargc++] = argv[k];
    uint32_t gargv = guest_strv(gargc, gargs);
    char *noenv[1] = { NULL };
    uint32_t genvp = guest_strv(0, noenv);
    char *apple_s[1] = { (char *)"executable_path=" };
    uint32_t gapple = guest_strv(1, apple_s);

    for (uint32_t k = 0; k < g_mod_init_count; k++) {
        uint64_t fn = rt_ld64(G(g_mod_init + 8 * k));
        guest_call(c, fn, 4, (uint64_t)gargc, (uint64_t)gargv, (uint64_t)genvp, (uint64_t)gapple);
    }
    uint64_t r = guest_call(c, g_entry, 4, (uint64_t)gargc, (uint64_t)gargv, (uint64_t)genvp, (uint64_t)gapple);
    run_atexit(c);
    return (int)r;
}

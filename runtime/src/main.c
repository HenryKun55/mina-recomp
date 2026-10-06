/* Entry point: loads the game's executable and runs its main(). */
#include "objc_rt.h"

#include <string.h>
#include <unistd.h>

Ctx *threads_main_ctx(uint32_t stack_size);
void run_atexit(Ctx *c);

static uint32_t guest_strv(int n, char **v)
{
    uint32_t arr = gcalloc((uint64_t)n + 1, 8);
    for (int k = 0; k < n; k++)
        rt_st64(G(arr + 8 * (uint32_t)k), gstrdup(v[k]));
    return arr;
}

int main(int argc, char **argv)
{
    const char *exe = getenv("MINA_EXE");
    if (!exe)
        exe = "gamedata/app/Contents/MacOS/MinaTheHollower";
    if (getenv("MINA_TRACE"))
        g_trace_imports = 1;

    mem_init();
    imports_init();
    loader_load(exe);
    cpu_init();
    objc_init();

    Ctx *c = threads_main_ctx(8u << 20);

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

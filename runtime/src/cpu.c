/* Indirect calls, host->guest calls and fatal guest errors. */
#include "runtime.h"

#include <pthread.h>
#include <string.h>
#if !defined(__vita__)
#include <execinfo.h>
#endif

static uint32_t *hash_addr;
static GuestFn *hash_fn;
static uint32_t hash_mask;

#define HASH(a) (((a) >> 2) * 0x9E3779B1u)

void cpu_init(void)
{
    uint32_t size = 1;
    while (size < g_guest_func_count * 2 + g_stub_count * 2)
        size <<= 1;
    hash_mask = size - 1;
    hash_addr = calloc(size, sizeof *hash_addr);
    hash_fn = calloc(size, sizeof *hash_fn);
    for (uint32_t k = 0; k < g_guest_func_count; k++) {
        uint32_t a = g_guest_funcs[k].addr, h = HASH(a) & hash_mask;
        while (hash_addr[h])
            h = (h + 1) & hash_mask;
        hash_addr[h] = a;
        hash_fn[h] = g_guest_funcs[k].fn;
    }
    /* stubs: code that takes the address of a stub calls the import */
    for (uint32_t k = 0; k < g_stub_count; k++) {
        uint32_t a = g_stubs[k].addr, h = HASH(a) & hash_mask;
        while (hash_addr[h])
            h = (h + 1) & hash_mask;
        hash_addr[h] = a;
        hash_fn[h] = g_import_fns[g_stubs[k].import];
    }
}

GuestFn cpu_lookup(uint32_t a)
{
    if (a >= GUEST_THUNK_BASE && a < GUEST_THUNK_END) {
        uint32_t i = (a - GUEST_THUNK_BASE) / 4;
        return i < g_import_count ? g_import_fns[i] : NULL;
    }
    uint32_t h = HASH(a) & hash_mask;
    while (hash_addr[h]) {
        if (hash_addr[h] == a)
            return hash_fn[h];
        h = (h + 1) & hash_mask;
    }
    return NULL;
}

void rt_call(Ctx *c, uint64_t addr)
{
    if (addr >> 32)
        rt_bad_jump(c, 0, addr);
    uint32_t a = (uint32_t)addr;
    if (a >= GUEST_THUNK_BASE && a < GUEST_THUNK_END) {
        uint32_t i = (a - GUEST_THUNK_BASE) / 4;
        c->cur_import = i;
        g_import_fns[i](c);
        return;
    }
    GuestFn fn = cpu_lookup(a);
    if (!fn)
        rt_bad_jump(c, 0, addr);
    fn(c);
}

uint64_t guest_call(Ctx *c, uint64_t addr, int nargs, ...)
{
    va_list ap;
    va_start(ap, nargs);
    for (int k = 0; k < nargs && k < 8; k++)
        c->x[k] = va_arg(ap, uint64_t);
    va_end(ap);
    uint64_t sp = c->sp;
    c->sp = (sp - 256) & ~15ull;   /* stay clear of the caller's red zone */
    rt_call(c, addr);
    c->sp = sp;
    return c->x[0];
}

const char *func_name_at(uint32_t pc, uint32_t *start)
{
    /* g_guest_funcs is sorted by address */
    uint32_t lo = 0, hi = g_guest_func_count;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (g_guest_funcs[mid].addr <= pc)
            lo = mid;
        else
            hi = mid;
    }
    *start = g_guest_funcs[lo].addr;
    return g_guest_func_names[lo];
}

static void report(Ctx *c, uint32_t pc)
{
    uint32_t start = 0;
    const char *name = func_name_at(pc, &start);
    fprintf(stderr, "  at guest pc %#x in %s (function %#x, original %#llx)\n", pc, name, start,
            (unsigned long long)(start - GUEST_IMAGE_BASE + GUEST_ORIG_BASE));
    for (int i = 0; i < 31; i += 4) {
        fprintf(stderr, "  ");
        for (int k = i; k < i + 4 && k < 31; k++)
            fprintf(stderr, "x%-2d=%016llx ", k, (unsigned long long)c->x[k]);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "  sp=%016llx\n", (unsigned long long)c->sp);
#if !defined(__vita__)
    /* host frames of generated functions (f_<offset>) name the guest call chain */
    void *frames[64];
    int n = backtrace(frames, 64);
    char **syms = backtrace_symbols(frames, n);
    fprintf(stderr, "  guest call chain:\n");
    for (int i = 0; i < n; i++) {
        const char *f = strstr(syms[i], " f_");
        if (getenv("MINA_BT")) fprintf(stderr, "    [%s]\n", syms[i]);
        if (!f)
            continue;
        uint32_t off = (uint32_t)strtoul(f + 3, NULL, 16), st;
        const char *nm = func_name_at(off + GUEST_IMAGE_BASE, &st);
        fprintf(stderr, "    %#x %s\n", off + GUEST_IMAGE_BASE, nm);
    }
    free(syms);
#endif
}

void rt_unimpl(Ctx *c, uint32_t pc, const char *what)
{
    fprintf(stderr, "unimplemented: %s\n", what);
    report(c, pc);
    abort();
}

void rt_trap(Ctx *c, uint32_t pc, uint32_t imm)
{
    fprintf(stderr, "guest trap (brk #%u)\n", imm);
    report(c, pc);
    abort();
}

void rt_bad_jump(Ctx *c, uint32_t pc, uint64_t target)
{
    fprintf(stderr, "bad jump to %#llx\n", (unsigned long long)target);
    report(c, pc);
    abort();
}

void rt_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void rt_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    abort();
}

/* Names of the guest functions on the host stack (for diagnostics). */
void guest_backtrace(char *out, size_t cap, int max)
{
    out[0] = 0;
#if !defined(__vita__)
    void *frames[64];
    int n = backtrace(frames, 64), k = 0;
    char **syms = backtrace_symbols(frames, n);
    size_t len = 0;
    for (int i = 0; i < n && k < max; i++) {
        const char *f = strstr(syms[i], " f_");
        if (!f)
            continue;
        uint32_t off = (uint32_t)strtoul(f + 3, NULL, 16), st;
        len += (size_t)snprintf(out + len, len < cap ? cap - len : 0, "%s%s", k ? " < " : "",
                                func_name_at(off + GUEST_IMAGE_BASE, &st));
        k++;
    }
    free(syms);
#else
    (void)cap; (void)max;
#endif
}

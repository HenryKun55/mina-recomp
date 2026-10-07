/* Indirect calls, host->guest calls and fatal guest errors. */
#include "runtime.h"

#include <pthread.h>
#include <string.h>
#if !defined(__vita__)
#include <execinfo.h>
#endif

/* address -> function: open addressing over entries that never change once
 * made (call sites keep pointers to them, see RT_CALL_IC) */
static GuestFunc *hash_ent;
static uint32_t hash_mask;

#define HASH(a) (((a) >> 2) * 0x9E3779B1u)

static void hash_put(uint32_t a, GuestFn fn)
{
    uint32_t h = HASH(a) & hash_mask;
    while (hash_ent[h].addr)
        h = (h + 1) & hash_mask;
    hash_ent[h].addr = a;
    hash_ent[h].fn = fn;
}

void cpu_init(void)
{
    uint32_t size = 1;
    while (size < g_guest_func_count * 2 + g_stub_count * 2)
        size <<= 1;
    hash_mask = size - 1;
    hash_ent = calloc(size, sizeof *hash_ent);
    for (uint32_t k = 0; k < g_guest_func_count; k++)
        hash_put(g_guest_funcs[k].addr, g_guest_funcs[k].fn);
    /* stubs: code that takes the address of a stub calls the import */
    for (uint32_t k = 0; k < g_stub_count; k++)
        hash_put(g_stubs[k].addr, g_import_fns[g_stubs[k].import]);
}

static const GuestFunc *cpu_entry(uint32_t a)
{
    uint32_t h = HASH(a) & hash_mask;
    while (hash_ent[h].addr) {
        if (hash_ent[h].addr == a)
            return &hash_ent[h];
        h = (h + 1) & hash_mask;
    }
    return NULL;
}

GuestFn cpu_lookup(uint32_t a)
{
    if (a >= GUEST_THUNK_BASE && a < GUEST_THUNK_END) {
        uint32_t i = (a - GUEST_THUNK_BASE) / 4;
        return i < g_import_count ? g_import_fns[i] : NULL;
    }
    const GuestFunc *e = cpu_entry(a);
    return e ? e->fn : NULL;
}

void rt_call_ic(Ctx *c, uint64_t addr, const GuestFunc **ic)
{
    uint32_t a = (uint32_t)addr;
    if (!(addr >> 32) && !(a >= GUEST_THUNK_BASE && a < GUEST_THUNK_END)) {
        const GuestFunc *e = cpu_entry(a);
        if (e) {
            __atomic_store_n(ic, e, __ATOMIC_RELAXED);
            e->fn(c);
            return;
        }
    }
    rt_call(c, addr);
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
    if (!fn) {
        /* the host code that asked for the jump (addr2line on the ELF) */
        rt_log("rt_call from host %p\n", __builtin_return_address(0));
        rt_bad_jump(c, 0, addr);
    }
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
    rt_log("  at guest pc %#x in %s (function %#x, original %#llx)\n", pc, name, start,
            (unsigned long long)(start - GUEST_IMAGE_BASE + GUEST_ORIG_BASE));
    for (int i = 0; i < 31; i += 4) {
        rt_log("  ");
        for (int k = i; k < i + 4 && k < 31; k++)
            rt_log("x%-2d=%016llx ", k, (unsigned long long)c->x[k]);
        rt_log("\n");
    }
    rt_log("  sp=%016llx\n", (unsigned long long)c->sp);
#if !defined(__vita__)
    /* host frames of generated functions (f_<offset>) name the guest call chain */
    void *frames[64];
    int n = backtrace(frames, 64);
    char **syms = backtrace_symbols(frames, n);
    rt_log("  guest call chain:\n");
    for (int i = 0; i < n; i++) {
        const char *f = strstr(syms[i], " f_");
        if (getenv("MINA_BT")) rt_log("    [%s]\n", syms[i]);
        if (!f)
            continue;
        uint32_t off = (uint32_t)strtoul(f + 3, NULL, 16), st;
        const char *nm = func_name_at(off + GUEST_IMAGE_BASE, &st);
        rt_log("    %#x %s\n", off + GUEST_IMAGE_BASE, nm);
    }
    free(syms);
#endif
}

void rt_unimpl(Ctx *c, uint32_t pc, const char *what)
{
    rt_log("unimplemented: %s\n", what);
    report(c, pc);
    abort();
}

void rt_trap(Ctx *c, uint32_t pc, uint32_t imm)
{
    rt_log("guest trap (brk #%u)\n", imm);
    report(c, pc);
    abort();
}

void rt_bad_jump(Ctx *c, uint32_t pc, uint64_t target)
{
    rt_log("bad jump to %#llx\n", (unsigned long long)target);
    report(c, pc);
    abort();
}

#if defined(__vita__)
/* No terminal on the Vita: the log goes to ux0:data/mina/mina.log (sceIo
 * directly, so it survives an abort). */
#include <psp2/io/fcntl.h>
static SceUID log_fd = -1;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static void log_write(const char *fmt, va_list ap)
{
    char buf[2048];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0)
        return;
    if (n > (int)sizeof buf - 1)
        n = (int)sizeof buf - 1;
    /* opened and closed every time: what is written survives a crash */
    pthread_mutex_lock(&log_lock);
    int flags = SCE_O_WRONLY | SCE_O_CREAT | (log_fd < 0 ? SCE_O_TRUNC : SCE_O_APPEND);
    SceUID fd = sceIoOpen("ux0:data/mina/mina.log", flags, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, buf, (SceSize)n);
        sceIoClose(fd);
        log_fd = 1;
    }
    pthread_mutex_unlock(&log_lock);
}
#endif

void rt_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
#if defined(__vita__)
    va_start(ap, fmt);
    log_write(fmt, ap);
    va_end(ap);
#endif
}

void rt_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
#if defined(__vita__)
    va_start(ap, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    rt_log("fatal: %s\n", msg);
#endif
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

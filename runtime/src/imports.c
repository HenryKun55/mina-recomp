/* Import table: binds every import of the executable to a host implementation
 * (functions) or to a block of guest memory (data). */
#include "runtime.h"

#include <string.h>

GuestFn g_import_fns[16384];
static uint32_t import_addrs[16384];
int g_trace_imports;

static const ImportImpl *const tables[] = {
    g_impl_libc, g_impl_pthread, g_impl_objc, g_impl_foundation, g_impl_cf, g_impl_misc, g_impl_metal,
};

static const ImportImpl *find_impl(const char *name)
{
    for (size_t t = 0; t < sizeof tables / sizeof tables[0]; t++)
        for (const ImportImpl *p = tables[t]; p->name; p++)
            if (strcmp(p->name, name) == 0)
                return p;
    return NULL;
}

static void imp_missing(Ctx *c)
{
    fprintf(stderr, "unimplemented import %s (x0=%#llx x1=%#llx x2=%#llx)\n",
            g_import_names[c->cur_import], (unsigned long long)c->x[0],
            (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
    abort();
}

/* Imports that are data, by name, when no implementation says otherwise. */
static int looks_like_data(const char *n)
{
    static const char *const prefixes[] = {
        "_OBJC_CLASS_$_", "_OBJC_METACLASS_$_", "_OBJC_EHTYPE_$_", "__NSConcrete", "_kCF", "_kCG",
        "_kTIS", "_kUT", "_kCA", "_kIO", "_NSZero",
    };
    static const char *const suffixes[] = { "Notification", "Key", "Mode", "Modes", "Type" };
    for (size_t k = 0; k < sizeof prefixes / sizeof prefixes[0]; k++)
        if (strncmp(n, prefixes[k], strlen(prefixes[k])) == 0)
            return 1;
    if (strncmp(n, "_NS", 3) == 0) {
        size_t len = strlen(n);
        for (size_t k = 0; k < sizeof suffixes / sizeof suffixes[0]; k++) {
            size_t sl = strlen(suffixes[k]);
            if (len > sl && strcmp(n + len - sl, suffixes[k]) == 0)
                return 1;
        }
    }
    static const char *const exact[] = {
        "___stack_chk_guard", "___stderrp", "___stdinp", "___stdoutp", "__dispatch_main_q",
        "__objc_empty_cache", "___CFConstantStringClassReference", "_NSApp",
        "_NSAppKitVersionNumber", "_mach_task_self_", "_kIOMasterPortDefault",
    };
    for (size_t k = 0; k < sizeof exact / sizeof exact[0]; k++)
        if (strcmp(n, exact[k]) == 0)
            return 1;
    return 0;
}

void imports_init(void)
{
    if (g_import_count > 16384)
        rt_fatal("too many imports");
    for (uint32_t i = 0; i < g_import_count; i++) {
        const char *name = g_import_names[i];
        const ImportImpl *p = find_impl(name);
        if (p && p->data_size) {
            uint32_t a = rt_alloc(p->data_size, 16);
            if (p->init)
                p->init(a);
            import_addrs[i] = a;
            g_import_fns[i] = imp_missing;
        } else if (p) {
            import_addrs[i] = GUEST_THUNK_BASE + 4 * i;
            g_import_fns[i] = p->fn;
        } else if (looks_like_data(name)) {
            import_addrs[i] = rt_alloc(256, 16);
            g_import_fns[i] = imp_missing;
        } else {
            import_addrs[i] = GUEST_THUNK_BASE + 4 * i;
            g_import_fns[i] = imp_missing;
        }
    }
}

uint32_t import_addr(uint32_t i) { return import_addrs[i]; }

int import_index(const char *name)
{
    for (uint32_t i = 0; i < g_import_count; i++)
        if (strcmp(g_import_names[i], name) == 0)
            return (int)i;
    return -1;
}

uint32_t import_data(const char *name)
{
    int i = import_index(name);
    return i < 0 ? 0 : import_addrs[i];
}

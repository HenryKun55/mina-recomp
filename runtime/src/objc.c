/* Minimal Objective-C runtime: classes, messaging, reference counting. */
#include "objc_rt.h"

#include <pthread.h>
#include <string.h>
#include <time.h>

/* ---- small hash map: uint32 key -> pointer -------------------------------- */

typedef struct Map {
    uint32_t *keys;
    void **vals;
    uint32_t cap, count;
} Map;

static void map_put(Map *m, uint32_t k, void *v);
static uint64_t stat_gets, stat_probes;

/* the high bits of the product, folded down: addresses are 16-byte aligned, so the
 * low bits of a plain multiply would leave most slots unused */
static uint32_t mhash(uint32_t k)
{
    k *= 0x9E3779B1u;
    return k ^ (k >> 16);
}

static void *map_get(const Map *m, uint32_t k)
{
    if (!m->cap)
        return NULL;
    uint32_t h = mhash(k) & (m->cap - 1);
    stat_gets++;
    while (m->keys[h]) {
        stat_probes++;
        if (m->keys[h] == k)
            return m->vals[h];
        h = (h + 1) & (m->cap - 1);
    }
    return NULL;
}

static void map_grow(Map *m)
{
    Map n = { 0 };
    n.cap = m->cap ? m->cap * 2 : 1024;
    n.keys = calloc(n.cap, sizeof *n.keys);
    n.vals = calloc(n.cap, sizeof *n.vals);
    for (uint32_t i = 0; i < m->cap; i++)
        if (m->keys[i] && m->vals[i])
            map_put(&n, m->keys[i], m->vals[i]);
    free(m->keys);
    free(m->vals);
    *m = n;
}

static void map_put(Map *m, uint32_t k, void *v)
{
    if ((m->count + 1) * 2 > m->cap)
        map_grow(m);
    uint32_t h = mhash(k) & (m->cap - 1);
    while (m->keys[h] && m->keys[h] != k)
        h = (h + 1) & (m->cap - 1);
    if (!m->keys[h])
        m->count++;
    m->keys[h] = k;
    m->vals[h] = v;
}

/* Deleting shifts the rest of the probe chain back (no tombstones: tables of
 * objects made and freed every frame would fill with them). */
static void map_del(Map *m, uint32_t k)
{
    if (!m->cap)
        return;
    uint32_t mask = m->cap - 1, h = mhash(k) & mask;
    while (m->keys[h] && m->keys[h] != k)
        h = (h + 1) & mask;
    if (!m->keys[h])
        return;
    uint32_t hole = h;
    for (uint32_t j = (h + 1) & mask; m->keys[j]; j = (j + 1) & mask) {
        uint32_t home = mhash(m->keys[j]) & mask;
        /* the entry at j may move to the hole if its home is not in (hole, j] */
        if (((j - home) & mask) >= ((j - hole) & mask)) {
            m->keys[hole] = m->keys[j];
            m->vals[hole] = m->vals[j];
            hole = j;
        }
    }
    m->keys[hole] = 0;
    m->vals[hole] = NULL;
    m->count--;
}

static pthread_mutex_t objc_lock = PTHREAD_MUTEX_INITIALIZER;
static Map class_by_obj, meta_addrs, objects;

/* MINA_OBJSTATS: the object tables (size, lookups, probes per lookup) */
static void map_stats(const char *name, const Map *m)
{
    uint64_t probes = 0;
    for (uint32_t i = 0; i < m->cap; i++)
        if (m->keys[i])
            probes += ((i - (mhash(m->keys[i]) & (m->cap - 1))) & (m->cap - 1)) + 1;
    rt_log("[objstats] %s: %u of %u, %.2f probes per hit\n", name, m->count, m->cap,
           m->count ? (double)probes / m->count : 0.0);
}
void objc_stats(void)
{
    map_stats("objects", &objects);
    map_stats("classes", &class_by_obj);
    map_stats("metaclasses", &meta_addrs);
    rt_log("[objstats] %llu lookups, %.2f probes each\n", (unsigned long long)stat_gets,
           stat_gets ? (double)stat_probes / stat_gets : 0.0);
    stat_gets = stat_probes = 0;
}
/* class_by_obj: class object / metaclass -> Class; meta_addrs: metaclass address ->
 * Class (class-method lookups); objects: object address -> Obj */
static Class *classes[512];
static int nclasses;

/* ---- selectors ----------------------------------------------------------------- */

typedef struct SelEntry {
    const char *name;
    uint32_t addr;
    struct SelEntry *next;
} SelEntry;

static SelEntry *sel_table[4096];

static uint32_t strhash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void sel_add(const char *name, uint32_t addr)
{
    uint32_t h = strhash(name) & 4095;
    for (SelEntry *e = sel_table[h]; e; e = e->next)
        if (strcmp(e->name, name) == 0)
            return;
    SelEntry *e = malloc(sizeof *e);
    e->name = name;
    e->addr = addr;
    e->next = sel_table[h];
    sel_table[h] = e;
}

uint32_t objc_sel(const char *name)
{
    uint32_t h = strhash(name) & 4095;
    pthread_mutex_lock(&objc_lock);
    for (SelEntry *e = sel_table[h]; e; e = e->next)
        if (strcmp(e->name, name) == 0) {
            pthread_mutex_unlock(&objc_lock);
            return e->addr;
        }
    size_t n = strlen(name) + 1;
    uint32_t a = rt_alloc((uint32_t)n, 1);
    memcpy(G(a), name, n);
    sel_add((const char *)G(a), a);
    pthread_mutex_unlock(&objc_lock);
    return a;
}

/* ---- classes ------------------------------------------------------------------ */

extern Class *const g_classes_foundation[];
extern Class *const g_classes_metal[];

Class *objc_class_named(const char *name)
{
    for (int i = 0; i < nclasses; i++)
        if (strcmp(classes[i]->name, name) == 0)
            return classes[i];
    return NULL;
}

static Class *placeholder_class(const char *name, uint32_t obj)
{
    Class *k = calloc(1, sizeof *k);
    k->name = strdup(name);
    k->super_name = strcmp(name, "NSObject") ? "NSObject" : NULL;
    k->obj = obj;
    k->instance_size = 16;
    classes[nclasses++] = k;
    map_put(&class_by_obj, obj, k);
    return k;
}

void objc_register(Class *k)
{
    if (nclasses >= (int)(sizeof classes / sizeof classes[0]))
        rt_fatal("too many classes");
    char imp[256];
    snprintf(imp, sizeof imp, "_OBJC_CLASS_$_%s", k->name);
    uint32_t a = import_data(imp);
    if (!a) {
        a = rt_alloc(64, 16);
    }
    k->obj = a;
    if (!k->instance_size)
        k->instance_size = 16;
    classes[nclasses++] = k;
    map_put(&class_by_obj, a, k);
}

static void link_supers(void)
{
    for (int i = 0; i < nclasses; i++) {
        Class *k = classes[i];
        if (!k->guest && k->super_name && !k->super)
            k->super = objc_class_named(k->super_name);
    }
}

/* class_ro_t of a guest class_t */
static uint32_t class_ro(uint32_t cls) { return (uint32_t)(rt_ld64(G(cls + 32)) & ~7ull); }

static void parse_guest_classes(void)
{
    for (uint32_t s = 0; s < g_objc_classlist_count; s++) {
        uint32_t cls = (uint32_t)rt_ld64(G(g_objc_classlist + 8 * s));
        uint32_t ro = class_ro(cls);
        uint32_t meta = (uint32_t)rt_ld64(G(cls));
        Class *k = calloc(1, sizeof *k);
        k->name = gstr(rt_ld64(G(ro + 24)));
        k->guest = 1;
        k->obj = cls;
        k->meta = meta;
        k->instance_size = rt_ld32(G(ro + 8));
        k->guest_imethods = (uint32_t)rt_ld64(G(ro + 32));
        k->guest_cmethods = (uint32_t)rt_ld64(G(class_ro(meta) + 32));
        classes[nclasses++] = k;
        map_put(&class_by_obj, cls, k);
        map_put(&meta_addrs, meta, k);
    }
    /* superclasses: a guest class_t or an imported class */
    for (int i = 0; i < nclasses; i++) {
        Class *k = classes[i];
        if (!k->guest)
            continue;
        uint32_t sup = (uint32_t)rt_ld64(G(k->obj + 8));
        Class *s = sup ? map_get(&class_by_obj, sup) : NULL;
        if (!s && sup) {
            /* imported class we do not implement: name it from the import table */
            const char *nm = "?";
            for (uint32_t j = 0; j < g_import_count; j++)
                if (import_addr(j) == sup && strncmp(g_import_names[j], "_OBJC_CLASS_$_", 14) == 0)
                    nm = g_import_names[j] + 14;
            s = placeholder_class(nm, sup);
            s->super = objc_class_named("NSObject");
        }
        k->super = s;
    }
}

static void collect_selectors(void)
{
    const char *p = (const char *)G(g_objc_methname);
    const char *end = p + g_objc_methname_size;
    while (p < end) {
        size_t n = strlen(p);
        sel_add(p, guest_addr(p));
        p += n + 1;
    }
}

void objc_init(void)
{
    for (Class *const *p = g_classes_foundation; *p; p++)
        objc_register(*p);
    for (Class *const *p = g_classes_metal; *p; p++)
        objc_register(*p);
    /* imported classes nobody implements: placeholders (messages to them are logged) */
    for (uint32_t j = 0; j < g_import_count; j++) {
        const char *n = g_import_names[j];
        if (strncmp(n, "_OBJC_CLASS_$_", 14) == 0 && !objc_class_named(n + 14))
            placeholder_class(n + 14, import_addr(j));
    }
    link_supers();
    parse_guest_classes();
    link_supers();
    collect_selectors();
    /* constant strings (__cfstring) have ___CFConstantStringClassReference as isa */
    uint32_t cs = import_data("___CFConstantStringClassReference");
    if (cs)
        map_put(&class_by_obj, cs, objc_class_named("__NSCFConstantString"));
}

Class *objc_class_of(uint64_t a)
{
    if (!a)
        return NULL;
    Class *k = map_get(&class_by_obj, (uint32_t)rt_ld64(G(a)));
    return k;
}

int objc_is_class_object(uint64_t a)
{
    return map_get(&class_by_obj, (uint32_t)a) != NULL || map_get(&meta_addrs, (uint32_t)a) != NULL;
}

/* ---- method lookup ---------------------------------------------------------------- */

typedef struct Imp {
    GuestFn host;
    uint32_t guest;
} Imp;

static int guest_list_find(uint32_t list, uint32_t sel, uint32_t *imp)
{
    if (!list)
        return 0;
    uint32_t ef = rt_ld32(G(list)), count = rt_ld32(G(list + 4));
    uint32_t entsize = ef & 0xFFFC;
    int relative = (ef & 0x80000000u) != 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t e = list + 8 + i * entsize;
        uint32_t name, impa;
        if (relative) {
            int32_t no = (int32_t)rt_ld32(G(e)), io = (int32_t)rt_ld32(G(e + 8));
            name = (uint32_t)rt_ld64(G(e + no));       /* offset to a selector reference */
            impa = e + 8 + io;
        } else {
            name = (uint32_t)rt_ld64(G(e));
            impa = (uint32_t)rt_ld64(G(e + 16));
        }
        if (name == sel || strcmp(gstr(name), gstr(sel)) == 0) {
            *imp = impa;
            return 1;
        }
    }
    return 0;
}

static int host_list_find(const Method *m, const char *sel, GuestFn *fn)
{
    for (; m && m->sel; m++)
        if (strcmp(m->sel, sel) == 0) {
            *fn = m->fn;
            return 1;
        }
    return 0;
}

static int lookup(Class *k, int meta, uint32_t sel, Imp *out)
{
    const char *name = gstr(sel);
    for (Class *c = k; c; c = c->super) {
        if (c->guest) {
            if (guest_list_find(meta ? c->guest_cmethods : c->guest_imethods, sel, &out->guest)) {
                out->host = NULL;
                return 1;
            }
        } else if (host_list_find(meta ? c->cmethods : c->imethods, name, &out->host)) {
            out->guest = 0;
            return 1;
        }
    }
    if (!meta) {
        for (Class *c = k; c; c = c->super)
            if (!c->guest && c->fallback) {
                out->host = c->fallback;
                out->guest = 0;
                return 1;
            }
    }
    if (meta) {
        /* class objects also answer the root class's instance methods */
        Class *root = objc_class_named("NSObject");
        if (root && host_list_find(root->imethods, name, &out->host)) {
            out->guest = 0;
            return 1;
        }
    }
    return 0;
}

/* (class, meta, selector) -> Imp cache */
typedef struct CacheEnt {
    Class *cls;
    uint32_t sel;
    int meta;
    Imp imp;
    struct CacheEnt *next;
} CacheEnt;
static CacheEnt *cache[8192];

static int cached_lookup_ent(Class *k, int meta, uint32_t sel, Imp *out, const CacheEnt **ent)
{
    uint32_t h = (uint32_t)(((uintptr_t)k >> 4) * 31u + sel * 2654435761u + (uint32_t)meta) & 8191;
    /* lock-free readers: entries are published with release, read with acquire */
    for (CacheEnt *e = __atomic_load_n(&cache[h], __ATOMIC_ACQUIRE); e; e = e->next)
        if (e->cls == k && e->sel == sel && e->meta == meta) {
            *out = e->imp;
            if (ent)
                *ent = e;
            return out->host || out->guest;
        }
    Imp imp = { 0, 0 };
    int found = lookup(k, meta, sel, &imp);
    pthread_mutex_lock(&objc_lock);
    CacheEnt *e = malloc(sizeof *e);
    e->cls = k;
    e->sel = sel;
    e->meta = meta;
    e->imp = imp;
    e->next = cache[h];
    __atomic_store_n(&cache[h], e, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&objc_lock);
    *out = imp;
    if (ent)
        *ent = e;
    return found;
}
static int cached_lookup(Class *k, int meta, uint32_t sel, Imp *out) { return cached_lookup_ent(k, meta, sel, out, NULL); }

static void nil_result(Ctx *c)
{
    c->x[0] = c->x[1] = 0;
    for (int i = 0; i < 4; i++)
        c->v[i] = vzero();
}

static void dispatch(Ctx *c, Class *k, int meta, uint32_t sel)
{
    Imp imp;
    if (!k) {
        rt_log("objc: message %s to an object of unknown class (%#llx)\n", gstr(sel),
               (unsigned long long)c->x[0]);
        nil_result(c);
        return;
    }
    if (!cached_lookup(k, meta, sel, &imp)) {
        rt_log("objc: unrecognized selector %c[%s %s] (sel %#x, receiver %#llx)\n", meta ? '+' : '-', k->name,
               gstr(sel), sel, (unsigned long long)c->x[0]);
        nil_result(c);
        return;
    }
    if (g_trace_imports)
        rt_log("objc: %c[%s %s]\n", meta ? '+' : '-', k->name, gstr(sel));
    if (imp.host)
        imp.host(c);
    else
        rt_call(c, imp.guest);
}

/* tagged host objects: see objc_rt.h (objc_obj_tagged) */
uint32_t g_obj_lo = 0xFFFFFFFFu, g_obj_hi;
#define obj_tagged objc_obj_tagged
#define obj_tag objc_obj_tag

static void resolve_receiver(uint64_t recv, Class **k, int *meta)
{
    Obj *o = obj_tagged(recv);       /* a host object (no class object is tagged) */
    if (o) {
        *k = o->cls;
        *meta = 0;
        return;
    }
    Class *cl = map_get(&class_by_obj, (uint32_t)recv);
    if (cl) {
        *k = cl;
        *meta = 1;
        return;
    }
    cl = map_get(&meta_addrs, (uint32_t)recv);
    if (cl) {
        *k = cl;
        *meta = 1;
        return;
    }
    *k = map_get(&class_by_obj, (uint32_t)rt_ld64(G(recv)));
    *meta = 0;
}

static void resolve_receiver(uint64_t recv, Class **k, int *meta);

/* A selector stub of the game (_objc_msgSend$sel): the method cache entry of
 * its last receiver class stays with the stub (entries never change), so a
 * repeat send is the class check and the call. */
void objc_msgSend_site(Ctx *c, uint32_t sel, const void **site)
{
    uint64_t recv = c->x[0];
    if (!recv) {
        nil_result(c);
        return;
    }
    Class *k;
    int meta;
    resolve_receiver(recv, &k, &meta);
    const CacheEnt *e = __atomic_load_n((const CacheEnt **)site, __ATOMIC_RELAXED);
    if (!e || e->cls != k || e->meta != meta || e->sel != sel || g_trace_imports) {
        Imp imp;
        if (!k || !cached_lookup_ent(k, meta, sel, &imp, &e)) {
            dispatch(c, k, meta, sel);           /* reports it */
            return;
        }
        __atomic_store_n((const CacheEnt **)site, e, __ATOMIC_RELAXED);
        if (g_trace_imports) {
            dispatch(c, k, meta, sel);
            return;
        }
    }
    if (e->imp.host)
        e->imp.host(c);
    else
        rt_call(c, e->imp.guest);
}

static void imp_objc_msgSend(Ctx *c)
{
    uint64_t recv = c->x[0];
    if (!recv) {
        nil_result(c);
        return;
    }
    Class *k;
    int meta;
    resolve_receiver(recv, &k, &meta);
    dispatch(c, k, meta, (uint32_t)c->x[1]);
}

/* objc_msgSendSuper2(struct objc_super *{receiver, current class}, sel, ...) */
static void imp_objc_msgSendSuper2(Ctx *c)
{
    uint32_t sup = (uint32_t)c->x[0];
    uint64_t recv = rt_ld64(G(sup));
    uint32_t cur = (uint32_t)rt_ld64(G(sup + 8));
    Class *k = map_get(&class_by_obj, cur);
    int meta = 0;
    if (!k) {
        k = map_get(&meta_addrs, cur);
        meta = 1;
    }
    c->x[0] = recv;
    if (!k || !k->super) {
        nil_result(c);
        return;
    }
    dispatch(c, k->super, meta, (uint32_t)c->x[1]);
}

uint64_t objc_send(Ctx *c, uint64_t recv, const char *sel, int nargs, ...)
{
    if (!recv)
        return 0;
    va_list ap;
    va_start(ap, nargs);
    for (int k = 0; k < nargs && k < 6; k++)
        c->x[2 + k] = va_arg(ap, uint64_t);
    va_end(ap);
    c->x[0] = recv;
    c->x[1] = objc_sel(sel);
    uint64_t sp = c->sp;
    c->sp = (sp - 256) & ~15ull;
    imp_objc_msgSend(c);
    c->sp = sp;
    return c->x[0];
}

uint64_t block_call(Ctx *c, uint64_t block, int nargs, ...)
{
    va_list ap;
    va_start(ap, nargs);
    c->x[0] = block;
    for (int k = 0; k < nargs && k < 6; k++)
        c->x[1 + k] = va_arg(ap, uint64_t);
    va_end(ap);
    uint64_t sp = c->sp;
    c->sp = (sp - 256) & ~15ull;
    rt_call(c, rt_ld64(G(block + 16)));
    c->sp = sp;
    return c->x[0];
}

/* Block_copy: blocks built on the stack are copied to the heap before they
 * outlive their frame. Layout: isa, flags (i32), reserved, invoke, descriptor
 * {reserved, size, copy helper, dispose helper}. */
#define BLOCK_REFCOUNT_MASK 0xfffeu
#define BLOCK_NEEDS_FREE (1u << 24)
#define BLOCK_HAS_COPY_DISPOSE (1u << 25)
#define BLOCK_IS_GLOBAL (1u << 28)

/* Heap copies are reference counted in their flags (2 per reference, as in
 * libclosure): copying one again only counts it, releasing the last reference
 * runs the dispose helper (which releases what the block captured). */
uint64_t block_copy(Ctx *c, uint64_t block)
{
    if (!block)
        return 0;
    uint32_t *fl = G(block + 8);
    uint32_t flags = __atomic_load_n(fl, __ATOMIC_ACQUIRE);
    if (flags & BLOCK_IS_GLOBAL)
        return block;
    if (flags & BLOCK_NEEDS_FREE) {
        __atomic_add_fetch(fl, 2, __ATOMIC_SEQ_CST);
        return block;
    }
    uint64_t desc = rt_ld64(G(block + 24));
    uint64_t size = rt_ld64(G(desc + 8));
    if (size < 32 || size > 4096)
        rt_fatal("block_copy: odd block size %llu", (unsigned long long)size);
    uint32_t heap = gmalloc(size);
    memcpy(G(heap), G(block), size);
    rt_st32(G(heap + 8), (flags & ~BLOCK_REFCOUNT_MASK) | BLOCK_NEEDS_FREE | 2);
    if (flags & BLOCK_HAS_COPY_DISPOSE) {
        uint64_t copy = rt_ld64(G(desc + 16));
        guest_call(c, copy, 2, (uint64_t)heap, block);
    }
    return heap;
}

void block_release(Ctx *c, uint64_t block)
{
    if (!block)
        return;
    uint32_t *fl = G(block + 8);
    uint32_t flags = __atomic_load_n(fl, __ATOMIC_ACQUIRE);
    if (!(flags & BLOCK_NEEDS_FREE))
        return;                               /* global or stack block */
    if ((__atomic_sub_fetch(fl, 2, __ATOMIC_SEQ_CST) & BLOCK_REFCOUNT_MASK) != 0)
        return;
    if (flags & BLOCK_HAS_COPY_DISPOSE) {
        uint64_t desc = rt_ld64(G(block + 24));
        uint64_t dispose = rt_ld64(G(desc + 24));
        if (dispose)
            guest_call(c, dispose, 1, block);
    }
    gfree((uint32_t)block);
}

/* ---- objects ---------------------------------------------------------------------- */

static Class *host_ancestor(Class *k)
{
    while (k && k->guest)
        k = k->super;
    return k;
}

/* MINA_NEWSTATS: objects made per second, by class */
static void new_stats(Class *k)
{
    static int on = -1;
    static Class *ks[128];
    static uint32_t n[128], nk;
    static time_t t0;
    if (on < 0)
        on = getenv("MINA_NEWSTATS") != NULL;
    if (!on)
        return;
    uint32_t i = 0;
    while (i < nk && ks[i] != k)
        i++;
    if (i == nk && nk < 128)
        ks[nk++] = k;
    if (i < 128)
        n[i]++;
    time_t t = time(NULL);
    if (!t0)
        t0 = t;
    if (t - t0 >= 4) {
        for (uint32_t j = 0; j < nk; j++)
            if (n[j])
                rt_log("[new] %s: %u per s (host %u bytes)\n", ks[j]->name, n[j] / (uint32_t)(t - t0), ks[j]->host_size);
        memset(n, 0, sizeof n);
        t0 = t;
    }
}

/* Objects of host classes are made again from their class's pool: the game
 * makes and frees descriptors by the hundred per frame (a depth-stencil
 * descriptor per draw). A pooled object keeps its guest block, its Obj, its
 * host state and its entry in the object table; it is untagged and its count
 * is far below zero (a stray release of a stale pointer never frees it). */
#define POOL_MAX 256
#define POOL_DEAD (-0x40000000)
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t objc_new_object(Class *k)
{
    new_stats(k);
    uint32_t size = k->instance_size < 16 ? 16 : k->instance_size;
    int tagged = !k->guest && size <= 16;
    if (tagged)
        size = 24;
    if (tagged && k->pool) {
        pthread_mutex_lock(&pool_lock);
        Obj *o = k->pool;
        if (o) {
            k->pool = o->next;
            k->pooled--;
        }
        pthread_mutex_unlock(&pool_lock);
        if (o) {
            uint32_t a = o->addr;
            rt_st64(G(a), k->obj);
            o->refs = 1;
            o->traced = 0;
            o->next = NULL;
            if (o->host) {
                uint32_t hs = 0, hz = 0;
                for (Class *c = k; c; c = c->super)
                    if (!c->guest && c->host_size > hs) {
                        hs = c->host_size;
                        hz = c->host_zero;
                    }
                memset(o->host, 0, hz && hz < hs ? hz : hs);
            }
            uint64_t p = (uint64_t)(uintptr_t)o;
            memcpy(G(a + 8), &p, 8);
            rt_st32(G(a + 16), obj_tag(a, o));
            return a;
        }
    }
    uint32_t a = gcalloc(1, size);
    rt_st64(G(a), k->obj);
    Obj *o = calloc(1, sizeof *o);
    o->addr = a;
    o->cls = k;
    o->refs = 1;
    {   /* MINA_TRACE_CLASS=name: retains, releases and deallocs of that class's objects */
        static const char *tc = (const char *)1;
        if (tc == (const char *)1)
            tc = getenv("MINA_TRACE_CLASS");
        if (tc && !strcmp(tc, k->name)) {
            o->traced = 1;
            objc_trace_ref(o, "new");
        }
    }
    /* host state: the biggest host_size along the chain (subclasses extend it) */
    uint32_t hs = 0, hz = 0;
    for (Class *c = k; c; c = c->super)
        if (!c->guest && c->host_size > hs) {
            hs = c->host_size;
            hz = c->host_zero;
        }
    if (hs) {
        o->host = malloc(hs);
        memset(o->host, 0, hz && hz < hs ? hz : hs);
    }
    pthread_mutex_lock(&objc_lock);
    map_put(&objects, a, o);
    pthread_mutex_unlock(&objc_lock);
    if (tagged) {
        pthread_mutex_lock(&objc_lock);
        if (a < g_obj_lo)
            g_obj_lo = a;
        if (a > g_obj_hi)
            g_obj_hi = a;
        pthread_mutex_unlock(&objc_lock);
        uint64_t p = (uint64_t)(uintptr_t)o;
        memcpy(G(a + 8), &p, 8);
        rt_st32(G(a + 16), obj_tag(a, o));
    }
    return a;
}

Obj *objc_obj(uint64_t a)
{
    if (!a)
        return NULL;
    Obj *t = obj_tagged(a);
    if (t)
        return t;
    pthread_mutex_lock(&objc_lock);
    Obj *o = map_get(&objects, (uint32_t)a);
    pthread_mutex_unlock(&objc_lock);
    {   /* MINA_OBJSTATS: untagged lookups, by class */
        static int on = -1;
        if (on < 0)
            on = getenv("MINA_OBJSTATS") != NULL;
        if (on) {
            static const char *names[64];
            static uint32_t n[64], nn, total;
            const char *nm = o ? o->cls->name : "(none)";
            uint32_t i = 0;
            while (i < nn && names[i] != nm)
                i++;
            if (i == nn && nn < 64)
                names[nn++] = nm;
            if (i < 64)
                n[i]++;
            if ((++total & 0xffff) == 0)
                for (uint32_t j = 0; j < nn; j++)
                    rt_log("[untagged] %s: %u\n", names[j], n[j]);
        }
    }
    return o;
}

void *objc_host_slow(uint64_t a)
{
    Obj *o = objc_obj(a);
    return o ? o->host : NULL;
}

void objc_trace_ref(Obj *o, const char *op)
{
    char bt[512];
    guest_backtrace(bt, sizeof bt, 4);
    rt_log("[ref] %s %#x %s -> %d: %s\n", op, o->addr, o->cls->name, o->refs, bt);
}

uint64_t objc_retain_obj(uint64_t a)
{
    Obj *o = objc_obj(a);
    if (o) {
        __atomic_add_fetch(&o->refs, 1, __ATOMIC_SEQ_CST);
        if (o->traced)
            objc_trace_ref(o, "retain");
    }
    return a;
}

static void destroy(Ctx *c, uint64_t a)
{
    /* -dealloc may be a guest method (which ends in [super dealloc]) */
    objc_send(c, a, "dealloc", 0);
}

void objc_release_obj(uint64_t a)
{
    Obj *o = objc_obj(a);
    if (!o)
        return;
    if (o->traced)
        objc_trace_ref(o, "release");
    if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_SEQ_CST) == 0)
        destroy(thread_ctx(), a);
}

/* NSObject -dealloc: host destructors along the chain, then the memory. */
static void m_dealloc(Ctx *c)
{
    uint64_t a = SELF;
    Obj *o = objc_obj(a);
    if (!o)
        return;
    if (o->traced)
        objc_trace_ref(o, "dealloc");
    for (Class *k = o->cls; k; k = k->super)
        if (!k->guest && k->dealloc && o->host)
            k->dealloc(o->host);
    const int tagged = rt_ld32(G(a + 16)) == obj_tag((uint32_t)a, o);
    if (tagged)
        rt_st32(G(a + 16), 0);                  /* untagged before the memory is reused */
    int poolable = 0;
    for (Class *k = o->cls; k && !poolable; k = k->super)
        poolable = k->poolable;
    if (tagged && poolable && o->cls->pooled < POOL_MAX && !o->cls->guest) {
        o->refs = POOL_DEAD;
        pthread_mutex_lock(&pool_lock);
        o->next = o->cls->pool;
        o->cls->pool = o;
        o->cls->pooled++;
        pthread_mutex_unlock(&pool_lock);
        return;
    }
    pthread_mutex_lock(&objc_lock);
    map_del(&objects, (uint32_t)a);
    pthread_mutex_unlock(&objc_lock);
    free(o->host);
    free(o);
    gfree((uint32_t)a);
}

/* ---- autorelease pools (per thread) --------------------------------------------- */

#if defined(__vita__)
#define pool (host_tls()->pool)
#define pool_n (host_tls()->pool_n)
#define pool_cap (host_tls()->pool_cap)
#else
static __thread uint64_t *pool;
static __thread uint32_t pool_n, pool_cap;
#endif

uint64_t objc_autorelease_obj(uint64_t a)
{
    if (!a)
        return 0;
    Obj *to = objc_obj(a);
    if (to && to->traced)
        objc_trace_ref(to, "autorelease");
    if (pool_n == pool_cap) {
        pool_cap = pool_cap ? pool_cap * 2 : 256;
        pool = realloc(pool, pool_cap * sizeof *pool);
    }
    pool[pool_n++] = a;
    return a;
}

static void imp_pool_push(Ctx *c) { RET((uint64_t)pool_n + 1); }
static void imp_pool_pop(Ctx *c)
{
    uint32_t mark = (uint32_t)A0 - 1;
    while (pool_n > mark) {
        uint64_t a = pool[--pool_n];
        objc_release_obj(a);
    }
}

/* ---- objc_* imports ------------------------------------------------------------------ */

static uint64_t alloc_for(uint64_t clsobj)
{
    Class *k = map_get(&class_by_obj, (uint32_t)clsobj);
    if (!k)
        rt_fatal("objc_alloc: unknown class object %#llx", (unsigned long long)clsobj);
    return objc_new_object(k);
}

static void imp_objc_alloc(Ctx *c) { RET(alloc_for(A0)); }
static void imp_objc_alloc_init(Ctx *c)
{
    uint64_t o = alloc_for(A0);
    RET(objc_send(c, o, "init", 0));
}
static void imp_objc_retain(Ctx *c) { RET(objc_retain_obj(A0)); }
static void imp_objc_release(Ctx *c) { objc_release_obj(A0); }
static void imp_objc_autorelease(Ctx *c) { RET(objc_autorelease_obj(A0)); }
static void imp_objc_retainAutorelease(Ctx *c) { RET(objc_autorelease_obj(objc_retain_obj(A0))); }
static void imp_objc_retainAutoreleasedReturnValue(Ctx *c) { RET(objc_retain_obj(A0)); }
static void imp_objc_unsafeClaim(Ctx *c) { (void)c; }
static void imp_objc_storeStrong(Ctx *c)
{
    uint64_t loc = A0, obj = A1;
    objc_retain_obj(obj);
    uint64_t old = rt_ld64(G(loc));
    rt_st64(G(loc), obj);
    objc_release_obj(old);
}
static void imp_objc_storeWeak(Ctx *c) { rt_st64(G(A0), A1); RET(A1); }
static void imp_objc_loadWeakRetained(Ctx *c) { RET(objc_retain_obj(rt_ld64(G(A0)))); }
static void imp_objc_destroyWeak(Ctx *c) { rt_st64(G(A0), 0); }
static void imp_objc_opt_class(Ctx *c)
{
    if (!A0) { RET(0); return; }
    if (objc_is_class_object(A0)) return;
    RET(rt_ld64(G(A0)));
}
static int is_kind(uint64_t obj, uint64_t clsobj)
{
    Class *want = map_get(&class_by_obj, (uint32_t)clsobj);
    for (Class *k = objc_class_of(obj); k; k = k->super)
        if (k == want)
            return 1;
    return 0;
}
static void imp_objc_opt_isKindOfClass(Ctx *c) { RET(A0 ? is_kind(A0, A1) : 0); }
static void imp_objc_opt_respondsToSelector(Ctx *c)
{
    if (!A0) { RET(0); return; }
    Class *k;
    int meta;
    resolve_receiver(A0, &k, &meta);
    Imp imp;
    RET(k && cached_lookup(k, meta, (uint32_t)A1, &imp));
}
static pthread_mutex_t sync_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static void imp_objc_sync_enter(Ctx *c) { pthread_mutex_lock(&sync_lock); RET(0); }
static void imp_objc_sync_exit(Ctx *c) { pthread_mutex_unlock(&sync_lock); RET(0); }
static void imp_objc_fatal(Ctx *c)
{
    rt_fatal("objc runtime function %s is not supported", g_import_names[c->cur_import]);
}

/* ---- NSObject ------------------------------------------------------------------------ */

static void m_alloc(Ctx *c) { RET(alloc_for(SELF)); }
static void m_new(Ctx *c)
{
    uint64_t o = alloc_for(SELF);
    RET(objc_send(c, o, "init", 0));
}
static void m_self(Ctx *c) { (void)c; }
static void m_retain(Ctx *c) { RET(objc_retain_obj(SELF)); }
static void m_release(Ctx *c) { objc_release_obj(SELF); }
static void m_autorelease(Ctx *c) { RET(objc_autorelease_obj(SELF)); }
static void m_class(Ctx *c)
{
    if (!objc_is_class_object(SELF))
        RET(rt_ld64(G(SELF)));
}
static void m_isKindOfClass(Ctx *c) { RET(is_kind(SELF, MARG(0))); }
static void m_respondsToSelector(Ctx *c)
{
    Class *k;
    int meta;
    resolve_receiver(SELF, &k, &meta);
    Imp imp;
    RET(k && cached_lookup(k, meta, (uint32_t)MARG(0), &imp));
}
static void m_hash(Ctx *c) { (void)c; }
static void m_isEqual(Ctx *c) { RET(SELF == MARG(0)); }
static void m_performSelector(Ctx *c)
{
    uint64_t self = SELF, sel = MARG(0);
    c->x[0] = self;
    c->x[1] = sel;
    imp_objc_msgSend(c);
}
static void m_zero(Ctx *c) { RET(0); }

static const Method nsobject_i[] = {
    { "init", m_self }, { "self", m_self }, { "retain", m_retain }, { "release", m_release },
    { "autorelease", m_autorelease }, { "dealloc", m_dealloc }, { "class", m_class },
    { "isKindOfClass:", m_isKindOfClass }, { "respondsToSelector:", m_respondsToSelector },
    { "hash", m_hash }, { "isEqual:", m_isEqual }, { "performSelector:", m_performSelector },
    { "copy", m_retain }, { "conformsToProtocol:", m_zero },
    { NULL, NULL },
};
static const Method nsobject_c[] = {
    { "alloc", m_alloc }, { "allocWithZone:", m_alloc }, { "new", m_new },
    { NULL, NULL },
};
Class cls_NSObject = { "NSObject", NULL, nsobject_i, nsobject_c, 0, NULL };

const ImportImpl g_impl_objc[] = {
    { "_objc_msgSend", imp_objc_msgSend, 0, NULL },
    { "_objc_msgSendSuper2", imp_objc_msgSendSuper2, 0, NULL },
    { "_objc_alloc", imp_objc_alloc, 0, NULL },
    { "_objc_alloc_init", imp_objc_alloc_init, 0, NULL },
    { "_objc_retain", imp_objc_retain, 0, NULL },
    { "_objc_release", imp_objc_release, 0, NULL },
    { "_objc_autorelease", imp_objc_autorelease, 0, NULL },
    { "_objc_autoreleaseReturnValue", imp_objc_autorelease, 0, NULL },
    { "_objc_retainAutorelease", imp_objc_retainAutorelease, 0, NULL },
    { "_objc_retainAutoreleaseReturnValue", imp_objc_retainAutorelease, 0, NULL },
    { "_objc_retainAutoreleasedReturnValue", imp_objc_retainAutoreleasedReturnValue, 0, NULL },
    { "_objc_unsafeClaimAutoreleasedReturnValue", imp_objc_unsafeClaim, 0, NULL },
    { "_objc_autoreleasePoolPush", imp_pool_push, 0, NULL },
    { "_objc_autoreleasePoolPop", imp_pool_pop, 0, NULL },
    { "_objc_storeStrong", imp_objc_storeStrong, 0, NULL },
    { "_objc_storeWeak", imp_objc_storeWeak, 0, NULL },
    { "_objc_loadWeakRetained", imp_objc_loadWeakRetained, 0, NULL },
    { "_objc_destroyWeak", imp_objc_destroyWeak, 0, NULL },
    { "_objc_opt_class", imp_objc_opt_class, 0, NULL },
    { "_objc_opt_isKindOfClass", imp_objc_opt_isKindOfClass, 0, NULL },
    { "_objc_opt_respondsToSelector", imp_objc_opt_respondsToSelector, 0, NULL },
    { "_objc_sync_enter", imp_objc_sync_enter, 0, NULL },
    { "_objc_sync_exit", imp_objc_sync_exit, 0, NULL },
    { "_objc_enumerationMutation", imp_objc_fatal, 0, NULL },
    { "_objc_begin_catch", imp_objc_fatal, 0, NULL },
    { "_objc_end_catch", imp_objc_fatal, 0, NULL },
    { NULL, NULL, 0, NULL },
};

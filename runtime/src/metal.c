/*
 * Metal on OpenGL. See runtime/include/metal_gl.h for the model.
 *
 * Encoders execute GL immediately (holding the GPU lock from creation to
 * endEncoding); command buffers only run their completion handlers and
 * present their drawable at commit.
 */
#include "metal_gl.h"
#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#endif

#include <SDL.h>
#include <pthread.h>
#include <string.h>

SDL_Window *sdl_main_window(void);
SDL_GLContext sdl_gl_context(void);
const char *ns_cstr(uint64_t s);
uint64_t ns_string_auto(const char *s);
uint64_t ns_array_new(void);
void ns_array_add(uint64_t arr, uint64_t item);

/* ---- GPU lock -------------------------------------------------------------------- */

static pthread_mutex_t gpu_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static pthread_t gl_owner;
static int gl_owned;
static SDL_Window *gl_window;

#if defined(__vita__) && !defined(MINA_NULL_GL)
/* vitaGL starts on the main thread at boot, before the game takes memory */
#include <vitashark.h>
static void shark_log(const char *msg, shark_log_level level, int line)
{
    static int n;
    if (n++ < 200)
        rt_log("[shacccg] %s (line %d): %s\n", level == SHARK_LOG_ERROR ? "error" : level == SHARK_LOG_WARNING ? "warning" : "info",
               line, msg);
}

void metal_vita_init(void)
{
    void mem_free_report(void);
    mem_free_report();           /* before vitaGL takes its share */
    vglInitExtended(0x800000, 960, 544, 0x1000000, SCE_GXM_MULTISAMPLE_NONE);
    shark_install_log_cb(shark_log);
    shark_set_warnings_level(SHARK_WARN_HIGH);
    static const char *names[] = { "vram", "ram", "phycont", "cdlg", "newlib" };
    for (int k = 0; k < 5; k++)
        rt_log("[vgl] %s: %.1f of %.1f MiB free\n", names[k], vglMemFree((vglMemType)k) / 1048576.0,
               vglMemTotal((vglMemType)k) / 1048576.0);
    mem_free_report();
}
#endif

void gpu_lock(void)
{
    pthread_mutex_lock(&gpu_mutex);
#if defined(__vita__)
#if !defined(MINA_NULL_GL)
#endif
    return;
#endif
    SDL_Window *w = sdl_main_window();
    if (!gl_owned || !pthread_equal(gl_owner, pthread_self()) || w != gl_window) {
        gl_window = w;
        if (SDL_GL_MakeCurrent(sdl_main_window(), sdl_gl_context()) != 0)
            rt_fatal("SDL_GL_MakeCurrent: %s", SDL_GetError());
        gl_owner = pthread_self();
        gl_owned = 1;
    }
}
void gpu_unlock(void) { pthread_mutex_unlock(&gpu_mutex); }

/* ---- property bags --------------------------------------------------------------------- */

static Class cls_PropBag;

static PropBag *bag(uint64_t obj) { return objc_host(obj); }

/* Interned strings: one permanent copy of each key. Insert-only, so readers need
 * no lock (an entry's string is published before its hash). */
#define INTERN_CAP 4096
static struct { uint32_t hash; const char *s; } interned[INTERN_CAP];
static pthread_mutex_t intern_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t str_hash(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h | 1;
}
static const char *intern_n(const char *s, size_t n)
{
    uint32_t h = str_hash(s, n);
    for (int locked = 0;; locked = 1) {
        uint32_t i = h & (INTERN_CAP - 1);
        for (uint32_t eh; (eh = __atomic_load_n(&interned[i].hash, __ATOMIC_ACQUIRE)); i = (i + 1) & (INTERN_CAP - 1))
            if (eh == h && strncmp(interned[i].s, s, n) == 0 && interned[i].s[n] == 0) {
                if (locked)
                    pthread_mutex_unlock(&intern_lock);
                return interned[i].s;
            }
        if (locked) {
            char *copy = malloc(n + 1);
            memcpy(copy, s, n);
            copy[n] = 0;
            interned[i].s = copy;
            __atomic_store_n(&interned[i].hash, h, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&intern_lock);
            return copy;
        }
        pthread_mutex_lock(&intern_lock);
    }
}
static const char *intern(const char *s) { return intern_n(s, strlen(s)); }

const char *pb_key(const char *s) { return intern(s); }

/* "@<n>" and "<n>" without printf; "@0".."@63" kept */
static const char *index_key_slow(const char *prefix, uint64_t n);
static const char *index_key(const char *prefix, uint64_t n)
{
    static const char *at[64];
    if (prefix[0] == '@' && !prefix[1] && n < 64) {
        const char *k = __atomic_load_n(&at[n], __ATOMIC_ACQUIRE);
        if (!k)
            __atomic_store_n(&at[n], k = index_key_slow(prefix, n), __ATOMIC_RELEASE);
        return k;
    }
    return index_key_slow(prefix, n);
}
static const char *index_key_slow(const char *prefix, uint64_t n)
{
    char k[32], *q = k + sizeof k;
    *--q = 0;
    do
        *--q = (char)('0' + n % 10);
    while (n /= 10);
    for (size_t pl = strlen(prefix); pl; pl--)
        *--q = prefix[pl - 1];
    return intern(q);
}

static int pb_find(PropBag *b, const char *key)
{
    for (int i = 0; i < b->n; i++)
        if (b->p[i].key == key)      /* interned */
            return i;
    return -1;
}
static int pb_slot(PropBag *b, const char *key)
{
    int i = pb_find(b, key);
    if (i >= 0)
        return i;
    if (b->n >= PB_MAX)
        rt_fatal("property bag full (%s)", key);
    i = b->n++;
    b->p[i].key = key;                   /* interned by the caller */
    b->p[i].v = 0;
    b->di[i] = -1;
    return i;
}
static double *pb_dslot(PropBag *b, int i)
{
    if (b->di[i] < 0) {
        if (b->nd >= PB_MAX_D)
            rt_fatal("property bag: too many floating-point properties (%s)", b->p[i].key);
        b->di[i] = (int8_t)b->nd++;
    }
    return b->d[b->di[i]];
}
uint64_t pb_get_k(uint64_t obj, const char *key, uint64_t def)
{
    PropBag *b = obj ? bag(obj) : NULL;
    int i = b ? pb_find(b, key) : -1;
    return i < 0 ? def : b->p[i].v;
}
double pb_getd_k(uint64_t obj, const char *key, int k, double def)
{
    PropBag *b = obj ? bag(obj) : NULL;
    int i = b ? pb_find(b, key) : -1;
    return i < 0 ? def : b->di[i] < 0 ? 0.0 : b->d[b->di[i]][k];
}
void pb_set_k(uint64_t obj, const char *key, uint64_t v)
{
    PropBag *b = bag(obj);
    b->p[pb_slot(b, key)].v = v;
}

static void bag_dealloc(void *h)
{
    PropBag *b = h;
    for (int i = 0; i < b->n; i++)
        if (b->p[i].key[0] == '@' || (b->held >> i & 1))
            objc_release_obj(b->p[i].v);
}

/* What a selector means to a bag, worked out once per selector (insert-only
 * table keyed by the selector's address, readers without a lock). */
enum { SEL_GET, SEL_SET, SEL_SET_F64, SEL_SET_F32, SEL_OTHER };
#define SELMAP_CAP 2048
static struct { uint32_t sel; uint8_t kind; int8_t child; const char *key; } selmap[SELMAP_CAP];
static pthread_mutex_t selmap_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *const fp_props[] = { "clearColor", "clearDepth", "lodMinClamp", "lodMaxClamp", NULL };

static int child_of_key(const char *key);
static int sel_info_at(uint64_t sel_addr, const char **key, int *child)
{
    uint32_t a = (uint32_t)sel_addr, i = (a * 0x9E3779B1u >> 21) & (SELMAP_CAP - 1);
    for (uint32_t e; (e = __atomic_load_n(&selmap[i].sel, __ATOMIC_ACQUIRE)); i = (i + 1) & (SELMAP_CAP - 1))
        if (e == a) {
            *key = selmap[i].key;
            if (child)
                *child = selmap[i].child;
            return selmap[i].kind;
        }
    /* setFoo: / isFoo / foo */
    const char *sel = gstr(sel_addr);
    size_t n = strlen(sel);
    int kind;
    const char *k;
    if (strncmp(sel, "set", 3) == 0 && n > 4 && strchr(sel, ':') == sel + n - 1) {
        char buf[96];
        size_t kl = n - 4 < sizeof buf ? n - 4 : sizeof buf - 1;
        memcpy(buf, sel + 3, kl);
        buf[kl] = 0;
        buf[0] += ('a' - 'A') * (buf[0] >= 'A' && buf[0] <= 'Z');
        k = intern(buf);
        kind = SEL_SET;
        for (const char *const *p = fp_props; *p; p++)
            if (strcmp(*p, k) == 0)
                kind = strncmp(k, "lod", 3) == 0 ? SEL_SET_F32 : SEL_SET_F64;
    } else if (strchr(sel, ':')) {
        k = sel;
        kind = SEL_OTHER;
    } else if (strncmp(sel, "is", 2) == 0 && sel[2] >= 'A' && sel[2] <= 'Z') {
        char buf[96];
        size_t kl = n - 2 < sizeof buf ? n - 2 : sizeof buf - 1;
        memcpy(buf, sel + 2, kl);
        buf[kl] = 0;
        buf[0] += 'a' - 'A';
        k = intern(buf);
        kind = SEL_GET;
    } else {
        k = intern(sel);
        kind = SEL_GET;
    }
    pthread_mutex_lock(&selmap_lock);
    for (uint32_t e; (e = selmap[i].sel) && e != a; i = (i + 1) & (SELMAP_CAP - 1))
        ;
    const int ch = kind == SEL_GET ? child_of_key(k) : -1;
    if (!selmap[i].sel) {
        selmap[i].kind = (uint8_t)kind;
        selmap[i].child = (int8_t)ch;
        selmap[i].key = k;
        __atomic_store_n(&selmap[i].sel, a, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&selmap_lock);
    *key = k;
    if (child)
        *child = ch;
    return kind;
}
static int sel_info(uint64_t sel_addr, const char **key) { return sel_info_at(sel_addr, key, NULL); }

static int seq_left = -1;              /* MINA_SEQLOG: messages logged after a stencil REPLACE */
static void seq_log(Ctx *c, const char *where)
{
    if (seq_left < 0)
        seq_left = getenv("MINA_SEQLOG") ? 0 : -2;
    if (seq_left <= 0)
        return;
    Class *k = objc_class_of(SELF);
    if (!k || !strstr(k->name, "Stencil"))
        return;
    seq_left--;
    rt_log("[seq] %s %s %#llx %s %#llx\n", where, k ? k->name : "?", (unsigned long long)SELF, gstr(c->x[1]),
           (unsigned long long)MARG(0));
}
static int bag_debug = -1;            /* MINA_SEQLOG or MINA_BAGLOG */
static void bag_message(Ctx *c, int kind, const char *key);
/* the debug logs of a message to a bag */
static void bag_fallback_debug(Ctx *c)
{
    if (seq_left == 0 && strcmp(gstr(c->x[1]), "setDepthStencilPassOperation:") == 0 &&
        MARG(0) == 2) {
        static int once;
        if (!once++)
            seq_left = 400;
    }
    seq_log(c, "set");
    const char *key;
    int kind = sel_info(c->x[1], &key);
    PropBag *b = bag(SELF);
    {   /* MINA_BAGLOG=name: the messages to bags of classes whose name contains it */
        static const char *bl = (const char *)1;
        if (bl == (const char *)1)
            bl = getenv("MINA_BAGLOG");
        static struct { uint32_t sel; uint64_t arg; } seen[256];
        static int n;
        if (bl) {
            Class *k = objc_class_of(SELF);
            if (k && strstr(k->name, bl)) {
                int dup = 0;
                for (int i = 0; i < n && !dup; i++)
                    dup = seen[i].sel == (uint32_t)c->x[1] && seen[i].arg == MARG(0);
                if (!dup && n < 256) {
                    seen[n].sel = (uint32_t)c->x[1];
                    seen[n++].arg = MARG(0);
                    rt_log("[bag] %s %s %#llx\n", k->name, gstr(c->x[1]), (unsigned long long)MARG(0));
                }
            }
        }
    }
    (void)kind;
    (void)b;
}
/* as a Metal descriptor does, an object set is retained until it is
 * replaced or the bag goes (a function the game released after setting it
 * was still read from the descriptor) */
static void bag_set_value(PropBag *b, const char *key, uint64_t v)
{
    const int i = pb_slot(b, key);
    const uint64_t old = b->p[i].v, bit = 1ull << i;
    const int had = (b->held & bit) != 0, obj = v && objc_obj(v) != NULL;
    if (obj)
        objc_retain_obj(v);
    b->p[i].v = v;
    b->held = obj ? b->held | bit : b->held & ~bit;
    if (had)
        objc_release_obj(old);
}
static void bag_message(Ctx *c, int kind, const char *key)
{
    PropBag *b = bag(SELF);
    switch (kind) {
    case SEL_SET:
        bag_set_value(b, key, MARG(0));
        return;
    case SEL_SET_F64:
    case SEL_SET_F32: {
        double *d = pb_dslot(b, pb_slot(b, key));
        for (int k = 0; k < 4; k++)
            d[k] = kind == SEL_SET_F32 ? (double)c->v[k].f[0] : c->v[k].df[0];
        return;
    }
    case SEL_OTHER:
        rt_log("metal: unhandled %s on %s\n", key, objc_class_of(SELF)->name);
        RET(0);
        return;
    }
    int i = pb_find(b, key);
    RET(i < 0 ? 0 : b->p[i].v);
    if (i >= 0 && b->di[i] >= 0)
        for (int k = 0; k < 4; k++)
            c->v[k] = vset_f64(b->d[b->di[i]][k]);
}

/* children: getters that return a sub-descriptor */
typedef struct ChildDef {
    Class *owner;
    const char *key;
    Class *child;
    Class *elem;             /* for arrays */
    const char *at_key;      /* "@<key>", interned */
} ChildDef;
static ChildDef children[32];
static int nchildren;

uint64_t pb_child_at(uint64_t obj, const char *at_key, Class *cls)
{
    PropBag *b = bag(obj);
    int i = pb_find(b, at_key);
    if (i >= 0)
        return b->p[i].v;
    uint64_t ch = objc_new_object(cls);
    b->p[pb_slot(b, at_key)].v = ch;
    return ch;
}
static const char *at_key(const char *key)
{
    char k[64];
    k[0] = '@';
    size_t n = strlen(key);
    if (n > sizeof k - 2)
        n = sizeof k - 2;
    memcpy(k + 1, key, n);
    k[n + 1] = 0;
    return intern(k);
}

static int child_of_key(const char *key)
{
    for (int i = 0; i < nchildren; i++)
        if (children[i].key == key)
            return i;
    return -1;
}
/* the sub-descriptor key names on obj's class, made on first use */
static int bag_child(uint64_t obj, const char *key, int first, uint64_t *out)
{
    Obj *self_o = objc_obj_tagged(obj);
    Class *k = self_o ? self_o->cls : objc_class_of(obj);
    for (int i = first; i < nchildren; i++) {
        if (children[i].key != key)
            continue;
        for (Class *o = k; o; o = o->super)
            if (children[i].owner == o) {
                uint64_t ch = pb_child_at(obj, children[i].at_key, children[i].child);
                if (children[i].elem && !bag(ch)->n)    /* a new array */
                    pb_set(ch, "__elem", (uint64_t)(uintptr_t)children[i].elem);
                *out = ch;
                return 1;
            }
    }
    return 0;
}

/* For natives that read and write descriptors as the game's messages would
 * (metal_natives.c); keys are interned (PB_K) */
uint64_t md_get(uint64_t obj, const char *key) { return obj ? pb_get_k(obj, key, 0) : 0; }
void md_set(uint64_t obj, const char *key, uint64_t v)
{
    if (obj)
        bag_set_value(bag(obj), key, v);
}
uint64_t md_child(uint64_t obj, const char *key)
{
    uint64_t ch;
    if (!obj)
        return 0;
    return bag_child(obj, key, child_of_key(key), &ch) ? ch : pb_get_k(obj, key, 0);
}
uint64_t md_elem(uint64_t arr, uint64_t i)
{
    if (!arr)
        return 0;
    Class *elem = (Class *)(uintptr_t)pb_get(arr, "__elem", 0);
    return pb_child_at(arr, index_key("@", i), elem);
}
static uint64_t bag_hash_of(uint64_t obj, int depth);
uint64_t md_hash(uint64_t obj) { return obj ? bag_hash_of(obj, 0) : 0; }

static void bag_child_getter(Ctx *c)
{
    if (bag_debug < 0)
        bag_debug = getenv("MINA_SEQLOG") || getenv("MINA_BAGLOG");
    if (bag_debug)
        seq_log(c, "get");
    const char *key;
    int first;
    const int kind = sel_info_at(c->x[1], &key, &first);
    uint64_t ch;
    if (kind == SEL_GET && first >= 0 && bag_child(SELF, key, first, &ch)) {     /* a sub-descriptor getter */
        RET(ch);
        return;
    }
    if (bag_debug)
        bag_fallback_debug(c);
    bag_message(c, kind, key);
}

static void array_object_at(Ctx *c)
{
    Class *elem = (Class *)(uintptr_t)pb_get(SELF, "__elem", 0);
    RET(pb_child_at(SELF, index_key("@", MARG(0)), elem));
}
static void array_set_object(Ctx *c)
{
    PropBag *b = bag(SELF);
    int i = pb_slot(b, index_key("@", MARG(1)));
    objc_retain_obj(MARG(0));
    if (b->p[i].v)
        objc_release_obj(b->p[i].v);
    b->p[i].v = MARG(0);
}

/* element of a descriptor array if it exists (no creation) */
static uint64_t pb_elem(uint64_t arr, int i) { return arr ? pb_get_k(arr, index_key("@", (uint64_t)i), 0) : 0; }
#define pb_existing_child(obj, key) ((obj) ? pb_get_k(obj, PB_K("@" key), 0) : 0)

/* -hash and -isEqual: by content, as Metal's descriptors have them: the game
 * keeps one depth/stencil descriptor, changes it per draw and makes a new
 * state only when its hash changes (ycRenderDevice::DrawCall); hashed by
 * address, every draw of a pass got the state of its first one */
static uint64_t bag_hash_of(uint64_t obj, int depth)
{
    PropBag *b = bag(obj);
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < b->n; i++) {
        uint64_t v = b->p[i].v;
        if (b->p[i].key[0] == '@' && v && depth < 4)
            v = bag_hash_of(v, depth + 1);
        const uint64_t kh = (uint64_t)(uintptr_t)b->p[i].key;     /* interned */
        /* order-independent: the same properties set in another order hash the same */
        uint64_t x = (kh * 0x9E3779B97F4A7C15ull) ^ (v * 0xC2B2AE3D27D4EB4Full);
        x ^= x >> 29;
        h += x * 0xBF58476D1CE4E5B9ull;
        if (b->di[i] >= 0)
            for (int k = 0; k < 4; k++) {
                uint64_t d;
                memcpy(&d, &b->d[b->di[i]][k], 8);
                h += (d ^ kh) * 0x94D049BB133111EBull;
            }
    }
    return h;
}
static void bag_hash(Ctx *c) { RET(bag_hash_of(SELF, 0)); }
static void bag_is_equal(Ctx *c)
{
    uint64_t o = MARG(0);
    Class *a = objc_class_of(SELF), *b = o ? objc_class_of(o) : NULL;
    RET(o == SELF || (a && a == b && bag_hash_of(SELF, 0) == bag_hash_of(o, 0)));
}
static const Method bag_methods[] = { { "hash", bag_hash }, { "isEqual:", bag_is_equal }, { NULL, NULL } };
static const Method array_methods[] = {
    { "objectAtIndexedSubscript:", array_object_at }, { "setObject:atIndexedSubscript:", array_set_object },
    { NULL, NULL },
};
static Class cls_PropBag = { "MTLPropBag", "NSObject", bag_methods, NULL, sizeof(PropBag), bag_dealloc, bag_child_getter,
                              .host_zero = offsetof(PropBag, p), .poolable = 1 };
static Class cls_BagArray = { "MTLBagArray", "MTLPropBag", array_methods, NULL, 0, NULL };

#define BAG_CLASS(var, name) static Class var = { name, "MTLPropBag", NULL, NULL, 0, NULL }
BAG_CLASS(cls_TextureDesc, "MTLTextureDescriptor");
BAG_CLASS(cls_SamplerDesc, "MTLSamplerDescriptor");
BAG_CLASS(cls_DepthStencilDesc, "MTLDepthStencilDescriptor");
BAG_CLASS(cls_StencilDesc, "MTLStencilDescriptor");
BAG_CLASS(cls_PipelineDesc, "MTLRenderPipelineDescriptor");
BAG_CLASS(cls_PipelineColorDesc, "MTLRenderPipelineColorAttachmentDescriptor");
BAG_CLASS(cls_VertexDesc, "MTLVertexDescriptor");
BAG_CLASS(cls_VertexAttrDesc, "MTLVertexAttributeDescriptor");
BAG_CLASS(cls_VertexLayoutDesc, "MTLVertexBufferLayoutDescriptor");
BAG_CLASS(cls_PipelineBufferDesc, "MTLPipelineBufferDescriptor");
BAG_CLASS(cls_PassDesc, "MTLRenderPassDescriptor");
BAG_CLASS(cls_PassColorDesc, "MTLRenderPassColorAttachmentDescriptor");
BAG_CLASS(cls_PassDepthDesc, "MTLRenderPassDepthAttachmentDescriptor");
BAG_CLASS(cls_PassStencilDesc, "MTLRenderPassStencilAttachmentDescriptor");

static void define_children(void)
{
    ChildDef defs[] = {
        { &cls_DepthStencilDesc, "frontFaceStencil", &cls_StencilDesc, NULL },
        { &cls_DepthStencilDesc, "backFaceStencil", &cls_StencilDesc, NULL },
        { &cls_PipelineDesc, "vertexDescriptor", &cls_VertexDesc, NULL },
        { &cls_PipelineDesc, "colorAttachments", &cls_BagArray, &cls_PipelineColorDesc },
        /* their mutability: the game sets it when it is not immutable yet, and
         * makes a new pipeline state when it sets anything (nil here, every draw) */
        { &cls_PipelineDesc, "vertexBuffers", &cls_BagArray, &cls_PipelineBufferDesc },
        { &cls_PipelineDesc, "fragmentBuffers", &cls_BagArray, &cls_PipelineBufferDesc },
        { &cls_VertexDesc, "attributes", &cls_BagArray, &cls_VertexAttrDesc },
        { &cls_VertexDesc, "layouts", &cls_BagArray, &cls_VertexLayoutDesc },
        { &cls_PassDesc, "colorAttachments", &cls_BagArray, &cls_PassColorDesc },
        { &cls_PassDesc, "depthAttachment", &cls_PassDepthDesc, NULL },
        { &cls_PassDesc, "stencilAttachment", &cls_PassStencilDesc, NULL },
    };
    memcpy(children, defs, sizeof defs);
    nchildren = (int)(sizeof defs / sizeof defs[0]);
    for (int i = 0; i < nchildren; i++) {
        children[i].key = intern(children[i].key);    /* compared with sel_info's keys */
        children[i].at_key = at_key(children[i].key);
    }
}

/* +[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:width:height:mipmapped:] */
static void td_texture2D(Ctx *c)
{
    uint64_t d = objc_new_object(&cls_TextureDesc);
    pb_set(d, "pixelFormat", MARG(0));
    pb_set(d, "width", MARG(1));
    pb_set(d, "height", MARG(2));
    uint32_t mips = 1;
    if (MARG(3) & 1) {
        uint64_t m = MARG(1) > MARG(2) ? MARG(1) : MARG(2);
        while (m > 1) { m >>= 1; mips++; }
    }
    pb_set(d, "mipmapLevelCount", mips);
    RET(objc_autorelease_obj(d));
}
static void pd_renderPassDescriptor(Ctx *c) { RET(objc_autorelease_obj(objc_new_object(&cls_PassDesc))); }
static const Method td_cmethods[] = {
    { "texture2DDescriptorWithPixelFormat:width:height:mipmapped:", td_texture2D }, { NULL, NULL },
};
static const Method pd_cmethods[] = { { "renderPassDescriptor", pd_renderPassDescriptor }, { NULL, NULL } };

/* ---- formats ----------------------------------------------------------------------------- */

typedef struct GLFmt {
    GLenum internal, format, type;
    int bpp;                 /* bytes per pixel (0: block compressed) */
    int block_bytes;         /* bytes per 4x4 block */
    int depth, stencil, srgb;
} GLFmt;

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM
#define GL_COMPRESSED_RGBA_BPTC_UNORM 0x8E8C
#endif

static int gl_format(uint32_t mtl, GLFmt *f)
{
    memset(f, 0, sizeof *f);
    switch (mtl) {
#if defined(__vita__)
    /* vitaGL makes GL_R8 a U8_R texture that glTexSubImage2D cannot write:
     * U8_RRRR (every channel = the value) samples the same for .r and .a */
    case 1:   *f = (GLFmt){ GL_INTENSITY, GL_RED, GL_UNSIGNED_BYTE, 1 }; return 1;    /* A8Unorm */
    case 10:  *f = (GLFmt){ GL_INTENSITY, GL_RED, GL_UNSIGNED_BYTE, 1 }; return 1;   /* R8Unorm */
#else
    case 1:   *f = (GLFmt){ GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1 }; return 1;           /* A8Unorm */
    case 10:  *f = (GLFmt){ GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1 }; return 1;
#endif
    case 25:  *f = (GLFmt){ GL_R16F, GL_RED, GL_HALF_FLOAT, 2 }; return 1;
    case 30:  *f = (GLFmt){ GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2 }; return 1;
    case 55:  *f = (GLFmt){ GL_R32F, GL_RED, GL_FLOAT, 4 }; return 1;
    case 65:  *f = (GLFmt){ GL_RG16F, GL_RG, GL_HALF_FLOAT, 4 }; return 1;
    case 70:  *f = (GLFmt){ GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4 }; return 1;
    case 71:  *f = (GLFmt){ GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, 0, 0, 0, 1 }; return 1;
    case 80:  *f = (GLFmt){ GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, 4 }; return 1;
    case 81:  *f = (GLFmt){ GL_SRGB8_ALPHA8, GL_BGRA, GL_UNSIGNED_BYTE, 4, 0, 0, 0, 1 }; return 1;
    case 90:  *f = (GLFmt){ GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, 4 }; return 1;
    case 92:  *f = (GLFmt){ GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV, 4 }; return 1;
    case 105: *f = (GLFmt){ GL_RG32F, GL_RG, GL_FLOAT, 8 }; return 1;
    case 110: *f = (GLFmt){ GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT, 8 }; return 1;
    case 115: *f = (GLFmt){ GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 8 }; return 1;
    case 125: *f = (GLFmt){ GL_RGBA32F, GL_RGBA, GL_FLOAT, 16 }; return 1;
    case 130: case 131: *f = (GLFmt){ GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 0, 0, 0, 8 }; return 1;
    case 132: case 133: *f = (GLFmt){ GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0, 0, 16 }; return 1;
    case 134: case 135: *f = (GLFmt){ GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0, 0, 16 }; return 1;
    case 152: case 153: *f = (GLFmt){ GL_COMPRESSED_RGBA_BPTC_UNORM, 0, 0, 0, 16 }; return 1;
    case 250: *f = (GLFmt){ GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 2, 0, 1 }; return 1;
    case 252: *f = (GLFmt){ GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 4, 0, 1 }; return 1;
    case 253: *f = (GLFmt){ GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4, 0, 1, 1 }; return 1;
    case 255: *f = (GLFmt){ GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4, 0, 1, 1 }; return 1;
    case 260: *f = (GLFmt){ GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, 8, 0, 1, 1 }; return 1;
    }
    return 0;
}

/* ---- device ------------------------------------------------------------------------------- */

static Class cls_Device, cls_Queue, cls_CmdBuf, cls_Buffer, cls_Texture,
    cls_Sampler, cls_DepthStencil, cls_Library, cls_Function, cls_Pipeline, cls_Layer, cls_Drawable;

static uint64_t device;

static void imp_MTLCreateSystemDefaultDevice(Ctx *c)
{
    if (!device)
        device = objc_new_object(&cls_Device);
    RET(objc_retain_obj(device));
}

static void d_name(Ctx *c) { RET(ns_string_auto("OpenGL (mina-recomp)")); }
static void d_newCommandQueue(Ctx *c) { RET(objc_new_object(&cls_Queue)); }
#if defined(__vita__)
static void d_supportsSampleCount(Ctx *c) { RET(MARG(0) == 1); }
#else
static void d_supportsSampleCount(Ctx *c) { RET(MARG(0) == 1 || MARG(0) == 2 || MARG(0) == 4); }
#endif
static void d_true(Ctx *c) { RET(1); }
static void d_maxBufferLength(Ctx *c) { RET(256u << 20); }

/* live buffers, for the memory report */
static MBuffer *live_bufs[8192];
static int nlive_bufs;
static pthread_mutex_t bufs_lock = PTHREAD_MUTEX_INITIALIZER;
static void buf_track(MBuffer *b, int add)
{
    pthread_mutex_lock(&bufs_lock);
    if (add) {
        if (nlive_bufs < 8192)
            live_bufs[nlive_bufs++] = b;
    } else {
        for (int i = 0; i < nlive_bufs; i++)
            if (live_bufs[i] == b) {
                live_bufs[i] = live_bufs[--nlive_bufs];
                break;
            }
    }
    pthread_mutex_unlock(&bufs_lock);
}

/* MINA_MEMSTAT (any build): the live buffers' guest memory by kind */
void metal_buffers_summary(void)
{
    pthread_mutex_lock(&bufs_lock);
    uint64_t shared = 0, kept = 0, given = 0, saved = 0;
    int nshared = 0, nbig = 0, ngiven = 0, nprivate = 0;
    for (int i = 0; i < nlive_bufs; i++) {
        const MBuffer *b = live_bufs[i];
        if (b->gpu_only) {
            nprivate++;
            continue;
        }
        if (b->contents) {
            shared += b->length;
            nshared++;
            if (b->length >= (512u << 10))
                nbig++;
            if (b->keep_contents)
                kept += b->length;
        } else if (b->given_back) {
            ngiven++;
            saved += b->saved_len;
        }
    }
    rt_log("[mtl] %d buffers: %d with guest memory %.1f MiB (%d of 512 KiB+, %.1f MiB kept), %d given back (%.1f MiB "
           "saved), %d private\n", nlive_bufs, nshared, shared / 1048576.0, nbig, kept / 1048576.0, ngiven,
           saved / 1048576.0, nprivate);
    pthread_mutex_unlock(&bufs_lock);
}

#if !defined(__vita__)
#include <sys/mman.h>
void metal_buffers_report(void)
{
    pthread_mutex_lock(&bufs_lock);
    uint64_t total = 0, touched = 0, big = 0, big_touched = 0;
    for (int i = 0; i < nlive_bufs; i++) {
        MBuffer *b = live_bufs[i];
        if (b->gpu_only)
            continue;
        uint64_t lo = (uint64_t)(uintptr_t)G(b->contents) & ~16383ull;
        uint64_t hi = ((uint64_t)(uintptr_t)G(b->contents) + b->length + 16383) & ~16383ull;
        unsigned char vec[4096];
        uint64_t res = 0;
        int first = -1, last = -1, holes = 0;
        if (hi - lo <= 4096ull * 16384 && mincore((void *)(uintptr_t)lo, (size_t)(hi - lo), (char *)vec) == 0)
            for (uint64_t k = 0; k < (hi - lo) / 16384; k++) {
                if (vec[k] & 1) {
                    res += 16384;
                    if (first < 0) first = (int)k;
                    if (last >= 0 && (int)k > last + 1) holes++;
                    last = (int)k;
                }
            }
        if (res > b->length) res = b->length;
        total += b->length;
        touched += res;
        if (b->length >= (512u << 10)) {
            big += b->length;
            big_touched += res;
            rt_log("[mtl]   buffer %7.1f KiB touched %7.1f KiB pages %d..%d holes %d contents asked %d opt %#x\n",
                   b->length / 1024.0, res / 1024.0, first, last, holes, b->contents_asked, b->options);
        }
    }
    rt_log("[mtl] %d buffers %.1f MiB (touched %.1f), >=512K: %.1f MiB (touched %.1f)\n", nlive_bufs,
           total / 1048576.0, touched / 1048576.0, big / 1048576.0, big_touched / 1048576.0);
    pthread_mutex_unlock(&bufs_lock);
}
#endif

static void buf_dealloc(void *h)
{
    MBuffer *b = h;
    buf_track(b, 0);
    if (b->gl) {
        gpu_lock();
        glDeleteBuffers(1, &b->gl);
        gpu_unlock();
    }
    free(b->shadow);
    free(b->saved);
    if (!b->fresh && !b->gpu_only)
        gfree(b->contents);
}
#define TILE_STAGING_LEN 0x13ffb0u   /* ycGpuBuffer of a tile level (WorldRegion::StartRunning) */
static void d_newBufferWithLength(Ctx *c)
{
    uint64_t o = objc_new_object(&cls_Buffer);
    MBuffer *b = objc_host(o);
    b->length = (uint32_t)MARG(0);
    b->options = (uint32_t)MARG(1);
    {
        static int ntraced;
        char bt[512];
        const char *len_only = getenv("MINA_BUFTRACE_LEN");     /* only buffers of this length */
        if (b->length >= (1u << 20) && getenv("MINA_BUFTRACE") && ntraced < 3 &&
            (!len_only || (uint32_t)atoi(len_only) == b->length) &&
            (!getenv("MINA_BUFTRACE_SHARED") || (b->options & 0xF0) != 0x20) &&
            (guest_backtrace(bt, sizeof bt, 8), strstr(bt, getenv("MINA_BUFTRACE")))) {
            ntraced++;
            Obj *ob = objc_obj(o);
            ob->traced = 1;
            objc_trace_ref(ob, "new");
        }
    }
    /* MTLStorageModePrivate: GPU memory only (filled by blits), no guest copy */
    if ((b->options & 0xF0) == 0x20 && !getenv("MINA_NO_PRIVATE")) {
        b->gpu_only = 1;
        if (b->length <= (256u << 10))
            b->shadow = calloc(1, b->length ? b->length : 16);
        buf_track(b, 1);
        RET(o);
        return;
    }
    /* A tile level's staging buffer (only that size: texture staging buffers
     * are written again through the pointer the game keeps): the ones this thread
     * made before are written by now (made, filled, queued for upload, the
     * next one made), so their memory goes, their bytes kept for the upload:
     * a region's layers would take 1.25 MiB each at once. */
    b->maker = thread_ctx();
    if (b->length == TILE_STAGING_LEN) {
        static int keep = -1;
        if (keep < 0)
            keep = getenv("MINA_KEEP_STAGING") != NULL;
        pthread_mutex_lock(&bufs_lock);
        for (int i = 0; i < nlive_bufs && !keep; i++) {
            MBuffer *x = live_bufs[i];
            if (x->length == TILE_STAGING_LEN && x->maker == b->maker && x->contents && x->contents_asked == 1)
                metal_buf_give_back(x);
        }
        pthread_mutex_unlock(&bufs_lock);
    }
#if !defined(__vita__)
    uint32_t gmalloc_fresh(uint64_t size);
    if (getenv("MINA_FRESH_BUFFERS")) {
        b->contents = gmalloc_fresh(b->length ? b->length : 16);
        b->fresh = 1;
    } else
#endif
    b->contents = gcalloc(1, b->length ? b->length : 16);
    buf_track(b, 1);
    RET(o);
}
static void d_newBufferWithBytes(Ctx *c)
{
    uint64_t o = objc_new_object(&cls_Buffer);
    MBuffer *b = objc_host(o);
    b->length = (uint32_t)MARG(1);
    b->options = (uint32_t)MARG(2);
    b->contents = gcalloc(1, b->length ? b->length : 16);
    memcpy(G(b->contents), G(MARG(0)), b->length);
    buf_track(b, 1);
    RET(o);
}

int metal_texlog(void)
{   /* MINA_TEXLOG: textures made and deleted, copies between them (debug) */
    static int on = -1;
    if (on < 0)
        on = getenv("MINA_TEXLOG") != NULL;
    return on;
}
static void fbo_forget(GLuint gl);
static void tex_dealloc(void *h)
{
    MTexture *t = h;
    gpu_lock();
    if (metal_texlog())
        rt_log("[tex] delete %ux%u fmt %u gl %u fbo %u\n", t->width, t->height, t->format, t->gl, t->fbo_cache);
    metal_fold_forget(t);
    fbo_forget(t->gl);
    if (t->gl && t->is_rb) glDeleteRenderbuffers(1, &t->gl);
    else if (t->gl) glDeleteTextures(1, &t->gl);
    if (t->fbo_cache) glDeleteFramebuffers(1, &t->fbo_cache);
    gpu_unlock();
    gfree(t->staging);
}

static GLenum tex_target(const MTexture *t)
{
    if (t->type == 4) return GL_TEXTURE_2D_MULTISAMPLE;
    if (t->type == 5) return GL_TEXTURE_CUBE_MAP;
    if (t->type == 3) return GL_TEXTURE_2D_ARRAY;
    return GL_TEXTURE_2D;
}

static void tex_create_gl(MTexture *t)
{
    GL_DIRTY();
    GLFmt f;
    if (!gl_format(t->format, &f))
        rt_fatal("metal: unsupported pixel format %u", t->format);
    GLenum target = tex_target(t);
#if defined(__vita__)
    if (f.depth) {
        /* depth/stencil attachments are renderbuffers (they are never sampled) */
        glGenRenderbuffers(1, &t->gl);
        glBindRenderbuffer(GL_RENDERBUFFER, t->gl);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)t->width, (GLsizei)t->height);
        t->is_rb = 1;
        if (t->usage & 1) {      /* MTLTextureUsageShaderRead: shaders read the depth */
            void vglTexImageRenderbufferDepth(GLuint rb, GLsizei width, GLsizei height);   /* deps/vitaGL patch */
            glGenTextures(1, &t->depth_tex);
            glBindTexture(GL_TEXTURE_2D, t->depth_tex);
            vglTexImageRenderbufferDepth(t->gl, (GLsizei)t->width, (GLsizei)t->height);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        return;
    }
#endif
    glGenTextures(1, &t->gl);
    glBindTexture(target, t->gl);
    if (target == GL_TEXTURE_2D_MULTISAMPLE) {
        glTexImage2DMultisample(target, (GLsizei)t->samples, f.internal, (GLsizei)t->width, (GLsizei)t->height, GL_TRUE);
        return;
    }
    if (target == GL_TEXTURE_2D_ARRAY)
        rt_fatal("metal: 2D array textures are not supported yet");
#if defined(__vita__)
    /* the GPU takes at most 4096 per side: bigger textures that are only
     * sampled (atlases) are kept halved; the game still sees its size */
    if (!(t->usage & 4) && !f.block_bytes)
        while ((t->width >> t->shift) > 4096 || (t->height >> t->shift) > 4096)
            t->shift++;
    if (t->shift)
        rt_log("[tex] %ux%u texture (format %u) kept at 1/%u\n", t->width, t->height, t->format, 1u << t->shift);
#endif
    for (uint32_t level = 0; level < t->mips; level++) {
        uint32_t lw = t->width >> t->shift, lh = t->height >> t->shift;
        GLsizei w = (GLsizei)(lw >> level ? lw >> level : 1);
        GLsizei h = (GLsizei)(lh >> level ? lh >> level : 1);
        int faces = target == GL_TEXTURE_CUBE_MAP ? 6 : 1;
        for (int face = 0; face < faces; face++) {
            GLenum tt = target == GL_TEXTURE_CUBE_MAP ? (GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face) : target;
            if (f.block_bytes) {
                GLsizei size = ((w + 3) / 4) * ((h + 3) / 4) * f.block_bytes;
                glCompressedTexImage2D(tt, (GLint)level, f.internal, w, h, 0, size, NULL);
            } else {
                glTexImage2D(tt, (GLint)level, (GLint)f.internal, w, h, 0, f.format, f.type, NULL);
            }
        }
    }
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, (GLint)(t->mips - 1));
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

uint64_t metal_new_texture(uint32_t type, uint32_t fmt, uint32_t w, uint32_t h, uint32_t mips, uint32_t samples,
                           uint32_t usage, uint32_t storage)
{
    uint64_t o = objc_new_object(&cls_Texture);
    MTexture *t = objc_host(o);
    t->type = type;
    t->format = fmt;
    t->width = w ? w : 1;
    t->height = h ? h : 1;
    t->depth = 1;
    t->mips = mips ? mips : 1;
    t->samples = samples ? samples : 1;
    t->usage = usage;
    t->storage = storage;
    if (t->samples > 1 && t->type == 2)
        t->type = 4;
    gpu_lock();
    tex_create_gl(t);
    if (metal_texlog() && t->width >= 64)
        rt_log("[tex] new %ux%u fmt %u gl %u\n", t->width, t->height, t->format, t->gl);
    gpu_unlock();
    return o;
}

static void d_newTextureWithDescriptor(Ctx *c)
{
    uint64_t d = MARG(0);
    RET(metal_new_texture((uint32_t)pb_get(d, "textureType", 2), (uint32_t)pb_get(d, "pixelFormat", 70),
                          (uint32_t)pb_get(d, "width", 1), (uint32_t)pb_get(d, "height", 1),
                          (uint32_t)pb_get(d, "mipmapLevelCount", 1), (uint32_t)pb_get(d, "sampleCount", 1),
                          (uint32_t)pb_get(d, "usage", 1), (uint32_t)pb_get(d, "storageMode", 0)));
}

static GLint gl_wrap(uint32_t m)
{
    switch (m) {
    case 2: return GL_REPEAT;
    case 3: return GL_MIRRORED_REPEAT;
    case 4: case 5: return GL_CLAMP_TO_BORDER;
    default: return GL_CLAMP_TO_EDGE;
    }
}
static GLenum gl_compare(uint32_t f)
{
    static const GLenum t[] = { GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };
    return f < 8 ? t[f] : GL_ALWAYS;
}

static void sampler_dealloc(void *h)
{
    MSampler *s = h;
    gpu_lock();
    if (s->gl) glDeleteSamplers(1, &s->gl);
    gpu_unlock();
}
/* Metal hands out one sampler (and depth/stencil) state object per distinct
 * descriptor, and keeps it: the game relies on that (it keeps using a sampler
 * after releasing what it asked for, once another asked for the same one).
 * The states made here are kept the same way: one per descriptor, never freed. */
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
#define MAX_STATES 256
static struct { MSampler key; uint64_t obj; } samplers_made[MAX_STATES];
static struct { MDepthStencil key; uint64_t obj; } ds_made[MAX_STATES];
static int nsamplers_made, nds_made;

static void d_newSamplerState(Ctx *c)
{
    uint64_t d = MARG(0);
    MSampler k;
    memset(&k, 0, sizeof k);
    k.minf = (uint32_t)pb_get(d, "minFilter", 0);
    k.magf = (uint32_t)pb_get(d, "magFilter", 0);
    k.mipf = (uint32_t)pb_get(d, "mipFilter", 0);
    k.s = (uint32_t)pb_get(d, "sAddressMode", 0);
    k.t = (uint32_t)pb_get(d, "tAddressMode", 0);
    k.r = (uint32_t)pb_get(d, "rAddressMode", 0);
    k.aniso = (uint32_t)pb_get(d, "maxAnisotropy", 1);
    k.compare = (uint32_t)pb_get(d, "compareFunction", 0);
    k.lod_min = (float)pb_getd(d, "lodMinClamp", 0, 0.0);
    k.lod_max = (float)pb_getd(d, "lodMaxClamp", 0, 1000.0);
    pthread_mutex_lock(&state_lock);
    for (int i = 0; i < nsamplers_made; i++)
        if (!memcmp(&samplers_made[i].key, &k, sizeof k)) {
            uint64_t o = samplers_made[i].obj;
            pthread_mutex_unlock(&state_lock);
            RET(objc_retain_obj(o));
            return;
        }
    pthread_mutex_unlock(&state_lock);
    uint64_t o = objc_new_object(&cls_Sampler);
    MSampler *s = objc_host(o);
    s->minf = (uint32_t)pb_get(d, "minFilter", 0);
    s->magf = (uint32_t)pb_get(d, "magFilter", 0);
    s->mipf = (uint32_t)pb_get(d, "mipFilter", 0);
    s->s = (uint32_t)pb_get(d, "sAddressMode", 0);
    s->t = (uint32_t)pb_get(d, "tAddressMode", 0);
    s->r = (uint32_t)pb_get(d, "rAddressMode", 0);
    s->aniso = (uint32_t)pb_get(d, "maxAnisotropy", 1);
    s->compare = (uint32_t)pb_get(d, "compareFunction", 0);
    s->lod_min = (float)pb_getd(d, "lodMinClamp", 0, 0.0);
    s->lod_max = (float)pb_getd(d, "lodMaxClamp", 0, 1000.0);
    gpu_lock();
    glGenSamplers(1, &s->gl);
    GLint minf;
    if (s->mipf == 0) minf = s->minf ? GL_LINEAR : GL_NEAREST;
    else if (s->mipf == 1) minf = s->minf ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    else minf = s->minf ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    glSamplerParameteri(s->gl, GL_TEXTURE_MIN_FILTER, minf);
    glSamplerParameteri(s->gl, GL_TEXTURE_MAG_FILTER, s->magf ? GL_LINEAR : GL_NEAREST);
    glSamplerParameteri(s->gl, GL_TEXTURE_WRAP_S, gl_wrap(s->s));
    glSamplerParameteri(s->gl, GL_TEXTURE_WRAP_T, gl_wrap(s->t));
    glSamplerParameteri(s->gl, GL_TEXTURE_WRAP_R, gl_wrap(s->r));
    if (s->lod_max > 0)
        glSamplerParameterf(s->gl, GL_TEXTURE_MAX_LOD, s->lod_max);
    glSamplerParameterf(s->gl, GL_TEXTURE_MIN_LOD, s->lod_min);
    if (s->compare) {
        glSamplerParameteri(s->gl, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glSamplerParameteri(s->gl, GL_TEXTURE_COMPARE_FUNC, (GLint)gl_compare(s->compare));
    }
    gpu_unlock();
    pthread_mutex_lock(&state_lock);
    if (nsamplers_made < MAX_STATES) {
        samplers_made[nsamplers_made].key = k;
        samplers_made[nsamplers_made++].obj = objc_retain_obj(o);     /* kept */
    }
    pthread_mutex_unlock(&state_lock);
    RET(o);
}

static void read_stencil(uint64_t d, MStencil *s)
{
    s->compare = (uint32_t)pb_get(d, "stencilCompareFunction", 7);
    s->fail = (uint32_t)pb_get(d, "stencilFailureOperation", 0);
    s->depth_fail = (uint32_t)pb_get(d, "depthFailureOperation", 0);
    s->pass = (uint32_t)pb_get(d, "depthStencilPassOperation", 0);
    s->read_mask = (uint32_t)pb_get(d, "readMask", 0xFFFFFFFFu);
    s->write_mask = (uint32_t)pb_get(d, "writeMask", 0xFFFFFFFFu);
}
static void d_newDepthStencilState(Ctx *c)
{
    uint64_t d = MARG(0);
    if (seq_left > 0)
        rt_log("[seq] newDepthStencilState with %#llx (front %#llx back %#llx)\n", (unsigned long long)d,
               (unsigned long long)pb_existing_child(d, "frontFaceStencil"), (unsigned long long)pb_existing_child(d, "backFaceStencil"));
    MDepthStencil k;
    memset(&k, 0, sizeof k);
    k.compare = (uint32_t)pb_get(d, "depthCompareFunction", 7);
    k.write = (uint32_t)pb_get(d, "depthWriteEnabled", 0) & 1;
    uint64_t f = pb_existing_child(d, "frontFaceStencil"), b = pb_existing_child(d, "backFaceStencil");
    static int dslog = -1;
    if (dslog < 0)
        dslog = getenv("MINA_DSLOG") != NULL;
    if (dslog) {                         /* each distinct descriptor content once: keys, values */
        char line[1024];
        int n = 0;
        PropBag *bg = bag(d);
        for (int i = 0; i < bg->n && n < 900; i++) {
            uint64_t v = bg->p[i].v;
            Obj *o = v ? objc_obj(v) : NULL;
            if (o && strstr(o->cls->name, "Stencil")) {
                PropBag *sb = bag(v);
                n += snprintf(line + n, sizeof line - n, " %s{", bg->p[i].key);
                for (int j = 0; j < sb->n && n < 900; j++)
                    n += snprintf(line + n, sizeof line - n, "%s=%llx ", sb->p[j].key, (unsigned long long)sb->p[j].v);
                n += snprintf(line + n, sizeof line - n, "}");
            } else {
                n += snprintf(line + n, sizeof line - n, " %s=%llx", bg->p[i].key, (unsigned long long)v);
            }
        }
        static char seen[64][1024];
        static int nseen;
        int dup = 0;
        for (int i = 0; i < nseen && !dup; i++)
            dup = strcmp(seen[i], line) == 0;
        if (!dup && nseen < 64) {
            strcpy(seen[nseen++], line);
            rt_log("[ds]%s\n", line);
        }
    }
    k.stencil_enabled = f || b;
    read_stencil(f, &k.front);
    read_stencil(b ? b : f, &k.back);
    if (dslog) {                         /* each distinct state built */
        static MDepthStencil seen[64];
        static int nseen;
        int dup = 0;
        for (int i = 0; i < nseen && !dup; i++)
            dup = !memcmp(&seen[i], &k, sizeof k);
        if (!dup && nseen < 64) {
            seen[nseen++] = k;
            rt_log("[dsk] cmp %u w %u st %d front %u/%u/%u/%u rm %x wm %x back %u/%u/%u/%u (f %#llx b %#llx)\n", k.compare,
                   k.write, k.stencil_enabled, k.front.compare, k.front.pass, k.front.fail, k.front.depth_fail,
                   k.front.read_mask, k.front.write_mask, k.back.compare, k.back.pass, k.back.fail, k.back.depth_fail,
                   (unsigned long long)f, (unsigned long long)b);
        }
    }
    pthread_mutex_lock(&state_lock);
    for (int i = 0; i < nds_made; i++)
        if (!memcmp(&ds_made[i].key, &k, sizeof k)) {
            uint64_t o = ds_made[i].obj;
            pthread_mutex_unlock(&state_lock);
            RET(objc_retain_obj(o));
            return;
        }
    uint64_t o = objc_new_object(&cls_DepthStencil);
    *(MDepthStencil *)objc_host(o) = k;
    if (nds_made < MAX_STATES) {
        ds_made[nds_made].key = k;
        ds_made[nds_made++].obj = objc_retain_obj(o);     /* kept */
    }
    pthread_mutex_unlock(&state_lock);
    RET(o);
}

/* ---- libraries, functions, pipelines -------------------------------------------------------- */

typedef struct MLib {
    int vs, fs;              /* shader indices found in this library (-1: none) */
} MLib;
typedef struct MFunc {
    int shader;
    int stage;
} MFunc;

static uint64_t fnv64(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static void d_newLibraryWithData(Ctx *c)
{
    /* dispatch_data_t from dispatch_data_create: +16 bytes, +24 size */
    uint64_t data = MARG(0);
    uint32_t bytes = (uint32_t)rt_ld64(G(data + 16));
    uint32_t size = (uint32_t)rt_ld64(G(data + 24));
    uint64_t h = fnv64(G(bytes), size);
    uint64_t o = objc_new_object(&cls_Library);
    MLib *l = objc_host(o);
    l->vs = l->fs = -1;
    for (int i = 0; i < g_shader_count; i++)
        if (g_shaders[i].hash == h) {
            if (g_shaders[i].stage == 0) l->vs = i;
            else l->fs = i;
        }
    if (l->vs < 0 && l->fs < 0)
        rt_log("metal: library %016llx (%u bytes) has no translated shader\n", (unsigned long long)h, size);
    RET(o);
}
static void lib_newFunctionWithName(Ctx *c)
{
    MLib *l = objc_host(SELF);
    const char *name = ns_cstr(MARG(0));
    int vertex = name && strstr(name, "vertex") != NULL;
    int idx = vertex ? l->vs : l->fs;
    if (idx < 0) {
        static int told;
        if (told++ < 8)
            rt_log("metal: function %s of a library without a translated shader: nil\n", name ? name : "?");
        RET(0);
        return;
    }
    uint64_t o = objc_new_object(&cls_Function);
    MFunc *f = objc_host(o);
    f->shader = idx;
    f->stage = vertex ? 0 : 1;
    RET(o);
}

static GLuint compile(GLenum type, const char *src, int index)
{
#if defined(__vita__)
    if (getenv("MINA_SHADER_MARKS")) {
        /* the emulator logs every open: the marks place a compiler crash */
        char mark[96];
        snprintf(mark, sizeof mark, "ux0:data/mina/mark_%s_%d", type == GL_VERTEX_SHADER ? "vs" : "fs", index);
        FILE *m = fopen(mark, "rb");
        if (m)
            fclose(m);
    }
#endif
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        rt_fatal("shader %d does not compile:\n%s\n%s", index, log, src);
    }
    return s;
}

static void pipe_dealloc(void *h)
{
    MPipeline *p = h;
    gpu_lock();
    if (p->prog && !p->shared_prog) glDeleteProgram(p->prog);
    if (p->vao) glDeleteVertexArrays(1, &p->vao);
    gpu_unlock();
}

/* GLSL 3.30 wants the interpolation qualifiers of the two stages to match:
 * copy the fragment shader's (flat / noperspective) onto the vertex outputs. */
static char *patch_vertex_outputs(const char *vs, const char *fs)
{
    size_t cap = strlen(vs) + 1024;
    char *out = malloc(cap);
    strcpy(out, vs);
    const char *quals[] = { "flat ", "noperspective " };
    for (int q = 0; q < 2; q++) {
        const char *p = fs;
        while ((p = strstr(p, quals[q])) != NULL) {
            const char *in = p + strlen(quals[q]);
            if (strncmp(in, "in ", 3) != 0) { p = in; continue; }
            const char *nm = strstr(in, "v_");
            const char *end = nm ? strchr(nm, ';') : NULL;
            if (!nm || !end) break;
            char name[96];
            snprintf(name, sizeof name, "%.*s;", (int)(end - nm), nm);
            /* find "out <type> name;" in the vertex shader */
            char *o = out;
            while ((o = strstr(o, "out ")) != NULL) {
                char *semi = strchr(o, ';');
                char *v = strstr(o, " v_");
                if (semi && v && v < semi && strncmp(v + 1, name, strlen(name)) == 0 &&
                    (o == out || o[-1] == '\n')) {
                    size_t ql = strlen(quals[q]);
                    memmove(o + ql, o, strlen(o) + 1);
                    memcpy(o, quals[q], ql);
                    break;
                }
                o += 4;
            }
            p = end;
        }
    }
    return out;
}

#if defined(__vita__)
/* The shader compiler (vitaShaRK, from glLinkProgram/glShaderSource) needs far
 * more stack than the game's threads have (512 KiB): run past their end, it
 * wrote over the heap (objects freed early, pointers like 0xd00000000). It
 * runs here, on one thread with a stack of its own, while the caller waits
 * (holding the GPU lock as before). */
#define BIG_STACK (4u << 20)
/* a Vita thread with Vita semaphores: a pthread for it never ran on the device
 * (the game stopped at its first shader) */
static SceUID big_work = -1, big_done = -1;
static void (*big_fn)(void *);
static void *big_arg;
static int big_worker(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    /* the stack below here painted: MINA_STACK_LOG logs the most the compiler used */
    uint8_t here;
    uint8_t *const top = &here - 256, *const lo = top - (BIG_STACK - 64 * 1024);
    const int measure = getenv("MINA_STACK_LOG") != NULL;
    if (measure)
        memset(lo, 0xA5, (size_t)(top - lo));
    size_t most = 0;
    for (;;) {
        sceKernelWaitSema(big_work, 1, NULL);
        big_fn(big_arg);
        if (measure) {
            const uint8_t *q = lo;
            while (q < top && *q == 0xA5)
                q++;
            const size_t used = (size_t)(top - q);
            if (used > most) {
                most = used;
                rt_log("[shader] the compiler used %u KiB of its stack (%u KiB never touched)\n", (unsigned)(used >> 10),
                       (unsigned)((size_t)(q - lo) >> 10));
            }
        }
        sceKernelSignalSema(big_done, 1);
    }
    return 0;
}
static void run_on_big_stack(void (*fn)(void *), void *arg)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (big_work < 0) {
        big_work = sceKernelCreateSema("mina_shc_work", 0, 0, 1, NULL);
        big_done = sceKernelCreateSema("mina_shc_done", 0, 0, 1, NULL);
        SceUID t = sceKernelCreateThread("mina_shader_compiler", big_worker, 0x10000100, BIG_STACK, 0,
                                         SCE_KERNEL_CPU_MASK_USER_ALL, NULL);
        if (big_work < 0 || big_done < 0 || t < 0 || sceKernelStartThread(t, 0, NULL) < 0)
            rt_fatal("metal: no thread for the shader compiler (%#x %#x %#x)", big_work, big_done, t);
        rt_log("[shader] compiler thread with a %u KiB stack\n", BIG_STACK >> 10);
    }
    big_fn = fn;
    big_arg = arg;
    sceKernelSignalSema(big_work, 1);
    sceKernelWaitSema(big_done, 1, NULL);
    pthread_mutex_unlock(&lock);
}
#endif

static void link_program_here(MPipeline *p);
static void link_program_cb(void *p) { link_program_here(p); }
static void link_program(MPipeline *p)
{
#if defined(__vita__)
    run_on_big_stack(link_program_cb, p);
#else
    link_program_here(p);
#endif
}
static void link_program_here(MPipeline *p)
{
    GL_DIRTY();
#if defined(__vita__)
    const char *fsrc = p->fs >= 0 ? g_shaders[p->fs].glsl : "void main(out float4 o0 : COLOR) { o0 = float4(0, 0, 0, 0); }\n";
    char *vsrc = strdup(g_shaders[p->vs].glsl);
#else
    const char *fsrc = p->fs >= 0 ? g_shaders[p->fs].glsl : "#version 330 core\nvoid main() {}\n";
    if (getenv("MINA_DEBUG_FS"))
        fsrc = "#version 330 core\nlayout(location = 0) out vec4 o0;\nvoid main() { o0 = vec4(1.0, 0.0, 1.0, 1.0); }\n";
    char *vsrc = patch_vertex_outputs(g_shaders[p->vs].glsl, fsrc);
#endif
    GLuint vs = compile(GL_VERTEX_SHADER, vsrc, p->vs);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fsrc, p->fs);
    free(vsrc);
    p->prog = glCreateProgram();
    glAttachShader(p->prog, vs);
    glAttachShader(p->prog, fs);
#if defined(__vita__)
    /* CG attributes are bound by name (a<N> is attribute N) */
    for (int i = 0; i < MAX_ATTRS; i++) {
        char an[8];
        snprintf(an, sizeof an, "a%d", i);
        glBindAttribLocation(p->prog, (GLuint)i, an);
    }
#endif
    glLinkProgram(p->prog);
    GLint ok;
    glGetProgramiv(p->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p->prog, sizeof log, NULL, log);
        rt_fatal("program (vs %d, fs %d) does not link:\n%s", p->vs, p->fs, log);
    }
#if !defined(__vita__)
    /* vitaGL frees a deleted shader at once, even when a program uses it */
    glDeleteShader(vs);
    glDeleteShader(fs);
#endif
    for (int i = 0; i < MAX_VBUF; i++)
        p->vcb[i] = p->fcb[i] = -1;
    p->ntex = 0;
    /* uniforms, read from the sources (GLSL or CG): "uniform <type> vcb<N>[K];",
     * "uniform <sampler> ftex<T>_s<S>;" (_r: texelFetch only, _ms: multisampled) */
    glUseProgram(p->prog);
    const char *srcs[2] = { g_shaders[p->vs].glsl, p->fs >= 0 ? g_shaders[p->fs].glsl : "" };
    for (int st = 0; st < 2; st++) {
        for (const char *q = srcs[st]; (q = strstr(q, "uniform ")) != NULL; q += 8) {
            char type[32], name[64];
            int count = 1;
            if (sscanf(q, "uniform %31s %63[A-Za-z0-9_][%d]", type, name, &count) < 2)
                continue;
            GLint loc = glGetUniformLocation(p->prog, name);
            if (loc < 0)
                continue;          /* optimized away */
            int n;
            if (sscanf(name, "vcb%d", &n) == 1 && n < MAX_VBUF) {
                p->vcb[n] = loc;
                p->vcb_size[n] = count;
            } else if (sscanf(name, "fcb%d", &n) == 1 && n < MAX_VBUF) {
                p->fcb[n] = loc;
                p->fcb_size[n] = count;
            } else if (strstr(name, "_texsize") && (name[0] == 'v' || name[0] == 'f') &&
                       sscanf(name + 1, "tex%d", &n) == 1) {
                for (int k = 0; k < p->ntex; k++)
                    if (p->texb[k].stage == (name[0] == 'f') && p->texb[k].tex == n)
                        p->texb[k].size_loc = loc;
            } else if ((name[0] == 'v' || name[0] == 'f') && sscanf(name + 1, "tex%d", &n) == 1 && p->ntex < 16) {
                const char *us = strchr(name, '_');
                int sm = (us && us[1] == 's') ? atoi(us + 2) : -1;
                int k = p->ntex++;
                p->texb[k].stage = name[0] == 'f';
                p->texb[k].tex = n;
                p->texb[k].samp = sm;
                p->texb[k].unit = k;
                p->texb[k].size_loc = -1;
                glUniform1i(loc, k);
            }
        }
    }
    glGenVertexArrays(1, &p->vao);
    p->yflip_loc = glGetUniformLocation(p->prog, "mina_yflip");
    p->depth_cmp_loc = glGetUniformLocation(p->prog, "mina_depth_cmp");
    p->dst_xform_loc = glGetUniformLocation(p->prog, "mina_dst_xform");
    p->dst_depth_unit = -1;
    GLint dl = glGetUniformLocation(p->prog, "mina_dst_depth");
    if (dl >= 0 && p->depth_cmp_loc >= 0) {
        p->dst_depth_unit = p->ntex;
        glUniform1i(dl, p->ntex);
    }
}

/* ---- pipeline reflection: what the engine binds per draw ---------------------------- */

typedef struct MArg {
    uint32_t type, index, size;     /* type: 0 buffer, 2 texture, 3 sampler */
} MArg;
typedef struct MRefl {
    uint64_t vargs, fargs;          /* NSArray of MTLArgument */
    /* the same as masks (md_refl_masks): buffers and textures by stage, a buffer over 4 KiB */
    uint64_t vbuf, vtex, fbuf, ftex;
    int big;
} MRefl;
static Class cls_Refl, cls_Arg;

static void arg_isActive(Ctx *c) { RET(1); }
static void arg_type(Ctx *c) { RET(((MArg *)objc_host(SELF))->type); }
static void arg_index(Ctx *c) { RET(((MArg *)objc_host(SELF))->index); }
static void arg_size(Ctx *c) { RET(((MArg *)objc_host(SELF))->size); }
static void arg_name(Ctx *c) { RET(ns_string_auto("arg")); }
static void refl_dealloc(void *h)
{
    objc_release_obj(((MRefl *)h)->vargs);
    objc_release_obj(((MRefl *)h)->fargs);
}
static void refl_vargs(Ctx *c) { RET(((MRefl *)objc_host(SELF))->vargs); }
static void refl_fargs(Ctx *c) { RET(((MRefl *)objc_host(SELF))->fargs); }
static const Method arg_m[] = {
    { "isActive", arg_isActive }, { "type", arg_type }, { "index", arg_index },
    { "bufferDataSize", arg_size }, { "name", arg_name }, { NULL, NULL },
};
static const Method refl_m[] = {
    { "vertexArguments", refl_vargs }, { "fragmentArguments", refl_fargs }, { NULL, NULL },
};
static Class cls_Arg = { "MTLArgument_GL", "NSObject", arg_m, NULL, sizeof(MArg), NULL };
static Class cls_Refl = { "MTLRenderPipelineReflection_GL", "NSObject", refl_m, NULL, sizeof(MRefl), refl_dealloc };

static void add_arg(uint64_t arr, uint32_t type, uint32_t index, uint32_t size)
{
    uint64_t a = objc_new_object(&cls_Arg);
    MArg *m = objc_host(a);
    m->type = type;
    m->index = index;
    m->size = size;
    ns_array_add(arr, a);
    objc_release_obj(a);
}

static uint64_t make_reflection(const MPipeline *p)
{
    uint64_t r = objc_new_object(&cls_Refl);
    MRefl *m = objc_host(r);
    m->vargs = ns_array_new();
    m->fargs = ns_array_new();
    for (int i = 0; i < MAX_VBUF; i++) {
        if (p->vcb[i] >= 0) {
            add_arg(m->vargs, 0, (uint32_t)i, (uint32_t)p->vcb_size[i] * 16);
            m->vbuf |= 1ull << (i & 63);
            m->big |= p->vcb_size[i] * 16 > 0x1000;
        }
        if (p->fcb[i] >= 0) {
            add_arg(m->fargs, 0, (uint32_t)i, (uint32_t)p->fcb_size[i] * 16);
            m->fbuf |= 1ull << (i & 63);
            m->big |= p->fcb_size[i] * 16 > 0x1000;
        }
    }
    for (int k = 0; k < p->ntex; k++) {
        uint64_t arr = p->texb[k].stage ? m->fargs : m->vargs;
        add_arg(arr, 2, (uint32_t)p->texb[k].tex, 0);
        if (p->texb[k].stage)
            m->ftex |= 1ull << (p->texb[k].tex & 63);
        else
            m->vtex |= 1ull << (p->texb[k].tex & 63);
        if (p->texb[k].samp >= 0)
            add_arg(arr, 3, (uint32_t)p->texb[k].samp, 0);
    }
    return objc_autorelease_obj(r);
}

/* what the game reads from a reflection: the buffers (type 0) and textures
 * (type 2) each stage uses, and whether a buffer is over 4 KiB */
int md_refl_masks(uint64_t refl, uint64_t *vbuf, uint64_t *vtex, uint64_t *fbuf, uint64_t *ftex)
{
    Obj *o = objc_obj(refl);
    if (!o || o->cls != &cls_Refl)
        return -1;
    const MRefl *m = o->host;
    *vbuf = m->vbuf;
    *vtex = m->vtex;
    *fbuf = m->fbuf;
    *ftex = m->ftex;
    return m->big;
}
uint64_t md_new(const char *cls_name)
{   /* [MTL...Descriptor new] for the descriptor classes */
    Class *k = objc_class_named(cls_name);
    return k ? objc_new_object(k) : 0;
}

uint32_t g_pipelines_made;          /* MINA_FPS */
static void d_newRenderPipelineState(Ctx *c)
{
    g_pipelines_made++;
    uint64_t reflection_out = strstr(gstr(c->x[1]), "reflection:") ? MARG(2) : 0;
    uint64_t d = MARG(0);
    uint64_t o = objc_new_object(&cls_Pipeline);
    MPipeline *p = objc_host(o);
    uint64_t vf = pb_get(d, "vertexFunction", 0), ff = pb_get(d, "fragmentFunction", 0);
    p->vs = vf ? ((MFunc *)objc_host(vf))->shader : -1;
    p->fs = ff ? ((MFunc *)objc_host(ff))->shader : -1;
    if (p->vs < 0) {
        /* Metal fails it (nil, an error): its draws are not drawn */
        static int told;
        if (told++ < 8)
            rt_log("metal: a pipeline without a vertex function (fs %d): nil\n", p->fs);
        objc_release_obj(o);
        if (reflection_out)
            rt_st64(G(reflection_out), 0);
        RET(0);
        return;
    }
    uint64_t vd = pb_existing_child(d, "vertexDescriptor");
    uint64_t attrs = pb_existing_child(vd, "attributes"), layouts = pb_existing_child(vd, "layouts");
    for (int i = 0; i < MAX_ATTRS; i++) {
        uint64_t a = pb_elem(attrs, i);
        if (!a || !pb_get(a, "format", 0))
            continue;
        p->attrs[i].format = (uint32_t)pb_get(a, "format", 0);
        p->attrs[i].offset = (uint32_t)pb_get(a, "offset", 0);
        p->attrs[i].buffer = (uint32_t)pb_get(a, "bufferIndex", 0);
        p->attr_mask |= 1u << i;
    }
    for (int i = 0; i < MAX_VBUF; i++) {
        uint64_t l = pb_elem(layouts, i);
        p->layouts[i].stride = (uint32_t)pb_get(l, "stride", 0);
        p->layouts[i].step_fn = (uint32_t)pb_get(l, "stepFunction", 1);
        p->layouts[i].step_rate = (uint32_t)pb_get(l, "stepRate", 1);
    }
    uint64_t colors = pb_existing_child(d, "colorAttachments");
    for (int i = 0; i < MAX_COLOR; i++) {
        uint64_t a = pb_elem(colors, i);
        MBlend *b = &p->color[i];
        b->format = (uint32_t)pb_get(a, "pixelFormat", 0);
        b->enabled = (uint32_t)pb_get(a, "blendingEnabled", 0) & 1;
        b->write_mask = (uint32_t)pb_get(a, "writeMask", 0xF);
        b->rgb_op = (uint32_t)pb_get(a, "rgbBlendOperation", 0);
        b->alpha_op = (uint32_t)pb_get(a, "alphaBlendOperation", 0);
        b->src_rgb = (uint32_t)pb_get(a, "sourceRGBBlendFactor", 1);
        b->dst_rgb = (uint32_t)pb_get(a, "destinationRGBBlendFactor", 0);
        b->src_a = (uint32_t)pb_get(a, "sourceAlphaBlendFactor", 1);
        b->dst_a = (uint32_t)pb_get(a, "destinationAlphaBlendFactor", 0);
    }
    p->depth_format = (uint32_t)pb_get(d, "depthAttachmentPixelFormat", 0);
    p->stencil_format = (uint32_t)pb_get(d, "stencilAttachmentPixelFormat", 0);
    p->samples = (uint32_t)pb_get(d, "rasterSampleCount", pb_get(d, "sampleCount", 1));
    gpu_lock();
    {   /* A pipeline's GL program depends on its two shaders only (the state is set
         * per draw): one is linked per pair and kept. The game keeps 31 pipelines
         * per shader by hash % 31, and two of its variants on one slot made it
         * make them again every frame: a shader compile each time on the Vita. */
        static struct { int vs, fs; MPipeline t; } progs[1024];
        static int nprogs;
        int i = 0;
        while (i < nprogs && !(progs[i].vs == p->vs && progs[i].fs == p->fs))
            i++;
        if (i < nprogs) {
            const MPipeline *t = &progs[i].t;
            p->prog = t->prog;
            memcpy(p->vcb, t->vcb, sizeof p->vcb);
            memcpy(p->fcb, t->fcb, sizeof p->fcb);
            memcpy(p->vcb_size, t->vcb_size, sizeof p->vcb_size);
            memcpy(p->fcb_size, t->fcb_size, sizeof p->fcb_size);
            memcpy(p->texb, t->texb, sizeof p->texb);
            p->ntex = t->ntex;
            glGenVertexArrays(1, &p->vao);
            p->yflip_loc = t->yflip_loc;
            p->depth_cmp_loc = t->depth_cmp_loc;
            p->dst_xform_loc = t->dst_xform_loc;
            p->dst_depth_unit = t->dst_depth_unit;
            p->shared_prog = 1;
        } else {
            link_program(p);
            if (nprogs < (int)(sizeof progs / sizeof progs[0])) {
                progs[nprogs].vs = p->vs;
                progs[nprogs].fs = p->fs;
                progs[nprogs].t = *p;
                nprogs++;
                p->shared_prog = 1;
            }
        }
    }
    gpu_unlock();
    if (reflection_out)
        rt_st64(G(reflection_out), make_reflection(p));
    RET(o);
}

/* ---- buffers, textures, others: properties ------------------------------------------------- */

/* a staging buffer's memory, made again with the bytes it had when given back */
uint32_t metal_buf_mem(MBuffer *b)
{
    if (!b->contents && !b->gpu_only) {
        b->contents = gcalloc(1, b->length ? b->length : 16);
        if (!b->contents)
            rt_fatal("metal: no guest memory for a %u-byte buffer", b->length);
        if (b->saved)
            memcpy(G(b->contents), b->saved, b->saved_len);
        free(b->saved);
        b->saved = NULL;
        b->saved_len = 0;
        b->given_back = 0;
    }
    return b->contents;
}

void metal_buf_give_back(MBuffer *b)
{
    if (!b->contents || b->gpu_only || b->fresh || b->keep_contents)
        return;
    const uint8_t *m = G(b->contents);
    uint32_t len = b->length;
    while (len >= 8 && rt_ld64((const void *)(m + len - 8)) == 0)
        len -= 8;
    while (len && m[len - 1] == 0)
        len--;
    if (getenv("MINA_STAGINGLOG")) {
        uint32_t first = 0;
        while (first < len && m[first] == 0)
            first++;
        rt_log("[staging] %u bytes: data %u..%u\n", b->length, first, len);
    }
    uint8_t *keepb = len ? malloc(len) : NULL;
    if (len && !keepb)
        return;
    if (len)
        memcpy(keepb, m, len);
    free(b->saved);
    b->saved = keepb;
    b->saved_len = len;
    b->given_back = 1;
    gfree(b->contents);
    b->contents = 0;
}

static void b_contents(Ctx *c)
{
    MBuffer *b = objc_host(SELF);
    b->contents_asked++;
    if (!b->contents && !b->gpu_only) {
        /* the game writes through this pointer whenever: never given back again */
        static int told;
        if (told++ < 8) {
            char bt[512];
            guest_backtrace(bt, sizeof bt, 6);
            rt_log("metal: a %u-byte buffer's memory is asked for after its upload (%u bytes kept): it stays: %s\n",
                   b->length, b->saved_len, bt);
        }
        b->keep_contents = 1;
    }
    RET(metal_buf_mem(b));
}
static void b_length(Ctx *c) { RET(((MBuffer *)objc_host(SELF))->length); }
static void b_didModifyRange(Ctx *c) { (void)c; }

static void t_width(Ctx *c) { RET(((MTexture *)objc_host(SELF))->width); }
static void t_height(Ctx *c) { RET(((MTexture *)objc_host(SELF))->height); }
static void t_depth(Ctx *c) { RET(1); }
static void t_pixelFormat(Ctx *c) { RET(((MTexture *)objc_host(SELF))->format); }
static void t_textureType(Ctx *c) { RET(((MTexture *)objc_host(SELF))->type); }
static void t_mipmapLevelCount(Ctx *c) { RET(((MTexture *)objc_host(SELF))->mips); }
static void t_sampleCount(Ctx *c) { RET(((MTexture *)objc_host(SELF))->samples); }
static void t_usage(Ctx *c) { RET(((MTexture *)objc_host(SELF))->usage); }

static GLuint tex_fbo(MTexture *t, int level);

/* replaceRegion:mipmapLevel:withBytes:bytesPerRow: (region passed by pointer) */
static void tex_upload(MTexture *t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t level,
                       uint32_t slice, const uint8_t *src, uint32_t bpr)
{
    GL_DIRTY();
    metal_fold_write(t);
    t->clear_valid = 0;
    GLFmt f;
    gl_format(t->format, &f);
    GLenum target = tex_target(t);
    GLenum tt = target == GL_TEXTURE_CUBE_MAP ? (GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + slice) : target;
    glBindTexture(target, t->gl);
    if (f.block_bytes) {
        GLsizei size = (GLsizei)(((w + 3) / 4) * ((h + 3) / 4) * (uint32_t)f.block_bytes);
        glCompressedTexSubImage2D(tt, (GLint)level, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, f.internal, size, src);
        return;
    }
#if defined(__vita__)
    uint8_t *packed = NULL;
    if (t->shift && f.bpp) {
        /* a box filter down to the GL texture (regions are taken whole cells);
         * one-channel textures hold palette indices: those are sampled, never mixed */
        uint32_t s = t->shift, n = 1u << s, bpp = (uint32_t)f.bpp;
        uint32_t dx = x >> s, dy = y >> s, dw = (w + n - 1) >> s, dh = (h + n - 1) >> s;
        packed = malloc((size_t)dw * dh * bpp);
        for (uint32_t r = 0; r < dh; r++)
            for (uint32_t col = 0; col < dw; col++)
                for (uint32_t ch = 0; ch < bpp; ch++) {
                    if (bpp == 1) {
                        packed[(size_t)r * dw + col] = src[(size_t)(r * n) * bpr + (size_t)(col * n)];
                        continue;
                    }
                    uint32_t sum = 0, cnt = 0;
                    for (uint32_t yy = r * n; yy < r * n + n && yy < h; yy++)
                        for (uint32_t xx = col * n; xx < col * n + n && xx < w; xx++, cnt++)
                            sum += src[(size_t)yy * bpr + (size_t)xx * bpp + ch];
                    packed[((size_t)r * dw + col) * bpp + ch] = (uint8_t)(cnt ? sum / cnt : 0);
                }
        glTexSubImage2D(tt, (GLint)level, (GLint)dx, (GLint)dy, (GLsizei)dw, (GLsizei)dh, f.format, f.type, packed);
        free(packed);
        return;
    }
    /* no GL_UNPACK_ROW_LENGTH: rows with padding are packed first */
    if (f.bpp && bpr != w * (uint32_t)f.bpp) {
        packed = malloc((size_t)w * h * (uint32_t)f.bpp);
        for (uint32_t row = 0; row < h; row++)
            memcpy(packed + (size_t)row * w * (uint32_t)f.bpp, src + (size_t)row * bpr, (size_t)w * (uint32_t)f.bpp);
        src = packed;
    }
    glTexSubImage2D(tt, (GLint)level, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, f.format, f.type, src);
    {
        GLenum err = glGetError();
        static int told;
        if (err && told++ < 16)
            rt_log("metal: upload to a %ux%u texture (format %u, %u mips, gl %u) at %u,%u %ux%u level %u: GL error 0x%x\n",
                   t->width, t->height, t->format, t->mips, t->gl, x, y, w, h, level, err);
    }
    free(packed);
#else
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, f.bpp ? (GLint)(bpr / (uint32_t)f.bpp) : 0);
    glTexSubImage2D(tt, (GLint)level, (GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, f.format, f.type, src);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
}
static void t_replaceRegion(Ctx *c)
{
    MTexture *t = objc_host(SELF);
    uint64_t r = MARG(0);   /* MTLRegion: origin{x,y,z}, size{w,h,d} */
    gpu_lock();
    tex_upload(t, (uint32_t)rt_ld64(G(r)), (uint32_t)rt_ld64(G(r + 8)), (uint32_t)rt_ld64(G(r + 24)),
               (uint32_t)rt_ld64(G(r + 32)), (uint32_t)MARG(1), 0, G(MARG(2)), (uint32_t)MARG(3));
    gpu_unlock();
}
/* getBytes:bytesPerRow:fromRegion:mipmapLevel: */
static void t_getBytes(Ctx *c)
{
    MTexture *t = objc_host(SELF);
    uint64_t r = MARG(2);
    uint32_t x = (uint32_t)rt_ld64(G(r)), y = (uint32_t)rt_ld64(G(r + 8));
    uint32_t w = (uint32_t)rt_ld64(G(r + 24)), h = (uint32_t)rt_ld64(G(r + 32));
    GLFmt f;
    gl_format(t->format, &f);
    gpu_lock();
    metal_fold_read(t);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, tex_fbo(t, (int)MARG(3)));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, f.bpp ? (GLint)(MARG(1) / (uint64_t)f.bpp) : 0);
    glReadPixels((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, f.format, f.type, G(MARG(0)));
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    gpu_unlock();
}

static void obj_label(Ctx *c) { RET(0); }
static void obj_nop(Ctx *c) { (void)c; }
static void obj_device(Ctx *c) { RET(device); }

/* ---- render targets ----------------------------------------------------------------------- */

static GLuint tex_fbo(MTexture *t, int level)
{
    if (t->fbo_cache && level == 0)
        return t->fbo_cache;
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    GLFmt f;
    gl_format(t->format, &f);
    GLenum att = f.depth ? (f.stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT) : GL_COLOR_ATTACHMENT0;
    glFramebufferTexture2D(GL_FRAMEBUFFER, att, tex_target(t), t->gl, level);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (level == 0)
        t->fbo_cache = fbo;
    return fbo;
}

/* FBO cache keyed by attachments */
typedef struct FboEnt {
    GLuint tex[MAX_COLOR + 1];
    int level[MAX_COLOR + 1];
    GLuint fbo;
} FboEnt;
static FboEnt fbos[256];
static int nfbos;

/* A texture goes: the passes' framebuffers with it go too (GL gives a new
 * texture its name again, and a framebuffer kept for that name would draw
 * into the deleted one: black after the game's pixel scale option made its
 * targets again). gpu_lock held. */
static void fbo_forget(GLuint gl)
{
    if (!gl)
        return;
    for (int i = 0; i < nfbos;) {
        int uses = 0;
        for (int k = 0; k <= MAX_COLOR && !uses; k++)
            uses = fbos[i].tex[k] == gl;
        if (uses) {
            glDeleteFramebuffers(1, &fbos[i].fbo);
            fbos[i] = fbos[--nfbos];
        } else {
            i++;
        }
    }
}

static GLuint pass_fbo(MTexture **color, const int *levels, MTexture *depth, MTexture *stencil)
{
    FboEnt key;
    memset(&key, 0, sizeof key);
    for (int i = 0; i < MAX_COLOR; i++) {
        key.tex[i] = color[i] ? color[i]->gl : 0;
        key.level[i] = color[i] ? levels[i] : 0;
    }
    key.tex[MAX_COLOR] = depth ? depth->gl : (stencil ? stencil->gl : 0);
    for (int i = 0; i < nfbos; i++)
        if (memcmp(fbos[i].tex, key.tex, sizeof key.tex) == 0 && memcmp(fbos[i].level, key.level, sizeof key.level) == 0)
            return fbos[i].fbo;
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    GLenum bufs[MAX_COLOR];
    int nb = 0;
    for (int i = 0; i < MAX_COLOR; i++) {
        if (color[i]) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (GLenum)i, tex_target(color[i]),
                                   color[i]->gl, levels[i]);
            bufs[i] = GL_COLOR_ATTACHMENT0 + (GLenum)i;
            nb = i + 1;
        } else {
            bufs[i] = GL_NONE;
        }
    }
    glDrawBuffers(nb ? nb : 1, nb ? bufs : (GLenum[]){ GL_NONE });
    MTexture *ds = depth ? depth : stencil;
    if (ds) {
        GLFmt f;
        gl_format(ds->format, &f);
        if (ds->is_rb)
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, ds->gl);
        else
            glFramebufferTexture2D(GL_FRAMEBUFFER, f.stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                                   tex_target(ds), ds->gl, 0);
    }
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE)
        rt_log("metal: incomplete framebuffer (%#x)\n", st);
    if (nfbos < (int)(sizeof fbos / sizeof fbos[0])) {
        key.fbo = fbo;
        fbos[nfbos++] = key;
    }
    return fbo;
}

/* ---- command queue / buffer ------------------------------------------------------------------ */

typedef struct MCmdBuf {
    uint64_t handlers[8];
    int nhandlers;
    uint64_t present;        /* drawable */
    int committed;
} MCmdBuf;

static void q_commandBuffer(Ctx *c) { RET(objc_autorelease_obj(objc_new_object(&cls_CmdBuf))); }

void block_release(Ctx *c, uint64_t block);

static void cb_addCompletedHandler(Ctx *c)
{
    MCmdBuf *cb = objc_host(SELF);
    if (cb->nhandlers < 8) {
        cb->handlers[cb->nhandlers++] = block_copy(c, MARG(0));
    }
}
static void cb_presentDrawable(Ctx *c)
{
    MCmdBuf *cb = objc_host(SELF);
    cb->present = objc_retain_obj(MARG(0));
}

#if !defined(MINA_NULL_GL)
/* MINA_DUMP=path: every 60th presented frame is written there (PPM, a
 * quarter of the size; "%d" in the path numbers the files) */
static void dump_frame(MTexture *t)
{
    static int frame, every = -1, full;
    const char *dump = getenv("MINA_DUMP");
    if (every < 0) {
        const char *v = getenv("MINA_DUMP_EVERY");     /* frames between dumps (60) */
        every = v && atoi(v) > 0 ? atoi(v) : 60;
        full = getenv("MINA_DUMP_FULL") != NULL;       /* the whole frame, not a quarter */
    }
    if (dump && ++frame % every == 0) {
        uint32_t W = t->width, H = t->height;
        uint8_t *px = malloc((size_t)W * H * 4);
#if defined(__vita__)
        /* vitaGL reads the bound GL_FRAMEBUFFER, and the GPU must be done */
        glFinish();
        glBindFramebuffer(GL_FRAMEBUFFER, tex_fbo(t, 0));
        glReadPixels(0, 0, (GLsizei)W, (GLsizei)H, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
#else
        glBindFramebuffer(GL_READ_FRAMEBUFFER, tex_fbo(t, 0));
        glReadPixels(0, 0, (GLsizei)W, (GLsizei)H, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
#endif
        char path[512];
        snprintf(path, sizeof path, dump, frame / every);
        FILE *f = fopen(path, "wb");
        if (f) {
            uint32_t step = full ? 1 : 4;
            fprintf(f, "P6 %u %u 255\n", W / step, H / step);
            for (uint32_t y = 0; y + step <= H; y += step)
                for (uint32_t x = 0; x + step <= W; x += step)
                    fwrite(px + 4 * (y * W + x), 1, 3, f);
            fclose(f);
        }
        free(px);
    }
}
#endif

void metal_present(uint64_t drawable);

static void cb_commit(Ctx *c)
{
    uint64_t self = SELF;
    MCmdBuf *cb = objc_host(self);
    cb->committed = 1;
    if (cb->present) {
        metal_present(cb->present);
        objc_release_obj(cb->present);
        cb->present = 0;
    }
    for (int i = 0; i < cb->nhandlers; i++) {
        block_call(c, cb->handlers[i], 1, self);
        block_release(c, cb->handlers[i]);
    }
    cb->nhandlers = 0;
}
static void cb_status(Ctx *c) { RET(((MCmdBuf *)objc_host(SELF))->committed ? 4 : 0); }

uint64_t metal_render_encoder(Ctx *c, uint64_t passdesc);
uint64_t metal_blit_encoder(Ctx *c);
/* not new/alloc/copy: the encoders come autoreleased, as in Cocoa */
static void cb_renderEncoder(Ctx *c) { RET(objc_autorelease_obj(metal_render_encoder(c, MARG(0)))); }
static void cb_blitEncoder(Ctx *c) { RET(objc_autorelease_obj(metal_blit_encoder(c))); }

/* ---- CAMetalLayer / drawable ---------------------------------------------------------------------- */

typedef struct MLayer {
    uint32_t format;
    uint64_t textures[3];
    int next;
    uint32_t w, h;
} MLayer;
typedef struct MDrawable {
    uint64_t texture;
} MDrawable;

static uint64_t the_layer;

uint64_t metal_layer_for_window(SDL_Window *w)
{
    (void)w;
    if (!the_layer) {
        the_layer = objc_new_object(&cls_Layer);
        MLayer *l = objc_host(the_layer);
        l->format = 80;
    }
    return the_layer;
}

static void layer_size(uint32_t *w, uint32_t *h)
{
#if defined(__vita__)
    *w = 960;
    *h = 544;
    return;
#endif
    int iw, ih;
    SDL_GL_GetDrawableSize(sdl_main_window(), &iw, &ih);
    *w = (uint32_t)iw;
    *h = (uint32_t)ih;
}

static void l_setPixelFormat(Ctx *c) { ((MLayer *)objc_host(SELF))->format = (uint32_t)MARG(0); }
static void l_pixelFormat(Ctx *c) { RET(((MLayer *)objc_host(SELF))->format); }
static void l_contentsScale(Ctx *c)
{
#if defined(__vita__)
    RETD(1.0);
    return;
#endif
    int lw, lh, pw, ph;
    SDL_GetWindowSize(sdl_main_window(), &lw, &lh);
    SDL_GL_GetDrawableSize(sdl_main_window(), &pw, &ph);
    RETD(lw ? (double)pw / lw : 1.0);
}
static void l_drawableSize(Ctx *c)
{
    uint32_t w, h;
    layer_size(&w, &h);
    c->v[0] = vset_f64(w);
    c->v[1] = vset_f64(h);
}
static void l_nextDrawable(Ctx *c)
{
    MLayer *l = objc_host(SELF);
    uint32_t w, h;
    layer_size(&w, &h);
    if (w != l->w || h != l->h) {
        for (int i = 0; i < 3; i++)
            if (l->textures[i])
                objc_release_obj(l->textures[i]);
        for (int i = 0; i < 3; i++) {
            l->textures[i] = metal_new_texture(2, l->format, w, h, 1, 1, 4, 2);
            ((MTexture *)objc_host(l->textures[i]))->is_drawable = 1;
        }
        l->w = w;
        l->h = h;
    }
    uint64_t d = objc_new_object(&cls_Drawable);
    ((MDrawable *)objc_host(d))->texture = objc_retain_obj(l->textures[l->next]);
    l->next = (l->next + 1) % 3;
    RET(objc_autorelease_obj(d));
}
static void dr_dealloc(void *h) { objc_release_obj(((MDrawable *)h)->texture); }
static void dr_texture(Ctx *c) { RET(((MDrawable *)objc_host(SELF))->texture); }
static void dr_present(Ctx *c) { metal_present(SELF); }

#if defined(__vita__) && !defined(MINA_NULL_GL)
/* A copy of a texture region (GL pixel coordinates of the Metal-sized
 * texture) to a region of a framebuffer (0: the screen), drawn with a quad:
 * vitaGL's glBlitFramebuffer draws only one triangle of its quad. flip_y
 * turns the copy upside down (Metal frames to the screen). */
static void copy_program_make(void *out)
{
    /* vitaGL declares gl_Position (POSITION) and gl_FragColor (COLOR) itself */
    static const char *vs =
        "void main(float2 position, float2 texcoord, out float2 vtex : TEXCOORD0)"
        " { vtex = texcoord; gl_Position = float4(position, 0.0, 1.0); }";
    static const char *fs =
        "uniform sampler2D tex;"
        " void main(float2 vtex : TEXCOORD0) { gl_FragColor = tex2D(tex, vtex); }";
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v, 1, &vs, NULL);
    glCompileShader(v);
    glShaderSource(f, 1, &fs, NULL);
    glCompileShader(f);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glBindAttribLocation(prog, 0, "position");
    glBindAttribLocation(prog, 1, "texcoord");
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok)
        rt_fatal("metal: the copy shader does not link");
    *(GLuint *)out = prog;
}

int g_copy_rgb;          /* vita_copy_quad leaves the destination's alpha alone */
void vita_copy_quad(MTexture *src, float sx, float sy, float sw, float sh, GLuint dst_fbo, GLint dx, GLint dy,
                    GLsizei dw, GLsizei dh, int flip_y)
{
    GL_DIRTY();
    static GLuint prog;
    static GLint tex_loc;
    if (!prog) {
        run_on_big_stack(copy_program_make, &prog);
        tex_loc = glGetUniformLocation(prog, "tex");
    }
    float u0 = sx / (float)src->width, u1 = (sx + sw) / (float)src->width;
    float v0 = sy / (float)src->height, v1 = (sy + sh) / (float)src->height;
    float vb = flip_y ? v1 : v0, vt = flip_y ? v0 : v1;
    const float pos[12] = { -1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1 };
    const float uv[12] = { u0, vb, u1, vb, u0, vt, u0, vt, u1, vb, u1, vt };
    glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
    glViewport(dx, dy, dw, dh);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, g_copy_rgb ? GL_FALSE : GL_TRUE);
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src->gl);
    glBindSampler(0, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUniform1i(tex_loc, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}
#endif

void metal_present(uint64_t drawable)
{
    GL_DIRTY();
    objc_frame_end();         /* GPU resources released 3 frames ago are freed */
    {
        extern int g_framelog;
        static int frames, every = -1;
        if (every < 0) {
            const char *v = getenv("MINA_FRAMELOG");
            every = v ? atoi(v) : 0;
        }
        frames++;
        g_framelog = every > 0 && frames % every == 0 ? frames : 0;
        extern int g_pass_index;
        g_pass_index = 0;
    }
    MTexture *t = objc_host(((MDrawable *)objc_host(drawable))->texture);
    gpu_lock();
#if defined(__vita__)
#if !defined(MINA_NULL_GL)
    /* vitaGL's glBlitFramebuffer draws one triangle of its quad here: the
     * frame goes to the screen with a quad of our own (Metal row 0 at the top) */
    {
        extern MTexture *g_show_tex;
        int direct_display(void);
        /* the drawable's content: drawn on the screen already, or a texture
         * a blit only aliased (see be_copyTexture), or the drawable itself */
        MTexture *src = g_show_tex ? g_show_tex : t->alias ? t->alias : t;
        if (g_show_tex || !t->on_display || !direct_display())
            vita_copy_quad(src, 0, 0, (float)src->width, (float)src->height, 0, 0, 0, 960, 544, 1);
        t->on_display = 0;
        t->alias = NULL;
        static int keep_show = -1;
        if (keep_show < 0)
            keep_show = getenv("MINA_SHOW_PASS") != NULL;
        if (!keep_show)
            g_show_tex = NULL;      /* MINA_SHOW_FS: only while that shader runs */
    }
    dump_frame(t);
    {
        /* MINA_FPS: every 2 s, frames per second, the time between frames and
         * how much of it is spent here waiting for the GPU (swap) */
        static int on = -1, frames;
        static uint64_t last, window, swap_us, frame_max;
        if (on < 0)
            on = getenv("MINA_FPS") != NULL;
        uint64_t t0 = sceKernelGetProcessTimeWide();
        {
            extern pthread_t g_present_thread;
            extern int g_present_thread_set;
            g_present_thread = pthread_self();
            g_present_thread_set = 1;
        }
        vglSwapBuffers(GL_FALSE);
        if (on) {
            uint64_t t1 = sceKernelGetProcessTimeWide();
            swap_us += t1 - t0;
            if (last && t1 - last > frame_max)
                frame_max = t1 - last;
            last = t1;
            if (!window)
                window = t1;
            frames++;
            if (t1 - window >= 2000000) {
                extern uint32_t g_fixed_updates, g_updates;
                extern uint64_t g_fixed_us, g_update_us;
                rt_log("[fps] %.1f fps, %.1f ms/frame (worst %.1f), %.1f ms of it waiting in swap; "
                       "%.2f updates (%.1f ms), %.2f fixed updates (%.1f ms) per frame\n",
                       frames * 1e6 / (double)(t1 - window), (t1 - window) / 1000.0 / frames, frame_max / 1000.0,
                       swap_us / 1000.0 / frames, g_updates / (double)frames, g_update_us / 1000.0 / frames,
                       g_fixed_updates / (double)frames, g_fixed_us / 1000.0 / frames);
                extern uint32_t g_composites, g_bounds, g_depth_fs_draws, g_depth_restarts;
                rt_log("[fps]   per frame: %u Composite, %u UpdateBounds; %.1f depth-testing shader draws, %.1f scene restarts\n",
                       g_composites / frames, g_bounds / frames, g_depth_fs_draws / (double)frames,
                       g_depth_restarts / (double)frames);
                g_composites = g_bounds = 0;
                g_depth_fs_draws = g_depth_restarts = 0;
                {
                    extern uint32_t g_recalcs, g_recalc_same;
                    if (g_recalcs)
                        rt_log("[fps]   %u world transform recalculations a frame, %u unchanged\n", g_recalcs / frames,
                               g_recalc_same / frames);
                    g_recalcs = g_recalc_same = 0;
                }
                extern uint64_t g_draw_ns, g_endenc_ns, g_present_sem_ns;
                extern uint32_t g_draw_calls;
                rt_log("[fps]   render: %u draws %.1f ms, pass ends %.1f ms; presenting thread blocked %.1f ms; "
                       "%.1f pipelines made\n",
                       g_draw_calls / frames, g_draw_ns / 1e6 / frames, g_endenc_ns / 1e6 / frames,
                       g_present_sem_ns / 1e6 / frames, g_pipelines_made / (double)frames);
                g_pipelines_made = 0;
                extern uint64_t g_dp_ns[4];
                if (g_dp_ns[0] + g_dp_ns[1] + g_dp_ns[2] + g_dp_ns[3]) {
                    rt_log("[fps]   indexed draws: setup %.1f ms, vertices %.1f ms, indices %.1f ms, GL draw %.1f ms\n",
                           g_dp_ns[0] / 1e6 / frames, g_dp_ns[1] / 1e6 / frames, g_dp_ns[2] / 1e6 / frames,
                           g_dp_ns[3] / 1e6 / frames);
                    memset(g_dp_ns, 0, sizeof g_dp_ns);
                }
                g_draw_ns = g_endenc_ns = g_present_sem_ns = 0;
                for (int k = 0; k < g_timed_count; k++) {
                    if (g_timed_calls[k])
                        rt_log("[fps]   %6.2f ms %6.1f calls  %s\n", g_timed_ns[k] / 1e6 / frames,
                               g_timed_calls[k] / (double)frames, g_timed_names[k]);
                    g_timed_ns[k] = 0;
                    g_timed_calls[k] = 0;
                }
                rt_icall_report(frames);
                void rt_log_flush(void);
                rt_log_flush();
                g_draw_calls = 0;
                g_updates = g_fixed_updates = 0;
                g_fixed_us = g_update_us = 0;
                if (getenv("MINA_OBJSTATS")) {
                    void objc_stats(void);
                    objc_stats();
                }
                frames = 0;
                window = t1;
                swap_us = frame_max = 0;
            }
        }
    }
#endif
    gpu_unlock();
    return;
#endif
    int w, h;
    SDL_GL_GetDrawableSize(sdl_main_window(), &w, &h);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, tex_fbo(t, 0));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    /* Metal layout (row 0 at the top) to the window (row 0 at the bottom);
     * the same size: GL_LINEAR draws nothing in a large window (3064x1990)
     * on macOS, without an error */
    glBlitFramebuffer(0, 0, (GLint)t->width, (GLint)t->height, 0, h, w, 0, GL_COLOR_BUFFER_BIT,
                      t->width == (uint32_t)w && t->height == (uint32_t)h ? GL_NEAREST : GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    dump_frame(t);
    SDL_GL_SwapWindow(sdl_main_window());
    gpu_unlock();
}

/* ---- class tables ---------------------------------------------------------------------------- */

static const Method device_m[] = {
    { "name", d_name }, { "newCommandQueue", d_newCommandQueue },
    { "newCommandQueueWithMaxCommandBufferCount:", d_newCommandQueue },
    { "newBufferWithLength:options:", d_newBufferWithLength },
    { "newBufferWithBytes:length:options:", d_newBufferWithBytes },
    { "newTextureWithDescriptor:", d_newTextureWithDescriptor },
    { "newSamplerStateWithDescriptor:", d_newSamplerState },
    { "newDepthStencilStateWithDescriptor:", d_newDepthStencilState },
    { "newLibraryWithData:error:", d_newLibraryWithData },
    { "newRenderPipelineStateWithDescriptor:error:", d_newRenderPipelineState },
    { "newRenderPipelineStateWithDescriptor:options:reflection:error:", d_newRenderPipelineState },
    { "supportsTextureSampleCount:", d_supportsSampleCount }, { "supportsFamily:", d_true },
    { "maxBufferLength", d_maxBufferLength }, { "hasUnifiedMemory", d_true },
    { NULL, NULL },
};
static const Method queue_m[] = {
    { "commandBuffer", q_commandBuffer }, { "commandBufferWithUnretainedReferences", q_commandBuffer },
    { "setLabel:", obj_nop }, { "label", obj_label }, { "device", obj_device },
    { NULL, NULL },
};
static const Method cmdbuf_m[] = {
    { "addCompletedHandler:", cb_addCompletedHandler }, { "presentDrawable:", cb_presentDrawable },
    { "commit", cb_commit }, { "waitUntilCompleted", obj_nop }, { "waitUntilScheduled", obj_nop },
    { "enqueue", obj_nop }, { "status", cb_status }, { "setLabel:", obj_nop }, { "label", obj_label },
    { "renderCommandEncoderWithDescriptor:", cb_renderEncoder }, { "blitCommandEncoder", cb_blitEncoder },
    { NULL, NULL },
};
static const Method buffer_m[] = {
    { "contents", b_contents }, { "length", b_length }, { "didModifyRange:", b_didModifyRange },
    { "setLabel:", obj_nop }, { "label", obj_label }, { "device", obj_device },
    { NULL, NULL },
};
static const Method texture_m[] = {
    { "width", t_width }, { "height", t_height }, { "depth", t_depth }, { "pixelFormat", t_pixelFormat },
    { "textureType", t_textureType }, { "mipmapLevelCount", t_mipmapLevelCount },
    { "sampleCount", t_sampleCount }, { "usage", t_usage }, { "arrayLength", t_depth },
    { "replaceRegion:mipmapLevel:withBytes:bytesPerRow:", t_replaceRegion },
    { "getBytes:bytesPerRow:fromRegion:mipmapLevel:", t_getBytes },
    { "setLabel:", obj_nop }, { "label", obj_label }, { "device", obj_device },
    { NULL, NULL },
};
static const Method simple_m[] = {
    { "setLabel:", obj_nop }, { "label", obj_label }, { "device", obj_device }, { NULL, NULL },
};
static const Method library_m[] = {
    { "newFunctionWithName:", lib_newFunctionWithName }, { "setLabel:", obj_nop }, { "label", obj_label },
    { NULL, NULL },
};
static const Method layer_m[] = {
    { "setPixelFormat:", l_setPixelFormat }, { "pixelFormat", l_pixelFormat },
    { "contentsScale", l_contentsScale }, { "drawableSize", l_drawableSize }, { "nextDrawable", l_nextDrawable },
    { "setDevice:", obj_nop }, { "setFramebufferOnly:", obj_nop }, { "setMagnificationFilter:", obj_nop },
    { "setDisplaySyncEnabled:", obj_nop }, { "setDrawableSize:", obj_nop }, { "setMaximumDrawableCount:", obj_nop },
    { "setContentsScale:", obj_nop }, { "setOpaque:", obj_nop }, { "device", obj_device },
    { NULL, NULL },
};
static const Method drawable_m[] = { { "texture", dr_texture }, { "present", dr_present }, { NULL, NULL } };

static Class cls_Device = { "MTLDevice_GL", "NSObject", device_m, NULL, 0, NULL };
static Class cls_Queue = { "MTLCommandQueue_GL", "NSObject", queue_m, NULL, 0, NULL };
static Class cls_CmdBuf = { "MTLCommandBuffer_GL", "NSObject", cmdbuf_m, NULL, sizeof(MCmdBuf), NULL };
static Class cls_Buffer = { "MTLBuffer_GL", "NSObject", buffer_m, NULL, sizeof(MBuffer), buf_dealloc, .gpu_lifetime = 1 };
static Class cls_Texture = { "MTLTexture_GL", "NSObject", texture_m, NULL, sizeof(MTexture), tex_dealloc, .gpu_lifetime = 1 };
static Class cls_Sampler = { "MTLSamplerState_GL", "NSObject", simple_m, NULL, sizeof(MSampler), sampler_dealloc };
static Class cls_DepthStencil = { "MTLDepthStencilState_GL", "NSObject", simple_m, NULL, sizeof(MDepthStencil), NULL };
static Class cls_Library = { "MTLLibrary_GL", "NSObject", library_m, NULL, sizeof(MLib), NULL };
static Class cls_Function = { "MTLFunction_GL", "NSObject", simple_m, NULL, sizeof(MFunc), NULL };
static Class cls_Pipeline = { "MTLRenderPipelineState_GL", "NSObject", simple_m, NULL, sizeof(MPipeline), pipe_dealloc };
static Class cls_Layer = { "CAMetalLayer", "NSObject", layer_m, NULL, sizeof(MLayer), NULL };
static Class cls_Drawable = { "CAMetalDrawable_GL", "NSObject", drawable_m, NULL, sizeof(MDrawable), dr_dealloc };

extern Class cls_RenderEnc, cls_BlitEnc;

Class *metal_class_texture(void) { return &cls_Texture; }
Class *metal_class_buffer(void) { return &cls_Buffer; }

void metal_init_classes(void);
Class *const g_classes_metal[] = {
    &cls_PropBag, &cls_BagArray, &cls_TextureDesc, &cls_SamplerDesc, &cls_DepthStencilDesc, &cls_StencilDesc,
    &cls_PipelineDesc, &cls_PipelineColorDesc, &cls_VertexDesc, &cls_VertexAttrDesc, &cls_VertexLayoutDesc,
    &cls_PipelineBufferDesc, &cls_PassDesc, &cls_PassColorDesc, &cls_PassDepthDesc, &cls_PassStencilDesc,
    &cls_Device, &cls_Queue, &cls_CmdBuf, &cls_Buffer, &cls_Texture, &cls_Sampler, &cls_DepthStencil,
    &cls_Library, &cls_Function, &cls_Pipeline, &cls_Layer, &cls_Drawable, &cls_RenderEnc, &cls_BlitEnc,
    &cls_Arg, &cls_Refl,
    NULL,
};

void metal_init(void)
{
    define_children();
    cls_TextureDesc.cmethods = td_cmethods;
    cls_PassDesc.cmethods = pd_cmethods;
}

/* MTLRenderPassDescriptor helpers for the encoder */
void metal_pass_info(uint64_t pd, uint64_t *color, uint64_t *depth, uint64_t *stencil)
{
    uint64_t colors = pb_existing_child(pd, "colorAttachments");
    for (int i = 0; i < MAX_COLOR; i++)
        color[i] = pb_elem(colors, i);
    *depth = pb_existing_child(pd, "depthAttachment");
    *stencil = pb_existing_child(pd, "stencilAttachment");
}
GLuint metal_pass_fbo(MTexture **color, const int *levels, MTexture *depth, MTexture *stencil)
{
    return pass_fbo(color, levels, depth, stencil);
}
GLuint metal_tex_fbo(MTexture *t, int level) { return tex_fbo(t, level); }
void metal_tex_upload(MTexture *t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t level, uint32_t slice,
                      const uint8_t *src, uint32_t bpr)
{
    tex_upload(t, x, y, w, h, level, slice, src, bpr);
}
GLenum metal_tex_target(const MTexture *t) { return tex_target(t); }
GLenum metal_gl_compare(uint32_t f) { return gl_compare(f); }

const ImportImpl g_impl_metal[] = {
    { "_MTLCreateSystemDefaultDevice", imp_MTLCreateSystemDefaultDevice, 0, NULL },
    { NULL, NULL, 0, NULL },
};

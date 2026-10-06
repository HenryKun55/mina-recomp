/* Guest threads are host threads: pthreads, synchronization, TLS, libdispatch. */
#include "runtime.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct GThread {
    pthread_t host;
    Ctx *ctx;
    uint32_t id;             /* guest-visible pthread_t (guest address of a small block) */
    uint32_t start, arg;
    uint32_t stack_lo, stack_size;
    uint64_t result;
    int detached;
} GThread;

#if defined(__vita__)
#define tl_ctx (host_tls()->ctx)

static pthread_key_t tls_key;
static pthread_once_t tls_once = PTHREAD_ONCE_INIT;

static void tls_free(void *p)
{
    HostTls *t = p;
    free(t->pool);
    if (t->err)
        free(G(t->err));
    if (t->scratch)
        free(G(t->scratch));
    free(t);
}
static void tls_init(void) { pthread_key_create(&tls_key, tls_free); }

HostTls *host_tls(void)
{
    pthread_once(&tls_once, tls_init);
    HostTls *t = pthread_getspecific(tls_key);
    if (!t) {
        t = calloc(1, sizeof *t);
        t->lazy_pending = -1;
        pthread_setspecific(tls_key, t);
    }
    return t;
}
#else
static __thread Ctx *tl_ctx;
#endif

/* Host stack of a guest thread: the recompiled functions keep the guest
 * registers in C locals, and on the Vita every 64-bit register is two. */
#if defined(__vita__)
/* 512 KiB (59 KiB used at most, measured): the real Vita has far less free memory than Vita3K (4 MiB stacks
 * ran out after five threads there); MINA_MEMSTAT reports what is used */
#define HOST_STACK (512 * 1024)
#else
#define HOST_STACK (2 * 1024 * 1024)
#endif

Ctx *thread_ctx(void) { return tl_ctx; }

/* ---- TLS ---------------------------------------------------------------- */

uint32_t tls_block(Ctx *c)
{
    if (!c->tls_base && g_tls_size) {
        uint32_t b = gmalloc(g_tls_size);
        /* template: __thread_data contents then zeroed __thread_bss (the
         * loader already zeroed bss in the template). */
        memcpy(G(b), G(g_tls_template), g_tls_size);
        c->tls_base = b;
    }
    return c->tls_base;
}

/* Descriptor {thunk, key, offset}: x0 = descriptor, returns the variable's address. */
static void imp_tlv_bootstrap(Ctx *c)
{
    uint64_t off = rt_ld64(G(A0 + 16));
    RET(tls_block(c) + off);
}

/* ---- threads -------------------------------------------------------------- */

/* Guest stacks: the engine asks for 256 KiB to 8 MiB per thread (~14 MiB in
 * all); the Vita gets what is asked up to 8 MiB. A smaller stack overflows
 * into the heap below it, where its own Ctx lives. */
#if defined(__vita__)
#define GUEST_STACK_CAP (8u << 20)
#else
#define GUEST_STACK_CAP (64u << 20)
#endif

static Ctx *new_ctx(uint32_t stack_size)
{
    Ctx *c = calloc(1, sizeof *c);
    if (g_trace_imports)
        rt_log("[thread] guest stack of %u KiB asked\n", stack_size >> 10);
    if (stack_size > GUEST_STACK_CAP)
        stack_size = GUEST_STACK_CAP;
    uint32_t lo = gmemalign(64, stack_size);
    if (!lo)
        rt_fatal("cannot allocate a guest stack of %u bytes", stack_size);
    c->stack_lo = lo;
    c->stack_hi = lo + stack_size;
    c->sp = c->stack_hi - 64;
    return c;
}

void threads_init_main(Ctx *c)
{
    (void)c;
}

static pthread_mutex_t threads_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t next_thread_id = 1;

Ctx *threads_main_ctx(uint32_t stack_size)
{
    Ctx *c = new_ctx(stack_size);
    GThread *t = calloc(1, sizeof *t);
    t->host = pthread_self();
    t->ctx = c;
    t->id = rt_alloc(64, 16);
    rt_st32(G(t->id), next_thread_id++);
    rt_st64(G(t->id + 8), (uint64_t)(uintptr_t)t);
    c->thread = t;
    c->thread_id = rt_ld32(G(t->id));
    tl_ctx = c;
    return c;
}

static GThread *thread_of_id(uint64_t id)
{
    if (!id)
        return NULL;
    return (GThread *)(uintptr_t)rt_ld64(G(id + 8));
}

/* Host stack watermarks (Vita, MINA_MEMSTAT): each thread paints its stack
 * below the entry frame and the report finds the deepest byte touched. */
#if defined(__vita__)
#define STACK_PAINT 0xA5u
static struct { const uint8_t *lo; uint32_t size; uint32_t id; } stacks[64];
static int nstacks;

static void stack_paint(uint32_t id)
{
    if (!getenv("MINA_MEMSTAT"))
        return;
    uint8_t here;
    uint8_t *top = &here - 512;              /* below this frame and its callees */
    uint32_t size = HOST_STACK - 32 * 1024;  /* pthread's own use stays above */
    uint8_t *lo = top - size;
    for (uint8_t *q = lo; q < top; q++)
        *q = STACK_PAINT;
    pthread_mutex_lock(&threads_lock);
    if (nstacks < 64) {
        stacks[nstacks].lo = lo;
        stacks[nstacks].size = size;
        stacks[nstacks].id = id;
        nstacks++;
    }
    pthread_mutex_unlock(&threads_lock);
}

void threads_stack_report(void)
{
    pthread_mutex_lock(&threads_lock);
    for (int i = 0; i < nstacks; i++) {
        uint32_t k = 0;
        while (k < stacks[i].size && stacks[i].lo[k] == STACK_PAINT)
            k++;
        rt_log("[stack] thread %u: %u KiB of %u KiB host stack used\n", stacks[i].id, (stacks[i].size - k) >> 10,
               stacks[i].size >> 10);
    }
    pthread_mutex_unlock(&threads_lock);
}
#else
static void stack_paint(uint32_t id) { (void)id; }
void threads_stack_report(void) {}
#endif

static void *thread_main(void *p)
{
    GThread *t = p;
    if (tl_ctx)
        rt_log("[thread] new host thread already has a ctx %p (stale TLS)\n", (void *)tl_ctx);
    tl_ctx = t->ctx;
    stack_paint(t->ctx->thread_id);
    if (g_trace_imports)
        rt_log("[thread] %u (pthread) host sp %p, guest stack %#x..%#x (host %p), ctx %p\n", t->ctx->thread_id,
               (void *)&t, t->ctx->stack_lo, t->ctx->stack_hi, G(t->ctx->stack_lo), (void *)t->ctx);
    if (!t->start)
        rt_log("pthread_create with a null start routine (arg %#x)\n", t->arg);
    t->result = guest_call(t->ctx, t->start, 1, (uint64_t)t->arg);
    return NULL;
}

/* pthread_attr_t layout (ours): +0 magic, +8 stack size, +16 detached */
#define ATTR_MAGIC 0x41545452u

/* A guest thread running a host function (NSThread, dispatch). */
typedef struct HostJob {
    void (*fn)(Ctx *, void *);
    void *arg;
    GThread *t;
} HostJob;

static void *host_job_main(void *p)
{
    HostJob *j = p;
    if (tl_ctx)
        rt_log("[thread] new host thread already has a ctx %p (stale TLS)\n", (void *)tl_ctx);
    tl_ctx = j->t->ctx;
    stack_paint(j->t->ctx->thread_id);
    if (g_trace_imports)
        rt_log("[thread] %u host sp %p, guest stack %#x..%#x (host %p), ctx %p\n", j->t->ctx->thread_id,
               (void *)&j, j->t->ctx->stack_lo, j->t->ctx->stack_hi, G(j->t->ctx->stack_lo), (void *)j->t->ctx);
    j->fn(j->t->ctx, j->arg);
    free(j);
    return NULL;
}

uint32_t thread_spawn(void (*fn)(Ctx *, void *), void *arg, uint32_t stack_size)
{
    if (stack_size < 256 * 1024)
        stack_size = 256 * 1024;
    GThread *t = calloc(1, sizeof *t);
    t->ctx = new_ctx(stack_size);
    t->ctx->thread = t;
    t->id = rt_alloc(64, 16);
    pthread_mutex_lock(&threads_lock);
    rt_st32(G(t->id), next_thread_id++);
    pthread_mutex_unlock(&threads_lock);
    rt_st64(G(t->id + 8), (uint64_t)(uintptr_t)t);
    t->ctx->thread_id = rt_ld32(G(t->id));
    HostJob *j = malloc(sizeof *j);
    j->fn = fn;
    j->arg = arg;
    j->t = t;
    pthread_attr_t ha;
    pthread_attr_init(&ha);
    pthread_attr_setstacksize(&ha, HOST_STACK);
    pthread_attr_setdetachstate(&ha, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&t->host, &ha, host_job_main, j);
    pthread_attr_destroy(&ha);
    if (r)
        rt_fatal("cannot start thread %u (pthread_create: %d): out of memory for its stack?", t->ctx->thread_id, r);
    return t->id;
}

int thread_is_main(Ctx *c) { return c->thread_id == 1; }

/* Context of the calling host thread, created on first use for threads the
 * guest did not start (SDL's audio thread, callbacks). */
Ctx *host_thread_ctx(void)
{
    if (tl_ctx)
        return tl_ctx;
    GThread *t = calloc(1, sizeof *t);
    t->host = pthread_self();
    t->ctx = new_ctx(512 * 1024);
    t->ctx->thread = t;
    t->id = rt_alloc(64, 16);
    pthread_mutex_lock(&threads_lock);
    rt_st32(G(t->id), next_thread_id++);
    pthread_mutex_unlock(&threads_lock);
    rt_st64(G(t->id + 8), (uint64_t)(uintptr_t)t);
    t->ctx->thread_id = rt_ld32(G(t->id));
    tl_ctx = t->ctx;
    return tl_ctx;
}

static void imp_pthread_attr_init(Ctx *c)
{
    memset(G(A0), 0, 64);
    rt_st32(G(A0), ATTR_MAGIC);
    rt_st64(G(A0 + 8), 512 * 1024);
    RET(0);
}
static void imp_pthread_attr_setstacksize(Ctx *c) { rt_st64(G(A0 + 8), A1); RET(0); }
static void imp_pthread_attr_setdetachstate(Ctx *c) { rt_st32(G(A0 + 16), (uint32_t)(A1 == 2)); RET(0); }

static void imp_pthread_create(Ctx *c)
{
    if (g_trace_imports)
        rt_log("pthread_create(start %#llx, arg %#llx)\n", (unsigned long long)A2, (unsigned long long)A3);
    uint32_t out = (uint32_t)A0, attr = (uint32_t)A1;
    uint64_t stack = 512 * 1024;
    int detached = 0;
    if (attr && rt_ld32(G(attr)) == ATTR_MAGIC) {
        stack = rt_ld64(G(attr + 8));
        detached = (int)rt_ld32(G(attr + 16));
    }
    if (stack < 256 * 1024)
        stack = 256 * 1024;
    GThread *t = calloc(1, sizeof *t);
    t->ctx = new_ctx((uint32_t)stack);
    t->ctx->thread = t;
    t->start = (uint32_t)A2;
    t->arg = (uint32_t)A3;
    t->detached = detached;
    t->id = rt_alloc(64, 16);
    pthread_mutex_lock(&threads_lock);
    rt_st32(G(t->id), next_thread_id++);
    pthread_mutex_unlock(&threads_lock);
    rt_st64(G(t->id + 8), (uint64_t)(uintptr_t)t);
    t->ctx->thread_id = rt_ld32(G(t->id));
    pthread_attr_t ha;
    pthread_attr_init(&ha);
    pthread_attr_setstacksize(&ha, HOST_STACK);   /* host frames of the recompiled code */
    if (detached)
        pthread_attr_setdetachstate(&ha, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&t->host, &ha, thread_main, t);
    pthread_attr_destroy(&ha);
    if (r)
        rt_log("pthread_create failed (%d): out of memory for a thread stack?\n", r);
    if (out)
        rt_st64(G(out), t->id);
    RET(r);
}

static void imp_pthread_join(Ctx *c)
{
    GThread *t = thread_of_id(A0);
    if (!t)
        rt_fatal("pthread_join: unknown thread");
    int r = pthread_join(t->host, NULL);
    if (A1)
        rt_st64(G(A1), t->result);
    RET(r);
}

static void imp_pthread_detach(Ctx *c)
{
    GThread *t = thread_of_id(A0);
    RET(t ? pthread_detach(t->host) : 0);
}

static void imp_pthread_self(Ctx *c) { RET(((GThread *)c->thread)->id); }

static void imp_pthread_getschedparam(Ctx *c)
{
    if (A1) rt_st32(G(A1), SCHED_OTHER);
    if (A2) rt_st32(G(A2), 31);
    RET(0);
}
static void imp_ret0(Ctx *c) { RET(0); }
static void imp_sched_get_priority_max(Ctx *c) { RET(47); }
static void imp_sched_get_priority_min(Ctx *c) { RET(15); }

/* ---- mutexes, conditions, rwlocks ----------------------------------------------
 * The guest object keeps its signature word; bytes 8..15 hold the host object. */

#define MUTEX_SIG_INIT 0x32AAABA7u
#define MUTEX_SIG_RINIT 0x32AAABA2u   /* PTHREAD_RECURSIVE_MUTEX_INITIALIZER */
#define MUTEX_SIG_DONE 0x4D555458u
#define COND_SIG_INIT 0x3CB0B1BBu
#define COND_SIG_DONE 0x434F4E44u
#define RW_SIG_INIT 0x2DA8B3B4u
#define RW_SIG_DONE 0x52574C4Bu

static pthread_mutex_t lazy_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *host_mutex(uint32_t g, int recursive)
{
    uint64_t h = AT_LOAD(uint64_t, G(g + 8));
    if (h && rt_ld32(G(g)) == MUTEX_SIG_DONE)
        return (pthread_mutex_t *)(uintptr_t)h;
    pthread_mutex_lock(&lazy_lock);
    h = rt_ld64(G(g + 8));
    if (!h || rt_ld32(G(g)) != MUTEX_SIG_DONE) {
        uint32_t sig = rt_ld32(G(g));
        pthread_mutex_t *m = malloc(sizeof *m);
        pthread_mutexattr_t at;
        pthread_mutexattr_init(&at);
        if (recursive || sig == MUTEX_SIG_RINIT)
            pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(m, &at);
        pthread_mutexattr_destroy(&at);
        h = (uint64_t)(uintptr_t)m;
        AT_STORE(uint64_t, G(g + 8), h);
        rt_st32(G(g), MUTEX_SIG_DONE);
    }
    pthread_mutex_unlock(&lazy_lock);
    return (pthread_mutex_t *)(uintptr_t)h;
}

/* mutexattr (ours): +0 type */
static void imp_pthread_mutexattr_init(Ctx *c) { memset(G(A0), 0, 16); RET(0); }
static void imp_pthread_mutexattr_settype(Ctx *c) { rt_st32(G(A0), (uint32_t)A1); RET(0); }

static void imp_pthread_mutex_init(Ctx *c)
{
    int recursive = A1 && rt_ld32(G(A1)) == 2;   /* PTHREAD_MUTEX_RECURSIVE */
    memset(G(A0), 0, 64);
    rt_st32(G(A0), MUTEX_SIG_INIT);
    host_mutex((uint32_t)A0, recursive);
    RET(0);
}
static void imp_pthread_mutex_destroy(Ctx *c)
{
    if (rt_ld32(G(A0)) == MUTEX_SIG_DONE) {
        pthread_mutex_t *m = (pthread_mutex_t *)(uintptr_t)rt_ld64(G(A0 + 8));
        pthread_mutex_destroy(m);
        free(m);
    }
    memset(G(A0), 0, 16);
    RET(0);
}
static void imp_pthread_mutex_lock(Ctx *c) { RET(pthread_mutex_lock(host_mutex((uint32_t)A0, 0))); }
static void imp_pthread_mutex_trylock(Ctx *c) { RET(pthread_mutex_trylock(host_mutex((uint32_t)A0, 0))); }
static void imp_pthread_mutex_unlock(Ctx *c) { RET(pthread_mutex_unlock(host_mutex((uint32_t)A0, 0))); }

static pthread_cond_t *host_cond(uint32_t g)
{
    uint64_t h = AT_LOAD(uint64_t, G(g + 8));
    if (h && rt_ld32(G(g)) == COND_SIG_DONE)
        return (pthread_cond_t *)(uintptr_t)h;
    pthread_mutex_lock(&lazy_lock);
    if (rt_ld32(G(g)) != COND_SIG_DONE) {
        pthread_cond_t *cv = malloc(sizeof *cv);
        pthread_cond_init(cv, NULL);
        AT_STORE(uint64_t, G(g + 8), (uint64_t)(uintptr_t)cv);
        rt_st32(G(g), COND_SIG_DONE);
    }
    h = rt_ld64(G(g + 8));
    pthread_mutex_unlock(&lazy_lock);
    return (pthread_cond_t *)(uintptr_t)h;
}

static void imp_pthread_cond_init(Ctx *c) { memset(G(A0), 0, 48); host_cond((uint32_t)A0); RET(0); }
static void imp_pthread_cond_destroy(Ctx *c)
{
    if (rt_ld32(G(A0)) == COND_SIG_DONE) {
        pthread_cond_t *cv = (pthread_cond_t *)(uintptr_t)rt_ld64(G(A0 + 8));
        pthread_cond_destroy(cv);
        free(cv);
    }
    memset(G(A0), 0, 16);
    RET(0);
}
static void imp_pthread_cond_signal(Ctx *c) { RET(pthread_cond_signal(host_cond((uint32_t)A0))); }
static void imp_pthread_cond_broadcast(Ctx *c) { RET(pthread_cond_broadcast(host_cond((uint32_t)A0))); }
static void imp_pthread_cond_wait(Ctx *c)
{
    RET(pthread_cond_wait(host_cond((uint32_t)A0), host_mutex((uint32_t)A1, 0)));
}
static void imp_pthread_cond_timedwait(Ctx *c)
{
    struct timespec ts;
    ts.tv_sec = (time_t)rt_ld64(G(A2));
    ts.tv_nsec = (long)rt_ld64(G(A2 + 8));
    int r = pthread_cond_timedwait(host_cond((uint32_t)A0), host_mutex((uint32_t)A1, 0), &ts);
    RET(r == ETIMEDOUT ? 60 : r);   /* macOS ETIMEDOUT = 60 */
}

static pthread_rwlock_t *host_rw(uint32_t g)
{
    uint64_t h = AT_LOAD(uint64_t, G(g + 8));
    if (h && rt_ld32(G(g)) == RW_SIG_DONE)
        return (pthread_rwlock_t *)(uintptr_t)h;
    pthread_mutex_lock(&lazy_lock);
    if (rt_ld32(G(g)) != RW_SIG_DONE) {
        pthread_rwlock_t *l = malloc(sizeof *l);
        pthread_rwlock_init(l, NULL);
        AT_STORE(uint64_t, G(g + 8), (uint64_t)(uintptr_t)l);
        rt_st32(G(g), RW_SIG_DONE);
    }
    h = rt_ld64(G(g + 8));
    pthread_mutex_unlock(&lazy_lock);
    return (pthread_rwlock_t *)(uintptr_t)h;
}
static void imp_pthread_rwlock_init(Ctx *c) { memset(G(A0), 0, 16); host_rw((uint32_t)A0); RET(0); }
static void imp_pthread_rwlock_destroy(Ctx *c) { RET(0); }
static void imp_pthread_rwlock_rdlock(Ctx *c) { RET(pthread_rwlock_rdlock(host_rw((uint32_t)A0))); }
static void imp_pthread_rwlock_wrlock(Ctx *c) { RET(pthread_rwlock_wrlock(host_rw((uint32_t)A0))); }
static void imp_pthread_rwlock_unlock(Ctx *c) { RET(pthread_rwlock_unlock(host_rw((uint32_t)A0))); }

/* ---- keys -------------------------------------------------------------------- */

static void imp_pthread_key_create(Ctx *c)
{
    pthread_key_t k;
    int r = pthread_key_create(&k, NULL);   /* guest destructors are not run */
    rt_st64(G(A0), (uint64_t)(uintptr_t)k);
    RET(r);
}
static void imp_pthread_key_delete(Ctx *c) { RET(pthread_key_delete((pthread_key_t)(uintptr_t)A0)); }
static void imp_pthread_getspecific(Ctx *c) { RET((uint64_t)(uintptr_t)pthread_getspecific((pthread_key_t)(uintptr_t)A0)); }
static void imp_pthread_setspecific(Ctx *c)
{
    RET(pthread_setspecific((pthread_key_t)(uintptr_t)A0, (void *)(uintptr_t)A1));
}

/* ---- semaphores (libdispatch) ------------------------------------------------- */

typedef struct HSem {
    pthread_mutex_t m;
    pthread_cond_t cv;
    long count;
} HSem;

/* dispatch objects: guest block, +0 kind, +8 host pointer */
#define DOBJ_SEM 1u
#define DOBJ_GROUP 2u
#define DOBJ_QUEUE 3u
#define DOBJ_DATA 4u

static HSem *hsem_new(long v)
{
    HSem *s = calloc(1, sizeof *s);
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->cv, NULL);
    s->count = v;
    return s;
}

static void hsem_signal(HSem *s)
{
    pthread_mutex_lock(&s->m);
    s->count++;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->m);
}

/* timeout: DISPATCH_TIME_FOREVER (~0) or a dispatch_time value (ns, mach clock) */
static int hsem_wait(HSem *s, uint64_t timeout)
{
    pthread_mutex_lock(&s->m);
    int r = 0;
    if (timeout == ~0ull) {
        while (s->count <= 0)
            pthread_cond_wait(&s->cv, &s->m);
    } else {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
        uint64_t wait_ns = timeout > now_ns ? timeout - now_ns : 0;
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        uint64_t t = (uint64_t)dl.tv_sec * 1000000000ull + (uint64_t)dl.tv_nsec + wait_ns;
        dl.tv_sec = (time_t)(t / 1000000000ull);
        dl.tv_nsec = (long)(t % 1000000000ull);
        while (s->count <= 0 && r == 0)
            r = pthread_cond_timedwait(&s->cv, &s->m, &dl);
        if (s->count <= 0) {
            pthread_mutex_unlock(&s->m);
            return 1;
        }
    }
    s->count--;
    pthread_mutex_unlock(&s->m);
    return 0;
}

static uint32_t dobj_new(uint32_t kind, void *host)
{
    uint32_t a = gmalloc(32);
    memset(G(a), 0, 32);
    rt_st32(G(a), kind);
    rt_st64(G(a + 8), (uint64_t)(uintptr_t)host);
    return a;
}
static void *dobj_host(uint64_t a) { return (void *)(uintptr_t)rt_ld64(G(a + 8)); }

static void imp_dispatch_semaphore_create(Ctx *c) { RET(dobj_new(DOBJ_SEM, hsem_new((long)A0))); }
static void imp_dispatch_semaphore_signal(Ctx *c) { hsem_signal(dobj_host(A0)); RET(0); }
static void imp_dispatch_semaphore_wait(Ctx *c) { RET(hsem_wait(dobj_host(A0), A1)); }

static void imp_dispatch_time(Ctx *c)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t base = A0 ? A0 : (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
    RET(base + A1);
}

/* Blocks: +16 invoke function, called with the block as first argument. */
static void run_block(Ctx *c, uint32_t block)
{
    uint64_t invoke = rt_ld64(G(block + 16));
    guest_call(c, invoke, 1, (uint64_t)block);
}

typedef struct AsyncJob {
    uint32_t block, fn, ctx;
    uint32_t group;
} AsyncJob;

static void group_leave(uint32_t g);
void block_release(Ctx *c, uint64_t block);

static void *async_main(void *p)
{
    AsyncJob *j = p;
    Ctx *c = new_ctx(512 * 1024);
    GThread *t = calloc(1, sizeof *t);
    t->ctx = c;
    t->id = rt_alloc(64, 16);
    rt_st64(G(t->id + 8), (uint64_t)(uintptr_t)t);
    c->thread = t;
    if (tl_ctx)
        rt_log("[thread] new host thread already has a ctx %p (stale TLS)\n", (void *)tl_ctx);
    tl_ctx = c;
    if (j->block) {
        run_block(c, j->block);
        block_release(c, j->block);
    } else {
        if (!j->fn)
            rt_log("async job without a block or function\n");
        guest_call(c, j->fn, 1, (uint64_t)j->ctx);
    }
    if (j->group)
        group_leave(j->group);
    free(j);
    /* one host thread per job: its guest stack and context go with it */
    tl_ctx = NULL;
    gfree(c->stack_lo);
    rt_st64(G(t->id + 8), 0);
    free(c);
    free(t);
    return NULL;
}

static void spawn_job(AsyncJob *j)
{
    pthread_t th;
    pthread_attr_t ha;
    pthread_attr_init(&ha);
    pthread_attr_setstacksize(&ha, HOST_STACK);
    pthread_attr_setdetachstate(&ha, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&th, &ha, async_main, j);
    pthread_attr_destroy(&ha);
    if (r)
        rt_fatal("cannot start a dispatch thread (pthread_create: %d): out of memory for its stack?", r);
}

/* The main queue: blocks wait here until the main thread drains them (it
 * does so around its SDL event waits, see sdl.c). */
static pthread_mutex_t mainq_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t mainq[256];
static int mainq_n;
void (*g_mainq_wake)(void);

void mainq_drain(Ctx *c)
{
    for (;;) {
        pthread_mutex_lock(&mainq_lock);
        if (!mainq_n) {
            pthread_mutex_unlock(&mainq_lock);
            return;
        }
        uint32_t b = mainq[0];
        memmove(mainq, mainq + 1, (size_t)(--mainq_n) * sizeof mainq[0]);
        pthread_mutex_unlock(&mainq_lock);
        run_block(c, b);
        block_release(c, b);
    }
}

uint64_t block_copy(Ctx *c, uint64_t block);
void block_release(Ctx *c, uint64_t block);

static void imp_dispatch_async(Ctx *c)
{
    uint64_t queue = A0;
    uint32_t b = (uint32_t)block_copy(c, A1);
    if (queue == import_data("__dispatch_main_q")) {
        pthread_mutex_lock(&mainq_lock);
        if (mainq_n == 256)
            rt_fatal("main queue full");
        mainq[mainq_n++] = b;
        pthread_mutex_unlock(&mainq_lock);
        if (g_mainq_wake)
            g_mainq_wake();
        return;
    }
    AsyncJob *j = calloc(1, sizeof *j);
    j->block = b;
    spawn_job(j);
}

static void imp_dispatch_sync(Ctx *c)
{
    /* the caller waits anyway: run it here (main-queue work has no thread affinity for us) */
    run_block(c, (uint32_t)A1);
}

static void imp_dispatch_once_f(Ctx *c)
{
    static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
    uint32_t pred = (uint32_t)A0;
    if (AT_LOAD(uint64_t, G(pred)) == ~0ull)
        return;
    uint64_t ctxv = A1, fn = A2;
    pthread_mutex_lock(&once_lock);
    if (rt_ld64(G(pred)) != ~0ull) {
        guest_call(c, fn, 1, ctxv);
        AT_STORE(uint64_t, G(pred), ~0ull);
    }
    pthread_mutex_unlock(&once_lock);
}

static void imp_dispatch_get_global_queue(Ctx *c) { RET(dobj_new(DOBJ_QUEUE, NULL)); }

typedef struct HGroup {
    pthread_mutex_t m;
    pthread_cond_t cv;
    long pending;
} HGroup;

static void imp_dispatch_group_create(Ctx *c)
{
    HGroup *g = calloc(1, sizeof *g);
    pthread_mutex_init(&g->m, NULL);
    pthread_cond_init(&g->cv, NULL);
    RET(dobj_new(DOBJ_GROUP, g));
}

static void group_leave(uint32_t ga)
{
    HGroup *g = dobj_host(ga);
    pthread_mutex_lock(&g->m);
    if (--g->pending == 0)
        pthread_cond_broadcast(&g->cv);
    pthread_mutex_unlock(&g->m);
}

static void imp_dispatch_group_async(Ctx *c)
{
    HGroup *g = dobj_host(A0);
    pthread_mutex_lock(&g->m);
    g->pending++;
    pthread_mutex_unlock(&g->m);
    AsyncJob *j = calloc(1, sizeof *j);
    j->block = (uint32_t)block_copy(c, A2);
    j->group = (uint32_t)A0;
    spawn_job(j);
}

static void imp_dispatch_group_wait(Ctx *c)
{
    HGroup *g = dobj_host(A0);
    pthread_mutex_lock(&g->m);
    while (g->pending > 0)
        pthread_cond_wait(&g->cv, &g->m);
    pthread_mutex_unlock(&g->m);
    RET(0);
}

static void imp_dispatch_data_create(Ctx *c)
{
    /* buffer, size, queue, destructor: the game only passes it to Metal */
    uint32_t a = dobj_new(DOBJ_DATA, NULL);
    rt_st64(G(a + 16), A0);
    rt_st64(G(a + 24), A1);
    RET(a);
}

static void main_q_init(uint32_t a) { rt_st32(G(a), DOBJ_QUEUE); }

/* ---- misc ------------------------------------------------------------------------- */

static void imp_usleep(Ctx *c) { usleep((useconds_t)A0); RET(0); }
static void imp_nanosleep(Ctx *c)
{
    struct timespec ts = { (time_t)rt_ld64(G(A0)), (long)rt_ld64(G(A0 + 8)) };
    nanosleep(&ts, NULL);
    RET(0);
}
static void imp_sched_yield(Ctx *c) { sched_yield(); RET(0); }

const ImportImpl g_impl_pthread[] = {
    { "__tlv_bootstrap", imp_tlv_bootstrap, 0, NULL },
    { "_pthread_attr_init", imp_pthread_attr_init, 0, NULL },
    { "_pthread_attr_setstacksize", imp_pthread_attr_setstacksize, 0, NULL },
    { "_pthread_attr_setdetachstate", imp_pthread_attr_setdetachstate, 0, NULL },
    { "_pthread_create", imp_pthread_create, 0, NULL },
    { "_pthread_join", imp_pthread_join, 0, NULL },
    { "_pthread_detach", imp_pthread_detach, 0, NULL },
    { "_pthread_self", imp_pthread_self, 0, NULL },
    { "_pthread_getschedparam", imp_pthread_getschedparam, 0, NULL },
    { "_pthread_setschedparam", imp_ret0, 0, NULL },
    { "_pthread_setcanceltype", imp_ret0, 0, NULL },
    { "_pthread_sigmask", imp_ret0, 0, NULL },
    { "_sched_get_priority_max", imp_sched_get_priority_max, 0, NULL },
    { "_sched_get_priority_min", imp_sched_get_priority_min, 0, NULL },
    { "_sched_yield", imp_sched_yield, 0, NULL },
    { "_pthread_mutexattr_init", imp_pthread_mutexattr_init, 0, NULL },
    { "_pthread_mutexattr_settype", imp_pthread_mutexattr_settype, 0, NULL },
    { "_pthread_mutex_init", imp_pthread_mutex_init, 0, NULL },
    { "_pthread_mutex_destroy", imp_pthread_mutex_destroy, 0, NULL },
    { "_pthread_mutex_lock", imp_pthread_mutex_lock, 0, NULL },
    { "_pthread_mutex_trylock", imp_pthread_mutex_trylock, 0, NULL },
    { "_pthread_mutex_unlock", imp_pthread_mutex_unlock, 0, NULL },
    { "_pthread_cond_init", imp_pthread_cond_init, 0, NULL },
    { "_pthread_cond_destroy", imp_pthread_cond_destroy, 0, NULL },
    { "_pthread_cond_signal", imp_pthread_cond_signal, 0, NULL },
    { "_pthread_cond_broadcast", imp_pthread_cond_broadcast, 0, NULL },
    { "_pthread_cond_wait", imp_pthread_cond_wait, 0, NULL },
    { "_pthread_cond_timedwait", imp_pthread_cond_timedwait, 0, NULL },
    { "_pthread_rwlock_init", imp_pthread_rwlock_init, 0, NULL },
    { "_pthread_rwlock_destroy", imp_pthread_rwlock_destroy, 0, NULL },
    { "_pthread_rwlock_rdlock", imp_pthread_rwlock_rdlock, 0, NULL },
    { "_pthread_rwlock_wrlock", imp_pthread_rwlock_wrlock, 0, NULL },
    { "_pthread_rwlock_unlock", imp_pthread_rwlock_unlock, 0, NULL },
    { "_pthread_key_create", imp_pthread_key_create, 0, NULL },
    { "_pthread_key_delete", imp_pthread_key_delete, 0, NULL },
    { "_pthread_getspecific", imp_pthread_getspecific, 0, NULL },
    { "_pthread_setspecific", imp_pthread_setspecific, 0, NULL },
    { "_dispatch_semaphore_create", imp_dispatch_semaphore_create, 0, NULL },
    { "_dispatch_semaphore_signal", imp_dispatch_semaphore_signal, 0, NULL },
    { "_dispatch_semaphore_wait", imp_dispatch_semaphore_wait, 0, NULL },
    { "_dispatch_time", imp_dispatch_time, 0, NULL },
    { "_dispatch_async", imp_dispatch_async, 0, NULL },
    { "_dispatch_sync", imp_dispatch_sync, 0, NULL },
    { "_dispatch_once_f", imp_dispatch_once_f, 0, NULL },
    { "_dispatch_get_global_queue", imp_dispatch_get_global_queue, 0, NULL },
    { "_dispatch_group_create", imp_dispatch_group_create, 0, NULL },
    { "_dispatch_group_async", imp_dispatch_group_async, 0, NULL },
    { "_dispatch_group_wait", imp_dispatch_group_wait, 0, NULL },
    { "_dispatch_data_create", imp_dispatch_data_create, 0, NULL },
    { "__dispatch_main_q", NULL, 64, main_q_init },
    { "_usleep", imp_usleep, 0, NULL },
    { "_nanosleep", imp_nanosleep, 0, NULL },
    { NULL, NULL, 0, NULL },
};

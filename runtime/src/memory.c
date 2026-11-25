/* Guest address space and guest heap. */
#include "runtime.h"

#include <pthread.h>
#include <string.h>

#if defined(__vita__)
#include <malloc.h>
#else
#include <sys/mman.h>
#endif

uint8_t *g_mem;

/* Mac heap above the lazy-bank window (0x40000000-0xA0000000, see libc.c) */
#define HEAP_BASE   0xA0000000u
#define HEAP_END    0xF8000000u

#if defined(__vita__)
/* Vita: the image is one host block and g_mem = block - GUEST_IMAGE_BASE.
 * Pointers are 32-bit, so every host pointer p is the guest address
 * p - g_mem (mod 2^32): the guest heap is newlib's malloc. */
#include <malloc.h>
/* with the extended memory mode (ATTRIBUTE2=12 in param.sfo) */
int _newlib_heap_size_user = 272 * 1024 * 1024;   /* the rest of the extended memory is vitaGL's */
static uint32_t image_size;

#include <pthread.h>
#include <unistd.h>
static uint32_t image_end;
static uint32_t skip_hi = GUEST_IMAGE_BASE;      /* image memory starts here (see mem_init) */
static void *lowguard_thread(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(20000);
        const uint32_t *w = (const uint32_t *)g_mem;
        for (uint32_t i = GUEST_NULL_GUARD / 4; i < GUEST_IMAGE_BASE / 4; i++) {
            if (w[i] != 0xA5A5A5A5u) {
                rt_log("[lowguard] guest %#x written: %#x %#x %#x %#x\n", i * 4, w[i], w[i + 1], w[i + 2], w[i + 3]);
                /* report each run once */
                uint32_t j = i;
                while (j < GUEST_IMAGE_BASE / 4 && w[j] != 0xA5A5A5A5u)
                    ((uint32_t *)g_mem)[j++] = 0xA5A5A5A5u;
                rt_log("[lowguard]   %u bytes\n", (j - i) * 4);
                i = j;
            }
        }
    }
    return NULL;
}

static void slab_init(void);
char *sbrk_start;
void mem_init(void)
{
    sbrk_start = sbrk(0);
    uint32_t end = GUEST_IMAGE_BASE;
    for (uint32_t k = 0; k < g_segment_count; k++)
        if (g_segments[k].addr + g_segments[k].size > end)
            end = g_segments[k].addr + g_segments[k].size;
    image_end = end;
    /* MINA_LOWGUARD: guest addresses below the image are a block of their own
     * (else they are this executable), filled with a pattern and watched */
    if (getenv("MINA_LOWGUARD")) {
        uint8_t *all = memalign(16384, end);
        if (!all)
            rt_fatal("cannot allocate the image and the low guard (%u bytes)", end);
        g_mem = all;
        memset(all, 0xA5, GUEST_IMAGE_BASE);
        pthread_t t;
        pthread_create(&t, NULL, lowguard_thread, NULL);
        return;
    }
    /* the Mach-O header and the arm64 code (never read: the recompiled C runs
     * instead) get no memory: the block starts after them (MINA_KEEPTEXT: whole) */
    skip_hi = getenv("MINA_KEEPTEXT") ? GUEST_IMAGE_BASE : g_text_hi;
    image_size = end - skip_hi;
    uint8_t *block = memalign(16384, end - skip_hi);
    if (!block)
        rt_fatal("cannot allocate the image (%u bytes)", end - skip_hi);
    g_mem = (uint8_t *)((uintptr_t)block - skip_hi);
    slab_init();
}
uint32_t image_skip_lo(void) { return GUEST_IMAGE_BASE; }
uint32_t image_skip_hi(void) { return skip_hi; }

uint32_t rt_alloc(uint32_t size, uint32_t align)
{
    void *p = memalign(align < 8 ? 8 : align, size);
    if (!p)
        rt_fatal("runtime allocation of %u bytes failed", size);
    memset(p, 0, size);
    return guest_addr(p);
}

static uint64_t heap_in_use;

/* MINA_HEAPCHECK on the Vita: every block gets a header and 16 canary bytes
 * after it (checked when it is freed), and a freed block stays painted in a
 * quarantine for a while (checked when it leaves it): an overrun or a write
 * after free is logged with the guest's call stack. */
#define HC_MAGIC 0x4B434831u
#define HC_FREED 0x4B434830u
#define HC_CANARY 0xFDu
#define HC_PAINT 0xDDu
#define HC_Q 2048
typedef struct { uint32_t magic, size, pad[2]; } HcHdr;
static int hc = -1;
static HcHdr *hc_q[HC_Q];
static uint32_t hc_qi;
static pthread_mutex_t hc_lock = PTHREAD_MUTEX_INITIALIZER;
static int heapcheck_on(void)
{
    if (hc < 0)
        hc = getenv("MINA_HEAPCHECK") != NULL;
    return hc;
}
static HcHdr *hc_hdr(uint32_t a)
{
    HcHdr *h = (HcHdr *)G(a - 16);
    return h->magic == HC_MAGIC || h->magic == HC_FREED ? h : NULL;
}
static void hc_report(const char *what, uint32_t a, uint32_t size, uint32_t at, uint8_t b)
{
    char bt[512];
    guest_backtrace(bt, sizeof bt, 8);
    rt_log("[heapcheck] %s: block %#x of %u bytes, byte +%u is %#x [%s]\n", what, a, size, at, b, bt);
}
static void hc_leave(HcHdr *h)
{
    const uint8_t *u = (const uint8_t *)(h + 1);
    for (uint32_t i = 0; i < h->size; i++)
        if (u[i] != HC_PAINT) {
            hc_report("written after its free", guest_addr((void *)u), h->size, i, u[i]);
            break;
        }
    free(h);
}

/* MINA_FPS: heap calls over 2 ms (the console's play stalled in allocations) */
#include <psp2/kernel/processmgr.h>
static uint64_t slow_now(void) { return sceKernelGetProcessTimeWide(); }
static void slow_note(const char *what, uint64_t t0, uint64_t size)
{
    const uint64_t dt = slow_now() - t0;
    if (dt < 2000)
        return;
    static int on = -1;
    if (on < 0)
        on = getenv("MINA_FPS") != NULL;
    if (!on)
        return;
    Ctx *thread_ctx(void);
    Ctx *tc = thread_ctx();
    rt_log("[slowheap] %s of %llu bytes: %.1f ms (thread %u)\n", what, (unsigned long long)size, dt / 1000.0,
           tc ? tc->thread_id : 0);
}
/* MINA_BIGLOG: guest blocks of 256 KiB or more, made and freed */
static int biglog = -1;
static void big_note(void *p, uint64_t size, int made, const void *ra)
{
    if (biglog < 0)
        biglog = getenv("MINA_BIGLOG") != NULL;
    if (biglog && size >= (256u << 10))
        rt_log("[big] %s %llu at %p\n", made ? "made" : "freed", (unsigned long long)size, p);
    static int traced;
    if (biglog && made && size == 524560 && traced++ < 3) {
        char bt[1024];
        guest_backtrace(bt, sizeof bt, 12);
        Ctx *thread_ctx(void);
        Ctx *tc = thread_ctx();
        rt_log("[big] 524560 from host %p, thread %u: %s\n", ra, tc ? tc->thread_id : 0, bt);
    }
}
/* Small blocks (up to SLAB_MAX bytes) come from slabs: 64 KiB pages of one size each
 * (16-byte steps), a free list per size under its own lock. newlib's malloc
 * (dlmalloc 2.6) keeps its free chunks in sorted lists: on the console, with the heap
 * in ~60k pieces, its calls took 2-65 ms under its one lock (the game thread lost ~7 s
 * in a session to it, the audio and render threads as much). Small blocks no longer
 * cut its free space into pieces, and theirs never walk a list. Pages are never given
 * back (a size's pages serve only that size). MINA_SLAB=0: newlib for all. */
#include <psp2/kernel/threadmgr.h>
#define SLAB_MAX 2048u
#define SLAB_NCLS (32 + 24)          /* 16-byte steps to 512, then 64-byte steps to 2048 */
static inline int slab_cls_of(uint64_t size)
{
    if (size <= 512)
        return size ? (int)((size - 1) >> 4) : 0;
    return 32 + (int)((size - 513) >> 6);
}
static inline uint32_t slab_size_of(int k) { return k < 32 ? (uint32_t)(k + 1) << 4 : 512u + (uint32_t)(k - 31) * 64u; }
#define SLAB_PAGE 0x10000u
#define SLAB_CHUNK 0x100000u
#define SLAB_LIMIT (112u << 20)       /* past this, newlib again */
typedef struct {
    SceKernelLwMutexWork mtx;
    uint8_t *free_list, *bump, *bump_end;
    uint32_t pages, live;
} SlabCls;
static SlabCls slab[SLAB_NCLS];
static uint8_t slab_dir[65536];       /* host address >> 16: size class + 1 */
static SceKernelLwMutexWork slab_chunk_mtx;
static uint8_t *slab_chunk, *slab_chunk_end;
static uint32_t slab_bytes;
static int slab_on;
static void slab_init(void)
{
    const char *v = getenv("MINA_SLAB");
    if ((v && !atoi(v)) || getenv("MINA_HEAPCHECK"))
        return;
    for (int i = 0; i < SLAB_NCLS; i++)
        sceKernelCreateLwMutex(&slab[i].mtx, "mina_slab", 0, 0, NULL);
    sceKernelCreateLwMutex(&slab_chunk_mtx, "mina_slab_chunk", 0, 0, NULL);
    slab_on = 1;
}
static uint8_t *slab_page(int k)
{
    uint8_t *pg = NULL;
    sceKernelLockLwMutex(&slab_chunk_mtx, 1, NULL);
    if (slab_chunk == slab_chunk_end && slab_bytes + SLAB_CHUNK <= SLAB_LIMIT) {
        slab_chunk = memalign(SLAB_PAGE, SLAB_CHUNK);
        slab_chunk_end = slab_chunk ? slab_chunk + SLAB_CHUNK : NULL;
        if (slab_chunk)
            slab_bytes += SLAB_CHUNK;
    }
    if (slab_chunk && slab_chunk < slab_chunk_end) {
        pg = slab_chunk;
        slab_chunk += SLAB_PAGE;
        slab_dir[(uintptr_t)pg >> 16] = (uint8_t)(k + 1);
    }
    sceKernelUnlockLwMutex(&slab_chunk_mtx, 1);
    return pg;
}
static void *slab_alloc(uint64_t size)
{
    const int k = slab_cls_of(size);
    SlabCls *s = &slab[k];
    const uint32_t sz = slab_size_of(k);
    sceKernelLockLwMutex(&s->mtx, 1, NULL);
    uint8_t *p = s->free_list;
    if (p)
        s->free_list = *(uint8_t **)p;
    else {
        if (s->bump + sz > s->bump_end) {
            uint8_t *pg = slab_page(k);
            if (!pg) {
                sceKernelUnlockLwMutex(&s->mtx, 1);
                return NULL;
            }
            s->bump = pg;
            s->bump_end = pg + SLAB_PAGE;
            s->pages++;
        }
        p = s->bump;
        s->bump += sz;
    }
    s->live++;
    sceKernelUnlockLwMutex(&s->mtx, 1);
    return p;
}
static inline int slab_class(const void *p) { return (int)slab_dir[(uintptr_t)p >> 16] - 1; }
static void slab_free(void *p, int k)
{
    SlabCls *s = &slab[k];
    sceKernelLockLwMutex(&s->mtx, 1, NULL);
    *(uint8_t **)p = s->free_list;
    s->free_list = p;
    s->live--;
    sceKernelUnlockLwMutex(&s->mtx, 1);
}
void slab_report(char *out, size_t n)
{
    uint64_t live = 0;
    for (int i = 0; i < SLAB_NCLS; i++)
        live += (uint64_t)slab[i].live * slab_size_of(i);
    snprintf(out, n, "small blocks %.1f MiB in %.1f MiB of slabs", live / 1048576.0, slab_bytes / 1048576.0);
}

/* newlib's blocks for the guest: malloc(size + align), aligned up inside, the offset kept in
 * the 8 bytes before. memalign(16, n) asked newlib for n + 16 + its minimum and gave back the
 * pieces around: a block freed left a hole the next request of the same size never fit, so the
 * break moved up instead (Vita3K, a level standing still: 209 MiB in use, the heap at 272 with
 * 63 free in 5k pieces, then out of memory; the console's crashes). One size now asks for one
 * size and takes its hole again. */
#define NL_MAGIC 0x4E4C4231u
static void *nl_alloc(size_t size, size_t align)
{
    uint8_t *p = malloc(size + align);
    if (!p)
        return NULL;
    uint8_t *q = (uint8_t *)(((uintptr_t)p + 8 + align - 1) & ~(uintptr_t)(align - 1));
    const uint32_t off = (uint32_t)(q - p);
    ((uint32_t *)q)[-1] = off;
    ((uint32_t *)q)[-2] = NL_MAGIC ^ off;
    return q;
}
static inline uint8_t *nl_base(void *q)
{
    const uint32_t off = ((uint32_t *)q)[-1];
    if ((((uint32_t *)q)[-2] ^ off) != NL_MAGIC || off < 8 || off > (1u << 20))
        rt_fatal("guest heap: block %p has no header (freed twice or written before its start)", q);
    return (uint8_t *)q - off;
}
static inline size_t nl_usable(void *q)
{
    uint8_t *b = nl_base(q);
    return malloc_usable_size(b) - (size_t)((uint8_t *)q - b);
}
static inline void nl_free(void *q) { free(nl_base(q)); }

/* MINA_HEAPSTAT: live guest blocks by size (16-byte steps below 4 KiB, then powers of
 * two), the sizes that grew most since the last report logged with a few guest
 * call stacks of their allocations (what leaks) */
#define HS_N 280
static int64_t hs_bytes[HS_N], hs_prev[HS_N], hs_cnt[HS_N];
static int hs_on = -1, hs_watch[3] = { -1, -1, -1 }, hs_samples;
static inline int hs_bucket(uint32_t u)
{
    if (u < 4096)
        return (int)(u >> 4);
    return 256 + (31 - __builtin_clz(u)) - 12;
}
static void hs_track(void *p, const uintptr_t *st);
static void hs_untrack(void *p);
static void hs_note(void *p, uint32_t u, int made)
{
    if (hs_on < 0)
        hs_on = getenv("MINA_HEAPSTAT") != NULL;
    if (!hs_on)
        return;
    const int b = hs_bucket(u);
    __atomic_add_fetch(&hs_bytes[b], made ? (int64_t)u : -(int64_t)u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&hs_cnt[b], made ? 1 : -1, __ATOMIC_RELAXED);
    if (!made && u <= 128)
        hs_untrack(p);
    if (made && u <= 128 && (b == hs_watch[0] || b == hs_watch[1])) {
        static uint32_t m;
        if ((__atomic_add_fetch(&m, 1, __ATOMIC_RELAXED) & 63) == 0) {
            uintptr_t st[10] = { 0 };
            const uintptr_t lo = (uintptr_t)&hs_note - 0x3000000u, hi = (uintptr_t)&hs_note + 0x3000000u;
            const uint32_t *sp = (const uint32_t *)__builtin_frame_address(0);
            for (int i = 0, k = 0; i < 1024 && k < 10; i++) {
                const uintptr_t w = sp[i];
                if ((w & 1) && w >= lo && w <= hi)
                    st[k++] = w;
            }
            hs_track(p, st);
        }
    }
    if (made && (u >= (4u << 20) || b == hs_watch[0] || b == hs_watch[1] || b == hs_watch[2])) {
        static uint32_t n;
        if (u >= (4u << 20) || ((__atomic_add_fetch(&n, 1, __ATOMIC_RELAXED) % 97) == 0 && hs_samples < 6)) {
            hs_samples++;
            char bt[900];
            bt[0] = 0;
            /* the recompiled functions on the host stack (return addresses: thumb, near this code) */
            const char *guest_func_at_host(const void *pc);
            const uintptr_t lo = (uintptr_t)&hs_note - 0x3000000u, hi = (uintptr_t)&hs_note + 0x3000000u;
            const uint32_t *st = (const uint32_t *)__builtin_frame_address(0);
            int o = 0, got = 0;
            const char *last = "";
            for (int i = 0; i < 1024 && got < 9 && o < (int)sizeof bt - 80; i++) {
                const uintptr_t w = st[i];
                if (!(w & 1) || w < lo || w > hi)
                    continue;
                const char *f = guest_func_at_host((const void *)w);
                if (!f || f[0] == '?' || f == last)
                    continue;
                last = f;
                o += snprintf(bt + o, sizeof bt - (size_t)o, "%s%p", got ? " < " : "", (void *)w);
                got++;
            }
            rt_log("[heapstat] %u bytes at %p: %s\n", u, p, bt);
        }
    }
}
/* the survivors: every 64th block of the watched sizes, its stack's host addresses; freed ones
 * are struck off; the report groups those alive for over one report by their stack */
#define HS_TRACK 4096
static struct { void *p; uint32_t gen; uintptr_t st[10]; } hs_trk[HS_TRACK];
static uint32_t hs_trk_i, hs_gen;
static void hs_track(void *p, const uintptr_t *st)
{
    const uint32_t i = __atomic_fetch_add(&hs_trk_i, 1, __ATOMIC_RELAXED) % HS_TRACK;
    hs_trk[i].p = p;
    hs_trk[i].gen = hs_gen;
    memcpy(hs_trk[i].st, st, sizeof hs_trk[i].st);
}
static void hs_untrack(void *p)
{
    for (int i = 0; i < HS_TRACK; i++)
        if (hs_trk[i].p == p)
            hs_trk[i].p = NULL;
}
void heapstat_report(void)
{
    if (hs_on <= 0)
        return;
    hs_gen++;
    {
        static uint8_t seen[HS_TRACK];
        memset(seen, 0, sizeof seen);
        int lines = 0;
        for (int i = 0; i < HS_TRACK; i++) {
            if (!hs_trk[i].p || hs_gen - hs_trk[i].gen < 2 || seen[i])
                continue;
            int n = 0;
            for (int j = i; j < HS_TRACK; j++)
                if (hs_trk[j].p && hs_gen - hs_trk[j].gen >= 2 && !seen[j] &&
                    !memcmp(hs_trk[j].st, hs_trk[i].st, sizeof hs_trk[i].st)) {
                    seen[j] = 1;
                    n++;
                }
            if (n >= 2 && lines++ < 12)
                rt_log("[heapstat] %d sampled blocks still alive from %p %p %p %p %p %p %p %p %p %p\n", n,
                       (void *)hs_trk[i].st[0], (void *)hs_trk[i].st[1], (void *)hs_trk[i].st[2],
                       (void *)hs_trk[i].st[3], (void *)hs_trk[i].st[4], (void *)hs_trk[i].st[5],
                       (void *)hs_trk[i].st[6], (void *)hs_trk[i].st[7], (void *)hs_trk[i].st[8],
                       (void *)hs_trk[i].st[9]);
        }
    }
    int top[6] = { -1, -1, -1, -1, -1, -1 };
    for (int b = 0; b < HS_N; b++) {
        const int64_t d = hs_bytes[b] - hs_prev[b];
        for (int k = 0; k < 6; k++)
            if (top[k] < 0 || d > hs_bytes[top[k]] - hs_prev[top[k]]) {
                for (int j = 5; j > k; j--)
                    top[j] = top[j - 1];
                top[k] = b;
                break;
            }
    }
    char line[512];
    int o = 0;
    for (int k = 0; k < 6; k++) {
        const int b = top[k];
        const uint32_t lo = b < 256 ? (uint32_t)b << 4 : 1u << (b - 256 + 12);
        o += snprintf(line + o, sizeof line - (size_t)o, " %u: %+.0f KiB (%.1f MiB, %lld)", lo,
                      (hs_bytes[b] - hs_prev[b]) / 1024.0, hs_bytes[b] / 1048576.0, (long long)hs_cnt[b]);
    }
    struct mallinfo mi = mallinfo();
    rt_log("[heapstat] guest blocks %.1f MiB of the %.1f in use (the rest: the runtime's own); grew most:%s\n",
           heap_in_use / 1048576.0, mi.uordblks / 1048576.0, line);
    for (int k = 0; k < 3; k++)
        hs_watch[k] = top[k];
    hs_samples = 0;
    memcpy(hs_prev, hs_bytes, sizeof hs_prev);
}

uint32_t gmalloc(uint64_t size)
{
    if (size > 0x20000000ull)
        return 0;
    if (slab_on && size <= SLAB_MAX) {
        void *q = slab_alloc(size);
        if (q) {
            const uint32_t sz = slab_size_of(slab_cls_of(size));
            heap_in_use += sz;
            hs_note(q, sz, 1);
            return guest_addr(q);
        }
    }
    if (heapcheck_on()) {
        HcHdr *h = memalign(16, (size_t)size + 32);
        if (!h)
            return 0;
        h->magic = HC_MAGIC;
        h->size = (uint32_t)size;
        memset((uint8_t *)(h + 1) + size, HC_CANARY, 16);
        return guest_addr(h + 1);
    }
    const uint64_t t0 = slow_now();
    void *p = nl_alloc(size ? (size_t)size : 1, 16);
    slow_note("malloc", t0, size);
    if (!p) {
        rt_log("guest heap: %llu bytes not available\n", (unsigned long long)size);
        static int told;
        if (!told++) {
            extern char *sbrk_start;
            char *now = sbrk(0);
            void *more = sbrk(1 << 20);
            struct mallinfo mi = mallinfo();
            rt_log("guest heap: break moved %.1f MiB since boot, sbrk(1 MiB) %s; arena %.1f MiB, free %.1f in %d chunks\n",
                   (now - sbrk_start) / 1048576.0, more == (void *)-1 ? "fails" : "works", mi.arena / 1048576.0,
                   mi.fordblks / 1048576.0, mi.ordblks);
            heapstat_report();
        }
        return 0;
    }
    heap_in_use += nl_usable(p);
    hs_note(p, (uint32_t)nl_usable(p), 1);
    big_note(p, size, 1, __builtin_return_address(0));
    return guest_addr(p);
}
/* heap blocks are never below the end of the image (that is the runtime area,
 * this executable, or the image): newlib would take such a block in */
static int not_heap(uint32_t a, const char *what)
{
    if (a >= image_end)
        return 0;
    static int told;
    if (told++ < 20)
        rt_log("%s of a non-heap pointer %#x ignored\n", what, a);
    return 1;
}

void gfree(uint32_t a)
{
    if (!a || not_heap(a, "free"))
        return;
    HcHdr *h = heapcheck_on() ? hc_hdr(a) : NULL;
    if (h) {
        if (h->magic == HC_FREED) {
            hc_report("freed twice", a, h->size, 0, 0);
            return;
        }
        uint8_t *u = (uint8_t *)(h + 1);
        for (uint32_t i = 0; i < 16; i++)
            if (u[h->size + i] != HC_CANARY) {
                hc_report("written past its end", a, h->size, h->size + i, u[h->size + i]);
                break;
            }
        h->magic = HC_FREED;
        if (h->size > (64u << 10)) {        /* big blocks leave at once */
            free(h);
            return;
        }
        memset(u, HC_PAINT, h->size);
        pthread_mutex_lock(&hc_lock);
        HcHdr *out = hc_q[hc_qi];
        hc_q[hc_qi] = h;
        hc_qi = (hc_qi + 1) % HC_Q;
        pthread_mutex_unlock(&hc_lock);
        if (out)
            hc_leave(out);
        return;
    }
    if (slab_on) {
        const int k = slab_class(G(a));
        if (k >= 0) {
            heap_in_use -= slab_size_of(k);
            hs_note(G(a), slab_size_of(k), 0);
            slab_free(G(a), k);
            return;
        }
    }
    if (heapcheck_on()) {               /* (a gmemalign block there: newlib's own) */
        free(G(a));
        return;
    }
    const size_t us = nl_usable(G(a));
    heap_in_use -= us;
    hs_note(G(a), (uint32_t)us, 0);
    if (biglog > 0)
        big_note(G(a), us, 0, __builtin_return_address(0));
    const uint64_t t0 = slow_now();
    nl_free(G(a));
    slow_note("free", t0, 0);
}
uint64_t gmalloc_usable(uint32_t a)
{
    if (slab_on) {
        const int k = slab_class(G(a));
        if (k >= 0)
            return slab_size_of(k);
    }
    HcHdr *h = heapcheck_on() ? hc_hdr(a) : NULL;
    return h ? h->size : nl_usable(G(a));
}
int gmalloc_was_fresh(void) { return 0; }     /* newlib memory is never known to be zero */
uint32_t gcalloc(uint64_t n, uint64_t size)
{
    uint64_t total = n * size;
    uint32_t a = gmalloc(total);
    const uint64_t t0 = slow_now();
    if (a)
        memset(G(a), 0, (size_t)total);
    slow_note("calloc's memset", t0, total);
    return a;
}
uint32_t grealloc(uint32_t a, uint64_t size)
{
    if (!a)
        return gmalloc(size);
    if (!size) {
        gfree(a);
        return 0;
    }
    if (not_heap(a, "realloc"))
        rt_fatal("realloc of a non-heap pointer %#x", a);
    size_t old = (size_t)gmalloc_usable(a);
    uint32_t b = gmalloc(size);
    if (!b)
        return 0;
    memcpy(G(b), G(a), old < size ? old : (size_t)size);
    gfree(a);
    return b;
}
uint32_t gmemalign(uint64_t align, uint64_t size)
{
    if (align < 16)
        align = 16;
    void *p = heapcheck_on() ? memalign((size_t)align, (size_t)size) : nl_alloc((size_t)size, (size_t)align);
    if (!p) {
        rt_log("guest heap: %llu bytes aligned to %llu not available\n", (unsigned long long)size,
               (unsigned long long)align);
        return 0;
    }
    if (!heapcheck_on()) {
        heap_in_use += nl_usable(p);
        hs_note(p, (uint32_t)nl_usable(p), 1);
    }
    return guest_addr(p);
}
uint32_t gstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    uint32_t a = gmalloc(n);
    memcpy(G(a), s, n);
    return a;
}
/* what the system still has (thread stacks and vitaGL's memory come from it) */
#include <psp2/kernel/sysmem.h>
void mem_free_report(void)
{
    SceKernelFreeMemorySizeInfo info;
    info.size = sizeof info;
    if (sceKernelGetFreeMemorySize(&info) == 0)
        rt_log("[mem] system free: user %.1f MiB, cdram %.1f MiB, phycont %.1f MiB\n", info.size_user / 1048576.0,
               info.size_cdram / 1048576.0, info.size_phycont / 1048576.0);
}

void mem_report(void)
{
    struct mallinfo mi = mallinfo();
    rt_log("[mem] guest heap %.1f MiB, newlib arena %.1f MiB (in use %.1f, free %.1f, largest free %.1f), image %.1f MiB\n",
           heap_in_use / 1048576.0, mi.arena / 1048576.0, mi.uordblks / 1048576.0, mi.fordblks / 1048576.0,
           mi.keepcost / 1048576.0, image_size / 1048576.0);
    mem_free_report();
    void threads_stack_report(void);
    threads_stack_report();
    void metal_buffers_summary(void);
    metal_buffers_summary();
}

#else

static int strict_mem;
static uint32_t skip_hi = GUEST_IMAGE_BASE;
uint32_t image_skip_lo(void) { return GUEST_IMAGE_BASE; }
uint32_t image_skip_hi(void) { return skip_hi; }

void mem_init(void)
{
    void *p = mmap(NULL, 0x100000000ull, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
        rt_fatal("cannot reserve the guest address space");
    g_mem = p;
    /* Everything above the null guard is read/write (committed lazily by the
     * OS); the first 64 KiB stay unmapped so null pointers fault. */
    if (mprotect(g_mem + GUEST_NULL_GUARD, HEAP_END - GUEST_NULL_GUARD, PROT_READ | PROT_WRITE) != 0)
        rt_fatal("cannot map the guest address space");
    /* the lazy-bank window is never memory: a stray access faults */
    mprotect(g_mem + 0x40000000u, 0x60000000u, PROT_NONE);
    /* MINA_NOTEXT: the Mach-O header and the arm64 code are no memory either,
     * as on the Vita (a read of them faults) */
    if (getenv("MINA_NOTEXT")) {
        skip_hi = g_text_hi;
        mprotect(g_mem + GUEST_IMAGE_BASE, skip_hi - GUEST_IMAGE_BASE, PROT_NONE);
    }
    /* MINA_STRICTMEM: only what is allocated is memory, as on the Vita, where
     * any other guest address is some host memory */
    if (getenv("MINA_STRICTMEM")) {
        uint32_t end = GUEST_IMAGE_BASE;
        for (uint32_t k = 0; k < g_segment_count; k++)
            if (g_segments[k].addr + g_segments[k].size > end)
                end = g_segments[k].addr + g_segments[k].size;
        end = (end + 16383u) & ~16383u;
        /* the runtime area comes from the heap (as on the Vita, where these
         * guest addresses are the executable itself) */
        mprotect(g_mem + GUEST_NULL_GUARD, GUEST_RT_END - GUEST_NULL_GUARD, PROT_NONE);
        mprotect(g_mem + end, 0x40000000u - end, PROT_NONE);
        mprotect(g_mem + HEAP_BASE, HEAP_END - HEAP_BASE, PROT_NONE);
        strict_mem = 1;
    }
}

/* ---- runtime area -------------------------------------------------------- */

static pthread_mutex_t rt_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t rt_top = GUEST_THUNK_END;

uint32_t rt_alloc(uint32_t size, uint32_t align)
{
    if (strict_mem) {
        uint32_t a = gmemalign(align, size);
        memset(G(a), 0, size);
        return a;
    }
    pthread_mutex_lock(&rt_lock);
    uint32_t a = (rt_top + align - 1) & ~(align - 1);
    if (a + size > GUEST_RT_END)
        rt_fatal("runtime area exhausted");
    rt_top = a + size;
    pthread_mutex_unlock(&rt_lock);
    memset(G(a), 0, size);
    return a;
}

/* ---- guest heap -------------------------------------------------------------
 * Segregated size classes, no coalescing: 16-byte steps up to 1 KiB, then four
 * classes per power of two. A 16-byte header before every block holds its
 * class and the requested size. Freed blocks go back to their class list. */

#define HDR 16u
#define NSMALL 64            /* 16 .. 1024 */
#define NCLASS (NSMALL + 4 * 22)

typedef struct BlockHdr {
    uint32_t cls;
    uint32_t magic;
    uint64_t size;           /* requested size */
} BlockHdr;

/* MINA_HEAPMAP: live bytes per allocating site (host return addresses of the
 * generated functions above malloc), reported by mem_report. */
#include <dlfcn.h>
#include <malloc/malloc.h>
#include <unistd.h>
#define SITES 4096
static struct { void *pc; int64_t bytes; int64_t count; } sites[SITES];
static uint32_t *block_site;       /* per 16-byte heap granule: site index + 1 */
static int heapmap = -1;

/* the live blocks of 1 MiB and more, with the guest call chain that made them */
static struct { uint32_t a; uint64_t size; char *chain; } bigs[65536];
static int nbigs;

static void big_add(uint32_t a, uint64_t size)
{
    char bt[512];
    guest_backtrace(bt, sizeof bt, 5);
    for (int i = 0; i < nbigs; i++)
        if (!bigs[i].a) {
            bigs[i].a = a; bigs[i].size = size; bigs[i].chain = strdup(bt);
            return;
        }
    if (nbigs < 65536) {
        bigs[nbigs].a = a; bigs[nbigs].size = size; bigs[nbigs].chain = strdup(bt);
        nbigs++;
    }
}
static void big_remove(uint32_t a)
{
    for (int i = 0; i < nbigs; i++)
        if (bigs[i].a == a) {
            bigs[i].a = 0;
            free(bigs[i].chain);
            return;
        }
}
static void bigs_report(void)
{
    /* totals per chain, biggest first */
    static struct { const char *chain; uint64_t bytes; int n; } t[65536];
    int nt = 0;
    for (int i = 0; i < nbigs; i++) {
        if (!bigs[i].a)
            continue;
        int k = 0;
        while (k < nt && strcmp(t[k].chain, bigs[i].chain))
            k++;
        if (k == nt) { t[nt].chain = bigs[i].chain; t[nt].bytes = 0; t[nt].n = 0; nt++; }
        t[k].bytes += bigs[i].size;
        t[k].n++;
    }
    for (int r = 0; r < 30; r++) {
        int best = -1;
        for (int k = 0; k < nt; k++)
            if (t[k].bytes && (best < 0 || t[k].bytes > t[best].bytes))
                best = k;
        if (best < 0)
            break;
        rt_log("[big] %7.1f MiB %4d x %s\n", t[best].bytes / 1048576.0, t[best].n, t[best].chain);
        t[best].bytes = 0;
    }
}

static int site_of(void *pc)
{
    uint32_t h = (uint32_t)(((uintptr_t)pc >> 2) * 2654435761u) % SITES;
    for (int k = 0; k < SITES; k++, h = (h + 1) % SITES) {
        if (sites[h].pc == pc || !sites[h].pc) {
            sites[h].pc = pc;
            return (int)h;
        }
    }
    return 0;
}

#define MAGIC_USED 0x6D696E61u   /* "mina" */
#define MAGIC_FREE 0x66726565u   /* "free" */

static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t heap_top = HEAP_BASE;
static uint32_t heap_mapped = HEAP_BASE;   /* MINA_STRICTMEM: heap memory ends here */

static void heap_map(void)
{
    if (strict_mem && heap_top > heap_mapped) {
        uint32_t to = (heap_top + (1u << 20) - 1) & ~((1u << 20) - 1);
        mprotect(g_mem + heap_mapped, to - heap_mapped, PROT_READ | PROT_WRITE);
        heap_mapped = to;
    }
}
static __thread int last_fresh;     /* the last gmalloc came from untouched (zero) memory */
static uint32_t free_list[NCLASS];
static uint64_t heap_in_use;

static uint32_t class_of(uint64_t size, uint64_t *cap)
{
    if (size <= 1024) {
        uint32_t k = size ? (uint32_t)((size + 15) / 16) : 1;
        *cap = (uint64_t)k * 16;
        return k - 1;
    }
    /* 1 KiB < size: power-of-two buckets split in 4 */
    int p = 63 - __builtin_clzll(size - 1);          /* 2^p < size <= 2^(p+1) */
    uint64_t base = 1ull << p, step = base / 4;
    uint64_t k = (size - base + step - 1) / step;     /* 1..4 */
    *cap = base + k * step;
    return NSMALL + (uint32_t)((p - 10) * 4 + (k - 1));
}

static uint64_t class_cap(uint32_t cls)
{
    if (cls < NSMALL)
        return (uint64_t)(cls + 1) * 16;
    uint32_t j = cls - NSMALL, p = 10 + j / 4, k = j % 4 + 1;
    uint64_t base = 1ull << p;
    return base + k * (base / 4);
}

/* MINA_HEAPCHECK: every block gets at least 16 canary bytes after its
 * requested size, checked when it is freed or reallocated (overruns that the
 * class rounding hides here corrupt newlib's heap on the Vita). */
#define CANARY 0xFDu
static int heapcheck = -1;

static void canary_set(uint32_t a, uint64_t size, uint64_t cap)
{
    memset(G(a + (uint32_t)size), CANARY, (size_t)(cap - size));
}

static void canary_check(uint32_t a, const char *what)
{
    BlockHdr *h = G(a - HDR);
    uint64_t cap = class_cap(h->cls);
    const uint8_t *p = G(a);
    for (uint64_t i = h->size; i < cap; i++) {
        if (p[i] != CANARY) {
            char bt[512];
            guest_backtrace(bt, sizeof bt, 8);
            rt_log("[heapcheck] %s %#x: block of %llu written at +%llu (%#x) [%s]\n", what, a,
                   (unsigned long long)h->size, (unsigned long long)i, p[i], bt);
            return;
        }
    }
}

#define FREED 0xDDu
#define QUARANTINE 8192
static uint32_t quarantine[QUARANTINE], q_next;

/* a freed block about to be reused: still painted after its free-list link? */
static void freed_check(uint32_t a, uint64_t cap)
{
    const uint8_t *p = G(a);
    for (uint64_t i = 4; i < cap; i++) {
        if (p[i] != FREED) {
            uint32_t w;
            memcpy(&w, p + (i & ~3ull), 4);
            rt_log("[heapcheck] freed block %#x (%llu bytes) written after its free at +%llu (word %#x)\n", a,
                   (unsigned long long)cap, (unsigned long long)i, w);
            return;
        }
    }
}

static uint32_t heap_top;
static pthread_mutex_t heap_lock;

/* walks the heap every second and checks the canaries of the live blocks */
static void *heapcheck_thread(void *arg)
{
    (void)arg;
    for (;;) {
        sleep(1);
        pthread_mutex_lock(&heap_lock);
        for (uint32_t blk = HEAP_BASE; blk < heap_top;) {
            BlockHdr *h = G(blk);
            if (h->magic != MAGIC_USED && h->magic != MAGIC_FREE) {
                rt_log("[heapcheck] heap walk lost at %#x\n", blk);
                break;
            }
            uint64_t cap = class_cap(h->cls);
            if (h->magic == MAGIC_USED) {
                const uint8_t *p = G(blk + HDR);
                for (uint64_t i = h->size; i < cap; i++) {
                    if (p[i] != CANARY) {
                        rt_log("[heapcheck] live block %#x of %llu written at +%llu (%#x)\n", blk + HDR,
                               (unsigned long long)h->size, (unsigned long long)i, p[i]);
                        memset(G(blk + HDR + (uint32_t)h->size), CANARY, (size_t)(cap - h->size));
                        break;
                    }
                }
            }
            blk += HDR + (uint32_t)cap;
        }
        pthread_mutex_unlock(&heap_lock);
    }
    return NULL;
}

uint32_t gmalloc(uint64_t size)
{
    uint64_t cap;
    if (heapcheck < 0) {
        heapcheck = getenv("MINA_HEAPCHECK") != NULL;
        if (heapcheck) {
            pthread_t t;
            pthread_create(&t, NULL, heapcheck_thread, NULL);
        }
    }
    uint64_t want = size;
    if (heapcheck)
        size += 16;
    if (size > 0x40000000ull)
        return 0;
    uint32_t cls = class_of(size, &cap);
    pthread_mutex_lock(&heap_lock);
    uint32_t a = free_list[cls];
    last_fresh = !a;
    const int reused = a != 0;
    if (a) {
        BlockHdr *h = G(a - HDR);
        free_list[cls] = *(uint32_t *)G(a);
        if (h->magic != MAGIC_FREE)
            rt_fatal("guest heap corrupted at %#x", a);
        if (heapcheck)
            freed_check(a, class_cap(cls));
        h->magic = MAGIC_USED;
        h->size = want;
    } else {
        uint32_t blk = heap_top;
        if ((uint64_t)blk + HDR + cap > HEAP_END) {
            pthread_mutex_unlock(&heap_lock);
            rt_log("guest heap exhausted (request %llu)\n", (unsigned long long)size);
            return 0;
        }
        heap_top = blk + HDR + (uint32_t)cap;
        heap_map();
        BlockHdr *h = G(blk);
        h->cls = cls;
        h->magic = MAGIC_USED;
        h->size = want;
        a = blk + HDR;
    }
    /* A reused block comes zeroed, as macOS's malloc gives it (zero on free):
     * the game stores into a new block's fields as if they held nil
     * (ycTexture::AllocStagingBuf released what a reused block held there:
     * samplers and textures died while bound) */
    if (reused && !heapcheck)
        memset(G(a), 0, (size_t)want);
    if (heapcheck)
        canary_set(a, want, cap);
    heap_in_use += cap;
    if (heapmap < 0)
        heapmap = getenv("MINA_HEAPMAP") != NULL;
    if (heapmap) {
        if (!block_site)
            block_site = calloc((HEAP_END - HEAP_BASE) / 16, sizeof *block_site);
        /* walk the frame chain: gmalloc <- imp_malloc <- allocator <- caller */
        void **fp = __builtin_frame_address(0);
        for (int d = 0; d < 3 && fp; d++)
            fp = (void **)fp[0];
        void *pc = fp ? fp[1] : NULL;
        int si = site_of(pc);
        sites[si].bytes += (int64_t)cap;
        sites[si].count++;
        block_site[(a - HEAP_BASE) / 16] = (uint32_t)si + 1;
        if (want >= (64u << 10))
            big_add(a, want);
    }
    pthread_mutex_unlock(&heap_lock);
    return a;
}

void gfree(uint32_t a)
{
    if (!a)
        return;
    BlockHdr *h = G(a - HDR);
    if (h->magic == 0x616C676Eu) {          /* aligned sub-block: free the real one */
        a = (uint32_t)h->size;
        h = G(a - HDR);
    }
    if (heapcheck > 0 && h->magic == MAGIC_USED)
        canary_check(a, "free");
    pthread_mutex_lock(&heap_lock);
    if (h->magic != MAGIC_USED)
        rt_fatal("free of a non-heap or freed pointer %#x", a);
    h->magic = MAGIC_FREE;
    if (heapmap > 0 && block_site) {
        uint32_t si = block_site[(a - HEAP_BASE) / 16];
        if (si) {
            sites[si - 1].bytes -= (int64_t)class_cap(h->cls);
            sites[si - 1].count--;
            block_site[(a - HEAP_BASE) / 16] = 0;
        }
        if (class_cap(h->cls) >= (64u << 10))
            big_remove(a);
    }
    heap_in_use -= class_cap(h->cls);
    if (heapcheck > 0) {
        /* painted, and reused only after QUARANTINE other frees: a write
         * to it in between is a use after free (logged when it is reused) */
        memset(G(a), FREED, (size_t)class_cap(h->cls));
        uint32_t out = quarantine[q_next];
        quarantine[q_next] = a;
        q_next = (q_next + 1) % QUARANTINE;
        if (!out) {
            pthread_mutex_unlock(&heap_lock);
            return;
        }
        a = out;
        h = G(a - HDR);
    }
    *(uint32_t *)G(a) = free_list[h->cls];
    free_list[h->cls] = a;
    pthread_mutex_unlock(&heap_lock);
}

uint64_t gmalloc_usable(uint32_t a)
{
    BlockHdr *h = G(a - HDR);
    return class_cap(h->cls);
}

uint32_t gcalloc(uint64_t n, uint64_t size)
{
    uint64_t total = n * size;
    if (size && total / size != n)
        return 0;
    uint32_t a = gmalloc(total);
    if (a && !last_fresh)
        memset(G(a), 0, total);
    return a;
}
int gmalloc_was_fresh(void) { return last_fresh; }

/* Debug: a block of never-used (zero, untouched) memory, for measuring what the
 * game writes; it is never reused once freed. */
uint32_t gmalloc_fresh(uint64_t size)
{
    pthread_mutex_lock(&heap_lock);
    uint32_t a = (heap_top + 16383u) & ~16383u;
    heap_top = a + (uint32_t)((size + 16383u) & ~16383ull);
    heap_map();
    pthread_mutex_unlock(&heap_lock);
    return a;
}

uint32_t grealloc(uint32_t a, uint64_t size)
{
    if (!a)
        return gmalloc(size);
    if (!size) {
        gfree(a);
        return 0;
    }
    BlockHdr *h = G(a - HDR);
    if (h->magic != MAGIC_USED)
        rt_fatal("realloc of a non-heap pointer %#x", a);
    if (heapcheck > 0)
        canary_check(a, "realloc");
    if (size + (heapcheck > 0 ? 16 : 0) <= class_cap(h->cls)) {
        h->size = size;
        if (heapcheck > 0)
            canary_set(a, size, class_cap(h->cls));
        return a;
    }
    uint32_t b = gmalloc(size);
    if (!b)
        return 0;
    memcpy(G(b), G(a), h->size < size ? h->size : size);
    gfree(a);
    return b;
}

uint32_t gmemalign(uint64_t align, uint64_t size)
{
    if (align <= 16)
        return gmalloc(size);
    /* Over-allocate from the size classes: a block of class >= align whose
     * start is aligned exists only for power-of-two caps, so place an aligned
     * sub-block inside a bigger one and keep a forwarding header. */
    uint32_t raw = gmalloc(size + align + HDR);
    if (!raw)
        return 0;
    uint32_t a = (raw + HDR + (uint32_t)align - 1) & ~((uint32_t)align - 1);
    if (a == raw)
        return raw;
    /* fake header in front of `a` pointing back to the real block */
    BlockHdr *h = G(a - HDR);
    h->cls = 0xFFFFFFFFu;
    h->magic = 0x616C676Eu;   /* "algn" */
    h->size = raw;
    return a;
}

uint32_t gstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    uint32_t a = gmalloc(n);
    memcpy(G(a), s, n);
    return a;
}

/* MINA_MEMSTAT: heap figures for the memory budget (Vita) */
const char *func_name_at(uint32_t pc, uint32_t *start);
void mem_report(void)
{
    static uint64_t peak;
    if (heap_in_use > peak)
        peak = heap_in_use;
    malloc_statistics_t hs;
    malloc_zone_statistics(NULL, &hs);
    rt_log("[mem] heap in use %.1f MiB (peak %.1f), heap span %.1f MiB, host malloc %.1f MiB\n",
           heap_in_use / 1048576.0, peak / 1048576.0, (heap_top - HEAP_BASE) / 1048576.0,
           hs.size_in_use / 1048576.0);
    void metal_buffers_report(void);
    metal_buffers_report();
    if (heapmap > 0) {
        pthread_mutex_lock(&heap_lock);
        bigs_report();
        pthread_mutex_unlock(&heap_lock);
        /* the 12 biggest sites */
        int order[12] = { 0 };
        int64_t best[12] = { 0 };
        for (int i = 0; i < SITES; i++) {
            int64_t b = sites[i].bytes;
            for (int k = 0; k < 12; k++)
                if (b > best[k]) {
                    memmove(best + k + 1, best + k, (size_t)(11 - k) * sizeof best[0]);
                    memmove(order + k + 1, order + k, (size_t)(11 - k) * sizeof order[0]);
                    best[k] = b;
                    order[k] = i;
                    break;
                }
        }
        /* resident (touched) bytes per site: mincore over every used block */
        static int64_t resident[SITES];
        memset(resident, 0, sizeof resident);
        int64_t total_res = 0;
        unsigned char vec[512];
        for (uint32_t a = HEAP_BASE; a < heap_top;) {
            BlockHdr *h = G(a);
            uint64_t cap = class_cap(h->cls);
            uint32_t data = a + HDR;
            if (h->magic == MAGIC_USED && block_site && block_site[(data - HEAP_BASE) / 16]) {
                uint64_t lo = (uint64_t)(uintptr_t)G(data) & ~16383ull;
                uint64_t hi = ((uint64_t)(uintptr_t)G(data) + cap + 16383) & ~16383ull;
                int64_t res = 0;
                for (uint64_t p = lo; p < hi; p += 512ull * 16384) {
                    uint64_t len = hi - p < 512ull * 16384 ? hi - p : 512ull * 16384;
                    if (mincore((void *)(uintptr_t)p, (size_t)len, (char *)vec) == 0)
                        for (uint64_t k = 0; k < len / 16384; k++)
                            res += (vec[k] & 1) ? 16384 : 0;
                }
                if (res > (int64_t)cap) res = (int64_t)cap;
                resident[block_site[(data - HEAP_BASE) / 16] - 1] += res;
                total_res += res;
            }
            a = data + (uint32_t)cap;
        }
        rt_log("[mem] resident (touched) in tracked blocks: %.1f MiB\n", total_res / 1048576.0);
        for (int k = 0; k < 12 && best[k] > 0; k++) {
            Dl_info di;
            const char *nm = "?";
            if (dladdr(sites[order[k]].pc, &di) && di.dli_sname) {
                nm = di.dli_sname;
                if (strncmp(nm, "f_", 2) == 0) {
                    uint32_t st;
                    nm = func_name_at((uint32_t)strtoul(nm + 2, NULL, 16) + GUEST_IMAGE_BASE, &st);
                }
            }
            rt_log("[mem]   %7.1f MiB (touched %6.1f) %6lld blocks  %s\n", best[k] / 1048576.0,
                   resident[order[k]] / 1048576.0, (long long)sites[order[k]].count, nm);
        }
    }
}

#endif

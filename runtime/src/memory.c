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

#define HEAP_BASE   0x07000000u
#if defined(__vita__)
#define HEAP_END    0x10000000u   /* placeholder: the Vita build sizes it at init */
#else
#define HEAP_END    0xF0000000u
#endif

void mem_init(void)
{
#if defined(__vita__)
    rt_fatal("mem_init: Vita memory model not done yet");
#else
    void *p = mmap(NULL, 0x100000000ull, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
        rt_fatal("cannot reserve the guest address space");
    g_mem = p;
    /* Everything above the null guard is read/write (committed lazily by the
     * OS); the first 64 KiB stay unmapped so null pointers fault. */
    if (mprotect(g_mem + GUEST_NULL_GUARD, HEAP_END - GUEST_NULL_GUARD, PROT_READ | PROT_WRITE) != 0)
        rt_fatal("cannot map the guest address space");
#endif
}

/* ---- runtime area -------------------------------------------------------- */

static pthread_mutex_t rt_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t rt_top = GUEST_THUNK_END;

uint32_t rt_alloc(uint32_t size, uint32_t align)
{
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

#define MAGIC_USED 0x6D696E61u   /* "mina" */
#define MAGIC_FREE 0x66726565u   /* "free" */

static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t heap_top = HEAP_BASE;
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

uint32_t gmalloc(uint64_t size)
{
    uint64_t cap;
    if (size > 0x40000000ull)
        return 0;
    uint32_t cls = class_of(size, &cap);
    pthread_mutex_lock(&heap_lock);
    uint32_t a = free_list[cls];
    if (a) {
        BlockHdr *h = G(a - HDR);
        free_list[cls] = *(uint32_t *)G(a);
        if (h->magic != MAGIC_FREE)
            rt_fatal("guest heap corrupted at %#x", a);
        h->magic = MAGIC_USED;
        h->size = size;
    } else {
        uint32_t blk = heap_top;
        if ((uint64_t)blk + HDR + cap > HEAP_END) {
            pthread_mutex_unlock(&heap_lock);
            rt_log("guest heap exhausted (request %llu)\n", (unsigned long long)size);
            return 0;
        }
        heap_top = blk + HDR + (uint32_t)cap;
        BlockHdr *h = G(blk);
        h->cls = cls;
        h->magic = MAGIC_USED;
        h->size = size;
        a = blk + HDR;
    }
    heap_in_use += cap;
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
    pthread_mutex_lock(&heap_lock);
    if (h->magic != MAGIC_USED)
        rt_fatal("free of a non-heap or freed pointer %#x", a);
    h->magic = MAGIC_FREE;
    *(uint32_t *)G(a) = free_list[h->cls];
    free_list[h->cls] = a;
    heap_in_use -= class_cap(h->cls);
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
    if (a)
        memset(G(a), 0, total);
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
    if (size <= class_cap(h->cls)) {
        h->size = size;
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

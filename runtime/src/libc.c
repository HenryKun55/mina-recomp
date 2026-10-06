/* libSystem / libc++ runtime imports. */
#include "runtime.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#if !defined(__vita__)
#include <sys/mman.h>
#endif
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#if !defined(__vita__)
#include <dirent.h>
#endif

/* ---- memory and strings ---------------------------------------------------- */

static void imp_memcpy(Ctx *c) { memcpy(G(A0), G(A1), (size_t)A2); }
static void imp_memmove(Ctx *c) { memmove(G(A0), G(A1), (size_t)A2); }
static void imp_memset(Ctx *c) { memset(G(A0), (int)A1, (size_t)A2); }
static void imp_bzero(Ctx *c) { memset(G(A0), 0, (size_t)A1); }
static void imp_memcmp(Ctx *c) { RET((int64_t)memcmp(G(A0), G(A1), (size_t)A2)); }
static void imp_memchr(Ctx *c)
{
    uint8_t *p = memchr(G(A0), (int)A1, (size_t)A2);
    RET(p ? guest_addr(p) : 0);
}
static void imp_memset_pattern16(Ctx *c)
{
    uint8_t *d = G(A0), *pat = G(A1);
    for (uint64_t k = 0; k < A2; k++)
        d[k] = pat[k & 15];
}
static void imp_strlen(Ctx *c) { RET(strlen(gstr(A0))); }
static void imp_strcmp(Ctx *c) { RET((int64_t)strcmp(gstr(A0), gstr(A1))); }
static void imp_strncmp(Ctx *c) { RET((int64_t)strncmp(gstr(A0), gstr(A1), (size_t)A2)); }
static void imp_strcasecmp(Ctx *c) { RET((int64_t)strcasecmp(gstr(A0), gstr(A1))); }
static void imp_strncasecmp(Ctx *c) { RET((int64_t)strncasecmp(gstr(A0), gstr(A1), (size_t)A2)); }
static void imp_strchr(Ctx *c)
{
    char *p = strchr(gstr(A0), (int)A1);
    RET(p ? guest_addr(p) : 0);
}
static void imp_strstr(Ctx *c)
{
    char *p = strstr(gstr(A0), gstr(A1));
    RET(p ? guest_addr(p) : 0);
}
static void imp_strcasestr(Ctx *c)
{
    const char *h = gstr(A0), *n = gstr(A1);
    size_t nl = strlen(n);
    for (const char *p = h; *p; p++)
        if (strncasecmp(p, n, nl) == 0) {
            RET(guest_addr(p));
            return;
        }
    RET(nl ? 0 : A0);
}
static void imp_strdup(Ctx *c) { RET(gstrdup(gstr(A0))); }
static void imp_strlcpy(Ctx *c)
{
    const char *s = gstr(A1);
    size_t n = strlen(s), cap = (size_t)A2;
    if (cap) {
        size_t k = n < cap - 1 ? n : cap - 1;
        memcpy(G(A0), s, k);
        ((char *)G(A0))[k] = 0;
    }
    RET(n);
}
static void imp_strlcat(Ctx *c)
{
    char *d = G(A0);
    const char *s = gstr(A1);
    size_t cap = (size_t)A2, dl = strnlen(d, cap), sl = strlen(s);
    if (dl < cap) {
        size_t k = sl < cap - dl - 1 ? sl : cap - dl - 1;
        memcpy(d + dl, s, k);
        d[dl + k] = 0;
    }
    RET(dl + sl);
}
static void imp_strerror(Ctx *c)
{
    static uint32_t buf;
    if (!buf)
        buf = rt_alloc(128, 16);
    snprintf(G(buf), 128, "%s", strerror((int)A0));
    RET(buf);
}
static void imp_strtol(Ctx *c)
{
    char *end;
    long v = strtol(gstr(A0), &end, (int)A2);
    if (A1) rt_st64(G(A1), guest_addr(end));
    RET((int64_t)v);
}
static void imp_strtoul(Ctx *c)
{
    char *end;
    unsigned long long v = strtoull(gstr(A0), &end, (int)A2);
    if (A1) rt_st64(G(A1), guest_addr(end));
    RET(v);
}
static void imp_strtod(Ctx *c)
{
    char *end;
    double v = strtod(gstr(A0), &end);
    if (A1) rt_st64(G(A1), guest_addr(end));
    RETD(v);
}
static void imp_atoi(Ctx *c) { RET((int64_t)atoi(gstr(A0))); }
static void imp_atof(Ctx *c) { RETD(atof(gstr(A0))); }
static void imp_tolower(Ctx *c) { RET((int64_t)tolower((int)A0)); }
static void imp_wcslen(Ctx *c)
{
    uint64_t n = 0;
    while (rt_ld32(G(A0 + 4 * n)))
        n++;
    RET(n);
}
static void imp_wcscpy(Ctx *c)
{
    uint64_t k = 0;
    uint32_t ch;
    do {
        ch = rt_ld32(G(A1 + 4 * k));
        rt_st32(G(A0 + 4 * k), ch);
        k++;
    } while (ch);
    RET(A0);
}

/* ---- math ------------------------------------------------------------------- */

static void imp_sinf(Ctx *c) { RETF(sinf(FARG(0))); }
static void imp_cosf(Ctx *c) { RETF(cosf(FARG(0))); }
static void imp_tanf(Ctx *c) { RETF(tanf(FARG(0))); }
static void imp_asinf(Ctx *c) { RETF(asinf(FARG(0))); }
static void imp_acosf(Ctx *c) { RETF(acosf(FARG(0))); }
static void imp_atan2f(Ctx *c) { RETF(atan2f(FARG(0), FARG(1))); }
static void imp_expf(Ctx *c) { RETF(expf(FARG(0))); }
static void imp_exp2f(Ctx *c) { RETF(exp2f(FARG(0))); }
static void imp_logf(Ctx *c) { RETF(logf(FARG(0))); }
static void imp_log2f(Ctx *c) { RETF(log2f(FARG(0))); }
static void imp_powf(Ctx *c) { RETF(powf(FARG(0), FARG(1))); }
static void imp_fmodf(Ctx *c) { RETF(fmodf(FARG(0), FARG(1))); }
static void imp_exp10f(Ctx *c) { RETF(powf(10.0f, FARG(0))); }
static void imp_sin(Ctx *c) { RETD(sin(DARG(0))); }
static void imp_cos(Ctx *c) { RETD(cos(DARG(0))); }
static void imp_atan(Ctx *c) { RETD(atan(DARG(0))); }
static void imp_atan2(Ctx *c) { RETD(atan2(DARG(0), DARG(1))); }
static void imp_exp(Ctx *c) { RETD(exp(DARG(0))); }
static void imp_log(Ctx *c) { RETD(log(DARG(0))); }
static void imp_pow(Ctx *c) { RETD(pow(DARG(0), DARG(1))); }
static void imp_fmod(Ctx *c) { RETD(fmod(DARG(0), DARG(1))); }
static void imp_ldexp(Ctx *c) { RETD(ldexp(DARG(0), (int)A0)); }
static void imp_exp10(Ctx *c) { RETD(pow(10.0, DARG(0))); }
static void imp_sincos_stret(Ctx *c)
{
    double x = DARG(0);
    c->v[0] = vset_f64(sin(x));
    c->v[1] = vset_f64(cos(x));
}
static void imp_sincosf_stret(Ctx *c)
{
    float x = FARG(0);
    c->v[0] = vset_f32(sinf(x));
    c->v[1] = vset_f32(cosf(x));
}

/* ---- lazy files ---------------------------------------------------------------
 * The engine reads whole files into memory (ycFile::Read: fopen, ftell, malloc,
 * fread). The music banks (700 MB of Ogg streams behind small metadata) stay on
 * the card instead:
 *  - malloc of the bank's size returns a block holding only a prefix (the
 *    metadata), and fread fills only that prefix;
 *  - once the bank is parsed, every pointer in the prefix that points at an Ogg
 *    stream of the bank (checked: "OggS" at that file offset) is moved into a
 *    window of guest addresses that no allocation ever uses;
 *  - the Ogg reader and ov_open read window addresses from the file. */

#include <fcntl.h>

#define LAZY_PREFIX (1u << 20)
#define LAZY_WINDOW_BASE 0x40000000u
#define LAZY_WINDOW_END 0xA0000000u

typedef struct LazyBlob {
    char path[512];
    int fd;
    uint64_t size;
    uint32_t base;           /* guest address of the malloc'ed block (0 until allocated) */
    uint32_t data;           /* where the file's first byte went (the fread target) */
    uint32_t loaded;         /* bytes of the file actually in memory */
    uint32_t win;            /* window address of the file's first byte */
    int fixed;               /* pointers moved into the window */
} LazyBlob;

static LazyBlob lazy[8];
static int nlazy;
static uint32_t next_window = LAZY_WINDOW_BASE;
static pthread_mutex_t lazy_lock = PTHREAD_MUTEX_INITIALIZER;
#if defined(__vita__)
#define lazy_pending (host_tls()->lazy_pending)
#else
static __thread int lazy_pending = -1;     /* blob whose malloc is expected on this thread */
#endif

static int lazy_candidate(const char *path)
{
    const char *e = getenv("MINA_LAZY_BANKS");
    if (e && *e == '0')
        return 0;
    /* big sound banks (music): sound effects stay in memory for latency */
    size_t n = strlen(path);
    struct stat st;
    return n >= 10 && strcmp(path + n - 10, ".sndbnk.yc") == 0 && stat(path, &st) == 0 &&
           st.st_size > (32 << 20);
}

/* Moves the bank's Ogg pointers into its window (after the engine parsed it). */
void lazy_fixup(void)
{
    pthread_mutex_lock(&lazy_lock);
    for (int i = 0; i < nlazy; i++) {
        LazyBlob *b = &lazy[i];
        if (!b->data || b->fixed)
            continue;
        b->fixed = 1;
        int moved = 0;
        uint32_t lo = (b->data + 7) & ~7u, hi = b->data + b->loaded;
        for (uint32_t a = lo; a + 8 <= hi; a += 8) {
            uint64_t v = rt_ld64(G(a));
            if (v < b->data || v >= (uint64_t)b->data + b->size)
                continue;
            uint64_t off = v - b->data;
            char magic[4];
            if (pread(b->fd, magic, 4, (off_t)off) == 4 && memcmp(magic, "OggS", 4) == 0) {
                rt_st64(G(a), (uint64_t)b->win + off);
                moved++;
            }
        }
        rt_log("[lazy] %s: %d streams on the card\n", b->path, moved);
    }
    pthread_mutex_unlock(&lazy_lock);
}

int lazy_contains(uint32_t addr) { return addr >= LAZY_WINDOW_BASE && addr < LAZY_WINDOW_END; }

/* Guest Ogg reads: window address -> file */
int lazy_read(uint32_t addr, void *dst, uint32_t n)
{
    if (!lazy_contains(addr))
        return 0;
    for (int i = 0; i < nlazy; i++) {
        LazyBlob *b = &lazy[i];
        if (addr >= b->win && addr < b->win + b->size) {
            ssize_t r = pread(b->fd, dst, n, (off_t)(addr - b->win));
            if (r != (ssize_t)n)
                rt_log("[lazy] read of %u bytes at %u: %zd\n", n, addr - b->win, r);
            return 1;
        }
    }
    rt_log("[lazy] window address %#x of no bank\n", addr);
    memset(dst, 0, n);
    return 1;
}

static void lazy_freed(uint32_t a)
{
    for (int i = 0; i < nlazy; i++)
        if (lazy[i].base == a) {
            lazy[i].base = lazy[i].data = 0;   /* its window slice is never reused */
            close(lazy[i].fd);
            lazy[i].fd = -1;
        }
}

/* ---- heap ----------------------------------------------------------------- */

static void log_big(Ctx *c, const char *what, uint64_t n)
{
    static int on = -1;
    if (on < 0)
        on = getenv("MINA_MEMSTAT") != NULL;
    if (on && n >= (64u << 10)) {
        char bt[1024];
        guest_backtrace(bt, sizeof bt, n >= (4u << 20) ? 4 : 3);
        rt_log("[alloc] %s %.1f MiB: %s\n", what, n / 1048576.0, bt);
    }
}
static void imp_malloc(Ctx *c)
{
    if (lazy_pending >= 0 && A0 >= lazy[lazy_pending].size && A0 <= lazy[lazy_pending].size + 4096) {
        LazyBlob *b = &lazy[lazy_pending];
        lazy_pending = -1;
        b->base = gmalloc(LAZY_PREFIX);
        b->data = 0;
        rt_log("[lazy] %s: %.1f MiB stay on the card\n", b->path, b->size / 1048576.0);
        RET(b->base);
        return;
    }
    log_big(c, "malloc", A0);
    RET(gmalloc(A0));
}
static void imp_calloc(Ctx *c) { log_big(c, "calloc", A0 * A1); RET(gcalloc(A0, A1)); }
static void imp_realloc(Ctx *c) { log_big(c, "realloc", A1); RET(grealloc((uint32_t)A0, A1)); }
static void imp_free(Ctx *c)
{
    if (nlazy)
        lazy_freed((uint32_t)A0);
    gfree((uint32_t)A0);
}

/* ---- errno ------------------------------------------------------------------ */

#if defined(__vita__)
#define tl_errno (host_tls()->err)
#else
static __thread uint32_t tl_errno;
#endif
static void imp_error(Ctx *c)
{
    (void)c;
    if (!tl_errno)
        tl_errno = rt_alloc(16, 16);
    RET(tl_errno);
}
static void set_errno(int e)
{
    if (!tl_errno)
        tl_errno = rt_alloc(16, 16);
    rt_st32(G(tl_errno), (uint32_t)e);
}

/* ---- formatted output --------------------------------------------------------
 * Apple arm64: variadic arguments live in consecutive 8-byte stack slots; a
 * va_list is the address of the next slot. */

int gvformat(char *out, size_t cap, const char *fmt, uint32_t va)
{
    size_t n = 0;
#define PUT(s, len)                                                     \
    do {                                                                \
        size_t l_ = (len);                                              \
        for (size_t i_ = 0; i_ < l_; i_++) {                            \
            if (n + 1 < cap) out[n] = (s)[i_];                          \
            n++;                                                        \
        }                                                               \
    } while (0)
    for (const char *p = fmt; *p;) {
        if (*p != '%') {
            PUT(p, 1);
            p++;
            continue;
        }
        const char *start = p++;
        char spec[64];
        size_t sl = 0;
        spec[sl++] = '%';
        while (*p && strchr("-+ #0'", *p))
            spec[sl++] = *p++;
        if (*p == '*') {
            sl += (size_t)snprintf(spec + sl, sizeof spec - sl, "%d", (int)rt_ld64(G(va)));
            va += 8;
            p++;
        }
        while (isdigit((unsigned char)*p))
            spec[sl++] = *p++;
        if (*p == '.') {
            spec[sl++] = *p++;
            if (*p == '*') {
                sl += (size_t)snprintf(spec + sl, sizeof spec - sl, "%d", (int)rt_ld64(G(va)));
                va += 8;
                p++;
            }
            while (isdigit((unsigned char)*p))
                spec[sl++] = *p++;
        }
        int lng = 0;      /* 0 int, 1 long, 2 long long, -1 short, -2 char, 3 size_t */
        for (;;) {
            if (*p == 'l') { lng = lng == 1 ? 2 : 1; p++; }
            else if (*p == 'h') { lng = lng == -1 ? -2 : -1; p++; }
            else if (*p == 'q' || *p == 'j') { lng = 2; p++; }
            else if (*p == 'z' || *p == 't') { lng = 3; p++; }
            else if (*p == 'L') { p++; }
            else break;
        }
        char conv = *p ? *p++ : 0;
        char buf[512];
        int len = 0;
        switch (conv) {
        case '%':
            PUT("%", 1);
            continue;
        case 'd': case 'i': {
            int64_t v = (int64_t)rt_ld64(G(va));
            va += 8;
            if (lng == 0) v = (int32_t)v;
            else if (lng == -1) v = (int16_t)v;
            else if (lng == -2) v = (int8_t)v;
            spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = 0;
            len = snprintf(buf, sizeof buf, spec, (long long)v);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t v = rt_ld64(G(va));
            va += 8;
            if (lng == 0) v = (uint32_t)v;
            else if (lng == -1) v = (uint16_t)v;
            else if (lng == -2) v = (uint8_t)v;
            spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = conv; spec[sl] = 0;
            len = snprintf(buf, sizeof buf, spec, (unsigned long long)v);
            break;
        }
        case 'c': {
            int v = (int)rt_ld64(G(va));
            va += 8;
            spec[sl++] = 'c'; spec[sl] = 0;
            len = snprintf(buf, sizeof buf, spec, v);
            break;
        }
        case 'p': {
            uint64_t v = rt_ld64(G(va));
            va += 8;
            len = snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)v);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double v = u2d(rt_ld64(G(va)));
            va += 8;
            spec[sl++] = conv; spec[sl] = 0;
            len = snprintf(buf, sizeof buf, spec, v);
            break;
        }
        case 's': {
            uint64_t a = rt_ld64(G(va));
            va += 8;
            const char *s = a ? gstr(a) : "(null)";
            spec[sl++] = 's'; spec[sl] = 0;
            /* the string may be longer than buf */
            int need = snprintf(NULL, 0, spec, s);
            if (need < (int)sizeof buf) {
                len = snprintf(buf, sizeof buf, spec, s);
            } else {
                char *big = malloc((size_t)need + 1);
                snprintf(big, (size_t)need + 1, spec, s);
                PUT(big, (size_t)need);
                free(big);
                continue;
            }
            break;
        }
        case 'n':
            va += 8;
            continue;
        default:
            PUT(start, (size_t)(p - start));
            continue;
        }
        if (len > 0)
            PUT(buf, (size_t)(len < (int)sizeof buf ? len : (int)sizeof buf - 1));
    }
    if (cap)
        out[n < cap ? n : cap - 1] = 0;
#undef PUT
    return (int)n;
}

static void imp_vsnprintf(Ctx *c) { RET((int64_t)gvformat(A0 ? G(A0) : NULL, (size_t)A1, gstr(A2), (uint32_t)A3)); }
static void imp_snprintf(Ctx *c) { RET((int64_t)gvformat(A0 ? G(A0) : NULL, (size_t)A1, gstr(A2), (uint32_t)c->sp)); }

/* sscanf: one host sscanf per directive (with %n to follow the input). */
static int gvscan(const char *in, const char *fmt, uint32_t va)
{
    int assigned = 0;
    const char *ip = in;
    for (const char *p = fmt; *p;) {
        if (isspace((unsigned char)*p)) {
            while (isspace((unsigned char)*ip)) ip++;
            p++;
            continue;
        }
        if (*p != '%' || p[1] == '%') {
            if (*p == '%') p++;
            if (*ip != *p) return assigned ? assigned : (*ip ? 0 : -1);
            ip++;
            p++;
            continue;
        }
        const char *start = p++;
        int suppress = 0;
        if (*p == '*') { suppress = 1; p++; }
        while (isdigit((unsigned char)*p)) p++;
        int lng = 0;
        for (;;) {
            if (*p == 'l') { lng = lng == 1 ? 2 : 1; p++; }
            else if (*p == 'h') { lng = lng == -1 ? -2 : -1; p++; }
            else if (*p == 'q' || *p == 'j' || *p == 'z' || *p == 't') { lng = 2; p++; }
            else if (*p == 'L') { p++; }
            else break;
        }
        char conv = *p;
        const char *end = p + 1;
        if (conv == '[') {
            end = p + 1;
            if (*end == '^') end++;
            if (*end == ']') end++;
            while (*end && *end != ']') end++;
            if (*end) end++;
        }
        /* host piece: same directive with a host-sized target, then %n */
        char piece[128];
        size_t plen = (size_t)(end - start);
        if (plen > 100) return assigned;
        memcpy(piece, start, plen);
        piece[plen] = 0;
        if (conv == 'n') {
            if (!suppress) rt_st32(G(rt_ld64(G(va))), (uint32_t)(ip - in));
            if (!suppress) va += 8;
            p = end;
            continue;
        }
        if (!*ip && conv != 'n') return assigned ? assigned : -1;
        int consumed = 0, r;
        char host[1024];
        long long iv = 0;
        double dv = 0;
        char *q = piece + plen;
        strcpy(q, "%n");
        /* rewrite the length modifier for the host target */
        char hp[128];
        size_t hl = 0;
        for (const char *s = piece; *s; s++) {
            if (s > piece && (*s == 'l' || *s == 'h' || *s == 'q' || *s == 'j' || *s == 'z' || *s == 't' || *s == 'L') && s < piece + plen)
                continue;
            hp[hl++] = *s;
        }
        hp[hl] = 0;
        char hp2[160];
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            /* insert ll before the conversion */
            size_t k = 0, pos = (size_t)(strchr(hp, conv) - hp);
            memcpy(hp2, hp, pos); k = pos;
            hp2[k++] = 'l'; hp2[k++] = 'l';
            strcpy(hp2 + k, hp + pos);
            r = suppress ? sscanf(ip, hp2, &consumed) : sscanf(ip, hp2, &iv, &consumed);
            if (r < (suppress ? 0 : 1) || (!suppress && r == 0)) return assigned;
            if (!suppress) {
                uint32_t dst = (uint32_t)rt_ld64(G(va));
                va += 8;
                if (lng == 2 || lng == 1) rt_st64(G(dst), (uint64_t)iv);
                else if (lng == -1) rt_st16(G(dst), (uint32_t)iv);
                else if (lng == -2) rt_st8(G(dst), (uint32_t)iv);
                else rt_st32(G(dst), (uint32_t)iv);
                assigned++;
            }
            break;
        }
        case 'f': case 'e': case 'g': case 'E': case 'G': case 'a': {
            size_t k = 0, pos = (size_t)(strchr(hp, conv) - hp);
            memcpy(hp2, hp, pos); k = pos;
            hp2[k++] = 'l';
            strcpy(hp2 + k, hp + pos);
            r = suppress ? sscanf(ip, hp2, &consumed) : sscanf(ip, hp2, &dv, &consumed);
            if (!suppress && r < 1) return assigned;
            if (!suppress) {
                uint32_t dst = (uint32_t)rt_ld64(G(va));
                va += 8;
                if (lng >= 1) rt_st64(G(dst), d2u(dv));
                else rt_st32(G(dst), f2u((float)dv));
                assigned++;
            }
            break;
        }
        case 's': case '[': case 'c': {
            r = suppress ? sscanf(ip, hp, &consumed) : sscanf(ip, hp, host, &consumed);
            if (!suppress && r < 1) return assigned;
            if (!suppress) {
                uint32_t dst = (uint32_t)rt_ld64(G(va));
                va += 8;
                if (conv == 'c') {
                    int w = atoi(start + 1);
                    memcpy(G(dst), host, (size_t)(w ? w : 1));
                } else {
                    strcpy(G(dst), host);
                }
                assigned++;
            }
            break;
        }
        default:
            return assigned;
        }
        ip += consumed;
        p = end;
    }
    return assigned;
}

static void imp_sscanf(Ctx *c) { RET((int64_t)gvscan(gstr(A0), gstr(A1), (uint32_t)c->sp)); }
static void imp_vsscanf(Ctx *c) { RET((int64_t)gvscan(gstr(A0), gstr(A1), (uint32_t)A2)); }

/* ---- files ------------------------------------------------------------------------
 * A guest FILE* is a 32-byte guest block: +0 magic, +8 host FILE*. */

#define FILE_MAGIC 0x46494C45u
static uint32_t g_stdin, g_stdout, g_stderr;

static uint32_t wrap_file(FILE *f)
{
    uint32_t a = gmalloc(32);
    memset(G(a), 0, 32);
    rt_st32(G(a), FILE_MAGIC);
    rt_st64(G(a + 8), (uint64_t)(uintptr_t)f);
    return a;
}
static FILE *host_file(uint64_t a)
{
    if (!a || rt_ld32(G(a)) != FILE_MAGIC)
        return NULL;
    return (FILE *)(uintptr_t)rt_ld64(G(a + 8));
}

/* The current directory is the runtime's: the game chdir()s into its
 * resources and opens relative paths, and the Vita has no working directory. */
static char cwd[512];
static pthread_mutex_t cwd_lock = PTHREAD_MUTEX_INITIALIZER;

/* Host path of a guest path: relative paths are resolved against cwd. */
const char *host_path(const char *p, char *buf, size_t cap)
{
    if (!p || !p[0])
        return p;
    int absolute = p[0] == '/' || strchr(p, ':') != NULL;
    pthread_mutex_lock(&cwd_lock);
    if (absolute || !cwd[0]) {
        pthread_mutex_unlock(&cwd_lock);
        return p;
    }
    const char *q = p;
    while (q[0] == '.' && q[1] == '/')
        q += 2;
    size_t n = strlen(cwd);
    snprintf(buf, cap, "%s%s%s", cwd, n && cwd[n - 1] == '/' ? "" : "/", q);
    pthread_mutex_unlock(&cwd_lock);
    return buf;
}

static void imp_fopen(Ctx *c)
{
    char pb[1024];
    const char *path = host_path(gstr(A0), pb, sizeof pb);
    FILE *f = fopen(path, gstr(A1));
    if (g_trace_imports)
        rt_log("fopen(%s, %s) -> %p\n", path, gstr(A1), (void *)f);
    if (!f) {
        set_errno(errno);
        RET(0);
        return;
    }
    uint32_t g = wrap_file(f);
    if (lazy_candidate(path) && nlazy < 8) {
        LazyBlob *b = &lazy[nlazy];
        memset(b, 0, sizeof *b);
        snprintf(b->path, sizeof b->path, "%s", path);
        b->fd = open(path, O_RDONLY);
        fseeko(f, 0, SEEK_END);
        b->size = (uint64_t)ftello(f);
        fseeko(f, 0, SEEK_SET);
        b->win = next_window;
        next_window += (uint32_t)((b->size + (16u << 20)) & ~((16ull << 20) - 1));
        if (next_window > LAZY_WINDOW_END)
            rt_fatal("lazy window exhausted");
        rt_st32(G(g + 4), (uint32_t)nlazy + 1);    /* marks the FILE as lazy */
        nlazy++;
    }
    RET(g);
}
static void imp_fclose(Ctx *c)
{
    FILE *f = host_file(A0);
    int r = f ? fclose(f) : -1;
    if (f) gfree((uint32_t)A0);
    RET((int64_t)r);
}
static void imp_fread(Ctx *c)
{
    FILE *f = host_file(A3);
    if (!f) {
        RET(0);
        return;
    }
    uint32_t li = rt_ld32(G(A3 + 4));
    uint64_t want = A1 * A2;
    LazyBlob *lb = li ? &lazy[li - 1] : NULL;
    if (lb && lb->base && A0 >= lb->base && A0 < lb->base + LAZY_PREFIX && want > LAZY_PREFIX / 2) {
        /* the allocator may put a header first: the file starts at A0 */
        size_t room = LAZY_PREFIX - (size_t)(A0 - lb->base);
        size_t got = fread(G(A0), 1, room, f);
        lb->loaded = (uint32_t)got;
        lb->fixed = 0;
        lb->data = (uint32_t)A0;
        fseeko(f, (off_t)want - (off_t)got, SEEK_CUR);
        RET(A1 ? want / A1 : 0);
        return;
    }
    RET(fread(G(A0), (size_t)A1, (size_t)A2, f));
}
static void imp_fwrite(Ctx *c) { FILE *f = host_file(A3); RET(f ? fwrite(G(A0), (size_t)A1, (size_t)A2, f) : 0); }
static void imp_fseek(Ctx *c) { FILE *f = host_file(A0); RET(f ? (int64_t)fseeko(f, (off_t)(int64_t)A1, (int)A2) : -1); }
static void imp_ftell(Ctx *c)
{
    FILE *f = host_file(A0);
    if (!f) {
        RET((uint64_t)-1);
        return;
    }
    uint32_t li = rt_ld32(G(A0 + 4));
    off_t pos = ftello(f);
    if (li && (uint64_t)pos == lazy[li - 1].size)
        lazy_pending = (int)li - 1;            /* the next malloc of this size is the blob */
    RET((int64_t)pos);
}
static void imp_fflush(Ctx *c) { FILE *f = host_file(A0); RET(f ? (int64_t)fflush(f) : 0); }
static void imp_ferror(Ctx *c) { FILE *f = host_file(A0); RET(f ? (int64_t)ferror(f) : 1); }
static void imp_rewind(Ctx *c) { FILE *f = host_file(A0); if (f) rewind(f); }
static void imp_fileno(Ctx *c) { FILE *f = host_file(A0); RET(f ? (int64_t)fileno(f) : -1); }
static void imp_fgets(Ctx *c)
{
    FILE *f = host_file(A2);
    char *r = f ? fgets(G(A0), (int)A1, f) : NULL;
    RET(r ? A0 : 0);
}

static void put_stat(uint32_t a, const struct stat *st)
{
    memset(G(a), 0, 144);
    rt_st32(G(a + 0), (uint32_t)st->st_dev);
    rt_st16(G(a + 4), (uint32_t)st->st_mode);
    rt_st16(G(a + 6), (uint32_t)st->st_nlink);
    rt_st64(G(a + 8), (uint64_t)st->st_ino);
    rt_st32(G(a + 16), (uint32_t)st->st_uid);
    rt_st32(G(a + 20), (uint32_t)st->st_gid);
#if defined(__APPLE__)
    rt_st64(G(a + 32), (uint64_t)st->st_atimespec.tv_sec);
    rt_st64(G(a + 48), (uint64_t)st->st_mtimespec.tv_sec);
    rt_st64(G(a + 56), (uint64_t)st->st_mtimespec.tv_nsec);
    rt_st64(G(a + 64), (uint64_t)st->st_ctimespec.tv_sec);
#else
    rt_st64(G(a + 32), (uint64_t)st->st_atime);
    rt_st64(G(a + 48), (uint64_t)st->st_mtime);
    rt_st64(G(a + 64), (uint64_t)st->st_ctime);
#endif
    rt_st64(G(a + 96), (uint64_t)st->st_size);
    rt_st64(G(a + 104), (uint64_t)st->st_blocks);
    rt_st32(G(a + 112), (uint32_t)st->st_blksize);
}
static void imp_stat(Ctx *c)
{
    struct stat st;
    char pb[1024];
    int r = stat(host_path(gstr(A0), pb, sizeof pb), &st);
    if (r == 0) put_stat((uint32_t)A1, &st);
    else set_errno(errno);
    RET((int64_t)r);
}
static void imp_fstat(Ctx *c)
{
    struct stat st;
    int r = fstat((int)A0, &st);
    if (r == 0) put_stat((uint32_t)A1, &st);
    RET((int64_t)r);
}
static void imp_mkdir(Ctx *c)
{
    char pb[1024];
    int r = mkdir(host_path(gstr(A0), pb, sizeof pb), (mode_t)A1);
    if (r) set_errno(errno);
    RET((int64_t)r);
}
static void imp_getcwd(Ctx *c)
{
    pthread_mutex_lock(&cwd_lock);
    if (!cwd[0] && !getcwd(cwd, sizeof cwd))
        cwd[0] = 0;
    snprintf(G(A0), (size_t)A1, "%s", cwd);
    pthread_mutex_unlock(&cwd_lock);
    RET(A0);
}
static void imp_chdir(Ctx *c)
{
    char pb[512];
    const char *p = host_path(gstr(A0), pb, sizeof pb);
    struct stat st;
    if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) {
        set_errno(ENOENT);
        RET((uint64_t)-1);
        return;
    }
    pthread_mutex_lock(&cwd_lock);
    snprintf(cwd, sizeof cwd, "%s", p);
    pthread_mutex_unlock(&cwd_lock);
    rt_log("[fs] chdir %s\n", cwd);
    RET(0);
}

static void stderr_init(uint32_t a) { if (!g_stderr) g_stderr = wrap_file(stderr); rt_st64(G(a), g_stderr); }
static void stdin_init(uint32_t a) { if (!g_stdin) g_stdin = wrap_file(stdin); rt_st64(G(a), g_stdin); }
static void stdout_init(uint32_t a) { if (!g_stdout) g_stdout = wrap_file(stdout); rt_st64(G(a), g_stdout); }

/* ---- time --------------------------------------------------------------------- */

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void imp_mach_absolute_time(Ctx *c) { RET(mono_ns()); }
static void imp_mach_timebase_info(Ctx *c) { rt_st32(G(A0), 1); rt_st32(G(A0 + 4), 1); RET(0); }
static void imp_clock_gettime_nsec_np(Ctx *c)
{
    struct timespec ts;
    clock_gettime(A0 == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &ts);
    RET((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec);
}
static void imp_gettimeofday(Ctx *c)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (A0) {
        rt_st64(G(A0), (uint64_t)tv.tv_sec);
        rt_st32(G(A0 + 8), (uint32_t)tv.tv_usec);
    }
    RET(0);
}

/* ---- process ---------------------------------------------------------------------- */

static void imp_getenv(Ctx *c)
{
    const char *n = gstr(A0);
    const char *v = getenv(n);
    if (g_trace_imports)
        rt_log("getenv(%s) -> %s\n", n, v ? v : "(null)");
    if (!v || strncmp(n, "SDL_", 4) == 0) {
        RET(0);
        return;
    }
    RET(gstrdup(v));
}
static void imp_getpagesize(Ctx *c) { RET(16384); }
static void imp_abort(Ctx *c) { (void)c; rt_fatal("guest called abort()"); }
static void imp_exit(Ctx *c) { exit((int)A0); }
static void imp_stack_chk_fail(Ctx *c) { (void)c; rt_fatal("guest stack check failed"); }
static void stack_guard_init(uint32_t a) { rt_st64(G(a), 0x2F8A6C3D9B1E4F70ull); }
static void imp_nop(Ctx *c) { (void)c; }
static void imp_ret0(Ctx *c) { RET(0); }
static void imp_ret1(Ctx *c) { RET(1); }
static void imp_retm1(Ctx *c) { RET((uint64_t)-1); }

/* atexit / __cxa_atexit: run in reverse order at exit */
#define MAX_ATEXIT 256
static struct { uint64_t fn, arg; int has_arg; } atexit_list[MAX_ATEXIT];
static int atexit_n;
static void imp_atexit(Ctx *c)
{
    if (atexit_n < MAX_ATEXIT) {
        atexit_list[atexit_n].fn = A0;
        atexit_list[atexit_n].has_arg = 0;
        atexit_n++;
    }
    RET(0);
}
static void imp_cxa_atexit(Ctx *c)
{
    if (atexit_n < MAX_ATEXIT) {
        atexit_list[atexit_n].fn = A0;
        atexit_list[atexit_n].arg = A1;
        atexit_list[atexit_n].has_arg = 1;
        atexit_n++;
    }
    RET(0);
}
void run_atexit(Ctx *c)
{
    while (atexit_n > 0) {
        atexit_n--;
        guest_call(c, atexit_list[atexit_n].fn, 1, atexit_list[atexit_n].arg);
    }
}

/* sysctl / sysctlbyname */
static void imp_sysctl(Ctx *c)
{
    /* name[0..1]: CTL_HW(6)/HW_MEMSIZE(24) or HW_NCPU(3), CTL_KERN(1)/... */
    uint32_t n0 = rt_ld32(G(A0)), n1 = A1 > 1 ? rt_ld32(G(A0 + 4)) : 0;
    uint32_t oldp = (uint32_t)A2, oldlenp = (uint32_t)A3;
    if (n0 == 6 && n1 == 24 && oldp) {
        rt_st64(G(oldp), 8ull << 30);
        if (oldlenp) rt_st64(G(oldlenp), 8);
        RET(0);
        return;
    }
    if (n0 == 6 && (n1 == 3 || n1 == 25) && oldp) {
        rt_st32(G(oldp), 4);
        if (oldlenp) rt_st64(G(oldlenp), 4);
        RET(0);
        return;
    }
    if (g_trace_imports)
        rt_log("sysctl(%u, %u) unsupported\n", n0, n1);
    set_errno(ENOENT);
    RET((uint64_t)-1);
}
static void imp_sysctlbyname(Ctx *c)
{
    const char *n = gstr(A0);
    uint32_t oldp = (uint32_t)A1, oldlenp = (uint32_t)A2;
    if (strcmp(n, "hw.memsize") == 0 && oldp) {
        rt_st64(G(oldp), 8ull << 30);
        if (oldlenp) rt_st64(G(oldlenp), 8);
        RET(0);
        return;
    }
    if ((strcmp(n, "hw.ncpu") == 0 || strcmp(n, "hw.logicalcpu") == 0 ||
         strcmp(n, "hw.physicalcpu") == 0) && oldp) {
        rt_st32(G(oldp), 4);
        if (oldlenp) rt_st64(G(oldlenp), 4);
        RET(0);
        return;
    }
    if (g_trace_imports)
        rt_log("sysctlbyname(%s) unsupported\n", n);
    set_errno(ENOENT);
    RET((uint64_t)-1);
}

/* mmap: anonymous memory from the guest heap.
 * The engine reserves address space (PROT_NONE) and commits it from the start
 * with mprotect as it grows (ycVirtualMemoryPoolTS: 16 MiB, SaveManager: 64 MiB).
 * The Vita cannot reserve without memory: a reservation gets a smaller real
 * block, and a commit past it stops with the numbers to raise. */
typedef struct Reservation {
    uint32_t base, real, len, committed;
} Reservation;
static Reservation reservations[32];
static int nres;

static uint32_t reservation_real_size(uint64_t len)
{
#if defined(__vita__)
    if (len >= (64u << 20))
        return 16u << 20;
    return len < (4u << 20) ? (uint32_t)len : (4u << 20);
#else
    return (uint32_t)len;
#endif
}

static void imp_mmap(Ctx *c)
{
    uint64_t len = A1;
    int prot = (int)A2;
    if (!(A3 & 0x1000)) {           /* MAP_ANON (the fd may be a VM tag) */
        rt_log("mmap of a file is not supported\n");
        RET((uint64_t)-1);
        return;
    }
    log_big(c, "mmap", len);
    uint32_t real = prot == 0 ? reservation_real_size(len) : (uint32_t)len;
    uint32_t a = gmemalign(16384, real);
    if (g_trace_imports || !a)
        rt_log("[mmap] %llu bytes (prot %d, real %u) -> %#x\n", (unsigned long long)len, prot, real, a);
    if (!a) {
        RET((uint64_t)-1);
        return;
    }
    int gmalloc_was_fresh(void);
    if (!gmalloc_was_fresh())
        memset(G(a), 0, real);     /* fresh memory is already zero (and stays untouched) */
    if (prot == 0 && nres < 32) {
        reservations[nres].base = a;
        reservations[nres].real = real;
        reservations[nres].len = (uint32_t)len;
        reservations[nres].committed = 0;
        nres++;
    }
    RET(a);
}
static void imp_munmap(Ctx *c) { gfree((uint32_t)A0); RET(0); }
static void imp_mprotect(Ctx *c)
{
    uint32_t a = (uint32_t)A0;
    uint64_t end = A0 + A1;
    /* the reservation holding a: the closest base at or below it (on the Vita
     * reservations are smaller than their nominal length and may follow each other) */
    Reservation *r = NULL;
    for (int i = 0; i < nres; i++)
        if (reservations[i].base <= a && a < reservations[i].base + reservations[i].len &&
            (!r || reservations[i].base > r->base))
            r = &reservations[i];
    if (r) {
        if (end - r->base > r->committed) {
            r->committed = (uint32_t)(end - r->base);
            if (getenv("MINA_MEMSTAT"))
                rt_log("[mmap] reservation %#x: %.1f of %.1f MiB committed\n", r->base, r->committed / 1048576.0,
                       r->len / 1048576.0);
        }
        if (end > (uint64_t)r->base + r->real)
            rt_fatal("reservation of %u MiB at %#x committed to %.1f MiB, past its %u MiB "
                     "(raise reservation_real_size)", r->len >> 20, r->base, (end - r->base) / 1048576.0,
                     r->real >> 20);
    }
    RET(0);
}

static void qsort_impl(Ctx *c, uint32_t base, uint64_t n, uint64_t size, uint64_t cmp);

static void imp_qsort(Ctx *c) { qsort_impl(c, (uint32_t)A0, A1, A2, A3); }

/* Insertion sort for small arrays, heap sort otherwise: the comparator is a
 * guest function, so the order of calls only has to be valid, not libc's. */
static int qcmp(Ctx *c, uint64_t cmp, uint32_t a, uint32_t b)
{
    return (int)(int32_t)guest_call(c, cmp, 2, (uint64_t)a, (uint64_t)b);
}
static void qswap(uint32_t a, uint32_t b, uint64_t size)
{
    uint8_t *p = G(a), *q = G(b), t;
    for (uint64_t k = 0; k < size; k++) {
        t = p[k];
        p[k] = q[k];
        q[k] = t;
    }
}
static void sift(Ctx *c, uint32_t base, uint64_t size, uint64_t cmp, uint64_t root, uint64_t end)
{
    for (;;) {
        uint64_t child = 2 * root + 1;
        if (child > end) return;
        if (child + 1 <= end && qcmp(c, cmp, base + (uint32_t)(child * size), base + (uint32_t)((child + 1) * size)) < 0)
            child++;
        if (qcmp(c, cmp, base + (uint32_t)(root * size), base + (uint32_t)(child * size)) < 0) {
            qswap(base + (uint32_t)(root * size), base + (uint32_t)(child * size), size);
            root = child;
        } else {
            return;
        }
    }
}
static void qsort_impl(Ctx *c, uint32_t base, uint64_t n, uint64_t size, uint64_t cmp)
{
    if (n < 2) return;
    if (n <= 16) {
        for (uint64_t i = 1; i < n; i++)
            for (uint64_t j = i; j > 0 &&
                 qcmp(c, cmp, base + (uint32_t)((j - 1) * size), base + (uint32_t)(j * size)) > 0; j--)
                qswap(base + (uint32_t)((j - 1) * size), base + (uint32_t)(j * size), size);
        return;
    }
    for (int64_t start = (int64_t)(n - 2) / 2; start >= 0; start--)
        sift(c, base, size, cmp, (uint64_t)start, n - 1);
    for (uint64_t end = n - 1; end > 0; end--) {
        qswap(base, base + (uint32_t)(end * size), size);
        sift(c, base, size, cmp, 0, end - 1);
    }
}

/* C++ static-local guards: byte 0 = initialized, byte 1 = in progress. */
static pthread_mutex_t guard_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t guard_cv = PTHREAD_COND_INITIALIZER;
static void imp_cxa_guard_acquire(Ctx *c)
{
    uint8_t *g = G(A0);
    if (AT_LOAD(uint8_t, g)) {
        RET(0);
        return;
    }
    pthread_mutex_lock(&guard_lock);
    while (g[1] && !g[0])
        pthread_cond_wait(&guard_cv, &guard_lock);
    int need = !g[0];
    if (need) g[1] = 1;
    pthread_mutex_unlock(&guard_lock);
    RET(need);
}
static void imp_cxa_guard_release(Ctx *c)
{
    uint8_t *g = G(A0);
    pthread_mutex_lock(&guard_lock);
    AT_STORE(uint8_t, g, 1);
    g[1] = 0;
    pthread_cond_broadcast(&guard_cv);
    pthread_mutex_unlock(&guard_lock);
}
static void imp_cxa_guard_abort(Ctx *c)
{
    uint8_t *g = G(A0);
    pthread_mutex_lock(&guard_lock);
    g[1] = 0;
    pthread_cond_broadcast(&guard_cv);
    pthread_mutex_unlock(&guard_lock);
}
static void imp_terminate(Ctx *c) { (void)c; rt_fatal("std::terminate"); }
static void imp_pure_virtual(Ctx *c) { (void)c; rt_fatal("pure virtual call"); }
static void imp_unwind_resume(Ctx *c) { (void)c; rt_fatal("_Unwind_Resume (C++ exception)"); }
static void imp_operator_delete(Ctx *c) { gfree((uint32_t)A0); }

/* _Block_object_assign(dst, src, flags), called by block copy helpers.
 * __block variables (BYREF) move to the heap: {isa, forwarding, flags, size,
 * [keep, destroy helpers], value...}; the stack copy forwards to the heap one. */
uint64_t block_copy(Ctx *c, uint64_t block);
uint64_t objc_retain_obj(uint64_t a);
static void imp_block_object_assign(Ctx *c)
{
    uint64_t dst = A0, src = A1;
    uint32_t flags = (uint32_t)A2;
    if (!src) {
        rt_st64(G(dst), 0);
        return;
    }
    if (flags & 8) {                                    /* BLOCK_FIELD_IS_BYREF */
        uint64_t fwd = rt_ld64(G(src + 8));
        if (fwd != src) {                               /* already on the heap */
            rt_st64(G(dst), fwd);
            return;
        }
        uint32_t bflags = rt_ld32(G(src + 16)), size = rt_ld32(G(src + 20));
        uint32_t heap = gmalloc(size);
        memcpy(G(heap), G(src), size);
        rt_st64(G(heap + 8), heap);
        rt_st64(G(src + 8), heap);
        if (bflags & (1u << 25)) {
            uint64_t keep = rt_ld64(G(src + 24));
            guest_call(c, keep, 2, (uint64_t)heap, src);
        }
        rt_st64(G(dst), heap);
        return;
    }
    if ((flags & 7) == 7) {                             /* BLOCK_FIELD_IS_BLOCK */
        rt_st64(G(dst), block_copy(c, src));
        return;
    }
    if ((flags & 3) == 3 && !(flags & 16))              /* object, not weak */
        objc_retain_obj(src);
    rt_st64(G(dst), src);
}

/* _Block_object_dispose(obj, flags), called by block dispose helpers: undoes
 * _Block_object_assign. __block variables stay on the heap (never freed). */
void block_release(Ctx *c, uint64_t block);
void objc_release_obj(uint64_t a);
static void imp_block_object_dispose(Ctx *c)
{
    uint64_t obj = A0;
    uint32_t flags = (uint32_t)A1;
    if (!obj)
        return;
    if ((flags & 7) == 7)                               /* BLOCK_FIELD_IS_BLOCK */
        block_release(c, obj);
    else if (flags & 8)                                 /* BLOCK_FIELD_IS_BYREF */
        return;
    else if ((flags & 3) == 3 && !(flags & 16))         /* object, not weak */
        objc_release_obj(obj);
}

static void imp_setjmp(Ctx *c) { RET(0); }
static void imp_longjmp(Ctx *c) { (void)c; rt_fatal("guest longjmp is not supported"); }

const ImportImpl g_impl_libc[] = {
    { "_memcpy", imp_memcpy, 0, NULL },
    { "_memmove", imp_memmove, 0, NULL },
    { "_memset", imp_memset, 0, NULL },
    { "_bzero", imp_bzero, 0, NULL },
    { "_memcmp", imp_memcmp, 0, NULL },
    { "_memchr", imp_memchr, 0, NULL },
    { "_memset_pattern16", imp_memset_pattern16, 0, NULL },
    { "_strlen", imp_strlen, 0, NULL },
    { "_strcmp", imp_strcmp, 0, NULL },
    { "_strncmp", imp_strncmp, 0, NULL },
    { "_strcasecmp", imp_strcasecmp, 0, NULL },
    { "_strncasecmp", imp_strncasecmp, 0, NULL },
    { "_strchr", imp_strchr, 0, NULL },
    { "_strstr", imp_strstr, 0, NULL },
    { "_strcasestr", imp_strcasestr, 0, NULL },
    { "_strdup", imp_strdup, 0, NULL },
    { "_strlcpy", imp_strlcpy, 0, NULL },
    { "_strlcat", imp_strlcat, 0, NULL },
    { "_strerror", imp_strerror, 0, NULL },
    { "_strtol", imp_strtol, 0, NULL },
    { "_strtoul", imp_strtoul, 0, NULL },
    { "_strtoull", imp_strtoul, 0, NULL },
    { "_strtod", imp_strtod, 0, NULL },
    { "_atoi", imp_atoi, 0, NULL },
    { "_atof", imp_atof, 0, NULL },
    { "___tolower", imp_tolower, 0, NULL },
    { "_wcslen", imp_wcslen, 0, NULL },
    { "_wcscpy", imp_wcscpy, 0, NULL },
    { "_sinf", imp_sinf, 0, NULL },
    { "_cosf", imp_cosf, 0, NULL },
    { "_tanf", imp_tanf, 0, NULL },
    { "_asinf", imp_asinf, 0, NULL },
    { "_acosf", imp_acosf, 0, NULL },
    { "_atan2f", imp_atan2f, 0, NULL },
    { "_expf", imp_expf, 0, NULL },
    { "_exp2f", imp_exp2f, 0, NULL },
    { "_logf", imp_logf, 0, NULL },
    { "_log2f", imp_log2f, 0, NULL },
    { "_powf", imp_powf, 0, NULL },
    { "_fmodf", imp_fmodf, 0, NULL },
    { "___exp10f", imp_exp10f, 0, NULL },
    { "_sin", imp_sin, 0, NULL },
    { "_cos", imp_cos, 0, NULL },
    { "_atan", imp_atan, 0, NULL },
    { "_atan2", imp_atan2, 0, NULL },
    { "_exp", imp_exp, 0, NULL },
    { "_log", imp_log, 0, NULL },
    { "_pow", imp_pow, 0, NULL },
    { "_fmod", imp_fmod, 0, NULL },
    { "_ldexp", imp_ldexp, 0, NULL },
    { "___exp10", imp_exp10, 0, NULL },
    { "___sincosf_stret", imp_sincosf_stret, 0, NULL },
    { "___sincos_stret", imp_sincos_stret, 0, NULL },
    { "_malloc", imp_malloc, 0, NULL },
    { "_calloc", imp_calloc, 0, NULL },
    { "_realloc", imp_realloc, 0, NULL },
    { "_free", imp_free, 0, NULL },
    { "___error", imp_error, 0, NULL },
    { "_vsnprintf", imp_vsnprintf, 0, NULL },
    { "_snprintf", imp_snprintf, 0, NULL },
    { "_sscanf", imp_sscanf, 0, NULL },
    { "_vsscanf", imp_vsscanf, 0, NULL },
    { "_fopen", imp_fopen, 0, NULL },
    { "_fclose", imp_fclose, 0, NULL },
    { "_fread", imp_fread, 0, NULL },
    { "_fwrite", imp_fwrite, 0, NULL },
    { "_fseek", imp_fseek, 0, NULL },
    { "_fseeko", imp_fseek, 0, NULL },
    { "_ftell", imp_ftell, 0, NULL },
    { "_ftello", imp_ftell, 0, NULL },
    { "_fflush", imp_fflush, 0, NULL },
    { "_ferror", imp_ferror, 0, NULL },
    { "_rewind", imp_rewind, 0, NULL },
    { "_fileno", imp_fileno, 0, NULL },
    { "_fgets", imp_fgets, 0, NULL },
    { "_stat", imp_stat, 0, NULL },
    { "_fstat", imp_fstat, 0, NULL },
    { "_mkdir", imp_mkdir, 0, NULL },
    { "_getcwd", imp_getcwd, 0, NULL },
    { "_chdir", imp_chdir, 0, NULL },
    { "___stderrp", NULL, 16, stderr_init },
    { "___stdinp", NULL, 16, stdin_init },
    { "___stdoutp", NULL, 16, stdout_init },
    { "_mach_absolute_time", imp_mach_absolute_time, 0, NULL },
    { "_mach_timebase_info", imp_mach_timebase_info, 0, NULL },
    { "_clock_gettime_nsec_np", imp_clock_gettime_nsec_np, 0, NULL },
    { "_gettimeofday", imp_gettimeofday, 0, NULL },
    { "_getenv", imp_getenv, 0, NULL },
    { "_getpagesize", imp_getpagesize, 0, NULL },
    { "_abort", imp_abort, 0, NULL },
    { "__Exit", imp_exit, 0, NULL },
    { "___stack_chk_fail", imp_stack_chk_fail, 0, NULL },
    { "___stack_chk_guard", NULL, 16, stack_guard_init },
    { "___chkstk_darwin", imp_nop, 0, NULL },
    { "_atexit", imp_atexit, 0, NULL },
    { "___cxa_atexit", imp_cxa_atexit, 0, NULL },
    { "_sysctl", imp_sysctl, 0, NULL },
    { "_sysctlbyname", imp_sysctlbyname, 0, NULL },
    { "_mmap", imp_mmap, 0, NULL },
    { "_munmap", imp_munmap, 0, NULL },
    { "_mprotect", imp_mprotect, 0, NULL },
    { "_qsort", imp_qsort, 0, NULL },
    { "_signal", imp_ret0, 0, NULL },
    { "_sigaction", imp_ret0, 0, NULL },
    { "_dlopen", imp_ret0, 0, NULL },
    { "_dlsym", imp_ret0, 0, NULL },
    { "_dlclose", imp_ret0, 0, NULL },
    { "_dlerror", imp_ret0, 0, NULL },
    { "___cxa_guard_acquire", imp_cxa_guard_acquire, 0, NULL },
    { "___cxa_guard_release", imp_cxa_guard_release, 0, NULL },
    { "___cxa_guard_abort", imp_cxa_guard_abort, 0, NULL },
    { "___cxa_pure_virtual", imp_pure_virtual, 0, NULL },
    { "__ZSt9terminatev", imp_terminate, 0, NULL },
    { "___cxa_begin_catch", imp_terminate, 0, NULL },
    { "__Unwind_Resume", imp_unwind_resume, 0, NULL },
    { "__ZdlPv", imp_operator_delete, 0, NULL },
    { "__ZdlPvSt11align_val_t", imp_operator_delete, 0, NULL },
    { "__Block_object_assign", imp_block_object_assign, 0, NULL },
    { "__Block_object_dispose", imp_block_object_dispose, 0, NULL },
    { "_setjmp", imp_setjmp, 0, NULL },
    { "_longjmp", imp_longjmp, 0, NULL },
    { "___availability_version_check", imp_ret1, 0, NULL },
    { "__availability_version_check", imp_ret1, 0, NULL },
    { "_os_log_create", imp_ret1, 0, NULL },
    { "_os_signpost_enabled", imp_ret0, 0, NULL },
    { "_os_signpost_id_generate", imp_ret1, 0, NULL },
    { "__os_signpost_emit_with_name_impl", imp_nop, 0, NULL },
    { "_mach_msg", imp_retm1, 0, NULL },
    { NULL, NULL, 0, NULL },
};

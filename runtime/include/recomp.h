/*
 * Guest CPU context and helpers used by the recompiled arm64 code.
 *
 * Guest memory is a flat 32-bit space: guest address A lives at g_mem + A.
 * Guest registers stay 64-bit; every guest pointer is below 4 GB, so an
 * address is its low 32 bits. All accessors are unaligned-safe (memcpy) so
 * the same code runs on the Mac (arm64) and on the Vita (ARMv7).
 */
#ifndef MINA_RECOMP_H
#define MINA_RECOMP_H

#include <math.h>
#include <stdint.h>
#include <string.h>

/* ---- layout of the guest space ---------------------------------------- */

#define GUEST_NULL_GUARD   0x00010000u
#define GUEST_RT_BASE      0x00010000u   /* import thunks, data imports, TLS */
#define GUEST_RT_END       0x01000000u
#define GUEST_IMAGE_BASE   0x01000000u   /* Mach-O image (originally at 0x1_0000_0000) */
#define GUEST_ORIG_BASE    0x100000000ull

/* Import thunk i lives at GUEST_THUNK_BASE + 4 * i. */
#define GUEST_THUNK_BASE   0x00010000u
#define GUEST_THUNK_END    0x00020000u

/* ---- registers --------------------------------------------------------- */

typedef union V128 {
    uint8_t b[16];
    int8_t sb[16];
    uint16_t h[8];
    int16_t sh[8];
    uint32_t s[4];
    int32_t ss[4];
    uint64_t d[2];
    int64_t sd[2];
    float f[4];
    double df[2];
} V128;

typedef struct Ctx {
    uint64_t x[31];
    uint64_t sp;
    V128 v[32];
    uint32_t nzcv;
    uint32_t thread_id;
    uint32_t tls_base;       /* guest address of this thread's TLS block */
    uint32_t stack_lo, stack_hi;
    uint32_t cur_import;     /* index of the import being called (diagnostics) */
    void *thread;            /* runtime's thread record */
    uint32_t spin;           /* iterations of spin-wait loops (RT_SPIN) */
} Ctx;

/* The back edge of a small loop with atomics (a spin-wait) and the yield/wfe
 * hints: every 64 times the thread gives its core away. The Vita does not
 * time-slice threads of the same priority, so spinning threads would keep
 * the lock holder (and everything else) off its three cores forever. */
void rt_yield(void);
#define RT_SPIN(c) do { if ((++(c)->spin & 63) == 0) rt_yield(); } while (0)

typedef void (*GuestFn)(Ctx *c);

typedef struct GuestFunc {
    uint32_t addr;           /* relocated guest address */
    GuestFn fn;
} GuestFunc;

extern uint8_t *g_mem;

typedef struct GuestBind { uint32_t slot, import; int32_t addend; } GuestBind;
typedef struct GuestStub { uint32_t addr, import; } GuestStub;
typedef struct GuestSegment { uint32_t addr, size, fileoff, filesize; } GuestSegment;

/* Generated tables (tables.c). */
extern const GuestFunc g_guest_funcs[];
extern const uint32_t g_guest_func_count;
extern const char *const g_guest_func_names[];
extern const char *const g_import_names[];
extern const uint32_t g_import_count;
extern const GuestBind g_binds[];
extern const uint32_t g_bind_count;
extern const uint32_t g_rebases[];
extern const uint32_t g_rebase_count;
extern const GuestStub g_stubs[];
extern const uint32_t g_stub_count;
extern const GuestSegment g_segments[];
extern const uint32_t g_segment_count;
extern const uint32_t g_spare_lo, g_spare_hi;   /* image memory the game never touches */
extern const uint32_t g_tls_template, g_tls_size, g_tls_vars, g_tls_vars_size;
extern const uint32_t g_entry, g_mod_init, g_mod_init_count;
extern const uint32_t g_objc_classlist, g_objc_classlist_count, g_objc_methname, g_objc_methname_size;

/* Indirect call / jump to a guest address (function or import thunk). */
void rt_call(Ctx *c, uint64_t addr);
/* The same through a call site's cache: the (never changing) table entry of the
 * site's last target, one pointer, so threads may share it. Imports are not cached. */
void rt_call_ic(Ctx *c, uint64_t addr, const GuestFunc **ic);
#define RT_CALL_IC(c, t, ic)                                                         \
    do {                                                                             \
        const GuestFunc *e_ = __atomic_load_n(&(ic), __ATOMIC_RELAXED);              \
        if (e_ != NULL && e_->addr == (t))                                          \
            e_->fn(c);                                                               \
        else                                                                         \
            rt_call_ic((c), (t), &(ic));                                             \
    } while (0)
/* Import i called directly through its stub. */
extern GuestFn g_import_fns[];
#define IMPORT_CALL(c, i) ((c)->cur_import = (i), g_import_fns[i](c))

void rt_unimpl(Ctx *c, uint32_t pc, const char *what) __attribute__((noreturn));
void rt_trap(Ctx *c, uint32_t pc, uint32_t imm) __attribute__((noreturn));
void rt_bad_jump(Ctx *c, uint32_t pc, uint64_t target) __attribute__((noreturn));

/* ---- memory ------------------------------------------------------------ */

#ifndef GA
/* uintptr_t arithmetic: on the Vita guest addresses wrap modulo 2^32 around g_mem */
#define GA(a) ((uint8_t *)((uintptr_t)gm + (uint32_t)(a)))
#endif

static inline uint32_t rt_ld8(const uint8_t *p) { return *p; }
static inline uint32_t rt_ld16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t rt_ld32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t rt_ld64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline V128 rt_ld128(const uint8_t *p) { V128 v; memcpy(&v, p, 16); return v; }
static inline void rt_st8(uint8_t *p, uint32_t v) { *p = (uint8_t)v; }
static inline void rt_st16(uint8_t *p, uint32_t v) { uint16_t t = (uint16_t)v; memcpy(p, &t, 2); }
static inline void rt_st32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void rt_st64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static inline void rt_st128(uint8_t *p, V128 v) { memcpy(p, &v, 16); }

#define LD8(a) rt_ld8(GA(a))
#define LD16(a) rt_ld16(GA(a))
#define LD32(a) rt_ld32(GA(a))
#define LD64(a) rt_ld64(GA(a))
#define LD128(a) rt_ld128(GA(a))
#define ST8(a, v) rt_st8(GA(a), (v))
#define ST16(a, v) rt_st16(GA(a), (v))
#define ST32(a, v) rt_st32(GA(a), (v))
#define ST64(a, v) rt_st64(GA(a), (v))
#define ST128(a, v) rt_st128(GA(a), (v))

/* Host pointer of a guest address (for the runtime and the natives). */
static inline void *G(uint64_t a) { return (void *)((uintptr_t)g_mem + (uint32_t)a); }

/* ---- atomics (guest threads are host threads) -------------------------- */

#define AT_LOAD(T, p) __atomic_load_n((T *)(p), __ATOMIC_SEQ_CST)
#define AT_STORE(T, p, v) __atomic_store_n((T *)(p), (T)(v), __ATOMIC_SEQ_CST)

#define RT_ATOMIC_OPS(T, N)                                                            \
    static inline T rt_ldadd##N(uint8_t *p, T v) { return __atomic_fetch_add((T *)p, v, __ATOMIC_SEQ_CST); } \
    static inline T rt_ldclr##N(uint8_t *p, T v) { return __atomic_fetch_and((T *)p, (T)~v, __ATOMIC_SEQ_CST); } \
    static inline T rt_ldeor##N(uint8_t *p, T v) { return __atomic_fetch_xor((T *)p, v, __ATOMIC_SEQ_CST); } \
    static inline T rt_ldset##N(uint8_t *p, T v) { return __atomic_fetch_or((T *)p, v, __ATOMIC_SEQ_CST); } \
    static inline T rt_swp##N(uint8_t *p, T v) { return __atomic_exchange_n((T *)p, v, __ATOMIC_SEQ_CST); } \
    static inline T rt_cas##N(uint8_t *p, T expected, T desired) {                     \
        __atomic_compare_exchange_n((T *)p, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
        return expected;                                                                \
    }
RT_ATOMIC_OPS(uint8_t, 8)
RT_ATOMIC_OPS(uint16_t, 16)
RT_ATOMIC_OPS(uint32_t, 32)
RT_ATOMIC_OPS(uint64_t, 64)

#define RT_ATOMIC_MINMAX(T, N, NAME, CMP)                                              \
    static inline T rt_ld##NAME##N(uint8_t *p, T v) {                                  \
        T old = __atomic_load_n((T *)p, __ATOMIC_SEQ_CST);                             \
        for (;;) {                                                                      \
            T nv = (CMP) ? old : v;                                                     \
            if (__atomic_compare_exchange_n((T *)p, &old, nv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) \
                return old;                                                             \
        }                                                                               \
    }
RT_ATOMIC_MINMAX(uint32_t, 32, umax, old > v)
RT_ATOMIC_MINMAX(uint32_t, 32, umin, old < v)
RT_ATOMIC_MINMAX(uint64_t, 64, umax, old > v)
RT_ATOMIC_MINMAX(uint64_t, 64, umin, old < v)
RT_ATOMIC_MINMAX(uint32_t, 32, smax, (int32_t)old > (int32_t)v)
RT_ATOMIC_MINMAX(uint32_t, 32, smin, (int32_t)old < (int32_t)v)
RT_ATOMIC_MINMAX(uint64_t, 64, smax, (int64_t)old > (int64_t)v)
RT_ATOMIC_MINMAX(uint64_t, 64, smin, (int64_t)old < (int64_t)v)

/* ---- integer helpers --------------------------------------------------- */

static inline uint32_t ror32(uint32_t v, unsigned r) { r &= 31; return r ? (v >> r) | (v << (32 - r)) : v; }
static inline uint64_t ror64(uint64_t v, unsigned r) { r &= 63; return r ? (v >> r) | (v << (64 - r)) : v; }

static inline uint32_t udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static inline uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
static inline uint32_t sdiv32(uint32_t a, uint32_t b)
{
    if (!b) return 0;
    if (a == 0x80000000u && b == 0xFFFFFFFFu) return a;
    return (uint32_t)((int32_t)a / (int32_t)b);
}
static inline uint64_t sdiv64(uint64_t a, uint64_t b)
{
    if (!b) return 0;
    if (a == 0x8000000000000000ull && b == ~0ull) return a;
    return (uint64_t)((int64_t)a / (int64_t)b);
}

static inline uint64_t umulh64(uint64_t a, uint64_t b)
{
#if defined(__SIZEOF_INT128__)
    return (uint64_t)(((unsigned __int128)a * b) >> 64);
#else
    uint64_t al = (uint32_t)a, ah = a >> 32, bl = (uint32_t)b, bh = b >> 32;
    uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
#endif
}
static inline uint64_t smulh64(uint64_t a, uint64_t b)
{
    uint64_t h = umulh64(a, b);
    if ((int64_t)a < 0) h -= b;
    if ((int64_t)b < 0) h -= a;
    return h;
}

static inline uint32_t clz32(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32; }
static inline uint64_t clz64(uint64_t v) { return v ? (uint64_t)__builtin_clzll(v) : 64; }
static inline uint32_t cls32(uint32_t v) { return clz32(v ^ (uint32_t)((int32_t)v >> 1)) - 1; }
static inline uint64_t cls64(uint64_t v) { return clz64(v ^ (uint64_t)((int64_t)v >> 1)) - 1; }
static inline uint32_t rbit32(uint32_t v)
{
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    return __builtin_bswap32(v);
}
static inline uint64_t rbit64(uint64_t v) { return ((uint64_t)rbit32((uint32_t)v) << 32) | rbit32((uint32_t)(v >> 32)); }
static inline uint32_t rev16_32(uint32_t v) { return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); }
static inline uint64_t rev16_64(uint64_t v) { return ((v & 0x00FF00FF00FF00FFull) << 8) | ((v >> 8) & 0x00FF00FF00FF00FFull); }
static inline uint64_t rev32_64(uint64_t v) { return ((uint64_t)__builtin_bswap32((uint32_t)(v >> 32)) << 32) | __builtin_bswap32((uint32_t)v); }

/* ---- flags -------------------------------------------------------------
 * NZCV packed as bits 3..0 (N=8, Z=4, C=2, V=1). */

static inline uint32_t flags_sub64(uint64_t a, uint64_t b)
{
    uint64_t r = a - b;
    return ((uint32_t)(r >> 63) << 3) | ((uint32_t)(r == 0) << 2) | ((uint32_t)(a >= b) << 1)
         | (uint32_t)(((a ^ b) & (a ^ r)) >> 63);
}
static inline uint32_t flags_sub32(uint32_t a, uint32_t b)
{
    uint32_t r = a - b;
    return ((r >> 31) << 3) | ((uint32_t)(r == 0) << 2) | ((uint32_t)(a >= b) << 1)
         | (((a ^ b) & (a ^ r)) >> 31);
}
static inline uint32_t flags_add64(uint64_t a, uint64_t b)
{
    uint64_t r = a + b;
    return ((uint32_t)(r >> 63) << 3) | ((uint32_t)(r == 0) << 2) | ((uint32_t)(r < a) << 1)
         | (uint32_t)((~(a ^ b) & (a ^ r)) >> 63);
}
static inline uint32_t flags_add32(uint32_t a, uint32_t b)
{
    uint32_t r = a + b;
    return ((r >> 31) << 3) | ((uint32_t)(r == 0) << 2) | ((uint32_t)(r < a) << 1)
         | ((~(a ^ b) & (a ^ r)) >> 31);
}
static inline uint32_t flags_logic64(uint64_t r) { return ((uint32_t)(r >> 63) << 3) | ((uint32_t)(r == 0) << 2); }
static inline uint32_t flags_logic32(uint32_t r) { return ((r >> 31) << 3) | ((uint32_t)(r == 0) << 2); }
static inline uint32_t flags_fcmp(double a, double b)
{
    if (a == b) return 0x6;
    if (a < b) return 0x8;
    if (a > b) return 0x2;
    return 0x3;
}

static inline int cond_holds(uint32_t f, int cond)
{
    int n = (f >> 3) & 1, z = (f >> 2) & 1, c = (f >> 1) & 1, v = f & 1, r;
    switch (cond >> 1) {
    case 0: r = z; break;
    case 1: r = c; break;
    case 2: r = n; break;
    case 3: r = v; break;
    case 4: r = c && !z; break;
    case 5: r = n == v; break;
    case 6: r = n == v && !z; break;
    default: return 1;
    }
    return (cond & 1) ? !r : r;
}

/* ---- floating point ---------------------------------------------------- */

static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

static inline V128 vset_u32(uint32_t s) { V128 v; v.d[0] = s; v.d[1] = 0; return v; }
static inline V128 vset_u64(uint64_t d) { V128 v; v.d[0] = d; v.d[1] = 0; return v; }
static inline V128 vset_f32(float f) { return vset_u32(f2u(f)); }
static inline V128 vset_f64(double f) { return vset_u64(d2u(f)); }
static inline V128 vzero(void) { V128 v; v.d[0] = 0; v.d[1] = 0; return v; }
static inline V128 vlow(V128 a) { a.d[1] = 0; return a; }

/* Fused multiply-add: exact on the desktop; the Cortex-A9 has no FMA, so the
 * Vita build rounds twice (MINA_FAST_FMA). */
#if defined(MINA_FAST_FMA)
#define FMA_F32(a, b, c) ((a) * (b) + (c))
#define FMA_F64(a, b, c) ((a) * (b) + (c))
#else
#define FMA_F32(a, b, c) fmaf((a), (b), (c))
#define FMA_F64(a, b, c) fma((a), (b), (c))
#endif

/* fmax/fmin: NaN if either is NaN, -0 < +0. */
static inline float fmax_f32(float a, float b)
{
    if (a != a || b != b) return a + b;
    if (a == 0 && b == 0) return signbit(a) ? b : a;
    return a > b ? a : b;
}
static inline float fmin_f32(float a, float b)
{
    if (a != a || b != b) return a + b;
    if (a == 0 && b == 0) return signbit(a) ? a : b;
    return a < b ? a : b;
}
static inline double fmax_f64(double a, double b)
{
    if (a != a || b != b) return a + b;
    if (a == 0 && b == 0) return signbit(a) ? b : a;
    return a > b ? a : b;
}
static inline double fmin_f64(double a, double b)
{
    if (a != a || b != b) return a + b;
    if (a == 0 && b == 0) return signbit(a) ? a : b;
    return a < b ? a : b;
}
/* fmaxnm/fminnm: a quiet NaN loses against a number; a signaling NaN still
 * wins (quieted), as in FPProcessNaNs. */
static inline int snan32(float a) { uint32_t u; memcpy(&u, &a, 4); return (u & 0x7FC00000u) == 0x7F800000u && (u & 0x3FFFFFu); }
static inline int snan64(double a) { uint64_t u; memcpy(&u, &a, 8); return (u & 0x7FF8000000000000ull) == 0x7FF0000000000000ull && (u & 0x7FFFFFFFFFFFFull); }
static inline float fmaxnm_f32(float a, float b)
{
    if (snan32(a) || snan32(b)) return a + b;
    if (a != a) return b != b ? a + b : b;
    if (b != b) return a;
    return fmax_f32(a, b);
}
static inline float fminnm_f32(float a, float b)
{
    if (snan32(a) || snan32(b)) return a + b;
    if (a != a) return b != b ? a + b : b;
    if (b != b) return a;
    return fmin_f32(a, b);
}
static inline double fmaxnm_f64(double a, double b)
{
    if (snan64(a) || snan64(b)) return a + b;
    if (a != a) return b != b ? a + b : b;
    if (b != b) return a;
    return fmax_f64(a, b);
}
static inline double fminnm_f64(double a, double b)
{
    if (snan64(a) || snan64(b)) return a + b;
    if (a != a) return b != b ? a + b : b;
    if (b != b) return a;
    return fmin_f64(a, b);
}

static inline float fneg_f32(float a) { return u2f(f2u(a) ^ 0x80000000u); }
static inline double fneg_f64(double a) { return u2d(d2u(a) ^ 0x8000000000000000ull); }
static inline float fabs_f32(float a) { return u2f(f2u(a) & 0x7FFFFFFFu); }
static inline double fabs_f64(double a) { return u2d(d2u(a) & 0x7FFFFFFFFFFFFFFFull); }

/* Round to integral, ties to even (frintn): rint() in the default rounding mode. */
static inline double rt_round_even(double x) { return rint(x); }
static inline float rt_round_evenf(float x) { return rintf(x); }

/* Rounding modes of the conversions: n (ties even), p (+inf), m (-inf), z, a (ties away). */
static inline double rt_round_mode(double x, char mode)
{
    switch (mode) {
    case 'n': return rt_round_even(x);
    case 'p': return ceil(x);
    case 'm': return floor(x);
    case 'a': return round(x);
    default: return trunc(x);
    }
}

/* Saturating float -> integer conversions (NaN -> 0). */
static inline int64_t cvt_s64(double x, char mode)
{
    if (x != x) return 0;
    x = rt_round_mode(x, mode);
    if (x >= 9223372036854775808.0) return INT64_MAX;
    if (x < -9223372036854775808.0) return INT64_MIN;
    return (int64_t)x;
}
static inline uint64_t cvt_u64(double x, char mode)
{
    if (x != x) return 0;
    x = rt_round_mode(x, mode);
    if (x >= 18446744073709551616.0) return UINT64_MAX;
    if (x <= 0) return 0;
    return (uint64_t)x;
}
static inline int32_t cvt_s32(double x, char mode)
{
    if (x != x) return 0;
    x = rt_round_mode(x, mode);
    if (x >= 2147483648.0) return INT32_MAX;
    if (x < -2147483648.0) return INT32_MIN;
    return (int32_t)x;
}
static inline uint32_t cvt_u32(double x, char mode)
{
    if (x != x) return 0;
    x = rt_round_mode(x, mode);
    if (x >= 4294967296.0) return UINT32_MAX;
    if (x <= 0) return 0;
    return (uint32_t)x;
}

/* Half precision. */
static inline float h2f(uint16_t h)
{
    uint32_t s = (uint32_t)(h & 0x8000u) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    if (e == 0) {
        if (!m) return u2f(s);
        float f = (float)m / 16777216.0f;
        return s ? -f : f;
    }
    if (e == 31) return u2f(s | 0x7F800000u | (m << 13));
    return u2f(s | ((e + 112) << 23) | (m << 13));
}
static inline uint16_t f2h(float f)
{
    uint32_t u = f2u(f), s = (u >> 16) & 0x8000u;
    int32_t e = (int32_t)((u >> 23) & 0xFF) - 112;
    uint32_t m = u & 0x7FFFFFu;
    if (((u >> 23) & 0xFF) == 0xFF) return (uint16_t)(s | 0x7C00u | (m ? 0x200u | (m >> 13) : 0));
    if (e >= 31) return (uint16_t)(s | 0x7C00u);
    if (e <= 0) {
        if (e < -10) return (uint16_t)s;
        m |= 0x800000u;
        uint32_t shift = (uint32_t)(14 - e);
        uint32_t hm = m >> shift, rem = m & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (hm & 1))) hm++;
        return (uint16_t)(s | hm);
    }
    uint32_t hm = m >> 13, rem = m & 0x1FFF;
    uint32_t r = s | ((uint32_t)e << 10) | hm;
    if (rem > 0x1000 || (rem == 0x1000 && (hm & 1))) r++;
    return (uint16_t)r;
}

#include "simd.h"

#endif

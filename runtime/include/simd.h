/*
 * Advanced SIMD semantics for the recompiled code.
 *
 * Every helper takes the element size in bits (es) and the number of active
 * lanes (lanes: 1 for the scalar forms, 64/es or 128/es for the vector forms)
 * and returns the whole destination register with the inactive part zeroed,
 * as the architecture does. es and lanes are constants at every call site, so
 * after inlining the loops unroll and the switches fold.
 */
#ifndef MINA_SIMD_H
#define MINA_SIMD_H

static inline uint64_t es_mask(int es) { return es == 64 ? ~0ull : ((1ull << es) - 1); }

static inline uint64_t vget(V128 v, int es, int i)
{
    switch (es) {
    case 8: return v.b[i];
    case 16: return v.h[i];
    case 32: return v.s[i];
    default: return v.d[i];
    }
}
static inline int64_t vgets(V128 v, int es, int i)
{
    switch (es) {
    case 8: return v.sb[i];
    case 16: return v.sh[i];
    case 32: return v.ss[i];
    default: return v.sd[i];
    }
}
static inline void vput(V128 *v, int es, int i, uint64_t x)
{
    switch (es) {
    case 8: v->b[i] = (uint8_t)x; break;
    case 16: v->h[i] = (uint16_t)x; break;
    case 32: v->s[i] = (uint32_t)x; break;
    default: v->d[i] = x; break;
    }
}
static inline double vgetf(V128 v, int es, int i) { return es == 32 ? (double)v.f[i] : v.df[i]; }
static inline void vputf(V128 *v, int es, int i, double x)
{
    if (es == 32) v->f[i] = (float)x;
    else v->df[i] = x;
}
/* Single-precision lanes are computed in float, not double, to round like the guest. */
#define VF32(v, i) ((v).f[i])

/* ---- integer, element-wise -------------------------------------------- */

#define VBIN_INT(name, expr)                                                         \
    static inline V128 vop_##name(V128 n, V128 m, int es, int lanes)                 \
    {                                                                                \
        V128 r = vzero();                                                            \
        uint64_t mask = es_mask(es); (void)mask;                                     \
        for (int i = 0; i < lanes; i++) {                                            \
            uint64_t a = vget(n, es, i), b = vget(m, es, i);                         \
            int64_t sa = vgets(n, es, i), sb = vgets(m, es, i);                      \
            (void)a; (void)b; (void)sa; (void)sb;                                    \
            vput(&r, es, i, (uint64_t)(expr));                                       \
        }                                                                            \
        return r;                                                                    \
    }

VBIN_INT(add, a + b)
VBIN_INT(sub, a - b)
VBIN_INT(mul, a * b)
VBIN_INT(and, a & b)
VBIN_INT(orr, a | b)
VBIN_INT(eor, a ^ b)
VBIN_INT(bic, a & ~b)
VBIN_INT(orn, a | ~b)
VBIN_INT(cmeq, a == b ? ~0ull : 0)
VBIN_INT(cmtst, (a & b) ? ~0ull : 0)
VBIN_INT(cmhi, a > b ? ~0ull : 0)
VBIN_INT(cmhs, a >= b ? ~0ull : 0)
VBIN_INT(cmgt, sa > sb ? ~0ull : 0)
VBIN_INT(cmge, sa >= sb ? ~0ull : 0)
VBIN_INT(umax, a > b ? a : b)
VBIN_INT(umin, a < b ? a : b)
VBIN_INT(smax, sa > sb ? sa : sb)
VBIN_INT(smin, sa < sb ? sa : sb)
VBIN_INT(uhadd, (a + b) >> 1)
VBIN_INT(shadd, (uint64_t)((sa + sb) >> 1))
VBIN_INT(uabd, a > b ? a - b : b - a)
VBIN_INT(sabd, sa > sb ? sa - sb : sb - sa)

/* Shift by a register: signed shift count in the low byte of each element. */
static inline uint64_t rt_ushl_elem(uint64_t a, int64_t sh, int es)
{
    if (sh >= es || sh <= -es) return 0;
    return sh >= 0 ? (a << sh) & es_mask(es) : a >> -sh;
}
static inline uint64_t rt_sshl_elem(int64_t a, int64_t sh, int es)
{
    if (sh >= es) return 0;
    if (sh <= -es) return a < 0 ? ~0ull : 0;
    return sh >= 0 ? ((uint64_t)a << sh) : (uint64_t)(a >> -sh);
}
VBIN_INT(ushl, rt_ushl_elem(a, (int8_t)b, es))
VBIN_INT(sshl, rt_sshl_elem(sa, (int8_t)b, es))

/* Pairwise add: lanes from the concatenation n:m. */
static inline V128 vop_addp(V128 n, V128 m, int es, int lanes)
{
    V128 r = vzero();
    int h = lanes / 2;
    for (int i = 0; i < h; i++) {
        vput(&r, es, i, vget(n, es, 2 * i) + vget(n, es, 2 * i + 1));
        vput(&r, es, h + i, vget(m, es, 2 * i) + vget(m, es, 2 * i + 1));
    }
    return r;
}

static inline V128 vop_mla(V128 d, V128 n, V128 m, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++) vput(&r, es, i, vget(d, es, i) + vget(n, es, i) * vget(m, es, i));
    return r;
}
static inline V128 vop_mls(V128 d, V128 n, V128 m, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++) vput(&r, es, i, vget(d, es, i) - vget(n, es, i) * vget(m, es, i));
    return r;
}

/* Bitwise selects, on 64-bit chunks (lanes = 1 or 2). */
static inline V128 vop_bsl(V128 d, V128 n, V128 m, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++) r.d[i] = (d.d[i] & n.d[i]) | (~d.d[i] & m.d[i]);
    return r;
}
static inline V128 vop_bit(V128 d, V128 n, V128 m, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++) r.d[i] = (d.d[i] & ~m.d[i]) | (n.d[i] & m.d[i]);
    return r;
}
static inline V128 vop_bif(V128 d, V128 n, V128 m, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++) r.d[i] = (d.d[i] & m.d[i]) | (n.d[i] & ~m.d[i]);
    return r;
}

/* ---- integer, unary ---------------------------------------------------- */

#define VUN_INT(name, expr)                                                          \
    static inline V128 vop_##name(V128 n, int es, int lanes)                         \
    {                                                                                \
        V128 r = vzero();                                                            \
        for (int i = 0; i < lanes; i++) {                                            \
            uint64_t a = vget(n, es, i);                                             \
            int64_t sa = vgets(n, es, i);                                            \
            (void)a; (void)sa;                                                       \
            vput(&r, es, i, (uint64_t)(expr));                                       \
        }                                                                            \
        return r;                                                                    \
    }

VUN_INT(not, ~a)
VUN_INT(neg, (uint64_t)-sa)
VUN_INT(abs, (uint64_t)(sa < 0 ? -sa : sa))
VUN_INT(cmeqz, a == 0 ? ~0ull : 0)
VUN_INT(cmgtz, sa > 0 ? ~0ull : 0)
VUN_INT(cmgez, sa >= 0 ? ~0ull : 0)
VUN_INT(cmltz, sa < 0 ? ~0ull : 0)
VUN_INT(cmlez, sa <= 0 ? ~0ull : 0)
VUN_INT(cnt, (uint64_t)__builtin_popcountll(a))
VUN_INT(clz, (uint64_t)(a ? __builtin_clzll(a) - (64 - es) : es))
VUN_INT(rbit, (uint64_t)(rbit64(a) >> (64 - es)))

/* Reverse the order of es-bit elements within each container of cs bits. */
static inline V128 vop_rev(V128 n, int es, int lanes, int cs)
{
    V128 r = vzero();
    int per = cs / es;
    for (int i = 0; i < lanes; i++) {
        int base = i - i % per;
        vput(&r, es, i, vget(n, es, base + (per - 1 - i % per)));
    }
    return r;
}
#define vop_rev64(n, es, lanes) vop_rev((n), (es), (lanes), 64)
#define vop_rev32(n, es, lanes) vop_rev((n), (es), (lanes), 32)
#define vop_rev16(n, es, lanes) vop_rev((n), (es), (lanes), 16)

/* ---- across lanes (result in element 0) -------------------------------- */

static inline V128 vop_addv(V128 n, int es, int lanes)
{
    uint64_t s = 0;
    for (int i = 0; i < lanes; i++) s += vget(n, es, i);
    V128 r = vzero();
    vput(&r, es, 0, s);
    return r;
}
static inline V128 vop_uaddlv(V128 n, int es, int lanes)
{
    uint64_t s = 0;
    for (int i = 0; i < lanes; i++) s += vget(n, es, i);
    V128 r = vzero();
    vput(&r, es * 2, 0, s);
    return r;
}
static inline V128 vop_saddlv(V128 n, int es, int lanes)
{
    int64_t s = 0;
    for (int i = 0; i < lanes; i++) s += vgets(n, es, i);
    V128 r = vzero();
    vput(&r, es * 2, 0, (uint64_t)s);
    return r;
}
static inline V128 vop_umaxv(V128 n, int es, int lanes)
{
    uint64_t s = 0;
    for (int i = 0; i < lanes; i++) { uint64_t a = vget(n, es, i); if (a > s) s = a; }
    V128 r = vzero();
    vput(&r, es, 0, s);
    return r;
}
static inline V128 vop_uminv(V128 n, int es, int lanes)
{
    uint64_t s = es_mask(es);
    for (int i = 0; i < lanes; i++) { uint64_t a = vget(n, es, i); if (a < s) s = a; }
    V128 r = vzero();
    vput(&r, es, 0, s);
    return r;
}
static inline V128 vop_smaxv(V128 n, int es, int lanes)
{
    int64_t s = vgets(n, es, 0);
    for (int i = 1; i < lanes; i++) { int64_t a = vgets(n, es, i); if (a > s) s = a; }
    V128 r = vzero();
    vput(&r, es, 0, (uint64_t)s);
    return r;
}
static inline V128 vop_sminv(V128 n, int es, int lanes)
{
    int64_t s = vgets(n, es, 0);
    for (int i = 1; i < lanes; i++) { int64_t a = vgets(n, es, i); if (a < s) s = a; }
    V128 r = vzero();
    vput(&r, es, 0, (uint64_t)s);
    return r;
}

/* ---- shifts by immediate ----------------------------------------------- */

static inline V128 vop_shl(V128 n, int sh, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++) vput(&r, es, i, vget(n, es, i) << sh);
    return r;
}
static inline V128 vop_ushr(V128 n, int sh, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++) vput(&r, es, i, sh >= es ? 0 : vget(n, es, i) >> sh);
    return r;
}
static inline V128 vop_sshr(V128 n, int sh, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++) vput(&r, es, i, (uint64_t)(vgets(n, es, i) >> (sh >= es ? es - 1 : sh)));
    return r;
}
static inline V128 vop_usra(V128 d, V128 n, int sh, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++)
        vput(&r, es, i, vget(d, es, i) + (sh >= es ? 0 : vget(n, es, i) >> sh));
    return r;
}
static inline V128 vop_ssra(V128 d, V128 n, int sh, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++)
        vput(&r, es, i, vget(d, es, i) + (uint64_t)(vgets(n, es, i) >> (sh >= es ? es - 1 : sh)));
    return r;
}
static inline V128 vop_sli(V128 d, V128 n, int sh, int es, int lanes)
{
    V128 r = vzero();
    uint64_t keep = es_mask(es) >> (es - sh);
    if (sh == 0) keep = 0;
    for (int i = 0; i < lanes; i++)
        vput(&r, es, i, (vget(d, es, i) & keep) | (vget(n, es, i) << sh));
    return r;
}
static inline V128 vop_sri(V128 d, V128 n, int sh, int es, int lanes)
{
    V128 r = vzero();
    uint64_t keep = sh >= es ? es_mask(es) : ~(es_mask(es) >> sh) & es_mask(es);
    for (int i = 0; i < lanes; i++)
        vput(&r, es, i, (vget(d, es, i) & keep) | (sh >= es ? 0 : vget(n, es, i) >> sh));
    return r;
}

/* Widening: es is the source element size; part selects the upper half. */
static inline V128 vop_ushll(V128 n, int sh, int es, int part)
{
    V128 r = vzero();
    int lanes = 64 / es, base = part ? lanes : 0;
    for (int i = 0; i < lanes; i++) vput(&r, es * 2, i, vget(n, es, base + i) << sh);
    return r;
}
static inline V128 vop_sshll(V128 n, int sh, int es, int part)
{
    V128 r = vzero();
    int lanes = 64 / es, base = part ? lanes : 0;
    for (int i = 0; i < lanes; i++) vput(&r, es * 2, i, (uint64_t)(vgets(n, es, base + i) << sh));
    return r;
}
#define vop_shll(n, es, part) vop_ushll((n), (es), (es), (part))

/* Narrowing: es is the destination element size; part writes the upper half
 * and keeps the lower half of d. */
static inline V128 vop_shrn(V128 d, V128 n, int sh, int es, int part)
{
    V128 r = part ? d : vzero();
    if (part) r = vlow(r);
    int lanes = 64 / es, base = part ? lanes : 0;
    for (int i = 0; i < lanes; i++) vput(&r, es, base + i, vget(n, es * 2, i) >> sh);
    return r;
}
#define vop_xtn(d, n, es, part) vop_shrn((d), (n), 0, (es), (part))

/* ---- three different (long / wide) -------------------------------------- */

#define VLONG(name, expr)                                                            \
    static inline V128 vop_##name(V128 n, V128 m, int es, int part)                  \
    {                                                                                \
        V128 r = vzero();                                                            \
        int lanes = 64 / es, base = part ? lanes : 0;                                \
        for (int i = 0; i < lanes; i++) {                                            \
            uint64_t a = vget(n, es, base + i), b = vget(m, es, base + i);           \
            int64_t sa = vgets(n, es, base + i), sb = vgets(m, es, base + i);        \
            (void)a; (void)b; (void)sa; (void)sb;                                    \
            vput(&r, es * 2, i, (uint64_t)(expr));                                   \
        }                                                                            \
        return r;                                                                    \
    }
VLONG(umull, a * b)
VLONG(smull, sa * sb)
VLONG(uaddl, a + b)
VLONG(saddl, sa + sb)
VLONG(usubl, a - b)
VLONG(ssubl, sa - sb)

/* Wide: n has double-width elements, m narrow ones. */
#define VWIDE(name, expr)                                                            \
    static inline V128 vop_##name(V128 n, V128 m, int es, int part)                  \
    {                                                                                \
        V128 r = vzero();                                                            \
        int lanes = 64 / es, base = part ? lanes : 0;                                \
        for (int i = 0; i < lanes; i++) {                                            \
            uint64_t a = vget(n, es * 2, i), b = vget(m, es, base + i);              \
            int64_t sb = vgets(m, es, base + i);                                     \
            (void)b; (void)sb;                                                       \
            vput(&r, es * 2, i, (uint64_t)(expr));                                   \
        }                                                                            \
        return r;                                                                    \
    }
VWIDE(uaddw, a + b)
VWIDE(saddw, a + (uint64_t)sb)
VWIDE(usubw, a - b)
VWIDE(ssubw, a - (uint64_t)sb)

/* ---- permutes ----------------------------------------------------------- */

static inline V128 vop_zip1(V128 n, V128 m, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes / 2; i++) {
        vput(&r, es, 2 * i, vget(n, es, i));
        vput(&r, es, 2 * i + 1, vget(m, es, i));
    }
    return r;
}
static inline V128 vop_zip2(V128 n, V128 m, int es, int lanes)
{
    V128 r = vzero();
    int h = lanes / 2;
    for (int i = 0; i < h; i++) {
        vput(&r, es, 2 * i, vget(n, es, h + i));
        vput(&r, es, 2 * i + 1, vget(m, es, h + i));
    }
    return r;
}
static inline V128 vop_uzp1(V128 n, V128 m, int es, int lanes)
{
    V128 r = vzero();
    int h = lanes / 2;
    for (int i = 0; i < h; i++) {
        vput(&r, es, i, vget(n, es, 2 * i));
        vput(&r, es, h + i, vget(m, es, 2 * i));
    }
    return r;
}
static inline V128 vop_uzp2(V128 n, V128 m, int es, int lanes)
{
    V128 r = vzero();
    int h = lanes / 2;
    for (int i = 0; i < h; i++) {
        vput(&r, es, i, vget(n, es, 2 * i + 1));
        vput(&r, es, h + i, vget(m, es, 2 * i + 1));
    }
    return r;
}
static inline V128 vop_trn1(V128 n, V128 m, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes / 2; i++) {
        vput(&r, es, 2 * i, vget(n, es, 2 * i));
        vput(&r, es, 2 * i + 1, vget(m, es, 2 * i));
    }
    return r;
}
static inline V128 vop_trn2(V128 n, V128 m, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes / 2; i++) {
        vput(&r, es, 2 * i, vget(n, es, 2 * i + 1));
        vput(&r, es, 2 * i + 1, vget(m, es, 2 * i + 1));
    }
    return r;
}
static inline V128 vop_ext(V128 n, V128 m, int imm, int q)
{
    uint8_t cat[32];
    int bytes = q ? 16 : 8;
    memcpy(cat, n.b, bytes);
    memcpy(cat + bytes, m.b, bytes);
    V128 r = vzero();
    memcpy(r.b, cat + imm, bytes);
    return r;
}
/* tbl / tbx with nregs consecutive table registers. */
static inline V128 vop_tbl(const V128 *t, int nregs, V128 idx, V128 d, int q, int tbx)
{
    V128 r = tbx ? d : vzero();
    int bytes = q ? 16 : 8;
    for (int i = 0; i < bytes; i++) {
        unsigned k = idx.b[i];
        if (k < 16u * (unsigned)nregs) r.b[i] = t[k / 16].b[k % 16];
        else if (!tbx) r.b[i] = 0;
    }
    if (!q) r = vlow(r);
    return r;
}

/* ---- element moves ------------------------------------------------------ */

static inline V128 vop_dup_elem(V128 n, int idx, int es, int lanes)
{
    V128 r = vzero();
    uint64_t x = vget(n, es, idx);
    for (int i = 0; i < lanes; i++) vput(&r, es, i, x);
    return r;
}
static inline V128 vop_dup_gen(uint64_t x, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++) vput(&r, es, i, x);
    return r;
}
static inline V128 vop_ins_gen(V128 d, uint64_t x, int idx, int es)
{
    vput(&d, es, idx, x);
    return d;
}
static inline V128 vop_ins_elem(V128 d, V128 n, int dst, int src, int es)
{
    vput(&d, es, dst, vget(n, es, src));
    return d;
}
static inline V128 vop_movi(uint64_t imm, int q)
{
    V128 r;
    r.d[0] = imm;
    r.d[1] = q ? imm : 0;
    return r;
}
static inline V128 vop_orr_imm(V128 d, uint64_t imm, int q)
{
    d.d[0] |= imm;
    d.d[1] = q ? d.d[1] | imm : 0;
    return d;
}
static inline V128 vop_bic_imm(V128 d, uint64_t imm, int q)
{
    d.d[0] &= ~imm;
    d.d[1] = q ? d.d[1] & ~imm : 0;
    return d;
}

/* ---- floating point, element-wise ---------------------------------------- */

#define VBIN_FP(name, e32, e64)                                                      \
    static inline V128 vop_##name(V128 n, V128 m, int es, int lanes)                 \
    {                                                                                \
        V128 r = vzero();                                                            \
        for (int i = 0; i < lanes; i++) {                                            \
            if (es == 32) { float a = n.f[i], b = m.f[i]; r.f[i] = (e32); }           \
            else { double a = n.df[i], b = m.df[i]; r.df[i] = (e64); }                \
        }                                                                            \
        return r;                                                                    \
    }
VBIN_FP(fadd, a + b, a + b)
VBIN_FP(fsub, a - b, a - b)
VBIN_FP(fmul, a * b, a * b)
VBIN_FP(fdiv, a / b, a / b)
VBIN_FP(fmax, fmax_f32(a, b), fmax_f64(a, b))
VBIN_FP(fmin, fmin_f32(a, b), fmin_f64(a, b))
VBIN_FP(fmaxnm, fmaxnm_f32(a, b), fmaxnm_f64(a, b))
VBIN_FP(fminnm, fminnm_f32(a, b), fminnm_f64(a, b))
VBIN_FP(fabd, fabs_f32(a - b), fabs_f64(a - b))
VBIN_FP(fmulx, a * b, a * b)

#define VCMP_FP(name, cmp)                                                           \
    static inline V128 vop_##name(V128 n, V128 m, int es, int lanes)                 \
    {                                                                                \
        V128 r = vzero();                                                            \
        for (int i = 0; i < lanes; i++) {                                            \
            double a = vgetf(n, es, i), b = vgetf(m, es, i);                         \
            vput(&r, es, i, (cmp) ? ~0ull : 0);                                      \
        }                                                                            \
        return r;                                                                    \
    }
VCMP_FP(fcmeq, a == b)
VCMP_FP(fcmge, a >= b)
VCMP_FP(fcmgt, a > b)
VCMP_FP(facge, fabs(a) >= fabs(b))
VCMP_FP(facgt, fabs(a) > fabs(b))

#define VCMPZ_FP(name, cmp)                                                          \
    static inline V128 vop_##name(V128 n, int es, int lanes)                         \
    {                                                                                \
        V128 r = vzero();                                                            \
        for (int i = 0; i < lanes; i++) {                                            \
            double a = vgetf(n, es, i);                                              \
            vput(&r, es, i, (cmp) ? ~0ull : 0);                                      \
        }                                                                            \
        return r;                                                                    \
    }
VCMPZ_FP(fcmeqz, a == 0)
VCMPZ_FP(fcmgez, a >= 0)
VCMPZ_FP(fcmgtz, a > 0)
VCMPZ_FP(fcmlez, a <= 0)
VCMPZ_FP(fcmltz, a < 0)

#define VUN_FP(name, e32, e64)                                                       \
    static inline V128 vop_##name(V128 n, int es, int lanes)                         \
    {                                                                                \
        V128 r = vzero();                                                            \
        for (int i = 0; i < lanes; i++) {                                            \
            if (es == 32) { float a = n.f[i]; r.f[i] = (e32); }                       \
            else { double a = n.df[i]; r.df[i] = (e64); }                             \
        }                                                                            \
        return r;                                                                    \
    }
VUN_FP(fneg, fneg_f32(a), fneg_f64(a))
VUN_FP(fabs, fabs_f32(a), fabs_f64(a))
VUN_FP(fsqrt, sqrtf(a), sqrt(a))
VUN_FP(frintm, floorf(a), floor(a))
VUN_FP(frintp, ceilf(a), ceil(a))
VUN_FP(frintz, truncf(a), trunc(a))
VUN_FP(frinta, roundf(a), round(a))
VUN_FP(frintn, rintf(a), rint(a))
VUN_FP(frintx, rintf(a), rint(a))
VUN_FP(frinti, nearbyintf(a), nearbyint(a))
VUN_FP(frecpe, 1.0f / a, 1.0 / a)
VUN_FP(frsqrte, 1.0f / sqrtf(a), 1.0 / sqrt(a))

static inline V128 vop_frecps(V128 n, V128 m, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++) {
        if (es == 32) r.f[i] = 2.0f - n.f[i] * m.f[i];
        else r.df[i] = 2.0 - n.df[i] * m.df[i];
    }
    return r;
}
static inline V128 vop_frsqrts(V128 n, V128 m, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++) {
        if (es == 32) r.f[i] = (3.0f - n.f[i] * m.f[i]) / 2.0f;
        else r.df[i] = (3.0 - n.df[i] * m.df[i]) / 2.0;
    }
    return r;
}

static inline V128 vop_fmla(V128 d, V128 n, V128 m, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++) {
        if (es == 32) r.f[i] = FMA_F32(n.f[i], m.f[i], d.f[i]);
        else r.df[i] = FMA_F64(n.df[i], m.df[i], d.df[i]);
    }
    return r;
}
static inline V128 vop_fmls(V128 d, V128 n, V128 m, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++) {
        if (es == 32) r.f[i] = FMA_F32(-n.f[i], m.f[i], d.f[i]);
        else r.df[i] = FMA_F64(-n.df[i], m.df[i], d.df[i]);
    }
    return r;
}

/* By element: m's element idx against every lane of n. */
static inline V128 vop_fmul_elem(V128 n, V128 m, int idx, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++) {
        if (es == 32) r.f[i] = n.f[i] * m.f[idx];
        else r.df[i] = n.df[i] * m.df[idx];
    }
    return r;
}
#define vop_fmulx_elem vop_fmul_elem
static inline V128 vop_fmla_elem(V128 d, V128 n, V128 m, int idx, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++) {
        if (es == 32) r.f[i] = FMA_F32(n.f[i], m.f[idx], d.f[i]);
        else r.df[i] = FMA_F64(n.df[i], m.df[idx], d.df[i]);
    }
    return r;
}
static inline V128 vop_fmls_elem(V128 d, V128 n, V128 m, int idx, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++) {
        if (es == 32) r.f[i] = FMA_F32(-n.f[i], m.f[idx], d.f[i]);
        else r.df[i] = FMA_F64(-n.df[i], m.df[idx], d.df[i]);
    }
    return r;
}
static inline V128 vop_mul_elem(V128 n, V128 m, int idx, int es, int lanes)
{
    V128 r = vzero();
    uint64_t b = vget(m, es, idx);
    for (int i = 0; i < lanes; i++) vput(&r, es, i, vget(n, es, i) * b);
    return r;
}
static inline V128 vop_mla_elem(V128 d, V128 n, V128 m, int idx, int es, int lanes)
{
    V128 r = vzero();
    uint64_t b = vget(m, es, idx);
    for (int i = 0; i < lanes; i++) vput(&r, es, i, vget(d, es, i) + vget(n, es, i) * b);
    return r;
}

/* Pairwise FP (vector) and the scalar pairwise forms (two lanes of n). */
static inline V128 vop_faddp(V128 n, V128 m, int es, int lanes)
{
    V128 r = vzero();
    int h = lanes / 2;
    for (int i = 0; i < h; i++) {
        if (es == 32) { r.f[i] = n.f[2 * i] + n.f[2 * i + 1]; r.f[h + i] = m.f[2 * i] + m.f[2 * i + 1]; }
        else { r.df[i] = n.df[2 * i] + n.df[2 * i + 1]; r.df[h + i] = m.df[2 * i] + m.df[2 * i + 1]; }
    }
    return r;
}
static inline V128 vop_faddp_scalar(V128 n, int es)
{
    return es == 32 ? vset_f32(n.f[0] + n.f[1]) : vset_f64(n.df[0] + n.df[1]);
}
static inline V128 vop_fmaxp_scalar(V128 n, int es)
{
    return es == 32 ? vset_f32(fmax_f32(n.f[0], n.f[1])) : vset_f64(fmax_f64(n.df[0], n.df[1]));
}
static inline V128 vop_fminp_scalar(V128 n, int es)
{
    return es == 32 ? vset_f32(fmin_f32(n.f[0], n.f[1])) : vset_f64(fmin_f64(n.df[0], n.df[1]));
}
static inline V128 vop_fmaxnmp_scalar(V128 n, int es)
{
    return es == 32 ? vset_f32(fmaxnm_f32(n.f[0], n.f[1])) : vset_f64(fmaxnm_f64(n.df[0], n.df[1]));
}
static inline V128 vop_fminnmp_scalar(V128 n, int es)
{
    return es == 32 ? vset_f32(fminnm_f32(n.f[0], n.f[1])) : vset_f64(fminnm_f64(n.df[0], n.df[1]));
}
static inline V128 vop_addp_scalar(V128 n, int es) { (void)es; return vset_u64(n.d[0] + n.d[1]); }

/* Across lanes FP (4 x f32). */
static inline V128 vop_fmaxv(V128 n, int es, int lanes) { (void)es; (void)lanes; return vset_f32(fmax_f32(fmax_f32(n.f[0], n.f[1]), fmax_f32(n.f[2], n.f[3]))); }
static inline V128 vop_fminv(V128 n, int es, int lanes) { (void)es; (void)lanes; return vset_f32(fmin_f32(fmin_f32(n.f[0], n.f[1]), fmin_f32(n.f[2], n.f[3]))); }
static inline V128 vop_fmaxnmv(V128 n, int es, int lanes) { (void)es; (void)lanes; return vset_f32(fmaxnm_f32(fmaxnm_f32(n.f[0], n.f[1]), fmaxnm_f32(n.f[2], n.f[3]))); }
static inline V128 vop_fminnmv(V128 n, int es, int lanes) { (void)es; (void)lanes; return vset_f32(fminnm_f32(fminnm_f32(n.f[0], n.f[1]), fminnm_f32(n.f[2], n.f[3]))); }

/* ---- conversions ---------------------------------------------------------- */

static inline V128 vop_scvtf(V128 n, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++) {
        if (es == 32) r.f[i] = (float)n.ss[i];
        else r.df[i] = (double)n.sd[i];
    }
    return r;
}
static inline V128 vop_ucvtf(V128 n, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++) {
        if (es == 32) r.f[i] = (float)n.s[i];
        else r.df[i] = (double)n.d[i];
    }
    return r;
}
static inline V128 vop_scvtf_fx(V128 n, int fbits, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++) {
        if (es == 32) r.f[i] = (float)((double)n.ss[i] / (double)(1ull << fbits));
        else r.df[i] = (double)n.sd[i] / (double)(1ull << fbits);
    }
    return r;
}
static inline V128 vop_ucvtf_fx(V128 n, int fbits, int es, int lanes)
{
    V128 r = vzero();
    for (int i = 0; i < lanes; i++) {
        if (es == 32) r.f[i] = (float)((double)n.s[i] / (double)(1ull << fbits));
        else r.df[i] = (double)n.d[i] / (double)(1ull << fbits);
    }
    return r;
}

#define VCVT_INT(name, mode, sgn)                                                    \
    static inline V128 vop_##name(V128 n, int es, int lanes)                         \
    {                                                                                \
        V128 r = vzero();                                                            \
        for (int i = 0; i < lanes; i++) {                                            \
            double a = vgetf(n, es, i);                                              \
            uint64_t x = es == 32 ? (sgn ? (uint32_t)cvt_s32(a, mode) : cvt_u32(a, mode)) \
                                  : (sgn ? (uint64_t)cvt_s64(a, mode) : cvt_u64(a, mode)); \
            vput(&r, es, i, x);                                                      \
        }                                                                            \
        return r;                                                                    \
    }
VCVT_INT(fcvtzs, 'z', 1)
VCVT_INT(fcvtzu, 'z', 0)
VCVT_INT(fcvtns, 'n', 1)
VCVT_INT(fcvtnu, 'n', 0)
VCVT_INT(fcvtms, 'm', 1)
VCVT_INT(fcvtmu, 'm', 0)
VCVT_INT(fcvtps, 'p', 1)
VCVT_INT(fcvtpu, 'p', 0)
VCVT_INT(fcvtas, 'a', 1)
VCVT_INT(fcvtau, 'a', 0)

static inline V128 vop_fcvtzs_fx(V128 n, int fbits, int es, int lanes)
{
    V128 r = vzero();
    double scale = (double)(1ull << fbits);
    for (int i = 0; i < lanes; i++) {
        double a = vgetf(n, es, i) * scale;
        vput(&r, es, i, es == 32 ? (uint32_t)cvt_s32(a, 'z') : (uint64_t)cvt_s64(a, 'z'));
    }
    return r;
}
static inline V128 vop_fcvtzu_fx(V128 n, int fbits, int es, int lanes)
{
    V128 r = vzero();
    double scale = (double)(1ull << fbits);
    for (int i = 0; i < lanes; i++) {
        double a = vgetf(n, es, i) * scale;
        vput(&r, es, i, es == 32 ? cvt_u32(a, 'z') : cvt_u64(a, 'z'));
    }
    return r;
}

/* fcvtl: es is the narrow (source) size; fcvtn: es is the narrow (dest) size. */
static inline V128 vop_fcvtl(V128 n, int es, int part)
{
    V128 r = vzero();
    if (es == 16) {
        for (int i = 0; i < 4; i++) r.f[i] = h2f(n.h[(part ? 4 : 0) + i]);
    } else {
        for (int i = 0; i < 2; i++) r.df[i] = (double)n.f[(part ? 2 : 0) + i];
    }
    return r;
}
static inline V128 vop_fcvtn(V128 d, V128 n, int es, int part)
{
    V128 r = part ? vlow(d) : vzero();
    if (es == 16) {
        for (int i = 0; i < 4; i++) r.h[(part ? 4 : 0) + i] = f2h(n.f[i]);
    } else {
        for (int i = 0; i < 2; i++) r.f[(part ? 2 : 0) + i] = (float)n.df[i];
    }
    return r;
}

#endif

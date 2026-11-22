/*
 * memcpy, memmove and memset for the Vita build. The vitasdk newlib's are a
 * branch to SceLibKernel's sceClib* (an import: in Vita3K each call is an HLE
 * call, ~10 us, and the renderer copies a uniform block per draw), so these
 * replace them: the linker takes ours and leaves newlib's members out.
 * NEON, unaligned loads and stores (Cortex-A9, normal memory).
 */
#ifdef __vita__
#include <arm_neon.h>
#include <stddef.h>
#include <stdint.h>

/* no loop here is turned back into a call to memcpy/memset */
#define FASTMEM __attribute__((optimize("no-tree-loop-distribute-patterns"), used))

typedef uint32_t u32u __attribute__((aligned(1), may_alias));
typedef uint16_t u16u __attribute__((aligned(1), may_alias));

static inline __attribute__((always_inline)) void copy_fwd(uint8_t *d, const uint8_t *s, size_t n)
{
    while (n >= 64) {
        uint8x16_t a = vld1q_u8(s), b = vld1q_u8(s + 16), c = vld1q_u8(s + 32), e = vld1q_u8(s + 48);
        vst1q_u8(d, a); vst1q_u8(d + 16, b); vst1q_u8(d + 32, c); vst1q_u8(d + 48, e);
        s += 64; d += 64; n -= 64;
    }
    while (n >= 16) {
        vst1q_u8(d, vld1q_u8(s));
        s += 16; d += 16; n -= 16;
    }
    if (n >= 8) {
        vst1_u8(d, vld1_u8(s));
        s += 8; d += 8; n -= 8;
    }
    if (n >= 4) {
        *(u32u *)d = *(const u32u *)s;
        s += 4; d += 4; n -= 4;
    }
    if (n >= 2) {
        *(u16u *)d = *(const u16u *)s;
        s += 2; d += 2; n -= 2;
    }
    if (n)
        *d = *s;
}

/* memcpy is memmove: the game's code comes from macOS, where memcpy copies
 * overlapping ranges right (and the game relies on it) */
FASTMEM void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    /* forward is safe below the source (each block is loaded before stored) */
    if (d <= s || d >= s + n) {
        copy_fwd(d, s, n);
        return dst;
    }
    d += n; s += n;
    while (n >= 16) {
        s -= 16; d -= 16; n -= 16;
        vst1q_u8(d, vld1q_u8(s));
    }
    while (n) {
        *--d = *--s;
        n--;
    }
    return dst;
}

FASTMEM void *memcpy(void *dst, const void *src, size_t n)
{
    const uint8_t *d = (const uint8_t *)dst, *s = (const uint8_t *)src;
    if (d <= s || d >= s + n) {
        copy_fwd((uint8_t *)dst, s, n);
        return dst;
    }
    return memmove(dst, src, n);
}

FASTMEM int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *p = (const uint8_t *)a, *q = (const uint8_t *)b;
    while (n >= 4 && *(const u32u *)p == *(const u32u *)q) {
        p += 4; q += 4; n -= 4;
    }
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

FASTMEM void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8x16_t v = vdupq_n_u8((uint8_t)c);
    while (n >= 64) {
        vst1q_u8(d, v); vst1q_u8(d + 16, v); vst1q_u8(d + 32, v); vst1q_u8(d + 48, v);
        d += 64; n -= 64;
    }
    while (n >= 16) {
        vst1q_u8(d, v);
        d += 16; n -= 16;
    }
    const uint32_t w = (uint8_t)c * 0x01010101u;
    while (n >= 4) {
        *(u32u *)d = w;
        d += 4; n -= 4;
    }
    while (n) {
        *d++ = (uint8_t)c;
        n--;
    }
    return dst;
}
#endif

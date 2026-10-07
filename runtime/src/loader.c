/* Loads the arm64 slice of the game's executable into guest memory. */
#include "runtime.h"

#include <string.h>

/* the guest range the loader leaves out (memory.c: the Vita has no memory there) */
uint32_t image_skip_lo(void);
uint32_t image_skip_hi(void);

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

void loader_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        rt_fatal("cannot open %s", path);
    uint8_t hdr[4096];
    if (fread(hdr, 1, sizeof hdr, f) < 8)
        rt_fatal("%s: short file", path);

    long slice = 0;
    if (be32(hdr) == 0xCAFEBABEu) {
        uint32_t n = be32(hdr + 4);
        slice = -1;
        for (uint32_t k = 0; k < n && 8 + 20 * (k + 1) <= sizeof hdr; k++) {
            const uint8_t *e = hdr + 8 + 20 * k;
            if (be32(e) == 0x0100000Cu)          /* CPU_TYPE_ARM64 */
                slice = (long)be32(e + 8);
        }
        if (slice < 0)
            rt_fatal("%s: no arm64 slice", path);
    }

    /* each segment in pieces around [skip_lo, skip_hi): the arm64 code and
     * what precedes it (the Mach-O header), which have no memory on the Vita */
    uint32_t skip_lo = image_skip_lo(), skip_hi = image_skip_hi();
    for (uint32_t k = 0; k < g_segment_count; k++) {
        const GuestSegment *s = &g_segments[k];
        uint32_t lo = s->addr, hi = s->addr + s->size;
        for (int part = 0; part < 2; part++) {
            uint32_t a = part ? (skip_hi > lo ? skip_hi : lo) : lo;
            uint32_t b = part ? hi : (skip_lo < hi ? skip_lo : hi);
            if (skip_lo == skip_hi && part)
                break;
            if (skip_lo == skip_hi)
                b = hi;
            if (a >= b)
                continue;
            memset(G(a), 0, b - a);
            uint32_t fend = s->addr + s->filesize;           /* file bytes cover [addr, fend) */
            uint32_t ra = a, rb = b < fend ? b : fend;
            if (ra >= rb)
                continue;
            if (fseek(f, slice + (long)(s->fileoff + (ra - s->addr)), SEEK_SET) != 0 ||
                fread(G(ra), 1, rb - ra, f) != rb - ra)
                rt_fatal("%s: cannot read segment %u", path, k);
        }
    }
    fclose(f);

    /* Rebase: every pointer in the image moves from 0x1_0000_0000 to the guest base. */
    for (uint32_t k = 0; k < g_rebase_count; k++) {
        uint64_t v = rt_ld64(G(g_rebases[k]));
        if (v)
            rt_st64(G(g_rebases[k]), v - GUEST_ORIG_BASE + GUEST_IMAGE_BASE);
    }
    /* Bind: imported functions get their thunk, imported data its block. */
    for (uint32_t k = 0; k < g_bind_count; k++) {
        const GuestBind *b = &g_binds[k];
        rt_st64(G(b->slot), (uint64_t)import_addr(b->import) + (int64_t)b->addend);
    }
}

/* Loads the arm64 slice of the game's executable into guest memory. */
#include "runtime.h"

#include <string.h>

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

    for (uint32_t k = 0; k < g_segment_count; k++) {
        const GuestSegment *s = &g_segments[k];
        memset(G(s->addr), 0, s->size);
        if (!s->filesize)
            continue;
        if (fseek(f, slice + (long)s->fileoff, SEEK_SET) != 0 ||
            fread(G(s->addr), 1, s->filesize, f) != s->filesize)
            rt_fatal("%s: cannot read segment %u", path, k);
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

/* Audio natives: the music banks stay on the card (see "lazy files" in libc.c). */
#include "runtime.h"

#include <stdlib.h>
#include <string.h>

int lazy_contains(uint32_t addr);
int lazy_read(uint32_t addr, void *dst, uint32_t n);
void lazy_fixup(void);

/* size_t ycAudio::Decoder::ycOggRead(void *dst, size_t size, size_t n, void *src)
 * src: {u64 pos, item*}; item: +8 u32 length, +16 data pointer */
void nat__ZN7ycAudio7Decoder9ycOggReadEPvmmS1_(Ctx *c)
{
    uint64_t dst = A0, want = A1 * A2, src = A3;
    uint64_t pos = rt_ld64(G(src));
    uint64_t item = rt_ld64(G(src + 8));
    uint64_t len = rt_ld32(G(item + 8));
    uint64_t data = rt_ld64(G(item + 16));
    uint64_t n = pos >= len ? 0 : (pos + want > len ? len - pos : want);
    if (n) {
        if (!lazy_read((uint32_t)(data + pos), G(dst), (uint32_t)n))
            memcpy(G(dst), G(data + pos), (size_t)n);
        rt_st64(G(src), pos + n);
    }
    RET(n);
}

/* int ov_open_callbacks(void *ds, OggVorbis_File *vf, const char *initial, long ibytes,
 *                       ov_callbacks cb)
 * The decoder also opens sounds from memory (initial = the whole item, no
 * callbacks): libvorbis copies the initial bytes during the call, so an item
 * on the card is read into a temporary buffer freed when the call returns. */
void f_9b3a8_orig(Ctx *c);

void nat_ov_open_callbacks(Ctx *c)
{
    uint64_t initial = A2, ibytes = A3;
    if (initial && ibytes && lazy_contains((uint32_t)initial)) {
        uint32_t tmp = gmalloc(ibytes);
        lazy_read((uint32_t)initial, G(tmp), (uint32_t)ibytes);
        c->x[2] = tmp;
        f_9b3a8_orig(c);
        gfree(tmp);
        return;
    }
    f_9b3a8_orig(c);
}

/* The bank's events are read (and short sounds decoded) right after it loads:
 * its Ogg pointers move to the card window first. */
void f_cdeb4_orig(Ctx *c);
void f_ce080_orig(Ctx *c);
void nat__ZN7ycAudio11FileManager10LoadEventsEPNS_10BankHandleEP15ycAudioItemBaseb(Ctx *c)
{
    lazy_fixup();
    f_cdeb4_orig(c);
}
void nat__ZN7ycAudio11FileManager11CacheEventsEPKNS_10BankHandleEP15ycAudioItemBaseb(Ctx *c)
{
    lazy_fixup();
    f_ce080_orig(c);
}

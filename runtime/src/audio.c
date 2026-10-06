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
    static int dbg = -1, n;
    if (dbg < 0)
        dbg = getenv("MINA_AUDIODBG") != NULL;
    if (dbg)
        rt_log("[audio] ov_open #%d %llu bytes at %#llx\n", ++n, (unsigned long long)ibytes,
               (unsigned long long)initial);
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
/* After the events, InitBankQueue asks the decoder to decode every sound of
 * the bank whose item has the "predecode" byte (+0x24) to float PCM, kept
 * for the bank's lifetime: 212 MiB for the 26 MiB of Ogg of the effects.
 * With the byte clear the decoder leaves the slot empty and plays the sound
 * straight from its Ogg (only what is playing is decoded). Items: bank+0x148
 * -> {u64 count << 34, item **}; item +8 length, +0x10 data, +0x18 channels,
 * +0x1c frames, +0x24 predecode. MINA_SFX_PREDECODE=1 keeps the original. */
static void bank_no_predecode(uint64_t bank)
{
    static int keep = -1;
    if (keep < 0)
        keep = getenv("MINA_SFX_PREDECODE") != NULL;
    uint64_t table = rt_ld64(G(bank + 0x148));
    if (keep || !table)
        return;
    uint32_t n = (uint32_t)((rt_ld64(G(table)) >> 34) & 0xffff);
    uint64_t items = rt_ld64(G(table + 8));
    uint32_t cleared = 0;
    uint64_t saved = 0;
    for (uint32_t i = 0; i < n && items; i++) {
        uint64_t it = rt_ld64(G(items + 8ull * i));
        if (!it || it >> 32)
            continue;
        if (rt_ld8(G(it + 0x24))) {
            saved += (uint64_t)rt_ld32(G(it + 0x18)) * rt_ld32(G(it + 0x1c)) * 4;
            rt_st8(G(it + 0x24), 0);
            cleared++;
        }
    }
    if (cleared)
        rt_log("[audio] %u sounds play from Ogg instead of %.1f MiB of decoded PCM\n", cleared,
               saved / 1048576.0);
}

void nat__ZN7ycAudio11FileManager10LoadEventsEPNS_10BankHandleEP15ycAudioItemBaseb(Ctx *c)
{
    uint64_t bank = A0;
    lazy_fixup();
    f_cdeb4_orig(c);
    bank_no_predecode(bank);
}
void nat__ZN7ycAudio11FileManager11CacheEventsEPKNS_10BankHandleEP15ycAudioItemBaseb(Ctx *c)
{
    lazy_fixup();
    f_ce080_orig(c);
}

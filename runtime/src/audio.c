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
    static int st = -1;
    if (st < 0)
        st = getenv("MINA_AUDIOSTAT") != NULL;
    uint64_t a_now(void);
    const uint64_t t0 = st ? a_now() : 0;
    const int lz = initial && ibytes && lazy_contains((uint32_t)initial);
    uint64_t t1 = t0;
    if (lz) {
        uint32_t tmp = gmalloc(ibytes);
        lazy_read((uint32_t)initial, G(tmp), (uint32_t)ibytes);
        if (st)
            t1 = a_now();
        c->x[2] = tmp;
        f_9b3a8_orig(c);
        gfree(tmp);
    } else
        f_9b3a8_orig(c);
    if (st && a_now() - t0 > 10000)
        rt_log("[audiostat] ov_open of %llu bytes%s: %.1f ms (reading %.1f)\n", (unsigned long long)ibytes,
               lz ? " from the card" : "", (a_now() - t0) / 1000.0, (t1 - t0) / 1000.0);
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
    uint32_t cleared = 0, kept = 0;
    uint64_t saved = 0, kept_b = 0;
    /* Every effect played from its Ogg kept the sound core's decoders busy (Vorbis in real
     * time, 35-65% of the core in a busy room) and an effect that starts waits for its
     * decoder (opening the Ogg alone: 15-40 ms): the effects came late. Those up to
     * MINA_SFX_SHORT_KIB of PCM (256: ~0.7 s of stereo) stay decoded, ~30 MiB. */
    static uint64_t short_max = ~0ull;
    if (short_max == ~0ull) {
        const char *v = getenv("MINA_SFX_SHORT_KIB");
        short_max = (uint64_t)(v ? atoi(v) : 256) << 10;
    }
    static int hist = -1;
    if (hist < 0)
        hist = getenv("MINA_SFX_HIST") != NULL;
    uint32_t hn[16] = { 0 };
    uint64_t hb[16] = { 0 };
    for (uint32_t i = 0; i < n && items; i++) {
        uint64_t it = rt_ld64(G(items + 8ull * i));
        if (!it || it >> 32)
            continue;
        if (rt_ld8(G(it + 0x24))) {
            const uint64_t pcm = (uint64_t)rt_ld32(G(it + 0x18)) * rt_ld32(G(it + 0x1c)) * 4;
            if (pcm <= short_max) {         /* short effects stay decoded: they start at once */
                kept++;
                kept_b += pcm;
                continue;
            }
            int k = 0;
            while (k < 15 && (16384ull << k) < pcm)
                k++;
            hn[k]++;
            hb[k] += pcm;
            saved += pcm;
            rt_st8(G(it + 0x24), 0);
            cleared++;
        }
    }
    if (hist)
        for (int k = 0; k < 16; k++)
            if (hn[k])
                rt_log("[audio]   up to %llu KiB: %u sounds, %.1f MiB\n", (16384ull << k) >> 10, hn[k], hb[k] / 1048576.0);
    if (cleared)
        rt_log("[audio] %u sounds play from Ogg instead of %.1f MiB of decoded PCM; %u short ones decoded (%.1f MiB)\n",
               cleared, saved / 1048576.0, kept, kept_b / 1048576.0);
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

/* MINA_AUDIOSTAT: the decoder threads' work (ycAudio::Decoder::DecodeCtx: one call fills
 * one sound's ring), per 2 s: calls, time, the longest */
void f_cb964_orig(Ctx *c);
#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
uint64_t a_now(void) { return sceKernelGetProcessTimeWide(); }
#else
#include <time.h>
uint64_t a_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
#endif
void nat__ZN7ycAudio7Decoder9DecodeCtxEPNS0_3CtxE(Ctx *c)
{
    static int on = -1;
    if (on < 0)
        on = getenv("MINA_AUDIOSTAT") != NULL;
    if (!on) {
        f_cb964_orig(c);
        return;
    }
    static uint64_t last, tot, mx;
    static uint32_t calls;
    const uint64_t ctx = A0;
    const uint32_t hdr = 0x1e89e68u + rt_ld32(G(ctx + 0x10)) * 0x40020u + 0xc00u;
    const uint32_t b0 = rt_ld32(G(ctx + 0x14)), s0 = rt_ld32(G(hdr)), w0 = rt_ld32(G(hdr + 4)), r0 = rt_ld32(G(hdr + 0xc));
    const uint64_t t0 = a_now();
    f_cb964_orig(c);
    const uint64_t dt = a_now() - t0;
    if (dt > 30000) {
        const uint64_t in = rt_ld64(G(ctx + 8)), fc = in ? rt_ld64(G(in + 0x110)) : 0;
        const uint64_t fr = fc ? rt_ld64(G(fc + 0x18)) : 0;
        rt_log("[audiostat] %.1f ms: slot %u ctx+14 %#x ring size %u write %u read %u -> write %u; inst %#llx "
               "filter %#llx kind %u ring %#llx size %u write %u read %u\n",
               dt / 1000.0, rt_ld32(G(ctx + 0x10)), b0, s0, w0, r0, rt_ld32(G(hdr + 4)), (unsigned long long)in,
               (unsigned long long)fc, fc ? rt_ld32(G(fc + 0xc)) : 0, (unsigned long long)fr,
               fr ? rt_ld32(G(fr)) : 0, fr ? rt_ld32(G(fr + 4)) : 0, fr ? rt_ld32(G(fr + 0xc)) : 0);
    }
    __atomic_add_fetch(&tot, dt, __ATOMIC_RELAXED);
    __atomic_add_fetch(&calls, 1, __ATOMIC_RELAXED);
    if (dt > mx)
        mx = dt;
    if (t0 - last > 2000000) {
        last = t0;
        rt_log("[audiostat] decoder: %u calls in 2 s, %.1f ms, longest %.1f ms\n", calls, tot / 1000.0, mx / 1000.0);
        calls = 0;
        tot = mx = 0;
    }
}

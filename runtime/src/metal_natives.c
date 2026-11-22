/*
 * The game's renderer functions that talk to Metal the most, in C.
 *
 * ycRenderDevice::DrawCall (the render thread, every draw): it keeps one
 * pipeline descriptor per shader and compares some fifty of its properties
 * with the draw's state each time, a message apiece (and an ARC retain and
 * release for every sub-descriptor it walks through); it makes a new depth
 * stencil descriptor for every draw. Here the descriptors are read and
 * written in place (md_*: the same properties, values and retains as the
 * messages), and only the encoder and the device get messages. The order of
 * everything the game can see (descriptor contents, its caches, the encoder
 * calls) is the original's.
 */
#include "runtime.h"
#include "objc_rt.h"
#include "metal_gl.h"
#include <string.h>

uint64_t md_get(uint64_t obj, const char *key);
void md_set(uint64_t obj, const char *key, uint64_t v);
uint64_t md_child(uint64_t obj, const char *key);
uint64_t md_elem(uint64_t arr, uint64_t i);
uint64_t md_hash(uint64_t obj);
int md_refl_masks(uint64_t refl, uint64_t *vbuf, uint64_t *vtex, uint64_t *fbuf, uint64_t *ftex);
void objc_msgSend_site(Ctx *c, uint32_t sel, const void **site);

#define GM_BEGIN uint8_t *const gm_ = g_mem
#define GP(a) ((void *)(gm_ + (uint32_t)(a)))
#define L8(a) (*(const uint8_t *)GP(a))
#define L32(a) rt_ld32(GP(a))
#define L64(a) rt_ld64(GP(a))
#define S8(a, v) (*(uint8_t *)GP(a) = (uint8_t)(v))
#define S32(a, v) rt_st32(GP(a), (uint32_t)(v))
#define S64(a, v) rt_st64(GP(a), (uint64_t)(v))
#define K(s) PB_K(s)

/* the renderer's globals (ycRenderDevice.cpp, anonymous namespace) */
enum {
    G_CUR_PASS = 0x603a580,          /* s_curRenderPass (encoder) */
    G_PASS_DESC = 0x603a588,         /* s_curRenderPassDesc */
    G_UNIFORM_BUFS = 0x603a590,      /* s_uniformBufs[] */
    G_SAMPLER_KEYS = 0x603a990,      /* s_samplers: ycOpenHash<uint, void*> */
    G_SAMPLER_CAP = 0x603a998,
    G_SAMPLER_PROBES = 0x603a9a0,
    G_SAMPLER_COUNT = 0x603a9a8,
    G_SAMPLER_VALUES = 0x603a9c0,
    G_DEFAULT_SAMPLER = 0x603a9c8,   /* s_defaultSampler */
    G_DEFAULT_SAMPLER_HASH = 0x603aa50,
    G_USED_UNIFORM_BUFS = 0x603ab00,
    G_VERT_SAMPLER_HASH = 0x603ab08, /* uint32[12] */
    G_FRAG_SAMPLER_HASH = 0x603ab38, /* uint32[12] */
    G_DS_HASH = 0x603ab68,           /* s_curDepthStencilHash */
    G_COLOR_TARGETS = 0x603ab70,     /* s_curBoundColorTargets */
    G_BOUND_W = 0x603ab74,
    G_BOUND_H = 0x603ab78,
    G_DEPTH_TARGET = 0x603ab90,      /* s_curBoundDepthTarget */
    G_VIEWPORT = 0x603aba0,          /* MTLViewport (6 doubles) */
    G_APPLY_VIEWPORT = 0x603abd0,
    G_SCISSOR = 0x603abe0,           /* 4 x int64 */
    G_APPLY_SCISSOR = 0x603ac00,
    G_DEVICE = 0x603ad38,            /* g_mtlDev */
    F_SAMPLERS_RESERVE = 0x11e8728,  /* ycOpenHash<...>::Reserve(size) */
    F_SAMPLERS_INSERT = 0x11e8864,   /* ycOpenHash<...>::InsertNoReserve(key) -> value slot */
};

/* kDecodeTbl: the engine's enums to Metal's */
static const uint8_t k_compare[8] = { 7, 0, 1, 2, 3, 4, 5, 6 };
static const uint8_t k_blend[10] = { 0, 1, 2, 3, 6, 7, 4, 5, 8, 9 };
static uint64_t blend_of(unsigned i) { return i < 10 ? k_blend[i] : 0; }
static uint64_t cmp_of(unsigned i) { return i < 8 ? k_compare[i] : 7; }

/* A message with a cache at its call site, as the game's selector stubs have. */
static uint64_t send_at(Ctx *c, const void **site, uint32_t *sel, const char *name, uint64_t recv, int n,
                        const uint64_t *a)
{
    if (!*sel)
        *sel = objc_sel(name);
    c->x[0] = recv;
    c->x[1] = *sel;
    for (int i = 0; i < n; i++)
        c->x[2 + i] = a[i];
    objc_msgSend_site(c, *sel, site);
    return c->x[0];
}
#define SEND(name, recv, ...)                                                                                  \
    ({                                                                                                         \
        static const void *site_;                                                                              \
        static uint32_t sel_;                                                                                  \
        const uint64_t a_[] = { 0, ##__VA_ARGS__ };                                                            \
        send_at(c, &site_, &sel_, name, (recv), (int)(sizeof a_ / sizeof a_[0]) - 1, a_ + 1);                  \
    })

static uint32_t tex_format(uint64_t t)
{
    MTexture *m = t ? objc_host(t) : NULL;
    return m ? m->format : 0;
}
static uint32_t tex_samples(uint64_t t)
{
    MTexture *m = t ? objc_host(t) : NULL;
    return m ? m->samples : 0;
}

/* metal_enc.c: the render encoder's state (DrawCall skips messages that change nothing) */
void *metal_renc_of(uint64_t obj);
int renc_same_pipeline(void *re, uint64_t v);
int renc_same_tex(void *re, int frag, int i, uint64_t t);
int renc_same_stencil_ref(void *re, uint32_t v);
int renc_same_winding(void *re, uint32_t v);
int renc_same_cull(void *re, uint32_t v);
int renc_same_bias(void *re, float b, float s);
int renc_same_vbuffer(void *re, int i, uint64_t buf, uint32_t off);
void renc_set_bytes(void *re, int frag, int i, uint64_t src, uint32_t n);
void renc_draw_indexed(Ctx *c);

/* set a property to v if it is not v already; 1 if it changed */
static int upd(uint64_t obj, const char *key, uint64_t v)
{
    if (md_get(obj, key) == v)
        return 0;
    md_set(obj, key, v);
    return 1;
}

/* s_samplers lookup (ycOpenHash, open addressing): the sampler state for a key, or 0 */
static uint64_t sampler_find(uint8_t *gm_, uint32_t key, uint32_t h)
{
    const uint64_t cap = L64(G_SAMPLER_CAP);
    if (h == 0xFFFFFFFFu || !cap)
        return 0;
    const uint64_t keys = L64(G_SAMPLER_KEYS);
    uint32_t slot = ((((key >> 20) | (key << 12)) + key) * 0xCC9E2D51u + 0xF4116D8Fu) & (uint32_t)(cap - 1);
    if (keys) {
        for (uint64_t probes = L64(G_SAMPLER_PROBES) + 1; probes; probes--) {
            const uint32_t k = L32(keys + slot * 4u);
            if (k == 0 || k == key)
                break;
            slot = (uint32_t)((slot + 1) & (cap - 1));
        }
    }
    if (L32(keys + slot * 4u) != key)
        return 0;
    const uint64_t vals = L64(G_SAMPLER_VALUES);
    return vals ? L64(vals + slot * 8u) : 0;
}

/* What a draw makes its shader's pipeline descriptor be: every value the
 * descriptor is compared with. The same as the last draw of that descriptor,
 * none of the fifty comparisons can find a difference (only DrawCall writes
 * the descriptor) and the hash is not taken. Render thread only. */
#define SNAP_MAX 96
static struct Snap {
    uint64_t desc;
    uint32_t n;
    uint64_t k[SNAP_MAX];
} snaps[64];
static struct Snap *snap_of(uint64_t desc) { return &snaps[(desc >> 4) & 63]; }
static int snap_key(uint8_t *gm_, uint64_t P, uint64_t R, uint64_t *k)
{
    int n = 0;
    k[n++] = L64(L64(R + 0x230) + 0x10);
    k[n++] = L64(L64(R + 0x238) + 0x10);
    k[n++] = L8(P + 0x38);
    const uint64_t li = L64(P + 0x28);
    k[n++] = li != 0;
    if (li) {
        for (int j = 0; j < 3; j++) {
            const uint64_t vb = L64(P + 0x10 + j * 8);
            k[n++] = vb ? ((uint64_t)L32(vb + 0xc) << 8 | (L8(li + 0xc) >> j & 1) << 1 | 1) : 0;
        }
        const uint32_t na = L32(li + 0x8);
        if (na > 24)
            return -1;
        k[n++] = na;
        for (uint32_t i = 0; i < na; i++) {
            const uint64_t e = li + 0x1c + i * 12u;
            k[n++] = (uint64_t)L32(e) << 32 | L32(e + 4);
            k[n++] = L32(e + 8);
        }
    }
    const uint32_t nc = L32(G_COLOR_TARGETS);
    if (nc > 8)
        return -1;
    k[n++] = nc;
    const uint64_t pd = L64(G_PASS_DESC);
    const int depth = L8(G_DEPTH_TARGET) & 1;
    /* the pass descriptor's targets: looked up again only when a descriptor
     * changed (g_pb_gen) or it is another one */
    extern uint32_t g_pb_gen;
    static struct { uint64_t pd, fmt[8], dsamp; uint32_t gen, nc; int depth, valid; } pdc;
    if (!pdc.valid || pdc.pd != pd || pdc.gen != g_pb_gen || pdc.nc != nc || pdc.depth != depth) {
        if (nc) {
            const uint64_t pcolors = md_child(pd, K("colorAttachments"));
            for (uint32_t i = 0; i < nc; i++) {
                const uint64_t t = md_get(md_elem(pcolors, i), K("texture"));
                MTexture *m = t ? objc_host(t) : NULL;
                pdc.fmt[i] = m ? (uint64_t)m->format << 32 | m->samples : 0;
            }
        }
        pdc.dsamp = 0;
        if (depth) {
            const uint64_t t = md_get(md_child(pd, K("depthAttachment")), K("texture"));
            MTexture *m = t ? objc_host(t) : NULL;
            pdc.dsamp = m ? m->samples : 0;
        }
        pdc.pd = pd;
        pdc.nc = nc;
        pdc.depth = depth;
        pdc.gen = g_pb_gen;              /* (after the lookups: they may make a child) */
        pdc.valid = 1;
    }
    if (nc) {
        for (uint32_t i = 0; i < nc; i++)
            k[n++] = pdc.fmt[i];
        k[n++] = L32(P + 0x210);                 /* blend factors */
        k[n++] = L32(P + 0x214) & 15;            /* write mask */
    }
    k[n++] = depth;
    if (depth)
        k[n++] = pdc.dsamp;
    return n;
}

/* the depth/stencil descriptor's hash for the draw's 15 bytes of depth and stencil state */
static struct { uint8_t raw[16]; uint64_t h; int valid; } ds_memo[64];

void f_1e8cd0_orig(Ctx *c);

void nat__ZN14ycRenderDevice8DrawCallEPv(Ctx *c)
{
    GM_BEGIN;
    const uint64_t P = c->x[0];
    const uint64_t R = L64(P + 0x8);
    const uint64_t desc = L64(R + 0x130);
    /* the first draw of a shader makes its descriptor; a sampler state of an
     * unknown filter would be made by the game's own switch: the original */
    int fallback = desc == 0;
    for (int i = 0; i < 12 && !fallback; i++) {
        const uint8_t b3 = L8(P + 0x1c3 + i * 4), f = L8(P + 0x1c0 + i * 4);
        if ((b3 & 0x80) && (f > 5 || (b3 & 0x7f) >= 8))
            fallback = 1;
    }
    if (fallback) {
        if (desc)
            snap_of(desc)->desc = 0;     /* the original may change it */
        f_1e8cd0_orig(c);
        return;
    }
    const uint64_t pass = L64(G_CUR_PASS), dev = L64(G_DEVICE);
    void *const re = metal_renc_of(pass);    /* our encoder: messages that change nothing are not sent */
    int changed = 0;
    uint64_t key[SNAP_MAX];
    const int nkey = snap_key(gm_, P, R, key);
    struct Snap *sn = snap_of(desc);
    const int same = nkey >= 0 && sn->desc == desc && sn->n == (uint32_t)nkey &&
                     !memcmp(sn->k, key, (size_t)nkey * sizeof key[0]);
    if (same)
        goto pipeline_set;

    /* ---- the pipeline descriptor ---- */
    const uint64_t vf = L64(L64(R + 0x230) + 0x10), ff = L64(L64(R + 0x238) + 0x10);
    changed |= upd(desc, K("vertexFunction"), vf);
    changed |= upd(desc, K("fragmentFunction"), ff);
    changed |= upd(desc, K("inputPrimitiveTopology"), L8(P + 0x38) ? 2 : 3);
    const uint64_t layout_info = L64(P + 0x28);
    if (layout_info) {
        /* sub-descriptors are made where the game first asks for them (their keys are in the hash) */
        for (int k = 0; k < 3; k++) {
            const uint64_t vb = L64(P + 0x10 + k * 8);
            if (!vb)
                continue;
            const uint64_t lay = md_elem(md_child(md_child(desc, K("vertexDescriptor")), K("layouts")), 8 + k);
            changed |= upd(md_elem(md_child(desc, K("vertexBuffers")), 8 + k), K("mutability"), 2);
            changed |= upd(lay, K("stepRate"), 1);
            changed |= upd(lay, K("stride"), L32(vb + 0xc));
            changed |= upd(lay, K("stepFunction"), (L8(layout_info + 0xc) >> k & 1) ? 2 : 1);
        }
        const uint32_t nattr = L32(layout_info + 0x8);
        for (uint32_t i = 0; i < nattr; i++) {
            const uint64_t a = md_elem(md_child(md_child(desc, K("vertexDescriptor")), K("attributes")), i);
            const uint64_t e = layout_info + 0x1c + i * 12u;
            changed |= upd(a, K("format"), L32(e + 4));
            changed |= upd(a, K("offset"), L32(e + 8));
            changed |= upd(a, K("bufferIndex"), (uint64_t)(uint32_t)(L32(e) + 8));
        }
    }
    const uint64_t colors = md_child(desc, K("colorAttachments"));
    const uint64_t pd = L64(G_PASS_DESC);
    if (L32(G_COLOR_TARGETS) == 0) {
        const uint64_t a = md_elem(colors, 0);
        changed |= upd(a, K("pixelFormat"), 0);
        changed |= upd(a, K("writeMask"), 0);
    }
    if (L32(G_COLOR_TARGETS)) {
        const uint64_t pcolors = md_child(pd, K("colorAttachments"));
        for (uint32_t i = 0; i < L32(G_COLOR_TARGETS); i++) {
            const uint64_t a = md_elem(colors, i);
            const uint64_t ptex = md_get(md_elem(pcolors, i), K("texture"));
            changed |= upd(a, K("pixelFormat"), tex_format(ptex));
            changed |= upd(desc, K("rasterSampleCount"), tex_samples(ptex));
            const uint32_t m = L32(P + 0x214);
            const uint32_t wm = (((m & 1) << 3) | ((m & 2) << 1) | ((m & 4) >> 1) | ((m & 8) >> 3));
            changed |= upd(a, K("writeMask"), wm);
            const uint8_t b0 = L8(P + 0x210), b1 = L8(P + 0x211), b2 = L8(P + 0x212), b3 = L8(P + 0x213);
            if (b0 == 1 && b1 == 0 && b2 == 1 && b3 == 0) {
                if ((uint32_t)md_get(a, K("blendingEnabled")) != 0) {
                    md_set(a, K("blendingEnabled"), 0);
                    changed = 1;
                }
            } else {
                if (!(md_get(a, K("blendingEnabled")) & 1)) {
                    md_set(a, K("blendingEnabled"), 1);
                    changed = 1;
                }
                changed |= upd(a, K("sourceRGBBlendFactor"), blend_of(b0));
                changed |= upd(a, K("destinationRGBBlendFactor"), blend_of(b1));
                changed |= upd(a, K("sourceAlphaBlendFactor"), blend_of(b2));
                changed |= upd(a, K("destinationAlphaBlendFactor"), blend_of(b3));
            }
        }
    }
    int rehash = 0;
    if (L8(G_DEPTH_TARGET) & 1) {
        changed |= upd(desc, K("depthAttachmentPixelFormat"), 0x104);
        changed |= upd(desc, K("stencilAttachmentPixelFormat"), 0x104);
        const uint64_t dtex = md_get(md_child(pd, K("depthAttachment")), K("texture"));
        if (upd(desc, K("rasterSampleCount"), tex_samples(dtex)))
            rehash = 1;
    } else {
        changed |= upd(desc, K("depthAttachmentPixelFormat"), 0);
        if (upd(desc, K("stencilAttachmentPixelFormat"), 0))
            rehash = 1;
    }
    if (changed || rehash) {
        const uint64_t h = md_hash(desc);
        const uint64_t idx = h % 31;
        if (L64(R + 0x138 + idx * 8) == h) {
            const uint64_t ps = L64(R + 0x30 + idx * 8), old = L64(R + 0x128);   /* objc_storeStrong */
            objc_retain_obj(ps);
            S64(R + 0x128, ps);
            objc_release_obj(old);
        } else {
            const uint64_t sp = c->sp, out = (sp - 0x40) & ~15ull;
            S64(out, 0);
            S64(out + 8, 0);
            c->sp = out;
            const uint64_t ps = SEND("newRenderPipelineStateWithDescriptor:options:reflection:error:", dev, desc, 1,
                                     out, out + 8);
            c->sp = sp;
            const uint64_t refl = L64(out), old = L64(R + 0x128);
            S64(R + 0x128, ps);
            objc_release_obj(old);
            uint64_t vbuf, vtex, fbuf, ftex;
            const int big = md_refl_masks(refl, &vbuf, &vtex, &fbuf, &ftex);
            if (big >= 0) {
                S64(R + 0x10, L64(R + 0x10) | vbuf);
                S64(R + 0x8, L64(R + 0x8) | vtex);
                S64(R + 0x20, L64(R + 0x20) | fbuf);
                S64(R + 0x18, L64(R + 0x18) | ftex);
                if (big)
                    S8(R + 0x28, 1);
            }
            S64(R + 0x138 + idx * 8, h);
            const uint64_t cur = L64(R + 0x128), prev = L64(R + 0x30 + idx * 8);   /* objc_storeStrong */
            objc_retain_obj(cur);
            S64(R + 0x30 + idx * 8, cur);
            objc_release_obj(prev);
        }
    }
    if (nkey >= 0) {
        sn->desc = desc;
        sn->n = (uint32_t)nkey;
        memcpy(sn->k, key, (size_t)nkey * sizeof key[0]);
    } else {
        sn->desc = 0;
    }
pipeline_set:
    if (!re || !renc_same_pipeline(re, L64(R + 0x128)))
        SEND("setRenderPipelineState:", pass, L64(R + 0x128));

    if (pass) {
        /* ---- constant buffers ---- */
        for (int i = 0; i < 8; i++) {
            const uint64_t e = P + 0x40 + i * 0x18, base = L64(e);
            if (!base)
                continue;
            const uint64_t bit = 1ull << i, vs = L64(R + 0x10) & bit, fs = L64(R + 0x20) & bit;
            if (!(vs | fs))
                continue;
            const uint64_t data = L64(e + 8);
            if (!L8(R + 0x28)) {
                uint64_t len = (uint64_t)L32(e + 0x10) << 4;
                len = len < 0x1000 ? len : 0x1000;
                len = len > 0x80 ? len : 0x80;
                if (re) {
                    if (vs)
                        renc_set_bytes(re, 0, i, data, (uint32_t)len);
                    if (fs)
                        renc_set_bytes(re, 1, i, data, (uint32_t)len);
                    continue;
                }
                if (vs)
                    SEND("setVertexBytes:length:atIndex:", pass, data, len, (uint64_t)i);
                if (fs)
                    SEND("setFragmentBytes:length:atIndex:", pass, data, len, (uint64_t)i);
                continue;
            }
            uint64_t ub = L64(base + 0x10);
            if (ub == ~0ull) {           /* first use: a buffer of its own */
                ub = L64(G_USED_UNIFORM_BUFS);
                S64(G_USED_UNIFORM_BUFS, ub + 1);
                const uint64_t size = L64(base + 8) - base;
                const uint64_t nb = SEND("newBufferWithLength:options:", dev, size + 0x3200, 0);
                const uint64_t old = L64(G_UNIFORM_BUFS + ub * 8);
                S64(G_UNIFORM_BUFS + ub * 8, nb);
                objc_release_obj(old);
                const uint64_t contents = SEND("contents", nb);
                memmove(GP(contents), GP(base), (size_t)size);
                S64(base + 0x10, ub);
            }
            const uint64_t buf = L64(G_UNIFORM_BUFS + ub * 8);
            if (vs)
                SEND("setVertexBuffer:offset:atIndex:", pass, buf, data - base, (uint64_t)i);
            if (fs)
                SEND("setFragmentBuffer:offset:atIndex:", pass, buf, data - base, (uint64_t)i);
        }
        /* ---- textures ---- */
        for (int i = 0; i < 12; i++) {
            const uint32_t fl = L32(P + 0x100 + i * 16);
            if (!fl)
                continue;
            const uint64_t o = L64(P + 0x108 + i * 16);
            uint64_t tex = 0;
            switch (fl & 0xff) {
            case 0: tex = L64(o + 0x20); break;
            case 1: tex = L32(o + 0x10) == 1 ? L64(o + 0x18) : L64(o + 0x20); break;
            case 2: tex = L32(o + 0x20) == 1 ? L64(o + 0x8) : L64(o + 0x10); break;
            case 3: tex = L64(o + 0x18); break;
            case 4: tex = L64(o + 0x8); break;
            }
            if ((fl >> 8 & 1) && (L64(R + 0x8) >> i & 1) && !(re && renc_same_tex(re, 0, i, tex)))
                SEND("setVertexTexture:atIndex:", pass, tex, (uint64_t)i);
            if ((fl >> 9 & 1) && (L64(R + 0x18) >> i & 1) && !(re && renc_same_tex(re, 1, i, tex)))
                SEND("setFragmentTexture:atIndex:", pass, tex, (uint64_t)i);
        }
        /* ---- samplers ---- */
        for (int i = 0; i < 12; i++) {
            const uint64_t vh = G_VERT_SAMPLER_HASH + i * 4, fh = G_FRAG_SAMPLER_HASH + i * 4;
            const uint8_t b3 = L8(P + 0x1c3 + i * 4);
            if (!(b3 & 0x80)) {          /* the default sampler */
                const uint32_t want = L8(G_DEFAULT_SAMPLER_HASH) ? 0x4effffffu : 0;
                if (L32(vh) != want) {
                    SEND("setVertexSamplerState:atIndex:", pass, L64(G_DEFAULT_SAMPLER), (uint64_t)i);
                    S32(vh, want);
                }
                if (L32(fh) != want) {
                    SEND("setFragmentSamplerState:atIndex:", pass, L64(G_DEFAULT_SAMPLER), (uint64_t)i);
                    S32(fh, want);
                }
                continue;
            }
            const uint8_t filter = L8(P + 0x1c0 + i * 4), wrap = L8(P + 0x1c1 + i * 4), cmp = b3 & 0x7f;
            const uint32_t hm = (cmp | (uint32_t)filter << 24 | (uint32_t)wrap << 16) * 0x9E3779B1u;
            const uint32_t key = ~hm;
            if (L32(fh) == key && L32(vh) == key)
                continue;
            uint64_t s = sampler_find(gm_, key, hm);
            if (!s) {                    /* a new sampler state, kept in s_samplers */
                static Class *sd_cls;
                if (!sd_cls)
                    sd_cls = objc_class_named("MTLSamplerDescriptor");
                const uint64_t sd = objc_new_object(sd_cls);
                const uint8_t aniso = L8(P + 0x1c2 + i * 4);
                if (filter == 0) {
                    md_set(sd, K("magFilter"), 0);
                    md_set(sd, K("minFilter"), 0);
                    md_set(sd, K("mipFilter"), 1);
                    md_set(sd, K("maxAnisotropy"), aniso > 1 ? aniso : 1);
                } else if (filter == 2) {
                    md_set(sd, K("magFilter"), 1);
                    md_set(sd, K("minFilter"), 1);
                    md_set(sd, K("mipFilter"), 1);
                    md_set(sd, K("maxAnisotropy"), 1);
                } else {
                    md_set(sd, K("magFilter"), 1);
                    md_set(sd, K("minFilter"), 1);
                    md_set(sd, K("mipFilter"), 2);
                    md_set(sd, K("maxAnisotropy"), aniso > 1 ? aniso : 1);
                }
                const uint64_t am = wrap ? 0 : 2;
                md_set(sd, K("rAddressMode"), am);
                md_set(sd, K("sAddressMode"), am);
                md_set(sd, K("tAddressMode"), am);
                md_set(sd, K("compareFunction"), cmp_of(cmp));
                s = SEND("newSamplerStateWithDescriptor:", dev, sd);
                if (!L64(G_SAMPLER_CAP)) {
                    c->x[0] = 0x100;
                    rt_call(c, F_SAMPLERS_RESERVE);
                }
                const uint64_t count = L64(G_SAMPLER_COUNT);
                if (count) {
                    const uint64_t cap = L64(G_SAMPLER_CAP);
                    if (!(cap * 14 > count * 16)) {
                        c->x[0] = cap << 1;
                        rt_call(c, F_SAMPLERS_RESERVE);
                    }
                }
                c->x[0] = key;
                rt_call(c, F_SAMPLERS_INSERT);
                S64(c->x[0], s);
                objc_release_obj(sd);
            }
            if (L32(vh) != key) {
                SEND("setVertexSamplerState:atIndex:", pass, s, (uint64_t)i);
                S32(vh, key);
            }
            if (L32(fh) != key) {
                SEND("setFragmentSamplerState:atIndex:", pass, s, (uint64_t)i);
                S32(fh, key);
            }
        }
    }

    /* ---- depth and stencil: a descriptor for the draw, a state when its hash changes ---- */
    if (L8(G_DEPTH_TARGET) == 1) {
        uint8_t raw[16] = { 0 };
        memcpy(raw, GP(P + 0x200), 15);
        if (!L8(P + 0x20e))              /* the stencil bytes do not count then */
            memset(raw + 1, 0, 11);
        if (!L8(P + 0x20c))
            raw[0] = 0;
        /* (a slot of the memo: it compares the bytes) */
        const uint32_t mh = (rt_ld32(raw) * 0x9E3779B1u) ^ (rt_ld32(raw + 4) * 0x85EBCA77u) ^
                            (rt_ld32(raw + 8) * 0xC2B2AE3Du) ^ (rt_ld32(raw + 12) * 0x27D4EB2Fu);
        __typeof__(&ds_memo[0]) dm = &ds_memo[mh >> 26];
        if (dm->valid && !memcmp(dm->raw, raw, 16) && dm->h == L64(G_DS_HASH)) {
            if (L8(P + 0x20e) && !(re && renc_same_stencil_ref(re, L8(P + 0x203))))
                SEND("setStencilReferenceValue:", pass, (uint64_t)L8(P + 0x203));
            goto ds_done;
        }
        static Class *ds_cls;
        if (!ds_cls)
            ds_cls = objc_class_named("MTLDepthStencilDescriptor");
        const uint64_t ds = objc_new_object(ds_cls);
        md_set(ds, K("depthWriteEnabled"), L8(P + 0x20d) != 0);
        md_set(ds, K("depthCompareFunction"), L8(P + 0x20c) ? cmp_of(L8(P + 0x200)) : 7);
        const uint64_t back = md_child(ds, K("backFaceStencil")), front = md_child(ds, K("frontFaceStencil"));
        if (L8(P + 0x20e)) {
            md_set(back, K("readMask"), L8(P + 0x201));
            md_set(back, K("writeMask"), L8(P + 0x202));
            md_set(back, K("stencilFailureOperation"), L8(P + 0x208) & 7);
            md_set(back, K("depthStencilPassOperation"), L8(P + 0x20a) & 7);
            md_set(back, K("depthFailureOperation"), L8(P + 0x209) & 7);
            md_set(back, K("stencilCompareFunction"), cmp_of(L8(P + 0x20b)));
            md_set(front, K("readMask"), L8(P + 0x201));
            md_set(back, K("writeMask"), L8(P + 0x202));     /* the game's: back again, front keeps its own */
            md_set(front, K("stencilFailureOperation"), L8(P + 0x204) & 7);
            md_set(front, K("depthStencilPassOperation"), L8(P + 0x206) & 7);
            md_set(front, K("depthFailureOperation"), L8(P + 0x205) & 7);
            md_set(front, K("stencilCompareFunction"), cmp_of(L8(P + 0x207)));
        } else {
            md_set(back, K("stencilCompareFunction"), 7);
            md_set(front, K("stencilCompareFunction"), 7);
        }
        const uint64_t h = md_hash(ds);
        memcpy(dm->raw, raw, 16);
        dm->h = h;
        dm->valid = 1;
        if (h != L64(G_DS_HASH)) {
            const uint64_t st = SEND("newDepthStencilStateWithDescriptor:", dev, ds);
            SEND("setDepthStencilState:", pass, st);
            objc_release_obj(st);
            S64(G_DS_HASH, h);
        }
        if (L8(P + 0x20e))
            SEND("setStencilReferenceValue:", pass, (uint64_t)L8(P + 0x203));
        objc_release_obj(ds);
    }
ds_done:

    /* ---- viewport, scissor, raster state ---- */
    if (L8(G_APPLY_VIEWPORT) == 1) {
        const uint64_t sp = c->sp, out = (sp - 0x40) & ~15ull;
        memcpy(GP(out), GP(G_VIEWPORT), 48);
        c->sp = out;
        SEND("setViewport:", pass, out);
        c->sp = sp;
        S8(G_APPLY_VIEWPORT, 0);
    }
    if (L8(G_APPLY_SCISSOR) == 1) {
        const int64_t x = (int64_t)L64(G_SCISSOR), y = (int64_t)L64(G_SCISSOR + 8);
        int64_t w = (int64_t)L64(G_SCISSOR + 16), h = (int64_t)L64(G_SCISSOR + 24);
        const int64_t rw = (int64_t)L32(G_BOUND_W) - x, rh = (int64_t)L32(G_BOUND_H) - y;
        if (rw < w)
            w = rw;
        if (rh < h)
            h = rh;
        const uint64_t sp = c->sp, out = (sp - 0x40) & ~15ull;
        S64(out, x);
        S64(out + 8, y);
        S64(out + 16, w);
        S64(out + 24, h);
        c->sp = out;
        SEND("setScissorRect:", pass, out);
        c->sp = sp;
        S8(G_APPLY_SCISSOR, 0);
    }
    if (!(re && renc_same_winding(re, 1)))
        SEND("setFrontFacingWinding:", pass, 1);
    if (L8(P + 0x1f1) <= 2 && !(re && renc_same_cull(re, L8(P + 0x1f1))))
        SEND("setCullMode:", pass, (uint64_t)L8(P + 0x1f1));
    memcpy(&c->v[0].f[0], GP(P + 0x1f4), 4);
    memcpy(&c->v[1].f[0], GP(P + 0x1f8), 4);
    memcpy(&c->v[2].f[0], GP(P + 0x1fc), 4);
    if (!(re && renc_same_bias(re, c->v[0].f[0], c->v[1].f[0])))
        SEND("setDepthBias:slopeScale:clamp:", pass);
    for (int k = 0; k < 3; k++) {
        const uint64_t vb = L64(P + 0x10 + k * 8);
        if (vb && !(re && renc_same_vbuffer(re, 8 + k, L64(vb + 0x18), 0)))
            SEND("setVertexBuffer:offset:atIndex:", pass, L64(vb + 0x18), 0, (uint64_t)(8 + k));
    }

    /* ---- the draw ---- */
    const uint64_t prim = L8(P + 0x38) == 1 ? 1 : 3;
    const uint64_t count = L32(P + 0x21c), inst = L32(P + 0x220), start = L32(P + 0x218);
    const int indexed = L32(P + 0x224) & 1;
    if (!indexed) {
        if (inst < 2)
            SEND("drawPrimitives:vertexStart:vertexCount:", pass, prim, start, count);
        else
            SEND("drawPrimitives:vertexStart:vertexCount:instanceCount:", pass, prim, start, count, inst);
        return;
    }
    const uint64_t ib = L64(P + 0x30);
    const uint32_t isz = L32(ib + 0xc);
    const uint64_t buf = L64(ib + 0x18);
    if (inst < 2 && re) {
        c->x[0] = pass;
        c->x[2] = prim;
        c->x[3] = count;
        c->x[4] = (uint64_t)(isz != 2);
        c->x[5] = buf;
        c->x[6] = (uint64_t)(uint32_t)(isz * (uint32_t)start);
        renc_draw_indexed(c);
    } else if (inst < 2)
        SEND("drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:", pass, prim, count,
             (uint64_t)(isz != 2), buf, (uint64_t)(uint32_t)(isz * (uint32_t)start));
    else
        SEND("drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:", pass, prim,
             count, (uint64_t)(isz != 2), buf, (uint64_t)(uint32_t)(isz * (uint32_t)start), inst);
}

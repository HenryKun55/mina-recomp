/* Metal on OpenGL: render and blit command encoders (executed immediately). */
#include <stddef.h>
#include "metal_gl.h"

#include <string.h>
#include <time.h>

void metal_pass_info(uint64_t pd, uint64_t *color, uint64_t *depth, uint64_t *stencil);
GLuint metal_pass_fbo(MTexture **color, const int *levels, MTexture *depth, MTexture *stencil);
GLuint metal_tex_fbo(MTexture *t, int level);
void metal_tex_upload(MTexture *t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t level, uint32_t slice,
                      const uint8_t *src, uint32_t bpr);
GLenum metal_tex_target(const MTexture *t);
GLenum metal_gl_compare(uint32_t f);

/* ---- streaming vertex/index ring ------------------------------------------------------- */

#if defined(__vita__)
#define RING_SIZE (8u << 20)      /* GPU memory: a frame streams far less */
#else
#define RING_SIZE (64u << 20)
#endif
static void gl_check(const char *where);
static GLuint ring;
static uint32_t ring_pos;

#if defined(__vita__)
/* vitaGL's glBufferSubData on a buffer drawn from in the last frames makes a
 * new one and copies all of it (8 MiB for each upload): the ring's memory is
 * written in place instead (glMapBuffer hands its pointer), and the ring
 * starts over from 0 - it only gets a new store (orphaned) when it went round
 * in fewer frames than the GPU can be behind. */
static uint8_t *ring_ptr;
static uint32_t ring_wrap_frame;
static void ring_map(void)
{
    glBindBuffer(GL_ARRAY_BUFFER, ring);
    ring_ptr = glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
    glUnmapBuffer(GL_ARRAY_BUFFER);
    if (!ring_ptr)
        rt_fatal("metal: the vertex ring cannot be mapped");
}
static uint32_t ring_upload(const void *data, uint32_t size)
{
    if (!ring) {
        glGenBuffers(1, &ring);
        glBindBuffer(GL_ARRAY_BUFFER, ring);
        glBufferData(GL_ARRAY_BUFFER, RING_SIZE, NULL, GL_STREAM_DRAW);
        ring_map();
    }
    uint32_t pos = (ring_pos + 255u) & ~255u;
    if (pos + size > RING_SIZE) {
        if (size > RING_SIZE)
            rt_fatal("metal: %u bytes for the vertex ring", size);
        if (g_rdbg.presents - ring_wrap_frame < 4) {
            glBindBuffer(GL_ARRAY_BUFFER, ring);
            glBufferData(GL_ARRAY_BUFFER, RING_SIZE, NULL, GL_STREAM_DRAW);   /* orphan */
            ring_map();
        }
        ring_wrap_frame = g_rdbg.presents;
        pos = 0;
    }
    memcpy(ring_ptr + pos, data, size);
    ring_pos = pos + size;
    return pos;
}
#else
static uint32_t ring_upload(const void *data, uint32_t size)
{
    if (!ring) {
        glGenBuffers(1, &ring);
        glBindBuffer(GL_ARRAY_BUFFER, ring);
        glBufferData(GL_ARRAY_BUFFER, RING_SIZE, NULL, GL_STREAM_DRAW);
    }
    glBindBuffer(GL_ARRAY_BUFFER, ring);
    uint32_t pos = (ring_pos + 255u) & ~255u;
    if (pos + size > RING_SIZE) {
        glBufferData(GL_ARRAY_BUFFER, RING_SIZE, NULL, GL_STREAM_DRAW);   /* orphan */
        pos = 0;
    }
    glBufferSubData(GL_ARRAY_BUFFER, pos, size, data);
    ring_pos = pos + size;
    return pos;
}
#endif

/* ---- render encoder ------------------------------------------------------------------------ */

typedef struct Binding {
    MBuffer *buf;            /* or NULL with inline bytes */
    uint64_t obj;
    uint32_t offset;
    uint8_t *bytes;          /* inline bytes when nbytes (the buffer kept for the next ones) */
    uint32_t nbytes, cap;
} Binding;

typedef struct REnc {
    GLuint fbo;
    MTexture *color[MAX_COLOR];
    uint64_t color_obj[MAX_COLOR];
    MTexture *resolve[MAX_COLOR];
    uint64_t resolve_obj[MAX_COLOR];
    uint32_t tw, th;
    uint64_t pipeline;
    Binding vb[MAX_VBUF], fb[MAX_VBUF];
    uint64_t vtex[16], ftex[16], vsamp[16], fsamp[16];
    uint64_t ds;
    uint32_t stencil_ref;
    uint32_t cull, winding;
    int ended;
    /* the pass starts lazily (pass_begin) at its first draw: a pass that
     * draws nothing only clears, and not even that when its targets already
     * hold those values (MTexture.clear_valid) */
    int begun, levels[MAX_COLOR];
    MTexture *dtex, *stex;
    unsigned clear_mask;                 /* color attachments with loadAction clear */
    float clear_color[MAX_COLOR][4];
    int clear_depth, clear_stencil;
    float depth_value;
    int stencil_value;
    int vp_set, sc_set;
    float depth_bias, depth_slope;       /* setDepthBias:slopeScale:clamp: (applied at each draw) */
    int to_display;                      /* Vita: the drawable's pass draws to the screen */
    int folded;                          /* Vita: its copy was folded (fold_pass), nothing drawn yet */
    int depth_stale;                     /* Vita: the depth in memory is not the pass's (depth_by_shader) */
    int depth_lost;                      /* Vita: a depth_by_shader draw would have written depth (fs) */
    int depth_aliased;                   /* Vita: the depth its first draw copies is the pass's (depth_alias) */
    struct MTexture *ftex_folded;        /* the copy's source */
    double vp[6];
    int sc[4];
} REnc;

static void bind_release(Binding *b)
{
    if (b->obj)
        objc_release_obj(b->obj);
    free(b->bytes);
    memset(b, 0, sizeof *b);
}

static void renc_dealloc(void *h)
{
    REnc *e = h;
    for (int i = 0; i < MAX_VBUF; i++) {
        bind_release(&e->vb[i]);
        bind_release(&e->fb[i]);
    }
    for (int i = 0; i < 16; i++) {
        objc_release_obj(e->vtex[i]);
        objc_release_obj(e->ftex[i]);
        objc_release_obj(e->vsamp[i]);
        objc_release_obj(e->fsamp[i]);
    }
    for (int i = 0; i < MAX_COLOR; i++) {
        objc_release_obj(e->color_obj[i]);
        objc_release_obj(e->resolve_obj[i]);
    }
    objc_release_obj(e->pipeline);
    objc_release_obj(e->ds);
}

Class cls_RenderEnc;

/* MINA_FRAMELOG=K: every K-th presented frame, its passes and draws (for
 * comparing the Vita with the Mac frame by frame) */
int g_framelog;          /* frame being logged, 0: none */
/* MINA_SHOW_PASS=n: the color target of the n-th render pass of every frame
 * is what reaches the screen (debug: see an intermediate layer) */
MTexture *g_show_tex;
int g_pass_index;
static void framelog_pass(REnc *e, uint64_t *color, uint64_t depth, uint64_t stencil)
{
    char buf[256];
    int n = snprintf(buf, sizeof buf, "[frame %d] pass %ux%u:", g_framelog, e->tw, e->th);
    for (int i = 0; i < MAX_COLOR && n < (int)sizeof buf; i++)
        if (e->color[i])
            n += snprintf(buf + n, sizeof buf - n, " c%d fmt %u load %u store %u%s#%04x", i, e->color[i]->format,
                          (uint32_t)pb_get(color[i], "loadAction", 0), (uint32_t)pb_get(color[i], "storeAction", 1),
                          e->color[i]->is_drawable ? " (drawable)" : "",
                          (unsigned)(((uintptr_t)e->color[i] >> 4) & 0xffff));
    if (e->color[0] && n < (int)sizeof buf)
        n += snprintf(buf + n, sizeof buf - n, " clear (%g %g %g %g)", pb_getd(color[0], "clearColor", 0, 0.0),
                      pb_getd(color[0], "clearColor", 1, 0.0), pb_getd(color[0], "clearColor", 2, 0.0),
                      pb_getd(color[0], "clearColor", 3, 1.0));
    if (depth && pb_get(depth, "texture", 0))
        n += snprintf(buf + n, sizeof buf - n, " depth load %u#%04x (clear %g)", (uint32_t)pb_get(depth, "loadAction", 0),
                      (unsigned)(((uintptr_t)objc_host(pb_get(depth, "texture", 0)) >> 4) & 0xffff),
                      pb_getd(depth, "clearDepth", 0, 1.0));
    if (stencil && pb_get(stencil, "texture", 0))
        n += snprintf(buf + n, sizeof buf - n, " stencil load %u (clear %u)", (uint32_t)pb_get(stencil, "loadAction", 0),
                      (uint32_t)pb_get(stencil, "clearStencil", 0));
    rt_log("%s\n", buf);
}

/* MINA_NO_DIRECT: the drawable stays a texture copied to the screen */
int direct_display(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("MINA_NO_DIRECT") == NULL;
    return on;
}

/* rows run the other way on the screen (the shaders' y sign is +1 there) */
static int screen_y(REnc *e, int y, int h) { return e->to_display ? (int)e->th - y - h : y; }

static void apply_viewport(REnc *e)
{
    const double *v = e->vp;
    glViewport((GLint)v[0], screen_y(e, (int)v[1], (int)v[3]), (GLsizei)v[2], (GLsizei)v[3]);
#if defined(__vita__)
    /* the CG shaders keep Metal's 0..1 depth: vitaGL's window z is
     * (f + n) / 2 + z * (f - n) / 2, so this stores zn + z * (zf - zn) */
    glDepthRange(2.0 * v[4] - v[5], v[5]);
#else
    glDepthRange(v[4], v[5]);
#endif
}

/* a stencil face that never changes the stencil */
static int stencil_keeps(const MStencil *s)
{
    return s->write_mask == 0 || (s->fail == 0 && s->depth_fail == 0 && s->pass == 0);
}

/* ---- folded copy passes (Vita) ---------------------------------------------------------
 * The game draws its frame into one texture, then copies it whole into another
 * (a shader raising the colour to a gamma of 1, alpha 1) that only a shader
 * reading the colour samples: a GXM scene of its own for nothing. That pass is
 * not drawn (fold_pass): its target reads as the source (MTexture.read_alias),
 * and is drawn for real (fold_draw) only when something needs it as it is: a
 * shader reading its alpha, a blit, a pass loading it. When the source changes
 * first the target goes stale: the game draws it again before reading it, and
 * if it ever reads it instead, that texture is never folded again (logged).
 * MINA_NO_FOLD: every copy is drawn. */
#if defined(__vita__)
extern int g_copy_rgb;
#define MAX_FOLDS 8
static MTexture *folds[MAX_FOLDS];
static int nfolds;

static void fold_drop(MTexture *x)
{
    x->read_alias = NULL;
    for (int i = 0; i < nfolds; i++)
        if (folds[i] == x) {
            folds[i] = folds[--nfolds];
            break;
        }
}

/* the copy, drawn: the source's colour with alpha 1 */
static void fold_draw(MTexture *x)
{
    GL_DIRTY();
    MTexture *src = x->read_alias;
    if (g_framelog)
        rt_log("[frame %d]   (fold drawn: #%04x from #%04x)\n", g_framelog, (unsigned)(((uintptr_t)x >> 4) & 0xffff),
               (unsigned)(((uintptr_t)src >> 4) & 0xffff));
    fold_drop(x);
    /* folding it only to draw it later saves nothing: not again */
    if (!x->no_fold) {
        x->no_fold = 1;
        rt_log("metal: a folded %ux%u copy is needed whole: that texture is not folded again\n", x->width, x->height);
    }
    GLuint fbo = metal_tex_fbo(x, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, (GLsizei)x->width, (GLsizei)x->height);
    glDisable(GL_SCISSOR_TEST);
    static const GLfloat black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearBufferfv(GL_COLOR, 0, black);
    g_copy_rgb = 1;
    vita_copy_quad(src, 0, 0, (float)src->width, (float)src->height, fbo, 0, 0, (GLsizei)x->width,
                   (GLsizei)x->height, 0);
    g_copy_rgb = 0;
}

/* contents lost to a fold read before being drawn again */
static void fold_check_stale(MTexture *t)
{
    if (!t->fold_stale)
        return;
    t->fold_stale = 0;
    t->no_fold = 1;
    rt_log("metal: a %ux%u texture was read after the copy folded into it went stale: not folded again\n",
           t->width, t->height);
}

/* t changes: textures reading as t go stale */
static void fold_source_changes(MTexture *t)
{
    for (int i = 0; i < nfolds;)
        if (folds[i]->read_alias == t) {
            folds[i]->fold_stale = 1;
            fold_drop(folds[i]);
        } else {
            i++;
        }
}

void metal_fold_read(MTexture *t)
{
    if (t->read_alias)
        fold_draw(t);
    fold_check_stale(t);
}
void metal_fold_write(MTexture *t)
{
    fold_source_changes(t);
    metal_fold_read(t);             /* a partial write keeps the rest */
}
void metal_fold_forget(MTexture *t)
{
    fold_source_changes(t);
    if (t->read_alias)
        fold_drop(t);
}

static int fold_off = -1;
static int fold_blend_ok(const MBlend *b)
{
    if (!b->enabled)
        return 1;
    /* with the shader's alpha of 1 these all write the source colour, alpha 1 */
    return (b->src_rgb == 1 || b->src_rgb == 4) && (b->dst_rgb == 0 || b->dst_rgb == 5) &&
           (b->src_a == 1 || b->src_a == 4) && (b->dst_a == 0 || b->dst_a == 5) && b->rgb_op == 0 && b->alpha_op == 0;
}
static const uint8_t *binding_data(const Binding *b, uint32_t *avail);

/* A pass whose first draw copies a texture whole into its cleared target: not
 * drawn (see above). */
/* MINA_FOLDLOG: why a copy pass was not folded */
static int fold_no(int why)
{
    static int log = -1, n;
    if (log < 0)
        log = getenv("MINA_FOLDLOG") != NULL;
    if (log && n++ < 20)
        rt_log("[fold] not folded (%d)\n", why);
    return 0;
}
static int fold_pass(REnc *e, MPipeline *p, uint32_t count, uint32_t instances)
{
    if (fold_off < 0)
        fold_off = getenv("MINA_NO_FOLD") != NULL || getenv("MINA_NO_SKIP") != NULL;
    MTexture *owe = NULL;
    if (fold_off || e->begun || e->folded || p->fs < 0 || !g_shaders[p->fs].gamma_copy0 || count != 6 ||
        instances != 1)
        return 0;
    MTexture *x = e->color[0];
    if (!x || x->no_fold || x->is_drawable || x->is_rb || x->shift || e->levels[0] || e->resolve[0] ||
        !(e->clear_mask & 1) || x->type != 2)
        return fold_no(2);
    for (int i = 1; i < MAX_COLOR; i++)
        if (e->color[i])
            return fold_no(3);
    MTexture *t = e->ftex[0] ? objc_host(e->ftex[0]) : NULL;
    if (!t || t == x || t->is_rb || t->shift || t->read_alias || t->fold_stale || t->width != x->width ||
        t->height != x->height || t->format != x->format || t->type != 2)
        return fold_no(4);
    uint32_t avail = 0;
    const uint8_t *cb = binding_data(&e->fb[1], &avail);
    float gamma;
    if (!cb || avail < 4)
        return fold_no(5);
    memcpy(&gamma, cb, 4);
    if (gamma != 1.0f || p->color[0].write_mask != 0xf || !fold_blend_ok(&p->color[0]))
        return fold_no(6);
    if (e->vp_set && (e->vp[0] != 0 || e->vp[1] != 0 || e->vp[2] != e->tw || e->vp[3] != e->th))
        return fold_no(7);
    if (e->sc_set && (e->sc[0] != 0 || e->sc[1] != 0 || e->sc[2] != (int)e->tw || e->sc[3] != (int)e->th))
        return fold_no(8);
    if (e->dtex || e->stex) {
        MDepthStencil *ds = e->ds ? objc_host(e->ds) : NULL;
        if (ds && (ds->write || ds->compare != 7 ||
                   (ds->stencil_enabled && !(stencil_keeps(&ds->front) && stencil_keeps(&ds->back)))))
            return fold_no(9);
        /* its depth/stencil clear: nothing to do, or owed to the next pass loading them */
        MTexture *d = e->dtex ? e->dtex : e->stex;
        if ((e->clear_depth || e->clear_stencil) &&
            !(d->clear_valid && d->clear_value[0] == e->depth_value && d->clear_value[1] == (float)e->stencil_value)) {
            if (!e->clear_depth || !e->clear_stencil || e->dtex != e->stex || !d->is_rb || d->sampled)
                return fold_no(10);
            owe = d;
        }
    }
    if (nfolds == MAX_FOLDS)
        return fold_no(11);
    if (owe) {
        owe->owe_clear = 1;
        owe->owe_value[0] = e->depth_value;
        owe->owe_value[1] = (float)e->stencil_value;
        owe->clear_valid = 0;
    }
    fold_source_changes(x);
    if (x->read_alias)
        fold_drop(x);
    x->fold_stale = 0;
    x->clear_valid = 0;
    x->read_alias = t;
    folds[nfolds++] = x;
    e->folded = 1;
    e->ftex_folded = t;
    if (g_framelog)
        rt_log("[frame %d]   (folded: #%04x reads as #%04x)\n", g_framelog, (unsigned)(((uintptr_t)x >> 4) & 0xffff),
               (unsigned)(((uintptr_t)t >> 4) & 0xffff));
    return 1;
}

/* before a pass draws into its targets */
static void fold_pass_targets(REnc *e)
{
    for (int i = 0; i < MAX_COLOR; i++) {
        MTexture *x = e->color[i];
        if (!x)
            continue;
        fold_source_changes(x);
        if (x->read_alias) {
            if (e->folded && i == 0) {
                fold_drop(x);           /* more draws after the folded copy: drawn in the pass (pass_begin) */
            } else if (e->clear_mask & (1u << i)) {
                fold_drop(x);
            } else {
                fold_draw(x);
            }
        } else if (x->fold_stale) {
            if (e->clear_mask & (1u << i))
                x->fold_stale = 0;
            else
                fold_check_stale(x);
        }
        if (e->resolve[i])
            metal_fold_write(e->resolve[i]);
    }
}

/* before a draw: textures read whole are drawn now (colour-only reads take the
 * source in bind_tex); returns whether the framebuffer binding changed */
static int fold_draw_reads(REnc *e, MPipeline *p)
{
    int moved = 0;
    unsigned rgb = p->fs >= 0 ? g_shaders[p->fs].rgb_only : 0;
    g_rdbg.stage = 2;
    for (int k = 0; k < p->ntex; k++) {
        g_rdbg.k = k;
        int t = p->texb[k].tex & 15;
        uint64_t to = p->texb[k].stage ? e->ftex[t] : e->vtex[t];
        MTexture *mt = to ? objc_host(to) : NULL;
        if (!mt)
            continue;
        if (mt->read_alias && !(p->texb[k].stage && (rgb >> t & 1))) {
            fold_draw(mt);
            moved = 1;
        }
        fold_check_stale(mt);
    }
    return moved;
}
#else
void metal_fold_read(MTexture *t) { (void)t; }
void metal_fold_write(MTexture *t) { (void)t; }
void metal_fold_forget(MTexture *t) { (void)t; }
#endif

/* The pass's framebuffer, clears and recorded viewport/scissor: at its first
 * draw, or at its end when it only clears. */
static void pass_begin(REnc *e)
{
    GL_DIRTY();
    e->begun = 1;
    e->depth_stale = 1;
    e->depth_lost = 0;
#if defined(__vita__)
    fold_pass_targets(e);
#endif
#if defined(__vita__)
    {   /* a clear a folded pass owed: this pass does it if it loads them */
        MTexture *d = e->dtex ? e->dtex : e->stex;
        if (d && d->owe_clear) {
            d->owe_clear = 0;
            if (e->dtex && !e->clear_depth) {
                e->clear_depth = 1;
                e->depth_value = d->owe_value[0];
            }
            if (e->stex && !e->clear_stencil) {
                e->clear_stencil = 1;
                e->stencil_value = (int)d->owe_value[1];
            }
        }
    }
#endif
#if defined(__vita__)
    /* the drawable is the screen: its passes draw there, no copy at present */
    if (e->color[0] && e->color[0]->is_drawable && !e->dtex && !e->stex && direct_display()) {
        e->to_display = 1;
        e->fbo = 0;
        e->color[0]->on_display = 1;
        e->color[0]->alias = NULL;
    } else
#endif
    e->fbo = metal_pass_fbo(e->color, e->levels, e->dtex, e->stex);
#if defined(__vita__)
    {   /* depth/stencil come from memory only when the pass loads them */
        MTexture *ds = e->dtex ? e->dtex : e->stex;
        if (ds && ds->is_rb) {
            void vglRenderbufferForceLoad(GLuint rb, GLboolean load);   /* deps/vitaGL patch */
            vglRenderbufferForceLoad(ds->gl, !(e->clear_depth || !e->dtex) || !(e->clear_stencil || !e->stex));
            if (e->clear_depth || e->clear_stencil) {
                /* the pass's clear is the GXM's own: the values a scene starts with
                 * when the buffer is not loaded (vitaGL's clear quad sits at clip
                 * z = 2 * value - 1, which the GXM clips below 0: the game clears
                 * to 0 and tests GREATER, and its world drew nothing) */
                void vglRenderbufferClearValues(GLuint rb, GLfloat depth, GLuint stencil);   /* deps/vitaGL patch */
                void vglRestartScene(void);
                vglRenderbufferClearValues(ds->gl, e->clear_depth ? e->depth_value : 1.0f,
                                           e->clear_stencil ? (GLuint)e->stencil_value : 0);
                vglRestartScene();
            }
        }
    }
#endif
    glBindFramebuffer(GL_FRAMEBUFFER, e->fbo);
    glViewport(0, 0, (GLsizei)e->tw, (GLsizei)e->th);
    glDepthRange(0.0, 1.0);
#if defined(__vita__)
    /* a load action clears the whole attachment: no scissor (vitaGL would
     * clip the clear with the previous framebuffer's size) */
    glDisable(GL_SCISSOR_TEST);
#else
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, (GLsizei)e->tw, (GLsizei)e->th);
#endif
    glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    for (int i = 0; i < MAX_COLOR; i++) {
        if (e->color[i])
            e->color[i]->clear_valid = 0;        /* drawn from now on */
        if (!(e->clear_mask & (1u << i)))
            continue;
        glColorMaski((GLuint)i, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearBufferfv(GL_COLOR, i, e->clear_color[i]);
    }
    {   /* depth/stencil: known clear until a draw writes them (apply_state) */
        MTexture *d = e->dtex ? e->dtex : e->stex;
        if (d && e->clear_depth && e->clear_stencil && e->dtex == e->stex) {
            d->clear_valid = 1;
            d->clear_value[0] = e->depth_value;
            d->clear_value[1] = (float)e->stencil_value;
        } else if (d && (e->clear_depth || e->clear_stencil || e->dtex != e->stex)) {
            d->clear_valid = 0;
        }
    }
    if (e->clear_depth) {
        glDepthMask(GL_TRUE);
        glClearBufferfv(GL_DEPTH, 0, &e->depth_value);
    }
    if (e->clear_stencil) {
        glStencilMask(0xFF);
        glClearBufferiv(GL_STENCIL, 0, &e->stencil_value);
    }
#if defined(__vita__)
    glDepthRange(-1.0, 1.0);      /* the default Metal viewport (see apply_viewport) */
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, (GLsizei)e->tw, (GLsizei)e->th);
#endif
#if defined(__vita__)
    if (e->folded) {
        /* the copy folded at the pass's first draw, then more draws: it is drawn
         * first after all (over the clear: colour from the source, alpha 1) */
        MTexture *src = e->ftex_folded;
        e->folded = 0;
        g_copy_rgb = 1;
        vita_copy_quad(src, 0, 0, (float)src->width, (float)src->height, e->fbo, 0, 0, (GLsizei)e->tw,
                       (GLsizei)e->th, 0);
        g_copy_rgb = 0;
        glViewport(0, 0, (GLsizei)e->tw, (GLsizei)e->th);
        glDepthRange(-1.0, 1.0);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, (GLsizei)e->tw, (GLsizei)e->th);
    }
#endif
    if (e->vp_set)
        apply_viewport(e);
    if (e->sc_set)
        glScissor(e->sc[0], screen_y(e, e->sc[1], e->sc[3]), e->sc[2], e->sc[3]);
    gl_check("pass begin");
}

static int skipping(void);

/* An empty pass: its clears only matter if the targets do not already hold them. */
static void pass_end_empty(REnc *e)
{
    int needed = 0;
    for (int i = 0; i < MAX_COLOR; i++) {
        MTexture *t = e->color[i];
        if (t && (e->clear_mask & (1u << i)) &&
            !(t->clear_valid && memcmp(t->clear_value, e->clear_color[i], sizeof t->clear_value) == 0))
            needed = 1;
        if (e->resolve[i])
            needed = 1;
    }
    if (e->clear_depth || e->clear_stencil) {
        MTexture *d = e->dtex ? e->dtex : e->stex;
        if (d && !(d->clear_valid && d->clear_value[0] == e->depth_value &&
                   d->clear_value[1] == (float)e->stencil_value))
            needed = 1;
    }
    if (!needed && skipping())
        return;
    pass_begin(e);
    for (int i = 0; i < MAX_COLOR; i++)
        if (e->color[i] && (e->clear_mask & (1u << i))) {
            e->color[i]->clear_valid = 1;
            memcpy(e->color[i]->clear_value, e->clear_color[i], sizeof e->color[i]->clear_value);
        }
    MTexture *d = e->dtex ? e->dtex : e->stex;
    if (d && e->clear_depth && e->clear_stencil) {
        d->clear_valid = 1;
        d->clear_value[0] = e->depth_value;
        d->clear_value[1] = (float)e->stencil_value;
    }
}

uint64_t metal_render_encoder(Ctx *c, uint64_t pd)
{
    (void)c;
    uint64_t o = objc_new_object(&cls_RenderEnc);
    REnc *e = objc_host(o);
    uint64_t color[MAX_COLOR], depth, stencil;
    metal_pass_info(pd, color, &depth, &stencil);
    int levels[MAX_COLOR] = { 0 };
    for (int i = 0; i < MAX_COLOR; i++) {
        uint64_t t = color[i] ? pb_get(color[i], "texture", 0) : 0;
        if (t) {
            e->color_obj[i] = objc_retain_obj(t);
            e->color[i] = objc_host(t);
            levels[i] = (int)pb_get(color[i], "level", 0);
            uint64_t r = pb_get(color[i], "resolveTexture", 0);
            uint32_t store = (uint32_t)pb_get(color[i], "storeAction", 1);
            if (r && (store == 2 || store == 3)) {
                e->resolve_obj[i] = objc_retain_obj(r);
                e->resolve[i] = objc_host(r);
            }
        }
    }
    uint64_t dt = depth ? pb_get(depth, "texture", 0) : 0;
    uint64_t st = stencil ? pb_get(stencil, "texture", 0) : 0;
    MTexture *dtex = dt ? objc_host(dt) : NULL, *stex = st ? objc_host(st) : NULL;

    gpu_lock();
    MTexture *first = e->color[0] ? e->color[0] : (dtex ? dtex : stex);
    for (int i = 0; !first && i < MAX_COLOR; i++)
        first = e->color[i];
    e->tw = first ? first->width >> levels[0] : 1;
    e->th = first ? first->height >> levels[0] : 1;
    memcpy(e->levels, levels, sizeof e->levels);
    e->dtex = dtex;
    e->stex = stex;
    for (int i = 0; i < MAX_COLOR; i++) {
        if (!e->color[i] || pb_get(color[i], "loadAction", 0) != 2)
            continue;
        e->clear_mask |= 1u << i;
        for (int k = 0; k < 4; k++)
            e->clear_color[i][k] = (float)pb_getd(color[i], "clearColor", k, k == 3 ? 1.0 : 0.0);
    }
    if (dtex && pb_get(depth, "loadAction", 0) == 2) {
        e->clear_depth = 1;
        e->depth_value = (float)pb_getd(depth, "clearDepth", 0, 1.0);
    }
    if (stex && pb_get(stencil, "loadAction", 0) == 2) {
        e->clear_stencil = 1;
        e->stencil_value = (int)pb_get(stencil, "clearStencil", 0);
    }
    e->cull = 0;
    e->winding = 0;
    if (g_framelog)
        framelog_pass(e, color, depth, stencil);
    {
        static int show = -2;
        if (show == -2) {
            const char *v = getenv("MINA_SHOW_PASS");
            show = v ? atoi(v) : -1;
        }
        if (show >= 0 && g_pass_index++ == show && e->color[0])
            g_show_tex = e->color[0];
    }
    gl_check("pass begin");
    return o;
}

static void batch_flush(void);
static int batch_pending(void);
static void re_endEncoding(Ctx *c)
{
    batch_flush();
    GL_DIRTY();
    REnc *e = objc_host(SELF);
    if (e->ended)
        return;
    e->ended = 1;
    if (!e->begun && !e->folded)
        pass_end_empty(e);
    for (int i = 0; i < MAX_COLOR && e->begun; i++) {
        if (!e->resolve[i])
            continue;
#if defined(__vita__)
        if (e->color[i])
            vita_copy_quad(e->color[i], 0, 0, (float)e->tw, (float)e->th, metal_tex_fbo(e->resolve[i], 0), 0, 0,
                           (GLsizei)e->tw, (GLsizei)e->th, 0);
#else
        glBindFramebuffer(GL_READ_FRAMEBUFFER, e->fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0 + (GLenum)i);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, metal_tex_fbo(e->resolve[i], 0));
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0, 0, (GLint)e->tw, (GLint)e->th, 0, 0, (GLint)e->tw, (GLint)e->th,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
#endif
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gpu_unlock();
}

static void set_binding(Binding *b, uint64_t buf, uint32_t offset)
{
    if (b->obj != buf) {
        objc_retain_obj(buf);
        if (b->obj)
            objc_release_obj(b->obj);
        b->obj = buf;
        b->buf = buf ? objc_host(buf) : NULL;
    }
    b->nbytes = 0;
    b->offset = offset;
}
/* every draw sets some: the copy goes to a buffer kept for the next */
static void set_bytes(Binding *b, uint64_t src, uint32_t n)
{
    if (b->obj)
        objc_release_obj(b->obj);
    b->obj = 0;
    b->buf = NULL;
    b->offset = 0;
    if (n > b->cap) {
        free(b->bytes);
        b->cap = n < 4096 ? 4096 : n;
        b->bytes = malloc(b->cap);
    }
    memcpy(b->bytes, G(src), n);
    b->nbytes = n;
}
static const uint8_t *binding_data(const Binding *b, uint32_t *avail)
{
    if (b->nbytes) {
        *avail = b->nbytes;
        return b->bytes;
    }
    if (!b->buf || b->buf->gpu_only) {
        *avail = 0;
        return NULL;
    }
    *avail = b->buf->length > b->offset ? b->buf->length - b->offset : 0;
    return G(metal_buf_mem(b->buf) + b->offset);
}

static void re_setVertexBuffer(Ctx *c) { set_binding(&((REnc *)objc_host(SELF))->vb[MARG(2) % MAX_VBUF], MARG(0), (uint32_t)MARG(1)); }
static void re_setFragmentBuffer(Ctx *c) { set_binding(&((REnc *)objc_host(SELF))->fb[MARG(2) % MAX_VBUF], MARG(0), (uint32_t)MARG(1)); }
static void re_setVertexBufferOffset(Ctx *c) { ((REnc *)objc_host(SELF))->vb[MARG(1) % MAX_VBUF].offset = (uint32_t)MARG(0); }
static void re_setFragmentBufferOffset(Ctx *c) { ((REnc *)objc_host(SELF))->fb[MARG(1) % MAX_VBUF].offset = (uint32_t)MARG(0); }
static void re_setVertexBytes(Ctx *c) { set_bytes(&((REnc *)objc_host(SELF))->vb[MARG(2) % MAX_VBUF], MARG(0), (uint32_t)MARG(1)); }
static void re_setFragmentBytes(Ctx *c) { set_bytes(&((REnc *)objc_host(SELF))->fb[MARG(2) % MAX_VBUF], MARG(0), (uint32_t)MARG(1)); }

static void set_obj(uint64_t *slot, uint64_t v)
{
    if (*slot == v)
        return;
    objc_retain_obj(v);
    objc_release_obj(*slot);
    *slot = v;
}
static void re_setVertexTexture(Ctx *c) { set_obj(&((REnc *)objc_host(SELF))->vtex[MARG(1) & 15], MARG(0)); }
static void re_setFragmentTexture(Ctx *c) { set_obj(&((REnc *)objc_host(SELF))->ftex[MARG(1) & 15], MARG(0)); }
static void re_setVertexSampler(Ctx *c) { set_obj(&((REnc *)objc_host(SELF))->vsamp[MARG(1) & 15], MARG(0)); }
static void re_setFragmentSampler(Ctx *c) { set_obj(&((REnc *)objc_host(SELF))->fsamp[MARG(1) & 15], MARG(0)); }
static void re_setPipeline(Ctx *c) { set_obj(&((REnc *)objc_host(SELF))->pipeline, MARG(0)); }
static void re_setDepthStencil(Ctx *c) { set_obj(&((REnc *)objc_host(SELF))->ds, MARG(0)); }
static void re_setStencilRef(Ctx *c) { ((REnc *)objc_host(SELF))->stencil_ref = (uint32_t)MARG(0); }
static void re_setCull(Ctx *c) { ((REnc *)objc_host(SELF))->cull = (uint32_t)MARG(0); }
static void re_setWinding(Ctx *c) { ((REnc *)objc_host(SELF))->winding = (uint32_t)MARG(0); }

/* setViewport:(MTLViewport) — six doubles: not an HFA (more than four
 * members), so the 48-byte struct is passed by pointer */
static void re_setViewport(Ctx *c)
{
    uint64_t v = MARG(0);
    double x = u2d(rt_ld64(G(v))), y = u2d(rt_ld64(G(v + 8))), w = u2d(rt_ld64(G(v + 16)));
    double h = u2d(rt_ld64(G(v + 24))), zn = u2d(rt_ld64(G(v + 32))), zf = u2d(rt_ld64(G(v + 40)));
    REnc *e = objc_host(SELF);
    double vp[6] = { x, y, w, h, zn, zf };
    if (batch_pending() && e->vp_set && !memcmp(e->vp, vp, sizeof vp))
        return;                          /* (the same: a held batch stays) */
    batch_flush();
    memcpy(e->vp, vp, sizeof vp);
    e->vp_set = 1;
    if (e->begun)
        apply_viewport(e);
}
/* setScissorRect:(MTLScissorRect) — 32 bytes, passed by pointer */
static void re_setScissor(Ctx *c)
{
    uint64_t r = MARG(0);
    REnc *e = objc_host(SELF);
    int sc[4];
    for (int k = 0; k < 4; k++)
        sc[k] = (int)rt_ld64(G(r + 8 * k));
    if (batch_pending() && e->sc_set && !memcmp(e->sc, sc, sizeof sc))
        return;
    batch_flush();
    memcpy(e->sc, sc, sizeof sc);
    e->sc_set = 1;
    if (e->begun)
        glScissor(e->sc[0], screen_y(e, e->sc[1], e->sc[3]), e->sc[2], e->sc[3]);
}
/* kept with the encoder: set before its first draw, it was lost when the
 * pass began (which turns the offset off) */
static void re_setDepthBias(Ctx *c)
{
    REnc *e = objc_host(SELF);
    e->depth_bias = c->v[0].f[0];
    e->depth_slope = c->v[1].f[0];
}
static void re_nop(Ctx *c) { (void)c; }
static void re_label(Ctx *c) { RET(0); }

/* ---- state application ----------------------------------------------------------------------------- */

static GLenum gl_blend_factor(uint32_t f)
{
    static const GLenum t[] = {
        GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
        GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA_SATURATE,
        GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR, GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA,
    };
    return f < 15 ? t[f] : GL_ONE;
}
static GLenum gl_blend_op(uint32_t o)
{
    static const GLenum t[] = { GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX };
    return o < 5 ? t[o] : GL_FUNC_ADD;
}
static GLenum gl_stencil_op(uint32_t o)
{
    static const GLenum t[] = { GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP };
    return o < 8 ? t[o] : GL_KEEP;
}

/* The GL state draws set, as last set within this epoch (GL_DIRTY: other
 * code changed some): a draw sets only what differs from the draw before. */
uint32_t g_gl_epoch;
enum {
    S_POLY_ON, S_POLY, S_DEPTH_ON, S_DEPTH_FUNC, S_DEPTH_MASK, S_ST_ON, S_ST_FUNC_F, S_ST_FUNC_B, S_ST_OP_F,
    S_ST_OP_B, S_ST_MASK_F, S_ST_MASK_B, S_CULL_ON, S_CULL, S_FRONT, S_PROG, S_ACTIVE,
    S_BLEND_ON, S_BLEND_FUNC = S_BLEND_ON + MAX_COLOR, S_BLEND_EQ = S_BLEND_FUNC + MAX_COLOR,
    S_CMASK = S_BLEND_EQ + MAX_COLOR, S_TEX = S_CMASK + MAX_COLOR, S_SAMP = S_TEX + 16, S_COUNT = S_SAMP + 16
};
static uint64_t gs_v[S_COUNT];
static uint32_t gs_epoch = ~0u;
static int gs_off = -1;                /* MINA_NO_GLCACHE: every draw sets everything (to compare) */
static void gs_begin(void)
{
    if (gs_off < 0)
        gs_off = getenv("MINA_NO_GLCACHE") != NULL;
    if (gs_epoch != g_gl_epoch || gs_off) {
        memset(gs_v, 0xff, sizeof gs_v);      /* unknown: no value packs to all ones */
        gs_epoch = g_gl_epoch;
    }
}
/* 1 if the slot holds v already, else it does from now on */
static inline int gs_same(int slot, uint64_t v)
{
    if (gs_v[slot] == v)
        return 1;
    gs_v[slot] = v;
    return 0;
}
static void gs_cap(int slot, GLenum cap, int on)
{
    if (!gs_same(slot, (uint64_t)on)) {
        if (on)
            glEnable(cap);
        else
            glDisable(cap);
    }
}

static void apply_state(REnc *e, MPipeline *p)
{
    gs_begin();
    if (e->depth_bias != 0.0f || e->depth_slope != 0.0f) {
        gs_cap(S_POLY_ON, GL_POLYGON_OFFSET_FILL, 1);
        uint32_t a, b;
        memcpy(&a, &e->depth_slope, 4);
        memcpy(&b, &e->depth_bias, 4);
        if (!gs_same(S_POLY, (uint64_t)a << 32 | b))
            glPolygonOffset(e->depth_slope, e->depth_bias);
    } else {
        gs_cap(S_POLY_ON, GL_POLYGON_OFFSET_FILL, 0);
    }
    for (int i = 0; i < MAX_COLOR; i++) {
        if (!e->color[i])
            continue;
        const MBlend *b = &p->color[i];
        if (b->enabled) {
            if (!gs_same(S_BLEND_ON + i, 1))
                glEnablei(GL_BLEND, (GLuint)i);
            const GLenum f0 = gl_blend_factor(b->src_rgb), f1 = gl_blend_factor(b->dst_rgb),
                         f2 = gl_blend_factor(b->src_a), f3 = gl_blend_factor(b->dst_a);
            if (!gs_same(S_BLEND_FUNC + i, (uint64_t)(f0 & 0xffff) << 48 | (uint64_t)(f1 & 0xffff) << 32 |
                                               (uint64_t)(f2 & 0xffff) << 16 | (f3 & 0xffff)))
                glBlendFuncSeparatei((GLuint)i, f0, f1, f2, f3);
            const GLenum q0 = gl_blend_op(b->rgb_op), q1 = gl_blend_op(b->alpha_op);
            if (!gs_same(S_BLEND_EQ + i, (uint64_t)q0 << 32 | q1))
                glBlendEquationSeparatei((GLuint)i, q0, q1);
        } else if (!gs_same(S_BLEND_ON + i, 0)) {
            glDisablei(GL_BLEND, (GLuint)i);
        }
        if (!gs_same(S_CMASK + i, b->write_mask & 15))
            glColorMaski((GLuint)i, (b->write_mask & 8) != 0, (b->write_mask & 4) != 0, (b->write_mask & 2) != 0,
                         (b->write_mask & 1) != 0);
    }
    /* Metal ignores depth/stencil state in a pass without those attachments;
     * the Vita's screen has its own, so it must not be tested there */
    MDepthStencil *ds = e->ds && (e->dtex || e->stex) ? objc_host(e->ds) : NULL;
#if defined(__vita__)
    static int no_depth = -1, no_stencil = -1;
    if (no_depth < 0) {
        no_depth = getenv("MINA_NO_DEPTH") != NULL;
        no_stencil = getenv("MINA_NO_STENCIL") != NULL;
    }
    if (no_depth && ds && !no_stencil)
        ds = ds->stencil_enabled ? ds : NULL;
    if (no_depth && no_stencil)
        ds = NULL;
#endif
#if defined(__vita__)
    if (e->depth_lost && ds && ds->compare != 7 && p->dst_depth_unit < 0) {
        static int n;
        if (n++ < 8)
            rt_log("metal: fs %d tests the depth fs %d wrote (not kept on the Vita)\n", p->fs, e->depth_lost - 1);
    }
#endif
    if (ds && (ds->compare != 7 || ds->write)
#if defined(__vita__)
        && !no_depth
#endif
        ) {
        gs_cap(S_DEPTH_ON, GL_DEPTH_TEST, 1);
        const GLenum f = metal_gl_compare(ds->compare);
        if (!gs_same(S_DEPTH_FUNC, f))
            glDepthFunc(f);
        if (!gs_same(S_DEPTH_MASK, ds->write != 0))
            glDepthMask(ds->write ? GL_TRUE : GL_FALSE);
    } else {
        gs_cap(S_DEPTH_ON, GL_DEPTH_TEST, 0);
        if (!gs_same(S_DEPTH_MASK, 0))
            glDepthMask(GL_FALSE);
    }
    if (ds && (ds->write || (ds->stencil_enabled && !(stencil_keeps(&ds->front) && stencil_keeps(&ds->back))))) {
        MTexture *d = e->dtex ? e->dtex : e->stex;
        d->clear_valid = 0;             /* written from now on */
#if defined(__vita__)
        if (p->dst_depth_unit < 0)      /* depth_by_shader's draws write none */
            e->depth_stale = 1;
#endif
    }
    if (ds && ds->stencil_enabled
#if defined(__vita__)
        && !no_stencil
#endif
        ) {
        gs_cap(S_ST_ON, GL_STENCIL_TEST, 1);
        const GLenum ff = metal_gl_compare(ds->front.compare), fb = metal_gl_compare(ds->back.compare);
        if (!gs_same(S_ST_FUNC_F, (uint64_t)ds->front.read_mask << 32 | (e->stencil_ref & 0xffff) << 16 | (ff & 0xffff)))
            glStencilFuncSeparate(GL_FRONT, ff, (GLint)e->stencil_ref, ds->front.read_mask);
        if (!gs_same(S_ST_FUNC_B, (uint64_t)ds->back.read_mask << 32 | (e->stencil_ref & 0xffff) << 16 | (fb & 0xffff)))
            glStencilFuncSeparate(GL_BACK, fb, (GLint)e->stencil_ref, ds->back.read_mask);
        const GLenum o0 = gl_stencil_op(ds->front.fail), o1 = gl_stencil_op(ds->front.depth_fail),
                     o2 = gl_stencil_op(ds->front.pass);
        if (!gs_same(S_ST_OP_F, (uint64_t)(o0 & 0xffff) << 32 | (o1 & 0xffff) << 16 | (o2 & 0xffff)))
            glStencilOpSeparate(GL_FRONT, o0, o1, o2);
        const GLenum b0 = gl_stencil_op(ds->back.fail), b1 = gl_stencil_op(ds->back.depth_fail),
                     b2 = gl_stencil_op(ds->back.pass);
        if (!gs_same(S_ST_OP_B, (uint64_t)(b0 & 0xffff) << 32 | (b1 & 0xffff) << 16 | (b2 & 0xffff)))
            glStencilOpSeparate(GL_BACK, b0, b1, b2);
        if (!gs_same(S_ST_MASK_F, ds->front.write_mask))
            glStencilMaskSeparate(GL_FRONT, ds->front.write_mask);
        if (!gs_same(S_ST_MASK_B, ds->back.write_mask))
            glStencilMaskSeparate(GL_BACK, ds->back.write_mask);
    } else {
        gs_cap(S_ST_ON, GL_STENCIL_TEST, 0);
    }
    if (e->cull) {
        gs_cap(S_CULL_ON, GL_CULL_FACE, 1);
        if (!gs_same(S_CULL, e->cull == 1))
            glCullFace(e->cull == 1 ? GL_FRONT : GL_BACK);
    } else {
        gs_cap(S_CULL_ON, GL_CULL_FACE, 0);
    }
    /* the y flip mirrors the winding: Metal clockwise is GL counter-clockwise */
    const GLenum front = (e->winding == 0) != e->to_display ? GL_CCW : GL_CW;   /* y mirrored on the screen */
    if (!gs_same(S_FRONT, front))
        glFrontFace(front);
}

/* The values each uniform location was last given (vitaGL keeps them per
 * program, and a location names one uniform of one program): the same bytes
 * again are not sent - vitaGL copies them, looks the parameter's type up and
 * marks the program's uniforms for a whole copy at the next draw. */
typedef struct { GLint loc; uint32_t len, cap; uint8_t *data; } UniMemo;
static UniMemo uni_memo[1024];
static int uni_memo_same(GLint loc, const void *d, uint32_t len)
{
    uint32_t h = ((uint32_t)loc * 2654435761u) >> 22;
    for (int probe = 0; probe < 8; probe++, h = (h + 1) & 1023) {
        UniMemo *m = &uni_memo[h];
        if (m->loc == loc || m->loc == 0) {
            if (m->loc == loc && m->len == len && !memcmp(m->data, d, len))
                return 1;
            if (m->cap < len) {
                free(m->data);
                m->cap = len < 64 ? 64 : len;
                m->data = malloc(m->cap);
            }
            memcpy(m->data, d, len);
            m->len = len;
            m->loc = loc;
            return 0;
        }
    }
    return 0;
}
static void set_uniform1f(GLint loc, GLfloat v)
{
    if (!uni_memo_same(loc, &v, 4))
        glUniform1f(loc, v);
}
/* (no memo: a draw's constant buffers change from one draw to the next,
 * comparing them cost more than it saved) */
static void set_uniform4fv(GLint loc, GLsizei n, const GLfloat *d)
{
    glUniform4fv(loc, n, d);
}
static void apply_uniforms(REnc *e, MPipeline *p)
{
    for (int i = 0; i < MAX_VBUF; i++) {
        uint32_t avail;
        if (p->vcb[i] >= 0) {
            const uint8_t *d = binding_data(&e->vb[i], &avail);
            if (d) {
                GLsizei n = p->vcb_size[i];
                if ((uint32_t)n * 16 > avail) n = (GLsizei)(avail / 16);
                set_uniform4fv(p->vcb[i], n, (const GLfloat *)d);
            }
        }
        if (p->fcb[i] >= 0) {
            const uint8_t *d = binding_data(&e->fb[i], &avail);
            if (d) {
                GLsizei n = p->fcb_size[i];
                if ((uint32_t)n * 16 > avail) n = (GLsizei)(avail / 16);
                set_uniform4fv(p->fcb[i], n, (const GLfloat *)d);
            }
        }
    }
}

/* an object bound to a draw that is not of the class it is used as: one the
 * game released (its address reused by another object), logged with where
 * the game is */
static int bound_wrong(uint64_t a, const char *want, const char *what)
{
    Obj *o = objc_obj(a);
    if (o && !strcmp(o->cls->name, want))
        return 0;
    static int n;
    if (n++ < 12 || (n & 1023) == 0) {      /* the first ones, then one in 1024 (n counts them all) */
        char bt[512];
        guest_backtrace(bt, sizeof bt, 6);
        const char *objc_freed_info(uint64_t a, uint32_t *frames_ago);
        uint32_t ago = 0;
        const char *was = o ? NULL : objc_freed_info(a, &ago);
        rt_log("metal: the %s %#llx bound to a draw is %s%s%s (%u frames ago) [%d so far]: %s\n", what,
               (unsigned long long)a, o ? o->cls->name : "not a live object", was ? ": a freed " : "", was ? was : "",
               ago, n, bt);
    }
    return 1;
}

/* a sampler state's GL sampler (0, the texture's own state, for none) */
static GLuint sampler_gl(uint64_t samp)
{
    if (!samp)
        return 0;
    if (bound_wrong(samp, "MTLSamplerState_GL", "sampler"))
        return 0;
    return ((MSampler *)objc_host(samp))->gl;
}

/* a texture and sampler to a unit (only what differs from the unit's last) */
static void bind_unit(int unit, GLenum target, GLuint tex, int has_samp, GLuint samp)
{
    const int same_tex = gs_same(S_TEX + (unit & 15), (uint64_t)target << 32 | tex);
    const int same_samp = !has_samp || gs_same(S_SAMP + (unit & 15), samp);
    if (same_tex && same_samp)
        return;
    if (!gs_same(S_ACTIVE, (uint64_t)unit))
        glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    if (!same_tex)
        glBindTexture(target, tex);
    if (!same_samp)
        glBindSampler((GLuint)unit, samp);
}
static void bind_tex(int unit, uint64_t tex, uint64_t samp)
{
    if (!tex) {
        bind_unit(unit, GL_TEXTURE_2D, 0, 0, 0);
        return;
    }
    if (bound_wrong(tex, "MTLTexture_GL", "texture")) {
        bind_unit(unit, GL_TEXTURE_2D, 0, 0, 0);
        return;
    }
    MTexture *t = objc_host(tex);
#if defined(__vita__)
    if (t->read_alias)       /* a folded copy read for its colour: the source (fold_draw_reads) */
        t = t->read_alias;
    if (t->is_rb) {          /* a depth/stencil renderbuffer: its depth texture */
        t->sampled = 1;
        if (t->owe_clear)
            rt_log("metal: a depth buffer is sampled before the clear a folded pass owed it\n");
        bind_unit(unit, GL_TEXTURE_2D, t->depth_tex, 1, sampler_gl(samp));
        return;
    }
#endif
    bind_unit(unit, metal_tex_target(t), t->gl, 1, sampler_gl(samp));
}

static void apply_textures(REnc *e, MPipeline *p)
{
    for (int k = 0; k < p->ntex; k++) {
        int t = p->texb[k].tex & 15, s = p->texb[k].samp;
        uint64_t tex = p->texb[k].stage ? e->ftex[t] : e->vtex[t];
        if (p->texb[k].stage)
            bind_tex(p->texb[k].unit, tex, s >= 0 ? e->fsamp[s & 15] : 0);
        else
            bind_tex(p->texb[k].unit, tex, s >= 0 ? e->vsamp[s & 15] : 0);
        if (p->texb[k].size_loc >= 0 && tex) {
            MTexture *mt = objc_host(tex);
            glUniform2f(p->texb[k].size_loc, (GLfloat)mt->width, (GLfloat)mt->height);
        }
    }
}

typedef struct VFmt {
    GLint size;
    GLenum type;
    GLboolean norm;
    int integer;
} VFmt;

static int vertex_format(uint32_t f, VFmt *v)
{
    switch (f) {
    case 1: *v = (VFmt){ 2, GL_UNSIGNED_BYTE, 0, 0 }; return 1;
    case 2: *v = (VFmt){ 3, GL_UNSIGNED_BYTE, 0, 0 }; return 1;
    case 3: *v = (VFmt){ 4, GL_UNSIGNED_BYTE, 0, 0 }; return 1;
    case 7: *v = (VFmt){ 2, GL_UNSIGNED_BYTE, 1, 0 }; return 1;
    case 8: *v = (VFmt){ 3, GL_UNSIGNED_BYTE, 1, 0 }; return 1;
    case 9: *v = (VFmt){ 4, GL_UNSIGNED_BYTE, 1, 0 }; return 1;
    case 10: *v = (VFmt){ 2, GL_BYTE, 1, 0 }; return 1;
    case 12: *v = (VFmt){ 4, GL_BYTE, 1, 0 }; return 1;
    case 13: *v = (VFmt){ 2, GL_UNSIGNED_SHORT, 0, 0 }; return 1;
    case 15: *v = (VFmt){ 4, GL_UNSIGNED_SHORT, 0, 0 }; return 1;
    case 16: *v = (VFmt){ 2, GL_SHORT, 0, 0 }; return 1;
    case 18: *v = (VFmt){ 4, GL_SHORT, 0, 0 }; return 1;
    case 19: *v = (VFmt){ 2, GL_UNSIGNED_SHORT, 1, 0 }; return 1;
    case 21: *v = (VFmt){ 4, GL_UNSIGNED_SHORT, 1, 0 }; return 1;
    case 22: *v = (VFmt){ 2, GL_SHORT, 1, 0 }; return 1;
    case 24: *v = (VFmt){ 4, GL_SHORT, 1, 0 }; return 1;
    case 25: *v = (VFmt){ 2, GL_HALF_FLOAT, 0, 0 }; return 1;
    case 26: *v = (VFmt){ 3, GL_HALF_FLOAT, 0, 0 }; return 1;
    case 27: *v = (VFmt){ 4, GL_HALF_FLOAT, 0, 0 }; return 1;
    case 28: *v = (VFmt){ 1, GL_FLOAT, 0, 0 }; return 1;
    case 29: *v = (VFmt){ 2, GL_FLOAT, 0, 0 }; return 1;
    case 30: *v = (VFmt){ 3, GL_FLOAT, 0, 0 }; return 1;
    case 31: *v = (VFmt){ 4, GL_FLOAT, 0, 0 }; return 1;
    case 32: *v = (VFmt){ 1, GL_INT, 0, 1 }; return 1;
    case 33: *v = (VFmt){ 2, GL_INT, 0, 1 }; return 1;
    case 34: *v = (VFmt){ 3, GL_INT, 0, 1 }; return 1;
    case 35: *v = (VFmt){ 4, GL_INT, 0, 1 }; return 1;
    case 36: *v = (VFmt){ 1, GL_UNSIGNED_INT, 0, 1 }; return 1;
    case 37: *v = (VFmt){ 2, GL_UNSIGNED_INT, 0, 1 }; return 1;
    case 38: *v = (VFmt){ 3, GL_UNSIGNED_INT, 0, 1 }; return 1;
    case 39: *v = (VFmt){ 4, GL_UNSIGNED_INT, 0, 1 }; return 1;
#if defined(__vita__)
    /* vitaGL has no GL_BGRA size: the bytes are swapped at upload (apply_vertices) */
    case 42: *v = (VFmt){ 4, GL_UNSIGNED_BYTE, 1, 0 }; return 1;
#else
    case 42: *v = (VFmt){ GL_BGRA, GL_UNSIGNED_BYTE, 1, 0 }; return 1;
#endif
    case 45: *v = (VFmt){ 1, GL_UNSIGNED_BYTE, 0, 0 }; return 1;
    case 47: *v = (VFmt){ 1, GL_UNSIGNED_BYTE, 1, 0 }; return 1;
    case 49: *v = (VFmt){ 1, GL_UNSIGNED_SHORT, 0, 0 }; return 1;
    case 51: *v = (VFmt){ 1, GL_UNSIGNED_SHORT, 1, 0 }; return 1;
    case 53: *v = (VFmt){ 1, GL_HALF_FLOAT, 0, 0 }; return 1;
    }
    return 0;
}

/* Uploads what the draw reads of every vertex buffer and points the attributes at it. */
static void apply_vertices(REnc *e, MPipeline *p, uint32_t max_vertex, uint32_t instances)
{
    glBindVertexArray(p->vao);
    uint32_t base[MAX_VBUF];
    GLuint vbo[MAX_VBUF];
    uint32_t done = 0;
    for (int a = 0; a < MAX_ATTRS; a++) {
        if (!(p->attr_mask & (1u << a)))
            continue;
        const MVertexAttr *at = &p->attrs[a];
        uint32_t bi = at->buffer % MAX_VBUF;
        const MVertexLayout *lay = &p->layouts[bi];
        if (!(done & (1u << bi))) {
            const Binding *bd = &e->vb[bi];
            if (bd->buf && bd->buf->gpu_only) {
#if defined(__vita__)
                for (int a2 = 0; a2 < MAX_ATTRS; a2++)
                    if ((p->attr_mask & (1u << a2)) && p->attrs[a2].format == 42 && p->attrs[a2].buffer % MAX_VBUF == bi) {
                        static int told;
                        if (!told++)
                            rt_log("metal: BGRA vertex colors in a private buffer: drawn as RGBA\n");
                    }
#endif
                /* private buffer: read in place */
                vbo[bi] = bd->buf->gl;
                base[bi] = bd->offset;
                if (bd->buf->gl_size < bd->buf->length && max_vertex != UINT32_MAX &&
                    bd->offset + (uint64_t)lay->stride * (max_vertex + 1) > bd->buf->gl_size) {
                    static int told;
                    if (told++ < 4)
                        rt_log("metal: a draw reads %u vertices of %u bytes at %u of a private buffer held at %u bytes\n",
                               max_vertex + 1, lay->stride, bd->offset, bd->buf->gl_size);
                }
            } else {
                uint32_t avail;
                const uint8_t *d = binding_data(bd, &avail);
                uint32_t count = lay->step_fn == 2 ? (instances + lay->step_rate - 1) / (lay->step_rate ? lay->step_rate : 1)
                               : lay->step_fn == 0 ? 1 : max_vertex + 1;
                uint32_t need = lay->stride * count;
                if (max_vertex == UINT32_MAX || need > avail)
                    need = avail;
#if defined(__vita__)
                /* BGRA byte attributes in this buffer: R and B swapped in the copy */
                uint8_t *sw = NULL;
                for (int a2 = 0; a2 < MAX_ATTRS && d && need; a2++) {
                    const MVertexAttr *x = &p->attrs[a2];
                    if (!(p->attr_mask & (1u << a2)) || x->format != 42 || x->buffer % MAX_VBUF != bi)
                        continue;
                    if (!sw) {
                        sw = malloc(need);
                        memcpy(sw, d, need);
                    }
                    for (uint32_t o = x->offset; lay->stride && o + 4 <= need; o += lay->stride) {
                        const uint8_t t = sw[o];
                        sw[o] = sw[o + 2];
                        sw[o + 2] = t;
                    }
                }
                base[bi] = d && need ? ring_upload(sw ? sw : d, need) : 0;
                free(sw);
#else
                base[bi] = d && need ? ring_upload(d, need) : 0;
#endif
                vbo[bi] = ring;
            }
            done |= 1u << bi;
        }
        VFmt vf;
        if (!vertex_format(at->format, &vf)) {
            rt_log("metal: vertex format %u not supported\n", at->format);
            continue;
        }
        glBindBuffer(GL_ARRAY_BUFFER, vbo[bi]);
        glEnableVertexAttribArray((GLuint)a);
        const void *off = (const void *)(uintptr_t)(base[bi] + at->offset);
        if (vf.integer)
            glVertexAttribIPointer((GLuint)a, vf.size, vf.type, (GLsizei)lay->stride, off);
        else
            glVertexAttribPointer((GLuint)a, vf.size, vf.type, vf.norm, (GLsizei)lay->stride, off);
        if (lay->step_fn == 2)
            glVertexAttribDivisor((GLuint)a, lay->step_rate ? lay->step_rate : 1);
#if !defined(__vita__)       /* vitaGL: divisors are only set where instancing is used */
        else
            glVertexAttribDivisor((GLuint)a, 0);
#endif
    }
}

static GLenum gl_prim(uint32_t t)
{
    static const GLenum m[] = { GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP };
    return t < 5 ? m[t] : GL_TRIANGLES;
}

static void gl_check(const char *where)
{
    static int on = -1;
    if (on < 0)
        on = getenv("MINA_GLCHECK") != NULL;
    if (!on)
        return;
    GLenum err;
    static int reported;
    while ((err = glGetError()) != GL_NO_ERROR)
        if (reported++ < 40)
            rt_log("GL error %#x after %s\n", err, where);
}

/* A draw that changes nothing: a shader that discards where texture 0 is
 * transparent, reading a texture still cleared to transparent (an empty layer). */
static int no_skip = -1;      /* MINA_NO_SKIP: every pass and draw runs (to compare) */
static int skipping(void)
{
    if (no_skip < 0)
        no_skip = getenv("MINA_NO_SKIP") != NULL;
    return !no_skip;
}

static int draw_is_noop(REnc *e, MPipeline *p)
{
    if (!skipping() || p->fs < 0 || !g_shaders[p->fs].discards_clear0)
        return 0;
    for (int k = 0; k < p->ntex; k++)
        if (p->texb[k].stage == 1 && p->texb[k].tex == 0) {
            uint64_t to = e->ftex[0];
            MTexture *t = to ? objc_host(to) : NULL;
            return t && t->clear_valid && t->clear_value[3] == 0.0f;
        }
    return 0;
}

#if defined(__vita__)
uint32_t g_depth_fs_draws, g_depth_restarts;     /* MINA_FPS */
/* A fragment shader that writes its depth (a layer composited with the depth
 * it was drawn with: the logo, the world's layers) tests it itself, against
 * the target's depth sampled as a texture: the scene drawn so far ends (its
 * depth stored) and another begins on the same target, loading it. The depth
 * it would write is not: later depth-tested draws of the pass see the old one. */
static void depth_by_shader(REnc *e, MPipeline *p)
{
    GL_DIRTY();
    static int no_depth = -1, off;
    if (no_depth < 0) {
        no_depth = getenv("MINA_NO_DEPTH") != NULL;
        off = getenv("MINA_NO_DEPTH_FS") != NULL;   /* the fixed-function test with the quad's depth, as before */
    }
    MDepthStencil *ds = e->ds && e->dtex ? objc_host(e->ds) : NULL;
    int cmp = ds && !no_depth && !off && e->dtex->is_rb ? (int)ds->compare : 7;
    if (cmp != 7) {
        MTexture *d = e->dtex;
        g_depth_fs_draws++;
        if (!d->depth_tex) {         /* a depth buffer never sampled before */
            void vglTexImageRenderbufferDepth(GLuint rb, GLsizei width, GLsizei height);   /* deps/vitaGL patch */
            glActiveTexture(GL_TEXTURE0 + (GLenum)p->dst_depth_unit);
            glGenTextures(1, &d->depth_tex);
            glBindTexture(GL_TEXTURE_2D, d->depth_tex);
            vglTexImageRenderbufferDepth(d->gl, (GLsizei)d->width, (GLsizei)d->height);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        void vglRenderbufferForceLoad(GLuint rb, GLboolean load);   /* deps/vitaGL patch */
        void vglRestartScene(void);
        if (e->depth_stale) {        /* not in memory since the last restart */
            vglRenderbufferForceLoad(d->gl, GL_TRUE);
            vglRestartScene();
            e->depth_stale = 0;
            g_depth_restarts++;
        }
        d->sampled = 1;
        glActiveTexture(GL_TEXTURE0 + (GLenum)p->dst_depth_unit);
        glBindTexture(GL_TEXTURE_2D, d->depth_tex);
        glBindSampler((GLuint)p->dst_depth_unit, 0);
        static int flip = -1;
        if (flip < 0)
            flip = getenv("MINA_DST_FLIP") != NULL;
        float w = 1.0f / (float)d->width, h = 1.0f / (float)d->height;
        glUniform4f(p->dst_xform_loc, w, flip ? -h : h, 0.0f, flip ? 1.0f : 0.0f);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        if (ds->write)
            e->depth_lost = p->fs + 1;
    }
    glUniform1f(p->depth_cmp_loc, (float)cmp);
}
#endif

#if defined(__vita__)
/* A pass whose first draw copies a depth buffer into its own (a fragment
 * shader writing the depth it samples: the world's layers composited, then the
 * sprites tested against them): the Vita's shaders cannot write depth, so the
 * pass uses that depth buffer itself, loaded (its stencil still cleared as the
 * pass asks), and the copy writes none. Without it the sprites were tested
 * against a cleared depth: Mina drawn over the bridge she walks under. */
static void depth_alias(REnc *e, MPipeline *p)
{
    static int off = -1;     /* MINA_DEPTH_ALIAS=1: on (Vita3K ran at 0.7 fps with it: being looked at) */
    if (off < 0)
        off = getenv("MINA_DEPTH_ALIAS") == NULL;
    if (off || p->fs < 0 || !g_shaders[p->fs].writes_depth || !e->dtex || !e->dtex->is_rb || !e->clear_depth)
        return;
    MDepthStencil *ds = e->ds ? objc_host(e->ds) : NULL;
    if (!ds || ds->compare != 7 || !ds->write)
        return;
    for (int k = 0; k < p->ntex; k++) {
        if (!p->texb[k].stage)
            continue;
        const uint64_t to = e->ftex[p->texb[k].tex & 15];
        MTexture *src = to ? objc_host(to) : NULL;
        if (!src || !src->is_rb || src == e->dtex || src->width != e->dtex->width || src->height != e->dtex->height)
            continue;
        if (e->stex == e->dtex)
            e->stex = src;
        e->dtex = src;
        e->clear_depth = 0;
        e->depth_aliased = 1;
        return;
    }
}
#endif

/* force: the pipeline to draw with instead of the bound one (its batching variant) */
static MPipeline *draw_setup_p(REnc *e, MPipeline *force)
{
    g_rdbg.stage = 1;
    if (!e->pipeline)
        return NULL;
    MPipeline *p = force ? force : (MPipeline *)objc_host(e->pipeline);
    g_rdbg.pipe = p;
    g_rdbg.vs = p->vs;
    g_rdbg.fs = p->fs;
    g_rdbg.ntex = p->ntex;
    g_rdbg.draws++;
    if (draw_is_noop(e, p))
        return NULL;
#if defined(__vita__)
    if (fold_draw_reads(e, p) && e->begun) {
        /* back to this pass (its depth/stencil kept) */
        MTexture *ds = e->dtex ? e->dtex : e->stex;
        if (ds && ds->is_rb) {
            void vglRenderbufferForceLoad(GLuint rb, GLboolean load);
            vglRenderbufferForceLoad(ds->gl, GL_TRUE);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, e->fbo);
        glViewport(0, 0, (GLsizei)e->tw, (GLsizei)e->th);
        glDepthRange(-1.0, 1.0);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, (GLsizei)e->tw, (GLsizei)e->th);
        if (e->vp_set)
            apply_viewport(e);
        if (e->sc_set)
            glScissor(e->sc[0], screen_y(e, e->sc[1], e->sc[3]), e->sc[2], e->sc[3]);
    }
#endif
#if defined(__vita__)
    if (!e->begun)
        depth_alias(e, p);
#endif
    g_rdbg.stage = 3;
    if (!e->begun)
        pass_begin(e);
    g_rdbg.stage = 4;
    {   /* MINA_SKIP_FS=n,m: draws with these fragment shaders are skipped (debug) */
        static int init, skip[8], nskip;
        if (!init) {
            init = 1;
            const char *v = getenv("MINA_SKIP_FS");
            while (v && *v && nskip < 8) {
                skip[nskip++] = atoi(v);
                v = strchr(v, ',');
                if (v)
                    v++;
            }
        }
        for (int i = 0; i < nskip; i++)
            if (p->fs == skip[i])
                return NULL;
    }
    {   /* MINA_SHOW_FS=n: the first texture that fragment shader reads goes to the screen */
        static int show_fs = -2;
        if (show_fs == -2) {
            const char *v = getenv("MINA_SHOW_FS");
            show_fs = v ? atoi(v) : -1;
        }
        if (p->fs == show_fs && p->ntex) {
            int t = p->texb[0].tex & 15;
            uint64_t to = p->texb[0].stage ? e->ftex[t] : e->vtex[t];
            if (to)
                g_show_tex = objc_host(to);
        }
    }
    static int drawlog = -1, display_log;  /* looked up once: getenv walks the environment */
    if (drawlog < 0) {
        display_log = getenv("MINA_DISPLAY_LOG") != NULL;
        drawlog = getenv("MINA_DRAWLOG") != NULL;
    }
#if defined(__vita__)
    if (drawlog) {
        static uint8_t seen[256][256];
        if (p->vs >= 0 && p->vs < 256 && p->fs >= 0 && p->fs < 256 && !seen[p->vs][p->fs]++)
            rt_log("[draw] first draw with vs %d fs %d (%d textures) prog %u\n", p->vs, p->fs, p->ntex, p->prog);
    }
#endif
    if (e->to_display && display_log) {
        static int n;
        if (n++ < 6)
            rt_log("[display] fs %d cull %u winding %u vp_set %d vp %g %g %g %g sc_set %d sc %d %d %d %d yflip_loc %d\n",
                   p->fs, e->cull, e->winding, e->vp_set, e->vp[0], e->vp[1], e->vp[2], e->vp[3], e->sc_set, e->sc[0],
                   e->sc[1], e->sc[2], e->sc[3], p->yflip_loc);
    }
    gs_begin();
    if (!gs_same(S_PROG, p->prog))
        glUseProgram(p->prog);
    if (p->yflip_loc >= 0)
        set_uniform1f(p->yflip_loc, e->to_display ? 1.0f : -1.0f);
#if defined(__vita__)
    if (drawlog) {
        GLint cur = -1;
        glGetIntegerv(GL_CURRENT_PROGRAM, &cur);
        static int n;
        if (cur != (GLint)p->prog && n++ < 10)
            rt_log("[draw] glUseProgram(%u) left the current program at %d (vs %d fs %d)\n", p->prog, cur, p->vs, p->fs);
    }
#endif
    g_rdbg.stage = 5;
    apply_state(e, p);
#if defined(__vita__)
    if (e->depth_aliased && p->fs >= 0 && g_shaders[p->fs].writes_depth) {      /* the copy of the depth the pass already has */
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        GL_DIRTY();
    }
#endif
    g_rdbg.stage = 6;
    apply_uniforms(e, p);
    g_rdbg.stage = 7;
    apply_textures(e, p);
    g_rdbg.stage = 8;
#if defined(__vita__)
    if (p->dst_depth_unit >= 0)
        depth_by_shader(e, p);
#endif
    g_rdbg.stage = 9;
    return p;
}
static MPipeline *draw_setup(REnc *e) { return draw_setup_p(e, NULL); }

static const uint8_t *binding_data(const Binding *b, uint32_t *avail);
static void framelog_draw(REnc *e, MPipeline *p, const char *kind, uint32_t count)
{
    char buf[512];
    int n = snprintf(buf, sizeof buf, "[frame %d]   %s %u vs %d fs %d", g_framelog, kind, count, p->vs, p->fs);
    for (int k = 0; k < p->ntex && n < (int)sizeof buf; k++) {
        int t = p->texb[k].tex & 15;
        uint64_t to = p->texb[k].stage ? e->ftex[t] : e->vtex[t];
        MTexture *mt = to ? objc_host(to) : NULL;
        n += snprintf(buf + n, sizeof buf - n, " t%d:%ux%u/%u%s#%04x", t, mt ? mt->width : 0, mt ? mt->height : 0,
                      mt ? mt->format : 0, mt && mt->is_rb ? "rb" : "", (unsigned)(((uintptr_t)mt >> 4) & 0xffff));
    }
    MDepthStencil *ds = e->ds ? objc_host(e->ds) : NULL;
    if (ds)
        n += snprintf(buf + n, sizeof buf - n, " depth cmp %u w %d stencil %d (cmp %u ref %u pass %u)", ds->compare,
                      ds->write, ds->stencil_enabled, ds->front.compare, e->stencil_ref, ds->front.pass);
    if (p->color[0].enabled && n < (int)sizeof buf)
        n += snprintf(buf + n, sizeof buf - n, " blend %u/%u", p->color[0].src_rgb, p->color[0].dst_rgb);
    if (n < (int)sizeof buf)
        n += snprintf(buf + n, sizeof buf - n, " mask %#x", p->color[0].write_mask);
    if (e->vp_set && n < (int)sizeof buf)
        n += snprintf(buf + n, sizeof buf - n, " vp %g %g %gx%g", e->vp[0], e->vp[1], e->vp[2], e->vp[3]);
    if (e->sc_set && n < (int)sizeof buf)
        snprintf(buf + n, sizeof buf - n, " sc %d %d %dx%d", e->sc[0], e->sc[1], e->sc[2], e->sc[3]);
    rt_log("%s\n", buf);
    /* MINA_FRAMELOG_FS=n: the fragment constant buffers of that shader too */
    static int dump_fs = -2;
    if (dump_fs == -2) {
        const char *v = getenv("MINA_FRAMELOG_FS");
        dump_fs = v ? atoi(v) : -1;
    }
    /* MINA_FRAMELOG_VS=n: that vertex shader's constant buffers and vertex bytes */
    static int dump_vs = -2;
    if (dump_vs == -2) {
        const char *v = getenv("MINA_FRAMELOG_VS");
        dump_vs = v ? atoi(v) : -1;
    }
    if (p->vs == dump_vs) {
        for (int i = 0; i < MAX_VBUF; i++) {
            uint32_t avail = 0;
            const uint8_t *d = binding_data(&e->vb[i], &avail);
            if (e->vb[i].buf && (!d || !avail)) {
                const MBuffer *b = e->vb[i].buf;
                rt_log("[frame %d]     vb%d: buffer %#llx at %u (length %u, gpu_only %d, shadow %d)\n", g_framelog, i,
                       (unsigned long long)e->vb[i].obj, e->vb[i].offset, b->length, b->gpu_only, b->shadow != NULL);
            }
            if (!d || !avail)
                continue;
            char line[1024];
            int m = snprintf(line, sizeof line, "[frame %d]     vb%d(%u%s):", g_framelog, i, avail,
                             e->vb[i].buf && e->vb[i].buf->gl ? " private" : "");
            for (uint32_t k = 0; k + 4 <= avail && k < 40 * 4 && m < (int)sizeof line; k += 4) {
                float f;
                memcpy(&f, d + k, 4);
                m += snprintf(line + m, sizeof line - m, " %g", f);
            }
            rt_log("%s\n", line);
        }
    }
    if (p->fs == dump_fs) {
        for (int i = 0; i < MAX_VBUF; i++) {
            uint32_t avail = 0;
            const uint8_t *d = binding_data(&e->fb[i], &avail);
            if (!d || !avail)
                continue;
            char line[512];
            int m = snprintf(line, sizeof line, "[frame %d]     fcb%d:", g_framelog, i);
            for (uint32_t k = 0; k + 4 <= avail && k < 24 * 4 && m < (int)sizeof line; k += 4) {
                float f;
                memcpy(&f, d + k, 4);
                m += snprintf(line + m, sizeof line - m, " %g", f);
            }
            rt_log("%s\n", line);
        }
    }
}

#if defined(__vita__)
/* ---- draw batching -------------------------------------------------------------------------
 * The game draws a sprite (a glyph and each of its outline copies, a tile of a
 * dialog box's frame, a particle) per draw call: a quad of its own vertices,
 * its transform in vcb1, everything else as in the draw before. With text on
 * the screen that is ~700 draws a frame at ~45 us each on the Vita. Consecutive
 * draws whose state is the same (pipeline, depth/stencil, viewport, scissor,
 * textures and samplers, the bytes of every other constant buffer) go to the
 * GPU as one: their triangles in their order, each vertex followed by its
 * draw's vcb1[0..3], drawn by the pipeline's batching variant (the same shaders
 * with vcb1 read from attributes 12..15: metal_batch_pipeline). The same
 * triangles in the same order through the same arithmetic. MINA_NO_BATCH: off. */
#define BATCH_UB 2048
typedef struct BKey {
    REnc *e;
    uint64_t pipeline, ds;
    uint32_t stencil_ref, cull, winding;
    int vp_set, sc_set, to_display;
    double vp[6];
    int sc[4];
    float depth_bias, depth_slope;
    uint64_t tex[8], samp[8];
} BKey;
static struct {
    int n;                       /* draws held */
    BKey key;
    MPipeline *p, *bp;
    uint32_t vbi, vstride, voff, stride;
    uint8_t ub[BATCH_UB];        /* the other constant buffers' bytes */
    uint32_t ub_len;
    uint8_t *v;                  /* vertices: each one's bytes, then vcb1[0..3] */
    uint32_t nv, cap;
    uint64_t held[34];           /* objects kept alive while held (a texture released between draws) */
    int nheld;
} B;
static int batch_on = -1;
uint32_t g_batched_draws, g_batches;
uint32_t g_batch_why[8];     /* a held batch not joined: key, constants, vertices, no index data, encoder, pipeline */

static void batch_key(BKey *k, REnc *e, MPipeline *p)
{
    memset(k, 0, sizeof *k);
    k->e = e;
    k->pipeline = e->pipeline;
    k->ds = e->ds;
    k->stencil_ref = e->stencil_ref;
    k->cull = e->cull;
    k->winding = e->winding;
    k->vp_set = e->vp_set;
    k->sc_set = e->sc_set;
    k->to_display = e->to_display;
    memcpy(k->vp, e->vp, sizeof k->vp);
    memcpy(k->sc, e->sc, sizeof k->sc);
    k->depth_bias = e->depth_bias;
    k->depth_slope = e->depth_slope;
    for (int i = 0; i < p->ntex && i < 8; i++) {
        const int t = p->texb[i].tex & 15, sm = p->texb[i].samp;
        k->tex[i] = p->texb[i].stage ? e->ftex[t] : e->vtex[t];
        k->samp[i] = sm < 0 ? 0 : p->texb[i].stage ? e->fsamp[sm & 15] : e->vsamp[sm & 15];
    }
}
static int key_same(const BKey *a, const BKey *b)
{
    const uint64_t *x = (const uint64_t *)(const void *)a, *y = (const uint64_t *)(const void *)b;
    for (size_t i = 0; i < sizeof *a / 8; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

/* The rows of every constant buffer but vcb1 the shaders read, as
 * apply_uniforms would send them: into out (0: a row read is missing, so a
 * draw would read an older value), or compared with B.ub (cmp). */
/* what batch_ub reads, made once per variant: for each constant buffer the
 * shaders read (but vcb1), the bytes the draw must have sent, and the runs of
 * 4-byte words read */
typedef struct BPlan {
    int nbuf, nrun, ok;
    struct { uint8_t st, idx; uint16_t need, run0, nrun; } buf[16];
    struct { uint16_t off, words; } run[96];
} BPlan;
static BPlan *batch_plan(MPipeline *p, MPipeline *bp)
{
    if (bp->bplan)
        return bp->bplan;
    BPlan *pl = calloc(1, sizeof *pl);
    pl->ok = 1;
    for (int st = 0; st < 2 && pl->ok; st++)
        for (int i = 0; i < MAX_VBUF && pl->ok; i++) {
            const GLint loc = st ? p->fcb[i] : p->vcb[i];
            if (loc < 0 || (!st && i == 1))
                continue;
            const uint8_t *rows = st ? bp->bmask->f[i] : bp->bmask->v[i];
            const uint32_t size = (uint32_t)(st ? p->fcb_size[i] : p->vcb_size[i]);
            int first = 1;
            for (uint32_t w = 0; w < 32 * 4 && w / 4 < size; w++) {
                if (!(rows[w / 4] & (1u << (w & 3))))
                    continue;
                if (first) {
                    if (pl->nbuf == 16) {
                        pl->ok = 0;
                        break;
                    }
                    pl->buf[pl->nbuf].st = (uint8_t)st;
                    pl->buf[pl->nbuf].idx = (uint8_t)i;
                    pl->buf[pl->nbuf].run0 = (uint16_t)pl->nrun;
                    pl->nbuf++;
                    first = 0;
                }
                pl->buf[pl->nbuf - 1].need = (uint16_t)((w / 4 + 1) * 16);
                if (pl->nrun && pl->buf[pl->nbuf - 1].nrun &&
                    pl->run[pl->nrun - 1].off + 4 * pl->run[pl->nrun - 1].words == 4 * w)
                    pl->run[pl->nrun - 1].words++;
                else if (pl->nrun == 96)
                    pl->ok = 0;
                else {
                    pl->run[pl->nrun].off = (uint16_t)(4 * w);
                    pl->run[pl->nrun].words = 1;
                    pl->nrun++;
                    pl->buf[pl->nbuf - 1].nrun++;
                }
            }
        }
    bp->bplan = pl;
    return pl;
}
/* The words of every constant buffer but vcb1 the shaders read, as
 * apply_uniforms sent them: into out (0: one read was not sent, so the draw
 * would read an older value), or compared with B.ub (cmp) */
static uint32_t batch_ub(REnc *e, MPipeline *p, MPipeline *bp, uint8_t *out, int cmp)
{
    const BPlan *pl = batch_plan(p, bp);
    if (!pl->ok)
        return 0;
    uint32_t n = 0;
    for (int b = 0; b < pl->nbuf; b++) {
        const int st = pl->buf[b].st, i = pl->buf[b].idx;
        uint32_t avail;
        const uint8_t *d = binding_data(st ? &e->fb[i] : &e->vb[i], &avail);
        if (!d || avail < pl->buf[b].need)
            return 0;
        for (int r = pl->buf[b].run0; r < pl->buf[b].run0 + pl->buf[b].nrun; r++) {
            const uint32_t len = 4u * pl->run[r].words;
            if (n + len > BATCH_UB)
                return 0;
            if (cmp) {
                const uint8_t *x = B.ub + n, *y = d + pl->run[r].off;
                for (uint32_t k = 0; k < len; k += 4)
                    if (rt_ld32(x + k) != rt_ld32(y + k))
                        return 0;
            } else
                memcpy(out + n, d + pl->run[r].off, len);
            n += len;
        }
    }
    return n ? n : 1;
}
static int batch_ub_same(REnc *e, MPipeline *p) { return batch_ub(e, p, B.bp, NULL, 1) != 0; }

/* every attribute from one buffer, per vertex, float formats */
static int batch_layout(MPipeline *p, uint32_t *vbi)
{
    int bi = -1;
    for (int a = 0; a < MAX_ATTRS; a++) {
        if (!(p->attr_mask & (1u << a)))
            continue;
        const int b = (int)(p->attrs[a].buffer % MAX_VBUF);
        if (bi >= 0 && b != bi)
            return 0;
        bi = b;
        VFmt vf;
        if (p->attrs[a].format == 42 || !vertex_format(p->attrs[a].format, &vf) || vf.integer)
            return 0;
    }
    if (bi < 0 || bi == 1 || p->vcb[bi] >= 0)
        return 0;
    const MVertexLayout *lay = &p->layouts[bi];
    if (lay->step_fn != 1 || !lay->stride || lay->stride > 64)
        return 0;
    *vbi = (uint32_t)bi;
    return 1;
}

static const uint8_t *batch_vdata(const Binding *bd, uint32_t *avail)
{
    const uint8_t *d = binding_data(bd, avail);
    if (d)
        return d;
    if (bd->buf && bd->buf->gpu_only && bd->buf->shadow && bd->buf->length > bd->offset) {
        *avail = bd->buf->length - bd->offset;
        return bd->buf->shadow + bd->offset;
    }
    return NULL;
}

static int batch_append(REnc *e, const uint8_t *idx, uint32_t count, uint32_t type)
{
    uint32_t avail, mavail;
    const uint8_t *vd = batch_vdata(&e->vb[B.vbi], &avail);
    const uint8_t *m = binding_data(&e->vb[1], &mavail);
    if (!vd || !m || mavail < 64)
        return 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t v = type ? rt_ld32(idx + 4 * i) : rt_ld16(idx + 2 * i);
        if ((uint64_t)(v + 1) * B.vstride > avail)
            return 0;
    }
    const uint32_t need = (B.nv + count) * B.stride;
    if (need > B.cap) {
        B.cap = need * 2 > 65536 ? need * 2 : 65536;
        B.v = realloc(B.v, B.cap);
    }
    uint8_t *o = B.v + B.nv * B.stride;
    for (uint32_t i = 0; i < count; i++, o += B.stride) {
        const uint32_t v = type ? rt_ld32(idx + 4 * i) : rt_ld16(idx + 2 * i);
        memcpy(o, vd + v * B.vstride, B.vstride);
        memcpy(o + B.voff, m, 64);
    }
    B.nv += count;
    return 1;
}

static void batch_flush(void)
{
    if (!B.n)
        return;
    MPipeline *bp = B.bp;
    glBindVertexArray(bp->vao);
    /* from memory (vitaGL copies the vertices to its frame's GPU memory): from
     * a buffer, its "packed" attributes keep their offset in the buffer in 16
     * bits (the ring's go past 64 KiB) */
    const uintptr_t pos = (uintptr_t)B.v;
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    for (int a = 0; a < MAX_ATTRS; a++) {
        if (!(bp->attr_mask & (1u << a)))
            continue;
        const MVertexAttr *at = &bp->attrs[a];
        VFmt vf;
        vertex_format(at->format, &vf);
        glEnableVertexAttribArray((GLuint)a);
        glVertexAttribPointer((GLuint)a, vf.size, vf.type, vf.norm, (GLsizei)B.stride,
                              (const void *)(pos + at->offset));
    }
    for (int k = 0; k < bp->batch_rows; k++) {
        glEnableVertexAttribArray((GLuint)(12 + k));
        glVertexAttribPointer((GLuint)(12 + k), 4, GL_FLOAT, GL_FALSE, (GLsizei)B.stride,
                              (const void *)(pos + B.voff + 16 * (uint32_t)k));
    }
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)B.nv);
    gl_check("batch");
    g_batches++;
    g_batched_draws += (uint32_t)B.n;
    B.n = 0;
    B.nv = 0;
    for (int i = 0; i < B.nheld; i++)
        objc_release_obj(B.held[i]);
    B.nheld = 0;
}

static int batch_pending(void) { return B.n; }
void metal_batch_forget(MPipeline *bp)
{
    if (B.n && B.bp == bp)
        batch_flush();
}

/* drawIndexedPrimitives through the batch: 1 when it was taken (held, or
 * nothing to draw); otherwise what is held is drawn and the caller draws it */
static int batch_draw(Ctx *c, REnc *e, uint32_t instances)
{
    if (batch_on < 0)
        batch_on = getenv("MINA_NO_BATCH") == NULL;
    if (!batch_on || g_framelog || !e->pipeline) {
        batch_flush();
        return 0;
    }
    const uint32_t prim = (uint32_t)MARG(0), count = (uint32_t)MARG(1), type = (uint32_t)MARG(2);
    const uint32_t off = (uint32_t)MARG(4);
    MBuffer *ib = MARG(3) ? objc_host(MARG(3)) : NULL;
    MPipeline *p = objc_host(e->pipeline);
    const uint32_t isz = type ? 4 : 2;
    const uint8_t *idx = NULL;
    if (prim == 3 && instances == 1 && ib && count && count % 3 == 0 && count <= 96 && e->begun && !e->folded)
        idx = ib->gpu_only ? (ib->shadow && off + count * isz <= ib->length ? ib->shadow + off : NULL)
                           : G(metal_buf_mem(ib) + off);
    if (idx && B.n && B.key.e == e && B.p == p) {
        BKey k;
        batch_key(&k, e, p);
        if (key_same(&k, &B.key) && batch_ub_same(e, p) && batch_append(e, idx, count, type)) {
            B.n++;
            return 1;
        }
        g_batch_why[key_same(&k, &B.key) ? (batch_ub_same(e, p) ? 2 : 1) : 0]++;
    } else if (B.n) {
        g_batch_why[!idx ? 3 : B.key.e != e ? 4 : 5]++;
    }
    batch_flush();
    if (!idx)
        return 0;
    uint32_t vbi;
    if (p->ntex > 8 || !batch_layout(p, &vbi))
        return 0;
    MPipeline *bp = metal_batch_pipeline(p);
    if (!bp)
        return 0;
    const uint32_t ub = batch_ub(e, p, bp, B.ub, 0);
    if (!ub)
        return 0;
    B.ub_len = ub;
    B.p = p;
    B.bp = bp;
    B.vbi = vbi;
    B.vstride = p->layouts[vbi].stride;
    B.voff = (B.vstride + 3) & ~3u;
    B.stride = B.voff + 64;
    B.nv = 0;
    if (!batch_append(e, idx, count, type))
        return 0;
    if (!draw_setup_p(e, bp)) {          /* nothing to draw (as for the draw itself) */
        B.nv = 0;
        return 1;
    }
    batch_key(&B.key, e, p);
    B.n = 1;
    B.nheld = 0;
    B.held[B.nheld++] = objc_retain_obj(e->pipeline);
    if (e->ds)
        B.held[B.nheld++] = objc_retain_obj(e->ds);
    for (int i = 0; i < p->ntex && i < 8; i++) {
        if (B.key.tex[i])
            B.held[B.nheld++] = objc_retain_obj(B.key.tex[i]);
        if (B.key.samp[i] && B.nheld < 34)
            B.held[B.nheld++] = objc_retain_obj(B.key.samp[i]);
        if (B.nheld >= 33)
            break;
    }
    return 1;
}
#else
static void batch_flush(void) {}
static int batch_pending(void) { return 0; }
#endif

/* drawPrimitives:vertexStart:vertexCount:[instanceCount:] */
static void draw_arrays(Ctx *c, uint32_t instances)
{
    REnc *e = objc_host(SELF);
    batch_flush();
#if defined(__vita__)
    if (e->pipeline && fold_pass(e, objc_host(e->pipeline), (uint32_t)MARG(2), instances))
        return;
#endif
    MPipeline *p = draw_setup(e);
    if (!p)
        return;
    uint32_t start = (uint32_t)MARG(1), count = (uint32_t)MARG(2);
    if (!count || !instances)
        return;
    apply_vertices(e, p, start + count - 1, instances);
    if (g_framelog)
        framelog_draw(e, p, "arrays", count);
    if (instances == 1)
        glDrawArrays(gl_prim((uint32_t)MARG(0)), (GLint)start, (GLsizei)count);
    else
        glDrawArraysInstanced(gl_prim((uint32_t)MARG(0)), (GLint)start, (GLsizei)count, (GLsizei)instances);
    gl_check("drawArrays");
}
/* MINA_FPS: time in the draws (setup, our state, vitaGL) and in ending passes */
uint64_t g_draw_ns, g_endenc_ns;
uint32_t g_draw_calls;
static uint64_t mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
#define TIMED(acc, call) do { uint64_t t0_ = mono_ns(); call; acc += mono_ns() - t0_; g_draw_calls++; } while (0)
static void re_drawPrimitives(Ctx *c) { TIMED(g_draw_ns, draw_arrays(c, 1)); }
static void re_drawPrimitivesInstanced(Ctx *c) { TIMED(g_draw_ns, draw_arrays(c, (uint32_t)MARG(3))); }

/* drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:[instanceCount:] */
/* MINA_DRAWPROF: the indexed draws' time by step (setup, vertices, indices, the GL draw), logged with MINA_FPS */
uint64_t g_dp_ns[4];
static int drawprof = -1;
static uint64_t dp_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
#define DP_MARK(k) do { if (drawprof) { uint64_t t_ = dp_now(); g_dp_ns[k] += t_ - dp_t; dp_t = t_; } } while (0)
static void draw_indexed(Ctx *c, uint32_t instances)
{
    if (drawprof < 0)
        drawprof = getenv("MINA_DRAWPROF") != NULL;
    uint64_t dp_t = drawprof ? dp_now() : 0;
    REnc *e = objc_host(SELF);
#if defined(__vita__)
    if (batch_draw(c, e, instances))
        return;
#endif
    MPipeline *p = draw_setup(e);
    DP_MARK(0);
    if (!p)
        return;
    uint32_t count = (uint32_t)MARG(1), type = (uint32_t)MARG(2);
    MBuffer *ib = objc_host(MARG(3));
    uint32_t off = (uint32_t)MARG(4);
    if (!count || !instances || !ib)
        return;
    uint32_t isz = type ? 4 : 2;
    const uint8_t *idx = ib->gpu_only ? (ib->shadow ? ib->shadow + off : NULL) : G(metal_buf_mem(ib) + off);
    uint32_t maxv = UINT32_MAX;          /* unknown: shared vertex buffers go up whole */
    /* (only a buffer that goes up needs it: the tile layers' private meshes
     * read ~100k indices a frame) */
    int uploads = 0;
    for (int a = 0; a < MAX_ATTRS && !uploads; a++)
        if (p->attr_mask & (1u << a)) {
            const Binding *bd = &e->vb[p->attrs[a].buffer % MAX_VBUF];
            uploads = !(bd->buf && bd->buf->gpu_only);
        }
    if (idx && uploads) {
        maxv = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t v = type ? rt_ld32(idx + 4 * i) : rt_ld16(idx + 2 * i);
            if (v > maxv) maxv = v;
        }
    }
    apply_vertices(e, p, maxv, instances);
    DP_MARK(1);
    if (g_framelog) {
        framelog_draw(e, p, "indexed", count);
        const char *v = getenv("MINA_FRAMELOG_VS");
        if (v && atoi(v) == p->vs)
            rt_log("[frame %d]     ib %#llx at %u (gpu_only %d, shadow %d) first %u max %u\n", g_framelog,
                   (unsigned long long)MARG(3), off, ib->gpu_only, ib->shadow != NULL,
                   idx ? (type ? rt_ld32(idx) : rt_ld16(idx)) : 0, maxv);
    }
    uint32_t ipos;
    if (ib->gpu_only) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->gl);
        ipos = off;
    } else {
        ipos = ring_upload(idx, count * isz);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ring);
    }
    DP_MARK(2);
    if (instances == 1)
        glDrawElements(gl_prim((uint32_t)MARG(0)), (GLsizei)count, type ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                       (const void *)(uintptr_t)ipos);
    else
        glDrawElementsInstanced(gl_prim((uint32_t)MARG(0)), (GLsizei)count, type ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                                (const void *)(uintptr_t)ipos, (GLsizei)instances);
    DP_MARK(3);
    gl_check("drawElements");
}
static void re_drawIndexed(Ctx *c) { TIMED(g_draw_ns, draw_indexed(c, 1)); }
static void re_drawIndexedInstanced(Ctx *c) { TIMED(g_draw_ns, draw_indexed(c, (uint32_t)MARG(5))); }
static void re_endEncoding_timed(Ctx *c)
{
    uint64_t t0 = mono_ns();
    re_endEncoding(c);
    g_endenc_ns += mono_ns() - t0;
}

static const Method renc_m[] = {
    { "endEncoding", re_endEncoding_timed },
    { "setRenderPipelineState:", re_setPipeline },
    { "setVertexBuffer:offset:atIndex:", re_setVertexBuffer },
    { "setFragmentBuffer:offset:atIndex:", re_setFragmentBuffer },
    { "setVertexBufferOffset:atIndex:", re_setVertexBufferOffset },
    { "setFragmentBufferOffset:atIndex:", re_setFragmentBufferOffset },
    { "setVertexBytes:length:atIndex:", re_setVertexBytes },
    { "setFragmentBytes:length:atIndex:", re_setFragmentBytes },
    { "setVertexTexture:atIndex:", re_setVertexTexture },
    { "setFragmentTexture:atIndex:", re_setFragmentTexture },
    { "setVertexSamplerState:atIndex:", re_setVertexSampler },
    { "setFragmentSamplerState:atIndex:", re_setFragmentSampler },
    { "setDepthStencilState:", re_setDepthStencil },
    { "setStencilReferenceValue:", re_setStencilRef },
    { "setCullMode:", re_setCull },
    { "setFrontFacingWinding:", re_setWinding },
    { "setViewport:", re_setViewport },
    { "setScissorRect:", re_setScissor },
    { "setDepthBias:slopeScale:clamp:", re_setDepthBias },
    { "setTriangleFillMode:", re_nop },
    { "setLabel:", re_nop }, { "label", re_label }, { "pushDebugGroup:", re_nop }, { "popDebugGroup", re_nop },
    { "insertDebugSignpost:", re_nop },
    { "drawPrimitives:vertexStart:vertexCount:", re_drawPrimitives },
    { "drawPrimitives:vertexStart:vertexCount:instanceCount:", re_drawPrimitivesInstanced },
    { "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:", re_drawIndexed },
    { "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:", re_drawIndexedInstanced },
    { NULL, NULL },
};
Class cls_RenderEnc = { "MTLRenderCommandEncoder_GL", "NSObject", renc_m, NULL, sizeof(REnc), renc_dealloc };

/* ---- blit encoder ------------------------------------------------------------------------------------ */

typedef struct BEnc {
    int ended;
} BEnc;
Class cls_BlitEnc;

uint64_t metal_blit_encoder(Ctx *c)
{
    (void)c;
    uint64_t o = objc_new_object(&cls_BlitEnc);
    gpu_lock();
    return o;
}
static void be_endEncoding(Ctx *c)
{
    BEnc *b = objc_host(SELF);
    if (b->ended)
        return;
    b->ended = 1;
    gpu_unlock();
}

/* copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:
 *   destinationSlice:destinationLevel:destinationOrigin:
 * x2 buffer, x3 offset, x4 bytesPerRow, x5 bytesPerImage, x6 &size, x7 texture,
 * stack: slice, level, &origin */
static void be_copyBufferToTexture(Ctx *c)
{
    MBuffer *b = objc_host(MARG(0));
    MTexture *t = objc_host(MARG(5));
    uint64_t size = MARG(4);
    uint64_t sp = c->sp;
    uint32_t slice = (uint32_t)rt_ld64(G(sp)), level = (uint32_t)rt_ld64(G(sp + 8));
    uint64_t origin = rt_ld64(G(sp + 16));
    metal_tex_upload(t, (uint32_t)rt_ld64(G(origin)), (uint32_t)rt_ld64(G(origin + 8)), (uint32_t)rt_ld64(G(size)),
                     (uint32_t)rt_ld64(G(size + 8)), level, slice, G(metal_buf_mem(b) + (uint32_t)MARG(1)),
                     (uint32_t)MARG(2));
}

/* copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:
 *   destinationLevel:destinationOrigin:
 * x2 src, x3 slice, x4 level, x5 &origin, x6 &size, x7 dst, stack: slice, level, &origin */
int direct_display(void);
static void be_copyTextureRegion(Ctx *c)
{
    GL_DIRTY();
    MTexture *s = objc_host(MARG(0)), *d = objc_host(MARG(5));
    if (!s || !d) {          /* a texture that is not live (see bound_wrong): nothing to copy */
        static int told;
        if (told++ < 8)
            rt_log("metal: a texture copy with a texture that is not live (%#llx -> %#llx)\n",
                   (unsigned long long)MARG(0), (unsigned long long)MARG(5));
        return;
    }
    metal_fold_read(s);
    metal_fold_write(d);
    d->clear_valid = 0;
    uint64_t so = MARG(3), sz = MARG(4), sp = c->sp;
    uint32_t dlevel = (uint32_t)rt_ld64(G(sp + 8));
    uint64_t dor = rt_ld64(G(sp + 16));
    GLint sx = (GLint)rt_ld64(G(so)), sy = (GLint)rt_ld64(G(so + 8));
    GLint w = (GLint)rt_ld64(G(sz)), h = (GLint)rt_ld64(G(sz + 8));
    GLint dx = (GLint)rt_ld64(G(dor)), dy = (GLint)rt_ld64(G(dor + 8));
    /* the region within both textures: the frame of the old window size is
     * copied into the drawable of the new one once (the game's pixel scale
     * option); GL draws nothing or faults on the parts outside */
    if (sx + w > (GLint)s->width) w = (GLint)s->width - sx;
    if (sy + h > (GLint)s->height) h = (GLint)s->height - sy;
    if (dx + w > (GLint)d->width) w = (GLint)d->width - dx;
    if (dy + h > (GLint)d->height) h = (GLint)d->height - dy;
    if (w <= 0 || h <= 0)
        return;
#if defined(__vita__)
    if (d->is_drawable && sx == 0 && sy == 0 && dx == 0 && dy == 0 && (uint32_t)w == s->width &&
        (uint32_t)h == s->height && s->width == d->width && s->height == d->height && direct_display()) {
        d->alias = s;           /* the frame copied whole into the drawable */
        d->on_display = 0;
        return;
    }
#endif
#if defined(__vita__)
    vita_copy_quad(s, (float)sx, (float)sy, (float)w, (float)h, metal_tex_fbo(d, (int)dlevel), dx, dy, w, h, 0);
#else
    glBindFramebuffer(GL_READ_FRAMEBUFFER, metal_tex_fbo(s, (int)MARG(2)));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, metal_tex_fbo(d, (int)dlevel));
    glDisable(GL_SCISSOR_TEST);
    {
        int metal_texlog(void);
        if (metal_texlog())
            rt_log("[tex] copy gl %u %ux%u -> gl %u %ux%u%s: %dx%d (fb %#x %#x)\n", s->gl, s->width, s->height, d->gl,
                   d->width, d->height, d->is_drawable ? " drawable" : "", w, h,
                   glCheckFramebufferStatus(GL_READ_FRAMEBUFFER), glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER));
    }
    glBlitFramebuffer(sx, sy, sx + w, sy + h, dx, dy, dx + w, dy + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
#endif
    gl_check("copyTexture");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
static void be_copyTexture(Ctx *c)
{
    GL_DIRTY();
    MTexture *s = objc_host(MARG(0)), *d = objc_host(MARG(1));
    if (!s || !d) {          /* a texture that is not live (see bound_wrong): nothing to copy */
        static int told;
        if (told++ < 8)
            rt_log("metal: a texture copy with a texture that is not live (%#llx -> %#llx)\n",
                   (unsigned long long)MARG(0), (unsigned long long)MARG(1));
        return;
    }
    metal_fold_read(s);
    metal_fold_write(d);
    d->clear_valid = 0;
#if defined(__vita__)
    /* the frame copied whole into the drawable: present shows it from there */
    if (d->is_drawable && s->width == d->width && s->height == d->height && direct_display()) {
        d->alias = s;
        d->on_display = 0;
        return;
    }
#endif
#if defined(__vita__)
    vita_copy_quad(s, 0, 0, (float)s->width, (float)s->height, metal_tex_fbo(d, 0), 0, 0, (GLsizei)d->width,
                   (GLsizei)d->height, 0);
#else
    glBindFramebuffer(GL_READ_FRAMEBUFFER, metal_tex_fbo(s, 0));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, metal_tex_fbo(d, 0));
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, (GLint)s->width, (GLint)s->height, 0, 0, (GLint)d->width, (GLint)d->height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
#endif
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
/* copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size: */
/* bytes of p[0..n) up to the last nonzero one */
static uint32_t data_extent(const uint8_t *p, uint32_t n)
{
    while (n >= 8) {
        uint64_t w;
        memcpy(&w, p + n - 8, 8);
        if (w)
            break;
        n -= 8;
    }
    while (n && p[n - 1] == 0)
        n--;
    return n;
}

#define BIG_PRIVATE (512u << 10)
static void gpu_buffer_write(MBuffer *d, uint32_t off, const void *src, uint32_t n)
{
    /* A big private buffer written whole from 0 (a tile level's 1.25 MiB, ~200 KiB
     * of it vertices, the rest zero): its GL buffer holds only up to the last
     * nonzero byte, with room to spare (the Vita's GPU memory: ~100 of them). */
    if (d->length >= BIG_PRIVATE && off == 0 && n == d->length && !d->shadow) {
        static int whole = -1;
        if (whole < 0)
            whole = getenv("MINA_WHOLE_PRIVATE") != NULL;
        if (!whole) {
            const uint32_t used = data_extent(src, n);
            uint32_t need = (used + (used >> 1) + (64u << 10) + 0xffffu) & ~0xffffu;
            if (need > d->length)
                need = d->length;
            if (!d->gl || d->gl_size < used) {
                if (!d->gl)
                    glGenBuffers(1, &d->gl);
                glBindBuffer(GL_ARRAY_BUFFER, d->gl);
                glBufferData(GL_ARRAY_BUFFER, need, NULL, GL_STATIC_DRAW);
                d->gl_size = need;
                d->gl_used = 0;
            }
            glBindBuffer(GL_ARRAY_BUFFER, d->gl);
            /* zeros where the last data went further */
            const uint32_t clear_to = d->gl_used < d->gl_size ? d->gl_used : d->gl_size;
            if (used)
                glBufferSubData(GL_ARRAY_BUFFER, 0, used, src);
            if (clear_to > used) {
                void *z = calloc(1, clear_to - used);
                if (z) {
                    glBufferSubData(GL_ARRAY_BUFFER, used, clear_to - used, z);
                    free(z);
                }
            }
            d->gl_used = used;
            return;
        }
    }
    if (!d->gl || (d->gl_size && d->gl_size < d->length)) {
        if (!d->gl)
            glGenBuffers(1, &d->gl);
        glBindBuffer(GL_ARRAY_BUFFER, d->gl);
        if (d->gl_size) {
            static int told;
            if (told++ < 4)
                rt_log("metal: a partial write (%u bytes at %u) to a %u-byte private buffer held small: made whole, "
                       "its old data lost\n", n, off, d->length);
        }
        glBufferData(GL_ARRAY_BUFFER, d->length, NULL, GL_STATIC_DRAW);   /* (a partial write: all of it) */
        d->gl_size = d->length;
        d->gl_used = d->length;
    }
    glBindBuffer(GL_ARRAY_BUFFER, d->gl);
    glBufferSubData(GL_ARRAY_BUFFER, off, n, src);
    if (d->shadow)
        memcpy(d->shadow + off, src, n);
}

static void be_copyBuffer(Ctx *c)
{
    MBuffer *s = objc_host(MARG(0)), *d = objc_host(MARG(2));
    uint32_t so = (uint32_t)MARG(1), dof = (uint32_t)MARG(3), n = (uint32_t)MARG(4);
    if (s->gpu_only) {
        /* a staging buffer is never private: this one was freed and its
         * address reused (see metal.c, command buffer completion) */
        static int told;
        if (told++ < 4)
            rt_log("metal: copy from a private buffer (%u bytes): skipped\n", n);
        return;
    }
    if (d->gpu_only) {
        if (!s->contents && s->given_back) {
            /* given back: uploaded again from its saved bytes (ycGpuBuffer keeps
             * its staging buffers to fill its GPU buffer again) */
            uint8_t *tmp = calloc(1, (size_t)so + n);
            if (!tmp)
                rt_fatal("metal: no memory to upload %u bytes again", n);
            if (s->saved_len)
                memcpy(tmp, s->saved, s->saved_len < so + n ? s->saved_len : so + n);
            gpu_buffer_write(d, dof, tmp + so, n);
            free(tmp);
            return;
        }
        gpu_buffer_write(d, dof, G(metal_buf_mem(s) + so), n);
        /* A whole staging buffer uploaded to GPU memory gives its memory back
         * (tile levels keep them all, 1.25 MiB each, with a few KiB written):
         * its bytes up to the last nonzero one are kept, for another upload. */
        static int keep = -1;
        if (keep < 0)
            keep = getenv("MINA_KEEP_STAGING") != NULL;
        if (!keep && so == 0 && n == s->length && !s->fresh && !s->keep_contents)
            metal_buf_give_back(s);
    } else
        memmove(G(metal_buf_mem(d) + dof), G(metal_buf_mem(s) + so), n);
}
/* fillBuffer:range:value: (NSRange in x3/x4) */
static void be_fillBuffer(Ctx *c)
{
    MBuffer *b = objc_host(MARG(0));
    if (b->gpu_only) {
        uint8_t *tmp = malloc((size_t)MARG(2));
        memset(tmp, (int)MARG(3), (size_t)MARG(2));
        gpu_buffer_write(b, (uint32_t)MARG(1), tmp, (uint32_t)MARG(2));
        free(tmp);
        return;
    }
    memset(G(metal_buf_mem(b) + (uint32_t)MARG(1)), (int)MARG(3), (size_t)MARG(2));
}
static void be_generateMipmaps(Ctx *c)
{
    GL_DIRTY();
    MTexture *t = objc_host(MARG(0));
    metal_fold_write(t);
    glBindTexture(metal_tex_target(t), t->gl);
    glGenerateMipmap(metal_tex_target(t));
}
static void be_nop(Ctx *c) { (void)c; }

static const Method benc_m[] = {
    { "endEncoding", be_endEncoding },
    { "copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:", be_copyBufferToTexture },
    { "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:", be_copyTextureRegion },
    { "copyFromTexture:toTexture:", be_copyTexture },
    { "copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:", be_copyBuffer },
    { "fillBuffer:range:value:", be_fillBuffer },
    { "generateMipmapsForTexture:", be_generateMipmaps },
    { "synchronizeTexture:slice:level:", be_nop }, { "synchronizeResource:", be_nop },
    { "setLabel:", be_nop }, { "pushDebugGroup:", be_nop }, { "popDebugGroup", be_nop },
    { NULL, NULL },
};
Class cls_BlitEnc = { "MTLBlitCommandEncoder_GL", "NSObject", benc_m, NULL, sizeof(BEnc), NULL };

/* ---- for ycRenderDevice::DrawCall's native: the render encoder's state ------------------
 * A message that would change nothing (the value the encoder already holds:
 * its setter returns at once) is not sent; the bytes and the draw go to the
 * methods directly. Render thread. */
void *metal_renc_of(uint64_t obj)
{
    Obj *o = obj ? objc_obj_tagged(obj) : NULL;
    return o && o->cls == &cls_RenderEnc ? o->host : NULL;
}
int renc_same_pipeline(void *re, uint64_t v) { return ((REnc *)re)->pipeline == v; }
int renc_same_tex(void *re, int frag, int i, uint64_t t)
{
    return (frag ? ((REnc *)re)->ftex : ((REnc *)re)->vtex)[i & 15] == t;
}
int renc_same_stencil_ref(void *re, uint32_t v) { return ((REnc *)re)->stencil_ref == v; }
int renc_same_winding(void *re, uint32_t v) { return ((REnc *)re)->winding == v; }
int renc_same_cull(void *re, uint32_t v) { return ((REnc *)re)->cull == v; }
int renc_same_bias(void *re, float b, float s)
{
    const REnc *e = re;
    return !memcmp(&e->depth_bias, &b, 4) && !memcmp(&e->depth_slope, &s, 4);
}
int renc_same_vbuffer(void *re, int i, uint64_t buf, uint32_t off)
{
    const Binding *b = &((REnc *)re)->vb[i % MAX_VBUF];
    return b->obj == buf && !b->nbytes && b->offset == off;
}
void renc_set_bytes(void *re, int frag, int i, uint64_t src, uint32_t n)
{
    REnc *e = re;
    set_bytes(frag ? &e->fb[i % MAX_VBUF] : &e->vb[i % MAX_VBUF], src, n);
}
/* drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset: with c->x[0] the encoder, x[2..6] its arguments */
void renc_draw_indexed(Ctx *c) { re_drawIndexed(c); }

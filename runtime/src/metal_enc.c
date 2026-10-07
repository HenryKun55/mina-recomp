/* Metal on OpenGL: render and blit command encoders (executed immediately). */
#include "metal_gl.h"

#include <string.h>

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

/* ---- render encoder ------------------------------------------------------------------------ */

typedef struct Binding {
    MBuffer *buf;            /* or NULL with inline bytes */
    uint64_t obj;
    uint32_t offset;
    uint8_t *bytes;
    uint32_t nbytes;
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
    int to_display;                      /* Vita: the drawable's pass draws to the screen */
    int folded;                          /* Vita: its copy was folded (fold_pass), nothing drawn yet */
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
        n += snprintf(buf + n, sizeof buf - n, " depth load %u#%04x", (uint32_t)pb_get(depth, "loadAction", 0),
                      (unsigned)(((uintptr_t)objc_host(pb_get(depth, "texture", 0)) >> 4) & 0xffff));
    if (stencil && pb_get(stencil, "texture", 0))
        n += snprintf(buf + n, sizeof buf - n, " stencil load %u", (uint32_t)pb_get(stencil, "loadAction", 0));
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
    for (int k = 0; k < p->ntex; k++) {
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
    e->begun = 1;
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

static void re_endEncoding(Ctx *c)
{
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
        bind_release(b);
        b->obj = buf;
        b->buf = buf ? objc_host(buf) : NULL;
    }
    b->offset = offset;
}
static void set_bytes(Binding *b, uint64_t src, uint32_t n)
{
    bind_release(b);
    b->bytes = malloc(n);
    memcpy(b->bytes, G(src), n);
    b->nbytes = n;
}
static const uint8_t *binding_data(const Binding *b, uint32_t *avail)
{
    if (b->bytes) {
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
    for (int k = 0; k < 4; k++)
        e->sc[k] = (int)rt_ld64(G(r + 8 * k));
    e->sc_set = 1;
    if (e->begun)
        glScissor(e->sc[0], screen_y(e, e->sc[1], e->sc[3]), e->sc[2], e->sc[3]);
}
static void re_setDepthBias(Ctx *c)
{
    float bias = c->v[0].f[0], slope = c->v[1].f[0];
    if (bias == 0 && slope == 0) {
        glDisable(GL_POLYGON_OFFSET_FILL);
        return;
    }
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(slope, bias);
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

static void apply_state(REnc *e, MPipeline *p)
{
    for (int i = 0; i < MAX_COLOR; i++) {
        if (!e->color[i])
            continue;
        const MBlend *b = &p->color[i];
        if (b->enabled) {
            glEnablei(GL_BLEND, (GLuint)i);
            glBlendFuncSeparatei((GLuint)i, gl_blend_factor(b->src_rgb), gl_blend_factor(b->dst_rgb),
                                 gl_blend_factor(b->src_a), gl_blend_factor(b->dst_a));
            glBlendEquationSeparatei((GLuint)i, gl_blend_op(b->rgb_op), gl_blend_op(b->alpha_op));
        } else {
            glDisablei(GL_BLEND, (GLuint)i);
        }
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
    if (ds && (ds->compare != 7 || ds->write)
#if defined(__vita__)
        && !no_depth
#endif
        ) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(metal_gl_compare(ds->compare));
        glDepthMask(ds->write ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }
    if (ds && (ds->write || (ds->stencil_enabled && !(stencil_keeps(&ds->front) && stencil_keeps(&ds->back))))) {
        MTexture *d = e->dtex ? e->dtex : e->stex;
        d->clear_valid = 0;             /* written from now on */
    }
    if (ds && ds->stencil_enabled
#if defined(__vita__)
        && !no_stencil
#endif
        ) {
        glEnable(GL_STENCIL_TEST);
        glStencilFuncSeparate(GL_FRONT, metal_gl_compare(ds->front.compare), (GLint)e->stencil_ref, ds->front.read_mask);
        glStencilFuncSeparate(GL_BACK, metal_gl_compare(ds->back.compare), (GLint)e->stencil_ref, ds->back.read_mask);
        glStencilOpSeparate(GL_FRONT, gl_stencil_op(ds->front.fail), gl_stencil_op(ds->front.depth_fail),
                            gl_stencil_op(ds->front.pass));
        glStencilOpSeparate(GL_BACK, gl_stencil_op(ds->back.fail), gl_stencil_op(ds->back.depth_fail),
                            gl_stencil_op(ds->back.pass));
        glStencilMaskSeparate(GL_FRONT, ds->front.write_mask);
        glStencilMaskSeparate(GL_BACK, ds->back.write_mask);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    if (e->cull) {
        glEnable(GL_CULL_FACE);
        glCullFace(e->cull == 1 ? GL_FRONT : GL_BACK);
    } else {
        glDisable(GL_CULL_FACE);
    }
    /* the y flip mirrors the winding: Metal clockwise is GL counter-clockwise */
    glFrontFace((e->winding == 0) != e->to_display ? GL_CCW : GL_CW);   /* y mirrored on the screen */
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
                glUniform4fv(p->vcb[i], n, (const GLfloat *)d);
            }
        }
        if (p->fcb[i] >= 0) {
            const uint8_t *d = binding_data(&e->fb[i], &avail);
            if (d) {
                GLsizei n = p->fcb_size[i];
                if ((uint32_t)n * 16 > avail) n = (GLsizei)(avail / 16);
                glUniform4fv(p->fcb[i], n, (const GLfloat *)d);
            }
        }
    }
}

static void bind_tex(int unit, uint64_t tex, uint64_t samp)
{
    glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
    if (!tex) {
        glBindTexture(GL_TEXTURE_2D, 0);
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
        glBindTexture(GL_TEXTURE_2D, t->depth_tex);
        glBindSampler((GLuint)unit, samp ? ((MSampler *)objc_host(samp))->gl : 0);
        return;
    }
#endif
    glBindTexture(metal_tex_target(t), t->gl);
    glBindSampler((GLuint)unit, samp ? ((MSampler *)objc_host(samp))->gl : 0);
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
    case 42: *v = (VFmt){ GL_BGRA, GL_UNSIGNED_BYTE, 1, 0 }; return 1;
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
                /* private buffer: read in place */
                vbo[bi] = bd->buf->gl;
                base[bi] = bd->offset;
            } else {
                uint32_t avail;
                const uint8_t *d = binding_data(bd, &avail);
                uint32_t count = lay->step_fn == 2 ? (instances + lay->step_rate - 1) / (lay->step_rate ? lay->step_rate : 1)
                               : lay->step_fn == 0 ? 1 : max_vertex + 1;
                uint32_t need = lay->stride * count;
                if (max_vertex == UINT32_MAX || need > avail)
                    need = avail;
                base[bi] = d && need ? ring_upload(d, need) : 0;
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

static MPipeline *draw_setup(REnc *e)
{
    if (!e->pipeline)
        return NULL;
    MPipeline *p = objc_host(e->pipeline);
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
    if (!e->begun)
        pass_begin(e);
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
#if defined(__vita__)
    if (getenv("MINA_DRAWLOG")) {
        static uint8_t seen[256][256];
        if (p->vs >= 0 && p->vs < 256 && p->fs >= 0 && p->fs < 256 && !seen[p->vs][p->fs]++)
            rt_log("[draw] first draw with vs %d fs %d (%d textures) prog %u\n", p->vs, p->fs, p->ntex, p->prog);
    }
#endif
    if (e->to_display && getenv("MINA_DISPLAY_LOG")) {
        static int n;
        if (n++ < 6)
            rt_log("[display] fs %d cull %u winding %u vp_set %d vp %g %g %g %g sc_set %d sc %d %d %d %d yflip_loc %d\n",
                   p->fs, e->cull, e->winding, e->vp_set, e->vp[0], e->vp[1], e->vp[2], e->vp[3], e->sc_set, e->sc[0],
                   e->sc[1], e->sc[2], e->sc[3], p->yflip_loc);
    }
    glUseProgram(p->prog);
    if (p->yflip_loc >= 0)
        glUniform1f(p->yflip_loc, e->to_display ? 1.0f : -1.0f);
#if defined(__vita__)
    if (getenv("MINA_DRAWLOG")) {
        GLint cur = -1;
        glGetIntegerv(GL_CURRENT_PROGRAM, &cur);
        static int n;
        if (cur != (GLint)p->prog && n++ < 10)
            rt_log("[draw] glUseProgram(%u) left the current program at %d (vs %d fs %d)\n", p->prog, cur, p->vs, p->fs);
    }
#endif
    apply_state(e, p);
    apply_uniforms(e, p);
    apply_textures(e, p);
    return p;
}

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
        snprintf(buf + n, sizeof buf - n, " mask %#x", p->color[0].write_mask);
    rt_log("%s\n", buf);
    /* MINA_FRAMELOG_FS=n: the fragment constant buffers of that shader too */
    static int dump_fs = -2;
    if (dump_fs == -2) {
        const char *v = getenv("MINA_FRAMELOG_FS");
        dump_fs = v ? atoi(v) : -1;
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

/* drawPrimitives:vertexStart:vertexCount:[instanceCount:] */
static void draw_arrays(Ctx *c, uint32_t instances)
{
    REnc *e = objc_host(SELF);
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
static void re_drawPrimitives(Ctx *c) { draw_arrays(c, 1); }
static void re_drawPrimitivesInstanced(Ctx *c) { draw_arrays(c, (uint32_t)MARG(3)); }

/* drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:[instanceCount:] */
static void draw_indexed(Ctx *c, uint32_t instances)
{
    REnc *e = objc_host(SELF);
    MPipeline *p = draw_setup(e);
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
    if (idx) {
        maxv = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t v = type ? rt_ld32(idx + 4 * i) : rt_ld16(idx + 2 * i);
            if (v > maxv) maxv = v;
        }
    }
    apply_vertices(e, p, maxv, instances);
    if (g_framelog)
        framelog_draw(e, p, "indexed", count);
    uint32_t ipos;
    if (ib->gpu_only) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->gl);
        ipos = off;
    } else {
        ipos = ring_upload(idx, count * isz);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ring);
    }
    if (instances == 1)
        glDrawElements(gl_prim((uint32_t)MARG(0)), (GLsizei)count, type ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                       (const void *)(uintptr_t)ipos);
    else
        glDrawElementsInstanced(gl_prim((uint32_t)MARG(0)), (GLsizei)count, type ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                                (const void *)(uintptr_t)ipos, (GLsizei)instances);
    gl_check("drawElements");
}
static void re_drawIndexed(Ctx *c) { draw_indexed(c, 1); }
static void re_drawIndexedInstanced(Ctx *c) { draw_indexed(c, (uint32_t)MARG(5)); }

static const Method renc_m[] = {
    { "endEncoding", re_endEncoding },
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
    MTexture *s = objc_host(MARG(0)), *d = objc_host(MARG(5));
    metal_fold_read(s);
    metal_fold_write(d);
    d->clear_valid = 0;
    uint64_t so = MARG(3), sz = MARG(4), sp = c->sp;
    uint32_t dlevel = (uint32_t)rt_ld64(G(sp + 8));
    uint64_t dor = rt_ld64(G(sp + 16));
    GLint sx = (GLint)rt_ld64(G(so)), sy = (GLint)rt_ld64(G(so + 8));
    GLint w = (GLint)rt_ld64(G(sz)), h = (GLint)rt_ld64(G(sz + 8));
    GLint dx = (GLint)rt_ld64(G(dor)), dy = (GLint)rt_ld64(G(dor + 8));
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
    glBlitFramebuffer(sx, sy, sx + w, sy + h, dx, dy, dx + w, dy + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
#endif
    gl_check("copyTexture");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
static void be_copyTexture(Ctx *c)
{
    MTexture *s = objc_host(MARG(0)), *d = objc_host(MARG(1));
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
static void gpu_buffer_write(MBuffer *d, uint32_t off, const void *src, uint32_t n)
{
    if (!d->gl) {
        glGenBuffers(1, &d->gl);
        glBindBuffer(GL_ARRAY_BUFFER, d->gl);
        glBufferData(GL_ARRAY_BUFFER, d->length, NULL, GL_STATIC_DRAW);
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
        static int told;
        if (!told++)
            rt_log("metal: copy from a private buffer is not supported\n");
        return;
    }
    if (d->gpu_only) {
        gpu_buffer_write(d, dof, G(metal_buf_mem(s) + so), n);
        /* A whole staging buffer uploaded to GPU memory: the game never reads
         * it again (tile levels even keep them all), so its memory goes now. */
        static int keep = -1;
        if (keep < 0)
            keep = getenv("MINA_KEEP_STAGING") != NULL;
        if (!keep && so == 0 && n == s->length && !s->fresh && !s->keep_contents) {
            gfree(s->contents);
            s->contents = 0;
        }
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

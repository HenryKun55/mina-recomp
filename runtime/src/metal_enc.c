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

#define RING_SIZE (64u << 20)
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
    e->fbo = metal_pass_fbo(e->color, levels, dtex, stex);
    glBindFramebuffer(GL_FRAMEBUFFER, e->fbo);
    MTexture *first = e->color[0] ? e->color[0] : (dtex ? dtex : stex);
    for (int i = 0; !first && i < MAX_COLOR; i++)
        first = e->color[i];
    e->tw = first ? first->width >> levels[0] : 1;
    e->th = first ? first->height >> levels[0] : 1;
    glViewport(0, 0, (GLsizei)e->tw, (GLsizei)e->th);
    glDepthRange(0.0, 1.0);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, (GLsizei)e->tw, (GLsizei)e->th);
    glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    for (int i = 0; i < MAX_COLOR; i++) {
        if (!e->color[i] || pb_get(color[i], "loadAction", 0) != 2)
            continue;
        GLfloat cc[4];
        for (int k = 0; k < 4; k++)
            cc[k] = (GLfloat)pb_getd(color[i], "clearColor", k, k == 3 ? 1.0 : 0.0);
        glColorMaski((GLuint)i, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearBufferfv(GL_COLOR, i, cc);
    }
    if (dtex && pb_get(depth, "loadAction", 0) == 2) {
        glDepthMask(GL_TRUE);
        GLfloat d = (GLfloat)pb_getd(depth, "clearDepth", 0, 1.0);
        glClearBufferfv(GL_DEPTH, 0, &d);
    }
    if (stex && pb_get(stencil, "loadAction", 0) == 2) {
        glStencilMask(0xFF);
        GLint s = (GLint)pb_get(stencil, "clearStencil", 0);
        glClearBufferiv(GL_STENCIL, 0, &s);
    }
    e->cull = 0;
    e->winding = 0;
    gl_check("pass begin");
    return o;
}

static void re_endEncoding(Ctx *c)
{
    REnc *e = objc_host(SELF);
    if (e->ended)
        return;
    e->ended = 1;
    for (int i = 0; i < MAX_COLOR; i++) {
        if (!e->resolve[i])
            continue;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, e->fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0 + (GLenum)i);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, metal_tex_fbo(e->resolve[i], 0));
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0, 0, (GLint)e->tw, (GLint)e->th, 0, 0, (GLint)e->tw, (GLint)e->th,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
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
    glViewport((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h);
    glDepthRange(zn, zf);
}
/* setScissorRect:(MTLScissorRect) — 32 bytes, passed by pointer */
static void re_setScissor(Ctx *c)
{
    uint64_t r = MARG(0);
    glScissor((GLint)rt_ld64(G(r)), (GLint)rt_ld64(G(r + 8)), (GLsizei)rt_ld64(G(r + 16)), (GLsizei)rt_ld64(G(r + 24)));
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
    MDepthStencil *ds = e->ds ? objc_host(e->ds) : NULL;
    if (ds && (ds->compare != 7 || ds->write)) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(metal_gl_compare(ds->compare));
        glDepthMask(ds->write ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
    }
    if (ds && ds->stencil_enabled) {
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
    glFrontFace(e->winding == 0 ? GL_CCW : GL_CW);
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
    glBindTexture(metal_tex_target(t), t->gl);
    glBindSampler((GLuint)unit, samp ? ((MSampler *)objc_host(samp))->gl : 0);
}

static void apply_textures(REnc *e, MPipeline *p)
{
    for (int k = 0; k < p->ntex; k++) {
        int t = p->texb[k].tex & 15, s = p->texb[k].samp;
        if (p->texb[k].stage)
            bind_tex(p->texb[k].unit, e->ftex[t], s >= 0 ? e->fsamp[s & 15] : 0);
        else
            bind_tex(p->texb[k].unit, e->vtex[t], s >= 0 ? e->vsamp[s & 15] : 0);
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
        glVertexAttribDivisor((GLuint)a, lay->step_fn == 2 ? (lay->step_rate ? lay->step_rate : 1) : 0);
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

static MPipeline *draw_setup(REnc *e)
{
    if (!e->pipeline)
        return NULL;
    MPipeline *p = objc_host(e->pipeline);
    glUseProgram(p->prog);
    apply_state(e, p);
    apply_uniforms(e, p);
    apply_textures(e, p);
    return p;
}

/* drawPrimitives:vertexStart:vertexCount:[instanceCount:] */
static void draw_arrays(Ctx *c, uint32_t instances)
{
    REnc *e = objc_host(SELF);
    MPipeline *p = draw_setup(e);
    if (!p)
        return;
    uint32_t start = (uint32_t)MARG(1), count = (uint32_t)MARG(2);
    if (!count || !instances)
        return;
    apply_vertices(e, p, start + count - 1, instances);
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
    uint32_t ipos;
    if (ib->gpu_only) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->gl);
        ipos = off;
    } else {
        ipos = ring_upload(idx, count * isz);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ring);
    }
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
static void be_copyTextureRegion(Ctx *c)
{
    MTexture *s = objc_host(MARG(0)), *d = objc_host(MARG(5));
    uint64_t so = MARG(3), sz = MARG(4), sp = c->sp;
    uint32_t dlevel = (uint32_t)rt_ld64(G(sp + 8));
    uint64_t dor = rt_ld64(G(sp + 16));
    GLint sx = (GLint)rt_ld64(G(so)), sy = (GLint)rt_ld64(G(so + 8));
    GLint w = (GLint)rt_ld64(G(sz)), h = (GLint)rt_ld64(G(sz + 8));
    GLint dx = (GLint)rt_ld64(G(dor)), dy = (GLint)rt_ld64(G(dor + 8));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, metal_tex_fbo(s, (int)MARG(2)));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, metal_tex_fbo(d, (int)dlevel));
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(sx, sy, sx + w, sy + h, dx, dy, dx + w, dy + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gl_check("copyTexture");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
static void be_copyTexture(Ctx *c)
{
    MTexture *s = objc_host(MARG(0)), *d = objc_host(MARG(1));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, metal_tex_fbo(s, 0));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, metal_tex_fbo(d, 0));
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, (GLint)s->width, (GLint)s->height, 0, 0, (GLint)d->width, (GLint)d->height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
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
        if (so == 0 && n == s->length && !s->fresh) {
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

/*
 * Metal on OpenGL: shared definitions of the emulation (runtime/src/metal*.c).
 *
 * Metal objects are Objective-C objects of the guest runtime with host state.
 * Descriptors are property bags. Command buffers record commands; commit()
 * executes them with GL on the committing thread, under the GPU lock.
 *
 * Orientation: textures and render targets keep Metal's layout (row 0 at the
 * top). Translated vertex shaders flip y and remap z from [0,1] to [-1,1];
 * the drawable is flipped when presented.
 */
#ifndef MINA_METAL_GL_H
#define MINA_METAL_GL_H

#include "objc_rt.h"

#if defined(MINA_NULL_GL)
#include "gl_null.h"
#elif defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#include "gl_compat_vita.h"
#endif

/* ---- property bags ------------------------------------------------------------ */

#define PB_MAX 40
#define PB_MAX_D 2      /* properties with floating-point values (clearColor, lodMinClamp...) */

/* Keys are interned strings: a bag compares them by address. */
typedef struct PropBag {
    int n, nd;                  /* first: a new bag zeroes only these (Class.host_zero) */
    uint64_t held;              /* properties holding an object it retained (bit per slot) */
    struct { const char *key; uint64_t v; } p[PB_MAX];
    int8_t di[PB_MAX];          /* index into d, or -1 */
    double d[PB_MAX_D][4];
} PropBag;

const char *pb_key(const char *s);    /* the interned copy of s */
uint64_t pb_get_k(uint64_t obj, const char *key, uint64_t def);
double pb_getd_k(uint64_t obj, const char *key, int i, double def);
void pb_set_k(uint64_t obj, const char *key, uint64_t v);
uint64_t pb_child_at(uint64_t obj, const char *at_key, Class *cls);
/* by a literal key, interned once per call site (the draw path asks a lot) */
#define PB_K(s) ({ static const char *pbk_; const char *t_ = __atomic_load_n(&pbk_, __ATOMIC_ACQUIRE); \
                   if (!t_) { t_ = pb_key("" s ""); __atomic_store_n(&pbk_, t_, __ATOMIC_RELEASE); } t_; })
#define pb_get(o, k, d) pb_get_k(o, PB_K(k), d)
#define pb_getd(o, k, i, d) pb_getd_k(o, PB_K(k), i, d)
#define pb_set(o, k, v) pb_set_k(o, PB_K(k), v)
/* child object (created on first access) */
#define pb_child(o, k, cls) pb_child_at(o, PB_K("@" k), cls)

/* ---- resources --------------------------------------------------------------------- */

typedef struct MBuffer {
    uint32_t contents;       /* guest memory */
    uint32_t length;
    uint32_t options;
    GLuint gl;
    int gpu_dirty;           /* private buffer written by a blit: re-upload */
    int fresh;               /* debug (MINA_FRESH_BUFFERS): untouched memory, never freed */
    int contents_asked;      /* times the game asked for the CPU pointer */
    int gpu_only;            /* MTLStorageModePrivate: only the GL buffer exists */
    uint8_t *shadow;         /* CPU copy of a small private buffer (index data) */
    int keep_contents;       /* the game asked for its memory after giving it back: it stays */
    uint8_t *saved;          /* given back: its bytes up to the last nonzero one (the rest are zero) */
    uint32_t saved_len;
    int given_back;          /* its memory went after an upload (saved holds its bytes) */
    const void *maker;       /* the thread (Ctx) that made it */
    uint32_t gl_size;        /* private: bytes of its GL buffer (big ones hold their data, not all their length) */
    uint32_t gl_used;        /* private: bytes up to the last nonzero one written whole */
} MBuffer;

/* guest memory of a shared buffer (given back after a whole-buffer upload: made again on use) */
uint32_t metal_buf_mem(MBuffer *b);
/* a staging buffer's guest memory goes, its bytes up to the last nonzero one kept */
void metal_buf_give_back(MBuffer *b);

typedef struct MTexture {
    uint32_t type, format, width, height, depth, mips, samples, usage, storage;
    GLuint gl;
    GLuint fbo_cache;
    uint32_t staging;        /* guest copy for CPU-visible textures (getBytes / replaceRegion) */
    int is_drawable;
    int is_rb;               /* a renderbuffer (Vita depth/stencil), not a texture */
    uint32_t shift;          /* Vita: the GL texture is 2^shift times smaller (sampled only) */
    GLuint depth_tex;        /* Vita: a texture showing a sampled depth renderbuffer (vitaGL patch) */
    int clear_valid;         /* holds clear_value everywhere (cleared, nothing drawn or copied since) */
    struct MTexture *alias;  /* Vita drawable: the texture a whole-texture blit would have copied in */
    int on_display;          /* Vita drawable: this frame's passes drew it straight to the screen */
    float clear_value[4];    /* color, or depth and stencil */
    /* Vita, a folded copy pass (metal_enc.c): this texture reads as read_alias's
     * colour with alpha 1, not drawn; fold_stale: read_alias changed since (the
     * contents are lost, the game redraws them first); no_fold: never again */
    struct MTexture *read_alias;
    int fold_stale, no_fold;
    /* Vita depth/stencil of a folded pass: the clear it skipped, done by the next
     * pass that loads them (sampled: they were read as a texture, never owed) */
    int owe_clear, sampled;
    float owe_value[2];
} MTexture;

/* Vita: a texture is about to be written / read outside a render pass, or freed
 * (folded copies are drawn or dropped first); no-ops elsewhere */
void metal_fold_write(MTexture *t);
void metal_fold_read(MTexture *t);
void metal_fold_forget(MTexture *t);

#if defined(__vita__) && !defined(MINA_NULL_GL)
void vita_copy_quad(MTexture *src, float sx, float sy, float sw, float sh, GLuint dst_fbo, GLint dx, GLint dy,
                    GLsizei dw, GLsizei dh, int flip_y);
#endif

typedef struct MSampler {
    uint32_t minf, magf, mipf, s, t, r, aniso, compare;
    float lod_min, lod_max;
    GLuint gl;
} MSampler;

typedef struct MStencil {
    uint32_t compare, fail, depth_fail, pass, read_mask, write_mask;
} MStencil;

typedef struct MDepthStencil {
    uint32_t compare, write;
    int stencil_enabled;
    MStencil front, back;
} MDepthStencil;

typedef struct MShader {
    int index;               /* into g_shaders */
    int stage;               /* 0 vertex, 1 fragment */
} MShader;

typedef struct MVertexAttr {
    uint32_t format, offset, buffer;
} MVertexAttr;

typedef struct MVertexLayout {
    uint32_t stride, step_fn, step_rate;
} MVertexLayout;

#define MAX_ATTRS 16
#define MAX_VBUF 31
#define MAX_COLOR 8

typedef struct MBlend {
    uint32_t enabled, write_mask, rgb_op, alpha_op, src_rgb, dst_rgb, src_a, dst_a, format;
} MBlend;

typedef struct MPipeline {
    int vs, fs;              /* shader indices */
    GLuint prog;
    MVertexAttr attrs[MAX_ATTRS];
    uint32_t attr_mask;
    MVertexLayout layouts[MAX_VBUF];
    MBlend color[MAX_COLOR];
    uint32_t depth_format, stencil_format, samples, topology;
    /* uniform locations: vertex/fragment constant buffers and textures by Metal index */
    GLint vcb[MAX_VBUF], fcb[MAX_VBUF];
    GLint vcb_size[MAX_VBUF], fcb_size[MAX_VBUF];
    /* texture bindings: stage (0 vertex, 1 fragment), Metal texture and sampler
     * indices (-1: no sampler), GL texture unit */
    struct { int stage, tex, samp, unit; GLint size_loc; } texb[16];   /* size_loc: CG <sampler>_texsize */
    int ntex;
    GLuint vao;
    GLint yflip_loc;         /* Vita: mina_yflip (-1 framebuffers, +1 the screen) */
    /* Vita: a fragment shader that writes its depth tests it itself (GXM has
     * no depth output): Metal's compare in mina_depth_cmp, the target's depth
     * texture on unit dst_depth_unit, mina_dst_xform window position -> its coordinates */
    GLint depth_cmp_loc, dst_xform_loc;
    int dst_depth_unit;
    int shared_prog;         /* prog belongs to the program cache (one per shader pair, kept) */
} MPipeline;

/* GLSL translated from the game's metallibs (generated: build/gen/shaders.c) */
typedef struct ShaderSrc {
    uint64_t hash;           /* FNV-1a 64 of the metallib */
    int stage;
    const char *glsl;
    int discards_clear0;     /* fragment: discards where texture 0's alpha is 0 (a cleared layer draws nothing) */
    int gamma_copy0;         /* fragment: (pow(texture 0 .rgb, fcb1[0].x), 1) at the quad's coordinates */
    unsigned rgb_only;       /* fragment: bit N, texture N's alpha is never read */
    int writes_depth;        /* fragment: writes its depth (CG: tests it itself, mina_depth_pass) */
} ShaderSrc;
extern const ShaderSrc g_shaders[];
extern const int g_shader_count;

/* Where the renderer is (a watchdog reports it when no frame comes for seconds):
 * cheap stores at the steps of a draw */
typedef struct RDbg {
    int stage;               /* 1 draw_setup ... 9 drawn; 20 present */
    const void *pipe;
    int vs, fs, ntex, k;
    uint32_t draws, presents;
} RDbg;
extern volatile RDbg g_rdbg;

/* ---- GPU lock: GL work happens with the context current on this thread ------------- */
/* GL state the draws keep (metal_enc.c) is known only between draws: any
 * other code that changes GL state (passes, copies, blits, uploads) says so */
extern uint32_t g_gl_epoch;
#define GL_DIRTY() (g_gl_epoch++)
void gpu_lock(void);
void gpu_unlock(void);

#endif

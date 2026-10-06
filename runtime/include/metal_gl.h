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

typedef struct PropBag {
    struct { const char *key; uint64_t v; double d[4]; } p[PB_MAX];
    int n;
} PropBag;

uint64_t pb_get(uint64_t obj, const char *key, uint64_t def);
double pb_getd(uint64_t obj, const char *key, int i, double def);
void pb_set(uint64_t obj, const char *key, uint64_t v);
/* child object (created on first access) */
uint64_t pb_child(uint64_t obj, const char *key, Class *cls);

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
} MBuffer;

/* guest memory of a shared buffer (given back after a whole-buffer upload: made again on use) */
uint32_t metal_buf_mem(MBuffer *b);

typedef struct MTexture {
    uint32_t type, format, width, height, depth, mips, samples, usage, storage;
    GLuint gl;
    GLuint fbo_cache;
    uint32_t staging;        /* guest copy for CPU-visible textures (getBytes / replaceRegion) */
    int is_drawable;
    int is_rb;               /* a renderbuffer (Vita depth/stencil), not a texture */
    uint32_t shift;          /* Vita: the GL texture is 2^shift times smaller (sampled only) */
    GLuint depth_tex;        /* Vita: a texture showing a sampled depth renderbuffer (vitaGL patch) */
} MTexture;

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
} MPipeline;

/* GLSL translated from the game's metallibs (generated: build/gen/shaders.c) */
typedef struct ShaderSrc {
    uint64_t hash;           /* FNV-1a 64 of the metallib */
    int stage;
    const char *glsl;
} ShaderSrc;
extern const ShaderSrc g_shaders[];
extern const int g_shader_count;

/* ---- GPU lock: GL work happens with the context current on this thread ------------- */
void gpu_lock(void);
void gpu_unlock(void);

#endif

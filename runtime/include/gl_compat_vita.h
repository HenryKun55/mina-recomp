/*
 * The desktop GL calls of the Metal layer that vitaGL does not have, written
 * with what it has. The game draws to one color attachment at a time and runs
 * without MSAA on the Vita, so the per-attachment forms map to the global ones.
 */
#ifndef MINA_GL_COMPAT_VITA_H
#define MINA_GL_COMPAT_VITA_H

#include <vitaGL.h>

#ifndef GL_COLOR
#define GL_COLOR 0x1800
#define GL_DEPTH 0x1801
#define GL_STENCIL 0x1802
#endif
#ifndef GL_TEXTURE_2D_MULTISAMPLE
#define GL_TEXTURE_2D_MULTISAMPLE 0x9100
#endif
#ifndef GL_TEXTURE_2D_ARRAY
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#endif
#ifndef GL_DEPTH32F_STENCIL8
#define GL_DEPTH32F_STENCIL8 0x8CAD
#endif
#ifndef GL_FLOAT_32_UNSIGNED_INT_24_8_REV
#define GL_FLOAT_32_UNSIGNED_INT_24_8_REV 0x8DAD
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif
#ifndef GL_R11F_G11F_B10F
#define GL_R11F_G11F_B10F 0x8C3A
#define GL_UNSIGNED_INT_10F_11F_11F_REV 0x8C3B
#endif
#ifndef GL_RGB10_A2
#define GL_RGB10_A2 0x8059
#define GL_UNSIGNED_INT_2_10_10_10_REV 0x8368
#endif
#ifndef GL_RGBA16
#define GL_RGBA16 0x805B
#endif
#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM
#define GL_COMPRESSED_RGBA_BPTC_UNORM 0x8E8C
#endif
#ifndef GL_TEXTURE_COMPARE_MODE
#define GL_TEXTURE_COMPARE_MODE 0x884C
#define GL_TEXTURE_COMPARE_FUNC 0x884D
#define GL_COMPARE_REF_TO_TEXTURE 0x884E
#endif
#ifndef GL_ACTIVE_UNIFORMS
#define GL_ACTIVE_UNIFORMS 0x8B86
#endif
#ifndef GL_PACK_ROW_LENGTH
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif
#ifndef GL_CLAMP_TO_BORDER
#define GL_CLAMP_TO_BORDER 0x812D
#endif
#ifndef GL_TEXTURE_WRAP_R
#define GL_TEXTURE_WRAP_R 0x8072
#endif
#ifndef GL_TEXTURE_MIN_LOD
#define GL_TEXTURE_MIN_LOD 0x813A
#define GL_TEXTURE_MAX_LOD 0x813B
#endif

/* GLES3 / desktop enums vitaGL's header lacks (the formats fall back at run time) */
#ifndef GL_R16F
#define GL_R16F 0x822D
#define GL_RG8 0x822B
#define GL_R32F 0x822E
#define GL_RG16F 0x822F
#define GL_RG32F 0x8230
#define GL_RGBA32F 0x8814
#endif
#ifndef GL_DEPTH_STENCIL
#define GL_DEPTH_STENCIL 0x84F9
#define GL_UNSIGNED_INT_24_8 0x84FA
#endif
#ifndef GL_CONSTANT_COLOR
#define GL_CONSTANT_COLOR 0x8001
#define GL_ONE_MINUS_CONSTANT_COLOR 0x8002
#define GL_CONSTANT_ALPHA 0x8003
#define GL_ONE_MINUS_CONSTANT_ALPHA 0x8004
#endif

static inline void mina_clear_color(GLint i, const GLfloat *c)
{
    (void)i;
    glClearColor(c[0], c[1], c[2], c[3]);
    glClear(GL_COLOR_BUFFER_BIT);
}
static inline void glClearBufferfv(GLenum buf, GLint i, const GLfloat *v)
{
    if (buf == GL_COLOR) {
        mina_clear_color(i, v);
    } else {
        glClearDepthf(v[0]);
        glClear(GL_DEPTH_BUFFER_BIT);
    }
}
static inline void glClearBufferiv(GLenum buf, GLint i, const GLint *v)
{
    (void)buf; (void)i;
    glClearStencil(v[0]);
    glClear(GL_STENCIL_BUFFER_BIT);
}

#define glColorMaski(i, r, g, b, a) do { if ((i) == 0) glColorMask((r), (g), (b), (a)); } while (0)
#define glEnablei(cap, i) do { if ((i) == 0) glEnable(cap); } while (0)
#define glDisablei(cap, i) do { if ((i) == 0) glDisable(cap); } while (0)
#define glBlendFuncSeparatei(i, a, b, c, d) do { if ((i) == 0) glBlendFuncSeparate((a), (b), (c), (d)); } while (0)
#define glBlendEquationSeparatei(i, a, b) do { if ((i) == 0) glBlendEquationSeparate((a), (b)); } while (0)
#define glDrawBuffers(n, b) ((void)(n), (void)(b))
#define glReadBuffer(b) ((void)(b))
#define glDepthRange(n, f) glDepthRangef((GLfloat)(n), (GLfloat)(f))
#define glVertexAttribIPointer(i, s, t, st, p) glVertexAttribPointer((i), (s), (t), GL_FALSE, (st), (p))
#define glTexImage2DMultisample(t, s, f, w, h, fx) ((void)0)
#define glPixelStorei(p, v) ((void)(p), (void)(v))
/* compressed textures are uploaded a whole level at a time */
#define glCompressedTexSubImage2D(t, l, x, y, w, h, f, sz, d) glCompressedTexImage2D((t), (l), (f), (w), (h), 0, (sz), (d))

#endif

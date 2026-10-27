/**
 * nv2a_pb_gl.c -- an OpenGL render back end for the pushbuffer executor.
 *
 * See nv2a_pb_gl.h for what this is for and which switches it reads.
 *
 * Shape of the thing:
 *
 *   main thread      nv2a_pb_gl_init(): SDL video, a hidden window, a 3.3 core
 *                    context, the shader, nv2a_backend_register()
 *   poll thread      clear/draw: bind the off-screen target for this surface,
 *                    apply the NV2A state as GL state, draw the batch
 *   poll thread      flip: glReadPixels the target back into the title's
 *                    colour surface, which is where the window present (and
 *                    RECOMP_FB_DUMP, and a title that reads its framebuffer
 *                    back) already looks for it
 *
 * Targets are cached by (colour surface, size, depth surface) so switching
 * between a render target and the back buffer, or between two depth buffers,
 * keeps both colour and depth, the way the CPU rasteriser keeps a float z
 * buffer per zeta offset.
 *
 * ponytail: one texture unit, stage 0, no combiners, no fog -- that is the
 * whole back-end contract as nv2a_backend.h states it. Textures are decoded
 * to RGBA once on first use; RECOMP_PB_GL_TEXREFRESH re-uploads them for a
 * title that rewrites a texture in place (a streamed mip, a dynamic atlas).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend.h"
#include "nv2a_pb_gl.h"

#if defined(_WIN32)
/* The Windows build keeps the CPU rasteriser for now: this file exists there
 * only so the symbol resolves, and nv2a_pb_gl_init() reports "not available". */
int nv2a_pb_gl_init(void)
{
    return 0;
}
#else

#include <SDL.h>
#include <epoxy/gl.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* ── Device ─────────────────────────────────────────────────────────────── */

static SDL_Window   *s_win;
static SDL_GLContext s_ctx;
static int           s_ready;         /* context exists */
static int           s_enabled;       /* RECOMP_PB_GL asked for it */
static int           s_verbose;
static uint32_t      s_zscale_override;
static uint32_t      s_tex_refresh;   /* frames between re-uploads, 0 = once */

static GLuint s_prog;
static GLint  u_surf, u_origin, u_incz, u_tex, u_texsize, u_alphatest,
              u_alphafunc, u_alpharef, u_textured;

static const char *k_vs =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"      /* surface pixels */
    "layout(location=1) in float aZ;\n"       /* the title's depth units */
    "layout(location=2) in float aRhw;\n"     /* 1/w */
    "layout(location=3) in uint aColor;\n"    /* 0xAARRGGBB */
    "layout(location=4) in vec2 aUV;\n"       /* texels */
    "uniform vec2 uSurf;\n"
    "uniform vec2 uOrigin;\n"
    "uniform float uInvZ;\n"
    "out vec4 vColor;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    float w = (aRhw != 0.0) ? 1.0 / aRhw : 1.0;\n"
    /* Screen y runs down, GL's runs up: flip it here so the image comes out
     * the right way up, and take the winding flip that costs with it in
     * apply_state(). Everything else is a straight mapping of surface pixels
     * into the viewport. */
    "    vec2 ndc = vec2((aPos.x - uOrigin.x) / uSurf.x * 2.0 - 1.0,\n"
    "                    1.0 - (aPos.y - uOrigin.y) / uSurf.y * 2.0);\n"
    "    float z = clamp(aZ * uInvZ, 0.0, 1.0);\n"
    /* Carry the title's own w in gl_Position.w so GL's perspective-correct
     * interpolation weights the varyings the way the CPU rasteriser's
     * 1/w interpolation does. */
    "    gl_Position = vec4(ndc * w, z * w, w);\n"
    "    vColor = vec4(float((aColor >> 16u) & 255u),\n"
    "                  float((aColor >>  8u) & 255u),\n"
    "                  float((aColor      ) & 255u),\n"
    "                  float((aColor >> 24u) & 255u)) / 255.0;\n"
    "    vUV = aUV;\n"
    "}\n";

static const char *k_fs =
    "#version 330 core\n"
    "in vec4 vColor;\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec2 uTexSize;\n"
    "uniform int uTextured;\n"
    "uniform int uAlphaTest;\n"
    "uniform int uAlphaFunc;\n"
    "uniform float uAlphaRef;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "    vec4 c = vColor;\n"
    "    if (uTextured == 1)\n"
    "        c *= texture(uTex, vUV / uTexSize);\n"
    "    if (uAlphaTest == 1) {\n"
    "        float a = c.a;\n"
    "        int f = uAlphaFunc;\n"
    "        bool pass = (f == 0x200) ? false :\n"
    "                    (f == 0x201) ? a <  uAlphaRef :\n"
    "                    (f == 0x202) ? a == uAlphaRef :\n"
    "                    (f == 0x203) ? a <= uAlphaRef :\n"
    "                    (f == 0x204) ? a >  uAlphaRef :\n"
    "                    (f == 0x205) ? a != uAlphaRef :\n"
    "                    (f == 0x206) ? a >= uAlphaRef : true;\n"
    "        if (!pass) discard;\n"
    "    }\n"
    "    frag = c;\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[PBGL] shader compile failed: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static int program_build(void)
{
    GLuint vs = compile(GL_VERTEX_SHADER, k_vs);
    GLuint fs = compile(GL_FRAGMENT_SHADER, k_fs);
    GLint ok = 0;

    if (!vs || !fs)
        return 0;
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glLinkProgram(s_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(s_prog, sizeof log, NULL, log);
        fprintf(stderr, "[PBGL] program link failed: %s\n", log);
        return 0;
    }
    u_surf      = glGetUniformLocation(s_prog, "uSurf");
    u_origin    = glGetUniformLocation(s_prog, "uOrigin");
    u_incz      = glGetUniformLocation(s_prog, "uInvZ");
    u_tex       = glGetUniformLocation(s_prog, "uTex");
    u_texsize   = glGetUniformLocation(s_prog, "uTexSize");
    u_alphatest = glGetUniformLocation(s_prog, "uAlphaTest");
    u_alphafunc = glGetUniformLocation(s_prog, "uAlphaFunc");
    u_alpharef  = glGetUniformLocation(s_prog, "uAlphaRef");
    u_textured  = glGetUniformLocation(s_prog, "uTextured");
    return 1;
}

/* The vertex the executor hands over, in the layout the vertex shader reads. */
typedef struct {
    float    x, y, z, rhw;
    uint32_t color;
    float    u, v;
    uint32_t pad;
} GlVert;

static GLuint s_vao, s_vbo;
static GlVert *s_verts;               /* staging, grown to the largest batch */
static size_t  s_verts_cap;

/* The context was created on the main thread; every call below arrives on
 * the executor's poll thread, so each entry point makes it current there
 * first. epoxy resolves its entry points on first use from whatever context
 * is current, and aborts outright if there is none -- so a failed
 * MakeCurrent must skip the work, not fall through into GL. Failures are
 * reported once: the reason matters, the repetition does not. */
static int gl_ready(void)
{
    static int complained;
    /* Per thread: the context is current here already. A draw is not the
     * place to spend a driver round trip, and there are tens of them a
     * frame; the executor's poll thread is the only one that ever gets
     * here, so the flag stays true for the life of the run. */
    static __thread int t_current;

    if (t_current)
        return 1;
    if (SDL_GL_MakeCurrent(s_win, s_ctx) == 0) {
        /* The program stays current for the life of the context: nothing else
         * draws, and every uniform below is set against it. */
        glUseProgram(s_prog);
        t_current = 1;
        return 1;
    }
    if (!complained) {
        fprintf(stderr, "[PBGL] SDL_GL_MakeCurrent failed: %s\n",
                SDL_GetError());
        complained = 1;
    }
    return 0;
}

/* ── Targets: one off-screen colour+depth pair per surface ──────────────── */

typedef struct {
    uint32_t color_va, zeta_va, w, h;
    GLuint   fbo, tex, rbo;
    uint64_t used;
} GlTarget;

#define TARGET_MAX 4
static GlTarget s_targets[TARGET_MAX];
static GlTarget *s_cur;
static Nv2aSurface s_cur_surf;        /* the surface s_cur belongs to */
static int s_frame_dirty;             /* something was drawn since the readback */

static uint32_t z_divisor(const Nv2aRenderState *rs)
{
    uint32_t zf;

    if (s_zscale_override)
        return s_zscale_override;
    /* Zeta format in SURFACE_FORMAT bits 4-7: 1 is Z16, 2 is Z24S8. The
     * title's depth lives in that many bits, so one fixed divisor per format
     * keeps every draw and every clear in the same units -- which is the
     * contract: window z is compared as it is, never rescaled per draw. */
    zf = (rs->zeta_format >> 4) & 0xF;
    return zf == 1 ? 65536u : 16777216u;
}

static void target_release(GlTarget *t)
{
    if (t == s_cur) s_cur = NULL;
    if (t->fbo)  glDeleteFramebuffers(1, &t->fbo);
    if (t->tex)  glDeleteTextures(1, &t->tex);
    if (t->rbo)  glDeleteRenderbuffers(1, &t->rbo);
    memset(t, 0, sizeof *t);
}

static GlTarget *target_for(const Nv2aSurface *s, const Nv2aRenderState *rs)
{
    GlTarget *t, *victim = &s_targets[0];
    uint32_t i;

    (void)rs;
    if (!s || !s->width || !s->height)
        return NULL;
    for (i = 0; i < TARGET_MAX; i++) {
        t = &s_targets[i];
        if (t->fbo && t->color_va == s->color_va && t->zeta_va == rs->zeta_va
            && t->w == s->width && t->h == s->height) {
            t->used = ++s_frame_dirty;   /* LRU stamp */
            return t;
        }
        if (!t->fbo || t->used < victim->used)
            victim = t;
    }
    if (victim->fbo)
        target_release(victim);          /* surface resized or replaced */

    t = victim;
    memset(t, 0, sizeof *t);
    t->color_va = s->color_va;
    t->zeta_va  = rs->zeta_va;
    t->w = s->width;
    t->h = s->height;

    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)t->w, (GLsizei)t->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenRenderbuffers(1, &t->rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, t->rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                          (GLsizei)t->w, (GLsizei)t->h);

    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, t->tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, t->rbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[PBGL] target %ux%u incomplete\n", t->w, t->h);
        target_release(t);
        return NULL;
    }
    if (s_verbose)
        fprintf(stderr, "[PBGL] new target %ux%u colour %08X zeta %08X\n",
                t->w, t->h, t->color_va, t->zeta_va);
    return t;
}

static void target_bind(GlTarget *t, const Nv2aSurface *s)
{
    if (!t)
        return;
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, (GLsizei)t->w, (GLsizei)t->h);
    s_cur = t;
    s_cur_surf = *s;
}

/* ── Textures ───────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t offset, w, h, fmt, levels;
    GLuint   tex;
    uint64_t stamp;
} GlTex;

#define TEX_MAX 512
static GlTex s_tex[TEX_MAX];
static uint64_t s_tex_clock;

static GlTex *tex_find(const Nv2aTexture *t)
{
    uint32_t i;
    for (i = 0; i < TEX_MAX; i++) {
        GlTex *e = &s_tex[i];
        if (e->tex && e->offset == t->offset && e->w == t->width
            && e->h == t->height && e->fmt == t->color
            && e->levels == t->levels)
            return e;
    }
    return NULL;
}

static GlTex *tex_slot(void)
{
    GlTex *victim = &s_tex[0];
    uint32_t i;
    for (i = 0; i < TEX_MAX; i++) {
        if (!s_tex[i].tex)
            return &s_tex[i];
        if (s_tex[i].stamp < victim->stamp)
            victim = &s_tex[i];
    }
    if (victim->tex)
        glDeleteTextures(1, &victim->tex);
    memset(victim, 0, sizeof *victim);
    return victim;
}

static int tex_upload(GlTex *e, const Nv2aTexture *t)
{
    uint32_t *argb;
    size_t n = (size_t)t->width * t->height;
    uint8_t *rgba, *dst;
    size_t i;

    if (!n || n > (64u << 20))
        return 0;
    argb = (uint32_t *)malloc(n * 4);
    rgba = (uint8_t *)malloc(n * 4);
    if (!argb || !rgba) {
        free(argb);
        free(rgba);
        return 0;
    }
    if (!nv2a_backend_decode_texture(t, argb)) {
        free(argb);
        free(rgba);
        return 0;
    }
    /* AARRGGBB -> RGBA bytes, row-major from the decoder. */
    dst = rgba;
    for (i = 0; i < n; i++) {
        uint32_t c = argb[i];
        dst[0] = (uint8_t)(c >> 16);
        dst[1] = (uint8_t)(c >> 8);
        dst[2] = (uint8_t)c;
        dst[3] = (uint8_t)(c >> 24);
        dst += 4;
    }
    if (!e->tex)
        glGenTextures(1, &e->tex);
    glBindTexture(GL_TEXTURE_2D, e->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)t->width,
                 (GLsizei)t->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    free(argb);
    free(rgba);

    e->offset = t->offset;
    e->w = t->width;
    e->h = t->height;
    e->fmt = t->color;
    e->levels = t->levels;
    e->stamp = ++s_tex_clock;
    if (s_verbose)
        fprintf(stderr, "[PBGL] upload %08X %ux%u fmt %02X\n",
                t->offset, t->width, t->height, t->color);
    return 1;
}

/* Bind stage 0, uploading the texture the first time it is seen. */
static void texture_bind(const Nv2aTexture *t, uint32_t frame)
{
    GlTex *e = t ? tex_find(t) : NULL;

    glActiveTexture(GL_TEXTURE0);
    if (!t) {
        glBindTexture(GL_TEXTURE_2D, 0);
        glUniform1i(u_textured, 0);
        glUniform2f(u_texsize, 1.0f, 1.0f);
        return;
    }
    if (!e) {
        e = tex_slot();
        if (!tex_upload(e, t)) {
            glBindTexture(GL_TEXTURE_2D, 0);
            glUniform1i(u_textured, 0);
            glUniform2f(u_texsize, 1.0f, 1.0f);
            return;
        }
    } else if (s_tex_refresh && frame && (frame % s_tex_refresh) == 0) {
        tex_upload(e, t);
    }
    glBindTexture(GL_TEXTURE_2D, e->tex);

    /* Address mode: the NV2A's 1 is wrap, 2 mirror, everything else clamps,
     * which is what the CPU sampler's wrap_coord() does. */
    {
        GLint u = GL_REPEAT, v = GL_REPEAT;
        if (t->addr_u == 2) u = GL_MIRRORED_REPEAT;
        else if (t->addr_u != 1) u = GL_CLAMP_TO_EDGE;
        if (t->addr_v == 2) v = GL_MIRRORED_REPEAT;
        else if (t->addr_v != 1) v = GL_CLAMP_TO_EDGE;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, u);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, v);
    }
    /* Level 0 only, as the CPU sampler does: MAG 2 is tent, MIN 2/4/6 is tent
     * at level 0, everything else nearest. */
    {
        uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
        GLint f = (mag == 2 || min == 2 || min == 4 || min == 6)
                  ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    }
    glUniform1i(u_textured, 1);
    glUniform1i(u_tex, 0);
    glUniform2f(u_texsize, (float)t->width, (float)t->height);
}

/* ── State ──────────────────────────────────────────────────────────────── */

static void apply_state(const Nv2aRenderState *rs, const Nv2aSurface *s,
                        uint32_t frame)
{
    static const uint32_t none = 0;

    glUniform2f(u_surf, (float)s->width, (float)s->height);
    glUniform2f(u_origin, (float)s->x, (float)s->y);
    glUniform1f(u_incz, 1.0f / (float)z_divisor(rs));

    if (rs->blend_enable) {
        glEnable(GL_BLEND);
        glBlendFunc(rs->blend_src, rs->blend_dst);   /* NV2A carries GL's
                                                      * own factor enums */
        glBlendEquation(rs->blend_eq);
        {
            uint32_t c = rs->blend_color;
            glBlendColor((float)((c >> 16) & 0xFF) / 255.0f,
                         (float)((c >>  8) & 0xFF) / 255.0f,
                         (float)( c        & 0xFF) / 255.0f,
                         (float)((c >> 24) & 0xFF) / 255.0f);
        }
    } else {
        glDisable(GL_BLEND);
    }

    if (rs->depth_test_enable) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);
    glDepthFunc(rs->depth_func ? rs->depth_func : 0x207);
    glDepthMask(rs->depth_write ? GL_TRUE : GL_FALSE);

    glUniform1i(u_alphatest, rs->alpha_test_enable ? 1 : 0);
    glUniform1i(u_alphafunc, (int)rs->alpha_func);
    glUniform1f(u_alpharef, (float)(rs->alpha_ref & 0xFF) / 255.0f);

    {
        uint32_t m = rs->color_mask;
        glColorMask((m >> 16) & 1, (m >> 8) & 1, m & 1, (m >> 24) & 1);
    }

    if (rs->cull_enable) {
        glEnable(GL_CULL_FACE);
        glCullFace(rs->cull_face ? rs->cull_face : 0x0405);
        /* Our y axis is the NV2A's flipped (see the vertex shader), so the
         * winding that reads "front" to us is the one it calls back. Swap the
         * two and the same triangles survive as under the NV2A's own
         * projected-position facing rule. */
        glFrontFace(rs->front_face == 0x0900 ? 0x0901 : 0x0900);
    } else {
        glDisable(GL_CULL_FACE);
    }

    if (rs->stencil_enable) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(rs->stencil_func, (int)rs->stencil_ref,
                      rs->stencil_func_mask);
        glStencilOp(rs->stencil_fail, rs->stencil_zfail, rs->stencil_zpass);
        glStencilMask(rs->stencil_write_mask);
    } else {
        glDisable(GL_STENCIL_TEST);
    }

    texture_bind(NULL, frame);   /* caller rebinds when the batch has one */
    (void)none;
}

/* ── The back end ───────────────────────────────────────────────────────── */

static uint32_t s_frame;

static void gl_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    GlTarget *t;
    GLbitfield bits = 0;
    uint32_t zf;

    if (!s_ready)
        return;
    if (!gl_ready())
        return;
    t = target_for(s, rs);
    if (!t)
        return;
    target_bind(t, s);
    apply_state(rs, s, s_frame);

    if (flags & 0xF0) {                      /* colour */
        glClearColor((float)((argb >> 16) & 0xFF) / 255.0f,
                     (float)((argb >>  8) & 0xFF) / 255.0f,
                     (float)( argb        & 0xFF) / 255.0f,
                     (float)((argb >> 24) & 0xFF) / 255.0f);
        bits |= GL_COLOR_BUFFER_BIT;
    }
    zf = (rs->zeta_format >> 4) & 0xF;
    if (flags & 0x01) {                      /* depth */
        double z = (zf == 1 ? (double)(zstencil & 0xFFFF)
                            : (double)(zstencil >> 8)) / (double)z_divisor(rs);
        glClearDepth(z < 0.0 ? 0.0 : (z > 1.0 ? 1.0 : z));
        bits |= GL_DEPTH_BUFFER_BIT;
    }
    if (flags & 0x02) {                      /* stencil */
        glClearStencil((int)(zstencil & 0xFF));
        bits |= GL_STENCIL_BUFFER_BIT;
    }
    if (bits) {
        glClear(bits);
        s_frame_dirty = 1;
    }
}

static uint64_t s_dropped_tris;

/* A vertex no screen could hold: non-finite, or millions of pixels out. The
 * CPU rasteriser never sees these (its span test clips them away), but GL
 * would happily rasterise whatever triangle they belong to, and a camera
 * glitch or an unset attribute stream turns into a full-screen wedge that
 * hides the scene behind it. Dropped, and counted. */
static int vert_usable(const Nv2aVertex *v)
{
    return isfinite(v->x) && isfinite(v->y) && isfinite(v->z)
        && isfinite(v->rhw) && fabsf(v->x) < 1e6f && fabsf(v->y) < 1e6f;
}

static void gl_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    GlTarget *t;
    size_t need, out;
    uint32_t i, dropped = 0;

    if (!s_ready || !b || !b->count)
        return;
    if (!gl_ready())
        return;
    t = target_for(s, b->state);
    if (!t)
        return;
    target_bind(t, s);
    apply_state(b->state, s, s_frame);

    need = b->count;
    if (need > s_verts_cap) {
        GlVert *grown = (GlVert *)realloc(s_verts, need * sizeof *grown);
        if (!grown)
            return;
        s_verts = grown;
        s_verts_cap = need;
    }
    for (i = 0, out = 0; i + 2 < b->count; i += 3) {
        const Nv2aVertex *v0 = &b->vertices[i];
        const Nv2aVertex *v1 = &b->vertices[i + 1];
        const Nv2aVertex *v2 = &b->vertices[i + 2];
        GlVert *o = &s_verts[out];

        if (!vert_usable(v0) || !vert_usable(v1) || !vert_usable(v2)) {
            dropped++;
            continue;
        }
        o[0].x = v0->x; o[0].y = v0->y; o[0].z = v0->z; o[0].rhw = v0->rhw;
        o[0].color = v0->diffuse; o[0].u = v0->u; o[0].v = v0->v;
        o[1].x = v1->x; o[1].y = v1->y; o[1].z = v1->z; o[1].rhw = v1->rhw;
        o[1].color = v1->diffuse; o[1].u = v1->u; o[1].v = v1->v;
        o[2].x = v2->x; o[2].y = v2->y; o[2].z = v2->z; o[2].rhw = v2->rhw;
        o[2].color = v2->diffuse; o[2].u = v2->u; o[2].v = v2->v;
        o[0].pad = o[1].pad = o[2].pad = 0;
        out += 3;
    }
    if (dropped) {
        s_dropped_tris += dropped;
        if (s_verbose && s_dropped_tris == dropped)
            fprintf(stderr, "[PBGL] dropping unusable triangles "
                            "(non-finite or off-scale vertices)\n");
    }
    if (!out)
        return;

    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(out * sizeof(GlVert)),
                 s_verts, GL_STREAM_DRAW);

    texture_bind(b->texture, s_frame);

    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)out);
    s_frame_dirty = 1;
}

/* Put the frame where the rest of the runtime already looks for it: the
 * title's colour surface, in the layout the window present reads. */
static void readback(void)
{
    const Nv2aSurface *s = &s_cur_surf;
    uint8_t *mem, *dst;
    uint32_t y, w, h, bpp, pitch;
    size_t rowbytes;

    if (!s_cur || !s->width || !s->height || !s->bytes_per_pixel || !s->color_va || !s->pitch)
        return;
    bpp = s->bytes_per_pixel;
    if (bpp != 2 && bpp != 4)
        return;                       /* the present path handles 1 and 8 */
    w = s->width;
    h = s->height;
    pitch = s->pitch;
    rowbytes = (size_t)w * bpp;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_cur->fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    {
        static uint8_t *buf;
        static size_t cap;
        size_t need = (size_t)w * h * 4;
        if (need > cap) {
            uint8_t *grown = (uint8_t *)realloc(buf, need);
            if (!grown)
                return;
            buf = grown;
            cap = need;
        }
        glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA,
                     GL_UNSIGNED_BYTE, buf);
        mem = (uint8_t *)xbox_GetMemoryOffset();
        dst = mem + s->color_va + (size_t)s->y * pitch + (size_t)s->x * bpp;
        /* GL's row 0 is the bottom of the image; the surface's is the top. */
        for (y = 0; y < h; y++) {
            const uint8_t *src = buf + (size_t)(h - 1 - y) * w * 4;
            uint8_t *d = dst + (size_t)y * pitch;
            uint32_t x;
            if (bpp == 4) {
                for (x = 0; x < w; x++) {   /* RGBA bytes -> AARRGGBB dword */
                    d[0] = src[2];
                    d[1] = src[1];
                    d[2] = src[0];
                    d[3] = src[3];
                    d += 4;
                    src += 4;
                }
            } else {
                for (x = 0; x < w; x++) {
                    uint32_t r = src[0], g = src[1], bl = src[2];
                    uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5)
                                            | (bl >> 3));
                    d[0] = (uint8_t)v;
                    d[1] = (uint8_t)(v >> 8);
                    d += 2;
                    src += 4;
                }
            }
        }
    }
    (void)rowbytes;
}

static void gl_flip(void)
{
    if (!s_ready || !gl_ready())
        return;
    if (s_frame_dirty && s_cur) {
        readback();
        s_frame_dirty = 0;
    }
    s_frame++;
}

static const Nv2aBackend s_backend = { gl_clear, gl_draw, gl_flip };

/* ── Bring-up ───────────────────────────────────────────────────────────── */

int nv2a_pb_gl_init(void)
{
    const char *e;
    int major = 3, minor = 3;

    /* On by default: the point of this back end is that the GPU draws and
     * the CPU does not. RECOMP_PB_GL=0 keeps the CPU rasteriser, which is
     * still the fallback for titles whose look needs combiners or fog. */
    e = getenv("RECOMP_PB_GL");
    if (e && e[0] == '0' && !e[1])
        return 0;
    s_enabled = 1;
    s_verbose = getenv("RECOMP_PB_GL_VERBOSE") != NULL;
    e = getenv("RECOMP_PB_GL_ZSCALE");
    if (e && *e)
        s_zscale_override = (uint32_t)strtoul(e, NULL, 0);
    e = getenv("RECOMP_PB_GL_TEXREFRESH");
    if (e && *e)
        s_tex_refresh = (uint32_t)strtoul(e, NULL, 0);

    /* SDL installs SIGINT/SIGTERM handlers that push an SDL_QUIT event, and
     * nothing here pumps events for the hidden window -- so a headless run
     * that initialized SDL video could no longer be stopped with Ctrl+C or
     * `timeout`, which is how every script in tools/ ends a run. Asked for
     * before SDL_Init, as the input layer already does (xinput_device.c). */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[PBGL] SDL_Init: %s\n", SDL_GetError());
        return 0;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    /* Hidden: the visible window is the framebuffer window, which keeps
     * presenting what readback() writes. This one exists for the context. */
    s_win = SDL_CreateWindow("otogi1 gl",
                             SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                             640, 480, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!s_win) {
        fprintf(stderr, "[PBGL] SDL_CreateWindow: %s\n", SDL_GetError());
        return 0;
    }
    s_ctx = SDL_GL_CreateContext(s_win);
    if (!s_ctx) {
        fprintf(stderr, "[PBGL] SDL_GL_CreateContext: %s\n", SDL_GetError());
        return 0;
    }
    SDL_GL_MakeCurrent(s_win, s_ctx);
    if (SDL_GL_SetSwapInterval(0) != 0) { /* not fatal: no swap is done here */ }

    if (!program_build())
        return 0;

    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GlVert),
                          (const void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, sizeof(GlVert),
                          (const void *)8);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(GlVert),
                          (const void *)12);
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(3, 1, GL_UNSIGNED_INT, sizeof(GlVert),
                           (const void *)16);
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, sizeof(GlVert),
                          (const void *)20);

    glDisable(GL_DITHER);
    glEnable(GL_MULTISAMPLE);

    s_ready = 1;
    nv2a_backend_register(&s_backend);
    if (s_verbose || 1)
        fprintf(stderr, "[PBGL] OpenGL %s, %s\n",
                (const char *)glGetString(GL_VERSION),
                (const char *)glGetString(GL_RENDERER));
    /* Release it here: GLX lets a context be current on one thread only, and
     * the executor's poll thread is the one that will use it. */
    SDL_GL_MakeCurrent(s_win, NULL);
    return 1;
}

#endif /* !_WIN32 */

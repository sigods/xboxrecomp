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
#include <time.h>

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
static GLint  u_surf, u_origin, u_incz, u_tex, u_tex1, u_tex2, u_tex3,
              u_alphatest, u_alphafunc, u_alpharef, u_textured;
/* The register combiners, as uniforms: uRcCin/uRcAin hold the ICW bytes, the
 * two output registers and their flag halves, and the factors. The GLSL below
 * is nv2a_combiner.c's nv2a_rc_eval written for a fragment shader -- the same
 * arithmetic on the same registers, because "diffuse times stage 0" is a
 * different function (it paints opaque black where a combiner takes its
 * colour from the alpha channel, which is every menu's text). */static GLint  u_rc_on, u_rc_stages, u_rc_muxalpha, u_rc_stpar, u_rc_cin,
              u_rc_ain, u_rc_cout, u_rc_flags, u_rc_c0, u_rc_c1, u_rc_final,
              u_rc_fc0, u_rc_fc1, u_rc_fog, u_rc_clip, u_flat;
static GLuint s_white;              /* 1x1 white: an unbound stage's texel */

static const char *k_vs =
    "#version 330 core\n"
    "layout(location=0) in vec4 aPos;\n"      /* x, y, z, 1/w */
    "layout(location=1) in uvec2 aCol;\n"     /* diffuse, specular */
    "layout(location=2) in vec4 aSt0;\n"      /* stage 0: s, t, q, - */
    "layout(location=3) in vec4 aSt1;\n"
    "layout(location=4) in vec4 aSt2;\n"
    "layout(location=5) in vec4 aSt3;\n"
    "layout(location=6) in float aFog;\n"     /* interpolated fog factor */
    "uniform vec2 uSurf;\n"
    "uniform vec2 uOrigin;\n"
    "uniform float uInvZ;\n"
    "out vec4 vColor;\n"
    "out vec4 vSpec;\n"
    "out vec4 vSt0;\n"
    "out vec4 vSt1;\n"
    "out vec4 vSt2;\n"
    "out vec4 vSt3;\n"
    "out float vFog;\n"
    "vec4 uncol(uint c) {\n"
    "    return vec4(float((c >> 16u) & 255u), float((c >> 8u) & 255u),\n"
    "                float(c & 255u), float((c >> 24u) & 255u)) / 255.0;\n"
    "}\n"
    "void main() {\n"
    "    float w = (aPos.w != 0.0) ? 1.0 / aPos.w : 1.0;\n"
    /* Screen y runs down, GL's runs up: flip it here so the image comes out
     * the right way up, and take the winding flip that costs with it in
     * apply_state(). Everything else is a straight mapping of surface pixels
     * into the viewport. */
    "    vec2 ndc = vec2((aPos.x - uOrigin.x) / uSurf.x * 2.0 - 1.0,\n"
    "                    1.0 - (aPos.y - uOrigin.y) / uSurf.y * 2.0);\n"
    "    float z = clamp(aPos.z * uInvZ, 0.0, 1.0);\n"
    /* Carry the title's own w in gl_Position.w so GL's perspective-correct
     * interpolation weights the varyings the way the CPU rasteriser's
     * 1/w interpolation does. */
    "    gl_Position = vec4(ndc * w, z * w, w);\n"
    "    vColor = uncol(aCol.x);\n"
    "    vSpec  = uncol(aCol.y);\n"
    "    vSt0 = aSt0; vSt1 = aSt1; vSt2 = aSt2; vSt3 = aSt3;\n"
    "    vFog = aFog;\n"
    "}\n";

/* The combiner, and the alpha test that reads its result. Register numbers,
 * input mappings and the final combiner are nv2a_combiner.h's. */
static const char *k_fs =
    "#version 330 core\n"
    "in vec4 vColor;\n"
    "in vec4 vSpec;\n"
    "in vec4 vSt0;\n"
    "in vec4 vSt1;\n"
    "in vec4 vSt2;\n"
    "in vec4 vSt3;\n"
    "in float vFog;\n"
    "uniform sampler2D uTex;\n"        /* stage 0 */
    "uniform sampler2D uTex1;\n"
    "uniform sampler2D uTex2;\n"
    "uniform sampler2D uTex3;\n"
    "uniform int uTextured;\n"
    "uniform int uAlphaTest;\n"
    "uniform int uAlphaFunc;\n"
    "uniform float uAlphaRef;\n"
    "uniform int uRcOn;\n"
    "uniform int uRcStages;\n"
    "uniform int uRcMuxAlpha;\n"
    "uniform uint uRcStPar;\n"
    "uniform uvec4 uRcClip[8];\n"
    "uniform uvec4 uRcCin[8];\n"
    "uniform uvec4 uRcAin[8];\n"
    "uniform uvec2 uRcCout[8];\n"
    "uniform uvec2 uRcFlags[8];\n"
    "uniform vec4 uRcC0[8];\n"
    "uniform vec4 uRcC1[8];\n"
    "uniform uvec2 uRcFinal;\n"
    "uniform vec4 uRcFC0;\n"
    "uniform vec4 uRcFC1;\n"
    "uniform vec4 uRcFog;\n"
    "uniform int uFlat;\n"
    "out vec4 frag;\n"
    /* PS_INPUTMAPPING (nv2a_combiner.c map_in). */
    "float rc_in(float x, uint byte) {\n"
    "    float p = max(x, 0.0);\n"
    "    switch ((byte >> 5) & 7u) {\n"
    "    case 0u: return p;\n"
    "    case 1u: return 1.0 - clamp(x, 0.0, 1.0);\n"
    "    case 2u: return 2.0 * p - 1.0;\n"
    "    case 3u: return -2.0 * p + 1.0;\n"
    "    case 4u: return p - 0.5;\n"
    "    case 5u: return -p + 0.5;\n"
    "    case 6u: return x;\n"
    "    default: return -x;\n"
    "    }\n"
    "}\n"
    "float rc_out(float x, uint m) {\n"
    "    if (m == 0x08u) return x - 0.5;\n"
    "    if (m == 0x10u) return x * 2.0;\n"
    "    if (m == 0x18u) return (x - 0.5) * 2.0;\n"
    "    if (m == 0x20u) return x * 4.0;\n"
    "    if (m == 0x30u) return x * 0.5;\n"
    "    return x;\n"
    "}\n"
    "vec3 rc_rgb(vec4 R[16], uint byte) {\n"
    "    vec4 r = R[byte & 0xFu];\n"
    "    vec3 s = ((byte & 0x10u) != 0u) ? r.aaa : r.rgb;\n"
    "    return vec3(rc_in(s.x, byte), rc_in(s.y, byte), rc_in(s.z, byte));\n"
    "}\n"
    "float rc_a(vec4 R[16], uint byte) {\n"
    "    vec4 r = R[byte & 0xFu];\n"
    "    return rc_in(((byte & 0x10u) != 0u) ? r.a : r.b, byte);\n"
    "}\n"
    /* One stage result, by its SHADER_STAGE_PROGRAM mode, and the same table
     * the CPU rasteriser's rc_stage_fetch() works from (nv2a_pb_exec.c):
     * 0 none, 1/2 a projected 2D sample, 3 a cubemap, 4 the coordinate
     * itself, 5 a clip plane, everything else the bump-env and dot-product
     * modes, which read zero until something depends on one.
     *
     * The clip-plane stage does not shade: it kills the pixel, which is why
     * this can discard from inside a function. A batch using one of these
     * modes used to be refused outright -- uRcOn stayed 0 and the whole
     * batch fell back to "diffuse times stage 0" -- and the title's player
     * model and bamboo are exactly such batches: they came out black, and
     * RECOMP_GL_FLAT showed every one of their triangles arriving and passing
     * every raster gate. Matching the CPU table here is what paints them. */
    "vec4 rc_stage(uint st, uint mode, vec4 c, sampler2D tx) {\n"
    "    if (mode == 0u) return vec4(0.0, 0.0, 0.0, 1.0);\n"
    "    if (mode == 4u) return clamp(c, 0.0, 1.0);\n"
    "    if (mode == 5u) {\n"
    "        for (int j = 0; j < 4; j++) {\n"
    "            bool ge = uRcClip[st][j] != 0u;\n"
    "            if (ge ? c[j] >= 0.0 : c[j] < 0.0) discard;\n"
    "        }\n"
    "        return vec4(0.0);\n"
    "    }\n"
    "    if (mode >= 6u) return vec4(0.0);\n"
    "    float q = (c.w != 0.0) ? c.w : 1.0;\n"
    "    return texture(tx, c.xy / q);\n"
    "}\n"
    "vec4 rc_eval(vec4 t0, vec4 t1, vec4 t2, vec4 t3) {\n"
    "    vec4 R[16];\n"
    "    for (int i = 0; i < 16; i++) R[i] = vec4(0.0);\n"
    "    R[3] = vec4(uRcFog.rgb, vFog);\n"
    "    R[4] = vColor;\n"
    "    R[5] = vSpec;\n"
    "    R[8] = t0; R[9] = t1; R[10] = t2; R[11] = t3;\n"
    "    R[12] = vec4(0.0, 0.0, 0.0, ((uRcStPar & 0x1Fu) != 0u) ? t0.a : 1.0);\n"
    "    for (int s = 0; s < uRcStages; s++) {\n"
    "        uint fl = uRcFlags[s].x, afl = uRcFlags[s].y;\n"
    "        vec3 A, B, C, D, ab, cd, ms;\n"
    "        float aA, aB, aC, aD, aab, acd, ams;\n"
    "        bool mux = uRcMuxAlpha != 0 ? (R[12].a >= 0.5)\n"
        "                                   : ((int(R[12].a * 255.0) & 1) != 0);\n"
    "        R[1] = uRcC0[s];\n"
    "        R[2] = uRcC1[s];\n"
    "        A = rc_rgb(R, uRcCin[s].x); B = rc_rgb(R, uRcCin[s].y);\n"
    "        C = rc_rgb(R, uRcCin[s].z); D = rc_rgb(R, uRcCin[s].w);\n"
    "        aA = rc_a(R, uRcAin[s].x); aB = rc_a(R, uRcAin[s].y);\n"
    "        aC = rc_a(R, uRcAin[s].z); aD = rc_a(R, uRcAin[s].w);\n"
    "        for (int i = 0; i < 3; i++) {\n"
    "            float dab = dot(A, B), dcd = dot(C, D);\n"
    "            float pab = ((fl & 2u) != 0u) ? dab : A[i] * B[i];\n"
    "            float pcd = ((fl & 1u) != 0u) ? dcd : C[i] * D[i];\n"
    "            float sum = ((fl & 4u) != 0u) ? (mux ? pcd : pab) : pab + pcd;\n"
    "            ab[i] = clamp(rc_out(pab, fl & 0x38u), -1.0, 1.0);\n"
    "            cd[i] = clamp(rc_out(pcd, fl & 0x38u), -1.0, 1.0);\n"
    "            ms[i] = clamp(rc_out(sum, fl & 0x38u), -1.0, 1.0);\n"
    "        }\n"
    "        {\n"
    "            float pab = aA * aB, pcd = aC * aD;\n"
    "            float sum = ((afl & 4u) != 0u) ? (mux ? pcd : pab) : pab + pcd;\n"
    "            aab = clamp(rc_out(pab, afl & 0x38u), -1.0, 1.0);\n"
    "            acd = clamp(rc_out(pcd, afl & 0x38u), -1.0, 1.0);\n"
    "            ams = clamp(rc_out(sum, afl & 0x38u), -1.0, 1.0);\n"
    "        }\n"
    "        {\n"
    "            uint d = (uRcCout[s].x >> 4) & 0xFu;\n"
    "            if (d != 0u)\n"
    "                R[d] = vec4(ab, ((fl & 0x80u) != 0u) ? ab.z : R[d].a);\n"
    "            d = uRcCout[s].x & 0xFu;\n"
    "            if (d != 0u)\n"
    "                R[d] = vec4(cd, ((fl & 0x40u) != 0u) ? cd.z : R[d].a);\n"
    "            d = (uRcCout[s].x >> 8) & 0xFu;\n"
    "            if (d != 0u)\n"
    "                R[d] = vec4(ms, R[d].a);\n"
    "            d = (uRcCout[s].y >> 4) & 0xFu;\n"
    "            if (d != 0u) R[d].a = aab;\n"
    "            d = uRcCout[s].y & 0xFu;\n"
    "            if (d != 0u) R[d].a = acd;\n"
    "            d = (uRcCout[s].y >> 8) & 0xFu;\n"
    "            if (d != 0u) R[d].a = ams;\n"
    "        }\n"
    "        R[0] = vec4(0.0);\n"
    "    }\n"
    "    vec4 out4;\n"
    "    if (uRcFinal.x != 0u || uRcFinal.y != 0u) {\n"
    "        uint f1 = uRcFinal.y, fflags = f1 & 0xFFu;\n"
    "        R[1] = uRcFC0;\n"
    "        R[2] = uRcFC1;\n"
    "        for (int i = 0; i < 3; i++) {\n"
    "            float a = ((fflags & 0x40u) != 0u) ? 1.0 - R[5][i] : R[5][i];\n"
    "            float b = ((fflags & 0x20u) != 0u) ? 1.0 - R[12][i] : R[12][i];\n"
    "            R[14][i] = ((fflags & 0x80u) != 0u) ? clamp(a + b, 0.0, 1.0)\n"
    "                                               : a + b;\n"
    "        }\n"
    "        R[14].a = 0.0;\n"
    "        R[15] = vec4(rc_rgb(R, f1 >> 24) * rc_rgb(R, f1 >> 16), 0.0);\n"
    "        vec3 A = rc_rgb(R, uRcFinal.x >> 24);\n"
    "        vec3 B = rc_rgb(R, uRcFinal.x >> 16);\n"
    "        vec3 C = rc_rgb(R, uRcFinal.x >> 8);\n"
    "        vec3 D = rc_rgb(R, uRcFinal.x);\n"
    "        out4 = vec4(D + C * (1.0 - A) + B * A, rc_a(R, f1 >> 8));\n"
    "    } else {\n"
    "        out4 = R[12];\n"
    "    }\n"
    "    return clamp(out4, 0.0, 1.0);\n"
    "}\n"
    "void main() {\n"
    "    if (uFlat == 1) { frag = vec4(1.0, 0.0, 1.0, 1.0); return; }\n"
    "    vec4 c;\n"
    "    if (uRcOn == 1) {\n"
    "        vec4 t0 = rc_stage(0u, (uRcStPar >> 0) & 0x1Fu, vSt0, uTex);\n"
    "        vec4 t1 = rc_stage(1u, (uRcStPar >> 5) & 0x1Fu, vSt1, uTex1);\n"
    "        vec4 t2 = rc_stage(2u, (uRcStPar >> 10) & 0x1Fu, vSt2, uTex2);\n"
    "        vec4 t3 = rc_stage(3u, (uRcStPar >> 15) & 0x1Fu, vSt3, uTex3);\n"
    "        c = rc_eval(t0, t1, t2, t3);\n"
    "    } else {\n"
    "        c = vColor;\n"
    "        if (uTextured == 1)\n"
    "            c *= texture(uTex, vSt0.xy);\n"
    "    }\n"
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
    u_tex1      = glGetUniformLocation(s_prog, "uTex1");
    u_tex2      = glGetUniformLocation(s_prog, "uTex2");
    u_tex3      = glGetUniformLocation(s_prog, "uTex3");
    u_alphatest = glGetUniformLocation(s_prog, "uAlphaTest");
    u_alphafunc = glGetUniformLocation(s_prog, "uAlphaFunc");
    u_alpharef  = glGetUniformLocation(s_prog, "uAlphaRef");
    u_textured  = glGetUniformLocation(s_prog, "uTextured");
    u_rc_on      = glGetUniformLocation(s_prog, "uRcOn");
    u_rc_stages  = glGetUniformLocation(s_prog, "uRcStages");
    u_rc_muxalpha = glGetUniformLocation(s_prog, "uRcMuxAlpha");
    u_rc_stpar   = glGetUniformLocation(s_prog, "uRcStPar");
    u_rc_cin     = glGetUniformLocation(s_prog, "uRcCin");
    u_rc_ain     = glGetUniformLocation(s_prog, "uRcAin");
    u_rc_cout    = glGetUniformLocation(s_prog, "uRcCout");
    u_rc_flags   = glGetUniformLocation(s_prog, "uRcFlags");
    u_rc_c0      = glGetUniformLocation(s_prog, "uRcC0");
    u_rc_c1      = glGetUniformLocation(s_prog, "uRcC1");
    u_rc_final   = glGetUniformLocation(s_prog, "uRcFinal");
    u_rc_fc0     = glGetUniformLocation(s_prog, "uRcFC0");
    u_rc_fc1     = glGetUniformLocation(s_prog, "uRcFC1");
    u_rc_clip    = glGetUniformLocation(s_prog, "uRcClip");
    u_rc_fog     = glGetUniformLocation(s_prog, "uRcFog");
    u_flat       = glGetUniformLocation(s_prog, "uFlat");
    return 1;
}

/* The vertex the executor hands over, in the layout the vertex shader reads:
 * position and 1/w, the two colours, every stage's texture coordinate (s, t,
 * q -- the combiners read all four stages), and the fog factor. */
typedef struct {
    float    x, y, z, rhw;
    uint32_t color, specular;
    float    st[4][4];
    float    fogf;
    float    pad[3];
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
    /* `pitch` is part of the identity, not decoration: for a linear format it
     * is the row stride the decoder walks, so the same offset and size bound
     * with a different pitch is a different image. Otogi binds its 640x480
     * A8R8G8B8 surfaces this way (pitch 2560), and a cache that ignored the
     * field would keep the first decode of an offset that a later bind reads
     * at another stride. */
    uint32_t offset, w, h, fmt, levels, pitch;
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
            && e->levels == t->levels && e->pitch == t->pitch)
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
    e->pitch = t->pitch;
    e->stamp = ++s_tex_clock;
    if (s_verbose)
        fprintf(stderr, "[PBGL] upload %08X %ux%u fmt %02X\n",
                t->offset, t->width, t->height, t->color);
    return 1;
}

/* Address mode and filter, as the CPU sampler applies them: the NV2A's 1 is
 * wrap, 2 mirror, everything else clamps; MAG 2 is tent, MIN 2/4/6 is tent at
 * level 0, everything else nearest. */
static void tex_params(const Nv2aTexture *t)
{
    GLint u = GL_REPEAT, v = GL_REPEAT;
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    GLint f;

    if (t->addr_u == 2) u = GL_MIRRORED_REPEAT;
    else if (t->addr_u != 1) u = GL_CLAMP_TO_EDGE;
    if (t->addr_v == 2) v = GL_MIRRORED_REPEAT;
    else if (t->addr_v != 1) v = GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, u);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, v);
    f = (mag == 2 || min == 2 || min == 4 || min == 6) ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
}

/* Which stages a draw has a texture bound on: the combiner shader samples
 * them, and a stage with none reads as the CPU sampler's failure colour (white)
 * rather than an unbound GL texture. */
static int s_stage_ok[4];

/* Bind one texture stage, uploading the texture the first time it is seen.
 * Returns 0 when there is nothing to bind (or the format cannot be decoded),
 * which the caller reports to the shader as "stage off". */
static int stage_bind(uint32_t unit, const Nv2aTexture *t, uint32_t frame)
{
    GlTex *e = t ? tex_find(t) : NULL;

    glActiveTexture(GL_TEXTURE0 + unit);
    if (!t) {
        glBindTexture(GL_TEXTURE_2D, 0);
        return 0;
    }
    if (!e) {
        e = tex_slot();
        if (!tex_upload(e, t)) {
            static uint32_t complained;
            glBindTexture(GL_TEXTURE_2D, 0);
            if (complained++ < 4)
                fprintf(stderr, "[PBGL] texture %08X %ux%u fmt %02X will not "
                                "decode; drawn untextured\n", t->offset,
                        t->width, t->height, t->color);
            return 0;
        }
    } else if (s_tex_refresh && frame && (frame % s_tex_refresh) == 0) {
        tex_upload(e, t);
    }
    glBindTexture(GL_TEXTURE_2D, e->tex);
    tex_params(t);
    return 1;
}

/* Bind stage 0 and tell the shader about it (the fallback pixel path). */
static void texture_bind(const Nv2aTexture *t, uint32_t frame)
{
    if (!stage_bind(0, t, frame)) {
        glUniform1i(u_textured, 0);
        return;
    }
    glUniform1i(u_textured, 1);
    glUniform1i(u_tex, 0);
}

/* ── State ──────────────────────────────────────────────────────────────── */

/* Does this shader evaluate the stage's mode? Every mode the CPU rasteriser
 * implements is now handled by rc_stage(), which follows nv2a_pb_exec.c's
 * rc_stage_fetch() table; there is nothing left to refuse, so the combiner
 * runs for every batch that has one. */
/* Which stage modes the title uses, counted across every batch of a run and
 * printed once, so "a mode this shader samples as 2D" can be told from "a mode
 * that reads zero" rather than guessed from one wrong-looking model. */
static uint32_t s_mode_hist[32];

/* Per-batch combiner state -> uniforms. nv2a_rc_eval's inputs, decoded the
 * same way the CPU rasteriser decodes them: the ICW bytes go up as they are
 * (the shader's rc_in/rc_rgb apply the mapping), the output registers packed
 * three nibbles at a time, and the factors resolved per stage here so the
 * shader needs no decision about COMBINERCOUNT. */
static int rc_upload(const Nv2aBatch *b, const Nv2aVertex *v)
{
    const Nv2aCombiner *rc = b->combiner;
    uint32_t cin[8][4], ain[8][4], cout[8][2], flags[8][2];
    float c0[8][4], c1[8][4], f0[4], f1[4];
    int on = 0, i;
    uint32_t n, ctl, per_stage_c0, per_stage_c1;

    (void)v;
    /* The CPU rasteriser evaluates the combiner whenever the title has
     * programmed one (rc_seen), and falls back to modulate only otherwise.
     * Matching that exactly is the point: a batch whose combiner reads (0,0,0,1)
     * for an off stage is a different draw from "diffuse times texture". */
    if (!rc)
        goto off;
    for (i = 0; i < 4; i++)
        s_mode_hist[(rc->stage_program >> (i * 5)) & 0x1F]++;
    /* Every stage mode runs the combiner. Refusing the batch to the fallback
     * (the old "mode not implemented" exit) cost the title its player model
     * and its bamboo: those batches use modes this shader samples as a plain
     * 2D lookup, and disabling the combiner painted them v0 times a texel,
     * which is black. rc_stage() now implements the same table as the CPU
     * rasteriser's rc_stage_fetch(), so the modes that read zero and the one
     * that kills the pixel do so here too. */
    ctl = rc->control;
    n = ctl & 0xFF;
    if (n > 8)
        n = 8;
    per_stage_c0 = (ctl >> 8) & 0x10;    /* COMBINERCOUNT bit for c0 */
    per_stage_c1 = (ctl >> 8) & 0x100;   /* ... and for c1 */
    for (i = 0; i < 8; i++) {
        uint32_t s = (uint32_t)i;
        nv2a_rc_unpack(rc->factor0[per_stage_c0 ? s : 0], c0[i]);
        nv2a_rc_unpack(rc->factor1[per_stage_c1 ? s : 0], c1[i]);
        cin[i][0] = (rc->color_icw[i] >> 24) & 0xFF;
        cin[i][1] = (rc->color_icw[i] >> 16) & 0xFF;
        cin[i][2] = (rc->color_icw[i] >> 8) & 0xFF;
        cin[i][3] = rc->color_icw[i] & 0xFF;
        ain[i][0] = (rc->alpha_icw[i] >> 24) & 0xFF;
        ain[i][1] = (rc->alpha_icw[i] >> 16) & 0xFF;
        ain[i][2] = (rc->alpha_icw[i] >> 8) & 0xFF;
        ain[i][3] = rc->alpha_icw[i] & 0xFF;
        cout[i][0] = rc->color_ocw[i];
        cout[i][1] = rc->alpha_ocw[i];
        flags[i][0] = rc->color_ocw[i] >> 12;
        flags[i][1] = rc->alpha_ocw[i] >> 12;
    }
    nv2a_rc_unpack(rc->final_c0, f0);
    nv2a_rc_unpack(rc->final_c1, f1);
    {
        /* SET_CLIP_PLANE_MODE: one bit per stage per coordinate channel. */
        uint32_t clip[8][4];
        int j;
        for (i = 0; i < 8; i++)
            for (j = 0; j < 4; j++)
                clip[i][j] = (b->clip_plane_mode >> (i * 4 + j)) & 1u;
        glUniform4uiv(u_rc_clip, 8, (const GLuint *)clip);
    }
    on = 1;

    glUniform1i(u_rc_stages, (int)n);
    glUniform1i(u_rc_muxalpha, (int)((ctl >> 8) & 1));
    glUniform1ui(u_rc_stpar, rc->stage_program);
    glUniform4uiv(u_rc_cin, 8, (const GLuint *)cin);
    glUniform4uiv(u_rc_ain, 8, (const GLuint *)ain);
    glUniform2uiv(u_rc_cout, 8, (const GLuint *)cout);
    glUniform2uiv(u_rc_flags, 8, (const GLuint *)flags);
    glUniform4fv(u_rc_c0, 8, (const GLfloat *)c0);
    glUniform4fv(u_rc_c1, 8, (const GLfloat *)c1);
    glUniform2ui(u_rc_final, rc->final0, rc->final1);
    glUniform4fv(u_rc_fc0, 1, f0);
    glUniform4fv(u_rc_fc1, 1, f1);
    {
        /* FOG_COLOR is R in bits 0-7, the reverse of a D3DCOLOR. */
        float fogc[4];
        nv2a_rc_unpack(b->fog_color, fogc);
        { float r = fogc[2]; fogc[2] = fogc[0]; fogc[0] = r; }
        glUniform4fv(u_rc_fog, 1, fogc);
    }
    /* RECOMP_GL_NO_COMB: skip the register combiners and modulate stage 0 by
     * the vertex colour, which is the back end's original pixel stage. A
     * scene that paints black with the combiner on and lit without it has a
     * combiner problem; one that is black either way has a colour or texture
     * problem. */
    glUniform1i(u_rc_on, getenv("RECOMP_GL_NO_COMB") != NULL ? 0 : on);
    /* Stage 0's texture is also the one the fallback path samples. */
    glUniform1i(u_textured, b->textures[0] && s_stage_ok[0] ? 1 : 0);
    return on;

  off:
    /* No combiner (or one whose stage modes this shader does not evaluate):
     * the batch's pixel stage is the old modulate, which is what the CPU
     * rasteriser does for the same batch. The texture has to stay bound for
     * it -- leaving uTextured at apply_state's 0 drew every such batch in flat
     * vertex colour, and a stage-0 texture with it. */
    glUniform1i(u_rc_on, 0);
    glUniform1i(u_textured, b->textures[0] && s_stage_ok[0] ? 1 : 0);
    return 0;
}

/* Diagnostic overrides, all off unless the environment asks for them: each
 * one forces a raster state open so a scene that draws too little can be
 * bisected (is it the cull, the depth test, the alpha test?). They are how
 * the 2026-10-27 stage capture was narrowed down and are kept for the next
 * title whose world comes out half drawn. */
static int s_no_cull, s_no_depth, s_no_blend, s_no_alpha, s_allchan;
static int s_front_mirror;

static void state_env_init(void)
{
    static time_t last;
    time_t now = time(NULL);

    if (now == last)
        return;
    last = now;
    s_no_cull  = getenv("RECOMP_GL_NO_CULL")  != NULL;
    s_no_depth = getenv("RECOMP_GL_NO_DEPTH") != NULL;
    s_no_blend = getenv("RECOMP_GL_NO_BLEND") != NULL;
    s_no_alpha = getenv("RECOMP_GL_NO_ALPHA") != NULL;
    s_allchan  = getenv("RECOMP_GL_ALLCHAN")  != NULL;
    /* RECOMP_GL_FRONT_MIRROR: the opposite front-face convention. The title's
     * SET_FRONT_FACE is carried through unchanged because window-space
     * winding is a picture property: the vertex shader's y flip puts a vertex
     * on the same pixel the NV2A puts it on, so the name means the same thing
     * on both. This exists to prove that on a capture -- a title whose culled
     * geometry comes out inside-out under one convention and right under the
     * other says which. */
    s_front_mirror = getenv("RECOMP_GL_FRONT_MIRROR") != NULL;
    /* RECOMP_GL_FLAT: paint every surviving fragment one colour. Depth, cull
     * and alpha test still apply, so what is magenta is geometry that is
     * drawn and passes every gate -- the answer to "is the missing thing not
     * submitted, or not painted". */
    glUniform1i(u_flat, getenv("RECOMP_GL_FLAT") != NULL ? 1 : 0);
}

static void apply_state(const Nv2aRenderState *rs, const Nv2aSurface *s,
                        uint32_t frame)
{
    static const uint32_t none = 0;

    state_env_init();
    glUniform2f(u_surf, (float)s->width, (float)s->height);
    glUniform2f(u_origin, (float)s->x, (float)s->y);
    glUniform1f(u_incz, 1.0f / (float)z_divisor(rs));

    if (rs->blend_enable && !s_no_blend) {
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

    if (s_no_depth) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_ALWAYS);
        glDepthMask(GL_TRUE);
    } else {
        if (rs->depth_test_enable) glEnable(GL_DEPTH_TEST);
        else glDisable(GL_DEPTH_TEST);
        glDepthFunc(rs->depth_func ? rs->depth_func : 0x207);
        glDepthMask(rs->depth_write ? GL_TRUE : GL_FALSE);
    }

    glUniform1i(u_alphatest,
                (rs->alpha_test_enable && !s_no_alpha) ? 1 : 0);
    glUniform1i(u_alphafunc, (int)rs->alpha_func);
    glUniform1f(u_alpharef, (float)(rs->alpha_ref & 0xFF) / 255.0f);

    if (s_allchan) {
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    } else {
        uint32_t m = rs->color_mask;
        glColorMask((m >> 16) & 1, (m >> 8) & 1, m & 1, (m >> 24) & 1);
    }

    if (rs->cull_enable && !s_no_cull) {
        glEnable(GL_CULL_FACE);
        glCullFace(rs->cull_face ? rs->cull_face : 0x0405);
        /* SET_FRONT_FACE names the winding the title calls front, and the
         * rasteriser has to keep that set. It used to be written mirrored on
         * the theory that the vertex shader's y flip (which does invert the
         * winding) had to be paid back here; that is the wrong end of the
         * pipeline -- the flip makes window-space winding match the NV2A's
         * screen-space one, so the name carries over unchanged. Mirrored, the
         * stage drew the inside of every closed mesh: the bamboo trunks were
         * hollow black wedges, the player was his own back faces, and the
         * ground (a single up-facing sheet) was culled away completely. That
         * was exactly what the 2026-10-28 capture with RECOMP_GL_NO_CULL=1
         * proved: culling off restored the trunks and the ground. */
        glFrontFace(((rs->front_face == 0x0900) != s_front_mirror)
                    ? 0x0900 : 0x0901);
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
        /* Print on change: the depth clear's VALUE decides what every later
         * comparison can pass against, and "wrong units" lives right here. */
        {
            static int seen = -1;
            int key = (int)(z * 1e6) ^ (flags << 8) ^ (zf << 4)
                    ^ (rs->zeta_format << 1);
            if (seen < 0) {
                const char *e = getenv("RECOMP_GL_STATE");
                seen = e && *e ? 1 : 0;
            }
            if (seen && key != seen) {
                seen = key ? key : 1;
                fprintf(stderr, "[GLST] clear flags %X zstencil %08X -> z %f"
                        " (zf %u div %u) argb %08X zfmt %X\n",
                        flags, zstencil, z, zf, z_divisor(rs), argb,
                        rs->zeta_format);
            }
        }
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

/* RECOMP_GL_COMB=<n>: the pixel stage the title asked for, once per distinct
 * configuration, for the 2D batches (z = 0, rhw = 1) a back end gets wrong
 * most visibly -- a menu's text. "Diffuse times stage 0" is not the combiner:
 * where a title takes the colour from the alpha channel it paints opaque
 * black instead. What this prints is the register set the CPU rasteriser
 * evaluates (nv2a_combiner.h), which is the one to match. */
static void comb_report(const Nv2aBatch *b, const Nv2aVertex *v)
{
    static int left = -1, left3 = -1, min3 = -1;
    static uint32_t seen[256];
    static int nseen;
    const Nv2aCombiner *rc = b->combiner;
    uint32_t h, i;
    int s, dup = 0, two_d;
    float x0, y0, x1, y1;

    if (left < 0) {
        const char *e = getenv("RECOMP_GL_COMB");
        const char *e3 = getenv("RECOMP_GL_COMB3");
        const char *m3 = getenv("RECOMP_GL_COMB3_MIN");
        left = e ? atoi(e) : 0;
        left3 = e3 ? atoi(e3) : 0;
        /* The world's geometry is the big 3D batches; a dump that fills up on
         * sprites never reaches them. */
        min3 = m3 ? atoi(m3) : 300;
    }
    if ((!left && !left3) || !rc || !b->count)
        return;
    /* RECOMP_GL_COMB3 is the same record for 3D batches, where the question
     * is the fog: the first vertex's factor is what the fragment shader's
     * fog register holds, and a stage that is supposed to be textured but is
     * painted flat is either that factor or the final combiner's A input. */
    two_d = v->z == 0.0f && v->rhw == 1.0f;
    if (two_d ? left <= 0 : (left3 <= 0 || b->count < (uint32_t)min3))
        return;
    /* The batch's screen box, computed before the duplicate check so two
     * sprites that share a combiner but sit in different places are reported
     * apart: "the white rectangle at the lower right" has to be matchable,
     * and this is the only place the batch's vertices are in hand. */
    x0 = y0 = 1e30f;
    x1 = y1 = -1e30f;
    for (i = 0; i < b->count; i++) {
        const Nv2aVertex *p = &b->vertices[i];
        if (p->x != p->x || p->y != p->y)
            continue;                           /* NaN: no box from this one */
        if (p->x < x0) x0 = p->x;
        if (p->y < y0) y0 = p->y;
        if (p->x > x1) x1 = p->x;
        if (p->y > y1) y1 = p->y;
    }
    if (x0 > x1) { x0 = x1 = y0 = y1 = 0.0f; }
    h = 2166136261u;
    for (i = 0; i < 8; i++)
        h = (h ^ rc->color_icw[i]) * 16777619u;
    for (i = 0; i < 8; i++)
        h = (h ^ (rc->alpha_icw[i] * 3 + rc->color_ocw[i])) * 16777619u;
    h = (h ^ rc->stage_program) * 16777619u;
    h = (h ^ rc->control) * 16777619u;
    h = (h ^ rc->final0) * 16777619u;
    h = (h ^ rc->final1) * 16777619u;
    h = (h ^ rc->factor0[0]) * 16777619u;
    h = (h ^ rc->factor1[0]) * 16777619u;
    h = (h ^ (two_d ? 1u : 2u)) * 16777619u;   /* the two are reported apart */
    h = (h ^ (uint32_t)(x0 / 32.0f) * 73856093u) * 16777619u;
    h = (h ^ (uint32_t)(y0 / 32.0f) * 19349663u) * 16777619u;
    for (i = 0; i < (uint32_t)nseen; i++)
        if (seen[i] == h) dup = 1;
    if (dup)
        return;
    if (nseen < (int)(sizeof seen / sizeof seen[0]))
        seen[nseen++] = h;
    if (two_d)
        left--;
    else
        left3--;
    fprintf(stderr, "[GLCOMB] batch n %u col %08X spec %08X uv %g %g"
                    " fogf %g stpar %08X ctl %08X final %08X/%08X"
                    " f0 %08X f1 %08X fog %08X box %.0f..%.0f %.0f..%.0f\n",
            b->count, v->diffuse, v->specular, v->u, v->v, v->fogf,
            rc->stage_program, rc->control, rc->final0, rc->final1,
            rc->final_c0, rc->final_c1, b->fog_color, x0, x1, y0, y1);
    for (s = 0; s < 8; s++)
        fprintf(stderr, "[GLCOMB]   stage %d c %08X/%08X a %08X/%08X"
                        " fac %08X %08X\n", s, rc->color_icw[s],
                rc->color_ocw[s], rc->alpha_icw[s], rc->alpha_ocw[s],
                rc->factor0[s], rc->factor1[s]);
    for (i = 0; i < 4; i++) {
        const Nv2aTexture *tex = b->textures[i];
        fprintf(stderr, "[GLCOMB]   tex%u %s", i,
                tex ? "" : "(none)\n");
        if (tex)
            fprintf(stderr, " off %08X %ux%u fmt %02X pitch %u\n",
                    tex->offset, tex->width, tex->height, tex->color,
                    tex->pitch);
    }
}

static void gl_vert_fill(GlVert *o, const Nv2aVertex *v)
{
    int st, j;

    o->x = v->x; o->y = v->y; o->z = v->z; o->rhw = v->rhw;
    o->color = v->diffuse;
    o->specular = v->specular;
    o->fogf = v->fogf;
    /* Normalised coordinates, one vec4 per stage: s, t, q and the sampler's
     * spare. The combiners' stage fetch divides by q exactly as the CPU
     * rasteriser does (nv2a_pb_exec.c's rc_stage_fetch), which is why nothing
     * here scales by the texture size -- the sampler's own normalisation is
     * the same thing, and it keeps a passthrough stage's coordinates
     * comparable instead of multiplied by a size that only this stage's
     * texture has. */
    for (st = 0; st < 4; st++)
        for (j = 0; j < 4; j++)
            o->st[st][j] = v->st[st][j];
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
    comb_report(b, &b->vertices[0]);

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
        gl_vert_fill(&o[0], v0);
        gl_vert_fill(&o[1], v1);
        gl_vert_fill(&o[2], v2);
        o[0].pad[0] = o[1].pad[0] = o[2].pad[0] = 0.0f;
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

    /* Every stage the title programmed, then the pixel stage itself. A stage
     * with nothing to bind gets a white texel, which is what the CPU
     * rasteriser's sampler returns when it cannot read one. */
    for (i = 0; i < 4; i++) {
        s_stage_ok[i] = stage_bind(i, b->textures[i], s_frame);
        if (!s_stage_ok[i]) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, s_white);
        }
    }
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(u_tex, 0);
    glUniform1i(u_tex1, 1);
    glUniform1i(u_tex2, 2);
    glUniform1i(u_tex3, 3);
    rc_upload(b, &b->vertices[0]);

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
    if (s_frame == 300 || s_frame == 3000 || s_frame == 6000) {
        int k, any = 0;
        fprintf(stderr, "[PBGL] stage modes at frame %u (stage uses): ",
                (unsigned)s_frame);
        for (k = 0; k < 32; k++)
            if (s_mode_hist[k]) {
                fprintf(stderr, "%u x%u ", k, s_mode_hist[k]);
                any = 1;
            }
        if (!any)
            fprintf(stderr, "none");
        fprintf(stderr, "\n");
    }
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
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(GlVert),
                          (const void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(1, 2, GL_UNSIGNED_INT, sizeof(GlVert),
                           (const void *)16);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(GlVert),
                          (const void *)24);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(GlVert),
                          (const void *)40);
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(GlVert),
                          (const void *)56);
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(GlVert),
                          (const void *)72);
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, sizeof(GlVert),
                          (const void *)88);

    /* A texture unit nothing is bound to samples as black, and the CPU
     * rasteriser's failed sample is white (rc_texel); a stage the title
     * programmed but this back end has no texture for reads the same on both
     * sides only if the unit holds white. */
    {
        static const uint8_t white[4] = { 255, 255, 255, 255 };
        glGenTextures(1, &s_white);
        glBindTexture(GL_TEXTURE_2D, s_white);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

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

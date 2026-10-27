/**
 * nv2a_pb_gl.h -- an OpenGL render back end for the pushbuffer executor.
 *
 * The executor (src/kernel/nv2a_pb_exec.c) decodes a title's NV2A methods and,
 * by default, rasterises them on the CPU into guest memory. With a back end
 * registered it hands the work over already transformed: surface state and
 * clears, triangle lists in surface pixels with per-vertex colour and
 * texel-space UVs, the texture the batch samples, and blend/depth/stencil/cull
 * state in the NV2A's own enum values (see nv2a_backend.h).
 *
 * This back end draws that on the GPU. It renders into an off-screen OpenGL
 * 3.3 target and writes the finished frame back into the title's colour
 * surface at flip, so everything downstream of the rasteriser -- the window
 * present, RECOMP_FB_DUMP, a title that reads its framebuffer back -- keeps
 * working exactly as it did with the CPU rasteriser, and RECOMP_FB_WINDOW is
 * still the one visible window.
 *
 * It is deliberately not a full NV2A: it sees stage 0 and the diffuse colour
 * only, because that is all the back-end interface passes on (no register
 * combiners, no texture stages 1-3, no fog term). Titles whose look depends
 * on those need the CPU rasteriser, which is what RECOMP_PB_GL=0 selects.
 *
 * Switches (read once, at nv2a_pb_gl_init):
 *   RECOMP_PB_GL=0              keep the CPU rasteriser; without it (or with
 *                               it unset) this back end draws
 *   RECOMP_PB_GL_ZSCALE=<n>     depth divisor (default 2^24 for Z24S8, 2^16
 *                               for Z16): the title's z is compared in its own
 *                               units, so one fixed scale keeps every draw's
 *                               depth in the same units, which is what the
 *                               back-end contract asks for
 *   RECOMP_PB_GL_TEXREFRESH=<n> re-upload a texture every n frames (0: once,
 *                               on first use -- the default, and right for
 *                               every static texture)
 *   RECOMP_PB_GL_VERBOSE=1      log the device, targets and texture traffic
 *
 * Call nv2a_pb_gl_init() from the host's main thread before the guest starts:
 * SDL video and the GL context are created there, and the executor's poll
 * thread merely makes the context current when it first draws.
 */
#ifndef XBOXRECOMP_NV2A_PB_GL_H
#define XBOXRECOMP_NV2A_PB_GL_H

/* Create the device and register the back end. Returns 1 if the back end is
 * now drawing, 0 if it is disabled or unavailable (the CPU rasteriser then
 * carries on as before). Safe to call on a host without a display. */
int nv2a_pb_gl_init(void);

#endif /* XBOXRECOMP_NV2A_PB_GL_H */

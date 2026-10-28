# xboxrecomp submodule

This directory is the MIT-licensed
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) runtime toolkit, pulled
in as a git submodule from `sigods/xboxrecomp` on the `otogi1-linux-v0.1`
branch. Clone with `--recursive`, or run `git submodule update --init` after
an ordinary clone.

| | |
|---|---|
| Upstream base | `v0.16.0` (`f0f8a46`) |
| Branch head | `aa486c8` (`otogi1-linux-v0.1`, on the fork) |
| License | MIT — see `LICENSE` in this directory |

## Local changes against `v0.16.0`

24 files, 3,154 insertions, 193 deletions. The ones that matter:

- `src/nv2a/nv2a_pb_gl.c`, `nv2a_pb_gl.h` — new. An OpenGL 3.3 back end for the
  push-buffer executor (`src/kernel/nv2a_backend.h`): it keeps one off-screen
  colour+depth target per surface, applies the NV2A render state, draws the
  batches the executor hands over, and reads the finished frame back into the
  title's colour surface at flip. Offered upstream as the natural next step for
  every title that currently runs on the CPU rasteriser.
- `src/kernel/nv2a_vsh_interp.c` / `.h` — the vertex program is decoded once
  per program (a generation counter bumps on every write) into a `VshIns`
  table instead of four bitfield extractions per source per vertex, and
  `nv2a_vsh_input_mask()` tells the executor which attribute streams the
  program actually reads. Together with the vertex-transform pool in
  `nv2a_pb_exec.c` (`RECOMP_XF_THREADS`) this took the start-game stage from
  ~12 fps to ~40 fps, and a perf profile put the interpreter at 37% of every
  host cycle before it.
- `src/kernel/nv2a_pb_exec.c` — status reporting uses wall time
  (`xbox_HostNowNs`) instead of `clock()`, which is summed over every thread;
  a rasteriser that got faster by using more cores read as slower. Both pools
  now exist on POSIX as well as Windows. `backend_flush()` flushes before the
  present reads the surface, and the surface a back end sees now carries its
  clip rectangle (`x`, `y`) and the zeta format.
- `src/input/xinput_device.c` — `RECOMP_AUTOPRESS` on POSIX, with a START-then-A
  schedule, so a headless run can reach a stage through the emulated USB pad.

- `src/platform/mmio_decode_x86_64.h` — new. Decodes trapped x86-64 MMIO
  instructions on Linux (ucontext) and Windows (VEH `CONTEXT`). Without it the
  APU/AC'97 and OHCI apertures fault on x86-64. The Linux-only half is offered
  upstream as sp00nznet/xboxrecomp#252.
- `src/apu/apu_mmio_hook.c`, `src/usb/ohci.c` — call the decoder on both hosts;
  the OHCI entry point previously referenced a function that did not exist.
- `src/kernel/kernel_bridge.c` — `bridge_async_io_enabled()` defaults to 1, so
  `NtReadFile` returns `STATUS_PENDING` and completes on the work queue; the
  timer-driven path is unchanged. Also calls `xbox_OhciInit()` once guest
  memory is mapped, so the USB aperture is trapped.
- `src/kernel/nv2a_pb_exec.c` — `fetch_attr()` resolves vertex attributes
  through `dma_resolve()`; position attribute `w == 0` is read as 1.0 for
  screen-space quads.
- `src/input/*`, `src/usb/usb_gamepad.c` — keyboard-to-pad bridge and synthetic
  pad for headless testing.

- `src/nv2a/nv2a_pb_gl.c` — the fragment shader's `rc_stage()` now implements
  the whole `SET_SHADER_STAGE_PROGRAM` table the CPU rasteriser's
  `rc_stage_fetch()` works from (none, projected 2D, cubemap, passthrough,
  clip plane, zero for the bump-env and dot-product modes), and discards for
  the clip-plane stage. `rc_upload()` no longer refuses a batch over a mode
  it cannot evaluate: refusing disabled the combiners for that batch and
  painted it `v0` times a texel, which is black for a lit model. This is what
  Otogi's player model and bamboo needed.
- `src/kernel/nv2a_backend.h`, `nv2a_pb_exec.c` — `Nv2aBatch` carries
  `clip_plane_mode`, filled from the executor's state, so a back end can
  apply the clip-plane stage's per-channel compare.
- `src/kernel/nv2a_pb_exec.c` — `sample_tex()` gained `D3DFMT_G8B8` (0x28,
  two 8-bit channels and no alpha) and `linear_twin()` maps it to
  `D3DFMT_LIN_G8B8`; without both, a swizzled G8B8 texture fell through to
  the unsupported-format path and a back end bound white for it.
- `src/nv2a/nv2a_core.c` — `pvideo_write()` reports the first few
  `NV_PVIDEO_BUFFER` writes that arm a bank. PVIDEO is only composited where
  a presenter runs, so a headless capture of a title's movie is black either
  because the overlay is doing its job off-screen or because the title never
  programmed it, and nothing else tells those apart.
- New diagnostics in `nv2a_pb_gl.c`: `RECOMP_GL_NO_COMB` (skip the combiners,
  modulate stage 0 by the vertex colour) to bisect a scene that paints wrong,
  and the `[PBGL] stage modes` census, which prints each
  `SHADER_STAGE_PROGRAM` slot count once per run at frames 300, 3000 and
  6000.
- `src/nv2a/nv2a_pb_gl.c`, `apply_state()` — `glFrontFace()` now uses the
  title's `SET_FRONT_FACE` value unchanged. It was mirrored on the theory
  that the vertex shader's y flip had to be paid back, but the flip is what
  makes window-space winding match the NV2A's screen-space one, so mirroring
  kept the back faces of every closed mesh: Otogi's bamboo drew as hollow
  black wedges and the player as an inside-out silhouette. Found by
  `RECOMP_GL_NO_CULL=1` restoring both on the same capture
  (`docs/BOOT_LOG.md`, 2026-10-28).

- `src/nv2a/nv2a_pb_gl.c`, `comb_report()` — the `[GLCOMB]` record now ends
  with the batch's screen box (`box x0..x1 y0..y1`) and the duplicate check
  hashes that box, quantised to 32 pixels, alongside the combiner state.
  Without it a frame full of six-vertex sprites reports one record per
  *combiner*, so a suspect quad could not be matched to its position: Otogi's
  transition frame has a pure-white rectangle at +499+309 that no record
  covered. The box is only computed when the record is about to be printed or
  hashed, so nothing changes unless `RECOMP_GL_COMB`/`_COMB3` is set.

## Updating

The submodule tracks our fork, so a rebase is a branch operation in
`sigods/xboxrecomp`:

1. Fetch the new upstream tag into the fork (add upstream as a remote once).
2. `git rebase <tag>` on `otogi1-linux-v0.1`; the local changes above are plain
   commits, so conflicts surface per hunk.
3. Force-push the branch, then bump the gitlink in the parent repository
   (`git add xboxrecomp`) and keep this file's table current.

To hand a change back upstream, branch from the upstream tag rather than from
this branch, so the diff carries only the topic change.

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

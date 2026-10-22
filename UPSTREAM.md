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

18 files, 1,648 insertions, 118 deletions. The ones that matter:

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

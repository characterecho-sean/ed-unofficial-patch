# Frame generation Phase 2.2: the slot's pose and S's own transfer, nothing shown

Branch `framegen-phase1`. Design: `docs/design-frame-generation-2026-10-26.md`
("### Where it runs", "### Pacing: Lock and Fill", ## Phases item 3 "Slices",
the journal through "Phase 2.1, time and VRAM"). The runtime map from the
2.2 exploration is summarised below; verify every reference before using it.

## Goal

1. **S's head pose comes from the runtime**, located for S's display time at
   the latest point before the synth runs, instead of the synth's own
   extrapolation over one Elite step.
2. **S crosses to the runtime** through its own transfer set, written by the
   synth's resample pass straight into the producer texture (no separate S
   texture on the game device), and the runtime consumes it and logs that it
   arrived and when.
3. **Nothing shown changes.** The runtime does not compose S; the frame loop's
   order and timing for real frames is unchanged. Key: still
   `fix.frame_generation_shadow = on`; with it off, byte-for-byte as before
   (no locate, no transfer, no textures).

## The slot's time and pose

- T = the pair's predicted display time (`boundary.frame()`), T_A = the
  previous real pair's. S is half an Elite step after B: **T_S = T + (T - T_A)
  / 2** (the synth's f = 0.5; at Lock 2:1 this is T + P). Clamp to
  [T + 1 ms, T + 50 ms]; outside, or with no previous pair, no pose (the synth
  falls back to its extrapolation and counts it).
- Locate the view (head) at T_S in the seated space at the freshest point the
  runtime has before the game device runs the synth: the 2.2 map says that is
  inside `capture()` before `graphicsCalls.invoke(treat)` (native_runtime_host.h
  ~2333). One locate per pair (not per eye), only when the key is on.
- Hand it to the d3d11 side for this treat: the temporal ABI
  (`native_temporal.h`, `edvrTemporalAaNoteHead`-style side channel or a new
  entry at the END of the table, versioned so an old d3d11.dll with a new
  runtime and a new d3d11.dll with an old runtime both work and say so). The
  value: S's head pose in the same space and convention as the head poses the
  temporal pass already gets (headNow), plus its XrTime and a valid flag.
- The synth builds head B -> S from headNow (B) and the handed pose with the
  same eye-to-head the pass uses (`eyeDelta` in native_temporal.cpp is the
  pattern). Rotation and translation both.
- The f = 1 measurement variant keeps its own extrapolation (no pose exists
  for C at B's time); its turned score stays as the pipeline's check.

## S's transfer

- A third set of per-eye `SharedTextureTransfer`s (or a third EyeCapture),
  created lazily when the key is on, separate from `captured` and
  `previousPair` (which own the real-frame replay; never touch their
  exchange). Format and size = dlOut's.
- The producer texture needs UAV binding so the resample pass writes S into it
  directly; if the shared texture cannot carry UAV (format or flags refused),
  fall back to the current private S texture plus a CopyResource, and log
  which path is live.
- Producer: in the game's immediate context, AcquireSync(0) before the
  resample dispatch, ReleaseSync(1) after (the transfer's own discipline; keep
  the one-handoff-in-flight rule: if the previous S was not consumed, skip
  this S and count `s_busy`).
- Consumer (runtime owner thread): consume S without delaying the real pair's
  compose: after the pair's finish, or on the owner's idle tick, AcquireSync(1)
  with a 0 or short timeout, copy to the private texture, ReleaseSync(0).
  Record when S was ready against T_S (the runtime's own clock) and how long
  the consume took.
- Per second, one runtime line `frame generation (synth) transfer:` with: S
  produced, consumed, `s_busy`, `s_late` (ready after T_S), ready-vs-T_S
  median/max ms, consume ms median/max, pose source counts (runtime /
  fallback), the transfer path (uav / copy), and the VRAM the set takes on
  each device. The d3d11 synth line adds the pose source counts too.

## VRAM

The game-device S texture goes away when the UAV path is live (about 47 MiB
per eye at 3756x3264 R11G11B10); the transfer adds its producer texture (the
same size, replacing S) and the runtime's private copy on the XR device. Log
both sides' bytes; the expected net against 2.1 is the private copy only.

## Tests

- ABI both directions: old d3d11 + new runtime, new d3d11 + old runtime
  (the `native_frame_test` / `openxr_native_test` patterns).
- The transfer: produce/consume, busy skip, timeout, release on key off
  (`openxr_shared_texture_test` pattern, WARP).
- The synth with a handed pose: the rig's head cases driven by a pose instead
  of the extrapolation, landing ≤ 0.1 px; a mutation (pose ignored, or the
  pose's direction inverted) fails.
- The runtime's T_S computation as a pure function (clamp, no previous pair).
- Report rigs for the new line fields; `edvr_log.py --health` rule for the
  transfer line (NEVER APPLIED when the synth is armed and the runtime reports
  no transfer line, or produced > 0 and consumed = 0).

## Build policy (the user's)

No full build: compile and run the touched rigs (openxr tests included),
`edvr_log.py --self-test`, the bench's self-test and `--frame`/`--replay` if
the synth's shaders change, commit, then `python tools\compile_only.py
--build-native --jobs 4` exits 0 with a receipt (it builds both DLLs).

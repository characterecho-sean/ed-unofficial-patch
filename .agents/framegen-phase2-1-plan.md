# Frame generation Phase 2.1: the synthesizer, nothing shown

Branch `framegen-phase1` (stays the working branch). Design:
`docs/design-frame-generation-2026-10-26.md` (## Design "The synthesis pass",
## Phases item 3 "Slices", journal flights 1-6). Phase 1's scorer code is the
pattern to follow: `src/d3d11/framegen_shadow.{h,cpp}`,
`src/d3d11/framegen_shadow_shader.h`, `src/common/framegen_shadow_math.h`,
`src/common/framegen_shadow_report.h`.

## Goal

At each real frame B, per eye, on the game device right after B's output is
published (beside the Phase 1 scorer call in `temporal_pass.cpp`), build the
synthetic frame S for one display period later, at output size, before the UI.
Nothing is shown and the frame loop does not change. Runs under the same key,
`fix.frame_generation_shadow = on` (no new key).

## Motion of a B pixel to S (render px; f = fraction of B's frame step S is ahead)

- f: S is one display period after B; Elite's step A -> B is two periods in
  Lock, so **f = 0.5** for the shown S. For scoring, a second variant with
  **f = 1** lands on C's time (see Scoring).
- Head B -> S: the head's own turn and translation carried at constant velocity
  from A -> B: rotation = slerp(I, R_AB_head, f) (axis-angle scaled by f),
  translation = f * t_AB_head, where (R,t)_AB_head is the head-only forward
  delta A -> B (the inverse of the backward `Hd_B | th_B` the scorer already
  gets as `deltas.Hd`, `deltas.th`). Document that this is the fallback until
  2.2 brings the runtime's slot pose.
- **cockpit** (A's split rule on B's depth, `temporalDepthMetres` +
  `temporalWorldPath` with dk and p.split[0], nearer than the split):
  rigid head transform B -> S with B's depth (carry + project with tanB both
  sides; S's tangents = tanB).
- **far and world** (ex2dh, the flight 5 winner): with B's DLSS vector mvB
  (B -> A, render px, jitter already cancelled) at the pixel:
  pHeadPrev = project(Hd_B_rot * dir(pB, tanB), tanB) (A's head turn removed;
  tanB for A's tangents as Phase 1 did), m = (pB + mvB) - pHeadPrev (the
  ship's screen motion over one Elite step, B -> A direction),
  pShip = pB - f * m, pS = project(R_BS_head * dir(pShip, tanB), tanB).
  Far uses the same rule (sky rides the vectors' rotation).

## The pass (render size unless stated; new HLSL entries in a new
`src/d3d11/framegen_synth_shader.h`, carry text cut from the
`EDVR_TEMPORAL_CARRY` markers as the scorer does)

1. **Dilate:** B's depth, nearest (max reversed-Z) over 3x3 -> class and depth
   per pixel use the dilated value (so edges move with the near object).
2. **Splat:** each B pixel -> pS; key = (asuint(1/z) & ~3) | class
   (far = 4, as the scorer), InterlockedMax into the 4 pixels of pS's
   bilinear footprint. Second pass: where Key == myKey, write the source
   pixel index and the sub-pixel landing offset (pS - target pixel centre)
   into a Src buffer (R32G32_UINT or two textures).
3. **Guard:** for each S pixel with a winner, if the winner's source pixel in
   B has a 3x3 neighbour nearer than itself by more than a small margin AND
   the read position falls on that nearer side (the edge case of the design:
   "sky took the colour of the cockpit wall"), read the source pixel itself
   (no sub-pixel offset toward the edge). Count guard hits.
4. **Holes (background pull):** S pixels with no winner: search a ring of
   radius up to 8 px for the neighbour with the FARTHEST depth key, and read
   that neighbour's source shifted 2 px further into its own surface (away
   from the hole) — never the nearer neighbour. Count holes filled / left.
   Other-eye fill is NOT in 2.1.
5. **Resample (output size):** each output pixel -> S render coordinate ->
   its S pixel's source B render coordinate (+ landing offset) -> B output
   coordinate -> bilinear read of B's upscaled output (`dlOut`, pre-UI).
   Write S to a per-eye output-size texture of dlOut's format.

## Scoring, timing, dump

- **Score (f = 1 variant, every other real frame per eye; the other frames
  build the f = 0.5 S):** reduce the f = 1 S to quarter-grid luma the same
  way the scorer's capture does, keep it, and at the next real frame C score
  it against C's captured luma: per class (B's class) 48-bin histogram,
  p50/p95/mean/share over 0.02 and 0.05, plus the no-warp (B vs C) baseline,
  the same per-class format as the scorer. This is a two-period test of the
  whole pipeline (dilate, splat, guard, fill, resample) against a real frame.
- **GPU time:** one GpuTimer per eye around the whole synth (both variants
  counted apart), median/max per second, census section `DoorFrameGenSynth`
  next to `DoorFrameGenShadow` (update `gpu_census_test`).
- **Counters per second:** holes filled / left (% of render pixels), guard
  hits (%).
- **Line:** `frame generation (synth): frames=<n f=0.5>/<n f=1> gpu_ms=<f=0.5
  med/max>(n=)/<f=1 med/max>(n=) holes=<filled%>/<left%> guard=<%> far=...
  cockpit=... world=...` (per-class fields = the f = 1 score vs C), once a
  second, after the scorer's lines; format in a pure helper under
  `src/common/` (e.g. `framegen_synth_report.h`) with its own report rig;
  `edvr_log.py --health` rule "frame generation synth" (label-only, quiet
  when absent, NEVER APPLIED when the shadow is armed and no synth line).
- **Dump:** find the existing eye-dump mechanism (the one
  `tools/character_motion_check.py` reads; grep `eye dump`, `dump` in
  `src/d3d11`), and when it fires also write, per eye, B's output, S (f =
  0.5) and, the next frame, C's output, in the same format and folder, named
  so the three line up. If no reusable trigger exists, report that and skip
  the dump rather than inventing a hotkey.

## Tests (before flying)

- **Synthesis-math rig on WARP** (`tools/framegen_synth_test`, registered in
  build.bat like `framegen_shadow_test`): CPU reference of the per-pixel
  motion against the shader to under 0.1 px for: cockpit under head
  translation+rotation, world plane with constant ship screen velocity and a
  changing head turn (ex2dh), the far plane, the 1 mm on-foot split; the guard
  (a near object edge over the sky: no sky pixel takes the near colour); the
  fill (a hole behind a moving near object is filled from the far side, never
  the near). Mutations: f ignored, head turn not removed, guard off, fill
  from the nearer side -> each fails a check.
- Report rig for the synth line; `edvr_log.py --self-test`.

## Build policy (the user's)

No full build for test iterations: compile and run the touched rigs,
`edvr_log.py --self-test`, commit, then `python tools\compile_only.py
--build-native --jobs 4` must exit 0 with a receipt.

# Frame generation Phase 1 (shadow scorer): working plan

Branch `framegen-phase1`. Design: `docs/design-frame-generation-2026-10-26.md`
(## Design, ## Phases item 2). This file hands the spec between sessions and
subagents; the evidence goes in the design doc's journal.

## Scope of this slice

- Classes scored: far, cockpit, world. **Cut** (next iteration): mover (no
  per-pixel signal; engine-record motion would be the source) and reactive
  (`dlMask` is written only with uiTrack||engineBound and modern DLSS presets
  ignore it). Also deferred from the design's Phase 1: interpolation (H4),
  stereo score (H8), surface classifier, per-eye GPU share (H9).
- Runs only on the trained-engine path (`usedDlaa`: dlss/dlaa/fsr/xess), VR
  only. Key `fix.frame_generation_shadow` off|on, hidden, default off. Off:
  no resources, no dispatches, no log lines.

## Steps and commits

1. Done: rig fix (126f7f5c).
2. Shared HLSL: `temporalPixelDir`, `temporalDepthMetres`, `temporalWorldPath`,
   `temporalCarry`, `temporalProjectPixel`, called by `fetchHistoryT` and
   `mv()`; bytecode compared against the pre-refactor build.
3. Key + armed line + `--health` rule (label-only, NEVER APPLIED when the armed
   line is present and no score line is).
4. Scorer pass + 1 s line + GPU timer + census section `DoorFrameGenShadow`.
5. WARP rig `tools/framegen_shadow_test` (shader vs CPU reference).
6. H6 in the OpenXR module (own 1 s line in `edvr_openxr_*.log`).
7. Docs: Status block + journal entry.

## The math (C++, per eye, at real frame B whose previous real frame is A)

Backward deltas (a point in B's eye space -> A's), as the motion pass uses:
- world W_B, tvCam_B: `temporalWorldFromRows(g_prevRows, g_curRows, ...)`.
- head Hd_B = `temporalHeadDelta(headPrev, headNow)`, th_B =
  `temporalHeadTranslation(headPrev, headNow, eyeOff)` (p.dR*, p.tvUsed).
Stored from A's frame: W_A, tvCam_A, Hd_A, th_A, valid flag.

Prediction (constant ship velocity, true head at B, so the head-pose miss is
H6's alone):
- ship part of A: S_A = W_A * Hd_A^T, s_A = tvCam_A - th_A (first order).
- predicted world: Wp_B = S_A * Hd_B, tp_B = s_A + th_B.
- rig miss (C++ only, logged): angle(Wp_B^T W_B) deg, |tp_B - tvCam_B| mm.

Backward transform per class (B point -> A): far [Wp_B | 0], world
[Wp_B | tp_B], cockpit [Hd_B | th_B]. Forward (A -> B) is the inverse:
[R^T | -R^T t]. Class split from A's depth: `temporalDepthMetres` with the
pass's knobs (x = A, z = B of depth = A + B/z), `temporalWorldPath` with
p.split[0] (10 m aboard, SRV 3.5 m, on foot 1 mm). Scanner UI: ignored.
Skip the frame (count by reason) when: !zPrevValid, no stored A deltas,
world rows invalid this frame or last, camera gate jump/rebase.

## GPU passes (game device, after `applyUiResolve`/copy, beside the sky ring)

Grid: quarter of render size, qw = ceil(w/4), qh = ceil(h/4); a cell's
render-pixel centre is cell*4 + 1.5.
0. Capture (every frame, at the end, into ping-pong A/B per eye):
   lumaQ = Rec.709 luma of `dlOut` (pre-UI, output size) averaged over the
   cell's output area (bilinear taps), clamped 0..1; depthQ = max reversed-Z of
   the cell's 4x4 render pixels of `dlDepth` (nearest wins = dilation).
1. Clear dest key buffer (R32_UINT, qw x qh) to 0.
2. Splat (per A cell): class from depthQ_A; dir with tanA (= this frame's
   tanPrev); `temporalCarry` with the forward transform; project with tanB (=
   tanNow, jitter excluded) at render size; scatter to the 4 cells of the
   bilinear footprint of pp/4. Key: 0 = hole; far = 4u | 0; else
   (max(asuint(1/zB), 8u) & ~3u) | class (1 cockpit, 2 world). InterlockedMax.
3. Score (per B cell): key 0 -> hole. Else class = key & 3, zB from the key
   (far: none), dir with tanB, `temporalCarry` with the class's BACKWARD
   transform, project with tanA, bilinear-sample lumaQ_A at that point.
   e = |lumaQ_B - pred|; also eId = |lumaQ_B - lumaQ_A same cell| (no warp).
   Groupshared 48-bin log histogram per class for e and eId: bin 0 for
   e < 1/1024, else clamp(1 + floor(4 * log2(e * 1024)), 1, 47); plus pixel
   count and fixed-point sum (e * 1024) per class; InterlockedAdd to a small
   structured buffer per eye. Hole count separately.
   (Phase 1 omits the splat-winner edge guard and the hole fill: holes are
   counted, not filled.)
4. CopyResource to a 4-deep staging ring; Map with DO_NOT_WAIT on later
   frames, never Flush.
GPU time: one `GpuTimer` per eye around 1-3 plus capture; median and max per
second.

## Log

- Once when armed (configure, key on, VR): `frame generation (shadow): armed,
  quarter <qw>x<qh> per eye, classes far cockpit world (mover, reactive not
  scored)`.
- Every 1 s (QPC window in `temporalPassFrameBoundary`, independent of the
  sky diag gate): frames scored, skipped by reason, per class share %, mean,
  p50, p95, % over 0.02 and over 0.05, the same for no-warp, hole %, gpu ms
  median/max, rig miss deg and mm median/max. Format in a pure helper under
  `src/common/` so the rig and `edvr_log.py`'s self-test read one string.

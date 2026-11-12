# Frame generation Phase 2.3b: S finished like B (UI, sharpening, format)

Branch `framegen-phase1`. Follows 2.3 (`.agents/framegen-phase2-3-plan.md`,
commits 6cdf599d..018b3ef7). Design: "### The synthesis pass" step 4 (UI with
the head-only transform) and "### Space without artifacts" (the HUD rides the
head path).

## Problem

S is the synth's resample of `dlOut` (pre-UI, pre-sharpen, R11G11B10). B, as
shown, is `dlOut` + the UI resolve (`applyUiResolve` into `e.dlSubmit`,
temporal_pass.cpp ~5148) + whatever the runtime then does to a captured eye
(`treatEyeTemporal` / `finishCapturedEye`: EDVR's menu, sharpening) in an
8-bit RGBA/BGRA format. The runtime refuses to show S in R11G11B10 (every S
slot replays B), and an S without the UI would make the HUD blink at 45 Hz.
The pilot will not fly Lock until S looks like B.

## Goal

Every S that reaches the runtime is finished exactly as B is, in B's shown
format, so an S slot and a B slot differ only by the synthesized motion:

1. **The UI on S.** Composite the same UI B got (the UI layer of B's frame,
   the `ui_quality` layer, holo panels, HUD) onto S, moved by the HEAD ONLY
   (B's eye to S's eye: the cockpit-class transform the synth already has).
   Find how the UI resolve composes (inputs, masks, depth if any) and either
   (a) run the same resolve with S as the scene input and the UI sampled
   through the head-only warp, or (b) the smallest equivalent. A rotation-only
   warp of the UI at its own depth (panel distance) is acceptable for v1 if
   the UI has no per-pixel depth; say which and why. World-anchored brackets
   lag one period (accepted by the design).
2. **The same post-processing as B,** in the same order: if sharpening (and
   anything else that changes pixels: tone, dither, gamma, the EDVR menu
   overlay) runs on the game device, run it on S too; if it runs in the
   runtime on a captured eye, run it on S's consumed copy in the runtime the
   same way. While EDVR's own menu is open, Lock may disengage instead
   (reason `menu`) if putting the menu on S is not trivial: say which.
3. **B's shown format:** S's producer texture (2.2's shared UAV texture) is
   created in the format the runtime shows B in, so the S slot shows S.
   If the UI resolve writes `dlSubmit` in that format, the cheapest path is:
   synth resample -> a scratch in dlOut's format (or straight into a
   UI-resolve input), UI resolve into the 8-bit producer texture. Keep the
   one-handoff rule and the busy/skip logic from 2.2.

## Verification before any Lock flight

- The eye dump (the settings menu's "Dump both eyes as seen", already wired
  to write `eye_<stamp>_S<k>.bmp` next to `P<k>`): write the FINISHED S
  (after UI and post-processing, the exact texture the runtime would show),
  so the pilot can compare B and S by eye with Lock OFF.
- A rig check: the finished S of a static scene (no head motion) equals B's
  finished frame within a small tolerance (the UI and sharpening applied to
  the same pixels); with a head turn the UI moves by the head-only warp
  (landing ≤ 0.5 px against a CPU reference).
- The runtime's "cannot be shown" line must not appear; the lock line's
  `s_from` must be able to count S (a rig: a delivered S in B's format is
  shown, not replayed).
- Cost: GPU time of the finishing per eye, logged on the synth line (and the
  bench if it can run it; if the UI path needs game state the bench lacks,
  say so).

## Build policy (the user's)

No full build: compile and run the touched rigs, `edvr_log.py --self-test`,
commit, then `python tools\compile_only.py --build-native --jobs 4` exits 0
with a receipt.

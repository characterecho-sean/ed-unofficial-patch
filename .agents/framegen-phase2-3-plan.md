# Frame generation Phase 2.3: Lock in the runtime, S shown

Branch `framegen-phase1`. Design: `docs/design-frame-generation-2026-10-26.md`
("### Pacing: Lock and Fill", "### Where it runs", ## Phases item 3, journal
through "Phase 2.2, flight 9"). Specs of 2.1 and 2.2 in `.agents/`. This is the
first slice that changes what the headset shows, and only with the new key on.

## Key

`fix.frame_generation = off | on` in `edvr.ini` (hidden, temporary; `on` is
forced Lock for A/B flights; `auto` comes in 2.4). Read on the d3d11 side
(`Config`), carried to the runtime as an ABI output word (free words 5-17 of
`reservedTerrainGuard`; version it like word 18, ladder-safe both ways).
**Off: byte-for-byte as today**, the frame loop untouched.

`on` implies the synth runs (as with the shadow key) but builds S at f = 0.5
for EVERY real frame (no f = 1 alternation); the f = 1 score runs only when
`fix.frame_generation_shadow = on` AND Lock is not engaged.

## Lock (2:1), the schedule

Elite's frame B is shown at slot T; S_B at T + P; Elite's next frame C at
T + 2P. OpenXR's `xrWaitFrame` hands out slots in order, so the synthetic slot
must be waited, begun and ended before Elite's next wait.

- In `waitPoses` for C (owner thread), when Lock is engaged and the previous
  pair B was composed: run the **S slot** first: `xrWaitFrame` (slot T + P),
  `xrBeginFrame`, compose one projection layer from S's consumed textures with
  **S's own views** (below), `xrEndFrame`. Then the normal wait/begin for C,
  whose predicted time is T + 2P: Elite is told it (no synthesized time).
- **S's views:** per eye, the pose S was rendered for: the head located at T_S
  in `capture()` (2.2) times the same eye-to-head the synth used, and B's fov
  (S is rendered with B's tangents). Store them with the pair. The compositor
  turns S by what remains (its own reprojection).
- **S missing** (not consumed, `s_busy`, no pose, transfer failed): the S slot
  still runs, showing B's own textures with B's views (the compositor turns
  it); count `late_s`. Never skip the slot while engaged: skipping hands C a
  one-period time.
- **B late:** Elite's next `waitPoses` arrives after slot T + 2P was due;
  count `late_real` from the boundary's own late counting; do not try to
  catch up.
- **Engage / disengage** only between pairs (in `waitPoses`, before any wait).
  Engaged while ALL hold: key on; a projection layer is being shown (not a
  menu, load, flat clear or withheld frame); the synth is delivering S (the
  2.2 transfer live, separate devices); deferred pacing (turbo /
  `fix.weapon_stability`'s on-foot deferral) is NOT active this frame (Lock
  stands aside for it in 2.3; say so in the log); the runtime's predicted
  period is the display's (not already halved). Any failure disengages at
  once; re-engage after the conditions hold for 1 s. Edge lines on every
  engage/disengage with the reason.
- **Fail-open:** disengaged = today's path exactly; predicted times stay
  monotonic through `FrameBoundary`'s floor; a disengage never hands Elite a
  doubled period.
- **frame_end_overlap:** B's queued finish must complete before the S slot's
  wait; respect the owner FIFO.
- `pumpEvents`' idle tick, `drain`, `clear`, loading and flat frames: must
  not interleave with an engaged S slot (disengage first).

## Logs

- Runtime, once a second: `frame generation lock: engaged=<s of the window>
  slots=<real>/<s> late_real=<n> late_s=<n> s_from=<S>/<B replay>
  wait_ms=<S slot wait med/max> period_ms=<med> disengages=<n>
  reason=<last>`; edge lines `frame generation lock: engage|disengage
  reason=...`.
- **H6 at the slot:** the H6 pose line gains the slot pose's own miss: the
  head located at T_S in `capture()` against the truth located once the clock
  passes T_S: `slot_mm=med/p95/max slot_deg=... slot_lead_ms=...`.
- `edvr_log.py --health` rule "frame generation lock" (NEVER APPLIED when the
  key is on and no lock line; flag late_s or late_real over 5% of slots).

## Tests (before flying)

- **The slot schedule as a pure state machine** (in the style of
  `openxr_frame_test`'s fake runtime): steady 2:1; late real; late S (B
  replayed); engage and disengage at pair boundaries; a cut (menu/load);
  deferred pacing arriving; period halved by the runtime; monotonic predicted
  times; fail-open never doubles a period; frame_end_overlap ordering.
- The ABI word both directions (`native_frame_test`).
- S's views: the layer's eye poses equal the poses the synth rendered for
  (a rig check from the same math).
- Report rig + `edvr_log.py --self-test` for the new lines.

## Build policy (the user's)

No full build: compile and run the touched rigs (openxr tests included),
`edvr_log.py --self-test`, commit, then `python tools\compile_only.py
--build-native --jobs 4` exits 0 with a receipt.

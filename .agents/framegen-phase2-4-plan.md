# Frame generation Phase 2.4: the gate and `auto`

Branch `framegen-phase1`. Design: `docs/design-frame-generation-2026-10-26.md`
"### The gate", "### Pacing: Lock and Fill", "### Vendor smoothing", ## Phases
item 3 (2.4), and the journal through 2026-11-08. Lock (2.3) and its own
engage conditions live in `src/openxr/framegen_lock.h` and
`src/common/framegen_lock_report.h` (`FrameGenLockGate`).

## Key

`fix.frame_generation = off | auto | on` (edvr.ini, live; the menu row on the
Performance page and `hotkey.frame_generation` follow):
- `off`: byte-for-byte as before.
- `auto` (new): Lock engages only while the **performance gate** below holds,
  on top of Lock's existing conditions.
- `on`: stays as today (forced, for A/B flights; temporary, listed in the
  Status block).
- The F6 hotkey toggles between `off` and the last non-off value (default
  `auto`); the toast says which.

## The performance gate (a pure function, table-tested)

Re-evaluated once a second from measured inputs, with a dwell (engage after
the conditions held 2 s; disengage at once on a cut, else after the stated
windows; then at least 5 s before re-engaging).

Engage when all hold for 2 s:
- temporal AA on and valid (depth and motion exist) — Lock's own checks
  already cover most of this; reuse them;
- **Elite cannot hold full rate:** its real rate is below 0.9 of the display
  rate (when Lock is not engaged), or the runtime has already halved its
  predicted period;
- **H1, Elite fits two periods:** Elite's GPU frame span plus the synth's
  cost, and its CPU frame time, are each under 0.85 of two display periods
  (medians over the last 2 s; use the measurements the Monitor page already
  has: GPU frame span, game-thread CPU time, the synth's gpu_ms + finish_ms;
  find where each is computed and feed them to the gate cheaply);
- **context admitted (v1):** in a ship or an SRV, including supercruise and
  near surfaces (flown in Phase 1/2); NOT on foot (unflown) — reason
  `on_foot`; this is a deliberate deviation from the design's surface-only
  v1, justified by flights 6 and 12; record it in the journal;
- **no cut in the last 2 s:** loading, a withheld frame, the hyperspace/jump
  tunnel, a camera jump, a `GuiFocus` change to a map/galaxy map/system map
  (journal/Status.json `GuiFocus`; find how EDVR reads it).

Disengage when any happens:
- a cut (at once);
- Elite's work under 0.75 of ONE period for 3 s (it could hold full rate) —
  reason `fits_full_rate`;
- Elite misses its two-period budget on more than 5% of frames over 2 s
  (`late_real` / pairs) — reason `too_slow` (3:1 is not offered);
- the runtime halves its period while engaged (exists: `period_halved`);
- motion invalid (exists).

The shadow-score term of the design is NOT in v1 (the synth's f=1 score only
runs with the shadow key); say so in the journal.

## H1 measurement

A once-a-second runtime or d3d11 line `frame generation gate:` with: state
(off/waiting/engaged), the reason, Elite's real fps, GPU frame span
median/max, CPU frame median/max, synth cost, the budget (0.85 x 2 periods),
the margin, and the context. This is the H1 evidence the design asked for.
The Monitor page block shows the gate state and reason in `auto`.

## Tests

- The gate as a pure function, table-driven: engage after 2 s; each
  disengage reason; the 5 s re-engage dwell; a cut resets; fail-open
  (missing measurements → not engaged, reason named); `on` bypasses the
  performance terms; `off` is never engaged.
- Config contract (choices off/auto/on), menu row, hotkey toggle memory.
- `edvr_log.py --health` rule for the gate line.

## Build policy (the user's)

No full build: compile and run touched rigs, `edvr_log.py --self-test`,
config contract checks, commit, then `python tools\compile_only.py
--build-native --jobs 4` exits 0 with a receipt.

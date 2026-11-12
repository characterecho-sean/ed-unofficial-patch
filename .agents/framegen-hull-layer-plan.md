# Frame generation: the hull layer (spec, 2026-11-08)

For the frame-generation arc (`docs/design-frame-generation-2026-10-26.md`).
Background: `docs/research-own-hull-layer-2026-11-08.md` (Status and
Result). Not started; nothing here is built.

## Goal and non-goal

- **Goal:** the synthesizer knows exactly which pixels of B ride with the
  camera: the own hull and anything else that moves with the ship. It
  warps them and the world as two layers, so the hull edge has one owner.
  Today ownership is guessed from depth and neighbourhood. That guessing
  is the cause of the hull-edge fix chain:
  - `FG_DILATE_MV` (3x3);
  - `FG_HALO` (the 2 px cyan outline);
  - `FG_SENTINEL` / `FG_SENTINEL_SURFACE` (the white blotch);
  - the `synBackAt` test, and `fill_reproj` / `fill_stretch` reading the
    outline back (FG doc entries 2026-11-08, lines ~1414 and ~1597).
- **Non-goal:** inventing the terrain under last frame's hull. The
  offline test refuted that: 94% of the band was unseen for 8 frames. The
  hole S opens beside the hull is still filled by `fgSynHoles`. The layer
  only makes its boundary exact, and keeps fill sources off the hull.

## The mask: "moves with the camera", not "is my ship"

Defining the own ship by record (rig name prefix, camera-containing
sphere) is open: it needs Sean and `analysis\`. For frame generation
the property that matters is motion, not identity, so the mask is
defined by motion. A pixel is in the layer when all of these hold:

1. Its decision path is 11 (engine record) and its engine kind is 1
   (joined): the exact record motion, `temporal_shader_source.h` ~1059-1062.
2. Its record's own motion this frame cancels the camera's ship motion.
   That is: its MV equals the head-only MV within `HULL_EPS` render px
   (start at 1.0, which is the jitter scale). The compose already has
   both, the engine term and the head rows.
3. Its depth is over 0.5 m (keeps the commander's body and near cockpit
   parts out of the test; they are path 1 or 11 too and also ride with
   the camera, so including them is harmless; decide in Phase 0).

What this buys:
- wingmen flying in formation and a docked ship beside a parked ship join
  the layer, which is correct for frame generation (they move with the
  view);
- a station while docked is still, so it is also near the head-only
  motion. Phase 0 must check it does not flood the mask in hangars.
  Fallback: add the 10 m split's own-ship term.

Outline: the cyan silhouette outline and its glow are not pool draws.
They have far-plane depth and are not in MRT6, at R = 2 render px. So the
mask carries a second bit: **ring**, the far-plane pixels within 2 px of
a layer pixel. That replaces what `synHalo` infers.

Format: R8_UINT at render size per eye, bit 0 layer, bit 1 ring. The
temporal pass writes it beside `dlMvSrv` / `dlDepthSrv`
(`temporal_pass.cpp`), only on frames where the engine compose ran. When
engine motion is off (no MRT6) the mask is cleared, and the synthesizer
must then behave exactly as today. Build-gate that in the WARP rig.

## Phases

### Phase 0: offline, no build (decides whether to go on)

On the existing eye dumps (D files: path, engine kind, MV, predicted
depth; motion.csv: the head rows) and the FG replays
(`.agents/framegen-replay-format.md`), compute the mask as defined above:

- **Precision:** the share of path-11 pixels over 10 m (the hull) inside
  the mask (expect over 95%). The share of world (path 2) pixels inside
  it (expect about 0).
- **Hangar check:** one docked dump. The mask must not take the station.
- **Edge census:** on the hull-turn replays (222433569001, 225459033001,
  225627723003, 230157175005, 120812713001), the share of S pixels
  produced by `FG_HALO`, `FG_SENTINEL*` or `fill_*` that lie within 3 px
  of the mask boundary.
  - If under 30%, the edge heuristics are mostly not hull-edge work.
    **Stop here** and record `ruled out: hull layer for FG, because ...`
    in the FG doc.

The scripts go in the shared notes folder, not the repo. The result goes
in a dated FG doc entry.

### Phase 1: the mask, written and captured (no behaviour change)

- The temporal pass writes the mask. The FG synth binds it in the
  resolve and fill passes (a free t-slot; t3, t5 and t10 belong to the
  finisher).
- The replay capture saves it as a new file in the replay folder; bump
  the format version and keep old replays readable. `--replay` loads it.
- The synth line's stats gain `hull=%` (mask share of render pixels) and
  `ring=%`. `edvr_log --health` must name the feature as ran or applied.
- Gates: `tools/framegen_synth_test` checks that mask absent means
  output bit-identical to today, and that the mask's bits survive capture
  and replay.

### Phase 2: the synthesizer uses it, behind `FG_HULL_MASK` (bench define, default 0)

- **Vectors:** in the 3x3 dilation, the halo and the sentinel ring, a
  pixel takes a neighbour's depth or vector only from its own layer. A
  ring pixel takes the nearest layer pixel's vector (what `synHalo` does
  by inference today).
- **Holes:** a hole whose B source was layer and whose S landing is world
  is a disocclusion hole. Its fill reads only world texels: fill keys,
  `fill_reproj` reads and `fill_stretch` steps skip layer and ring
  texels.
- **Keys:** where a layer pixel and a world pixel land on the same S
  pixel, the layer wins (depth agrees today; the mask makes it explicit
  and covers the ring, whose depth is the far plane).
- No config key: if the bench and a flight say it helps, `FG_HULL_MASK 1`
  ships as the behaviour. Then propose which of the old heuristics it
  retires, each one quoted, as AGENTS.md "Scope control" requires.

## Verdicts (pre-written)

- **Bench:** on the five hull-turn replays, the hole-region error (the
  `--replay` metric, milli-luma) improves by at least 10% against
  `FG_HULL_MASK 0`. The detached-outline count on 120812713001 (93 output
  pixels after the 2026-11-08 fixes) is no higher. The surface replays
  regress by no more than 2%. Otherwise: do not fly; record the result.
- **Flight (pilot, Lock on, Anaconda turn over terrain and against
  space):** `hull=` is nonzero in the synth line, and `--health` shows it
  applied. The torn hull outline and the sawtooth behind the strake are
  not visible.
  - Refuted if `hull=` is 0: the mask was never written (engine compose
    off), so the first check is `fix.engine_motion`.

## Cost estimate (not measured)

- Mask write: one R8 store in the compose, which reads both terms
  already. About 7 MB per eye at 2817x2448.
- Synth: one extra R8 load in the 3x3 and the fill loops. The bench's
  time pass measures it (FG doc, Phase 2.3 time entries).

## Environment it depends on

- `fix.engine_motion = on` with `temporal_aa` (MRT6 and path 11 exist
  only then).
- Pool families keyed (`engine_velocity.cpp` kFamilies). An Elite patch
  that recompiles the hull shaders leaves the mask empty, and frame
  generation falls back to today's heuristics; it does not break.
- Measured on the pilot's rig: OpenXR (EDVR runtime), Pimax Dream Air,
  2817x2448 render, DLSS preset K.

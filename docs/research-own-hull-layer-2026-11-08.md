# The own hull as a separate layer: feasibility (2026-11-08)

Question: can EDVR keep the pilot's own hull (the parts outside the canopy,
e.g. an Anaconda's front V wings at 40-80 m) out of what DLSS and frame
generation see, have the world behind it, and put the hull back on top?
Motivation: `design-frame-generation-2026-10-26.md`, entry "2026-11-08:
EDVR's base frame, not frame generation: the wing gap smears with FG off".
The smear is terrain that is newly uncovered at the hull edge every frame
(20-30 render px a frame in a turn at 45 fps). DLSS has no history for it.

## Status

* **Separate mode WORKS in the headset (2026-11-08 ~22:00, build
  4e252a32, FG off):** ship drawn, terrain smear gone, HUD correct.
  Step 1 CONFIRMED (20:30, cfa4e5f5: median 1.000). Steps 2-3, AO, dump
  v2, station fixes, night vision state: their sections; not all flown.
  **Open:** the 6 own forward draws still draw in the world.
* **Benches FLOWN** ("Bench flown", "Flight b30a1c6f"): sep_taa is the AA;
  A', B, tiles run. 5bf8f651 ("Flight 5bf8f651"): wing hazy; reflection
  dump 192439364002: private = game around the pass (median 1.000).
* **Planet through the wing FIXED** (2026-11-11 20:07, 98276da2, sections
  "Unmirrored inputs", "Two input leaks"): the tone's bloom zeroed
  (`tone bloom: zeroed=3478 declined=0`), `eye_sized=0`; the pilot: the
  planet no longer shows on the wing. Direct composite on (`direct=3478`).
* **Frame generation integrated, FLOWN** (2026-11-11 21:02, 9dc9bd31, HMD
  0.5, FG forced on; section "Frame generation integration"): `hull_in_s`
  = `s_frames` (2676), 0 declines; 45.5 real fps to 90; layer 0.97 ms, synth
  1.57 ms; GPU 21.3/18.9 ms (late 0-4/90). Pilot: slight smear, "overall
  OK". Not flown: S first (no world copy), S overlay over tiles ("S first").
* **Not flown:** night vision state; station fixes. **TAA flicker**
  (2026-11-12, "Silhouette coverage test"): edge history kept when its
  coverage agrees, sharpen 0.6 -> 0.3; offline edge flicker 2.0% -> 0.4%,
  lines 1.14% -> 0.57%, lag unchanged; fly: thin lines, edge, head turns.
* **Temporary keys of this arc:** `diag.hull_layer` (off | dump | separate),
  `hotkey.hull_bench`. Temporary code: the busy governor (`kBusyDraws`,
  `kBusyDrawsCulled`), `hull_bench.cpp` and the bench in `hull_layer.cpp`,
  `g_hullFeature` (dlaa.cpp), the bench's `hull_dlss` timer.
* **Background:** ownership is per record ("The binding map"). Rig seed
  (not flown): offline wrong object 14 -> 0; `own seed:` `draw` ~0.
* **Ruled out (this study):**
  * ruled out (2026-11-11, flight of 223a4cda): the direct composite as the
    cause of the planet through the wing, because with it off
    (`copy_why off=3396`) the planet still showed through.
  * ruled out (2026-11-10, offline): the reflection pass's cluster lists as
    the cause of the bright, hazy hull, because its cluster index takes the
    pixel's own depth (t3, the private one) and the list builders
    (593EA04A, 074CB657, F7CED662) read no depth.
  * ruled out: isolating the hull by stencil, because hull pixels carry
    stencil 0/4 like the world (kinematic doc, 2026-11-02 entry).
  * ruled out: a separate DLSS feature for the hull alone (b) as a fix,
    because it gives the world behind the hull no history. It is useful only
    on top of a world-only input.
  * ruled out (Step 3 design, not flown): a second DLSS evaluation as the
    layer's AA, because DLSS carries no coverage channel and its three NGX
    slots are shared with FSR, XeSS and the world route.
  * ruled out: skipping the hull at draw level by shader pair, because
    those pairs also draw other ships and stations in the same instanced
    calls.
  * ruled out: option (e), the hull hole-fill, for the pilot's case, because
    the band was not in any recent frame (section "Result").
  * ruled out (21:45): the composite's cover, depth test, TAA or blend as the
    cause of the missing hull, because dlOut after it is correct on 16 frames.
  * ruled out (2026-11-10, offline): the seed draw as the own seed (wrong
    object in 14 of 45 dumps), and three rig-set rules: section "Rig seed".
  * ruled out (2026-11-10, offline): unknown lights as the cause of the
    ~0% compares, because those 15 dumps are exactly the night-vision ones.
  * ruled out (2026-11-10, bench): exposure for the dark pad hull, because the
    world moved 0.94-1.03, the hull 0.30-0.39, under one tonemap exposure.

## 1. How Elite draws the own hull

- Deferred. The eye pass writes four G-buffer targets plus depth.
  The resolve pixel shaders read depth t0 and G-buffer t1-3
  (`design-flat-temporal-aa-2026-09-23.md:1818-1819`). The fourth target
  is emissive or zero (`kinematic-motion-injection-2026-09-19.md:1640-1647`).
  There is one pass per eye, and offscreen work (shadow maps, probes) runs
  first (`engine-render-pipeline.md` stage 4-5).
- Hull draws are pool families keyed in `engine_velocity.cpp` kFamilies
  (kinematic doc 1912-1920). The hull's depth writers in run 193124490001
  are vs_66DE/ps_864F (most), 61AE, AACF and DE54, all GEQUAL, write on and
  zero bias. Decals on the hull are vs_BBE5/ps_DB3E with depth writes off
  (kinematic doc 4486-4491).
- Cockpit: the shell is vs_BFE51414CC3024B4 / ps_DB79AE788E049DFD and the
  cockpit trim is at stencil 144/148 (kinematic doc 2321, 2330). The canopy
  glass is a separate transparent draw that EDVR already routes into a
  half-resolution glass target (`ui_layer.cpp` ~3128-3140,
  `dlss-blur-elements-2026-10-14.md` 442-483).
- What depends on the hull's depth or G-buffer:
  - deferred lighting, including the `lighting_volume` class in the GPU
    pass profile;
  - fog and atmosphere, which are depth-based;
  - transparents and particles, which are depth-tested against it;
  - the canopy glass drawn over it;
  - EDVR's temporal pass (path 11, `temporal_shader_source.h` ~984-1001 and
    ~1059-1062);
  - the hidden-history test (~684-729, written ~1082-1085);
  - the overlay guard.
  Shadow maps are separate passes. Leaving the eye pass alone leaves them
  intact.
- Elite has no occlusion culling. Everything in the frustum is submitted,
  and early-z rejects what is hidden (`engine-render-pipeline.md` stage 1).
  So the terrain behind the hull is drawn every frame and then discarded.
  That makes (a) possible, but it does not make it cheap: the lighting is
  the problem.

## 2. What EDVR knows about own-ship draws

- **Per pixel: yes, almost free.** The MRT6 slot and depth are written by the
  patched hull pixel shaders, and the compose already tests ownership
  bit-exactly against scene depth (kinematic doc 1928-1940). With a list of
  the own ship's slots, a hull mask is one compare per pixel inside the
  existing temporal pass.
- **Per draw: no.** A pool draw covers an instance range, and the record
  index lives in the instance stream (t33). The per-instance join is exact
  offline (stage 4, "the join is exact via each draw's instance range"), but
  the CPU does not read it at draw time. Splitting a draw by instance would
  be new.
- **Which records are "mine":** `optimized_station_core.h:787-809`
  (`kOwnShipRadiusM = 40`, `ownShipExempt`) and `optimized_station.cpp`
  (`setOwnPrefixAtEngage`, `g_ownCandidateName`) exist, but only in the
  station seal's context. The rig/record tables of the engine motion path
  (stage 2: rig -> collection -> records) are the place to pick out the
  smallest rig that contains the camera. A new per-frame "own slots" list
  would be built from that. It needs a rule for SRV, on-foot, carriers and
  multicrew (no own hull: the feature is off).

## 3. Precedents

- The UI layer redirects the game's own draw by binding EDVR's target
  for that one draw (`ui_layer.cpp` 2655 `beginInner`, 2435 `restore`).
  Draws that write depth or stencil get a second, colour-less issue into
  the game's depth. Its composite runs after DLSS (`uiLayerComposite`
  7728). Its misclassification history is the warning for any draw-level
  hull route: unnamed panel pairs smeared, and world screens were taken by
  mistake (`ui-layer-2026-09-23.md` 41, 430-450, 2078).
- The weapons-fire layer needed a tonemap-difference composite, because a
  post-tonemap add over a lit background was 40-66/255 off at p90
  (ui-layer doc 2068-2077). A hull composited after the game's tonemap has
  no such problem if it is cut from the game's own final image, as in (e).
- `vr_world_route` re-issues one draw immediately with PS slot 0
  swapped. Nothing in EDVR captures a draw to replay later.
- CPU: EDVR's draw path costs 283-349 ns a draw. Re-issues are negligible
  at a few per frame (`vr-draw-path-cpu-2026-11-01.md` Status).
- GPU (gpu pass profile, offline tables, 4 frames, low confidence):
  - scene_gbuffer 0.7-1.1 ms;
  - lighting_volume 0.1-0.6 ms;
  - EDVR's passes 4.9-5.8 ms of an 11-12 ms frame;
  - DLSS evaluate ~0.94 ms in the flat profile (`flat-cz-fps-2026-10-11.md:330`).
  Key: `hotkey.gpu_pass_profile = "F9 60"`; read the `gpu pass profile:`
  lines.

## 4. Options

| | What it takes | GPU / CPU | Patch fragility | If it fails, the pilot sees |
|---|---|---|---|---|
| (a) hull out of the G-buffer, re-issued into a second G-buffer and re-lit | a PS patch that discards own slots in the eye pass; a second issue of the hull families into a private G-buffer and depth; then re-running the deferred lighting, fog, transparents and post on it | lighting + post a second time: several ms; second issue ~0.1-0.3 ms CPU in space, far more at a settlement | the lighting and post sequence is the least mapped part; every Elite patch can move it | no hull, a flat-lit hull, a hull with wrong fog, or particles in front of it |
| (b) separate DLSS feature for the hull | a second NGX feature per eye on a hull-only image | +~1-2 ms a frame | low | nothing gained: the world behind is still unseen |
| (c) the world drawn a second time without the hull, for DLSS only | replay of the whole eye pass | doubles the eye pass's CPU and GPU | very high | frame rate collapse |
| (d) hull reused from the previous frame | an EDVR-kept hull layer, re-lit nowhere | cheap | low | stale lighting on the hull in turns (sun direction changes 1-3 deg a frame) |
| **(e) hull hole-fill + layered composite** | per-eye world cache (colour + depth at render resolution, kept from earlier frames); before DLSS, hull pixels (MRT6 slot in the own list) get the cached world reprojected by the camera term, with world MVs and depth; DLSS then sees a world-only frame; after DLSS, the hull, cut from the game's own frame and accumulated in view space (it is static there), is composited by depth under the cockpit and glass layers | estimate 0.5-1.5 ms an eye (splat + fill + hull accumulation + composite); no extra game draws | low: keyed on the families engine motion already keys; game lighting is untouched | where the fill has no data (terrain never seen) it falls back to today's behaviour; a wrong own list makes the hull vanish or turns another ship into an overlay; the hull edge needs its own AA (from accumulated jittered coverage) or it shows jaggies |

Notes on (e):

- DLSS's history is its own previous output. If no input frame contains
  the hull, the history behind the hull is terrain. It is carried by the MVs
  EDVR writes under the hull. When the wing moves off that terrain, DLSS
  accepts it instead of showing one jittered sample.
- The fill's quality decides the result. Terrain that slid behind a wing
  emerges after about wing width / speed frames (roughly 4-12 at 25
  px/frame). It was seen before and can be reprojected with its depth. The
  FG disocclusion fill (FG doc entry "2026-11-08: torn terrain behind the
  hull") is the closest existing code to reuse.
- Frame generation gains most: the synthesizer can warp the world layer
  with world MVs and the hull layer with near-zero MVs, then composite. The
  hull edge then has no disocclusion at all.
- Hard parts:
  - the composite order: cockpit nearer than hull, canopy glass over hull,
    the HUD layer last (already after DLSS);
  - hull-edge AA at output resolution;
  - particles and lights between canopy and hull, which are rare.

## The deciding experiment (offline, existing dumps)

On `eye_131530778003` (slide-away band, 2-5%) and `eye_132100543006`
(slide-into band, 25-37%), 16 frames each:

1. Hull mask from `EngineSlots.bin` and `EnginePool.bin`: the slots of the
   record(s) owning the 20-120 m pixels at path 11. Take the largest
   record(s) for this desk test.
2. For each frame k, forward-splat the non-hull pixels of frames k-1..k-8
   (C, Z, the camera rows in `motion.csv`) into k's view, nearest depth
   wins.
3. Measure over the band pixels (non-hull in k, hidden bit 16, within 32 px
   of the hull mask):
   - **coverage**: the share the fill reaches;
   - **error**: luma error of the fill against the raw C(k) at those pixels,
     against today's P(k) error there (from the FG doc entry, Laplacian
     P/C 1.15-1.26).

Verdicts, written before the run:

- Coverage at or above 70% and median luma error at or below 6/255
  (the open-terrain P residual): (e) is worth a build. The next step is a
  compile-only instrument that writes the fill under the hull in the dump
  only, without feeding DLSS, flown once.
- Coverage under 40% in the slide-into case (the case the pilot reported):
  the band is mostly terrain never seen, and (e) cannot fix it. Then the
  only remaining lever is frame rate, as the 2026-11-08 entry already says.
- In between: build (e) for frame generation only (layered extrapolation),
  where any coverage helps, and leave DLSS alone.

Flight confirmation, only after a build: in the temporal pass's window
line, a counter of hidden-history pixels along the hull edge, which
should fall to the fill's uncovered share. The pilot's view of the wing
gap in a turn, against `fix` off.

## Result (2026-11-08, offline, no build)

Method as above, with changes forced by what the dumps hold:

- Per-frame depth is not in the dump (Z is frame 0 only). The walk back
  therefore uses frame k's own terrain vector for the first step. Each
  later step uses the median world vector of frame j in a 65 px window.
- Hull = decision path 11 with predicted depth over 10 m. "Seen" = world
  path (2), not hull, and its 3x3 neighbours world too.
- Crop: 1400x1400 (the D crop). MV sign: previous = current + MV, checked
  per dump (median one-step error 7.9 against 26.3, and 0.5 against 2.9).

Script and full tables: shared notes folder, `hull-fill-20261108\`
(`hull_fill_test.py`, `fill_out*.txt`). Results excluding the H6 frames
(rows unchanged, terrain MV 0):

| dump | band px, frames 1-15 | seen within 8 frames | fill error p50 / p90 (luma) | open-terrain floor p50 |
|---|---|---|---|---|
| 131530778003 (slides away, 2-5% of gap) | 36,000 | 44-99% a frame | 7-34 / 29-130 | 6-9 |
| 132100543006 (the reported smear, 25-37% of gap) | 318,910 | **5.8%** | 28.5 / 105.7 | 0.5-6 |

- In 132100543006, 86-97% of band pixels a frame were **still behind
  the ship or cockpit after 8 frames** (250-340 px of terrain travel).
  Only 2-7% left the crop. So the crop is not the cause: that terrain
  was hidden longer than the dump's whole history.
- Even where a fill was found, its error is 3-5 times the open-terrain
  floor. That comes from the walk's motion model (no depth, a constant
  local flow), not from the fill idea, but it does not change the
  verdict.
- **Verdict, against the pre-written thresholds: below 40% in the
  slide-into case. Option (e) cannot fix the reported smear.**
  - ruled out: filling the band from earlier frames (e), because 94%
    of it was not visible in any of the previous 8 frames (dump
    132100543006).
  - Options (a) and (c) remain the only ones that can supply that
    terrain, since the game draws it every frame and depth discards it.
    Their cost (re-lighting and post, several ms) was already judged too
    high. A lower frame rate widens the band.
- **What remains:**
  - the band is DLSS's limit at this frame rate (FG doc 2026-11-08);
  - for frame generation, layered extrapolation (world and hull warped
    separately) still removes the hull-edge disocclusion *between* game
    frames, where the world under last frame's hull is not needed. That
    is a separate design question for the FG arc, not a fix for DLSS.

## The eye pass's draw order (2026-11-08, offline, no build)

Question: can the own hull be split off at the G-buffer, before the lighting,
and what would re-lighting it cost? Source: the eye run ledgers
`edvr_logs\pool\draws_<stamp>.bin` (v2 rows: VS, PS, RT token, instance
range; `tools\eye_run_ledger.py` format), `inst_*` and `pool_*` for the
records. Six captures: 132100543006 (terrain, the smear), 222433569001,
225627723003, 120812713001 (space), 152830339005 (station), 202822011003
(busy station, 5,406 draws). Scripts: shared notes `hull-fill-20261108\`
(`draw_order.py`, `hull_block.py`, `block_pos.py`).

The frame, the same in all six (L then R where the stage is per eye):

1. Depth-only prepass into each eye, null PS, ~40-50 draws an eye
   (`ACE405F4`, 26k-104k vertices a draw, not pool draws: terrain). The
   hull is not in it.
2. G-buffer, left eye, then right. Inside it: cockpit shell (`BFE5` x15),
   pool draws, then **the own ship as one contiguous block of 37-40 draws**
   (AACF 10, DE54 13-15, 66DE 13, 61AE 1; 1-8 instances a draw), then the
   terrain (`6C320453`, `72BDD292`), then more pool draws.
3. Hull decals (`BBE5`) as their own run after each eye's G-buffer, 22-25
   draws an eye (hundreds at a station: other ships').
4. Screen-space passes, 3 draws an eye (`DEF19B03`, `53211E8C` x2).
5. **Lighting: 4-12 draws an eye** (`7E38A6AA`, `8289669D`, `24DE25E4`,
   `0357BBB2`, `963B52C7`), instanced volumes, 279-2,693 instances.
6. Forward and transparents into the lit target, 57-200 draws an eye
   (34-156 of them pool draws).

The block is the own ship:

- Its records are the same 54-57 in every capture and every frame, 60-75 m
  apart, and none of them is drawn by any other hull-family draw. At the
  busy station the 32 other ships' records are all outside it.
- The same 11-13 records also take the hull decals.
- Not yet checked: that the camera sits inside the block's extent (needs
  `camera_<stamp>.json` and the pool's frame of reference).

What this changes:

- The split can be per draw, not per pixel. The own ship arrives as one
  batch, not mixed into other ships' instanced calls. The ruled-out line
  above is about a rule by shader pair, and it stands; a rule by batch
  (the family block after the cockpit shell, or by its records) is new.
- The terrain is drawn after the hull with the prepass's depth. With the
  hull block left out of the main G-buffer, the terrain fills the hull's
  pixels by itself.
- Re-lighting a hull layer is ~10 draws an eye, not a re-run of the
  pipeline: re-issue the block (~40) and its decals (~22) into a private
  G-buffer and depth, then the 3 screen-space and 4-12 lighting draws with
  those bound. That is ~80 immediate re-issues an eye, ~25-50 µs of CPU at
  the measured 283-349 ns a draw. GPU: about lighting_volume's 0.1-0.7 ms
  again, less if scissored to the hull, plus the terrain now shaded under
  the hull. **Finding 3's "several ms" was wrong for (a).**

### The five checks (same day, offline; scripts `slot_check.py`, `forward.py`, `rdef.py`)

1. **Pixel ownership: passed.** EngineSlots (MRT6, code = 2 x slot + 1)
   against the block's records of the matching ledger frame (slots frame N
   = ledger frame N+1): **100% of the keyed pixels at 10-150 m** belong to
   block records, 451,748 px in 132100543006 and 427,584 px in 222433569001.
   So do 46-48% of the keyed pixels under 10 m (the ship's own near parts;
   the rest is the cockpit shell). One frame off, the match is 0%: record
   indices are reassigned every frame, so the own list must be rebuilt per
   frame from the block, never cached.
2. **Lighting inputs: rebindable.** From the shader notes in
   `design-flat-temporal-aa-2026-09-23.md` (1818, 4616-4624): the light
   draws load depth at t0 and the G-buffer at t1-3, and write two targets.
   `8289669D` / `C6E6E419` (point and spot volumes) adds a structured light
   list at t0 of the VS. `7E38A6AA` is the full-screen pass. The 3
   screen-space draws are a copy (`DEF19B03`) and full-screen triangles
   with inert PS (`53211E8C` / `B403F48C`). These shaders are not in this
   machine's `edvr_logs\shaders`, and their RDEF is stripped, so the
   exact slot list is a check for the first build's census.
   **Atmosphere and sky are in the forward pass**, not in the lighting:
   `41E245D4` and `D95905C1` (atmosphere, depth read as a texture) and
   `F8FA801F` (sky), one each an eye, right after the lighting. In the world
   layer they see world depth, which is right for the terrain behind the
   hull. The hull layer needs those 2 draws re-issued against its own depth
   (inscatter over 40-80 m is small, but the colour shift is visible in a
   thick atmosphere).
3. **Forward pass: 6 own-ship draws an eye.** In all three captures
   checked, exactly six forward draws take block records: `88DCF116`
   (the laser), `025B4B9F` and `0B5981F2` / `C5A5C7E8`, `F464E27C`,
   `6DB587D2` and `2B3F53DD` / `B2DE0A41`. They move to the hull layer like
   the block. Everything else in the forward pass (HUD, sky, starfield,
   atmosphere, billboards) stays in the world layer. The flight HUD
   `B7790CBF` is routed by the UI layer already.
4. **Composite: after DLSS, through a re-issued tonemap.** EDVR's temporal
   pass, and so DLSS, runs on the tonemapped eye (`black-holes.md` B4;
   `cockpit-hud-layer-design-2026-09-27.md` 117). So the hull layer cannot
   ride the game's tonemap. It needs the game's own tonemap draw re-issued
   on it, as the UI layer's tonemap re-issue does (do not transcribe the
   tonemap: the cockpit HUD design rejects that). Then it is upscaled and
   composited by depth against the main depth, which keeps the cockpit in
   front. Side effects to check: exposure is metered without the hull (the
   world-only image), and bloom from the hull's lights is lost unless the
   layer's own bloom is added.
5. **Shadows: unchanged.** The ledger has no depth-only eye draws (RT -1)
   in any of the six frames, so the shadow maps are drawn outside the eye
   pass. Leaving them alone keeps the hull's shadow on the terrain and on
   itself.

**Re-issue budget per eye:** the block (~40), its decals (~22), the
screen-space passes (3), the lighting (4-12), the 6 forward draws, the
atmosphere (2) and the tonemap (1-3). That is about 80-90 draws, all
immediate. The hull layer's own anti-aliasing is still open: a second DLSS
feature (~1 ms) or EDVR's TAA on a layer that is static in view.

### The binding map, from an existing census (same day)

The pilot's flight at 13:14 already carries a full draw census: census 7 in
`edvr_gfx_20261108_131109.log`, frame 1, 597 eye draws. It has no `mt=`
(render targets 1-3), which commit 6727f7ff adds. Script: shared notes
`census_hull.py`. Left eye (the right eye is the same at #405-552):

| Stage | Draw | Writes | Reads |
|---|---|---|---|
| G-buffer | hull block (`DE54` ...) | RT0 f23 `E76C20`, RT1-3 not recorded; depth f19 `E78A60`, GEQUAL, write on | material textures, the pool at VS t33 |
| Depth copy | #237 `DEF19B03`/`CB95394B` | f39 `E76EE0` | the eye depth `E78A60` |
| AO apply | #429 `DEF19B03`/`06CD42A9` | blends into f27 `E771A0` (a G-buffer target) | a 704x612 f60 target, made offscreen |
| Shadow mask | #430-431 `53211E8C` x2 | f60 `E77F60`, stencil 10 | depth copy, G-buffer `E75BA0` and `E771A0`, a 1024x512 lookup |
| Lighting | #432-435 (`7E38A6AA`, `8289669D`, `24DE25E4`, `0357BBB2`) | HDR f26 `E77460` (R11G11B10), additive | depth copy, G-buffer `E76C20`, `E75BA0`, `E771A0`, shadow mask; `24DE25E4` also a 1536x1536 shadow map and an 896-byte light list |
| Sky, atmosphere | #443 `F8FA801F`, #446 `41E245D4`, #447 `D95905C1` | HDR `E77460`; `41E245D4` writes depth too | depth copy, sky and LUT textures |
| Own forward | #450-480, the six pairs | HDR `E77460`, depth test on | their own textures and the pool, nothing screen-space |
| Tonemap | #592 `2D78DC3F`/`99C21CEB`, a 3-vertex triangle | f27 `E77CA0` (8-bit eye) | t1 = HDR `E77460`; t0 = `@2000`, not in the id table (an offscreen product: exposure or bloom) |

What it settles:

- **Closed by the `mt=` flight** (build 639c02a9 = 6727f7ff
  cherry-picked, `edvr_gfx_20261108_173723.log`, census 1 = capture
  180353328001, complete: 1,955 ids interned, no overflow):
  - The hull block and its decals write four targets: RT0 f23
    (normal or material), RT1 and RT2 f27, and **RT3 = the HDR target
    itself** (f26, the one the lighting then adds into), plus depth f19.
    The G-buffer's emissive goes straight into the HDR image. A hull layer
    therefore needs its own HDR target as RT3, cleared to zero. The
    lighting adds into it, and the world's HDR gets no hull emissive.
  - The lighting draws (now 11 an eye: `7E38A6AA`, `8289669D`, `963B52C7`,
    `24DE25E4`, `B121A79E`, `0357BBB2` x3+) write RT0 = HDR and RT1 = an
    eye-size f53 (R16) target. No recorded draw or dispatch in the census
    reads that R16 target (PS t0-7, VS t32-39, compute SRVs). For the
    re-issue a scratch R16 target is enough; whatever reads it later keeps
    the world's.
  - Census 2 (capture 180426745002, docking) stopped at the default
    4,096-line cap after frame 0 (26,231 draws). For station dumps set
    `advanced.census_lines = 16384`.
- Every screen-space input the hull layer needs is made inside the eye
  pass from depth and the G-buffer: the depth copy, the shadow mask and the
  lighting. All of them can be re-issued against a private G-buffer.
- Two inputs come from outside the eye pass:
  - **AO** is a compute chain. Depth is reduced to 176x153 (f39), then a
    dispatch (`ch=D31E7812`, 44x39 groups) writes a 704x612 f60 target,
    and #429 blends that into G-buffer `E771A0`. On a hull layer the AO
    would be the world's. Either re-dispatch the chain on the hull's depth
    or apply none to the hull.
  - **The tonemap's t0** (`@2000`) did not resolve (census 7's id table
    was full: 2,048 interned, 2,547 overflowed), and no draw, copy or
    dispatch in the census writes it by token. The tonemap reads only t0
    and the HDR, so no separate bloom texture enters it. t0 is most likely
    the exposure value from the compute exposure chain
    (`gpu-pass-profile-2026-10-12.md`, class `exposure`). Then the
    re-issued tonemap applies the world's exposure to the hull, which is
    what the game does today.
- The atmosphere writes depth, so in the hull layer it must run after the
  own forward draws or with depth writes off.

**Next: no flight of its own.** Every eye dump (`[hotkey] dump_eyes`)
already arms a three-frame census with offscreen draws (`DC eye capture
link`). Commit 6727f7ff adds `mt=` to the census, and it compiles (compile-only,
2026-11-08). Carried into the next flight build of any arc, one eye dump over
terrain names G-buffer RT1-3 and the lighting's second target. Read the dump
with `census_hull.py <log> --census N`. The open design items are then the
AO re-dispatch and the hull layer's anti-aliasing.

## Step 1 flight (2026-11-08 19:20, build c80a4ef1, dump 192424067001)

Log `edvr_gfx_20261108_192046.log`, build verified (`--expect-build
c80a4ef1`). VR, OpenXR (EDVR runtime), 2817x2448 render per eye.

- **What worked.** The layer ran (`state=applied`). It found a block in
  every eye-frame and re-issued it into private targets, and all three
  stages were substituted. The dump wrote HullHdr, GameHdrSun and
  HullDepth for both eyes. The private depth covers the lower 34.5% of the
  view, where the nose is. The game's frame was not touched.
- **The verdict cannot be read.** `hull_layer_compare.py` says REFUTED
  (99% of the private hull black), but GameHdrSun is black too: 0.1% of
  all pixels are non-zero, terrain included. The comparison point is
  wrong, not the layer.
  - ruled out: `7E38A6AA`/`7CECABDE` as the sun pass, because the game's
    HDR right after it is black on 99.9% of the frame (dump 192424067001).
    The sun and the other lights are in the volume draws after it.
    `24DE25E4` and `B121A79E` read the 1536x1536 shadow map.
- **The block over-takes.** The census of the same dump has 92 hull-family
  G-buffer draws an eye, not 39. Another ship's draws follow the own
  block with gaps of 1-13 pool draws. The layer took 84 in the dump frame.
  - ruled out: recognising the own ship as the contiguous hull-family run
    after the cockpit shell, because the game draws front to back and a
    neighbouring ship joins the run (census of 192424067001, 92 against 39
    an eye).
- **Two code bugs:**
  - A run never ended at the terrain (`ended=gap` on all 2,858 blocks).
    The "pool draw" test reads whether VS t33 is bound, but D3D state
    persists, so t33 is still bound on the terrain draws.
  - The second dump (192435503002) captured nothing. The 16-mirror cap was
    full of the first targets when the game made a new eye depth (log
    19:24:25).
- **The ledger lost its instance streams.** Both dumps of this flight
  wrote "0 instance streams (... stride 0)". Every earlier dump today
  wrote 19, including 180353328001 on 639c02a9. Without them the records
  per draw cannot be joined, which item 2 below needs. The suspect is the
  hull layer: it was on, and the code otherwise differs from 639c02a9 only
  by the FG gate commits. Check this with the key off before v2.
- **Later dumps of the same flight (19:28-19:30, key still on) have
  instance streams again** (19 each: 192850710003, 192950193004,
  193014549005, 193029413006, 193050273007). So the first two empty
  ledgers were not the layer's doing. Script: shared notes
  `own_records.py`, `cb_per_object.py`.
  - In all five, the **first hull-family G-buffer draw after the cockpit
    shell belongs to the own ship**. Clustering each draw's records by
    position (120 m) gives object A with exactly 54 records every time.
    100% of the keyed pixels at 10-150 m are A's, and 98-99% under 10 m.
  - Neighbour ships are drawn interleaved with A, for example
    `ABBBCAAAA...AACCBB` and `AAAA(x5)AAAAAAAA x AAAA...xxxxx`. So no run,
    gap or order rule can isolate A.
  - Per draw, nothing CPU-visible separates A from a neighbour. VS cb0 is
    unbound (`c=-`). VS t33 and t38 are the same views for all draws. Only
    the mesh range (`ia=` start index, base vertex) differs, and a
    neighbour of the same ship type would share it.
  - ruled out: telling the own ship's draws apart on the CPU by binding,
    order or contiguity, because neighbours interleave and share every
    binding (census 6 = 193029413006: 71 hull draws, 26 of them a
    neighbour's, all with identical tokens).
- **Own-ship identity on the GPU, per instance (the design for v2):**
  1. **Seed.** At the first hull-family G-buffer draw after the shell, the
     CPU knows its instance range (start instance, count) and has the
     instance stream SRV bound. A tiny compute dispatch reads the seed
     records' positions from the pool. It marks every pool record within
     120 m of their centroid in an own mask (2,048 bits). The log line
     prints the mask's count (54 expected for the Anaconda).
  2. **Filter in the pixel shader.** The four hull families are already
     patched by engine motion (MRT6 slot export, `dxbc_engine_velocity.h`).
     Add an own-mask SRV to the patch:
     - in the main pass, discard own instances (Step 2);
     - in the private re-issue, discard all others (Step 1).
     Then every hull-family G-buffer draw is re-issued (70-90 an eye),
     and mixed draws are handled per instance. Decals need the same
     filter.
  3. It depends on `fix.engine_motion` (the patched shaders). Without it
     the layer is off.
- **Next build (Step 1 v2):**
  1. Re-issue every lighting draw of the eye, not the sun alone, and
     snapshot the game's HDR after the last lighting draw, before the sky.
     The comparison is then exact, apart from AO and decals.
  2. Recognise the own ship by record, not by run. Options: a per-draw
     test of the draw's records against the camera (the own ship
     contains it); or keep the run but end it at the first hull draw whose
     records lie outside the first draws' cluster. Settle this offline on
     the ledger of a dump with a neighbour ship first.
  3. End a run on the terrain by its VS, without the t33 test. Evict
     mirrors of game resources not seen for a few frames.

## Step 1 build (2026-11-08, compile-only, not flown)

The hull layer in the eye dump only: a private, lit copy of the own ship's
G-buffer, sun and ambient only. Nothing the game draws or presents changes.
Source: `src\d3d11\hull_layer.cpp` (hull_layer.h says what it does), called
from the four draw thunks in `vscreen.cpp` right after the game's own issue.

**The key:** `diag.hull_layer = off | dump` in `[diag]`, TEMPORARY, default
off, live, VR only (not in the flat profile's list). With `dump` it runs every
frame; an eye dump (`hotkey.dump_eyes`) arms the capture for the next whole
frame. With it off the draw path pays one bool load. With it on, the private
mirrors cost video memory. Per eye at 2817x2448 there are nine of them:
RT0-2, the HDR, depth (8 bytes a texel), the depth copy, the shadow mask
and the R16. That is about 0.25 GB an eye and 0.5 GB for both. Expect the
frame time with the key on to be a little worse; that is not the layer's
production cost.

**What runs, per eye:**

1. *The block.* The run starts at the first hull-family draw (VS 66DE, 61AE,
   AACF, DE54) after the cockpit shell (VS BFE5) into the same eye depth, both
   drawn with four render targets and a depth bound. It holds up to 6 non-hull
   pool draws (VS t33 bound) in a row and ends at a non-pool draw, the terrain
   (`72BDD292...` or `6C320453...`) or a longer gap. The eye is known by the
   depth texture's identity; frames by vScreen's frame boundary. Each block
   draw is issued again into private RT0-3 and a private depth made from the
   game's own descs (RT3 is the private HDR). At the block's first draw they
   are cleared: colour to 0, depth and stencil to the values the game last
   cleared that eye depth to (0 and 0 if no clear was seen). Only RT0-3 are
   bound: engine velocity's slot target at RT6 is not written twice.
2. *The stages,* each once per eye after its block: the depth copy
   (`DEF19B03`/`CB95394B`), the shadow mask (`53211E8C` with `7EAC7196`, then
   `B403F48C`) and the sun and ambient (`7E38A6AA`/`7CECABDE`). Every bound
   target gets a private copy (the f39 copy, the f60 mask, the R16 scratch are
   made at their first use); the depth target must be the eye's private depth;
   every PS resource (t0-15) whose texture has a private copy is swapped for
   it, by texture identity. The game's clears of a texture with a private copy
   are made on the copy too.
3. *Left out, known differences:* the hull decals (`BBE5`/`DB3E`: their depth
   test would pass for other ships' decals against a cleared private depth),
   the AO apply (`DEF19B03`/`06CD42A9`: the private RT2 has no AO), the point
   and spot volumes (Step 1b), the six own forward draws and the atmosphere.

**The files** (edvr_logs\eyes, EDVRTEX1, read by `tools\eye_inputs.py`):
`eye_<stamp>_HullHdr.bin` (the private HDR right after the private sun draw,
R11G11B10, DXGI 26), `eye_<stamp>_GameHdrSun.bin` (the game's HDR right after
its own sun draw: emissive plus sun, the same stage) and
`eye_<stamp>_HullDepth.bin` (the private depth, DXGI 19). Eye 1's names end
in `_e1`. The header's frame is the layer's own frame count.

**The log lines** (graphics log, untagged; `edvr_log.py --health` reads them
as "hull layer"):

```
hull layer: diag.hull_layer=dump (TEMPORARY; ...)          at each change
hull layer: state=applied frames=2700 eye_frames=5400 blocks=5400 not_found=0
  hull_draws=37/39/40 gap_draws=120 reissued=232200 clears=5400
  subst=copy:10800,mask:32400,sun:21600 no_counterpart=copy:0,mask:0,sun:0
  ended=terrain:0,non_pool:5400,gap:0,frame:0                every 30 s (one line)
hull layer: dump <stamp> eye 0 frame N: HullHdr WxH format 26 written,
  GameHdrSun written, HullDepth written; block 39 draws     per eye, per dump
```

`state` is `no-eye-pass` (no eye drew the shell: never ran on an eye),
`no-block` (it ran and found nothing), `partial` (the block was re-issued but
no sun draw was substituted) or `applied`. `eye_frames` counts eye-frames that
drew the shell, `blocks` those with a block, `not_found` frames with no block
in either eye; `hull_draws` is min/median/max of the block's hull draws an
eye (the ledgers say 37-40); `subst` counts targets and resources swapped a
stage, `no_counterpart` frames where a stage's pair ran with no private eye
behind it (a use of the same pair before the block also counts there);
`ended` says how each run ended. No GPU time yet: the CPU time is the
`hull_layer` caller of the `draw reissue CPU:` line.

**Flight plan:** fly over terrain with `diag.hull_layer = dump`, take one eye
dump, land. First `python tools\edvr_log.py --target steam --expect-build
<commit> --health`: "hull layer" must be OK, with hull draws 37-40 an eye.

**Offline comparison:**
`python tools\hull_layer_compare.py <edvr_logs>\eyes\eye_<stamp>` (add
`--eye 1` for the right eye). Hull pixels are where the private depth is not 0.

- **Confirms Step 1:** HullHdr equals GameHdrSun on the hull pixels within a
  small tolerance (the tool: median luma ratio 0.95-1.35 and at least half the
  hull pixels within 5%), except where the decals or the game's AO differ
  (the layer has neither; where the game's AO darkens, the layer reads
  brighter).
- **Refutes it:** the layer is black or unlit on the hull (the lighting or
  shadow-mask bindings are wrong), offset (a whole-pixel shift matches better
  than none), or the private depth is empty (the block rule found nothing).

Not verified before the flight: the hash constants are from the 2026-11-08
ledgers and census; the clear values are the game's as the clear hook sees
them; that the shell and the block draw with four targets bound; and that no
third pass draws the shell into a G-buffer of its own (`eye_frames` would be
more than twice `frames`).

## Step 3 flight (21:45) (2026-11-08, build 3bcb06e7): the hull held to the background

Log `edvr_gfx_20261108_214551.log`; dumps 214749972002 and 215152750003 (the
second taken where the pilot saw the failure). Separate engaged: 1395
frames, 2788 composited eyes, `own_draws=109838`. The pilot saw the hull as
a near-black silhouette over space and shadowed terrain and as speckle over
lit terrain, and invisible looking into black space.

* **The composite is right.** The eye run's P crops are dlOut right after
  the composite (`temporal_pass.cpp`, the pre-UI stage); they carry the
  solid hull on all 16 frames of both dumps (frame 0 equals DlssBeforeUi
  and HullComposite exactly). The T crops (dlSubmit, what goes out) and
  FinalCrisp have lost it: T matches FinalCrisp to 1.5/255, not P.
* **Cause: the legacy UI resolve** (`ui_resolve.h`, `applyUiResolve`) runs
  after the composite and writes dlSubmit from dlOut bounded by the *raw*
  input (`e.dlColourSrv`), which by Step 2 has no own hull. Its corona hold
  (`resolve.y` = `kCoronaSmearLevel` 64/255, always on under `ui_depth`)
  clamps every pixel whose raw 3x3 is faint (luma < 64/255) and flat to the
  raw 2x2 range +-1/255: over space or shadowed terrain the hull becomes the
  background. Where the raw is bright or textured the weight is 0 and the
  hull survives: the speckle. Pixels marked as UI get the same clamp
  (+-ghost tolerance). A numpy re-run of the hold on P with the C crops
  reproduces T to 0.002 (mean abs, 0..1) on the ~31-36k pixels where T and
  P differ, against 0.18-0.56 for P itself; the weight there averages
  0.98-0.99 (script `hold_sim.py`, scratch).
* ruled out: the cover's depth test (reversed Z), the TAA history or the
  composite's blend as the cause, because dlOut after the composite holds
  the correct hull on every frame of both runs.
* ruled out: a stale or one-frame-late tonemap re-issue, because
  `declined=stale:2` in 2788 and the P crops hold the hull every frame.
* ruled out: the final eye built from another source than dlSubmit, because
  T (dlSubmit) already lacks the hull and FinalCrisp matches T.
* ruled out: frame generation, sky ring or sharpen overwriting dlSubmit,
  because FG was off and the T-from-P hold model leaves no residue.
* **Fix:** the composite moves after the UI resolve and lands on dlSubmit
  through a UNORM view (the same view frame generation's finish makes); a
  game format that has no such view has no resolve either, and composites
  into dlOut and copies again. The composite line gains
  `site=after_ui_resolve:N,upscaler_out:M`; on the 21:45 build every
  composite would have read `upscaler_out`. Consequence: the eye run's P
  crops and DlssBeforeUi (dlOut) no longer show the hull; T, the final
  eye and HullComposite do. The hull is no longer held by the resolve
  against UI ghosts, which is right: the raw has no hull to bound it by.
* Unverified: the fix in the headset. As before, anything the game drew
  into its own eye over the hull (not the UI layer) ends up under it.

## AO on the hull layer (2026-11-08, compile-only, not flown)

The private layer had no ambient occlusion: the hull looked flat, without
the darkening around its panels, and AO was half of the Step 1 v2 residue
(p95 ratio 1.013-1.090).

**The chain, mapped** from census 3 of `edvr_gfx_20261108_215951.log`
(capture 220517898003), with the ids resolved from its `DC id` lines.
Frames 0-2 have the same order; frame 1's q:

| Step | CS (`ch=`) | Groups | Reads | Writes | Eye 0 q | Eye 1 q |
|---|---|---|---|---|---|---|
| depth copy (draw) | `DEF19B03`/`CB95394B` | | eye depth | f39 2817x2448 | 1040 | 1265 |
| A | `B49A50B5` | 23x20 | depth copy | f39 353x306 | 1041 | 1266 |
| B | `4FD03C16` | 353x306 | depth copy, eye depth | f39 2817x2448 | 1065 | 1290 |
| C | `CBAAD243` | 353x306 | B's output | **the eye's G-buffer RT0-3** as UAVs | 1066 | 1291 |
| D | `FB277B33` | 44x39 | depth copy (t0, t1) | f39 176x153 | 1293 | 1318 |
| E | `9347F8FC` | 11x10x16 | RT0, D's output, a 768-byte kernel (stride 48) | f60 176x153 | 1294 | 1319 |
| F | `D31E7812` | 44x39 | D's and E's outputs | f60 704x612 | 1295 | 1320 |
| apply (draw) | `DEF19B03`/`06CD42A9` | | F's output | RT2 (blend) | 1296 | 1321 |

Eye 0's D-F and apply run after eye 1's G-buffer and A-C; eye 1's follow
eye 0's lighting. Correction to the brief: C is not a deinterleave into
four quarter-res targets. Its four UAVs are the eye's own full-size
G-buffer RT0-3 (`BBA60`, `BB220`, `B93E0`, `BBD20` for eye 0). The AO
proper is D (a 4x4 deinterleave of the depth copy into 16 slices), E (HBAO
a slice) and F (the slices put back into the 704x612 AO).

**Design.** A, B and C are not re-issued. Nothing in D-F or the apply
reads their outputs, and C would write the private G-buffer by a rule not
yet known. The doubt is recorded below. D, E and F are re-issued, both
modes:

- `exposure_fix.cpp hookedDispatch` calls `hullLayerAfterDispatch` after
  the game's dispatch: one bool load while the key is off.
  `hullLayerBeginDispatch` matches the CS content hash (`hashOf`) against
  `kAoCs` and names the step's eye. It reads the eye's depth copy (recorded
  at the copy stage), its G-buffer RT0-3 (recorded at the seed), or an
  earlier step's output (recorded as each step runs). A step runs only
  when the eye has a private G-buffer this frame, its depth copy was
  re-issued and every earlier step was re-issued. Otherwise a private
  input is last frame's.
- Every bound UAV gets a private copy, made at first use
  (`privateView(ViewKind::Uav)`, with the game view's own desc, so an
  array slice range comes across). If one cannot be made, the step is
  declined, so the game's own target is never written. Every SRV whose
  texture has a private copy is swapped. The kernel buffer, the constant
  buffers, the samplers and the shader stay the game's. The same groups
  are dispatched raw. `hullLayerEndDispatch` puts t0-t15 and u0-u7 back,
  with the UAV counters kept (-1). The swap is one to one and the game's
  dispatch was hazard-free, so no private view is bound as an SRV and a
  UAV at once.
- The apply is a stage (`kAo`, bit 8). It names its eye by RT2 or by F's
  output and declines unless all three steps ran for that eye this frame
  and its AO SRV was swapped. It binds the private RT2 and does not open
  the lighting window. The game issues it before the shadow mask and the
  lighting, so the private RT2 has AO before the private lighting reads it.
- `kMaxMirrors` 32 → 48: the chain adds three private copies an eye.

**Counters**: a new 30 s line, `hull layer: ao: eye_frames= full=
dispatches= per_eye_frame= applied= missed=deinterleave:,hbao:,interleave:,apply:
no_eye=`. `per_eye_frame` is 3 when every step ran. `missed` counts
eye-frames where the game ran the step for an eye with a private G-buffer
and the layer did not. `edvr_log.py --health` reads it, and gives PARTIAL
when the chain ran and no eye-frame had it in full. The same commit fixes
the rule's composite pattern, which had missed the `site=` field since
4e252a32, so the self-test failed.

**What one dump should show** (`hull_layer_compare.py`, HullHdr against
GameHdrLit on the hull pixels the game shows). The p95 ratio residue
(1.013-1.090 before) should shrink to the decal residue. Where the game's
AO darkens a panel seam, HullHdr should now be dark too. The `ao:` line
should read `per_eye_frame=3.00`, `applied` equal to `eye_frames`, and
`missed` all 0.

**Open:**

- C's effect on hull pixels is not copied. E reads RT0 after C wrote it.
  If C changes RT0 on the hull, the private HBAO sees other normals. A
  dump residue that stays at the seams would point here.
- The private depth copy is far (0) off the hull, so HBAO sees no
  occluder beyond the hull: no contact darkening from the cockpit or the
  terrain. This is intended for a layer composited over the world.

## AO flown, decals found missing (2026-11-08 22:50, build 20ad8719)

Log `edvr_gfx_20261108_225009.log`. The AO chain ran in full:
`per_eye_frame=3.00`, applied on every eye-frame. But the pilot saw no
change: the panel "bumps" of the game's own hull were still missing.

The main-pass line says `decal_draws=0 decals_reissued=0`. The own hull
decals (`BBE5`/`DB3E`, the panel details with their normals) were neither
discarded nor re-issued. The game draws them after the eye's depth copy
(census 3 of 215951: the copy at q1040, the decals at q1042-1065), and the
layer's G-buffer window ended at the copy.
- ruled out: AO as the main missing "bumps", because with AO applied in
  full the hull looked the same.
- Fix: a decal window per eye, from the depth copy to the eye's next stage
  (AO apply, shadow mask or light). In it, own decals take the
  own-discard variant in the main pass and are re-issued into the private
  G-buffer.

A blink at the start remains: `failed=composite:2` in the first separate
window, a lost frame each. Open.

**Decals in, but dark (build 5532477b, ~23:00).** The pilot: the panel
details appear, but unlit, "a black outline". In census 3 of 215951 the
game copies G-buffer RT0 just before the depth copy
(`CopySubresourceRegion`, dst `...ACA0` from RT0 `...BA60`, q1039). Every
hull decal reads that copy at t0 as the base it blends over. The layer's
decals read the main pass's copy, which no longer holds the own hull.
- Fix: `hullLayerAfterCopy`. A whole-texture copy out of a mirrored
  target is made between the private copies too. A decal re-issue swaps
  every PS SRV that has a private copy. Counters:
  `hull layer: decals: copies_mirrored= srv_swaps=`.
- ruled out: the CS step `CBAAD243` as the dark decals' cause, because it
  reads only the depth-derived `4FD03C16` output (census 3, resolved ids).

**Close to the original (build 4c9005e8, 23:05).** The pilot: "the picture
is close to the original". Two things remain:
- the outlines (thin panel lines) are a little soft;
- they now flicker "as if there were no TAA".

Fix:
- **Variance clipping.** The history is held to the 3x3 mean ± 1.25
  sigma, inside the min/max box. On thin contrasted lines the min/max box
  swings with the jitter and lets the history jump.
- **Sharpen 0.6** instead of 0.8: it amplified that flicker.

`hlsl_embedded_check.py` now skips C++ comment lines between the literals:
a quoted phrase in one was read as HLSL.

**Build ad1b3906, 23:15-23:18.** The pilot reported:
- a little shimmer remains;
- a "shadow bug": a dark band on the hull body under the white wing beam.

A dump in `dump` mode at the same spot (231824436002; the game draws the
ship, the layer runs beside it) compares the two on the 418,035 hull
pixels the game shows:
- 95.0% are within 5%, with a median and a p95 ratio of 1.000;
- 1.37% are darker by more than 10% and 1.4% brighter, all scattered
  single pixels along thin edges and panel lines (diff map
  `diff_231824.png`, shared notes).

The band is in the game's own lighting as well.
- ruled out: the dark band under the wing beam as a hull-layer bug,
  because the game's HDR has it at the same pixels. It is the sun's
  shadow from the beam, more readable now that AO and decals give the
  hull its relief.

With AO and the decals in, the layer matches the game's HDR closely. The
shimmer is the layer's TAA. In `dump` mode, where DLSS draws the ship,
nothing shimmers.

**The shimmer, shown (dump 232023684003, T15):** a thin diagonal panel line
on the nose block comes out as a dashed staircase. It is thinner than a
render pixel, so with the jitter it is in a pixel one frame and out the
next. The tight clamp (min/max ∩ mean ± 1.25 sigma, current weight 0.1)
reset the history to that frame's broken line every frame.

Fix, inside the hull only (the whole 3x3 is hull, min alpha 1): a loose box
(mean ± 3 sigma) and half the current weight (0.05). The hull moves
against the camera only by the head's motion, which the reprojection
carries exactly, so the history can be trusted, and a sub-pixel line
converges. The silhouette, where the world behind changes, keeps the
tight box. The per-frame kernel widens back to exp(-3.5 d^2). Risk to
watch: a lag on fast lighting changes inside the hull, such as a light
sweeping across it, about 20 frames to settle.

## Dump v2 (2026-11-09, compile-only, not flown)

Goal: one `dump_eyes` press answers the open questions offline, so the
pilot flies less. Everything below runs only with `diag.hull_layer` =
`dump` or `separate`; `off` is untouched. No new config key.

**What one dump now holds** (`edvr_logs\eyes\eye_<stamp>_*`, and the
graphics log):
- **The census, whole.** The eye dump's census ("DC eye capture link")
  records up to 16,384 eye lines, whatever `advanced.census_lines` says.
  The station frames of `edvr_gfx_20261108_231250.log` (censuses 4-6) were
  cut at 4,096. Bounded: when frames 0-1 spend over half the ceiling, the
  census ends after them, and only three censuses a session are lifted
  (about 4.6 MB of the 16 MB log at most each). Lines: `DC eye capture
  link: census=N lines=16384 ...` and `... ends after 2 frames: dense`.
- **The game's own passes, timed:** the dump arms the GPU pass profile for
  4 frames with no delay (`gpu pass profile: armed by the eye dump`;
  refused with a note while a hotkey capture runs).
- **The layer's own GPU time, by stage, every 30 s** (not only in a
  dump): `hull layer: gpu_ms gbuffer= mask_filter= screen= light= tone=
  taa= total=`, the median GPU ms of a timed frame (one frame in eight,
  a timestamp pair per item), with the items and CPU ms a frame. The
  stages are: the G-buffer and decal re-issues; the seed, filter and
  count; the depth copy, shadow mask and AO; the lighting and night
  vision; the tonemap and cover; and the TAA, sharpen and composite.
- **A reference frame (separate mode).** On the capture frame the main
  pass keeps the own ship, as on a fallback frame: the layer runs, and its
  composite is made but not applied. The ship is shown by the game for one
  frame. `GameHdrLit` then holds the game's hull beside `HullHdr`, and
  `_RefDepth` / `_RefGbuf0` (both eyes) replace `_GameDepth` /
  `_GameGbuf0`. Line: `hull layer: dump S eye N: reference frame: ...`.
- **`_HullLights.json`:** every draw of the dump frame between an eye's
  shadow mask and its sky that writes the eye's HDR, plus every
  stencil-only draw there (a light-volume stencil pass). Each draw has its
  VS/PS, instances, whether the layer re-issued it, and whether it is
  unknown. This is for the station lights that did not light the hull.
  Line: `hull layer: dump S: HullLights written: N draws ...`.
- **The TAA sequence (separate mode, eye 0, 16 frames from the capture
  frame).** Each frame `nn` has `_HullSeq_<nn>_Cover.bin` (the cover at
  render size, RGBA8, coverage in alpha), `_Depth.bin` (R32F) and
  `_Comp.bin` (the composite the GPU wrote, output size). All three are
  cropped to the hull. `_HullSeq_00_Hist.bin` is the history frame 0
  read, and `_HullSeq.json` holds the jitter, tangents, head delta and
  translation, depth encoding, sizes and crops. Staging is a ring of three
  sets, written two frames later, at most 768 MB. A frame without eye 0's
  composite ends the run. Line: `hull layer: dump S: HullSeq 16 of 16
  frames written ...`.

**Offline commands:**
```
python tools\edvr_log.py --target steam --expect-build HEAD --health
python tools\hull_layer_compare.py <eyes>\eye_<stamp> [--eye 1]
python tools\hull_taa_sim.py <eyes>\eye_<stamp> --compare
python tools\hull_taa_sim.py <eyes>\eye_<stamp> --current 0.08 --clip-interior 2.5 --sharpen 0.4
```
`hull_layer_compare.py` uses `_RefDepth` in place of the eye run's `Z`
as the mask of where the game shows the hull, so it works in separate
mode without switching the ini. `hull_taa_sim.py` is `kTaaHlsl` and
`kSharpHlsl` in numpy, with every parameter as a flag; its self-test
fails the build if the shader's literals drift from its defaults. It
reports the shimmer (luma std over the last N frames on the hull
interior, simulation and GPU), the sharpness (high-frequency energy
against the last frame's bicubic upscale), and with `--compare` the
difference from the GPU's composites (MATCH: mean ≤ 1, p99 ≤ 4, 8-bit).

**Cost when not dumping:** two QPC reads per layer item and, one frame in
eight, two timestamp queries per item (about 600-800 an eye-frame pair),
read back without waiting. Nothing else runs outside a dump.

**Unverified (next flight):** that the GPU stage times read sensibly and
the timed frame's queries fit (`overflow=0`); that `--compare` reports
MATCH on a real sequence (the bilinear filter's fixed-point weights may
cost a little); that the eye run's `Z` frame and the hull capture frame
are the same (no longer needed for the compare); and the pass profile
next to the eye run.

## Station fixes (2026-11-09, compile-only, not flown)

**The analysis** (read-only, the 23:35 docking dumps: ledgers 233520921004,
233536735005, 233545646006; log `edvr_gfx_20261108_231250.log`):
- At the station the layer re-issued 600-1,700 hull-family draws an eye,
  only 7-15% of them the own ship's. The station and other ships draw with
  the same families, and the CPU cannot tell them apart: the signatures
  are shared and unstable, the own draws are scattered over 40-48 runs,
  and 4-12 draws mix own and foreign instances. The neighbour filter then
  cleared 3.3-4.9 M pixels an eye.
- The extra cost is the foreign re-issue and the filter (est. +2-3.5 ms
  GPU, about 1-1.5 ms CPU); the rest of the layer's cost is fixed.
- The station's lights missed the hull: two light-volume pairs were not
  in `kLightVs`, VS `A1B7CFCD0BE7493E` / PS `2DB678B6B558B604` (box
  volumes, 198-226 instances) and `CE24A73943632F55` / `1F64463B15189104`
  (45-54 instances). Their state is like the other volumes'; no stencil
  marker draw is missing.
- `unknown_light_pairs` was a false negative: the 16-pair cap was spent on
  forward draws (`2CECEC30`, `925ACEDA`) logged while no sky closed the
  lighting window.
- Mirror churn: 131 depth and 81 RG32F mirrors made, 263 dropped, the
  48 cap hit. Offscreen targets (256x256, 512x512, 612x404, 904x404) got
  mirrors, because stages matched offscreen lighting passes (A1B7CFCD in
  DCO lines at 256x256).
- The scratch R16 lighting RT1 (2817x2448 f53) is read by nothing: 27.6 MB.
- The filter and count passes and the slot-target mirror (RG32F, 55 MB an
  eye) exist only to remove foreign pixels.

**What changed** (`hull_layer.cpp`, `engine_velocity.cpp`
`engineVelocityKeepOwnVs`, `dxbc_engine_velocity.h`
`engineVelocityPatchVsKeepOwn`; `hull_layer.h` items 15-18):
1. *Foreign instances culled in the vertex shader.* A keep-own variant of
   the vertex shader bound for the re-issue (the game's, or engine
   motion's slot or second-skin patch of it): `and` v0.x (the instance
   stream's record index, the value the PS's slot input carries) to 23
   bits, `ld_raw` the own mask at **VS t127**, and before every `ret`, when
   the record is not own, SV_Position = (2, 2, 2, 1): every vertex of the
   instance on one point outside the clip volume. t127: the families' VS
   declare t33 (and the second skin t108-t110); the patch refuses a shader
   that names t127, an indexed register range, a position that is not an
   xyzw SV_Position. The re-issue of every hull-family draw and own decal
   (BBE5) binds it with the eye's mask at VS t127; End puts the VS and the
   game's t127 back. A geometry or tessellation stage bound, or a shader
   that declines (said once a shader: `hull layer: keep-own vertex shader
   declined for ...`), is re-issued whole as before.
2. *The filter as the fallback only.* The private slot target is made at
   the first re-issue of an eye-frame that could not take the variant
   (cleared to -1; every later slot-exporting re-issue binds it, so an own
   draw over a neighbour's pixel replaces its slot). The filter and count
   run only when it was made. After 120 eye-frames without it its private
   copy is dropped (`the private copy of eye N's slot target dropped`).
   The report line gains `culled_draws=` and `filter_draws=`;
   `filtered_px` stays and reads 0 when nothing needed the filter.
3. *The busy governor* (temporary code, no key). At the frame boundary:
   an eye with more hull-family G-buffer draws than `kBusyDraws` = 400
   (when a re-issue of that frame needed the filter) or `kBusyDrawsCulled`
   = 2400 (when every re-issue was culled: the station measured 600-1,700)
   puts the layer to sleep: only a count of G-buffer hull draws (RT3 with
   a DSV) runs; no seed, discard, re-issue, stage, AO step, mirror copy or
   composite, and the game draws the ship. 120 calm frames wake it; in
   separate mode the 120-frame hold follows. Lines `hull layer: busy:` and
   `hull layer: busy over:`; `busy=` (frames asleep) on the report line,
   and `state=busy` for a window asleep throughout (`--health`: PARTIAL).
4. *Station lights.* VS `A1B7CFCD0BE7493E` and `CE24A73943632F55` join
   `kLightVs`. The unknown pairs: an HDR writer in the window waits; it is
   said only when a later lighting draw of the same eye follows (the cap
   is spent only on those). A forward draw (writing the HDR with depth
   writes on) or the sky or the span closes that window. `HullLights.json`
   still records from the shadow mask to the sky, unchanged.
5. *Eye size only.* A stage draw (copy, shadow mask, lighting, night
   vision, AO apply) whose targets are not the eye's render size (the
   private depth's) is declined before it opens, ends or counts anything,
   and no private copy is made (said once a stage; `offscreen=` on the
   report line). The tonemap was already matched by the HDR's identity;
   the AO compute steps' own targets are quarter size by design and are
   named by the eye's resources, so they are unchanged.
6. *No scratch R16.* A lighting re-issue binds no RT1 when the game's RT1 is
   a one-channel 16-bit target: a null RTV is legal and drops the writes
   (`null_rt1=` on the report line). No shared scratch was needed.
7. *Scissor to the hull's rectangle:* **skipped.** It needs a cloned
   rasterizer state per game state (scissor enable) for the lighting,
   shadow mask and AO, a dispatch region for TAA, sharpen and composite,
   and a rectangle from the previous frame's private depth (a reduction
   pass); with the foreign re-issue gone it is the smaller cost, and a
   wrong rectangle cuts the hull. Left in "Optimisation debt" item 3.

**Proof** (`engine_velocity_test`, WARP, `keep_own_vs_tests.h`; 19,174
checks pass): a synthetic family-shaped VS (INSTANCEANDMODELDATAINDEX at
v0, the t33 pool read by v0.x), four instances of slots 3, 7, 1000 (past
the mask) and 9, 7 and 9 own: 256 own pixels bit-identical to the plain VS
(colour and depth), 256 foreign pixels never written, depth clip off;
with no record own nothing is drawn. Stream output (a pass-through GS
made from the VS bytecode): 12 own vertices bit-identical in every
output, 12 foreign at the culled point. The real fixtures `vs_66DE` and
`vs_AACF`, driven with a layout from their own signature and patterned
resources, two data sets: 6 own vertices bit-identical, 6 foreign culled,
each. Refused: a VS naming t127, a position register that is not
SV_Position. The lifecycle rig: the bound family VS gets its variant once.
`--corpus` (also run on the local dump, `--partial`: only `vs_BBE5` with
the candidate `ps_7311054A` present) proves `vs_BBE5` the same way; DE54,
61AE (and its second skin) were not in that dump and stay unproven
offline, though the patch takes any family VS and says when it declines.

**One docking flight** (separate, fix.temporal_aa on, FG off; one dump
inside the station, one in flight):
- `edvr_log.py --expect-build HEAD --health`: the hull layer OK;
  `hull layer: keep-own vertex shader made for the game's vs_66DE...`
  (and AACF, DE54, 61AE's second skin, BBE5) and no `declined`.
- The report line: `culled_draws` about the station's 600-1,700 an
  eye-frame, `filter_draws=0`, `filters=0`, `busy=0` (or rare), and
  `offscreen` > 0 at the station.
- `hull layer: gpu_ms`: `gbuffer` near the flight's own-ship-only cost
  (the foreign draws are culled before rasterisation; their vertex work
  remains), `mask_filter` the seed alone, `total` some 2-3 ms below the
  23:35 station.
- In the station dump: `HullLights.json` lists A1B7CFCD and CE24A739 as
  known and re-issued; `hull_layer_compare.py` shows the station's light on
  the hull (HullHdr against GameHdrLit on the reference frame).

**Unverified:** that the GPU drivers drop the culled triangles as WARP does
(by the clip rules they must); that v0.x is per instance for every family
(the layout is not visible to the shader; the seed relies on the same);
the R16 being read by nothing (from the census, not checked at run time);
that every station light is now known (a new unknown pair says so); that
offscreen passes never use the eye's own size.

## Dump gaps fixed (2026-11-09, compile-only, not flown)

A pre-flight review of the station dump plan found four gaps (code
commit 6a86d10e):
- **The busy governor emptied a dump.** Asleep, the layer only counts
  draws, so a station dump had no HullHdr, GameHdrLit, HullDepth, Ref*
  or HullSeq files and an empty `HullLights.json`. Now a dump armed while
  asleep wakes the layer: one warm-up frame (the private copies, aged out
  while asleep, are made again and see the game's clears), then the
  capture frame, then in separate mode the 16-frame TAA sequence. No
  dump is put to sleep while it runs. A dump that woke the layer puts it
  back to sleep after, at the limit it slept at. Lines: `hull layer:
  busy: woken for dump S` and `hull layer: busy: dump S done`. The hold
  (`kHoldFrames`) is unchanged, so after a wake the main pass keeps the
  ship and the sequence records composites made and not applied
  (`applied=false` in `HullSeq.json`). That is the safe choice: the
  discard is not forced on at a station without the hold's clean frames.
- **The stage timer's cap.** `kStagePairCap` is 4,096 timestamp pairs a
  timed frame (was 1,024; a station re-issues 1,200-3,400 items). A pair
  per run of one stage was rejected: the game's draws between the
  layer's items would be timed with them. `edvr_log --health` now gives
  the median by stage over every `gpu_ms` line of the window, and
  `overflow=` with a warning when it is over 0.
- **A refused press** logs `eye dump: press refused, the previous run is
  still writing`.
- **Format 23** (R10G10B10A2_UNORM, RefGbuf0 and GameGbuf0) is read by
  `eye_inputs.read`. And `hull_layer_compare.py <prefix> --lights` lists
  `HullLights.json`'s HDR writers by eye and VS, with re-issued and
  unknown counts.

**Unverified:** that the capture frame after a wake lights the hull like
an ordinary capture (the warm-up frame is the only preparation); that the
eye dump's census still covers the capture frame, which is now one frame
later when the layer slept (the census keeps at least two frames); that
4,096 pairs is enough at the densest station (`overflow=` says).

## Rig probe (2026-11-09, compile-only, not flown)

The question: can EDVR know the own ship's pool (t33) slots exactly on the
CPU, from the engine's rig data, instead of the GPU mask's 120 m sphere?
`src\d3d11\hull_rig_probe.cpp` answers it with a diagnostic only: nothing
drawn changes. It runs while `diag.hull_layer` is `dump` or `separate`; it
has no key of its own.

The chain, per frame:
1. vehicle_rig's refresh publishes the ship's and the cockpit's record
   arrays, `[lo, hi)` at stride 0x2F0 (`g_vehicleRigOwn`).
2. At the emit (FUN_144315A20, job threads), a rig record inside either
   array claims the 0x150 items the call appended, by address, for the
   present frame. The lookup is engine motion's (`collectItems`).
3. A list merge (0x43513A0) moves the claims with their items, by the rule
   primaryCopy's plans use (full nodes keep their addresses, a partial
   node's items move into the destination tail or shift down).
4. At the pool copier (0x4C846A0), a claimed item copied to slot
   `firstSlot + i` sets that slot's own bit, if its pose is still its rig
   record's bit for bit (else `pose_mismatch`: a recycled address). Every
   other copy clears the slot's bit. A DISCARD map clears the buffer's bits.
   So the CPU set is "own records the buffer holds now", per watched
   336-byte pool buffer.
5. At an eye's seed, every ~30 s and on the eye dump's frame, the GPU mask
   goes to staging with that moment's CPU sets. A few frames later it is
   read back without a wait and compared.

Lines (all `hull layer: rig_probe ...`, graphics log, no build tag):
- `rig_probe armed (...)` / `rig_probe off`: the probe follows the layer.
- `rig_probe target ship="AnacondaMkII" ship_recs=60 cockpit="..."
  cockpit_recs=16 frame=F`: the own rigs changed (`-`: none).
- `rig_probe own emits stopped: none for N ms, target known|none` and
  `... resumed after N ms`: a 2 s gap in own emits.
- `rig_probe frame=F present=P eye=E target=S/C ship_recs= cockpit_recs=
  own_emits=s,c appending=s,c claimed=s,c copier_hits=s,c merges_moved=
  spliced= merge_failed= unwound= unconsumed= stale= pose_mismatch=
  locate_failed=s,c hits_no_pool= emit_calls= copier_calls= copier_refused=
  copier_no_pool= maps=discard,no_overwrite,other pool=known|partial|none
  records= written= cpu= cpu_fresh= cpu_retained= gpu= both= cpu_only=
  gpu_only= gpu_only_written= gpu_only_held= state=S`. The counters cover
  the window since the last such line; the sets are the seed's frame.
  `state`: `ok`, `no_target`, `no_emits`, `no_seed` (no seed in 30 s,
  `gpu=-1`), `no_pool` (the t33 buffer is not one engine motion watches),
  `readback_late`, `readback_failed`.
- `rig_probe dump S: HullRigProbe written`: `eye_S_HullRigProbe.json`
  beside the dump. It holds the CPU, CPU-fresh and GPU slot lists and the
  counters. Each `cpu_only`/`gpu_only` slot has its position, from the
  layer's pool copy at the seed (record +16, the seed's own read), and
  whether this frame wrote it. Lists are capped at 4,096.

"Never ran" and "ran, found 0" differ: no `armed` line, no probe at all;
`emit_calls=0`, the emit tap never ran (engine motion not live);
`own_emits=0,0` with a target, no own record emitted; `claimed>0` and
`copier_hits=0,0`, the chain breaks between the emit and the copier.
`edvr_log --health` gives the probe its own row, `hull layer rig probe`:
OK with an ok comparison and copier hits.

What each open question (study `own-ship-from-engine-rigs.md`, "Missing")
reads from:
1. **Do all hull records go through FUN_144315A20?** `gpu_only_written`:
   slots the GPU calls own that this frame's copier filled from unclaimed
   items. With `copier_hits` ≈ `claimed` and `gpu_only_written` 0 (or only
   at neighbour positions in the JSON), yes. Own-looking positions among
   `gpu_only` slots mean another producer (the builder 0x42B7040, the
   primary path) writes own records.
2. **Unchanged records.** `own_emits` against `appending` (an own call
   that appended nothing), `cpu_retained` (own slots held from an earlier
   frame, not rewritten), `gpu_only_held` (slots the GPU calls own that no
   copy touched this frame) and `maps=` (DISCARD against NO_OVERWRITE). If
   the pool is DISCARD-mapped and `cpu_retained` is 0, every own record is
   re-emitted each frame.
3. **Merge relocation for secondary items.** `merges_moved` > 0 with
   `copier_hits` near `claimed` says claims survive the merge;
   `unconsumed` high with `merges_moved` 0 says they do not (or the merge
   never moves them); `merge_failed` counts lists the walk refused.
4. **Hyperspace, multicrew, fighters, on foot.** The stopped/resumed lines
   and `state=no_emits` (hyperspace), the `target` lines (a fighter or SRV
   names other rigs; on foot `-`), `state=no_target`.

**Unverified (not flown):** the merge carry-over (written from primaryCopy's
rule, untested here); that the copier's `frame` equals the emit's for these
items (primaryCopy assumes it; `stale` counts the others); that the t33
buffer is one of engine motion's watched pools (`pool=none` says not);
that a view whose first element is not on a record boundary never occurs
(it gets `pool=none`). The readback is tried 2 layer frames after the copy
and given up after 30. On job threads the copier takes the probe's own
mutex for every span while the layer is on (about 5k a second), and an own
emit takes it once; the emit tap is four atomic loads for any other record.

## Rig seed (2026-11-09/10, offline, then compile-only, not flown)

Flight 2026-11-09 20:26 (build bbdfeb37, 45 readable dumps): the GPU
mask was on the wrong object in 14 (hangar 204100, station 204126-204146,
outside 204217-204304, planet 204541, and 205107-205122, 205312, 205323
in the transport `Lander01` with the mothership airborne). The rig
probe's CPU set was on the own vehicle in all 45, but small: 12-14
cockpit-rig slots before supercruise, 44-47 with the ship rig, 7-74 in
the transport.

**Offline.** `eye_<S>_EnginePool.bin` is the whole t33 pool of the dump
frame (temporal_pass's engine buffer capture, the same buffer and frame
as the probe's: every `cpu_only`/`gpu_only` position in 45 JSONs matched
it). Rules over the full pool, scored against own = within 80 m
(Anaconda) / 30 m (transport) of the CPU centroid, foreign = past 100 m /
60 m, the band between uncertain (dumps; own found / missed / foreign /
band, summed):

| location (dumps) | today's mask | 120 m about CPU | reach + 30 m | 20 m linkage |
|---|---|---|---|---|
| hangar (2) | 176/187/23/18, 1 wrong | 363/0/230/245 | 363/0/0/26 | 322/41/1/9 |
| station (4) | 7/658/202/0, 4 wrong | 665/0/7/8 | 665/0/0/3 | 650/15/0/0 |
| outside (7) | 640/681/127/113, 3 wrong | 1321/0/90/115 | 1321/0/0/22 | 1296/25/10/10 |
| supercruise (11) | 2967/0/0/20 | 2967/0/0/20 | 2967/0/0/0 | 2965/2/0/0 |
| planet (13) | 4611/143/194/220, 2 wrong | 4754/0/66/220 | 4754/0/0/93 | 4522/232/56/192 |
| transport (8) | 1657/782/2712/0, 5 wrong | 2439/0/5/0 | 2439/0/0/0 | 2439/0/0/0 |

Own records lay 13-25 m past the rig records' reach (Anaconda 71-83 m
against 54-61; transport 17-28 against 4-6); the nearest record past
reach + 30 was foreign in every dump. No record of the landed mothership
lay within 30-110 m of the transport (205230-205251).
- ruled out: the seed draw as the own seed, because it took another
  object in 14 of 45 dumps.
- ruled out: a fixed 120 m sphere about the rig set, because it takes
  hangar walls and neighbours (230 foreign records in two hangar dumps).
- ruled out: 20 m single linkage from the rig set, because own parts lie
  more than 20 m from any other record (109 missed in 204601, 68 at
  touchdown).
- ruled out: radius reach x 1.5, because it misses the transport's own
  records 9-28 m out (92 in 205230).

**Built.** `hullRigProbeOwnSlots` hands the seed the content set of the
seed's t33 buffer (own items the copier put there; any other copy and a
DISCARD map clear them) under the probe's mutex, on the render thread at
the eye's seed, before the dispatch. It is the frame being drawn: the
copier fills the buffer between its map and the frame's draws, and in
all 45 dumps the set matched the pool the seed read. The slots go into
the seed's instance buffer; the seed shader takes their mean (records at
the origin skipped), their reach, and marks records within
min(120, reach + 30) m. The reach is held per eye while the own rigs'
generation stays the same (a frame with few slots keeps the radius).
No slot (on foot, hyperspace, the buffer not watched): the old seed.
The probe was already on whenever the layer is; it is now load-bearing.
Offline, the shader's math ported 1:1 (`port.py`): every dump's CPU set
inside its mask, no record past 100 m / 60 m; masks of 95-1092 records
(Anaconda 139-1092 with its hold at 91 m, transport 95-696 at 34-36 m).

Line, every 30 s after `hull layer: state=`: `hull layer: own seed:
rig=N draw=N hold_resets=N rig_slots=lo/mid/hi radius=lo/mid/hi
reach=lo/mid/hi`. `--health`: PARTIAL when every seed was the draw's.

**Unverified (not flown):** the rig seed in the headset; the hold across
a ship/transport switch (it restarts on the rigs' generation); the cost
of the probe's mutex on the render thread twice a frame (a copier span
holds it briefly); a view not starting on a record (falls back).

## Night vision state and the hangar light (2026-11-10, offline, then compile-only, not flown)

The 15 dumps that compared at ~0% (2026-11-09 evening, build bbdfeb37)
are exactly the 15 with night vision on. EDVR's realistic night vision
(`night_vision.cpp`) swaps the draw's PS, sets a dual-source blend, the
exterior gain (8x) at PS b3 and the exterior mask at PS t5, and
`nightVisionEnd` puts the game's state back inside the draw's forwarding.
`hullLayerAfterDraw` ran after that, so the private hull got stock night
vision: private/game median 0.109-0.130, about 1/8. A live bug too: with
night vision on, the composited hull was ~8x darker than the scene.

- ruled out: unknown lights as the cause of the ~0% compares, because
  the bad dumps are the night-vision ones; the "unknown" writers were
  forward draws (glass, stars, scanner, glows, HUD) after the
  night-vision draw, which reopened the lighting window as a `kLightVs`.

Fixes:
- `nightVisionReissueBegin/End` (night_vision.cpp): binds again, through
  the raw entries, exactly what `nightVisionBegin` gave the game's issue
  of the draw just issued (PS; realistic: blend, b3, t5), once; a declined
  draw or night vision off re-issues as the game drew it. vscreen calls it
  around the hull re-issue of a `kNightVision` draw. Rig: the re-issue
  equals the engaged draw (gain 8 included) and restores PS, b3, t5, blend.
- The exterior mask stays the game's (classified from the game's stencil
  just before): bit 16 is clear on 100% of the hull pixels in all 32
  reference eye-frames and on 100% of sky pixels; the world behind the
  hull in an applied frame is stencil 0/4 (section "Status", ruled out by
  stencil), so it is exterior there too.
- FCF7 is `kNightVisionVs`, out of `kLightVs`: still re-issued and staged
  (`stageGameHdr`), but it opens no lighting window, resets no span,
  commits nothing and moves no stage eye.
- `kLightVs` gains A7339D1F (PS 93A4B9DA), the hangar's third box light
  volume, the only "U" row of 204100368002. Census (edvr_gfx 20:28:36): bound
  as A1B7CFCD's PS 6796562E draw, a light buffer at t0 (3920 B against
  8192 B), the eye-size f39/f23/f27/f27 at t1-t4, the 1536^2 shadow map:
  no new substitution.

Offline prediction (`nv_predict.py`, scratch): private HullHdr x8 against
GameHdrLit on the hull (RefDepth mask), the 16 dumps with hull pixels
(204255 and 204304 show none): median
0.99-1.04 in 9 (within 5%: 33-83%), 0.93-0.95 in 4 late ones (within 5%:
9-44%), 0.87 in 204757, 0.56 in 204335, 0.11 in 205323 (12k px). The
residue is the contour and pulse, which the realistic PS draws
differently from the stock one; the fix re-issues the same PS, so the
fixed compare should be closer than x8.

Log: the 30 s `hull layer: state=` line ends `night_vision=N/M`: N
night-vision draws re-issued, M of them with EDVR's state. 0/0: no such
draw; N/0: re-issued as the game's own. `--health` says both.

**Unverified (not flown):** the fixed compare on a night-vision dump
(median ~1.0, no "U" rows after FCF7); A7339D1F as K+R in a hangar dump
with a correct mask; the world's stencil behind the hull in an applied
frame (inferred, not measured).

## Variant bench (2026-11-10, compile-only, not flown)

Builds 76a0fed5 (stage 1) and bc7c052e (stage 2), and the commit that
adds this section. One press of `hotkey.hull_bench` (TEMPORARY, default
unbound) runs the hull variants live, one after another, at the same spot.
It writes `eye_<stamp>_Bench*.bin` and `_Bench.json` to `edvr_logs\eyes`.
`tools\hull_bench_report.py` (committed before) scores them. The spec is
the side session's `bench-spec.md`; this entry records where the build
differs from it.

Environment: OpenXR (EDVR runtime), DLSS on the eyes (the bench refuses
FSR and XeSS), FG off, 2817x2448 render to 3756x3264 output per eye
(x4/3), NGX 310.9.1.

**Variants:**

| id | what runs |
|---|---|
| `orig_a`, `orig_b` | The layer sleeps (`g_benchSleep`): no seed, discard, re-issue, stage or composite. The game draws the hull and the eyes' DLSS sees it. This is not counted as busy and arms no hold. `orig_b` is the stillness control. |
| `sep_taa` | Today's separate mode. dump v2's `HullSeq` covers its last 8 warm frames and its 8 captures. |
| `sep_noaa` | The same TAA CS with its history off (`cb.jit[2] = 0`): the 3x3 reconstruct, then the sharpen. |
| `dlss_full` | A second DLSS on the hull (below). |
| `dlss_sub` | The same, with the bench rect as an NGX output subrect. |
| `native`, `native_fxaa` | `refused: needs shader dump (feasibility builds A and C)`. |

**Machine** (`hull_layer.cpp`, "the variant bench"):

- Phases: Rect, Quiet, then per variant Warm, Capture (8), Settle (2),
  then Drain. Each phase names the frame about to run. `benchStep` runs
  before `decideNext`, `benchDecided` after it.
- Rect: both eyes' cover alpha is staged and read 2 frames later. The
  bench rect is the union bbox, grown by 96 output px and aligned to 3
  render px, so both corners land on whole output pixels.
- Quiet: the bench waits until 2.8 s after the press, so the press toast
  is gone before the first headset frame. `overlay_in_final` is false.
- Warm: 60 frames for the history variants, 8 for `sep_noaa`.
- A frame that `decideNext` makes a fallback, or whose eye-0 composite was
  not applied, does not count as warm. During Capture it restarts the run
  of 8, after an 8-frame re-warm.
- A variant is refused after 150 fallback frames or 4 restarts. 150, not
  the spec's 60: one miss on waking from an original arms the 120-frame
  hold, and that should cost time, not the variant.
- Abort: frame generation, the busy governor, the vehicle (`journalSeatKind`),
  menu or loading (Status.json has no vehicle), the jump tunnel, device
  loss, an eye size change, a non-DLSS upscaler, an eye dump armed, 20 s,
  or a second press. The manifest is still written, with `aborted`.
- Refused at the press (toast and line): diag not separate, not VR, on
  foot, FG on, busy, no device yet, a dump or eye run in progress, the
  last bench still writing, or under 4 GB free.
- Captures: the submitted eye after the composite (`temporal_pass.cpp`,
  outside the composite's condition). Eye 0 gives 8 frames; eye 1 and the
  headset frame (`temporalPassCaptureFinalEye`, region-local, flips in the
  manifest) give frame 0 only.
- Copies go to 16 staging copies, mapped 2 frames later without waiting.
  A writer thread (`hull_bench.cpp`) writes them. A capture run starts
  only with 6 copies free and the queue under 384 MB.
- The mask is `sep_taa`'s TAA history on its last capture frame (RGBA16F,
  coverage in alpha), or `sep_noaa`'s when `sep_taa` has none.

**Measurements** (medians over the capture frames, both eyes summed):

- `layer_stage`: the layer's stage timers. Every frame is timed while the
  bench runs (`g_benchTimeAll`), and each set carries its frame number.
- `main_dlss`: a timestamp pair around the eyes' `dlaaEvaluate`.
- `hull_dlss`: a pair around the second NGX evaluation only. The bench uses
  its own query ring here instead of a new `TStage`. The prep and
  composite CS stay in the `taa` stage.
- `frame_span`: `gpuCensusFrameSpan`, sampled at each capture boundary.
  It is the last completed frame's span, so it lags by 1-3 frames, all
  inside the same variant's warm or capture.
- `cpu_ms`: the stages' CPU ticks per frame.
- `mb.own`: from the descs, the layer's mirrors, tone, cover, masks and
  composite targets. The second DLSS's four private targets are added for
  its variants. The originals are 0.
- `mb.ngx`: the change in `NGX_DLSS_GET_STATS` (on the capability block)
  from the variant's start to its end.
- `mb.dxgi`: `QueryVideoMemoryInfo` CurrentUsage at the variant's end.

**Second DLSS** (`dlaa.cpp` `dlaaHullEvaluate`, `g_hullFeature[2]`):

- A private pair, outside `kUpscalerSlots`. It uses the eyes' create flags
  and preset hints, and picks the mode by size ratio (no range walk).
- The full pair is released before the subrect pair is made, and each pair
  is released when its variant ends.
- `hull_dlss_in_cs` writes the inputs at render size. Where the cover is
  hull: the cover colour, the cover depth, and a motion vector from the
  TAA's head-only reprojection (`pp - pr`). Elsewhere: the eye's own
  `dlColour`, `dlDepth` and `dlMv`.
- `hull_cov_comp_cs` lerps the hull DLSS output over the submitted eye by
  the cover alpha, upsampled bilinearly, inside the bench rect only.
- If NGX refuses the feature, the subrect or an evaluation, that frame
  falls back to the TAA path and the next boundary refuses the variant
  with NGX's reason.

**Checks run:** `compile_only.py --build-native` green at each stage.
`hlsl_embedded_check.py` passes, the two new CS included.
`edvr_log.py --self-test` passes (the new "hull layer bench" row: OK,
aborted, NEVER APPLIED, refused, silent).
`hull_bench_report.py --self-test` passes. The new rig
`tools\hull_bench_test` (`build.bat :rig_hull_bench`) writes a synthetic
press through `hull_bench.cpp`, and `hull_bench_report.py` reads it back
(exit 0, the dlss and refused rows as expected). It was run by hand; the
full `build.bat` has not run it yet.

**Logs:**

- Never pressed: no `hull layer: bench` line, and `--health` says nothing.
- Refused: `hull layer: bench refused: <why>`.
- Ran: `armed`, `rect:`, one `variant <id>: <status> frames= ... reason=`
  line per variant, then `done:` or `aborted:`. `--health` reads them
  into the "hull layer bench" row.

**Stage 3, the shader dump for the native rect.** The feasibility doc's
missing PS are:

- the DE54, 61AE and 66DE 864F, and decal DB3E8D20 PS;
- the depth copy DEF19B03/CB95394B;
- the sun 7E38A6AA;
- the tonemap 2D78DC3F;
- the lights 963B52C7, 24DE25E4 and B121A79E.

`advanced.glare_shader_dump = 1` already writes every VS, PS and CS the
game creates to `edvr_logs\shaders\<vs|ps|cs>_<hash>.dxbc`. Compute was
added on 2026-09-07; the feasibility doc's "CS never dumped" is out of
date. So no code is needed. The key is read when the hooks install, so it
needs a game start. One eye dump in the cockpit names the PS of the three
lights in `HullLights.json`.

**Unverified (not flown):** none of the above has run in the game. In
particular:

- whether NGX accepts a second feature pair and a subrect, and whether
  the subrect's cost scales;
- the hull motion vectors' sign and scale against `dlMv`;
- whether the headset frame lands in the capture's frame (it is taken at
  the first final-eye call after capture frame 0 is decided);
- whether `dlSubmit`'s format reads back (typeless families are written
  as their UNORM);
- the writer's throughput (about 1.7 GB a press);
- the memcpy hitch on the render thread during capture.

## Bench flown (2026-11-10 07:00-07:17, builds 44d83543 and 4fd502ad)

Env: OpenXR (EDVR runtime), 2817x2448 render, 3756x3264 output an eye,
DLSS preset K quality, FG off. 14 presses; `hull_bench_report.py` on each.

- **Second DLSS refused** on 44d83543 in all 11 presses: `0xBAD00002` full,
  `0xBAD00005` subrect. Cause: its colour, depth and motion inputs had no
  shader-resource bind flag (`makeTexture` sets it only for a view). Fixed
  in 4fd502ad; dlss_full then ran: hull DLSS 3.6-4.2 ms both eyes, layer
  total 7.2-8.5 ms, NGX 890 MB, sharpness 1.27-1.48 (sep_taa = 1, the
  original 1.12-1.41), shimmer like the original. dlss_sub still
  `0xBAD00005` (open).
- **dlss_sub's `0xBAD00005`: the feature size** (2026-11-10, after the
  flight, untested). NGX has no output-subrect size; the output region is
  the feature's target size at the output base. The subrect feature was
  made at the whole eye (2817x2448 to 3756x3264), so any base past 0
  overran the output. It is now made at the subrect's own render and
  output size (`dlaaHullEvaluate`); a new bench rect remakes it. The
  signature on the next press: a `dlss_sub` row with `hull_dlss_ms` and
  no refusal. Why it matters: if the subrect DLSS costs near sep_taa, it
  becomes the layer's AA and the TAA the fallback, as in the eyes' path.
- Still presses (071557, 071633): sep_taa 2.1-2.9 ms, about as sharp as the
  original, less shimmer; sep_noaa 1.8-2.4 ms, shimmer 4-9x. Memory own
  1.2-1.3 GB both eyes. The report's `frame ms` doubles on every layer
  variant, which the layer's ms do not explain: not trusted yet.
- **Cockpit details lost** (pilot: the beam above the glass): the console
  grilles and the strut details vanish in every separate variant, at
  pixels outside the composite cover (`Bench_Mask`). The main pass gave
  every own-slot decal (BBE5) the own discard; own records also carry the
  decals on the cockpit's consoles and struts, which the layer does not
  draw, so they were in neither image. Fix (HEAD): decals stay in the main
  pass; a decal over the removed hull projects nothing, the world behind
  is outside its box. The layer still re-issues them on its hull.
  `decal_draws=` in the main-pass line is now always 0.
- **Hull dark on a station pad**, not in the hangar: dluma 0.30-0.39
  (065707, 065728) against 1.05 docked and 1.06 over a planet; the cover
  (our tonemap, before TAA) is already dark. Leading cause: the game's
  reflection pass after the last light volume (`cs_5998146D`,
  `cs_EB0245DE`, indirect over 120 px tiles: depth copy, stencil,
  G-buffer, an environment cubemap array, a BRDF table; adds into the HDR
  at u0 and an R16 target at u1), which the layer did not re-issue.
  `GameHdrLit` is staged before it, so the compares never saw it. Fix
  (HEAD, untested): the pair re-issued like an AO step into the private HDR
  with the game's tile list (`hull layer: reflections:` line). Risk: the
  game's tiles come from the main pass without the hull; a tile with only
  space behind the hull may be skipped.
- Also seen, not acted on: six own forward draws (glass candidates) still
  draw in the world; a 6-vertex overlay `5DAD41AA` after the tonemap; the
  report's stillness test flags every flight press (the world moves).

## Alternatives reviewed (2026-11-10, after the bench)

The pilot asked whether the layer's cost and fragility (every missed
lighting pass is a visible hull bug) are justified, and for every
alternative, however wild.

- Measured cost, flight 07:13 (4fd502ad), both eyes: layer 1.2-1.4 ms GPU
  without its TAA, 2.0-2.3 ms with it (gbuffer 0.47-0.6, mask 0.07, screen
  0.29, light 0.17, tone 0.2, taa+composite 0.8); CPU 0.16-0.27 ms. Most of
  it is whole-eye passes, so a ship with little hull in view (Chieftain)
  pays nearly the same. The GPU frame was 11-12 ms at 45-50 fps (CPU-bound,
  low-confidence profile), so the GPU part may not cost frames; not measured.
- ruled out: a DisocclusionMask or bias-current-colour input as a fix,
  because EDVR already marks disoccluded background with the MV sentinel
  (`temporal_shader_source.h` 1082) and preset K reads only that; a mask
  cannot create the history that is missing.
- ruled out: DLSS-G or another vendor FG, because none has a D3D11 path, it
  interpolates (a period of latency) and it would see the same hole.
- **Checkerboard (candidate, offline synthetic only).** The main pass
  discards 1 of every 4 hull pixels (2x2 pattern rotating per frame); the
  game lights the hull itself, nothing is re-lit. The world under the hull
  is rebuilt for DLSS from the last 4 frames' sparse samples; the hull's
  missing quarter comes from its own previous frame. Synthetic test (real
  hull mask 204552648029, real terrain 205107507039, pure pan with exact
  MVs; scripts in the session scratchpad `checker\`): in the trailing band
  today has no sample at all; this frame's quarter alone gives mean 2.1-3.3
  / p95 8-14 (0-255 luma; the true world varies 30-80 across the band); 4
  frames give 0.03-2.4 / 0-9, exact only at whole-pixel speeds. Hull fill
  from the previous frame: 0.13-0.43 mean still, 11.3 / p95 35 at 0.6
  deg/frame head turn, so it needs head-delta reprojection (the layer TAA
  has it). Untested: parallax, lighting change, jitter, the game's
  screen-space effects (AO, reflections) seeing a holed hull. No dump holds
  consecutive hull-free world frames; a real test needs `HullSeq` to also
  write the main-pass HDR and MV per frame, flown in a turn, or the mode
  built and looked at.

## Build A' (2026-11-10, compile-only, not flown): the re-issues scissored

Every private target stays full-eye and every coordinate unchanged; only
the layer's re-issued draws are scissored to a per-eye rectangle. Inside it
the output must equal the previous build's.

- **The rectangle.** The cover CS (`kCoverHlsl`) writes the shown pixels'
  box into a 16-byte buffer (one atomic set an 8x8 group), copied to a
  staging ring and read back without a stall 2+ frames later. The eye-frame
  uses the newest box grown by 50 render px per frame of its age, at least
  150 (head rotation max 50 px a frame), clamped to the eye. Whole eye:
  no box yet or since the last probe, a box over 8 frames old, an empty
  box, and a probe every 64 frames (the rectangle then waits for the probe
  frame's box, so hull appearing far outside it is found).
- **Scissored:** the hull and decal G-buffer re-issues, shadow mask, AO
  apply, light volumes, sun, night vision and tonemap. A clone of the
  game's rasterizer state with `ScissorEnable` (cached by the game's state)
  is bound for the re-issue and the game's state and scissor rects put back
  after; a game scissor already on is intersected. The cover CS writes 0
  outside the rectangle.
- **Not scissored:** the depth copy (the AO's compute steps read the private
  depth copy over the whole eye; a scissored copy would leave old depths
  outside), the AO and reflection compute steps, the cover CS's dispatch.
- **Known difference from the previous build:** outside the rectangle the
  private layer now holds no hull (the hidden hull behind the cockpit was
  drawn there before), so an AO kernel or a reflection ray from inside the
  rectangle that reaches outside finds no hull there. Shown pixels are at
  least 150 px minus the head's motion from the edge.
- **No key.** A comparison uses the previous build's logs; dumps run the
  rectangle too, so their compare checks it against the game's hull.
- **Next flight log:** `hull layer: rect: decided=N rect=R share=S
  whole=no_box:..,stale:..,empty:..,probe:.. boxes=.. scissored_draws=..`.
  Expect R near 0.95 N, share about 0.45-0.65, `no_state=0`, `stale` near
  0; `decided=0` means it never ran. `gpu_ms` expected (estimate, both
  eyes): gbuffer 0.47-0.6 to about 0.3-0.45, light 0.17 to about 0.1, tone
  0.2 to about 0.13, screen 0.29 to about 0.22; total about 0.3-0.4 ms
  less. Look for a seam at the rectangle's edges and hull missing in a
  fast head turn.

## Build B (2026-11-10, compile-only, not flown): the depth prefill

83-87% of the hull's pixels are behind the cockpit shell, yet every one was
shaded into the private G-buffer. Separate mode only, render scale.

- **Prefill, by copy, not by re-issuing the shell.** At the seed (before the
  eye's first hull draw) the game's eye depth holds the prepass, the shell
  (BFE5) and the draws before the own block, and no hull (the main pass
  discards it). At render scale it has the private depth's raster, so it is
  exact. A full-screen pass in the Build A' rectangle writes it to the
  private depth (SV_Depth, depth ALWAYS, stencil off: the private stencil
  stays at its clear, so the hull draws' stencil sees what it saw) and to P,
  an R32_FLOAT copy. Float depth only (R32G8X24 or R32): SV_Depth and P then
  hold the same bits. A hull pixel behind any of it fails the depth test.
- **Reset**, at the start of `finishGbuffer` (the end of the window: before
  the neighbour filter, the dump's depth copy, the game's depth copy, the
  decals, AO, reflections and lighting): a mark pass (the private depth read
  as a texture) marks every pixel the hull did not make nearer than P; a
  reset pass writes the clear depth there (SV_Depth) and the clear stencil
  (REPLACE). "Not nearer" rather than "equal": a draw with its own depth
  test that wrote something farther is behind the prefill anyway.
- **Ordering: reset before the depth copy**, so the AO, the reflections and
  the lighting see a hull-only private depth as before. The AO's cockpit
  contact occlusion (the feasibility's plus) is left for later. The only
  difference from Build A': hull pixels hidden behind what the prefill held
  are now clear in the private depth and G-buffer, so an AO kernel or
  reflection ray from a shown hull pixel no longer finds the hidden hull
  just behind the shell edge, and the TAA's 3x3 nearest reprojection depth
  (`coverDepth`) is 0 there instead of the hidden hull's. The cover is
  unchanged (it already showed only hull at least as near as the main
  depth, which is never farther than the prefill).
- **Not changed:** dump mode (`diag.hull_layer = dump`) does not prefill:
  its main pass keeps the hull. The separate dump copies the private depth
  after the reset. Cost: P and the mark add 5 bytes a render pixel an eye.
- **Next flight log:** `hull layer: prefill: seeds=N prefilled=P in_rect=..
  reset=R unreset=0 skip=mode:0,format:0,view:0,shader:0,targets:0`. Expect
  P = R = N; `seeds=0` means it never ran; `format>0` means the eye depth is
  not float on that rig. `gpu_ms` gbuffer (it now includes the three passes;
  estimate, both eyes) from Build A''s about 0.3-0.45 to about 0.15-0.25.
  Look for the cockpit shell painted as hull (a reset that missed) and hull
  holes at the shell's edges. Risk: a hull PS that turns off early depth
  (writes depth or a UAV) gains nothing.

## Flight b30a1c6f (2026-11-10 20:46-20:53, near a planet)

Build b30a1c6f (compile-only; `--expect-build HEAD` matched), OpenXR (EDVR
runtime), 2817x2448 to 3756x3264 per eye, DLSS 310.9.1 preset K, FG off.
The pilot looked around and turned fast: no seam at the rectangle's edge,
no consoles painted as hull, no hull lost at the edge, no smear band in a
turn over the planet. One bench press, `eye_205053107001` (the stillness
check flagged it, as every press so far: orig_a vs orig_b 6.64).

- Log, 30 s windows 20:51:15-20:53:15: `reflections: dispatches` 6108-7816,
  `declined=0`; `rect:` rect 95% of decided, share 0.41-0.60, `no_state=0`,
  whole eye 83-102 no_box plus a 50-62 probe a window; `prefill:` prefilled
  = reset = seeds, `unreset=0`, no skips.
- `gpu_ms` (both eyes): gbuffer 0.55-0.66, mask 0.07, screen 0.18-0.19,
  light 0.34-0.46 (now holds the reflection re-issue), tone 0.15-0.17, taa
  0.43-0.49; total 1.83-1.99. Against the 07:13 flight (station, 4fd502ad):
  taa -40%, screen -35%, total -10-15%. The gbuffer did not drop with the
  prefill (128 hull items a window here against 108 there; no A/B).
- Bench (frame_ms untrusted as before):

  | variant | layer ms | hull DLSS ms | MB own/ngx | shimmer int | sharp | dluma |
  |---|---|---|---|---|---|---|
  | orig_a / orig_b | 0 | 0 | 0 | 0.42 / 0.31 | 1.29 / 1.38 | 1.00 |
  | sep_taa | 1.95 | 0 | 1249 / 0 | 0.23 | 1.00 | 1.14 |
  | sep_noaa | 1.83 | 0 | 1249 / 0 | 1.02 | 0.95 | 1.14 |
  | dlss_full | 5.77 | 3.94 | 1500 / 890 | 1.85 | 0.61 | 1.14 |
  | dlss_sub | 3.54 | 1.80 | 1500 / 414 | 0.45 | 1.25 | 1.15 |

- dlss_sub works with the subrect-sized feature (af3b03c4): about half of
  dlss_full's DLSS time, at the original's shimmer and sharpness, still
  4x the TAA's.
- **Open:** dluma 1.14 on every separate variant (1.06 over a planet before
  the reflection re-issue, Build A' and Build B). Candidates: the
  reflection pass added where the game's own hull had less; the AO losing
  the hidden hull (Build B resets it, Build A' does not draw it outside
  the box). Discriminator: a dump compare by stage, or the bench with the
  prefill and the reflection re-issue as variants.
- **Open:** dlss_full sharp 0.61 and shimmer 1.85, against 1.27-1.48 and
  the original's shimmer on 4fd502ad. Changed since: af3b03c4 (full path
  untouched by the diff), A' (the cover is zero outside the box; the full
  feature evaluates the whole eye), the TAA tiles. Not chased: dlss_full
  is not a candidate.

## Bench A/B variants (2026-11-10, compile-only, not flown)

Two changes after the b30a1c6f flight: three bench variants to find what
moved dluma, and the composite without its two whole-eye copies.

- **The variants.** `sep_norefl`, `sep_nopre`, `sep_norect` run after
  sep_taa (same long warm, 8 capture frames), each sep_taa with one feature
  off for that variant only: the reflection re-issue (`beginReflStep` returns
  before the swap), Build B's prefill and so its reset (`prefillEye` returns
  after its checks; the private depth stays at its clear, as before Build
  B), Build A''s rectangle (`eyeRect` leaves the eye whole: re-issues, the
  prefill rectangle and the cover unlimited). Each place counts a switch-off
  that would otherwise have run; the row's reason says `feature off N times
  in the capture` (expect about 32 for refl: two steps, two eyes; about 16
  for the other two, fewer probe frames), or `NOT SWITCHED OFF` when the
  feature never ran (then the row is sep_taa again). orig_b stays last.
- **The report** (`hull_bench_report.py`) prints the stage split of every
  separate row and an `A/B <id> - sep_taa:` line: dluma, layer ms and each
  stage's ms as differences. The stage medians were already in the
  manifest's `layer_stage`; only the sum was shown.
- **What the next F11 press should show.** The variant whose dluma drops
  from about 1.14 to about 1.06 or below is the cause. Expected cost
  (estimates): sep_norefl light about -0.2 ms (0.34-0.46 now holds the
  re-issue); sep_nopre gbuffer about +-0.1 (the prefill did not lower it, so
  its three passes may cost what they save); sep_norect gbuffer, light, tone
  and screen up about 0.3-0.4 ms total (Build A''s estimate in reverse).
- **The direct composite.** In `hullLayerCompositeEye` (TAA path) the
  composite copied the upscaled eye into `c.out`, sharpened over the tile
  list, then copied `c.out` back: two full copies of a 3756x3264 RGBA8 eye
  per eye a frame. Now an applied composite writes the hull straight onto
  the eye's listed tiles: `hull_layer_sharp_direct_cs` reads the world
  through a typed R8G8B8A8_UNORM UAV of the eye itself, at the pixel it
  writes only (the sharpen's cross reads the history, never the world), so
  the read and write cannot race and the pixels outside the tiles are not
  touched. Same body as `hull_layer_sharp_cs` (one macro), the same UNORM
  decode and store, so the eye equals `c.out` bit for bit. Chosen over a
  blend onto an RTV (the blend unit's arithmetic is not the shader's, so not
  bit-equal, and a draw needs separate indirect arguments) and over copying
  only the tiles (their list is on the GPU). The eye (`dlSubmit` after the
  UI resolve, or `dlOut`) must carry the UAV bind flag, an R8G8B8A8
  TYPELESS/UNORM texture with a UNORM view, and the device
  `D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD` and `_STORE` for R8G8B8A8_UNORM;
  else the copy path, said once a texture (`composite for eye N writes the
  eye directly` / `copies the eye whole twice: <why>`).
- **Still through `c.out`:** a composite not applied (the eye must not
  change; the history stays warm), the dump's capture frame and the TAA
  sequence's frames (they stage `c.out`), and the bench's dlss_full and
  dlss_sub (their own copy). The bench's other captures read the submitted
  eye, so they measure the direct path.
- **Log:** `hull layer: composite method: direct=N copy=N
  copy_why=not_applied:..,dump_or_seq:..,no_uav:..,format:..,typed_load:..,view:..,shader:..
  bench_dlss=..` every 30 s. Expect direct near composited_eyes and copy
  only not_applied. `gpu_ms` taa (both eyes, estimate at about 0.8-1 TB/s):
  0.43-0.49 to about 0.1-0.2 (four copies of 49 MB read and written gone).
  A difference from b30a1c6f's look would be a bug: the output is meant
  to be identical.
- **Risk:** binding the eye's UAV to the compute stage unbinds the eye
  where it is still bound as a shader resource in another stage (the copy
  never did); the compute slots are put back as before.

## Flight 5bf8f651 (2026-11-10 21:23-21:28, HMD 0.5, preset M)

Render 1878x1632 to 3756x3264 per eye (x2), DLSS preset M, the rest as
before. The pilot saw the wing "see-through". Benches `eye_212720335001`
and `eye_212754918002` (both flagged by the stillness check):

| variant | dluma | layer ms |
|---|---|---|
| sep_taa | 1.41-1.44 | 1.09-1.10 |
| sep_norefl | 0.42 | 0.91-0.92 |
| sep_nopre | 1.40-1.43 | 0.99 |
| sep_norect | 1.39-1.44 | 1.10 |

- The wing in the side window, sep_taa against orig_a: hazy, a soft bright
  blob across it, its shadow band mostly gone; without the reflection
  re-issue it is nearly black. So the game's reflection pass carries most
  of the wing's light, and the re-issue adds about the original's whole
  luminance on top of 0.42.
- Composite `direct` on all 3892 eye-composites, no fallback; prefill and
  rect as before. Prefill saved 0.04-0.05 ms gbuffer and cost 0.04 ms
  screen; the rect nothing measurable at this size.
- Reflection pass (offline, scratchpad `cluster\` notes): t2 the tile list,
  the cluster = tile plus a log-depth slice of t3, t0 per-cluster records,
  t1 object data, t5 the SH probe grid. Candidates left for the 1.44 and the
  blob: (1) the stencil t4 (`(stencil & cb1[26].y) == cb1[26].y` takes the
  last volume and probe only, else blends every local probe); (2) a surplus
  from before the pass (GameHdrLit is staged before it, so no dump compare
  has seen it); (3) the G-buffer occlusion `1-(1-t11.w)*cb1[24].zw`; (4) the
  private R16 u1, made with no data and never cleared (nothing reads it).
- Next: stage the game's HDR and the private HDR after the reflection pass,
  and both stencils at hull pixels, in one dump; clear the private u1.

## Reflection dump (2026-11-10, compile-only, not flown)

Built for the four candidates of "Flight 5bf8f651", in one capture.

- **Stages (capture frame only).** Right after the game's dispatch of each
  reflection step (`beginReflStep`, before any decline): the game's HDR
  (last step wins, so after both), once an eye the textures under t4 (the
  stencil) and t11, and the bound CS cb1 (a staging copy of the whole
  buffer; the 11.1 first constant kept). Right after each re-issue
  (`hullLayerEndDispatch`): the private HDR. At the re-issue's bind: the
  private t4 and t11 (the game's own where an input has no private copy).
- **Files** (`_e1` for eye 1): `eye_<stamp>_GameHdrRefl.bin`,
  `_HullHdrRefl.bin`, `_GameStencil.bin` / `_HullStencil.bin` (the depth-
  stencil raw; the compare takes its 8-bit plane), `_GameT11.bin` /
  `_HullT11.bin`, and `_Refl.json`: `game_steps`, `hull_steps`, `cb1_26`
  (y: the mask the pass tests), the u1 clear value, `why`.
- **If a stage never ran:** its `.bin` is absent, `Refl.json` has
  `game_steps` or `hull_steps` 0 and `why` (no dispatch for the eye, no
  private HDR lit, a view not made, sep_norefl, not re-issued), and the
  log line `hull layer: dump <stamp> eye N: reflection: ...` says `missing`
  per file and `not staged: <why>`. `cb1[26].y=unread:` when cb1 was not
  copied. The compare prints `NOT STAGED` and exits 2.
- **Dump mode too:** the reflection re-issue now also runs on a dump-mode
  capture frame (only then), so either mode gives both sides.
- **u1 clear (the fix, candidate 4):** before an eye's first re-issue each
  frame, the private copies of the pass's other targets (the R16 u1) are
  cleared, to the game's own last clear of its u1 when one is seen (the
  RTV clears are already mirrored onto a private copy), else 0. The 30 s
  line: `reflections: ... u1_cleared=N u1_game_clears=N u1_clear=game:|default:V`.
  `u1_game_clears` > 0 answers whether the game clears that target.

**Next capture** (the as-flown path is separate mode, a reference frame):
keep `diag.hull_layer = separate` (or set it to `dump`), press
`hotkey.dump_eyes` once over the wing, then set the key back if changed.
Then `python tools\hull_layer_compare.py <edvr_logs>\eyes\eye_<stamp>
--refl` (and `--eye 1`). It prints, on the hull pixels the game shows:
luma private/game before the pass, after it, and of the pass's own
addition (median and sum); the stencil's equal share, the share where the
test `(s & cb1[26].y) == cb1[26].y` agrees, true on each side, and the top
(game, private) mismatches; t11.w medians and their difference.

| candidate | shows as |
|---|---|
| (1) stencil | test agreement under 95%; the pairs name the values |
| (2) surplus before | before-pass median above 1.15 |
| (3) t11.w | median abs. difference above 0.02 |
| (4) u1 | after the clear, a pass addition still about 2x with (1)-(3) agreeing: INCONCLUSIVE, the pass's other inputs |

Exit: MATCHES 0 (after-pass median 0.9-1.15), DIFFERS 3 (names the
input), INCONCLUSIVE 1, NOT STAGED or bad input 2.

## Unmirrored inputs (2026-11-11, compile-only, not flown)

The reference-frame dump showed the private HDR after the reflection pass
equal to the game's (median 1.000), yet on normal separate frames the pilot
sees a soft bright blob on the wing. **Hypothesis:** a re-issued pass reads
a game texture the main pass produced (main depth or a pyramid of it, the
game's AO, fog, SSR, a lit HDR copy) that has no private copy. On a
reference frame it holds the hull; on a separate frame, the world behind it.

- **Built.** On separate frames only (not the reference frame), at every
  re-issue (lighting-window draws incl. the depth copy, shadow mask, AO
  apply and night vision; the AO steps; the reflection CS; the tonemap's
  slots other than the HDR; the decals, eye-sized inputs only), each bound
  input left as the game's own is one key {pass hash, stage, slot,
  resource, desc}, kept once a session (a hash lookup afterwards; 256 keys,
  eye-sized ones past that up to 512). An eye-sized input (the eye's render
  size, half or quarter) is watched for 600 frames: the last game draw that
  bound it as RTV or DSV (its PS), dispatch that bound it as a CS UAV, or
  full copy into it, in the frame it was read, is its writer. While a watch
  is open every game draw reads its targets (the only per-draw cost).
- **Log.** Each new key once, at the 30 s report (an eye-sized one waits for
  its writer or the watch's end): `hull layer: unmirrored input: pass=<hash>
  stage=PS|CS t<slot> <w>x<h> <format> mips=N eye_sized=no|yes|half|quarter
  written_by=ps:<hash>(rtv|dsv)|cs:<hash>|copy|- ... mirrored_unswapped=`.
  Then always `hull layer: unmirrored inputs: checks=N (session N) keys=N
  new=N eye_sized=N watching=N capped=N`: `checks=0` means it never ran (no
  separate frame or no re-issue), `keys=0` with checks above 0 means every
  input was swapped.
- **Read next** (separate mode, a few minutes in flight with the wing in
  view): `python tools\edvr_log.py --target steam --expect-build HEAD`, then
  the two greps. **Confirms** the hypothesis: an `eye_sized=yes|half|quarter`
  key on a lighting, AO, reflection or tonemap pass whose `written_by` is a
  main-pass shader (a draw census names it). `mirrored_unswapped=yes` on a
  tonemap slot is a second suspect of the same kind. **Refutes** it: no
  eye-sized key, or only ones whose writer runs before the main pass.

## Two input leaks (2026-11-11, compile-only, not flown)

Flight 2026-11-11 19:40, build 223a4cda (the direct composite off,
`copy_why off=3396`), HMD 0.5, render 1878x1632: the planet still showed
through the wing. The unmirrored-input log found exactly two eye-sized
inputs the re-issues read from the game's world-only frame:

1. `pass=D0A16B9E55BF22CC stage=PS t2 469x408 26 eye_sized=quarter
   written_by=ps:DED8796049C7BB4A(rtv)`: the tonemap re-issue's bloom
   (LUT t0, HDR t1, bloom t2), the game's bloom of its own image. In
   separate mode that is the bloom of the planet behind the hull, which the
   hull's tone adds onto the hull.
2. `pass=DB3E8D20CF53FBC0 stage=PS t3 1878x1632 16 eye_sized=yes
   written_by=copy bind=0x8`, one per eye. Format 16 is R32G32_FLOAT. It is
   not a game G-buffer copy: it is engine motion's own snapshot of its slot
   target (`engine_velocity.cpp` `snapshotOverlayBase`, PS t3 =
   `kEngineVelocityOverlaySnapshotSlot`), taken before the game's decal
   issue. The guarded decal PS reads it only for MRT6's depth
   (`dxbc_engine_velocity.h` `guardedTail`), never for colour. It was not
   mirrored because the copy's source, the slot target, has a private copy
   only once a re-issue needs the neighbour filter (`filter_draws=0` in that
   flight: every re-issue was culled).

Fixes (`hull_layer.cpp`):

- The tonemap re-issue binds at t2 a zeroed texture with the game bloom's
  desc and view (made once per size and format, cleared once), for PS
  D0A16B9E only. The hull loses the bloom that bright things spill over it,
  a known, small loss; a reference frame (main pass keeps the hull) now
  differs from the private tone there by that bloom. Log: `hull layer: tone
  bloom: zeroed=N declined=N last_declined=<why>` every 30 s; both 0 means
  no tonemap re-issue ran.
- The decal t3: `hullLayerAfterCopy` remembers the texture each copy out of
  an eye's slot target went into. In the decal re-issue, when t3 is that
  texture: its private copy when one was made at the same copy (MRT6 bound,
  the filter path), else, with MRT6 not bound, nothing (the PS's only use
  of it goes to an unbound target). Log: `hull layer: decals: ...
  overlay_t3=private:N,unbound:N,kept:N`; all 0 with decals re-issued means
  the snapshot was never seen copied or read. `kept` counts the case still
  open: MRT6 bound and no private copy (the filter's first eye-frame).

Fix 2 cannot explain a planet on the wing (it touches no colour); fix 1
can, where the planet's bloom is bright. **Next session** (separate mode,
the wing over a bright planet): `--expect-build HEAD`; the unmirrored-input
log lists no `D0A16B9E` t2 and no `DB3E8D20` t3 key; `tone bloom: zeroed`
above 0 and `declined=0`; `overlay_t3=` `unbound` above 0; and no planet
visible on the wing. If the planet still shows, the leak is not an input of
a re-issued pass.

## Frame generation integration (2026-11-11, compile-only, not flown)

Option (e)'s note, now on separate mode: frame generation's S warps the
world layer with the world's motion and the hull layer with the head's,
then composites, so the hull edge has no disocclusion in S. Until now
separate mode stood aside while any FG key was on (`fallback=framegen`).

Environment: OpenXR (EDVR's runtime), DLSS, FG `auto` or `on` (Phase 2.4),
S built on the game device in the temporal pass right after B's eye.

Design (`hull_layer.cpp`, `framegen_synth.cpp`, `temporal_pass.cpp`):

- **The stand-aside is gone** (`decideNext`). `fallback=framegen` stays in
  the composite line and reads 0. The variant bench still refuses, and
  aborts, while FG is on (bench runs with FG off only).
- **S's world-only input.** In separate mode the DLSS inputs hold no own
  ship, so the upscaler's output (`dlOut`) is world-only; the hull goes onto
  the submitted eye (`dlSubmit`) after the UI resolve. S reads `dlOut` when
  B's door does not finish S, which is already world-only (`direct`). When
  the door finishes S, S reads `dlSubmit`: the composite then copies the
  eye before it writes the hull (`HullLayerCompositeInputs::keepWorld`, one
  whole-eye copy into `Comp::world`) and `hullLayerSInput` hands S that
  copy (`copy`). Reordering S before the composite was not taken: S needs
  this frame's hull TAA result, which the composite makes.
- **The hull layer in S.** The layer is the composite's TAA result,
  `hist[read]` after the flip (premultiplied colour, coverage in alpha, at
  output size). S is built in the same treat right after the composite, and
  the next write to that texture is the next frame's TAA, so no texture is
  kept for S. Right after S's resample (before the kept luma, the eye
  dump's stage and B's door finish) the synthesizer calls an overlay
  (`FrameGenSynthFrame::overlay` = `hullLayerCompositeS`) with the head's
  step S was built with (`cb.rs`: the runtime's slot pose, else the
  extrapolation). `hull_layer_s_cs`, one thread an S pixel over the whole
  eye: the B position by the step's inverse (rotation, then once more at the
  hull's cover depth when the step carries a translation), the layer
  sharpened as B's composite sharpens it (`hullLayerSharpAt`, shared with
  the composite's shader) at the four texels around it, bilinear, over S.
  The real frame B keeps today's composite unchanged.
- **UI** stays on its own path (B's door finish draws it over S).
- **Dumps:** the eye dump stages S after the overlay. A hull dump's capture
  frame is a reference frame (the main pass keeps the ship): its S takes no
  layer (`not_applied`). The FG replay capture records the world-only input
  without the hull; the replay bench does not draw the layer.

Cost (estimates, not measured): the overlay, a full-eye dispatch, about
0.05-0.1 ms an eye, inside the synth's `synth_ms`; the copy, only when the
door finishes S, about 0.1 ms an eye inside the composite's `gpu_ms`, and
one texture an eye in the eye's format at output size (RGBA8: 47 MB at
3756x3264), made on first use. No new key.

Log, every 30 s in separate mode: `hull layer: framegen: fg=<off|auto|on|
shadow> s_frames= hull_in_s= world_only_inputs=copy:,direct: world_copies=
declined=not_applied:,no_composite:,no_world_copy:,size:,unsupported:`.
`s_frames=0` with FG on: S never reached the layer. Said once: `hull layer:
framegen: the hull layer drawn into frame generation's S` and `world-only
copy made for eye N`. `s_frames` minus `hull_in_s` minus the declines: S
builds the synthesizer abandoned (busy producer texture).

Risks: S's hull goes through one more resample (bilinear) than B's, so it
may look a little softer every other frame; the hull's parallax against the
cockpit in S uses one depth per pixel; with a fallback frame (hold, mask
large) S warps the game's ship with the world, as before.

**Next flight** (FG `auto` or `on`, in a ship over a planet):
`--expect-build HEAD`; `hull layer: framegen:` `hull_in_s` close to
`s_frames`, declines near 0 (`not_applied` only on hold frames);
`world_only_inputs` names the source; the composite line's
`fallback=framegen:0` and `composited_eyes` above 0; the FG gate line still
admits. The pilot: no smear at the hull edge in S in a head turn and a ship
turn, and the hull not doubled. Then FG off: the bench still runs.

## S first, S tiles (2026-11-11, compile-only, not flown)

Flight of 9dc9bd31 (1878x1632 render, 3756x3264 out, RTX 5080): composite
`gpu_ms` 0.145-0.163 with FG on against 0.101-0.109 off; the world copy
(47 MB an eye) about 0.05 ms an eye.

- **S first, no world copy.** `HullLayerCompositeInputs::sFirst`: when the
  door finishes S from `dlSubmit` and the composite is direct, the
  composite runs its TAA and tile list, S is built from `dlSubmit` (still
  world-only), then `hullLayerCompositeFinish` runs the same sharpen onto
  the eye (`hist[read]` after the flip, the same tile list); the luma
  probe's stage 1 moves after it. Same pixels in B and S: S reads what the
  copy held, the sharpen reads the same inputs. Fallback to the copy,
  counted in `copy_why`: `not_asked` (composite into `dlOut`, the hull
  bench capture), `not_direct` (dump, TAA sequence, no UAV), `bench`. The
  composite's `gpu_ms` adds the finish's span to its call.
- **S overlay over tiles.** Measured offline (scratch bench, this GPU,
  timestamps, median of 400): the whole-eye `hull_layer_s_cs` costs
  0.114 ms an eye at 10% hull coverage, 0.134 ms at 25%, so it was built.
  `hull_layer_s_tile_list_cs`, one thread an S tile, maps the tile's
  corners back by the step's rotation, grows the box by the translation's
  reach at the nearest cover depth the tile can read, and lists the tile
  when a B tile of this frame's TAA (`tileState` bit 0) is in it;
  `hull_layer_s_tiles_cs` runs the unchanged body by `DispatchIndirect`.
  Bench: byte for byte equal to the whole-eye pass (old and new shader)
  on seven steps (still to a 12 deg, 3 cm whip; 7-26% of tiles listed).
  Cost 0.040 ms an eye at 10% (list 0.012, overlay 0.026), 0.075 at 25%.
  The whole-eye pass stays the fallback (no tile shaders, no list, tile
  state not kept).

Log: `hull layer: framegen:` now `world_only_inputs=before_hull:,copy:,
direct: world_copies= copy_why=not_asked:,not_direct:,bench: hull_after_s=
lost= overlay=tiles:,whole:`. `lost` > 0: an eye went out without the hull
(the finish never came).

**Next FG flight** must show: `before_hull` close to `hull_in_s`,
`world_copies` near 0 (only `copy_why` cases), `hull_after_s` =
`before_hull`, `lost=0`, `overlay=tiles` = `hull_in_s`, `whole=0`; the
composite's `gpu_ms` back near 0.10-0.11; the hull in B and S as before.

## Silhouette coverage test (2026-11-12, compile-only, not flown)

The pilot: since the hull went through the layer's own TAA, its thin
lines and edges flicker (render scale x2, 1878x1632 to 3756x3264).

**Change** (`kTaaHlsl`, `kSharpAmount`):

- Silhouette only (the 3x3 not all hull): the 3x3's alpha range is kept
  before the 1.25-sigma clip tightens `lo`/`hi`. A history whose coverage
  agrees (`h.a` within that range +- 0.05) is taken unclamped in colour
  (saturated) at the interior's current weight (`jit.w * 0.5`); any other
  keeps the clip. The interior branch and its 3-sigma clamp are unchanged.
- Sharpen 0.6 -> 0.3 (the FG overlay shares it through `hullLayerSharpAt`).
- `tools\hull_taa_sim.py --shader 2` (default) fits builds after b85d728c;
  `--shader 1` (sharpen 0.6) fits the dumps of older builds.

**Offline numbers** (the study's harness on recorded 16-frame TAA
sequences; frames 8-15 against a jitter-free reference; flicker = share of
pixels whose luma changes by more than 4/255 against the reprojected last
frame). Mean of the moving sequences 192439364002, 204234585011,
204757684033, 212720335001; the still sequence 212754918002 separately:

| | line, moving | silhouette, moving | silhouette, still |
|---|---|---|---|
| shipped, sharpen 0.6 | 1.14% | 2.01% | 2.10% |
| shipped, sharpen 0.3 | 0.57% | 2.00% | 2.09% |
| shipped, sharpen 0 | 0.39% | 2.00% | 2.09% |
| new, sharpen 0.6 | 1.14% | 0.44% | 0.05% |
| new, sharpen 0.3 | 0.57% | 0.44% | 0.05% |
| new, sharpen 0 | 0.39% | 0.44% | 0.05% |

One sequence went the other way: 192439364002's silhouette barely
flickered before (0.01%) and does a little now (0.13%).

Lighting step (still sequence looped, colour x 0.6 at frame 32), share of
the step left on the interior after 1 / 4 / 8 / 16 frames: shipped 0.33 /
0.23 / 0.18 / 0.12, new 0.34 / 0.23 / 0.18 / 0.12. On line pixels (which
include pixels next to the edge) 0.83 / 0.65 / 0.50 / 0.33 against 0.88 /
0.68 / 0.53 / 0.35. The silhouette's mean error against the reference rose,
2.55 -> 3.19 (8-bit) moving, and fell, 0.94 -> 0.76, still: the edge
follows the history more and the jitter less.

**Limits of the offline data:** the sequences move little (no fast head
motion: at most a few output pixels a frame), and none holds a frame
generation frame, so neither the edge under a fast turn nor the FG
overlay's sharpen was measured.

**Next flight, the pilot looks at:** the thin panel lines (steadier, a
little softer) and the hull's edge against the world while turning the
head: no smear or trailing edge, no ghost of the hull where it was.
`--expect-build` the branch commit; a dump's TAA sequence checks with
`hull_taa_sim.py --compare` (shader 2).

## Optimisation debt (recorded 2026-11-08 at the pilot's request)

The layer was built to work first. Before it ships as anything but a
diagnostic, measure it, then cut. Ordered by expected size:

1. **Measure first.** Only the tonemap and composite carry GPU time today
   (`gpu_ms=tone,composite`). Add one GPU census scope over everything the
   layer issues per eye: re-issued G-buffer draws, filter, depth copy, AO
   chain, shadow mask, lighting, night vision, tonemap, TAA, sharpen,
   composite. Compare a frame with `separate` against `off`.
2. **Video memory.** 16-48 full-size private mirrors over two eyes (about
   0.5-1 GB). Drop mirrors whose content is not needed after the frame,
   share the scratch R16 and per-stage targets between eyes, and evict
   what a stage no longer uses.
3. **Full-screen passes for a partial-screen object.** The re-issued
   lighting volumes, shadow mask, AO dispatches, TAA, sharpen and composite
   all cover the whole eye, while the hull covers part of it. Bound them to
   the hull's screen rectangle (scissor for draws, a dispatch region for
   compute) from the private depth's extent, or from the previous frame's.
4. **CPU per draw with the key on:** shader-hash lookups and stage
   matching on every draw. Short-circuit by pass: nothing to match outside
   the G-buffer, lighting and post windows.
5. **The neighbour filter pass** runs every eye-frame even with no
   neighbour. Skip it when the seed's pool region has no foreign hull draws.
6. **Frame generation**: integrated 2026-11-11 (section "Frame generation
   integration"); left: the overlay is a whole-eye dispatch (tile-limit it
   to the hull's box moved by the step) and the world-only copy.

## Night vision on the hull (2026-11-08 ~22:15, build 1a1b6c60)

The pilot: night vision now reaches the own ship, but "the ship's outline
used to be wider"; not disliked. Night vision draws its outline at depth
edges, on both sides of the silhouette. In separate mode:
- the private layer has the inner half (the hull pixels);
- the composite's cover keeps only hull pixels, so the outer half, on the
  background side, is dropped;
- the main pass has no hull edge left to draw it.

A possible change, not made: widen the cover to pixels around the hull
where the private HDR received night-vision light. Deferred while the
pilot is content.

**The blink is gone** with the 120-frame hold. **The hull's textures look
soft** (the pilot, 22:20). The layer's TAA read its history with one
bilinear tap at a current weight of 0.1, so a little blur accumulated every
frame. Before separate mode, DLSS reconstructed the hull from the jitter.
Fix: the history is read through a 9-tap Catmull-Rom, then clamped to the
3x3 range as before. If the hull is still softer than the world, the next
lever is the current-frame kernel (exp(-2.29 d^2) over 3x3 render pixels)
or a sharpen on the layer only.

**Still soft after the Catmull-Rom history and a narrower kernel**
(exp(-5 d^2)). The pilot compares it with the first seconds of a session,
when the game draws the ship through DLSS. Measured on dumps 222624596001
and 222629772002 (`taa_blur.py`, shared notes): on the hull interior, the
TAA output holds 26-29% of the high-frequency energy of the bicubically
upscaled single frame. That fits a Gaussian of sigma 0.7 output px, with no
history shift (0 or 1 px). The reprojection is right, and the softness is
the TAA's own.
- ruled out: a misregistered head-motion history as the softness, because
  the best fit has no shift.
- Fix: a sharpen pass after the TAA. A cross unsharp mask, amount 0.8,
  clamped to the cross's range (no halo), only where the whole cross is
  hull, so the silhouette's edge keeps the TAA's anti-aliasing. Then the
  composite. The history is not sharpened.

`tools\hlsl_embedded_check.py` read a compiler blob after releasing it. The
larger TAA shader made that fail with an access violation. It now reads the
blob first.

## Separate mode flown (2026-11-08 ~22:00, build 4e252a32): works; night vision, load blink

Log `edvr_gfx_20261108_215951.log`, FG off. The pilot: the ship is drawn,
the wing-edge terrain smear is gone, the HUD is correct. The composite site
was `after_ui_resolve` on every applied composite.

- **Night vision does not reach the own ship.** Census 3 (dump
  220517898003, night vision off) against census 4 (220530815004, on) at
  the same spot. In the eye pass, night vision adds one draw an eye:
  - VS `FCF7BD2896751D96`, 240 indices, 2 instances;
  - into the HDR (f26), with the eye depth bound, after the lighting;
  - reading t0 a 6x1 f39, t1 the depth copy, t2 G-buffer RT0, t3 a 1x1.

  It also adds an offscreen 256x256 G-buffer pass of the own ship's
  families, which the eye pass does not read. Fix: night vision joins the
  re-issued lighting draws (`kLightVs`). Its depth-copy, RT0 and HDR inputs
  have private mirrors, so the hull layer gets night vision as the game
  draws it.
- **The ship blinked at the start, while textures streamed in.** The 22:01:53
  window: `failed=composite:2`, `fallback=streak:16`, and `mask_pending`
  right before. Each miss lost one frame (that frame's main pass had
  already discarded the ship); the 16-frame hold then let separate resume
  into the next miss.
  - Fix: the hold is 120 frames, and any other fallback ending
    (mask pending, frame generation, outside) starts it too. The layer
    still runs on held frames, so a miss there restarts the hold
    without a visible loss.

## Step 3 flight (2026-11-08 21:39, build 8386287e): separate barely engaged

Log `edvr_gfx_20261108_213914.log`, build verified. FG was switched off
with F6. In the first two windows separate was active on 1 frame;
`fallback=mask_large` on 578 and 1,230 eye-frames, with the own mask at
804-7,168 records. The pilot saw no change (the ship stayed in the main
pass), and "the night-vision smear is still there". Dump 214124277001:
- the composite was made but not applied (a fallback frame);
- the own-discard variants were made for all six hull pixel shaders seen,
  including 61AE and DE54 (two DE54 variants and two 66DE);
- one composite was declined as `stale` (the submit after the frame
  boundary), twice in 30 s.

Cause (`mask_check.py` on 214124277001): 936 of the 1,056 records in the
mask are unused pool records at exactly (0,0,0). This frame the own ship
was near the origin. With them excluded, the mask is 260-697 records on
four ledgers: the 54 hull records, the ~66 the own ship draws with other
families, and its records not drawn that frame. None was a neighbour.
- ruled out: the 400-record own-mask guard, because the own ship alone
  puts 260-700 records in the mask over terrain.
- Fix: the seed skips records at (0,0,0), and the guard is raised to 1,500.
  A guard that counts the own records actually drawn by hull draws (from
  the private slot target) is the proper one, if docking needs it.

## Step 3 build (2026-11-08, compile-only, not flown)

The hull put back after the upscaler, in `diag.hull_layer = separate` only (`off` and `dump`
unchanged). Source: `hull_layer.cpp` (hull_layer.h items 9-11), the call in
`temporal_pass.cpp` right after the upscaler's evaluation, the tonemap hook in `vscreen.cpp`
(`hullLayerAfterDraw` now passes the draw's kind and count).

1. *The tonemap re-issue.* After the game's own issue of a 3-vertex, one-instance draw that
   `tonemap_admit.h` admits by structure (one RT, no depth, blend off, b2 >= 256, exposure
   and a 3D LUT bound) and whose PS slot 0-3 reads an eye's HDR (the G-buffer's RT3) after that
   eye's private lighting: the same draw again, with that slot bound to the private HDR and RT0
   to a private R8G8B8A8 target T (the game's output size, the game's RTV format) cleared to
   transparent black. Once an eye a frame. No hash table.
2. *The cover.* A compute pass right after it: where the private depth is set (nearer than its
   clear) and at least as near as the game's main depth (reversed Z; the main depth holds the
   cockpit shell and no longer the hull), the cover C = T's bytes with alpha 1, else 0; the
   private depth beside it. The main depth is the game's eye depth, read in place (a
   reference held from the seed to the frame's end).
3. *The composite point.* Inside `temporalInner`, right after the upscaler (DLSS, FSR or XeSS)
   wrote `dlOut` at output size and before anything reads it: the eye run's captures, the UI
   resolve, the copy into `dlSubmit`, the frame-generation synthesizer, and later the UI layer's
   door (`native_sharpen.cpp`, `uiLayerComposite`), so the HUD stays on top. The hull eye is the
   one whose tonemap wrote the submitted texture (identity), else the same index
   (`matched_by_index`). Render size against output size: each output pixel is mapped to the
   render grid by `(o + 0.5) * render / output - 0.5`, inside the eye's region of C.
4. *The AA: (c), a small TAA of the layer's own.* Not (a): DLSS has no coverage channel (the
   layer over black would fringe dark), and the NGX wrapper's three slots are the eyes' and the
   world route's, shared with FSR and XeSS. Not (b): the pass's own TAA shader is bound to the
   whole pass (its constant block, depth pair, UI and engine inputs). The new one, at output
   size: this frame's cover in a 3x3 around the pixel's unjittered position, each sample at
   q - jitter weighted exp(-2.29 d^2) (the pass's own filter); the history (R16G16B16A16F,
   premultiplied colour and coverage) carried by the head's motion alone, the pass's head delta
   and translation term at the cover's nearest depth (the pass's own carry, transcribed), fetched
   bilinearly and clamped to the 3x3's range; current weight 0.1. Then
   `eye = eye * (1 - a) + colour` (premultiplied), copied into `dlOut`. The hull is rigid with
   the cockpit, so the head's motion is its motion.
5. *Safety.* Decided at the frame boundary before the frame's first draw. The main pass
   discards the own ship only with: the VR profile and not on foot; frame generation off
   (`fix.frame_generation` and `fix.frame_generation_shadow`; said once); an own mask read
   back and of at most 400 records (else a line with the count, once a window); and no miss
   in the last 16 frames. A miss: an eye that seeded and drew the own ship but was not lit,
   tonemapped or composited this frame. On a fallback frame the layer, tonemap, cover and TAA
   still run and the composite is made but not copied, so the history stays warm and a miss is
   seen without the ship being lost. A failed seed is counted but holds nothing (that eye's
   ship stayed in the main pass).
6. *Cost.* D3D11 timestamps around the tonemap re-issue with its cover, and around the TAA
   with its copy; read back a few frames later; the mean a call in the 30 s line. The composite
   also lies inside the GPU census's temporal whole.

**Known differences, not re-issued in this step:** atmosphere and fog over the hull layer;
bloom from the hull's lights; the 6 own forward draws (they still draw into the main pass, over
the terrain, and the hull covers them). The canopy glass stays over the hull only where the UI
layer takes it (the legacy glass route into the weapons layer, composited at the door after
this point); a glass drawn in the main pass lies under the composited hull.

**The log lines** (separate only):

```
hull layer: composite: separate_frames=N composited_eyes=N ready_eyes=N
  fallback=framegen:N,outside:N,mask_pending:N,mask_large:N,streak:N
  failed=seed:N,lit:N,tone:N,composite:N declined=stale:N,size:N,gpu:N toned=N
  tone_declined=N history_resets=N matched_by_index=N own_max=N aa=hull_taa
  gpu_ms=tone:X,composite:X (...)                                    every 30 s
hull layer: separate mode stands aside while frame generation is on   once
hull layer: separate mode falls back: the own mask holds N records    once a window
hull layer: tonemap re-issue declined: <why>                          once
hull layer: composite declined for eye N: <why>                       once a reason
hull layer: dump <stamp> eye N: HullTone ..., HullComposite ... (the composite applied
  | made, not applied)                                                per eye, per dump
```

The dump adds `eye_<stamp>_HullTone.bin` (C, render size) and `_HullComposite.bin` (the
composited eye, output size, after the composite and before the UI). `--health`: PARTIAL in
separate mode with no composite line, or with no eye-frame composited.

**What the pilot should see:** the own ship as before, its edges clean in a turn: the terrain
behind the wing edge drawn and upscaled like the rest, the hull on top with its own TAA. The
HUD and the cockpit in front of the hull.

**What to fly:** FG off (`fix.frame_generation = off`, `fix.frame_generation_shadow = off`),
`fix.temporal_aa` on, `diag.hull_layer = separate`; a turn over terrain with the wing edge
in view; one eye dump in the turn. Then `edvr_log.py --health` (`hull layer` OK,
`composited_eyes` near two a frame, `failed` and `streak` near 0).

- **Confirmed** if: no wing-edge smear in turns; the ship looks the same as the game's.
- **Refuted** if: the ship missing, flickering, sliding against the cockpit, or with jaggy edges.

**Not verified before the flight:** that the treat runs before the frame boundary (else every
composite is `stale` and the mode falls back for good); that the submitted texture is the
tonemap's output (else `matched_by_index`); that the HDR the tonemap reads is the G-buffer's RT3
(the binding map says so); that the atmosphere's depth writes never land nearer than the hull;
the TAA's head-motion sign conventions on a real head (transcribed, not run); the alpha the
game's tonemap writes (unused: coverage comes from depth).

## Step 2 build (2026-11-08, compile-only, not flown)

The own ship out of the game's main G-buffer, so the terrain behind it is drawn. New value
`diag.hull_layer = separate` (TEMPORARY): everything `dump` does, plus the items below.
**It hides the own ship**: nothing puts it back until Step 3 (the composite after DLSS).
`off` and `dump` are unchanged. Source: `hull_layer.cpp` (hull_layer.h says what it does),
`dxbc_engine_velocity.h`, `engine_velocity.cpp` (`engineVelocityOwnDiscardPs`), the four
draw hooks in `vscreen.cpp`.

1. *The seed before the seed draw.* In `separate` the seed (instance window, pool copy, mask
   dispatch, private clears) runs in the pre-draw path of the seed draw, after engine
   motion's substitution, so the seed draw itself is discarded too. `dump` still seeds
   after the draw.
2. *The own discard.* A variant of engine motion's patched pixel shader: the same patch,
   plus, ahead of the original code, `and` the slot, `ld_raw` the own mask at **PS t127**
   (one uint a record; past its end reads 0, not own), `discard_nz`. The patch refuses a
   shader that names t127 or forces the early depth test. t127: the hull PS fixtures declare
   t0-t9 (66DE/BBDE4E71) and t0-t5 (AACF/CF534B32); EDHM reads t120, 3Dmigoto t120/t125;
   EDVR binds nothing above t110. From the seed to the end of the eye's G-buffer, every
   hull-family draw and every hull decal (BBE5/DB3E, the guarded variant in VR eye passes:
   these are patched pool draws) that engine motion substituted is issued with the variant
   and the eye's mask at t127. The patched shader and the game's t127 go back right after
   the game's issue, before the private re-issue (which draws normally). No seed, no slot
   target at RT6, or no variant: the draw is the game's, the ship stays.
3. *Decals into the private layer* (separate only): own decals that export the slot are
   re-issued like the hull and filtered by the same pass.
4. *The own forward draws* (VS `88DCF116`, `025B4B9F`, `0B5981F2`, `F464E27C`, `6DB587D2`,
   `2B3F53DD`) are **not** engine-motion families (not in `engine_velocity_families.h`), so
   they have no slot and no variant. They still draw into the main HDR with the depth test;
   with the hull gone from the main depth they draw over the terrain. **Open item** for
   Step 3: they are not emitted twice and no CPU ownership test is used.
5. *The dump* (separate only) adds `eye_<stamp>_GameGbuf0.bin` (the game's G-buffer RT0) and
   `_GameDepth.bin` (the eye depth), copied when the eye's G-buffer ends.
   `hull_layer_compare.py` reads a dump with `_GameDepth` as a separate-mode dump: on the
   private hull pixels it reports the share where the main depth is farther (filled in),
   at the clear value (holes), equal (the hull still there) and nearer (cockpit in front).
   TAKEN OUT (exit 0) at most 5% equal, else NOT TAKEN OUT (3).

**Proof (engine_velocity_test, WARP):** synthetic four strips (slots 3, 7, 1000 past the
mask, 9; 7 and 9 own): every non-own pixel's RT0-3, depth and MRT6 bit-identical to the
patched shader, every own pixel keeps all clears, plain and guarded; with no record own the
frame is the patched one bit for bit. The two real hull PS fixtures (66DE + BBDE4E71, AACF +
CF534B32) through the corpus harness, two data sets: 49,152 texels each, 0 differ, 0
written when own. `--corpus` runs the same on every hull-family pair of a dump. The
lifecycle rig checks the variant of a live substitute (plain and guarded) is made once.

**The log lines** (separate only):

```
hull layer: main pass: own_draws=N decal_draws=N no_variant_draws=N fallback_frames=N
  decals_reissued=N decal_no_slot=N (...)                            every 30 s
hull layer: separate mode hides the own ship: ...                    at the key change
hull layer: own-discard variant made for vs_66DE... + ps_... (plain patch; ...)
hull layer: own-discard variant declined for ...: <why>; ...         once a shader
hull layer: dump <stamp> eye N: GameGbuf0 written, GameDepth written (...; own_draws=N
  decal_draws=N decals_reissued=N)                                   per eye, per dump
```

`fallback_frames` counts eye-frames whose seed failed (the ship stayed in the main pass);
`no_variant_draws` counts window draws that could not take the variant. `--health` is
PARTIAL in separate mode when no main-pass line comes or `own_draws` is 0.

**Flight plan:** with `fix.temporal_aa` on and `diag.hull_layer = separate`, fly low over
terrain; expect the own ship gone and terrain behind it, the six forward draws (laser,
lights) floating. Then `edvr_log.py --health` ("hull layer" OK, `own_draws` > 0,
`fallback_frames` near 0) and `hull_layer_compare.py <eyes>\eye_<stamp>` (and `--eye 1`):
TAKEN OUT, with the filled share high over terrain.

**Not verified before the flight:** that hull-family draws of the own ship after the depth
copy (none in the ledgers) or own decals before the seed do not occur; that every own pool
draw in the window reads the same pool view the seed copied (the mask is indexed by slot);
that the three keyed PS families not on this machine (61AE, DE54, DB3E) take the variant
(the patch declines cleanly and says so); the R10G10B10A2 read of GameGbuf0 (f23 assumed
DXGI 23); a neighbour within 120 m is discarded as own too (untested, as in Step 1).

## Step 1 v2 flight (2026-11-08 20:30, build cfa4e5f5): CONFIRMED

Log `edvr_gfx_20261108_203001.log`, build verified. Three auto dumps:
203155534001, 203204345002 (sunlit) and 203212174003 (the ship in
shadow).

- **The layer ran:** `state=applied`, 39-44 hull draws an eye, 4 lighting
  draws an eye re-issued, `no_slot` on 16 of 1,878 eye-frames. The seed
  draw has a single instance (`seed=1/1`).
- **Verdict, `hull_layer_compare.py`, eye 0:** CONFIRMED on all three.
  On the pixels where the game shows the hull, the private/game luma
  median ratio is 1.000. 91.5-98.9% are within 5%, and 85-98% are
  bit-exact. p95 is 1.013-1.090, the AO and decal residue.
  - The first read said REFUTED or INCONCLUSIVE. The tool compared every
    private hull pixel, but in the game's image the cockpit covers 73% of
    them. Seen side by side (preview in the shared notes), the private
    layer is the whole nose without the cockpit. The tool now keeps the
    pixels where the eye run's scene depth `Z` equals the private depth,
    with a self-test case for it.
- **The own mask holds 136-271 records, not 54**, and up to thousands in
  the 30 s lines. Replayed on three ledgers (`mask_check.py`), every
  record within 120 m of the seed comes from the own ship. That is the 54
  hull-family records, the 64 the own ship draws with other families
  (cockpit, weapons) and records not drawn that frame. No neighbour's
  hull records are in it. The filter only reads hull pixels, so the count
  is cosmetic. A neighbour within 120 m (docking, tight formation) would
  join the mask; that case is untested.
- **Not exercised:** the neighbour filter (`filtered_px=0`: no ship near
  in these dumps).

## Step 1 v2 flight (2026-11-08 20:19, build 472aaccf): no result, a shader keyword

Log `edvr_gfx_20261108_201922.log`, build verified. The layer never ran:
`hull_layer_count_cs` failed to compile at run time (`error X3000: syntax
error: unexpected token 'Pass'`). `Pass` is an HLSL keyword, and it was the
name of the cbuffer shared by the count and filter shaders. Every window
logged `state=no-shader`. Five dumps (202246398001 to 202450601005) carry
only GameHdrLit. A few eye-frames also logged `no-slot-target` (2-10 a
window); read that again on the next flight.

The fix renames the cbuffer, and adds `tools\hlsl_embedded_check.py`, gated
in `build.bat`. It compiles every `// hlsl-check:` shader of a source with
the system `d3dcompiler_47.dll`, so a run-time shader cannot fail first in a
flight. It reproduces this error on the 472aaccf source.

## Step 1 v2 build (2026-11-08, compile-only, not flown)

The fixes the v1 flight asked for (section "Step 1 flight"), still in the
eye dump only: nothing the game draws or presents changes. Source:
`src\d3d11\hull_layer.cpp` (hull_layer.h says what it does). Same key,
`diag.hull_layer = off | dump` (TEMPORARY, default off). It now also needs
engine motion (part of `fix.temporal_aa`, no key of its own): its patched
pixel shaders write the record slot at MRT6, and the filter reads it.

**What changed:**

1. *The seed and the own mask.* Per eye-frame, the first hull-family
   G-buffer draw after the cockpit shell is the seed. Its instance window
   (the stride-8 instance stream, record index first) and the pool (VS t33,
   336-byte records, position at bytes 16-27) are copied. One compute
   dispatch then marks every pool record within 120 m of the seed records'
   centroid in an own mask (one uint a record, sized from the pool's view).
   The count is read back 2+ frames later with `DO_NOT_WAIT`, never a stall.
   If the seed draw has no R32G32 slot target at RT6, the eye-frame does
   nothing (`no_slot`, said once as `no-slot-target`).
2. *Every hull-family draw, filtered per pixel.* From the seed to the eye's
   depth copy (or the next eye's shell), every hull-family G-buffer draw is
   issued again into private RT0-3, a private depth and a private slot
   target (cleared to -1, no slot). A full-screen pass then sets RT0-3 to 0
   and the depth to the clear value (SV_Depth, func ALWAYS) wherever the
   slot names a record outside the mask. A count pass counts those pixels
   (`filtered_px`). Neighbours in front of the own ship leave holes there,
   which is accepted. The run, gap and terrain rule and its t33 test are
   gone. Decals stay out.
3. *All the lighting.* Every draw with VS `7E38A6AA`, `8289669D`,
   `963B52C7`, `24DE25E4`, `B121A79E` or `0357BBB2` is re-issued, with any
   PS. Every target and PS resource that has a private copy is swapped; the
   shadow map and the light list stay the game's. Another VS that writes the
   eye's HDR between the shadow mask and the sky is logged as an unknown
   lighting pair. The depth copy and the shadow mask are unchanged.
4. *Fixes.* A private copy whose game texture was unseen for 120 frames is
   dropped (the cap is now 32). This is the v1 dump that captured nothing.
5. *Auto dumps.* The layer arms the eye dump itself (the hotkey's own call,
   `temporalPassArmEyeDump`) once it has seeded over terrain on 600 frames in
   a row with an own mask of 20-200 records. At most 3 a session, 60 s
   apart. The hotkey still works.

**The files:** `eye_<stamp>_HullHdr.bin` (the private HDR after the eye's
last private lighting draw), `eye_<stamp>_GameHdrLit.bin` (renamed from
GameHdrSun: the game's HDR after its last lighting draw, before the sky) and
`eye_<stamp>_HullDepth.bin` (the private depth after the filter). Eye 1's
names end in `_e1`.

**The log lines** (graphics log, untagged; `edvr_log.py --health` reads them
as "hull layer"):

```
hull layer: state=applied frames=2700 eye_frames=5400 seeds=5400 no_seed=0
  no_slot=0 refused=0 own=54/54/55 readbacks=5400 filtered_px=0/1200/9000
  hull_draws=70/84/92 reissued=453600 no_slot_draws=0 clears=5400
  filters=5400 subst=copy:10800,mask:32400,light:59400 light_draws=11/11/11
  no_counterpart=copy:0,mask:0,light:0 unknown_light_pairs=0
  ended=copy:5400,shell:0,light:0,frame:0 terrain_frames=2700 auto_dumps=0
                                                          every 30 s (one line)
hull layer: auto dump armed (reason: own mask 54 records, seeded over terrain
  for 600 frames in a row; 1 of 3 this session)
hull layer: dump <stamp> eye 0 frame N: HullHdr WxH format 26 written,
  GameHdrLit written, HullDepth written; own=54 filtered_px=3000 seed=3/3
  hull_draws=84 light_draws=11                              per eye, per dump
hull layer: unknown lighting pair vs <16 hex> ps <16 hex> writes the eye's
  HDR between the shadow mask and the sky (...)             once a pair
```

`state` is `applied`, `partial` (seeded, no lighting substituted),
`mask-pending` (seeded, no readback yet), `no-shader`, `no-slot-target`,
`seed-refused`, `no-seed` (the shell, no hull draw after it), `no-shell`
(hull draws, no shell) or `no-eye-pass`. `own` and `filtered_px` are
min/median/max over the window's readbacks; `seed=` in the dump line is the
seed records read / the seed's instances.

**Flight plan:** with the key on and `fix.temporal_aa` on, fly low over
terrain for 10 s or more at a time, and do not press the dump key. The
layer arms up to three dumps. Then:

1. `python tools\edvr_log.py --target steam --expect-build <commit>
   --health`: "hull layer" OK, `own` near 54 for the Anaconda, three `auto
   dump armed` lines.
2. For each dump: `python tools\hull_layer_compare.py
   <edvr_logs>\eyes\eye_<stamp>` (and `--eye 1`). It reads the own count
   from the dump's log line.

- **Confirms Step 1:** HullHdr equals GameHdrLit on the hull pixels (median
  luma ratio 0.95-1.35, at least half within 5%), with no better whole-pixel
  shift, and `own` is 54 (Anaconda). AO and decals may differ.
- **Refutes it:** the layer is black where the game is lit (the lighting
  bindings), offset, or the private depth is empty. It is also refuted if
  `own` is far from the ship's record count (the seed or the radius is
  wrong), or if the neighbour pixels survive the filter in HullHdr.
- **Inconclusive, not refuted:** GameHdrLit black on most hull pixels (the
  comparison point is wrong again), or the luma close but not within the
  limits.

**Not verified before the flight:**

- the instance stream binding (the stride-8 buffer in vertex slots 0-3,
  offset plus StartInstanceLocation x 8);
- that the pool view starts at element 0, and that pool positions are
  camera-near enough for a float centroid. The first seed logs the view
  once.
- the slot target's format (R32G32_FLOAT) and its clear (-1, no slot);
- the four lighting VS hashes from the `mt=` census, `B121A79E457669E8`
  among them;
- that `temporalPassArmEyeDump` may be called from the frame boundary;
- that SV_Depth with func ALWAYS writes the D32S8 private depth as
  expected. The filter leaves stencil as the neighbour wrote it.
- The shaders compile at the first seed (d3dcompiler_47.dll), a one-time
  hitch.

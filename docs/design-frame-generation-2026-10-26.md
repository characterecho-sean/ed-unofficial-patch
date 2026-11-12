# Frame generation below headset rate (an Elite-specific OFXR-Bridge) — design, 2026-10-26

## Status

- **State (2026-11-08): Phase 2.4 built, not flown** (fork `framegen-phase1`):
  `auto` and the performance gate (2026-11-08 gate entry); Lock (2.3) flies,
  S in every slot; the remaining smear is the base frame (S quality arc). Design from OFXR-Bridge `d7fcca72`,
  EDVR `6b8ef374`. No issue yet.
- **Recommendation:** take OFXR-Bridge's frame loop, not its synthesizer. Elite
  renders at half the headset rate and EDVR's runtime presents every display
  period. The frame in between is **extrapolated** from Elite's newest frame,
  with the depth and motion vectors EDVR makes for DLSS, the game camera and
  the head pose predicted for that slot: no optical flow, no interpolation, no
  added latency. It is made on the game device after the temporal pass and
  presented one period later (## Recommendation, ## Design).
- **Why not a straight port:** OFXR-Bridge is GPL-3.0 and EDVR is MIT, so no
  code is copied. Its colour-only optical flow is "depth-unaware" for
  translation and cannot tell the cockpit from the world, and interpolating
  holds every real frame back at least one period (## Why not a straight port).
- **When it runs (`auto`):** only while a measured gate holds. Elite is below
  headset rate, its frame fits in two periods with room for the synthesis, a
  shadow score says the prediction is clean, and there is no cut. As built,
  v1 admits a ship (supercruise too) or the SRV, not on foot, and has no
  score term (## The gate; 2026-11-08 gate entry).
- **Space:** use geometry, not flow. Stars and sky turn with rotation only. The
  cockpit moves with the head only. The world moves with the head and the
  extrapolated ship, movers with EDVR's engine-record vectors, and the UI on
  its own path. No two frames are blended, so nothing doubles. A Fill mode
  extrapolates only the slots Elite misses (## Space without artifacts).
- **Hypotheses (none tested):**
  - H1: surface frame drops fit half rate with room. Elite's work is under
    0.85 of two periods, less the synthesis.
  - H2: on surfaces, the extrapolation residual is near the DLSS
    frame-to-frame floor, except at disocclusions.
  - H3: in space, the far and cockpit classes predict to within that floor.
  - H4: interpolation (OFXR's model, with EDVR's motion) beats extrapolation
    by enough to pay for its latency. Expected false.
  - H5: synthesis on the game device costs at most 1.5 ms a pair at the
    flown sizes.
  - H6: the head pose predicted for the slot misses by little after the
    compositor's rotation correction. PiOpenXR may predict only about 20 ms
    ahead (VectorXR), so a velocity fallback is designed.
  - H7: the runtime stays at full rate while EDVR submits every period.
    VDXR's sticky half rate is the risk.
  - H8: one eye rebuilt from the other at the same moment (AFW's stereo warp)
    leaves little that neither eye saw, and no HUD doubling off the UI path.
  - H9: settlements are GPU-limited, and one eye is a large share of the GPU
    frame. Only then would alternate-eye rendering pay.
- **Phase 0 (no EDVR code):** fly stock OFXR-Bridge and VectorXR (colour-only
  routes) over EDVR at a bright settlement and in space, with each runtime's
  own smoothing off and then on. This is the visual reference to beat.
- **Next:** fly `auto` in a ship below headset rate and at full rate: the
  `frame generation gate:` line is H1 (budget, margin) and says each
  second why Lock is or is not allowed (2026-11-08 gate and cost entries).
- **Temporary keys:** `fix.frame_generation_shadow` (off | on, hidden);
  `fix.frame_generation`'s `on` (forces Lock for A/B; `auto` is to stay).
- **Declined in design (re-open on new evidence):** ## Declined, including AFW
  as a replacement and flat-profile frame generation (2026-10-27).
- **Open:** ## Open questions. The own hull's layer in S: FLOWN 2026-11-11
  (hull in every S; FG adds 0.8-1.35 ms of EDVR work a real frame). The
  frame recomputed from the logs: entry 2026-11-12.

## The ask

> I want you to build a design doc that would port a bespoke and Elite
> optimized version of OFXR-Bridge. The goal here would be to have it only run
> in situations where it performs well, bright scenery where Elite's frame rate
> drops below the target for the headset. In space this rarely happens. [...]
> If you have ideas around how to make it perform well in space without
> artifacts, that would be welcome as well.

(Sean, 2026-10-26.)

## OFXR-Bridge, as read

What was read: the source at
[`d7fcca72`](https://github.com/tig3rmast3r/OFXR-Bridge/tree/d7fcca72d569ec364281ab63ae29378bbfaf1251)
(v0.3.1, 2026-10-26) and at tag `0.2.1`
([`dad56aca`](https://github.com/tig3rmast3r/OFXR-Bridge/tree/dad56acafc6e1dde219940427738b926cf2ea555)),
the optical-flow-only core. Nothing was built or run. Paths below are in that
repository at `d7fcca72` unless they say 0.2.1.

| | |
|---|---|
| What it is | An OpenXR API layer that "inserts generated frames between rendered frames" (`README.md:3`). It is registered as an implicit layer under `SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit` (`include/xrfg/implicit_layer.hpp:40-41`), so it loads into every OpenXR app unless excluded. |
| Frame loop | The app's `xrWaitFrame` returns a doubled (or tripled) period (`src/layer/openxr_layer.cpp:2712-2718`), and its `xrBeginFrame` is answered locally. A presenter thread makes the real wait, begin and end once per display period and alternates the synthetic frame S with the real frame B (0.2.1 `src/layer/openxr_layer.cpp:2073-2090`, `:3035-3330`). |
| Synthesis | It interpolates between the previous real frame A and B, and shows S before B (`include/xrfg/three_slot_buffer.hpp:21-33`). The synthesizer takes an extrapolation fraction (`include/xrfg/d3d12_frame_synthesizer.hpp:183-188`), but the shipped scheduler interpolates, and an upstream extrapolation route was not imported (`docs/V445_UPSTREAM_INTEGRATION.md:36`). |
| Motion | By default, colour-only optical flow: NVIDIA's optical-flow hardware, or FidelityFX Optical Flow, on luma at 50% scale. A pack shader first removes the image motion from the OpenXR rotation and FOV change, so the flow sees only the residual (`src/d3d12/frame_synthesis.hlsl:482-543`). A midpoint solve then picks flow or camera-only motion per pixel (`:877-1050`). "Translation remains depth-unaware" (`d3d12_frame_synthesizer.hpp:203-205`). Optional routes: game vectors taken by hooking NGX DLSS Evaluate, a hybrid of the two, FSR 3.1 FG, and DLSS-G. |
| Graphics | Native D3D12. A D3D11 app is given a D3D12 session on the layer's own device and renders into shared textures (`openxr_layer.cpp:5705-5760`). |
| Head pose | By default S carries B's pose, which leaves black strips on the trailing edge during head turns (`openxr_layer.cpp:11138-11152`). An interpolated pose is opt-in. The newest head pose never reaches the synthesis. |
| Latency | B is held behind S. "Prefer FPS over latency", on by default, holds frames one more refresh: "about 11 ms at 90 Hz" (`docs/releases/0.2.4.md`). |
| When it runs | Always, once armed. It fails open when a submission has no projection layer, quarantines for 1 s after a layout change, and has a hotkey pause. By its own guidance it helps a game that "can't hold full rate", and the game needs headroom above half (`docs/TROUBLESHOOTING.md:66-72`). Vendor smoothing "fights OFXR" (`:69`). |
| HUD | On the default route, a HUD baked into the projection layer is flowed like the scene. |
| Known artifacts | "moving objects, disocclusions and head rotation" (0.2.1 `README.md:36`). FidelityFX "will produce artifacts during headset rotation in dark areas, this is known and cannot be avoided" (0.2.1 `README.md:79`). |
| GPU | The NVIDIA routes need Turing or newer with optical-flow hardware (not TU117, not Pascal). FidelityFX serves other GPUs (`README.md:32-34`). No millisecond cost is published. |
| Licence | GPL-3.0 since v0.3.0, commit `3085994` of 2026-10-25 (`README.md:279`). Tags up to 0.2.1 are LGPL-3.0-or-later. |
| Size | About 85k lines of C++20 and HLSL outside vendored code; `openxr_layer.cpp` alone is 17,225 lines. 148 commits since 2026-09-05; pre-releases only. |

Two of its lessons carry over regardless of the motion source:

- **A fail-open path must not keep handing a doubled period.** With no
  projection layer (a menu, a load) the app was paced once per presenter frame
  but told a doubled period, and the predicted time drifted a second per
  second. One session reached 637 s of lead and never generated again
  (`openxr_layer.cpp:6427-6437`).
- **Vendor smoothing must be off while the bridge runs**: SteamVR Motion
  Smoothing, Virtual Desktop SSW, Pimax Smart Smoothing
  (`docs/TROUBLESHOOTING.md:69`).

## What EDVR has, and what it lacks

- **The frame loop is EDVR's.** Elite's `WaitGetPoses` and `Submit`
  (`src/openxr/openvr_compositor.cpp`) reach `NativeRuntimeHost`. An owner
  thread (`owner_service.h`) makes every OpenXR call through `FrameBoundary`
  and `SessionState`. `FramePacer` (`frame_pacer.h`) can own `xrWaitFrame` on
  its own thread and hand Elite a synthesized predicted time
  (`frame_boundary.h`, `synthesizedTime`). This is the deferred ("turbo")
  pacing that `fix.weapon_stability` uses on foot
  (`src/d3d11/native_frame.cpp:469`).
- **A previous pair can already be presented again.** For transition-flash
  suppression the d3d11 half can withhold a frame, and the runtime then submits
  the kept pair with its old views and head pose (`native_runtime_host.h`,
  `sceneFinished`/`sceneLayerAvailable`, counters `replayedPairs`,
  `withheldPairs`). This is the plumbing a second, made pair would ride on.
- **One projection layer, colour only, on EDVR's own XR device.** Only the
  D3D11, time-conversion, refresh-rate, visibility-mask and performance
  extensions are enabled (`native_runtime_host.h:4054-4059`). No depth layer is
  submitted: it was set aside on 2026-09-24
  (`docs/openxr-performance-review-2026-09-14.md:33`). Each eye crosses from
  the game device by `SharedTextureTransfer`, and the two-device split is
  deliberate (the same review, "Keep the two-device architecture").
- **Depth and motion exist only on the game device, and only while
  `fix.temporal_aa` is not `off`.** The temporal pass holds, per eye, the
  colour, reversed-Z depth (0.025 to 50000 m), last frame's depth, the reactive
  mask, and R16G16 motion in render pixels from current to previous
  (`src/d3d11/temporal_pass.cpp:184-215`). EDVR makes that motion itself. The
  head delta covers pixels nearer than the 10 m ship split, the camera's rows
  cover pixels beyond it (`src/common/temporal_mode.h:41-58`), and terms are
  added for engine-record rigid movers, terrain, smoke, UI and supercruise
  planet patches. It is validated:
  `docs/kinematic-motion-injection-2026-09-19.md` (Status: certified
  engine-record deltas; raw-image warps agree with the engine motion to a quiet
  median of 0.05 input px). The open gaps it lists (walkers, some ships, the
  Coriolis brace's coverage) are the movers class below.
- **Context:** `SeatContext` (`where=aboard|on_foot|unknown`) and the
  Status.json flags in `src/d3d11/journal_watch.h` (supercruise, docked, on
  foot, SRV). Loading has `loading_state.h`. A planet patch's colour pass is
  recognised by its vertex shader (`src/d3d11/celestial_motion.h:29`). There is
  **no planetary-surface classifier**.
- **Timing:** late periods (`FrameBoundary::lateFrames`), the frame-cycle
  windows (`frame_cycle_stats.h`), the slow-regime observer (`slow_regime.h`,
  which names who held the time), and the display rate from
  `XR_FB_display_refresh_rate`. The app's GPU span exists but sits behind
  `advanced.app_gpu_timing`, which is off by default (`edvr.ini:1610`).
- **The deficit is where Sean says it is.** Stock fpsVR at one settlement:
  about 2 ms CPU in space, 9.5 ms in the settlement cockpit
  (`docs/engine-render-performance-2026-09-19.md`, 2026-09-21). A Pimax 8KX at
  4100x3212 per eye: producer 11.9-13.4 ms at the settlement against 0.5-3.3 ms
  away (same doc, 2026-09-22 addendum 2).
- **Elite already runs at half rate there; the runtime fills the gap.** At that
  settlement SteamVR's predicted period read 22.222 ms (45 Hz). Over VDXR at 72
  Hz, a GPU-bound ship at about 41 fps put the period at 27.78 ms
  (`docs/headset-lock-vdxr-2026-10-02.md`). The question is therefore not
  whether to halve, but **what fills the other slot**.
- **Earlier decisions:** a frame-rate lock was declined because "SteamVR's
  motion smoothing already halves adaptively" (`docs/performance.md:148`).
  Turbo pacing is "not a performance lever" (`:147`). Optical flow as a motion
  source was declined in favour of instrument use
  (`docs/per-object-motion.md:141`). This design does not reverse the first
  decision. It replaces what fills the halved slot.

## What fills the missing frame today

None of these was flown for this doc. URLs marked (search) were found by search
and not opened, so re-check them before quoting.

- **Meta PC ASW** takes motion from historic frames, not from app vectors
  ([Meta,
  search](https://developers.meta.com/vr/documentation/native/pc/asynchronous-spacewarp/)).
  Depth enabled ASW 2.0 under LibOVR
  ([search](https://developers.meta.com/horizon/blog/developer-guide-to-asw-20/)),
  and EDVR submits none.
- **SteamVR Motion Smoothing** extrapolates from the last two frames, and Valve
  contrasts it with TV interpolation, which "adds latency". It is for headsets
  on SteamVR's own compositor ([Valve,
  search](https://steamcommunity.com/games/250820/announcements/detail/1705071932992003492)).
- **Virtual Desktop SSW** runs on the headset
  ([search](https://uploadvr.com/virtual-desktop-synchronous-spacewarp/)).
  VDXR's open source shows no app-visible control for it
  ([mbucchia/VirtualDesktop-OpenXR](https://github.com/mbucchia/VirtualDesktop-OpenXR)).
- **Pimax Smart Smoothing** generates intermediate frames when the PC cannot
  hold the rate ([Pimax,
  search](https://pimax.com/blogs/crystal-light-tutorials/pimax-play-tips-and-features-overview)).
  How it triggers and whether it uses depth was not found.
- **`XR_EXT_frame_synthesis`** is the cross-vendor version of what this design
  builds. The app hands the runtime motion vectors and depth for each view, and
  the runtime "may use this information to extrapolate the rendered frame into
  a future frame"
  ([spec](https://github.com/KhronosGroup/OpenXR-Docs/blob/main/specification/sources/chapters/extensions/ext/ext_frame_synthesis.adoc)).
  In the Khronos runtime inventory at
  [`fa71884c`](https://github.com/KhronosGroup/OpenXR-Inventory/tree/fa71884c0e74e6a447760eb1524a078d6c31960f/runtimes),
  it is advertised by Quest standalone, Android XR, Pico and SteamVR on Steam
  Frame (Linux). It is not advertised by `meta_pc`, `meta_pc_dev_mode` or
  `valve_steamvr_windows`. **No Windows PC runtime EDVR flies offers it
  today.**

Every one of these sees the cockpit as still and the universe as moving. Each
must estimate the universe's motion from pixels, and stars on black give a
motion estimator almost nothing to track. EDVR knows that motion exactly, from
the game's camera rows and the head pose it gave the game. That knowledge is
the case for building this at all.

## Why not a straight port

1. **Licence.** OFXR-Bridge is GPL-3.0 (LGPL-3.0-or-later up to 0.2.1), and
   EDVR is MIT. Copying either would relicense EDVR, so this is a
   reimplementation of ideas, with no code or shaders copied.
2. **Architecture.** EDVR is already the OpenXR application and owns the frame
   loop. A layer between EDVR and the runtime would take over EDVR's pacing
   (deferred pacing, withhold and replay, the slow-regime observer). It would
   see only the final colour, and it would move EDVR's XR device onto a D3D12
   session.
3. **Motion source.** Colour-only flow is depth-unaware for translation, which
   matters for low flight, the SRV and on foot. It is weakest on dark, sparse
   content, which is space. It is NVIDIA hardware or D3D12 only: FidelityFX has
   a DX12 backend and no DX11 path ([FidelityFX SDK
   v2.3.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK),
   `Kits/FidelityFX/backend/`). EDVR's motion is D3D11-native, runs on any GPU,
   and is already validated against the engine.
4. **Latency.** Interpolation cannot show S until B exists, so B waits at least
   one period, plus OFXR's default hold of one more.
5. **Always on** against a game whose deficit lives in a few contexts.

**Kept from OFXR-Bridge:**
- the split between the app at half rate and a presenter at display rate;
- the virtual-period ceiling;
- failing open with no projection layer;
- the quarantine after a layout change;
- removing known camera motion first and solving only the rest;
- choosing per pixel between camera motion and other motion;
- the instruction to turn vendor smoothing off.

## Recommendation

Build an EDVR-owned frame synthesizer that **extrapolates** the in-between
frame from Elite's newest frame, using:
- EDVR's depth and motion vectors,
- the ship's motion taken from the game camera, and
- the head pose predicted for the synthetic frame's own display time.

Make the frame on the game device, inside the temporal pass, right after the
real frame is published. That is where the data already is, and it queues ahead
of Elite's next frame, so no GPU priority is needed. The XR owner thread
presents it in the next display period, using the pacing EDVR already has for
deferred waits and replayed pairs.

Run it only behind a measured gate, and prove it first in a measure-only phase
that scores the predictor against real frames. That phase costs one flight and
puts nothing new in front of Sean's eyes. Leave optical flow and interpolation
out of v1: re-open either only if Phase 1 shows its classes dominate the
residual.

## Design

### Motion model

Each pixel of Elite's frame B, per eye, gets a class. The class decides how the
pixel moves to the synthetic frame S, which is shown one period later at time
T_S.

| Class | Pixels | Moves with | Expected exactness |
|---|---|---|---|
| far | the far plane, plus what the motion shader already treats as far or celestial: sky, stars, distant bodies | head and ship **rotation** only | exact |
| cockpit | aboard, nearer than `kTemporalShipMetres` (10 m) | the head only (B's eye to S's eye) | exact, up to disocclusion |
| world | farther than the split, static | the head and the ship rig, extrapolated | as good as the rig's extrapolation |
| mover | engine-record motion differs from camera-only motion | world, plus the object's own residual motion carried forward | linear over one period |
| reactive | the reactive mask: smoke, dust, lasers, explosions, shields | treated as world static, and scored on its own | not modelled |
| UI | the `ui_quality` layer, or UI depth in the picture (holo panels, flight HUD, target sprite) | the head only | exact for ship-fixed UI. World-anchored brackets lag one period |

**The ship rig** is the game camera with the eye removed: the view rows for B
times the inverse of the eye EDVR gave the game for B. The rig for S is B's rig
carried forward by the rig's linear and angular velocity over the display times
of A and B. Elite's flight model is inertial, so a constant-velocity step over
one period should be close. The error shows under boost, collisions and
touchdown, which is failure class 7 below. On foot there is no rig: the split
is 1 mm (`kTemporalOnFootSplitMetres`), and the world takes the camera's
motion.

This is the algebra the motion shader already runs for DLSS (the head delta
inside the split, the camera's rows outside it), run forward in time instead of
backward. **One owner:** the class decision and the transforms come from the
motion shader's code, not a second copy of it.

### The synthesis pass

1. **Splat (render size).** First dilate B's depth (the nearest depth over a
   1x3 or 3x3 window), so an object's edge pixels move with the object.
   OFXR-Bridge adds the same pass to its AFW route to reduce "ghosting, flicker
   and shimmering", at the cost of less precise silhouettes
   ([`README.md:98`](https://github.com/tig3rmast3r/OFXR-Bridge/blob/d7fcca72d569ec364281ab63ae29378bbfaf1251/README.md)).
   Then forward-splat B's depth into S's grid using each pixel's class
   transform. The nearest pixel wins: a UAV atomic max on reversed-Z depth,
   then a pass that records each winner's source. The result names, for every
   pixel of S, the pixel of B it came from. Where a winner's offset would read
   something nearer than its source (the edge of what stood in front of it),
   the pixel reads the source itself. Without that guard VectorXR saw "sky took
   the colour of the cockpit wall it had been next to"
   ([`framegen.hlsl:4187-4194`](https://github.com/DienerTech/vectorxr/blob/87e575fb21cde8ad9ef87763c67471b7d0b7ee0a/layer/shaders/framegen.hlsl)).
2. **Holes (render size).** A disocclusion is filled first from the other eye's
   real frame B, carried to S by the same class transforms and depth-tested.
   What the head's sideways motion uncovers behind a cockpit strut is often in
   the other eye's view. This is AFW's stereo warp (2026-10-27 entry). Only
   what neither eye saw is filled from its farther neighbour (background pull),
   never from the nearer one. The fill takes its colour a few pixels into that
   surface, not from the edge pixel: an edge pixel blends both surfaces, and
   stretched across the gap it streaks the near edge's colour (VectorXR,
   [`framegen.hlsl:4306-4320`](https://github.com/DienerTech/vectorxr/blob/87e575fb21cde8ad9ef87763c67471b7d0b7ee0a/layer/shaders/framegen.hlsl)).
3. **Resample (output size).** Read B's upscaled colour (the DLSS, FSR, XeSS or
   TAA output, before the UI layer) at the mapped positions. S's pixels pass
   through one more resample than B's, so the filter must be sharp enough that
   a star keeps its energy, or stars soften every other frame.
4. **UI (output size).** Compose the UI layer with the head-only transform.
5. **Publish.** Copy the pair to a second `SharedTextureTransfer` set with S's
   views.

There is one source per pixel. Two frames are never blended, so stars, lasers
and HUD lines cannot double. Extrapolation's worst case is a held or stretched
pixel at an edge. Interpolation's worst case is a ghost.

### Where it runs

On the game device, in the temporal pass, right after B's output is published:
- **The data is already there.** Depth, motion, the reactive mask, the camera
  rows and the UI layer all live on the game device. None of them crosses to
  the XR device.
- **Ordering does the scheduling.** S's work queues on Elite's immediate
  context ahead of Elite's next frame, so it needs no second-device GPU
  priority. That priority is murky on D3D11. `SetGPUThreadPriority` has a
  documented absolute mode, but its effect under hardware GPU scheduling was
  not found
  ([docs](https://github.com/MicrosoftDocs/sdk-api/blob/docs/sdk-api-src/content/dxgi/nf-dxgi-idxgidevice-setgputhreadpriority.md)).
  D3D12's `GLOBAL_REALTIME` queue needs a privilege
  ([docs](https://github.com/MicrosoftDocs/sdk-api/blob/docs/sdk-api-src/content/d3d12/ne-d3d12-d3d12_command_queue_priority.md)).
  The public NVAPI has no context priority
  ([NVIDIA/nvapi](https://github.com/NVIDIA/nvapi)).
- **S's pose** is the freshest prediction for T_S available at the temporal
  pass, from the path that already answers Elite's "now" head queries
  (`pose_cache.h`, `now_head_publication.h`). The runtime adds those views to
  the frame input. `EdvrNativeFrameInput` is versioned and only ever extended
  at the end (`src/common/native_frame.h`). The compositor's own correction
  takes the remaining rotation at display, and H6 measures what is left.
- **The runtime may not predict that far.** VectorXR does not ask the runtime
  where the eyes will be, because "Pimax returns about the current pose for any
  later time"
  ([`framegen_presenter.cpp:5625-5627`](https://github.com/DienerTech/vectorxr/blob/87e575fb21cde8ad9ef87763c67471b7d0b7ee0a/layer/src/framegen/framegen_presenter.cpp)),
  and PiOpenXR predicts the head about 20 ms past the moment it is asked
  ([`head_prediction.h:14-18`](https://github.com/DienerTech/vectorxr/blob/87e575fb21cde8ad9ef87763c67471b7d0b7ee0a/layer/include/depthxr/head_prediction.h)).
  T_S is about 1.5 to 2 periods past the temporal pass, 17-22 ms at 90 Hz,
  right at that limit. So the runtime's answer has a fallback, measured beside
  it in Phase 1: the eye position carried forward at its velocity between the
  last two real frames, and the rotation carried at the runtime's own angular
  velocity, from two locates inside its limit (VectorXR's method,
  `head_prediction.h:20-24`).
- **Cost:** S adds its GPU time to every real frame it is made for. In Lock
  that is against a two-period budget.

**Rejected for v1:** synthesizing on the XR device at the slot. It needs depth
and motion carried across devices, and a priority D3D11 does not cleanly offer.

### Pacing: Lock and Fill

**Lock (2:1, Phase 2).** FramePacer owns `xrWaitFrame` for every slot, and the
owner thread runs `xrBeginFrame` and `xrEndFrame` for every slot. Elite's
`WaitGetPoses` returns once per two periods, with the predicted time of the
slot its frame will be shown in.

| Slot | Shown | Made on the game device | Elite |
|---|---|---|---|
| T | B, rendered for T | S_B for T+P, after B's temporal pass | starts C, told T+2P |
| T+P | S_B | (nothing) | renders C |
| T+2P | C | S_C for T+3P | starts D, told T+4P |

- **B late:** the slot gets the existing replay path (the previous pair with
  its own views), counted as `late_real`.
- **S missing:** B is replayed with its own pose, and the compositor turns it.
  Counted as `late_s`.
- **When S's GPU work ran:** its start and finish against the release of the
  slot's frame wait, as medians and maxima, so a late S can be told from a slow
  one. VectorXR logs the same pair as `gpuStartMs` and `gpuDoneMs`
  ([`docs/usage.md:789-799`](https://github.com/DienerTech/vectorxr/blob/87e575fb21cde8ad9ef87763c67471b7d0b7ee0a/docs/usage.md)).
- **Entering or leaving Lock** happens on a slot boundary. The predicted times
  stay monotonic through the floor `FrameBoundary` already applies.
- **Fail-open:** with no projection layer (menus, loads), Elite is paced and
  told one period, never a doubled one. This is OFXR's drift lesson.
- **Deferred pacing:** Lock subsumes it. Both own `xrWaitFrame`, so while Lock
  is engaged it is the pacing and `fix.weapon_stability`'s on-foot deferral
  stands aside.
- **Latency:** each real frame is shown at the time Elite was told. Against
  today's runtime-halved stretches nothing is added. Against full rate, the
  world updates at half rate, with predictions in between.

**Fill (Phase 4, an experiment).** No lock. The presenter shows each real frame
when it arrives. When a slot's deadline passes with no new real frame, it shows
the S made from the newest one. This is for space and other contexts that
rarely miss. S must be made for every real frame while armed, so Fill arms only
when the recent miss rate is above a small threshold.

### The gate

The gate is a pure function of measured inputs, re-evaluated once a second,
with a 5 s minimum dwell each way.

**Engage Lock when all of these have held for 2 s:**
- temporal AA is on and valid this frame (depth and motion exist);
- Elite's real rate is below 0.9 of the display rate, or the runtime has
  already halved its predicted period;
- Elite's frame fits: GPU span plus S's cost, and CPU work, are each under 0.85
  of two periods;
- the context is admitted (v1: surface);
- the shadow score for the last 2 s is under its thresholds;
- there has been no cut in the last 2 s: a load, a withheld frame, the jump
  tunnel, or a `GuiFocus` change to a map or menu.

**Disengage when any of these happens:**
- Elite's work is under 0.75 of one period for 3 s, so it could hold full rate;
- the shadow score is over its thresholds for 0.5 s;
- a cut;
- motion is invalid;
- Elite misses its two-period budget on more than 5% of frames over 2 s (3:1 is
  not offered);
- the runtime halves its period while Lock is engaged, so vendor smoothing is
  fighting it.

**Notes on the inputs:**
- **Surface (new):** on foot, or in the SRV, or aboard with a planet patch's
  colour pass drawn this frame and not in supercruise.
- **The thresholds** (0.9, 0.85, 0.75, the score limits) are placeholders that
  Phase 1 sets. They ship as constants, not keys.
- **Brightness is not an input.** Bright scenes suit OFXR because flow needs
  texture and contrast. EDVR's motion does not care about either, so the gate
  measures prediction quality directly. On planet surfaces the two agree, and
  in space they need not.

### Space without artifacts

Space is where flow is worst and geometry is best:
- **Far field.** Sky, stars and distant bodies move by rotation alone, which is
  known exactly. They need no motion estimate, so dark, featureless and
  point-like content costs nothing. OFXR's FidelityFX route documents exactly
  that failure: "artifacts during headset rotation in dark areas".
- **Cockpit.** Rigid in the ship, it moves only with the head. Its only error
  is disocclusion behind struts and frames under head translation, which is a
  few pixels at one period.
- **World.** The head plus the ship, with the ship extrapolated. Fast turning
  is rotation, and rotation is exact in the far field.
- **Movers** (ships, stations) by engine-record motion. Lasers, explosions and
  particles are reactive: they are held where they were, and never doubled.
- **Stars must keep their energy.** The synthetic frame's far-field resample
  must match the real frame's sharpness, or stars pulse at half the refresh
  rate. The scorer measures it as a far-class high-pass energy ratio, synthetic
  over real, which must be about 1.
- **The HUD rides the head path,** never the world's. World-anchored brackets
  lag one period at half rate, which is what they already do at 45 fps.
- **Cuts are never synthesized across:** hyperspace, the transition-flash
  withholds and supercruise exit. Extrapolation is single-source, so a cut can
  at worst show one stale frame, never a blend of two scenes.
- **Fill mode suits space.** Space rarely misses, so the right tool there is to
  extrapolate the odd missed slot instead of letting the runtime repeat a
  frame. It adds no lock and no latency, and does nothing when it is not
  needed.
- **One question to answer from an eye dump before Phase 3:** does the canopy
  glass write depth? If it does, stars seen through it sit nearer than 10 m and
  would ride the cockpit path, frozen to the ship in a turn. The DLSS motion
  would have the same problem, so the motion shader may already handle it.

### Planets and on foot

- Static terrain and buildings are exact, except at disocclusions. Low flight
  and the SRV have the most parallax. The background pull covers one period's
  edge, after the other eye's frame has filled what it saw. A further source
  (A, reprojected) is the upgrade if the disocclusion share still dominates
  Phase 1's residual.
- Thruster dust and smoke are reactive (the motion shader's smoke term). NPCs
  are movers, and walkers are a known gap in the engine-record motion
  (`kinematic-motion-injection`, "Other open work").
- On foot, the first-person weapon and hands move with the camera. They are
  movers with certified motion, or the class's residual will show them.
- The on-foot composite route (`vr_world_route.h`) and native on-foot VR differ
  in what depth the eye image carries. Phase 1 scores both.

### Vendor smoothing, and the forward path

- While `auto` is set, each runtime's own smoothing should be off.
  `docs/troubleshooting.md` gets the per-runtime steps: SteamVR per-app Motion
  Smoothing, Virtual Desktop SSW, Pimax Smart Smoothing, and Meta ASW in the
  Oculus Debug Tool. While engaged, a halved predicted period is logged and
  disengages Lock. After a few seconds of it, the user is told which runtime
  settings to check, as VectorXR does
  ([`docs/usage.md:941-950`](https://github.com/DienerTech/vectorxr/blob/87e575fb21cde8ad9ef87763c67471b7d0b7ee0a/docs/usage.md)).
- **The synthesizer's inputs are `XR_EXT_frame_synthesis`'s inputs.** Per view,
  the extension takes a motion image, a depth image with near/far and a
  reversed flag, and an app-space delta pose. Its motion runs from previous to
  current, in Vulkan-style NDC, 3D by default. EDVR's motion runs from current
  to previous, in render pixels, so the conversion is a sign and a scale. When
  a Windows runtime advertises the extension, EDVR can submit those images and
  let the runtime synthesize, keeping its own synthesizer as the fallback.
  EDVR's seated space is the cockpit, so the delta pose is identity, and the
  ship's motion is in the vectors. A layer could consume them too: VectorXR has
  a research path for `XR_FB_space_warp` and `XR_EXT_frame_synthesis` vectors.
  It is off by default, D3D12 only, and never runs through its D3D11 bridge
  ([`openxr_layer.cpp:16296-16316`](https://github.com/DienerTech/vectorxr/blob/87e575fb21cde8ad9ef87763c67471b7d0b7ee0a/layer/src/openxr_layer.cpp)).

## Failure classes and their signatures

These are enumerated before building, so that Phase 1's one flight can separate
them. "Residual" is the shadow scorer's per-class mean and p95 of post-tonemap
luma error, against the floor.

| # | Class | Where expected | Signature | Mitigation |
|---|---|---|---|---|
| 1 | Disocclusion | cockpit struts under head translation; building edges in low flight, the SRV, on foot | residual concentrated at depth discontinuities; disocclusion share | the other eye's frame; background pull; A as a further source |
| 2 | Unmodelled movers | walkers, some ships, the Coriolis brace | mover/world residual where engine-record motion is absent | the kinematic arc's open items |
| 3 | Reactive content | lasers, explosions, dust, smoke, shields | residual on reactive-mask pixels | held; accept, or residual flow (Phase 5) |
| 4 | Transparency and UI | canopy glass, holo panels, target sprite, world-anchored brackets | UI-class residual; far colour behind near depth | head path; the canopy check |
| 5 | View-dependent shading | specular, sun glare and bloom, lens flares | broad low residual, correlated with the sun in view (`docs/sun-glare.md`) | accept |
| 6 | Resampling shimmer | stars, thin lines, text | far-class and edge high-pass energy ratio away from 1 | a sharper resample filter; depth dilation at silhouettes |
| 7 | Rig acceleration | boost, collisions, FA-off flips, touchdown, camera shake | far/world residual against the rig's angular acceleration | a higher-order step, or disengage on spikes |
| 8 | Timing | the GPU saturated, Elite over budget, vendor halving | `late_real`, `late_s`, halved-period counts | the gate |
| 9 | Stereo mismatch | any class | per-eye residual difference | the same transform for both eyes |
| 10 | Jitter | DLSS input jitter left in depth and motion | a static-scene residual above the floor | remove the Halton offset when mapping render to output pixels |
| 11 | Pose prediction shortfall | PiOpenXR past about 20 ms; possibly other runtimes | the eye-position miss (mm, degrees) against the lead time; unrendered area at the edge the head turns toward | the velocity fallback (## Where it runs) |

## Budget

- **GPU:** unknown, and that is H5. The target is at most 1.5 ms a pair at the
  flown sizes against a 22.2 ms half-rate frame at 90 Hz. The scorer works at
  quarter size, with a target of at most 0.3 ms. For scale only: NVIDIA's own
  FRUC is quoted at 4.41 ms per 1080p interpolated frame on a 4090 ([NVIDIA
  application note,
  search](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note/index.html)).
  A VR pair is several times that many pixels.
- **VRAM:** S is an output-size RGBA8 pair, held on the game device, in the
  shared transfer and on the XR side. At 2600x2514 per eye that is 52 MB a
  copy, about 160 MB for three. At 4068x4016 it is 131 MB a copy, about 390 MB
  for three. The render-size splat buffers add one R32 surface per eye.
- **Latency:** none added in Lock or Fill (## Pacing). Interpolation would add
  at least one period, 11.1 ms at 90 Hz.

## Config

- `fix.frame_generation` = `off` (default) | `auto`. The menu label is
  "Frame generation". The value names what the user gets. `on` is a temporary
  A/B value that forces Lock in admitted contexts, ignoring the performance
  terms, and goes when the arc closes.
- `fix.frame_generation_shadow` = `off` | `on`, hidden and temporary, for
  Phase 1 only.
- There is no space toggle. A context is admitted by evidence, and a fix that
  always helps gets no switch.

## Phases and decision gates

1. **Phase 0, references (no EDVR code, one session).**
   - Fly stock OFXR-Bridge 0.3.1 on its default optical-flow route over EDVR,
     with vendor smoothing off. Do not use its game-vector route: it hooks
     NGX DLSS Evaluate inside the game, where EDVR's d3d11.dll calls it.
   - Scenes: a bright settlement, supercruise near a star, a station
     interior.
   - EDVR's frame-cycle windows at 22.2 ms show the layer engaged under
     EDVR's session.
   - Then VectorXR v0.20.0 on the same scenes, with only its Frame
     Generation module enabled. Over EDVR it runs through its D3D11 bridge,
     where the depth "stays private"
     ([`framegen_d3d11_bridge.h:35-37`](https://github.com/DienerTech/vectorxr/blob/87e575fb21cde8ad9ef87763c67471b7d0b7ee0a/layer/include/depthxr/framegen_d3d11_bridge.h)),
     so it is colour flow with head rotation removed. It was tested on the
     same headset, a Pimax Crystal Super on PiOpenXR
     ([`docs/usage.md:683-687`](https://github.com/DienerTech/vectorxr/blob/87e575fb21cde8ad9ef87763c67471b7d0b7ee0a/docs/usage.md)).
   - Then fly the same scenes with neither layer and with each runtime's own
     smoothing on.
   - **Gate:** none. It sets the bar the bespoke work must clear. If a layer
     does not load under EDVR, record why: it is a reference, not a
     dependency.
2. **Phase 1, measure only (`fix.frame_generation_shadow = on`).**
   - The scorer: every real frame, at quarter size and per class, it
     extrapolates A to B and interpolates A and C to B (scored one frame
     late), both with EDVR's motion.
   - The floor: measured with the head still while docked.
   - The surface classifier.
   - The frame GPU span while armed, without the per-draw queries.
   - The eye-position miss (H6): the runtime's predicted views for T_S and
     the velocity fallback's, each against the views located for T_S, in mm
     and degrees.
   - The stereo score (H8): every real frame, B's right eye rebuilt from B's
     left eye with depth, against the real right eye, per class, with the
     share of pixels neither eye saw. It prices AFW-style reconstruction in
     Elite, HUD doubling included, without building alternate-eye rendering.
     It also answers the canopy question (## Open questions): stars that line
     up between the eyes as infinitely far, at pixels whose depth says near,
     are scenery behind glass written to depth. VectorXR's depth check scores
     exactly that case ([`docs/architecture.md:194-195`](https://github.com/DienerTech/vectorxr/blob/87e575fb21cde8ad9ef87763c67471b7d0b7ee0a/docs/architecture.md)).
   - The GPU pass profiler (`hotkey.gpu_pass_profile`, built, not flown) at a
     settlement, for one eye's share of Elite's GPU frame (H9).
   - A `frame generation (shadow)` line once a second, with an
     `edvr_log.py --health` tag. A run with no such line reads NEVER
     APPLIED, which tells it apart from a clean score.
   - **Flight:** a settlement in a ship, low flight, the SRV, on foot in a
     settlement, supercruise near a planet, a station interior, and a
     combat zone if one is reachable.
   - **Gate:** H1-H4, H6, H8 and H9 decided per context, and the gate's
     thresholds set from the floor.
3. **Phase 2, Lock on surfaces (`fix.frame_generation = auto | on`).**
   - Build the presenter, the S publication, the ABI extension and the
     gate.
   - **Slices (2026-11-07, after Phase 1's flights; H1, H8, H9 not
     measured, H1 moves to 2.4):**
     - 2.1 the synthesizer on the game device, nothing shown: far and world
       by ex2dh from B's own vectors, the cockpit by the head alone; splat
       at render size with dilation and the winner's guard, background
       pull; resample of B's upscaled output. S's head is extrapolated
       from the head's own motion (no ABI yet). Scored, timed, dumped.
     - 2.2 the slot's pose at Submit (ABI v13) and S's own transfer set;
       nothing shown.
     - 2.3 Lock in the runtime, `fix.frame_generation = off | on`
       (on forced, temporary for A/B), `late_real`, `late_s`, the slot
       schedule tests. Vendor smoothing off first.
     - 2.4 the gate and `auto`, with H1 and vendor halving.
   - **World class, changed by Phase 1:** the ship rig is not the world's
     image motion in flight (flights 2-4); the world rides B's own vectors
     less B's head turn, carried one period, plus the slot's head turn
     (ex2dh, flights 5-6).
   - **Flight:** an A/B at one settlement, off (vendor smoothing at its
     default) against `auto`. Log slots synthesized, `late_real`, `late_s`,
     engage and disengage with reasons, and vendor halving.
   - **Gate to ship:** Sean prefers `auto`, `late_s` is under 1% of
     slots, there is no vendor halving while engaged, and Lock disengages
     within 1 s of a cut.
4. **Phase 3, space,** if Phase 1's space classes pass: the star energy check,
   and the UI path for every context.
5. **Phase 4, Fill mode,** as an experiment.
6. **Phase 5, only on evidence:**
   - residual optical flow after the full geometric pre-warp, if classes 3
     or 5 dominate on surfaces (the licence question below comes first);
   - or the `XR_EXT_frame_synthesis` hand-off, once a Windows runtime
     offers it;
   - or an alternate-eye mode in the style of AFW, if H8 and H9 hold.

## Tests required before a flight

- **A synthesis-math rig.** A CPU reference of each class transform against
  synthetic scenes with known head, rig and object motion, to under 0.1 px. It
  covers a reversed-Z far-plane case, the 1 mm on-foot split, jitter removal,
  the splat winner's guard and the fill's inset.
- **The gate as a pure function,** table-driven: engage, each disengage reason,
  the dwell, a cut, and fail-open.
- **The presenter's slot schedule,** in the style of `native_frame_test`:
  steady state, late real, late S, a cut, engage and disengage. Predicted times
  stay monotonic, and a fail-open path never hands a doubled period.
- **ABI:** an old d3d11.dll against the new runtime, and the reverse.
- **The config contract** (`tools/check_config_contract.py`) for the new keys.

## Declined

These are declined in design, not by flight. Each one names what would re-open
it.

- **Copying OFXR-Bridge or VectorXR code or shaders:** the licences.
  OFXR-Bridge is GPL-3.0, and VectorXR is MPL-2.0, whose copyleft holds per
  file.
- **Optical-flow-first synthesis:** flow is weak on space content, and EDVR has
  better motion. Re-open if Phase 1 shows classes 3 and 5 dominate on surfaces.
- **Interpolation as the default:** the latency. Re-open if H4 holds by a clear
  margin.
- **3:1:** extrapolating two periods ahead from a game below a third of the
  rate is out of scope.
- **Synthesis on the XR device at the slot:** see ## Where it runs.
- **A high-priority D3D12 queue for synthesis (VectorXR's):** it suits optical
  flow, which is heavy. VectorXR measured a stall of about 7 ms with flow on
  the game's queue
  ([`docs/architecture.md:163-171`](https://github.com/DienerTech/vectorxr/blob/87e575fb21cde8ad9ef87763c67471b7d0b7ee0a/docs/architecture.md)).
  This design's frame has no flow and is made on the game device. Re-open if H5
  fails.
- **A brightness gate:** brightness is flow's proxy for prediction quality, and
  the gate measures prediction quality directly.
- **Depth submission to the vendor as the fix:** it was set aside on
  2026-09-24, and the vendor still would not know the ship.
- **AFW-style alternate-eye rendering as the replacement for Lock:** Elite
  renders both eyes every frame from one game tick, and EDVR has never dropped
  one eye's work. Alternate eyes save GPU, not CPU, while settlements need
  9.5-13.4 ms of CPU against 11.1 ms at 90 Hz. AFW's DLL is D3D12 only, closed
  and unlicensed. Re-open if H8 and H9 hold. Its stereo hole fill and depth
  dilation are taken (## The synthesis pass; 2026-10-27 entry).
- **Frame generation in the flat profile:** no vendor SDK has a D3D11 path,
  flat's slow cases are mostly EDVR's own render-thread cost, and the users who
  would want it most are under AMD's 60 fps input floor. Re-open when treated
  flat frames hold 60 fps or more on 120 Hz or faster displays, with FSR 3
  frame interpolation from the D3D11 fork EDVR already pins and an EDVR-written
  presenter (2026-10-27 entry).

## Open questions

1. Does the canopy write depth (## Space)? Phase 1's stereo score answers it.
   An eye dump would too.
2. Can S's swapchain images be acquired early, so the copy happens when S
   arrives rather than at its slot? This needs the OpenXR swapchain acquire
   rules checked.
3. Does each runtime return to full rate once EDVR submits every period? VDXR
   held exactly half rate for minutes at a time, and SSW engaging on the game's
   stalls and staying on is that doc's suspected cause
   (`docs/engine-render-performance-2026-09-19.md`, 2026-09-22 addendum).
4. Is a lower refresh rate (`XR_FB_display_refresh_rate`, already enabled) an
   artifact-free alternative for some headsets? The spec makes it only a
   request, and says lowering gives "higher latency and flickering"
   ([spec](https://github.com/KhronosGroup/OpenXR-Docs/blob/main/specification/sources/chapters/extensions/fb/fb_display_refresh_rate.adoc)).
   Which rates each runtime enumerates and honours was not found.
5. Licence, if flow is ever added: NVIDIA's Optical Flow SDK licence is
   reported to forbid use that makes it subject to an open-source licence
   ([search](https://docs.nvidia.com/video-technologies/optical-flow-sdk/license/index.html)).
   The D3D12 interface headers OFXR vendors carry MIT-style terms
   (`licenses/NVIDIA-Optical-Flow-Headers.txt`). Resolve this before Phase 5.

## Environment

- **Applies to** EDVR's native OpenXR runtime on Windows, on whichever OpenXR
  runtime it selects: SteamVR, Meta PC (Link and Air Link), VDXR, or Pimax
  OpenXR. The game is D3D11 with temporal AA on (DLSS, FSR 3.1, XeSS-SR or
  TAA), because depth and motion exist only then. It works on any GPU vendor,
  since there is no flow.
- **Does not apply to** the flat profile on any platform (## Declined), or
  Elite's own LibOVR and OpenVR routes, which EDVR's runtime bypasses.
- **Per-eye sizes on record:** Pimax Crystal Super flown between 2016x2017 and
  2604x2644; SteamVR at 4068x4016; Quest 3 from 1824x1968 base
  (`docs/openxr-resolution-per-headset-2026-09-14.md`) and 1536x1632 on foot
  (`docs/design-native-onfoot-vr-2026-10-13.md`, F95).
- **Refresh rates:** 72, 90 and 120 Hz runtimes have all been seen. At 72 Hz,
  Lock means 36 real fps, which is the same as today's halved stretches there.
- **No fixed-size tables** are planned beyond the one S pair.

## Journal

### 2026-10-26: design written

- **Read:**
  - OFXR-Bridge's source at `d7fcca72` and 0.2.1, not built;
  - EDVR's frame loop, temporal pass, context and timing code at
    `6b8ef374`;
  - the platform sources linked above. Vendor documentation hosts were
    unreachable from the session, so Meta, Valve, Pimax and NVIDIA
    behaviour is from search results, marked (search). The GitHub-hosted
    specs, the Khronos inventory, Microsoft's API docs, the FidelityFX SDK
    and NVAPI were read.
- **The finding that shaped the design:** Elite already runs at half rate where
  it struggles, because SteamVR and VDXR halve the period. So the feature is a
  better filler for the slot the runtime already leaves. EDVR is the only party
  that knows the ship's motion, the cockpit, the HUD and the depth, and an
  extrapolator built on those adds no latency.
- **Next:** Sean's call on Phase 0, and the Phase 1 build.

### 2026-10-27: AFW, and frame generation in the flat profile

**AFW, suggested by a user (relayed by Sean).** The user's report from UEVR: no
ghosting, but the edges of some objects shimmer in fast movement.

- **What it is.** PureDark's Alternate Frame Warping, on UEVR's `AFW` branch
  (`fc8de102`). The game renders one eye per frame, alternating, at the full
  display rate. A closed DLL rebuilds the other eye in one of three modes,
  `AlternateEyeWarping`, `PreviousFrameWarping` and `CombinedWarping`
  ([`PDAFWPlugin.h:36-38`](https://github.com/PureDark/UEVR/blob/f49cd54e919c368fdf1b253ac5b1266caf56436a/dependencies/pd-afwmod/include/PDAFWPlugin.h)).
  Each call takes the rendered eye's colour, depth and motion and "returns
  reprojected result" (`:287`). OFXR-Bridge's fallback describes the alternate
  mode as "current source/depth stereo warp, no missing temporal color"
  ([`luke_afw_continuity.hpp:21`](https://github.com/tig3rmast3r/OFXR-Bridge/blob/d7fcca72d569ec364281ab63ae29378bbfaf1251/include/xrfg/luke_afw_continuity.hpp)).
  So: the fresh eye warped across with depth, the held eye's own last frame
  carried forward, or both. No future frame is used, which fits "no ghosting".
- **Preconditions and licence.**
  - D3D12 only. UEVR enables it only under DX12
    ([`VR.hpp:520-523`](https://github.com/PureDark/UEVR/blob/fc8de1026622d42d0d1f76c3e9c8bb0d80b11035/src/mods/VR.hpp)) and otherwise says "AFW only
    supports DX12, fallback to AFR" ([`VR.cpp:3052`](https://github.com/PureDark/UEVR/blob/fc8de1026622d42d0d1f76c3e9c8bb0d80b11035/src/mods/VR.cpp)).
  - Its inputs come from hooking DLSS Evaluate.
  - The repository holds the header and a "dummy implementation of the DLL"
    ([`dummy/PDAFWPlugin.cpp`](https://github.com/PureDark/UEVR/blob/f49cd54e919c368fdf1b253ac5b1266caf56436a/dependencies/pd-afwmod/dummy/PDAFWPlugin.cpp)). The DLL ships in
    release archives, and the fork's `LICENSE` reads "All rights reserved".
  - OFXR-Bridge runs the beta.6 DLL for Luke Ross "Legacy AER" games
    ([`README.md:85-99`](https://github.com/tig3rmast3r/OFXR-Bridge/blob/d7fcca72d569ec364281ab63ae29378bbfaf1251/README.md)).
- **Known artifacts.** OFXR-Bridge reports HUD doubling when moving or rotating
  in five of the seven titles it tested (`README.md:105-120`), likely because a
  HUD with no depth of its own is warped with the world behind it. It added its
  own depth-dilation pass
  ([`luke_depth_convert.hlsl`](https://github.com/tig3rmast3r/OFXR-Bridge/blob/d7fcca72d569ec364281ab63ae29378bbfaf1251/src/d3d12/luke_depth_convert.hlsl))
  to reduce "ghosting, flicker and shimmering" (`README.md:98`), which matches
  the user's edge shimmer.
- **Elite against those preconditions.**
  - Elite renders each eye as its own pass, left first, both from one game tick
    (`docs/engine-render-pipeline.md`, the 2026-09-21 known-facts entry).
    Culling is one traversal with a per-view loop
    (`docs/stereo-cpu-sharing-2026-10-16.md`). The G-buffer, light grid, post
    chain, bloom and EDVR's DLSS run per eye.
  - EDVR has never dropped one eye's scene draws. Doing so needs each draw's
    eye. That is solid in a cockpit (5068 matched, 0 swapped,
    `docs/ui-layer-2026-09-23.md`) and about 98% on foot
    (`docs/design-native-onfoot-vr-2026-10-13.md`, F121), and one on-foot
    session mislabelled the eyes.
  - Alternate eyes save GPU, not CPU. Elite still simulates, culls and records
    both eyes, now every display period. A settlement cockpit measured about
    9.5 ms of CPU, and the Pimax producer 11.9-13.4 ms, against 11.1 ms at
    90 Hz (`docs/engine-render-performance-2026-09-19.md`). Lock gives 22.2 ms.
  - No per-eye split of Elite's GPU frame has been measured. DLSS alone is
    2.0-2.8 ms an eye on the Pimax (`docs/performance.md`).
  - DLSS resets an eye's history whenever that eye's frames are not
    contiguous (`src/d3d11/native_temporal.cpp:505`; `skipEye` at `:556`), so
    evaluating each eye every other frame needs a two-frame history path.
- **Decision.**
  - Not a replacement for Lock (## Declined).
  - Taken into the design: the other eye's frame as the first disocclusion
    fill, and depth dilation before the splat (## The synthesis pass).
  - Added to Phase 1: the stereo score (H8) and the per-eye GPU share (H9). If
    both hold, an alternate-eye mode becomes a Phase 5 candidate.
  - Its advantage is real. Every displayed frame carries one fresh eye, and the
    rebuilt eye borrows from a real image of the same moment. So lasers,
    explosions and walkers come out right except where only one eye sees,
    which are the classes extrapolation cannot predict (3 and 2 above).

**Frame generation in the flat profile (Sean's question).**

- **No vendor SDK has a D3D11 path.**
  - DLSS Frame Generation is D3D12 and Vulkan
    ([Streamline DLSS-G guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md)).
  - FSR frame generation is D3D12 only in the current FidelityFX SDK, and no
    tag of it has a DX11 backend
    ([FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)).
  - XeSS-FG is D3D12 only and requires XeLL
    ([guide](https://github.com/intel/xess/blob/main/doc/xess_fg_developer_guide_english.md)).
  - Under Proton, vkd3d-proton (D3D12) added the interop that lets dxvk-nvapi
    run DLSS frame generation
    ([changelog](https://github.com/HansKristian-Work/vkd3d-proton/blob/master/CHANGELOG.md)).
    Nothing equivalent was found for D3D11 under DXVK.
- **The D3D11 fork EDVR pins for FSR upscaling has half of it.** That fork is
  optiscaler/FidelityFX-SDK-DX11 at `9b04fa49` (`tools/fetch_ffx_dx11.py`).
  - It carries FSR 3's frame-interpolation and optical-flow shaders, built for
    D3D11. EDVR's build turns them off (`-DFFX_FI=OFF`, `-DFFX_OF=OFF`).
  - It has no presenter. The backend's frame-generation hook is an empty stub
    ([`ffx_dx11.cpp:157`](https://github.com/optiscaler/FidelityFX-SDK-DX11/blob/9b04fa49abae5aa341c0007af13c8ba1b4ad9913/sdk/src/backends/dx11/ffx_dx11.cpp)).
    The `FrameInterpolationSwapchain` directory its CMake lists
    ([`CMakeLists.txt:202-203`](https://github.com/optiscaler/FidelityFX-SDK-DX11/blob/9b04fa49abae5aa341c0007af13c8ba1b4ad9913/sdk/src/backends/dx11/CMakeLists.txt))
    does not exist.
  - OptiScaler, which maintains the fork, says its own frame generation
    supports DX12 only
    ([README](https://github.com/optiscaler/OptiScaler/blob/master/README.md)).
- **Flat's slow cases are mostly EDVR's own render-thread cost with AA on.**
  - An RTX 3050 in an on-foot combat zone ran 21 fps with DLSS and 45-46 fps
    with AA off (`docs/flat-cz-fps-2026-10-11.md`).
  - The Panther Lake handheld ran 28.5-37.3 fps (`docs/performance.md`,
    2026-10-21).
  - AMD says FSR 3.1 frame generation "is designed for an input framerate of
    at least 60 FPS"
    ([doc](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/techniques/frame-interpolation-swap-chain.md)).
- **Driver-level options already work on D3D11** for users at 60 fps or more.
  These come from search results, not opened: AMD Fluid Motion Frames 2, NVIDIA
  Smooth Motion, Lossless Scaling, and lsfg-vk over DXVK on Linux. One user
  already ran Fluid Motion Frames 2.1 alongside EDVR
  (`docs/design-flat-temporal-aa-2026-09-23.md`, ruled out as the cause of a
  drop).
- **Decision: declined for now** (## Declined). In VR the runtime fills a
  missed slot anyway, so a better filler is pure gain. In flat a missed frame
  is only a lower frame rate.

### 2026-10-30: VectorXR

What was read:
[DienerTech/vectorxr](https://github.com/DienerTech/vectorxr/tree/87e575fb21cde8ad9ef87763c67471b7d0b7ee0a)
at `87e575fb` (v0.20.0, 2026-10-29), not built or run. Its issues could not be
read from this session. Paths below are in that repository.

- **What it is.** An OpenXR API layer with five modules, frame generation among
  them (`README.md:3-28`). Frame generation first landed on 2026-10-04 and
  shipped as experimental in v0.19.0 on 2026-10-13. It was tested on an RTX
  5090 with a Pimax Crystal Super on PiOpenXR and on SteamVR
  (`docs/usage.md:683-687`). The licence is MPL-2.0 (`LICENSE`), copyleft per
  file, so its ideas can be used but not its code.
- **What it confirms.**
  - Extrapolation reads only the newest frame and adds no latency. Uncertain
    motion "shrinks toward a still image instead of blending two positions,
    which would ghost" (`layer/shaders/framegen.hlsl:2212-2214`).
  - Its head-movement shift is this design's splat: the nearest pixel wins, the
    winner's source is recorded, and holes are filled by stretching the
    background (`framegen.hlsl:4187-4194`, `:4306-4320`).
  - Its engagement is adaptive. Auto steps up when more than 30% of a second's
    frames miss, and probes back down after five steady seconds
    (`docs/architecture.md:98-103`).
  - Cost: 3.1 ms per real frame for both eyes at 6240x6280 per eye, 4x
    Extrapolate at Balanced flow quality on D3D12 on a 5090, and 3.8 ms at
    Quality. "slower GPUs and Direct3D 11 and Vulkan take more"
    (`docs/usage.md:1003-1006`). That includes optical flow, so H5's 1.5 ms
    target for a flow-free pass at smaller sizes stands.
- **Where it falls short for Elite.**
  - Motion is estimated from colour: it "does not use game motion vectors yet"
    (`docs/usage.md:996-998`). Its engine-vector path is research and D3D12
    only (`layer/src/openxr_layer.cpp:16296-16316`).
  - A D3D11 game's "depth stays private and never reaches the runtime"
    (`layer/include/depthxr/framegen_d3d11_bridge.h:35-37`). Over EDVR it would
    be colour flow with head rotation removed.
  - "There is no HUD masking yet" (`docs/usage.md:999`).
  - Its cockpit and world classes (head-fixed nearer than 4 m, world from 30 m,
    no model between) are research and off by default
    (`layer/include/depthxr/framegen_plan.h:955-959`).
  - The other eye checks depth but supplies no colour for holes
    (`docs/architecture.md:185-199`).
- **Taken into the design.**
  - H6's risk, its fallback and failure class 11 (## Where it runs). EDVR has
    never measured how far PiOpenXR predicts.
  - The splat winner's guard and the fill's inset (## The synthesis pass).
  - The start and finish of each S's GPU work (## Pacing: Lock and Fill).
  - The half-rate report with the settings to check (## Vendor smoothing, and
    the forward path).
  - The canopy answer from the stereo score (Phase 1).
  - VectorXR as a second Phase 0 reference.
- **Not taken:** its high-priority D3D12 queue (## Declined).

### 2026-11-07: Phase 1, first slice built (not flown)

Branch `framegen-phase1` (fork `090809/edvr-unofficial-patch`), for a
Pimax Dream Air on SteamVR's OpenXR runtime (SBoys3 driver), 90 Hz, render
2817x2448 per eye, output 3756x3264, DLSS.

- **Key:** `fix.frame_generation_shadow` (off | on), hidden, temporary,
  read in `temporalPassConfigure`. Off: no resources, no dispatches, no
  lines, and the runtime makes no extra locates.
- **One owner:** the pixel direction, the metres from depth, the world/ship
  split, the carry and the projection are five HLSL functions between the
  `EDVR_TEMPORAL_CARRY` markers in `temporal_shader_source.h`. The history
  fetch and `mv()` call them, and the scorer's shader is built from that
  same text. The motion bytecode changed (a hoisted `pp`, a recomputed
  `rows * d`), not its maths; the temporal rigs and their mutations pass.
- **The scorer** (`src/d3d11/framegen_shadow.cpp`), per eye after the
  trained engine's output, at a quarter of render size: capture luma of the
  upscaled output (before the UI) and the nearest depth of each 4x4; splat
  the previous real frame A forward with the predicted transforms; score
  each cell of B against the real B, and against A unwarped. The prediction
  is constant ship velocity (A's ship part, the world delta less the head's)
  with B's true head, so the head's own miss is H6's alone. The rig miss is
  logged as degrees and millimetres.
- **Cut from the slice:** mover and reactive (no per-pixel signal: no
  general mover mask, and `dlMask` is written only with the UI track or
  engine records), interpolation (H4), the stereo score (H8), the surface
  classifier and H9. Holes are counted, not filled, and the splat winner's
  edge guard is not built.
- **Lines:** `frame generation (shadow): armed, ...` once; `frame generation
  (shadow): frames=... far=... cockpit=... world=... holes=... gpu_ms=...
  rig_deg=... rig_mm=...` each second in the graphics log; `frame generation
  (shadow) pose: ... runtime_mm runtime_deg velocity_mm velocity_deg ...`
  each second in the runtime log (H6, measured at the pair's submit, T_S =
  T + P, truth located once the clock passes T_S). The flag reaches the
  runtime as word 18 of `EdvrNativeFrameOutput` version 12.
- **`gpu_ms`** is sampled only on frames where the GPU census times the
  scorer (`DoorFrameGenShadow`), so the 1 s line carries `(n=...)`, and
  `-(n=0)` when none was timed.
- **Rigs:** `framegen_shadow_test` on WARP (static, head turn, cockpit
  translation with its hole strip, world with constant and accelerating
  ship, the far plane and the 1 mm split, the key off) and
  `framegen_shadow_report_test`; `edvr_log.py --health` rule "frame
  generation shadow" (NEVER APPLIED when armed with no score line, or with
  a runtime log and no pose line).
- **Next flight:** Konovalets Hydroponics Site (Warrapatis 1 b) in a ship,
  low flight, on foot; then docked with the head still (the floor); then
  supercruise near a planet.

### 2026-11-07: Phase 1, flight 1 (v0.22.0-rc.7-27-g1c403828)

Same rig as above; landed at Konovalets, head turns in the cockpit, then
about two minutes of low flight. No on-foot segment. The game ran at 45 fps
(SteamVR's half rate) at rest and 30-45 fps under head turns.
`edvr_log.py --health`: "frame generation shadow" OK. Errors below are
luma x 1024, p95 warped / p95 unwarped (`id:`).

- **Cost:** scorer median 0.05-0.07 ms per eye; a few single 7-10 ms
  maxima at loading stalls.
- **Landed, head still:** every class 0-2 both ways (no signal).
- **Landed, head turns:** cockpit 17-29 / 96-229, world 0-10 / 24-96, far
  0-3 / 17-48. Holes 3-13% at fast turns, read as the view edge (not yet
  split out). The rig miss stays at 0.01 mm with the ship still.
- **Flying:** world 160-650 / 40-230: **the warp is worse than no warp**;
  20-60% of world cells over 0.02. Cockpit 10-34 / 10-40, far 0-2 / 0-5.
  `rig_mm` (predicted world translation against the true one, per frame)
  median 0.2-0.6 m, max 4.9 m, with the head nearly still, so it is the
  frame-to-frame change of the world delta itself.
- **`camera_jump` skips** take up to 54 of 86 frames a second during fast
  head turns: the temporal pass's camera gate fires on head rotation, and
  the scorer drops the hardest frames. Score them, report them apart.
- **H6:** at a 50-70 ms lead (T + P at 45 fps), the runtime's rotation
  misses 0.5-1.8 deg median, p95 to 8 deg, under turns; position 5-13 mm.
  `velocity_deg` equals `runtime_deg` by construction (both turn the pose
  at T by the runtime's angular velocity); only its position differs, and
  it beats the runtime's there (about 5 against 13 mm).
- **Open for the world class, one build to separate them:**
  - W1: frame intervals vary and the prediction is per frame, not per
    second. Signature: the miss tracks `dt_B/dt_A - 1`, and a dt-scaled
    prediction misses much less.
  - W2: A is sometimes two game frames back. Signature: `|tvCam_B| /
    |tvCam_A|` clusters near 2 and 0.5.
  - W3: the scorer's translation convention is wrong (the WARP rig checks
    the shader against a CPU reference with the same convention, so it
    cannot catch this). Signature: scoring with B's true world delta (an
    oracle) is still worse than no warp.

### 2026-11-07: Phase 1, flight 2 (v0.22.0-rc.7-30-g9870da64)

Same site, low flight about 1-2 m a frame (`motion_mm` median to 2000),
45 fps; celestial motion gated off (not supercruise), so the DLSS vectors'
world path is the camera rows' `[W | tvCam]`, the oracle's own transform.

- **The oracle is as bad as the prediction:** world p95 459-545 both ways
  (no warp 136-324), mean 78-87 against 29-55, 30-36% of world cells over
  0.02; median 4. Far and cockpit stay clean.
- ruled out: W1 (per-frame vs per-second), because `rig_dt_mm` is larger
  than `rig_mm` in every flying second (e.g. 103 against 57 mm): Elite's
  step is per frame, and `dt_ratio` 0.8-1.2 is CPU jitter, not sim time.
- ruled out: W2 (A two frames back), because `mag_ratio` p5/p50/p95 stays
  about 0.85/1.00/1.15 in flight.
- ruled out: the prediction as the world class's cause, because the oracle
  (B's measured transform) scores the same as the prediction.
- **Open:** W3 narrowed: either the scorer's pipeline (capture, pairing,
  forward splat) is wrong under translation, or the rows' world transform
  is not the terrain's image motion and the DLSS vectors carry the same
  error. Discriminator: score B backward with the DLSS motion vectors and
  with the true transform at B's own depth (no splat), and log how far the
  two land apart.

### 2026-11-07: Phase 1, flight 3 (v0.22.0-rc.7-34-g2ebd6ecd, compile-only)

Same site, about two minutes low and fast (`motion_mm` 0.4-6.4 m a frame),
40-45 fps, celestial gated off; engine-record motion live. World p95,
warped / unwarped (luma x 1024), per set:

| set | world p95 | over 0.02 |
|---|---|---|
| predicted | 136-771 / 57-459 | 13-57% |
| oracle (B's rows, splat) | same as predicted | same |
| bdepth (B's rows, B's depth, no splat) | 136-771, a little worse | 17-56% |
| mv (EDVR's DLSS vectors, B's depth) | 7-136 / 10-648 | 2-40% |

`mvdis_world_px` (rows landing against DLSS-vector landing): p50 0.00, p95
40-197 px (197 is the top bin). Cockpit: p50 0.00, p95 under 1 px.

- ruled out: the scorer's capture and frame pairing, because the mv set
  scores through the same captures and pairs and beats no warp.
- ruled out: the forward splat, because bdepth (no splat) is no better
  than the oracle.
- **Finding:** for most world pixels the camera rows' `[W | tvCam]` and the
  DLSS vectors agree exactly, but for a large share (over 5% of world
  cells, often far more) they land 40-200+ px apart, and the vectors are
  the ones that match the image. The vector pass gives those pixels
  another motion (engine-record motion is live; which `decisionPath` was
  not logged). So the world class cannot be extrapolated with one rigid
  transform from the rows: the world in flight needs per-pixel motion.
- **Next:** score a per-pixel extrapolation of the world from A's own
  vectors (constant screen velocity, and constant 3D velocity from A's
  vector and depth), and log the share of world pixels by the vector
  pass's decision path.

### 2026-11-07: Phase 1, flight 4 (v0.22.0-rc.7-38-gaaf7f861, compile-only)

Same site, about two minutes low and fast (`motion_mm` 0.4-11 m a frame),
with turns. World p95 (luma x 1024), typical seconds, warped / unwarped:

| set | world p95 | over 0.02 |
|---|---|---|
| rows, predicted | 81-648 | 11-60% |
| mv (B's own vectors: the ceiling) | 24-115 / 34-545 | 7-36% |
| ex2d (A's vectors, constant screen velocity) | 29-229 | 8-57% |
| ex3d (A's vectors and depths, constant 3D velocity) | as ex2d, a little worse | |

`rowsoff_world` (rows and B's vectors more than 4 px apart): 16-60% of
world cells; `rowsoff_cockpit` 0.2-5%.

- **ex2d works for the world in flight:** within 1-2 bins of the mv ceiling
  in steady flight, and below no warp in most seconds. It fails in fast
  turns (12:24:11-13: ex2d 324-459 / mv 68-81 / no warp 545), where the
  screen velocity changes between frames.
- ruled out: constant 3D velocity as an improvement over constant screen
  velocity at this site, because ex3d never beats ex2d by a bin.
- **Cockpit:** ex2d cockpit p95 5-162 is worse than the rigid head path
  (10-34 in flight 1-3): the cockpit keeps the true head of B, not
  extrapolated vectors.
- **Next:** `ex2d` with the head taken out: A's screen velocity less the
  head's own A-1 -> A turn, plus the head's true A -> B turn (the pose the
  runtime predicts for the slot), to see how much of the turn failures is
  the head's.

### 2026-11-07: Phase 1, flight 5 (v0.22.0-rc.7-41-g1ab498ea, compile-only)

Same site: low and fast with ship turns, then steady flight with the head
turned hard and often (from about 12:47:56). `ex2dh` = A's vectors less A's
own head turn, carried at constant screen velocity, plus B's true head
turn (rotation only). World, p95 / mean (luma x 1024):

| segment | mv (ceiling) | ex2d | ex2dh | no warp |
|---|---|---|---|---|
| steady | 7-57 / 2-13 | 9-115 / 3-29 | 7-81 / 2-18 | 12-229 |
| ship turns | 57-324 / 13-57 | 115-324 / 29-75 | 57-115 / 15-26 | 229-545 |
| head turns | 81-386 / 20-79 | 229-648 / 46-127 | 81-386 / 20-81 | 229-771 |

Cockpit p95 in the head-turn segment: ex2d 136-229, ex2dh 34-81.

- **ex2dh is at the ceiling:** within one bin of warping B by B's own
  vectors in every segment, the head-turn one included, where ex2d is two
  to four bins worse. The turn failures of flight 4 were the head's.
- **H3 for the world, answered:** the world in flight extrapolates per
  pixel from A's vectors with the head's turn swapped for the slot's pose,
  as well as B's own vectors would allow. What remains (p95 81-386 under
  head turns) is the ceiling's: quarter grid, disocclusion, DLSS noise.
- ruled out: the ship's own turn as the main cause of flight 4's failures,
  because ex2dh, which keeps the ship's motion constant, stays at the
  ceiling through ship turns.
- **Open:** ex2dh ignores the head's translation (rotation only); the
  cockpit keeps the rigid true-head path, which scores better; holes are
  not filled; the slot's head pose comes from the runtime (H6: 0.5-1.8 deg
  median miss under turns at 50-70 ms lead, flight 1).

### 2026-11-07: Phase 1, flight 6, supercruise (same build)

Supercruise 12:51:05-12:54:25, leaving the planet, cruising, then a body
growing to 35% of the frame. The celestial gate opened, but planet patch
draws stopped 20 s in (`draws=0`), so path 12 never ran; the rows' world
translation in supercruise is near zero (`motion_mm` mostly under 15 mm).
p95 (luma x 1024), warped / unwarped:

- **Far (14-48% of the frame):** 0-5 / 0-14 in every set: stars and sky
  ride the head's rotation.
- **World (the body, 5-38%):** ex2dh 14-96 / 14-648, within a bin of mv
  everywhere; ex2d one to three bins worse under turns (12:52:16: 324
  against 96); the rows' prediction fails on bursts (12:51:40: 545
  against 96).
- **Cockpit:** rigid 12-68, ex2dh 12-68.
- **H3, answered for space in this flight:** far and cockpit predict to
  the floor, and the world through ex2dh to the ceiling. Not covered: a
  supercruise approach with the body in view and path 12 live (the pilot
  left under FSD and barely looked at the planet).
- **After the drop (12:54:48-12:55:50): glide and the approach to the
  settlement, head on it.** World p95 (1-7 m a frame): ex2dh 81-193, mv
  68-115, ex2d 193-459, rows 193-771, no warp 229-648. ex2dh is within one
  bin of the ceiling except in the glide's first seconds (12:54:55: all
  sets 324-459 against 545 unwarped). The drop itself (one frame of 23 km)
  reads as a camera jump.

### 2026-11-07: Phase 2.1, flight 7 (v0.22.0-rc.7-49-g9e71db96, compile-only)

The synthesizer built, nothing shown. Landed with head turns, then low
flight. Every other real frame per eye builds S at f = 0.5 (the shown
variant), the others at f = 1, scored against C's real luma.

- **Cost:** 0.58-0.73 ms per eye per variant (median), max 1.2-1.6 ms with
  a few 2-10 ms outliers: 1.2-1.4 ms a pair for the shown variant, inside
  H5's 1.5 ms. Guard 0.2-0.3%; holes 1.3-1.8% under head turns, 1-4% of
  them left unfilled (the view's entering edge, not read).
- **f = 1 against C, head turns, p95 synth / unwarped:** far 4-20 / 4-20,
  cockpit 57-162 / 136-229, world 68-229 / 115-386. Phase 1's scorer on the
  same seconds (one step, B's TRUE head): far 0-1, cockpit 17-34, world
  17-34. In flight the synth's world is at or a little under no warp
  (e.g. 96 / 273, 459 / 545 in a turn) where ex2dh (true head) scores
  41-81.
- **Hypothesis (not tested): the gap is the head's extrapolation,** not
  the pipeline. S at f = 1 carries the head 44 ms at A -> B's velocity;
  the far class (rotation only) going from 0-1 to 4-20 is that miss. On
  display the compositor turns S by what remains of the head's rotation,
  so this score overstates what is seen. Discriminator: turn S by the
  rotation between the predicted and C's true head before scoring (the
  compositor's step); if the far class falls back to 0-1 and the world
  near ex2dh's, the pipeline is sound.
- **VRAM:** 98% of the budget with pressure lines (11.36 of 11.57 GB);
  the synth adds about 20 B per render pixel per eye.

### 2026-11-07: Phase 2.1, flight 8 and ten replays (v0.22.0-rc.7-55-gcfcf06b9)

The turned score (S at f = 1 turned by the residual between its predicted
head and C's true head, the compositor's step) in game, and ten captures
(`fgreplay_*`, 6 frames each, two of them all camera jumps) replayed on the
RTX 5080 with `framegen_synth_bench`. p95 (luma x 1024), synth / unwarped:

- **Game, turned vs raw:** far raw 2-136 -> turned 0-17; world raw 24-162
  -> turned 14-48, equal to Phase 1's ex2dh (true head) within a bin in
  every second (e.g. 41/41, 20/20, 48/48); cockpit raw 24-229 -> turned
  14-68 against 17-229 unwarped.
- **Replays, aggregates:** world turned 20-41 against 48-136 unwarped
  (one flight capture 115/136); far turned 0-1; cockpit turned 20-48
  against 34-136. Residual turn median 0.07-0.4 deg, max 2.0-2.5 deg.
- ruled out: the synthesis pipeline (dilation, splat, guard, fill,
  resample) as the source of flight 7's gap, because turning S by the
  head's residual brings the far class to 0-1 and the world to ex2dh's
  score: the gap was the head's extrapolation over one Elite step.
- **Bench (idle RTX 5080, 2817x2448 / 3756x3264):** 0.53 ms per eye for the
  shown variant (median; p95 0.69), five passes of 0.05-0.14 ms each; 274
  MB for two eyes.
- **VRAM:** pressure lines all flight, at the end 104-107% of budget with
  the capture's staging in flight (nonlocal 1.4 GB). The baseline before
  the synth was already 94-95% (flight 3).
- **Next:** 2.2 (the slot's pose from the runtime, S's own transfer); an
  optimization pass against the bench and the replays first if wanted.

### 2026-11-07: Phase 2.1, time and VRAM (bench and replays, no flight)

Measured on the RTX 5080 with `framegen_synth_bench` (`--frame` on a real
capture, per eye, median) and checked on all ten replays: aggregate scores
byte-identical, S images within the tie-race floor (0.02% of pixels).

- **Time, shown S:** 0.532 -> 0.414 ms (hardware clears, a hole list with
  one thread per hole, one packed read offset per S pixel). Rejected by
  measurement: 16x16 and 32x8 groups, a pre-read before the splat's max.
  Splat, source and resample move about 100 MB in 0.1 ms each, at the
  card's bandwidth.
- **VRAM, two eyes, the pilot's sizes:** 230.1 -> 170.9 MiB. Synth 184.0 ->
  157.7 (the source grid merged into the packed offset); scorer 46.1 ->
  13.2 (Phase 1's oracle, bdepth, extrapolation sets and their RGBA32F
  vector capture removed; the predicted score, the jump count and the mv
  ceiling kept). Each module logs its bytes by group once per arm.
- **Found:** this GPU's typed R16G16_FLOAT store rounds toward zero; the
  packed offsets truncate explicitly, so the result no longer depends on
  the store's rounding.
- **Pilot's setting:** `ui_quality` 200 -> 100 (the UI layer was 7512x6528
  per eye, several times the frame generation's whole budget).
- **Next in 2.2:** S written straight into its transfer texture, about 94
  MiB of the remaining 158.

### 2026-11-07: Phase 2.2, flight 9 (v0.22.0-rc.7-70-ga9f0a796, compile-only)

The slot's pose from the runtime (T_S = T + (T - T_A) / 2, located once a
pair in `capture()` before the treat) and S's own transfer (the resample
writes S into a keyed-mutex shared texture; the runtime consumes it after
the pair's finish). Nothing shown. About 7 minutes, landed and low flight,
45-51 fps.

- **The transfer works:** `path=uav` (the shared R11G11B10 texture takes
  the UAV on this NVIDIA card); produced = consumed every second, `s_busy`
  0, `s_late` 0 after the first second (2 then); S ready 13-52 ms before
  T_S (`ready_ms` is the CPU handoff, not GPU completion); consume 0.05 ms
  median, 0.46 max.
- **The pose:** from the runtime on all but 1-2 builds a second in the
  first seconds (`pose=89/0` ... `pose=54/0`).
- **VRAM:** game device 93.5 MiB (the producer textures replace S), XR
  device +93.5 MiB (the private copies).
- **Not measured yet:** the miss of the slot pose itself. H6's pose line
  still measures from Submit to T + P (lead 35-64 ms); the slot pose is
  located later and for T_S. 2.3 should point H6 at the slot pose.
- **Next: 2.3**, Lock in the runtime. Vendor smoothing (SteamVR Motion
  Smoothing for Elite) off first.

### 2026-11-07: Phase 2.3 and 2.3b built; flight 10, Lock off, finished S dumped

Lock (2:1 S slot in `waitPoses`, `fix.frame_generation = off | on`, ABI word
17) built in 2.3; 2.3b finishes S like B: resampled from `dlSubmit`, B's
RCAS sharpening, B's UI layer composited moved by the head only (a plane at
1.5 m), shown format (`dlOut` is R8G8B8A8_UNORM, not R11G11B10 as earlier
entries assumed). The pilot's SteamVR per-app throttling set to fixed:
predicted period 11.1 ms, Elite at 57-59 fps unthrottled.

- **Flight 10 (v0.22.0-rc.7-81-gf9fdceff), Lock off:** every synth line
  `why=finished`, `unfinished=0`; finishing 0.16-0.20 ms per eye.
- **Seven dumps, finished S against finished B (`S<k>` / `SB<k>`):** still
  head: mean 0.3-0.4 of 255, 0.8% of pixels over 8; head and ship motion:
  the difference is the motion itself. By eye: the HUD, holo panels and
  scanner are present in every S, aligned with the cockpit, text as crisp
  as B's; terrain marginally softer.
- **Next:** the first Lock flight.

### 2026-11-07: Phase 2.3, flight 11 diagnosed: the S slots broke the d3d11 half's history (built, not flown)

- **Cause, from the flight's own logs and reproduced in the rig:** each S slot
  that begins takes one of the session's frame sequences, so the game's
  engaged frames were numbered b, b + 2, b + 4. The d3d11 half reads a gap as
  a dropped frame (native_temporal.cpp: `continuity + 1 != seq` resets the
  eye's history); with no head delta the temporal pass writes no depth, and
  the synthesizer skips the build. So no S existed for any engaged pair but the
  one built before engaging. Field signature: the graphics log's synth
  `skipped=` is exactly two per engaged real pair in every burst second (8 for
  `slots=4/4`, 6 for 3, 0 with no burst). `produced = consumed` held because
  only S that was built is counted, and four missing pairs in a second do not
  show against 80.
- **ruled out: the slot matching S by the wrong sequence, because** the slot
  asks for `lastPairSequence` and S carries `fs.sequence`, both the same
  `activeFrameSequence` of the same pair; the rig's first replayed slot asks
  for pair n + 2 while the copy holds n, and the reason is `not_built`, never
  `seq_mismatch`. The sequence skip was the root, through the history, not
  the match.
- **Also ruled out for this flight:** S consumed after the slot composes
  (not_consumed would need a built S); the d3d11 half learning Lock a frame
  late (Lock's slot is handed before the treat of the same pair); the shadow
  key's alternation (it decides only whether the first slot shows S: 1/3 or
  0/3, as flown); the finish and door order (s_unfinished would have said it);
  the pose (`pose=` all runtime).
- **Fix:** the runtime numbers the game's frames without the S slots'
  sequences (`FrameGenGameSequence`, framegen_lock.h; reset at a session
  restart only). No S slot: the session's numbering exactly.
- **Rig** (openxr_frame_test, `delivery()`): the host's S bookkeeping and the
  d3d11 continuity, cut to sequences, against the fake runtime: 72 engaged
  pairs, every S slot shows S. Flight 11's build (the mutant) gives S, B, B, B,
  then `no_s`, twice: `s_from=2/6`. Reverting the fix fails three checks.
- **Instrument:** after a one-second line with B replayed, `frame generation
  lock: replayed B: not_built= kept= busy= unfinished= not_consumed=
  seq_mismatch= (asked N had M) no_pose= format= adopt=`; `--health` sums it.
- **Next flight's signature:** `s_from` near `slots`, no `no_s` disengage, synth
  `skipped=` 0 while engaged, and the native temporal resets not climbing per
  engaged pair.

### 2026-11-07: Phase 2.3, flight 12, Lock holds (v0.22.0-rc.7-90-g9a58cf0f)

With the sequence fix (63a29232), the Monitor block and the menu row.

- **Lock held 36 s** (18:09:49-18:10:25) until the pilot opened EDVR's menu:
  every second `slots=45/45 s_from=45/0 late_s=0`, S slot wait 8.4-9.7
  ms; `late_real` 0 but one second of 21 (a stall). Replays of B only as
  `kept` on an engage's first slot. `no_s` never again.
- **The menu disengages Lock by design** (S does not carry the menu), so
  the Monitor page always read "Lock waiting (menu)".
- **Flicker the pilot still felt, 18:12-18:15:** with Lock disengaged the
  runtime's period flips between 11.1 and 22.2 ms (SteamVR throttling a
  ~49 fps app); `period_halved` blocked engaging and the dwell restarted
  every second. The period never halved while Lock was engaged.
- **Next:** engage from a halved period (disengage only if it stays halved
  while engaged), and the menu drawn on S.

### 2026-11-07: the own hull smeared in a ship turn: S's edges (eye dump and replay 222433569001; built, not flown)

Pilot, Lock on: the own ship's hull outside the canopy smears in turns; a distant
docking ship a little. Dump frame 4 = replay f4 (frame 11393): ship pitch 0.72° a
frame (camera rows), head 0.02°. S04 against SB04: the hull's cyan outline torn at
the top edges, a lattice of hull and outline colour beside the lower-left edges.
The bench's f = 0.5 S of replay f4 reproduces both, pixel for pixel in place.

| Hypothesis | Signature | Observed (replay f4, render x 1345-1720, y 1026-1176) | Verdict |
|---|---|---|---|
| H-b the hull split by the 10 m split, torn at the seam | hull depth both sides of `split.x`; cockpit and world keys meeting on the hull | hull 46-83 m (p10-p90), 98.6% world class; the 418 cockpit-class pixels are canopy frame, no seam on the hull | ruled out |
| H-b' (lead) ex2dh's m on the hull carries the turn | hull vector minus the head's part non-zero | hull vector (-0.44, -0.22) px, flat over the region; the head's Hd alone moves it 0.41, 0.21 px: m about 0.03 px. The asteroid moves (-1.95, -15.2) | ruled out |
| H-e (new) the rejection sentinel read as motion | vectors of exactly (2W, 2H) in the hole bands | 3-7% of every frame's pixels carry (5632, 4896) = render size x 2, the temporal pass's history-rejection write (`written`, temporal_shader_source.h) for a background just uncovered, a masked engine record or a fresh screen edit. A band up to 30 px wide beside every edge the background leaves; each such pixel lands off the image, so the band is a hole, filled from 8 px around: the lattice | confirmed |
| H-d edge vectors not dilated with the depth | a pixel within 1 px of the hull: hull key, background vector | the 1 px ring (where the game draws the cyan outline, on background depth) takes the hull's key and moves 7.6 px at the background's motion, into or off the hull: the torn outline | confirmed |
| H-a the fill pulls from the wrong side | hole fills reading the near surface | the fill takes the farthest key; with the bands gone the remaining holes are true disocclusions, filled from the background | ruled out as the cause |
| H-c the outline has its own (holo) motion | decision path 8/10 on the outline | the outline is on background depth with the background's vector (or the sentinel); its motion is H-d's | ruled out as separate |

- ruled out: the hull on the wrong path (H-b), because the hull is 46-83 m and
  its vector is the head's to 0.03 px: ex2dh moves it with the head only.
- ruled out: the fill direction (H-a), because the holes it filled wrongly were
  the sentinel bands; the background fill is right where the hole is real.
- ruled out: a holo or UI motion of the outline (H-c), because the outline is
  background depth and vector, which H-d accounts for.
- **Fix (framegen_synth_shader.h):** a pixel moves by the vector of the 3x3
  pixel whose depth it took (`FG_DILATE_MV`), and a sentinel vector borrows the
  vector of the farthest non-sentinel pixel on a ring of 2-32 px in 8
  directions, else the head's motion alone (`FG_SENTINEL`).
- **Measured (bench, f = 1 S against the next frame, 8-bit):** f0->f1 (the one
  pair not under a camera jump) lead's crop 5.02 -> 4.22 mean, over 32
  2.73% -> 2.04%; the five pairs' crop 12.53 -> 11.85; replay f4 holes filled
  2.58% -> 1.14%, left 1.59% -> 0.39%. The ten 15:26-15:33 replays: no class
  field worse but 152830339005's cockpit over-8 share 8.6 -> 8.7% (the sentinel
  change; world 49.1 -> 48.8, holes left 4.04 -> 1.65%). Cost 0.607 -> 0.628 ms
  an eye (f = 1, frame 4). Crops before and after are in the session's scratch.
- **Rig:** case 13 (a still hull at 60 m over a background moving 8 px, its
  outline at background depth, the uncovered band carrying the sentinel); each
  of `FG_DILATE_MV` and `FG_SENTINEL` off fails it. Case 5's guard now moves no
  read (the sky pixel moves with the object), so the guard's mutation runs with
  the dilated vectors off too.
- **Open:** with the dilated vectors the guard changes 0.26% of replay f4's S
  pixels and is neutral to slightly worse (crop 4.22 vs 4.19 without it); a
  case for removing it, not done here. The f = 1 S on f1-f4 (camera-jump
  pairs) is worse than no warp in the crop (11.85 against 11.02).
- **Flight signature:** in a ship turn with the hull in view, no lattice
  beside the hull's edges and the outline whole in S; the synth line's
  `holes=` filled and left both lower than in flight 12.

### 2026-11-07: S softer than B under Lock: the resample (built, not flown)

The lead measured three eye dumps of this build (225459033001, 225515258002,
225627723003): the finished S has 64-98% (typically ~80%) of the finished B's
mean |Laplacian| of luma, the cockpit worst (0.64-0.92) though it moves with
the head only. Under Lock the pilot sees sharp and soft alternate. Lock itself
held (251 of 263 s engaged).

- **Measure (bench):** `framegen_synth_bench --replay` now prints S's
  sharpness against B (mean |Laplacian| of luma, S over B, per class of the B
  pixel each S pixel reads; f = 0.5 and f = 1) and `--define NAME=V` compiles
  a shader switch as V. 22 replays (all of 15:26-23:03, 8-bit `dlOut`, no
  RCAS). Medians over replays, the shown f = 0.5 S:

  | Resample | far | cockpit | world | all | resample ms/eye |
  |---|---|---|---|---|---|
  | bilinear (before) | 0.815 | 0.833 | 0.791 | 0.819 | 0.091 |
  | Catmull-Rom, 2x2 clamp (chosen) | 0.921 | 0.944 | 0.913 | 0.937 | 0.171 |
  | Catmull-Rom, 4x4 clamp | 0.922 | 0.946 | 0.915 | 0.939 | - |
  | Catmull-Rom, unclamped | 0.923 | 0.952 | 0.917 | 0.942 | 0.156 |
  | Lanczos-2, 2x2 clamp | 0.921 | 0.945 | 0.914 | 0.937 | 0.212 |

  Cockpit per replay 0.806-0.898 before, 0.927 and up after; the highest replay
  1.011 (target ~1.0, not over 1.1). Time: `--bench --frame
  fgreplay_225515258002:4`, the whole f = 0.5 build 0.481 -> 0.570 ms an eye.
- **Scores:** the raw and turned scores against the next frame are unchanged
  to the milli-luma (median per-replay change of every class mean 0; the sum of
  raw world p95 over 22 replays 1960 -> 1986, turned world 1146 -> 1135);
  holes and guard identical. Bilinear through the new read position equals the
  old code's scores exactly.
- **Chosen:** Catmull-Rom with the result clamped per channel to the 2x2
  texels around the read (`synSharp`, `FG_SHARP 1`). Lanczos-2 is no sharper
  and costs a quarter more; the unclamped cubic gains under 1% and can ring at
  a step; the 2x2 clamp is the tighter halo bound at the same sharpness as the
  4x4. The read position is now the output pixel's own centre plus the offset
  in output pixels, so a zero or whole offset returns B's texel exactly.
- ruled out: RCAS at B's strength over-softening S, because AMD RCAS (strength
  0.4, the pilot's `render_sharpness`) applied to both S and B in numpy leaves
  the S/B ratio within 0.01 of the unsharpened one (225515258002 f2 0.981 ->
  0.981, 225627723003 f4 0.932 -> 0.919), and S is already resampled from the
  pre-sharpen `dlOut`. Raising S's strength would be compensation; not done.
- **What remains:** about 6% of the energy; an interpolator half a pixel off
  passes nothing at Nyquist, so this is the floor of any resample.
- **Rig:** case 14 (still scene and a whole-pixel read return B bit for bit, at
  1:1 and 2:1; a 5 px period read 0.25 px off keeps 0.966, bilinear 0.861; a
  step read 0.3 px off stays within its two levels). `FG_SHARP` 0 (bilinear)
  and 3 (unclamped) each fail it.
- **Flight signature:** with Lock engaged, the eye dumps' S/SB Laplacian
  ratio about 0.9-1.0 in the cockpit, and no sharp/soft alternation felt.

### 2026-11-08: torn terrain behind the hull in a ship turn: the disocclusion fill (built, not flown)

The pilot circled it and the lead confirmed it in eye dump 230157175005
(S02/S06 against SB02/SB06, crop x 950-1550, y 700-950): in a ship turn over
terrain, the terrain uncovered behind the trailing edge of the own hull's raised
strake shows in S as a sawtooth of torn strips, different in every S. Cause:
the hole fill (`fgSynHoles`) took the farthest key within 8 px and read 2 px
further into its source; each row picked a different neighbour.

- **Built:** the fill carries the hole back into B by the background
  neighbour's own read offset (its motion). Where B there is the background's
  surface (every texel of the 2x2 from 0.75 to 4/3 of the neighbour's distance,
  the far plane only for the far plane), S reads it (`fill_reproj`). Where the
  occluder covered that point in B too, the read steps along the background's
  motion relative to the occluder (the occluder pixel's own landing taken out),
  up to 32 px, to the first such texel and reads 2 px past it
  (`fill_stretch`). Otherwise the old pull. A sparse ring (12 and 16 px, 8
  directions) offers a farther key than the 17x17 search, for holes wider than
  it. `FG_FILL_REPROJ 0` is the old fill exactly (`2`: no stretch, bench only).
  Stats words 603/604; the synth line gains `fill_reproj=% fill_stretch=%` of
  the filled after `clamp=` (`edvr_log` reads lines without them).
- **Bench:** `--replay` now prints the hole region's error (mean |luma| of the
  f = 1 S against C over output pixels whose render pixel was a hole, B against
  C beside it) and S/B sharpness for the holes and outside them. 21 scored
  replays, medians, before -> after: hole region 34.3 -> 32.6 milli-luma
  (B 11.5); the hull-turn replays gain most (222433569001 40.4 -> 32.2,
  225459033001 51.0 -> 40.9, 225627723003 61.8 -> 52.4, 230157175005 30.5 ->
  29.7); six surface replays of 15:30-20:28 lose 1.5-3.8, in the lens corners
  where S itself is mispredicted. Raw and turned class means and p95 unchanged
  (one world p95 162 -> 136). Sharpness S/B outside the holes 0.927 -> 0.926
  (f = 0.5); over all pixels 0.937 -> 0.933, the sawtooth's false edges gone.
  Fill shares (median): reprojected 4%, stretched 53% (hull-turn replays
  22-56% reprojected). Cost (`--bench --frame fgreplay_230157175005:2`, f = 0.5,
  three runs): holes 0.095 -> 0.102 ms, total 0.564 -> 0.573 ms per eye.
- ruled out: an ungated reprojection with a one-sided depth test ("at least as
  far"), because it read farther surfaces beyond a silhouette and made the
  hole region worse in 15 of 21 replays (median +1.9 milli-luma).
- ruled out: gating the read on its own texel landing on the hole (within 1.5 px,
  also with a 4-step backward search), because almost no texel passes (0.1-3%
  reprojected): the holes are true disocclusions, and the gains went with it.
- ruled out: the two-sided test without the ring, because the strake's own
  side (world class here, past the 10 m split) was the farthest key within
  8 px of the deeper holes and was pasted beside the strake, though it scored
  best (median 31.5).
- **Rig:** case 15 (a world plane, farther up the image, uncovered beside a still
  near bar: every row reads its own row, stretched); `FG_FILL_REPROJ 0` fails it,
  `FG_FILL_FAR 0` now fails it too (case 5's sky is found from either side).
  Case 13 also checks the hole count, since the sentinel mutation's holes are
  now filled right. Case 5's guard counter flips 0/1 in about 1 run in 8 on
  WARP before and after this change (a tie in `fgSynSrc`), not this fill.
- **Crops:** before and after S (f = 0.5, frame 24124 = dump k02) of the
  pilot's region in the agent's scratchpad; the after shows the terrain
  continuing behind the strake, a faint seam left at the strip's top.
- **Flight signature:** the eye dumps' S beside the hull's trailing edge in a
  ship turn continues the terrain, the same in every S; the synth line's
  `fill_stretch` 40-60% of the filled.

### 2026-11-08: Phase 2.3, second time and VRAM pass (bench and replays, no flight)

RTX 5080, `framegen_synth_bench --bench --frame` on 230157175005:2 (hull turn),
225515258002:4 and 222433569001:4, three runs of 300 each; ms per eye, the
shown f = 0.5 S, median (p95 within 0.006 of it), in that order of captures. Run-to-run spread of a pass
median 0.000-0.005 (holes), of the total up to 0.011.

| Pass | before (87014896^) | after (87014896) |
|---|---|---|
| clear | 0.016 | 0.016 |
| splat | 0.103 / 0.107 / 0.106 | 0.108 / 0.111 / 0.109 |
| src | 0.135 / 0.151 / 0.152 | 0.132 / 0.136 / 0.144 |
| resolve | 0.053 / 0.054 / 0.053 | 0.055 / 0.055 / 0.055 |
| holes | 0.098 / 0.066 / 0.086 | 0.104 / 0.067 / 0.087 |
| resample | 0.162 / 0.169 / 0.169 | 0.161 / 0.169 / 0.168 |
| total | 0.568 / 0.566 / 0.584 | 0.577 / 0.552 / 0.580 |

- **Accepted, `fgSynSrc`'s tie (87014896):** two sources tying for an S pixel
  both wrote and the last stood, so S at ties (0.022% of S pixels between two
  runs of a replay) and the guard counter varied (rig case 5 failed about 1
  run in 8). The splat now sets the read offsets to all ones and `fgSynSrc`
  offers with InterlockedMin; the guard mark moved to the y half's exponent
  with the sign bit, so a tie keeps the unguarded read. Cost-neutral (above).
  Two replay runs now give byte-identical S and scores; rig 30 of 30. Against
  the racy build 0.14% of the f = 0.5 S pixels differ; class means and p95
  unchanged in median (one turned world p95 4 lower), hole error unchanged
  (median 32.62 milli-luma), guard 0.30 -> 0.27%, S/B sharpness outside the
  holes median -0.0005 (worst 222433569001, 0.9154 -> 0.9103): a guarded read
  copies a whole texel, and the race kept more of them. Preferring the guarded
  read instead fails rig case 6 (a still scene's edges move a pixel).
- ruled out: a 16x16 groupshared tile for the resample's 4x4 reads (resample
  0.169 -> 0.277), because the pass is bound by DRAM traffic (about 125 MB in
  0.16 ms: B's output, S, the read offsets), not by texture loads; two output
  pixels a thread sharing a 4x5 window gained 0.000-0.010 (within the spread);
  16x4, 32x2 and 4x16 groups lost 0.002-0.016.
- ruled out: a 10x10 groupshared depth tile for the splat's and the source's
  3x3 (splat +0.001-0.005, src 0.135 -> 0.143 on 230157175005), because the
  3x3 reads hit the cache.
- ruled out: the resolve folded into the source pass (the holes listed there,
  the guard counted in the resample or from InterlockedMin's returned value),
  because the source pass grew 0.03-0.04 and the resample 0.02 (it reads the
  offsets the resolve left in L2): totals -0.011 at best, +0.05 with the
  returning atomics.
- ruled out: eight threads a hole in `fgSynHoles` (the 17x17 search, the ring,
  the winner's 16 candidates and the stretch's steps split and reduced in
  order), because the pass went 0.104 -> 0.125: it is bound by its loads, not
  by one thread's chain.
- **VRAM:** unchanged, 157.7 MiB for two eyes and the scratch (per eye s 46.8,
  luma 0.8, class 3.3 MiB; shared key 26.3, read 26.3, holes 3.3 MiB).
- **Not attempted:** RCAS and the UI composite in one pass on S (the
  finishing, 0.16-0.20 ms in game): it needs B's sharpener's HLSL inside the
  synth's text and has no bench; it is measurable only in a flight.

### 2026-11-08: Phase 2.3, third time pass, an independent review (bench and replays, no flight)

A reviewer's roofline of the shown f = 0.5 S on the RTX 5080 (960 GB/s, 64 MB
L2; render 6.90 Mpx, output 12.26 Mpx: a render-size grid is 27.6 MB, an
output-size one 49.0 MB), 230157175005:2, against the bytes each pass must
move. The bound a pass sits at was tested by turning its work off with the
shader's own switches, not inferred from the byte count.

| pass | ms | bytes it must move (MB) | what bounds it |
|---|---|---|---|
| clear | 0.016 | key 27.6 (API fast clear) | the API; an all-ones clear of the read grid costs 0.019, so it has no fast path |
| splat | 0.109 | depth + mv 55 read, read grid 27.6 write, key 27.6 read-modify-write | at the bound: 110-138 MB is more than DRAM gives in 0.109 ms, so the key grid's 27.6M InterlockedMax run in L2; FG_SENTINEL=0 saves 0.004 |
| src | 0.132 | depth + mv 55 read, key 27.6 read, read grid about 7M InterlockedMin | not DRAM-bound (83 MB, 630 GB/s): 0.016 is the guard's nine metres (FG_GUARD=0), 0.017 the sentinel ring (FG_SENTINEL=0), 0.005 the dilation; the rest the key reads and the atomics |
| resolve | 0.055 | key 27.6 + read 27.6 read | two dependent loads a pixel at DRAM speed (1.0 TB/s equivalent) though both grids should sit in L2 |
| holes | 0.100 (f = 1 0.168) | about 100k holes, 289 key loads + 16 candidates each | latency and divergence; FG_FILL_REPROJ=0 saves 0.008 |
| resample | 0.161 | B 49 read + S 49 write, read grid 27.6 | **fetch-bound, not memory-bound**: FG_SHARP=0 (4 fetches, the same 98 MB) runs in 0.089; 8 fetches 0.126, 12 0.143, 16 0.161 |

- **What the second pass missed:** the resample's "bound by DRAM traffic" was
  inferred from 125 MB in 0.16 ms; the bilinear switch refutes it (0.089 ms
  for the same bytes). The pass pays per fetch, so the same Catmull-Rom in
  fewer fetches is the saving: the centre 2x2 as point loads (exact, and the
  clamp's bounds), each edge pair's two centre taps as one bilinear fetch
  through a clamped sampler (s0, new in the pass table) at the ratio of their
  weights, the corners as point loads: 12 fetches. The sampler's weights are
  8-bit; against the 16-load form 0.053% of the S pixels of the 22 replays
  differ, all by one 8-bit linear step (the PNGs are sRGB, so the dark ones
  show as 1-13 steps); every class mean and p95, the hole error and the
  sharpness are unchanged to the printed digit.
- **Accepted, the resample in 12 fetches:** resample 0.161 / 0.169 / 0.168 ->
  0.142 / 0.153 / 0.159 ms on 230157175005:2 / 225515258002:4 /
  222433569001:4; total 0.576 / 0.549 / 0.581 -> 0.559 / 0.535 / 0.572
  (three runs of 300 each, run-to-run spread of a pass median at most 0.004,
  of the total 0.007). Rig 137 of 137. The 16-load form stays as the bench's
  comparison `FG_SHARP=5`.
- **Accepted, the corners dropped (8 fetches, `FG_SHARP` 1):** the four
  corner taps weigh at most a sixteenth squared each (under 1.6% in all) and
  are dropped, the rest renormalised: resample -> 0.126 / 0.133 / 0.139,
  total 0.539 / 0.514 / 0.551 (6.4% / 6.4% / 5.2% under the second pass). On
  the 22 replays every turned p95 is the same bin, every mean the same
  milli-luma but one (cockpit 10 -> 9 on 202815590002), the hole error +0.00
  to +0.04 milli-luma (at most +0.1%), S/B sharpness outside the holes +0.003
  to +0.006 (the kernel is a shade more peaked: S nearer B). 2.1% of the S
  pixels differ from the 12-fetch form, by up to three 8-bit linear steps.
  Its own commit, so it can go back alone; the 12-fetch form is the bench's
  comparison `FG_SHARP=6`.
- ruled out: the read grid's all-ones by `ClearUnorderedAccessViewUint`
  instead of the splat's store, because the clear costs 0.019 (no fast path
  for a non-zero clear on this card) and the splat saves 0.006.
- ruled out: `[unroll]` on the fill's 17 search rows or on the winner's 16
  candidates, because the hole pass went 0.099 -> 0.098 / 0.103 at f = 0.5
  (0.167 -> 0.150 at f = 1 for the rows) and the rows' unroll takes fxc 145
  seconds a compile.
- Not pursued, with the reason: a one-pass splat (key and offset in one
  atomic) needs 64-bit atomics, which cs_5_0 has not; the landing stored by
  the splat for the source pass to read moves 55 MB more than the recompute
  it saves; a coarser splat where the motion is smooth, or S = B over the
  static cockpit, still needs every pixel's landing to know it is static;
  the resample "only where S differs from B" still reads B and writes S for
  the copy (the same 98 MB). The f = 1 build is not an extra per-frame cost:
  the shadow alternates it with the f = 0.5 build frame by frame and Lock
  builds f = 0.5 only.
- **Checked, not a bug:** the source pass recomputes the splat's landing in a
  second shader; a landing that rounded differently would leave a key with
  no offer, which the resolve would count as guarded and the resample would
  read 2047 px away. The bench now counts those pixels (`orphan` on the
  sharpness line): 0 on every frame of the 22 replays. The design still
  rests on two shaders agreeing bit for bit; the resolve could treat the
  splat's all-ones under a key as a hole for a load and a compare.
- **Review, the rest of the synth path:** nothing else found that fails an
  input. Two small things in the bench, not the synth: `--png-dir` makes one
  directory level only (a missing parent writes nothing and says so per
  PNG), and without `--png-dir` it writes `bench_out` into the capture.

### 2026-11-08: the own hull's outline torn against space in a ship turn (eye dump and replay 120812713001; built, not flown)

Pilot, Lock on: the own ship's hull still shows torn edges where it is seen
against space (the 2026-11-07 fix covered the hull over terrain). Lead's crop
of the dump (x 550-1550, y 560-900 of S03 and S14): the thin cyan outline
along the silhouette breaks off in stair steps (top-left strake), and a white
blotch on the central block's top. Replay f3 = dump k03 (frame 20409; the
replay has six frames, so S14 is not replayed). The bench's f = 0.5 S of f3
reproduces both in place: the outline is drawn a second time 4-5 render px
up-left of the hull, in the sky, and the hull's own edge loses it in steps.

Environment: OpenXR, left eye, render 2817x2448, output 3756x3264 (DLSS, 8-bit
`dlOut`), the hull world class at 43-80 m (the canopy frame, 2-3.5 m, is the
cockpit class). Ship turn 1.8 degrees a frame (`world_w`), head 0.06: the
sky's vector (8.6, 14.9) render px median, the hull's (1.26, 0.51), the head's
alone.

| Hypothesis | Signature | Observed (replay f3, render x 1120-1870, y 943-1198) | Verdict |
|---|---|---|---|
| H-o (lead) the outline is drawn without depth, so it moves with the sky | the outline's pixels far-plane depth, beyond the 3x3's reach | the outline is the far plane (1178 of its 2083 cyan render pixels; the rest on the hull's own edge). In render pixels it sits 1 px out, but in B's output it and its glow reach 2 render px from the hull (Chebyshev 2: 12% of the cyan output pixels over the sky, mean luma 23 against the sky's 0.1), none at 3. The 3x3 dilation (aede0bf4) reaches 1 | confirmed, R = 2 (not 3-4) |
| H-o' the fill reads the outline back | hole pixels beside the hull read B on the outline | the S pixels the sky leaves beside the hull are holes; the fill carries them back by the sky's motion onto the 2 px ring, whose depth is the far plane, so `synBackAt` takes it for sky: with the halo alone the detached outline stays (1036 of its output pixels were fill kind 2, reprojected) | confirmed |
| H-o'' the footprint reads into the ring | far-key winners whose read is 1 px from their source, on the ring | a sky pixel 3 px out offers its footprint neighbour a read 1 px toward the hull, inside the ring: 93 detached cyan output pixels after the two fixes above (rig: S 4 px into the sky reads the outline exactly) | confirmed |
| H-c the outline has its own (holo or screen) vectors | outline vectors unlike the sky's or the hull's | 85% of the 1 px ring carries the rejection sentinel, the rest the sky's vector; nothing of its own | ruled out |
| H-g the guard moves it | guarded reads at the outline | 13 of the 1260 detached outline output pixels are guarded reads | ruled out |
| H-w the white blotch: a rejected patch borrows the sky's vector | sentinel vectors on the block top, read offsets the sky's | a 1 m nearer feature on the block top (57.9 m on a 58.7 m hull) carries the sentinel; `FG_SENTINEL`'s ring takes the FARTHEST non-sentinel pixel within 32 px, which is the sky past the silhouette: those pixels read (4.65, 4.0) px, the sky's motion, against the hull's (0.63, 0.25) | confirmed |

- ruled out: the outline on a holo or screen path of its own (H-c), because
  its pixels carry the sky's vector or the sentinel, nothing else.
- ruled out: the guard (H-g), because 13 of 1260 detached outline pixels
  are guarded reads; the rest are winners or fills.
- ruled out: an outline 3-4 px wide (the lead's estimate), because no cyan
  output pixel lies over a sky render pixel 3 px from the hull: R = 2.
- **Fix (framegen_synth_shader.h, `FG_HALO`, `FG_SENTINEL_SURFACE`):**
  - a far-plane pixel whose 3x3 is all the far plane but whose 5x5 ring holds
    a nearer surface takes the nearest of that ring's depths and that pixel's
    vector (`synHalo`, in every place the dilated depth is taken); stars
    further out are untouched;
  - the fill's test that B is the far plane at a read also requires no nearer
    surface within 2 px of its 2x2 (`synBackAt`), so a hole is not filled
    from the outline;
  - a far-plane source with a nearer surface 3 px away marks that direction
    for the guard (`synHaloMetres`), so its footprint read does not reach into
    the ring;
  - a sentinel vector borrows from the farthest ring pixel on its own surface
    (within 0.75 to 4/3 of its distance; the far plane only for the far
    plane) before the farthest of any surface.
- An early-return form of the fill's 6x6 test (`return false` inside the
  loops) had no effect in the bench on the RTX 5080; written as a maximum
  over the block it works. Noted in case the pattern is reused.
- **Measured, replay f3 (f = 0.5 S, lead's crop):** cyan output pixels in
  the sky 3 px or more from the hull 1260 -> 123 with the halo, the fill
  test and the surface rule (counted before the guard part was added; the
  rig shows that part's case exactly, a read of the outline 4 px out);
  the outline sits on the hull as in B, the block top matches B.
- **Measured, the 23 replays in the eyes folder (`--replay`, before = the
  three switches 0, which reproduces HEAD's output):** turned class means and
  p95 unchanged but four one-step moves (far mean -1 on 222433569001 and
  225627723003, cockpit +1 on 202815590002, world p95 20 -> 24 on
  153002735006); raw world p95 unchanged on all. Hole region error median
  32.6 -> 31.1 milli-luma (sum 880 -> 851; better in 15, worse in 3 by at
  most 0.6); the hull-turn replays gain most: 120812713001 32.6 -> 26.5,
  225459033001 40.9 -> 28.0, 222433569001 32.2 -> 26.7. Holes filled +0.01
  points (median 1.16 -> 1.17%), left +0.01, guard 0.27 -> 0.24%. S/B
  sharpness outside the holes median 0.931 -> 0.926; the drop is on the
  hull-in-space replays where the stray outline added edges (120812713001
  0.974 -> 0.926, 225459033001 0.970 -> 0.937), elsewhere within 0.002.
- **Cost** (`--bench --frame`, f = 0.5, ms per eye): 120812713001:3 0.533 ->
  0.612, 230157175005:2 0.532 -> 0.603: src 0.133 -> 0.18 (the halo in the
  source pass; the 7x7 guard ring is not the cost: one load in place of 24
  times the same), holes 0.095 -> 0.12, splat +0.001.
- **Rig:** case 17 (a still hull at 60 m over the far plane moving 8 px, a
  2 px outline at far depth with the sky's vectors, a 58 m patch on the hull
  with the sentinel): the outline stays on the hull and nowhere in the sky,
  the sky moves as the sky, the patch with the hull. `FG_HALO 0` and
  `FG_SENTINEL_SURFACE 0` each fail it. Case 5's guard check now allows one
  read a row (the sky pixel 3 px out keeps its footprint read on itself).
  151 of 151.
- **Not flown.** Flight signature: in a ship turn with the hull against
  space, the eye dumps' S keeps the outline on the silhouette, nothing cyan
  in the sky beside it, no white patch on the hull; the synth line's `guard=`
  slightly lower.

### 2026-11-08: EDVR's base frame, not frame generation: the wing gap smears with FG off (eye dumps 131530778003, 132027190004, 132100543006, 132115012007; analysis only)

Pilot, Anaconda, Pimax Dream Air at 90 Hz on SteamVR, Elite at 45-50 fps,
frame generation off (F6 at 13:14:49; dump 131530778003 at 13:15:30; the
three later dumps are FG on, which does not touch B). The front V wings
(40-80 m) look smeared in motion; he circled the terrain in the gap between
the wing tips and the central block, and the wings' inner faces.

Environment: OpenXR (EDVR's runtime), left eye, render 2817x2448, output
3756x3264 (DLSS), split 10 m. 131530778003: ship turn 1.3-2.6 deg/frame
(the camera rows), head 0.02-0.22; terrain 25-52 render px/frame.

| Hypothesis | Signature | Measured | Verdict |
|---|---|---|---|
| H1 hull vectors carry the ship's motion | hull MV against the image's motion and the head-only motion | hull (zPred 20-120 m) is 98% path 11, engine record kind 1 (exact), in all four dumps; its MV equals the cockpit's (k2: hull 2.1, cockpit 2.3, terrain 52.5 px); a block search about the MV finds the best match within 1 px (the jitter) on all 15 pairs of each dump | ruled out |
| H2 DLSS unstable on the hull | P(k) against P(k-1) warped by MV, hull against the console | P residual over raw residual 0.2-0.4 on the hull, 0.25-0.3 on the console (luma 1.2-2.9 against raw 6-9). 132100543006 k6-15: hull P 8-21, but raw C 13-26 as well: the game's own lighting | ruled out |
| H5 terrain revealed at the hull edge has no history | the hidden sentinel (decision bit 16) along the silhouette; DLSS output there | a band along the hull's edge, as wide as the terrain's motion across it (20-30 render px), every frame: 131530778003 2-5% of the gap's terrain (the terrain slid away from the wing faces), 132100543006 25-37% (3200-5000 of 13000 px; 31-45k in the eye; terrain moving into the edge). The sentinel is right: last frame's footprint is the hull. P there is raw-like (Laplacian P/C 1.15-1.26 at the edge against 0.81-0.92 on open terrain). Terrain vectors in the gap are right to 1 px | confirmed: the smear |
| H5' the bright smudge right of the left wing tip | it in the raw input | it is in C00/C01 as in P00/P01: a terrain feature as the game drew it | not EDVR |
| H6 (new) terrain vectors zero for two frames | rows unchanged, then a 2-frame delta refused | k3-4 of 131530778003 and 132027190004, k2-3 of 132100543006: rows 0.00/0.05 deg, then 5.4-7.7 deg refused as another camera's (over 3 deg) and the gate carries the last good delta, the zero; terrain MV 0 while the image moved 44-54 px, 179k hidden, P residual 40-63. Each sits 2-3 frames after the dump started, with the dump's long frame (87 ms 13:15:30.865, 253 ms 13:20:27.555) | real mechanism; here a dump artefact, not shown in flight |
| H3 45 fps held two refreshes | runtime period and cadence | predicted period alternates 11.1/22.2 ms (13:14:34-13:15:19), 54-63 fps windows, 22-50 ms LONG FRAMEs every ~5 s; terrain moves 1.3-2.9 deg per game frame past a still hull | plausible, not measurable in a dump |
| H4 compositor smoothing warps the edge | the runtime's smoothing state | SteamVR global `motionSmoothing` false, Elite's `motionSmoothingOverride` 2; rotation-only reprojection moves hull and terrain together. Pimax Play's own smoothing is unknown | open; ask the pilot |

- ruled out: the hull's vectors (H1), because 98% of its pixels take the
  engine record's exact motion (`temporal_shader_source.h` 1059-1062, after
  the 10 m split at 984-1001) and match the image to 1 px in four dumps.
- ruled out: DLSS instability on the hull (H2), because its frame-to-frame
  residual is the raw input's in the same proportion as the console's.
- **The smear is a disocclusion, not a wrong vector.** Each frame the terrain
  that was behind the hull is new; `backgroundHistoryHidden`
  (`temporal_shader_source.h` 684-729, written at 1082-1085) correctly
  tells DLSS so, and DLSS shows one jittered sample there. The gap is edged
  by hull on three sides, so a large share of it is that band; at 45 fps the
  band is twice as wide as it would be at 90. Frame generation cannot widen
  or narrow it (B is the same). No sharpening or threshold change: the band
  is the content DLSS has not seen yet.
- H6's mechanism (`temporal_math.h` 515-547): a twin of last frame's rows is
  accepted as the view's own while the reference is own (the parked test
  runs only otherwise; the chooser's twin fallback, `temporal_pass.cpp`
  3146), then the catch-up delta is refused and the carried "last good" is
  that zero. Flight signature: the registration line's "nothing followed
  last frame's on N" and "dropped ... as another camera's" in a window with
  no dump; every window in this log that printed them held a dump.
- Crops: scratchpad `gap_P_grid.png`, `gap_C_grid.png`,
  `gap_flags_grid.png`, `path_132100543006_08.png` (white = hidden).
- ruled out: vendor smoothing (H4), because SteamVR's is off for Elite and
  the pilot's Pimax setup (SBoys3 on SteamVR) has no smoothing of its own.
- **Pilot's A/B of the DLSS preset (K, L, M, live):** K gives the best
  picture; the wing gap is not better under L or M. The band is DLSS's
  limit at this frame rate; it narrows only with a higher game frame rate.

### 2026-11-08: Phase 2.4, the gate and `auto` (built, not flown)

- **Key:** `fix.frame_generation = off | auto | on`. `off` as before (word 0,
  no synthesizer, no gate line). `on` is 2.3's forced Lock. `auto` runs the
  synthesizer and the slot; Lock engages only while the gate admits it, on
  top of its own conditions (reason `auto_gate`, tested last). The hotkey
  turns the key off, or back to the value it last turned off (auto first).
- **Where it runs:** the d3d11 half (`perf_monitor.cpp` `frameGenGateFrame`,
  once a frame), which already has the inputs: the Monitor page's GPU TIME and
  CPU TIME series (median and max over 2 s), the display and predicted
  periods, the synthesizer's window, Status.json and the journal. Lock's
  counts come back in the slot. The pure gate is `src/common/framegen_gate.h`,
  tabled in `framegen_synth_report_test`.
- **ABI:** word 17 is 2 (auto, closed) or 3 (open). A 2.3 runtime reads only
  1 as on, so auto reads off there; a 2.3 d3d11.dll reads `auto` in the ini
  as off. The slot grows to size 4 (`lockMode`). With no size 4 slot for 10 s
  of VR frames the gate holds the synthesizer and says so once.
- **Terms as built:** engage after 2 s of: runtime knows auto, every input
  measured (else `no_*`, fail-open), ship or SRV, real rate under 0.9 of the
  display's or a halved predicted period, GPU median plus the synthesizer's
  cost (both eyes, build plus finish) and the CPU median each under 0.85 of
  two periods, no cut for 2 s, 5 s since the gate last let go. Let go at
  once on a cut, a lost input or on foot, on `too_slow` (late_real over 5%
  of at least 10 S slots over two windows), and on `fits_full_rate` (GPU and
  CPU medians under 0.75 of one period for 3 s). `period_halved` and motion
  stay Lock's own reasons. Cuts: GuiFocus 5 and up, the jump tunnel, the
  temporal pass's camera gate, no pair for 250 ms, Lock's `no_projection`.
- **Deviation, context:** v1 admits a ship anywhere (supercruise included)
  and the SRV, not only surfaces as "### The gate" says: flights 6
  (supercruise) and 12 (Lock holds in a ship) flew them. On foot is not
  admitted (`on_foot`): unflown.
- **Deviation, no shadow-score term:** the synthesizer's f = 1 score runs only
  under the shadow key, so v1 has no score term; the design's "shadow score"
  engage and disengage terms wait for a score that runs with `auto`.
- **Choices made against the code:** the GPU figure is GPU TIME's (the
  application render, else the frame's work span), which may already hold
  the synthesizer's work, so adding its cost again errs safe. Elite's real
  rate is the CPU series' count over 2 s. `too_slow` uses S slots for pairs.
- **H1 instrument:** `frame generation gate: state= reason= mode= real_fps=
  period_ms= gpu_ms=med/max cpu_ms=med/max synth_ms= budget_ms= margin_ms=
  late_real= context= cut= lock=` once a second (auto and on), the edges in
  auto. Never admitted reads `state=waiting reason=<why>`; `--health` says it.

### 2026-11-08: Phase 2.4, the synthesizer's cost with the gate closed (built, not flown)

- **Measured (flight log, this build's parent):** the synthesizer cost
  1.1-1.5 ms of GPU a game frame (both eyes) and ran on every frame in
  `auto` with the gate closed, and with `fix.frame_generation = off` under
  the shadow key, where nobody shows S. EDVR's total was 8.5-10 ms against
  5.3-5.5 ms in v0.21.1; about 2 ms of that is upstream rc.7 changes, not
  frame generation. The gate line read `state=waiting
  reason=over_budget_gpu ... gpu_ms=17.68/21.26 synth_ms=1.58
  budget_ms=18.89 margin_ms=-0.37`: the synthesizer's own cost pushed Elite
  under 90 fps and made the gate want frame generation.
- **Fix 1 (`a89c7834`):** in `auto` with the gate closed the treat builds S
  once per eye per one-second synth window (the first call after each
  report), so `synth_ms` stays measured; the other eye-frames dispatch
  nothing and keep every resource. `on`, `auto` admitted, the shadow key and
  the eye dump build on every frame. The f = 0.5 S needs no history, so
  there is no warm-up: Lock's delivery clears on the first consumed pair and
  its 1 s dwell runs from there. Lock's `no_s` is now a blocker only once
  the gate admits, so a closed gate reads `auto_gate`.
- **Fix 2 (`55ae3107`), the double count:** the treat lies inside both GPU
  TIME sources (the application segment closes after the treat; the work
  span ends after the door), so the GPU median already held the synthesizer
  when it ran on most frames, and "adding its cost again errs safe" (gate
  entry above) counted it twice. The gate now takes `synth_ms` out of the
  median when more than half the window's frames built S, and adds it back
  once. `gpu_ms` stays raw; `margin_ms` is corrected.
- **Confirm in flight:** with the gate waiting, the census's `frame
  generation synth` section runs twice a second (one call per eye: about
  0.02-0.05 a frame at 90-45 fps) and the synth
  report says `frames=2/0` a second; `synth_ms` stays a number (not `-`,
  not `no_synth_cost`); the runtime's lock line reads `auto_gate`, not
  `no_s`. EDVR's GPU total drops by about the synthesizer's cost.

### 2026-11-12: the frame recomputed from the raw logs (no new flight)

Sources: the six same-spot profiles of 2026-11-11 21:18-21:23, the census
and `native benchmark` lines of that log and of the release's
(`edvr_gfx_20261110_210546.log`, v0.22.0), the runtime log's windows.
1878x1632 to 3756x3264 per eye, RTX 5080, SteamVR/OpenXR.

- **How to read a profile.** A pass's ms is the GPU clock between two
  boundaries, so the passes tile the frame period: their sum cannot show
  idle. Work the hooks do not see (NVIDIA's evaluate, EDVR's raw-call
  passes) is an empty segment, carried into the next pass and listed as the
  previous pass's `gap_after_ms`. The 7.2-7.6 ms of "gaps" a frame are
  work: 5.2-5.3 ms is the two DLSS evaluations (the segment before
  `f0d765e7`, 2.37 ms an eye at its floor), about 0.9 the HDR HUD seeds
  (before the hud_overlay passes), the rest EDVR's raw passes.
- **FG off, hull layer on, 63-67 fps (15.0-15.9 ms):** EDVR about 9.5-10 ms,
  the game about 5.5-6. EDVR by its census (21:19:34, per frame): DLSS M
  5.1-5.5; UI layer composite with the HUD's SMAA 1.08-1.16; HDR HUD
  depth-stencil seed 0.73-0.79 (4 a frame, 0.19 each, target 4695x4080
  D32S8, 153 MB; one of each eye's two is a stale re-seed); UI layer
  re-issues 0.2-0.4; UI resolve 0.34-0.42; UI depth coverage 0.32-0.38;
  motion prep 0.26-0.34; engine velocity 0.12-0.14; holograms 0.12; hull
  layer 0.97-0.98 (its own line). The HUD machinery is 1.1 ms an eye-frame
  median, 1.8 p95 (`ui quality: layer:` line).
- **The release build is the same:** v0.22.0, preset M, 68-78 fps, census
  EDVR 8.5-9.0 ms a frame (upscaler 5.0-5.2, UI composite 1.06-1.11, seed
  0.70-0.79). The pilot's 12-13 ms there is 13.2 ms (76 fps).
- **Preset M against K, same session, in a ship (release log):** upscaler
  3.78-3.86 ms with K (21:11:48-21:13:48, 79-88 fps), 5.0-5.2 with M (from
  21:14:05, 70-78 fps): M costs 1.25 ms a frame more, about a third, not 5%.
- **`ui_quality` 125 against 100 (release log, changed at 21:15):** UI
  composite 0.65-0.70 to 1.08-1.14, seed 0.47-0.65 to 0.70-0.79: about
  0.7 ms a frame.
- **Another GPU client takes slices.** The DLSS segment is bimodal in all
  six captures: 2.34-2.40 ms in 60-70% of 720 samples, 3.0-3.5 in 21%. 21%
  is the segment's share of an 11.1 ms display period: one slice of
  0.6-1.1 ms a period (the compositor's, most likely). With FG on the
  profiler's own delay loop at the frame start runs 1.55x slower (2.6 ns
  an iteration against 1.6-1.7, three pairs): about 2 ms of other work
  lands there. So "the game's passes cost more with FG" (entry below) is
  not established as the game's own work: at least part is this
  contention, placed at the same point of every locked frame.
- ruled out: the profile's gaps as GPU idle, because their largest part
  sits in the fixed place of the DLSS evaluation with its fixed floor.
- ruled out: the unknown-shader draw at the desktop size after eye 1 as a
  mirror cost: it carries EDVR's raw passes (0.18 ms median FG off, 1.03
  with FG on: the synthesis's unseen part).

### 2026-11-11: FG's cost, same spot (build 9dc9bd31, HMD 0.5, preset M)

Two GPU pass profiles 8 s apart, head still over a planet, hull layer
separate: `gpu_pass_profile_20261111-211850` (FG on) and `-211858` (FG off).
A real frame: 19.08 ms against 15.40 (+3.7). EDVR's passes +1.35 (52 calls
against 38: the synthesis, the world copy, the hull in S). The game's own
passes +2.3, mostly where its call counts rise: probe_cube 60.7 calls against
38 (+0.50 ms), other 174 against 134 (+0.41), compute +0.77, post +0.23; the
G-buffer, shadows and forward passes are flat. Read: the game spreads
time-sliced work (probe faces, streaming) over its real frames, so at 45 fps
each frame carries more of it; per second the GPU does less (0.86 against
1.0 of a second). Not FG's overhead.
- ruled out: FG slowing the game's own passes, because the passes whose call
  counts are flat (G-buffer +0.09, shadows +0.03, forward +0.03) cost the
  same with FG on.
- The pilot's monitor at the spot: GPU 20 / CPU 10 ms with FG, 15 / 10
  without; hull layer off and FG off 13.5 / 8.5; release 0.22.0 12-13 / ~3.
  CORRECTED 2026-11-12: the release's ~3 ms was a menu scene; in a ship its
  log reads cpu p50 6.5-7.1 ms (`native benchmark`, 2026-11-10 21:11-21:15),
  ours 7.5-9.0 with the hull layer and FG on. The gap is 1-2.5 ms, not 5.5.
- An earlier pair (21:09 on, 21:16 off) read +5.2: two different scenes.
- A second same-spot pair (`-212101` on, `-212107` off): 18.52 against
  16.13 (+2.4); EDVR +0.83; compute +0.71, probe_cube +0.32 (52 calls
  against 36), post +0.25, other +0.23 (167 against 150); G-buffer +0.03,
  shadows +0.01, forward +0.02. Same pattern.
- A third pair (`-212303` on, `-212309` off): 18.22 against 15.48 (+2.7);
  EDVR +1.09; compute +0.73 (45 calls against 42), other +0.46, probe_cube
  +0.25 at the SAME 34.6 calls, post +0.13; the flat-call passes +0.05 or
  less. So the game's extra is not only more calls: some time-sliced passes
  do more per call (larger dispatches or faces), as a per-frame time budget
  would. Not excluded: the compositor's 90 Hz work pre-empting long compute
  passes and stretching their timestamps; the discriminator is the
  dispatch sizes, which the profile does not record.
- At a station (`-212627` FG on, `-212635` off, 8 s apart): 20.78 against
  21.38, but the forward passes jumped in the off capture (236 calls against
  161, 4.06 ms against 0.47: the scene changed, the pilot was attacked).
  Without them: +3.0; EDVR +1.15, compute +0.97, probe_cube +0.39, the rest
  +0.15 or less; G-buffer, shadows, lighting volumes flat. The later pair
  (`-212722`, `-212727`) is the attack (Elite 42 ms a frame): not used.
- Busy scenes (`-213447`/`-213455` a carrier fight, `-213740`/`-213747`
  docked): a real frame 51-84 ms, Elite 41-73 of it, EDVR 9-10. Shadows
  18-25 ms, the G-buffer 13-32 ms (5.7-6.2 ms an eye docked, 414 draws, 0.78 M
  triangles). The shadows are one set a frame, not one an eye: four 512x512
  local maps (1.4-3.1 ms each with 38-125 draws, 0.2-0.6 M triangles) and
  four 3072x3072 cascades (up to 5.8 ms, 1097 draws, 1.6 M triangles); over
  the planet the same eight passes cost 0.02-0.05 ms each. Two local maps
  with identical draw lists show in some frames (two lights over the same
  objects), not in all. Vertex-bound game geometry: no FG or hull layer
  lever; the game's model and shadow settings are.
- ruled out: per-eye shadow maps in VR, because each frame has one set of
  the eight shadow passes, unpaired by eye.

### 2026-11-11: the own hull as its own layer in S (built, not flown)

The hull layer's separate mode (`docs\research-own-hull-layer-2026-11-08.md`,
section "Frame generation integration") no longer stands aside under frame
generation. With it on, S is built from the world-only eye (the upscaler's
output, or a copy of the submitted eye taken before the hull composite when
B's door finishes S), and the hull layer is drawn over S right after the
resample, moved by S's head step alone: the hull edge has no disocclusion in
S. The module gained one hook, `FrameGenSynthFrame::overlay`, called after
the resample inside the build's timer, so `synth_ms` carries it; the report
lines are unchanged. The evidence is the hull layer's 30 s line `hull layer:
framegen: ... hull_in_s=`. Flight: FG on over a planet, no hull-edge smear
in a head and a ship turn.
### 2026-11-07: Phase 2.3, flight 11, the first Lock flight (same build)

`fix.frame_generation = on`, about 12 minutes. Pilot: "a slight flicker",
and "a feeling of blur without blur" (sharp frames, smeared motion): read
as the B replays (303 of 355 S slots show B again, turned only by the
compositor) and the once-a-second engage/disengage cadence; to be compared
once S fills the slots.
`--health`: lock PARTIAL.

- **Lock cycles:** 101 engages and 101 disengages; engaged 8 s in all. Each
  engage lasts about 80 ms (3-4 S slots), then `disengage reason=no_s`,
  then a 1 s dwell, then engage again. The flicker is that cycle.
- **In every engaged burst the first S slot shows S and the rest replay
  B** (`s_from=1/3`, 52 S shown / 303 B replayed in the flight), while the
  transfer line says S was produced and consumed every frame (`produced=
  consumed`, `s_busy=0`, `s_late=0`, pose from the runtime).
- **Hypothesis (not tested): the S slot looks for the wrong pair's S.**
  The S slot advances the session's frame sequence by one (2.3's report),
  and the consumed S is matched by sequence, so after the first S slot the
  sequence the slot asks for and the one S carries differ by one.
  Signature: a lock-side count of "S consumed but for another sequence",
  with the two sequences, nonzero exactly from the second engaged slot.
- **Also:** `late_real` 1 in the whole flight (Elite fits two periods);
  S slot wait 7-9 ms; `period_halved` blocked engaging 349 seconds-lines:
  the runtime's period was 22.2 ms in 16 of 60 metrics samples, so SteamVR
  still halves at times.

### 2026-11-07: Phase 2.1, the turned score and the VRAM cut (built, not flown)

- **The compositor's turn, scored (the flight 7 discriminator).** At B the f = 1
  S keeps the head rotation it was built for (`rs`, B's eye space to S's, the
  extrapolated Hd_B^T) and the tangents it was built with. At C the true head
  rotation B -> C is C's own backward delta (x_B = Hd_C x_C + th), so a C-cell
  direction reaches the S eye as `kept * Hd_C`: that matrix is the residual
  (`frameGenSynthResidual`), the identity when the head did what S was built
  for. The score shader rotates each C quarter-cell's direction by it,
  projects with the kept tangents, reads the kept S quarter luma bilinearly
  and scores that against C's luma by B's class of the cell; a direction
  outside S is not scored and is counted (`out=`). The raw per-class fields
  are unchanged; the line gains ` turned: far=... cockpit=... world=...
  rot_res_deg=<median>/<max> out=<%>` (711 characters at the widest, one line).
  `edvr_log --health` says the world p95 raw against turned. Rig: a far scene
  from a head turning at a changing rate scores 0.061 raw and 0.003 turned;
  not applying the residual or applying it transposed each fail that case.
  Flight to run: world/cockpit/far turned against ex2dh's 17-34 / 0-1.
- **VRAM, per eye at 2817x2448 render, 3756x3264 output (RGBA16F S):** the
  synth held 20 bytes per render pixel (dilated depth 4, key 4, source 4, read
  position 8) = 138 MB, S 98 MB, the quarter grids 4 MB: 240 MB. Now 12 bytes:
  the dilated depth is the 3x3 nearest taken where it is read (no texture), the
  read position is an R16G16_FLOAT offset from the S pixel centre, and the key,
  source and offset textures are scratch rebuilt by every call, so the two eyes
  share one set (6 bytes per eye amortised): 185 MB per eye with a set each,
  144 MB per eye shared. S stays per eye (the shown f = 0.5 S must outlive the
  call in a later phase); sharing it across the eyes would take another 49 MB
  per eye. Offsets are exact to 1/16 px up to 256 px, 1/8 px up to 512, 1/2 px
  up to the clamp at 2000 px; past it the read is clamped (wrong) and counted
  (`clamp=` of the render pixels). Cost: the 3x3 depth is re-read per use.

### 2026-11-07: Phase 1, extrapolation sets built (not flown)

- **Built:** the scorer keeps A's own DLSS vectors (a quarter-grid copy, with
  the frame before's depth where each vector points) and forward-splats A's
  cells by them into a key grid, a second pass writing each winner's cell
  index. Two variants, scored per class against A's luma at the B cell's
  centre carried back by its winner's own motion (bilinear, not the
  winner's exact cell: the 4-cell footprint puts the winner up to a cell
  away, which alone costs 0.04 luma on a zooming plane):
  - `ex2d`: constant screen velocity, `pB = pA - mvA`.
  - `ex3d`: constant view-space velocity, `P_B = 2 P_A - P_prev`, `P_prev`
    the point at A's vector target at the frame before's depth (both seen
    through A's tangents), projected through B's. No head or ship rotation
    is applied; far cells take the screen rule.
- **Also:** `rowsoff_world` / `rowsoff_cockpit`, the share of cells, of those
  where either landing is inside the image, where B's true rows and the
  DLSS vector land more than 4 px apart (one inside and one outside counts).
  The vector pass's `decisionPath` is not readable without the trace buffer,
  so it is not logged.
- **Log:** a second line, `frame generation (shadow) extrapolation:
  ex2d=n far/cockpit/world ex3d=n far/cockpit/world rowsoff_world=%
  rowsoff_cockpit=%`, right after the report (which is at the widest a line
  takes). `edvr_log.py --health` reads it as optional.
- **WARP rig:** a plane scrolling at 8 px a frame with exact vectors: ex2d
  and ex3d mean error 0.0000, holes 48 (the leading two columns); an
  approaching plane (60, 50, 40 m): ex3d 0.0054, ex2d 0.0445, no-warp 0.1477.
  A flipped vector sign in the extrapolation fails the scrolling case.
- **Next:** fly it; compare ex2d, ex3d and mv (B's own vector) per class.

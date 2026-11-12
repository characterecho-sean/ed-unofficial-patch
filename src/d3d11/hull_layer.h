// The own hull as a private layer, Step 1 v2: a lit copy of the own ship's G-buffer, written into the eye dump only
// (diag.hull_layer, TEMPORARY; docs\research-own-hull-layer-2026-11-08.md "Step 1 v2 build"). Nothing the game draws or
// presents changes: every draw below is issued once more, after the game's own issue, into EDVR's private targets,
// and the game's bindings go back before the next game call.
//
// Per eye, inside the eye pass:
//   1. The seed: the eye's first hull-family G-buffer draw (VS 66DE, 61AE, AACF, DE54) after the cockpit shell (VS
//      BFE5) into the same eye depth. Its instance window (the stride-8 instance stream, record index first) and the
//      pool (VS t33, 336-byte records, position at bytes 16-27) are copied, and one compute dispatch marks every pool
//      record within 120 m of the seed records' centroid in the eye's own mask (one uint a record). Since the rig seed
//      (2026-11-10, docs "Rig seed") the seed records are the rig probe's own slots this frame (hull_rig_probe.h) and the
//      radius their reach plus 30 m (at most 120 m); the draw's instance window only when the pool holds no own slot. The mask's count
//      is read back a few frames later, never with a stall. Needs engine motion's patched shaders (part of fix.temporal_aa):
//      without the slot target at MRT6 on the seed draw the eye-frame does nothing (no-slot-target).
//   2. The private G-buffer: from the seed until the eye's G-buffer ends (its depth copy DEF19B03/CB95394B, or the next
//      eye's shell), every hull-family G-buffer draw is issued again into private RT0-3, a private slot target at RT6
//      (cleared to -1, no slot) and a private depth. Then a full-screen filter pass clears RT0-3 and the depth wherever
//      the private slot names a record outside the own mask (neighbour ships), and a count pass counts those pixels
//      (since the Station fixes, 15 below: only for re-issues the keep-own VS could not take).
//      The hull decals (BBE5/DB3E) are not re-issued.
//   3. The screen-space stages after it: the depth copy, the shadow mask (53211E8C with 7EAC7196, then B403F48C) and
//      every lighting draw (VS 7E38A6AA, 8289669D, 963B52C7, 24DE25E4, B121A79E, 0357BBB2, any PS). Every bound target
//      is given a private counterpart, and every PS resource whose texture has one is swapped for it, so the stage
//      reads the private G-buffer, depth, depth copy and shadow mask. A draw that writes the eye's HDR between the
//      shadow mask and the sky (F8FA801F) with another VS is logged as an unknown lighting pair.
//   4. On the frame an eye dump arms (hullLayerArmDump; the hotkey, or the layer's own auto dump), the private HDR and
//      the game's HDR after the eye's last lighting draw, and the private depth after the filter, go to
//      edvr_logs\eyes\eye_<stamp>_HullHdr.bin, _GameHdrLit.bin and _HullDepth.bin (eye 1: _e1), EDVRTEX1.
//
// Step 2 (diag.hull_layer = separate; "Step 2 build"): everything above, and the own ship is taken out of the game's main
// G-buffer, so the terrain behind it is drawn. THE OWN SHIP IS THEN NOT SHOWN until the composite (Step 3) lands.
//   5. The seed runs before the seed draw itself is issued (hullLayerBeforeMainDraw), so the own mask exists for it.
//   6. From the seed to the end of the eye's G-buffer, every hull-family draw and hull decal (VS BBE5) that engine motion
//      substituted is issued with the own-discard variant of its patched pixel shader (engineVelocityOwnDiscardPs), with
//      the eye's own mask at PS t127: a pixel whose record is own is discarded. The patched shader and the game's t127 go
//      back right after the game's issue (hullLayerAfterMainDraw), before the private re-issue, which draws normally.
//      No seed, no slot target, no variant: the draw is issued as the game made it (the ship stays).
//   7. The own decals are re-issued into the private layer too (filtered like the hull), when they export the slot.
//   8. The dump adds the game's G-buffer RT0 and main depth after the eye's G-buffer (_GameGbuf0, _GameDepth).
//   8a. The ambient occlusion (both modes; "AO on the hull layer"): the game's AO chain (three compute steps an eye after
//      its depth copy) is dispatched once more after each game dispatch on the private depth copy and RT0, into private
//      copies of its targets, and its apply draw (DEF19B03/06CD42A9) is re-issued into the private RT2 before the shadow mask.
//   The own forward draws after the lighting (VS 88DCF116, 025B4B9F, 0B5981F2, F464E27C, 6DB587D2, 2B3F53DD) are not
//   engine-motion families: they still draw into the main pass (an open item of the doc).
//
// Step 3 (separate; "Step 3 build"): the hull put back after the upscaler.
//   9. Right after the game's tonemap of an eye (admitted by structure, tonemap_admit.h, and by the identity of the HDR it
//      reads: the eye's G-buffer RT3), the same draw is issued once more with that HDR slot bound to the private HDR and RT0
//      to a private 8-bit target cleared to transparent black. A compute pass then keeps it where the hull is shown (the
//      private depth set and at least as near as the game's main depth, reversed Z, so the cockpit stays in front) into the
//      eye's cover (alpha 1 there, 0 elsewhere) and the private depth beside it.
//  10. In the temporal pass, right after the upscaler wrote the eye (hullLayerCompositeEye), the hull layer's own TAA at
//      output size: this frame's jittered cover (a 3x3 Gaussian at the output pixel's unjittered position), the history
//      reprojected by the head's motion alone (the pass's head delta and translation, at the cover's depth) and clamped to
//      the 3x3's range, blended 0.1; then composited over the upscaled eye (premultiplied), before the UI layer's door.
//  11. Safety: the main pass discards the own ship only on a frame decided at the frame boundary before it: the VR profile
//      and not on foot, an own mask read back and of at most 400 records, and no miss (an eye that
//      seeded but was not lit, tonemapped and composited) in the last 16 frames. On every other frame the layer still runs
//      and its composite is made but not applied, so a miss is seen without the ship being lost.
//
// Dump v2 (both modes unless said; docs "Dump v2"):
//  12. GPU time by stage: one frame in eight brackets each of the layer's own items (re-issued draws, passes, dispatches) with a
//      timestamp pair, summed by stage; the 30 s line "hull layer: gpu_ms" gives the median a timed frame, with the items and CPU ms.
//  13. The dump frame's lighting windows (shadow mask to sky, per eye): every draw writing the eye's HDR and every stencil-only draw,
//      with its pair, instances and whether the layer re-issued it, to eye_<stamp>_HullLights.json.
//  14. Separate mode: the dump's capture frame is a reference frame (the main pass keeps the own ship; the layer runs; the composite
//      is made, not applied), and the TAA sequence: eye 0's cover, cover depth and composite for 16 frames from it, its constants
//      and frame 0's history, to eye_<stamp>_HullSeq_<nn>_*.bin and _HullSeq.json (tools\hull_taa_sim.py).
//  The eye dump also lifts its census to the line ceiling (draw_census.cpp) and arms the GPU pass profile for 4 frames.
//
// Frame generation (separate; docs "Frame generation integration", 2026-11-11): the mode no longer stands aside. The synthesizer's S
// of an eye-frame is built right after its composite, in the same treat, from the world-only eye (hullLayerSInput: the upscaler's
// output when the composite went onto the submitted eye; the submitted eye itself when its hull write waits for S, sFirst and
// hullLayerCompositeFinish; else a copy of the eye taken before the composite, keepWorld), and the hull layer (the TAA result,
// sharpened as the composite does) is drawn over S after the resample, moved by the head's step B -> S alone (hullLayerCompositeS,
// over the S tiles the hull can reach, else the whole eye). The real frame keeps the composite's pixels. The 30 s "framegen" line
// counts it.
//
// Station fixes (both modes unless said; docs "Station fixes"):
//  15. The re-issue into the private G-buffer (2 and 7) binds the keep-own variant of the draw's vertex shader
//      (engineVelocityKeepOwnVs) with the eye's own mask at VS t127: a foreign instance (the station, another ship: the same
//      families) is culled before it is rasterised. The private slot target, the filter and the count run only on an eye-frame
//      with a re-issue that could not take the variant (from that draw on); its private copy goes after 120 eye-frames unused.
//  16. The busy governor (temporary): an eye drawing more hull-family G-buffer draws than kBusyDraws (kBusyDrawsCulled when every
//      re-issue was culled in the VS) puts the layer to sleep for the scene -- only the count runs, the game draws the ship -- until
//      120 calm frames; decided at the frame boundary. A dump wakes it (a warm-up frame, the capture frame, separate mode's TAA
//      sequence) and it sleeps again after.
//  17. Two station light volumes join the lighting (A1B7CFCD, CE24A739). An HDR writer in the lighting window is said as an
//      unknown lighting pair only when a later lighting draw follows it; a forward draw (depth writes on) closes that window.
//  18. Stages and their private copies only at the eye's render size; the lighting re-issue leaves the scratch R16 at RT1 unbound.
//
// Cost with the key off: one bool load per draw (hullLayerWanted). VR owner context only.
#pragma once

#include <d3d11.h>

#include <cstdint>

namespace edvr {

class Config;

namespace detail {
extern bool g_hullLayerOn;
extern bool g_hullLayerSeparate;
}  // namespace detail

// The draw path's one load.
inline bool hullLayerWanted() { return detail::g_hullLayerOn; }
// diag.hull_layer = separate: the draw path's pre-draw load (only read while hullLayerWanted).
inline bool hullLayerSeparateWanted() { return detail::g_hullLayerSeparate; }

// diag.hull_layer = off | dump | separate. Live; a change is said once.
void hullLayerConfigure(Config& cfg);

// Separate mode, before the game's own issue of an owner-context draw (after engine motion's substitution), with the
// draw's instance count and StartInstanceLocation: at the eye's seed draw the own mask is made; inside the eye's G-buffer
// window a hull-family or hull decal draw gets the own-discard variant and the mask at PS t127. True: the caller calls
// hullLayerAfterMainDraw right after the game's issue (and before hullLayerBegin), which puts the shader and t127 back.
bool hullLayerBeforeMainDraw(ID3D11DeviceContext* ctx, UINT instances, UINT startInstance);
void hullLayerAfterMainDraw(ID3D11DeviceContext* ctx);

// Right after the game's own issue of an owner-context draw, with the draw's bindings still bound, its kind ('D', 'I',
// 'N', 'X'), vertex or index count, and instance count and StartInstanceLocation (1 and 0 for a draw that is not
// instanced). True: the private targets and resources are bound and the caller issues the same draw once more (raw),
// then calls hullLayerEnd, which puts the game's bindings back. False: nothing was bound (the layer may have run its own
// passes, with the game's state put back).
bool hullLayerBegin(ID3D11DeviceContext* ctx, char kind, UINT count, UINT instances, UINT startInstance);
void hullLayerEnd(ID3D11DeviceContext* ctx);
// Between hullLayerBegin and the re-issue of night vision's matched draw: EDVR's night-vision state was bound for it
// (nightVisionReissueBegin). Counted for the state line's night_vision=<re-issued>/<with EDVR's state>.
void hullLayerNoteNightVisionState();

// Right after the game's own issue of an owner-context Dispatch (exposure_fix.cpp hookedDispatch), with its bindings still
// bound and the bound compute shader's content hash: a step of the game's AO chain (kAoCs in hull_layer.cpp) whose eye has a
// private G-buffer gets private UAV targets and its private-copied inputs. True: the caller dispatches the same groups once
// more (raw), then calls hullLayerEndDispatch, which puts the game's compute views back. Both modes (dump and separate).
// Also after an owner-context DispatchIndirect (hookedDispatchIndirect): the game's reflection pass (kReflCs), separate mode
// only, re-issued into the private HDR; the caller then dispatches the same indirect arguments again.
bool hullLayerBeginDispatch(ID3D11DeviceContext* ctx, uint64_t csHash);
void hullLayerEndDispatch(ID3D11DeviceContext* ctx);

// From the clear hooks (owner context, the game's own clears): a clear of a target that has a private counterpart is
// made on the counterpart too, and the value an eye depth is cleared to is remembered for the private depth's clear.
void hullLayerNoteClearRtv(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, const float colour[4]);
// From the copy hooks (owner context, after the game's own copy): a whole-texture copy whose source has a private
// counterpart is made between the counterparts too (the destination's made on first use). The game copies G-buffer RT0
// before the depth copy and the hull decals read that copy as their base; without this the layer's decals read the
// main pass's RT0, which no longer holds the own hull, and came out dark (2026-11-08 23:00).
void hullLayerAfterCopy(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src);
void hullLayerNoteClearDsv(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, UINT flags, float depth, UINT8 stencil);

// The eye dump's arming (temporal_pass.cpp beginEyeRun): the next whole frame is captured. A no-op with the key off.
void hullLayerArmDump(const wchar_t* stamp);

// Step 3 (separate): what the temporal pass hands the composite, right after the upscaler wrote the eye.
struct HullLayerCompositeInputs {
    const void* source = nullptr;          // the submitted eye texture (identity): the tonemap output it should be
    uint32_t regionX = 0, regionY = 0;     // the eye's region in it
    uint32_t renderW = 0, renderH = 0;     // the region's size: the render size
    uint32_t outW = 0, outH = 0;           // the upscaler's output size
    ID3D11Texture2D* output = nullptr;     // the eye at the output size, R8G8B8A8 family, outW x outH: the composite lands here
                                           // (outputSrv a UNORM view of it)
    ID3D11ShaderResourceView* outputSrv = nullptr;
    const float* tanNow = nullptr;         // l r t b, jitter excluded
    const float* tanPrev = nullptr;        // last frame's, or null when the pass has no continuity (the layer's history restarts)
    const float* headDelta = nullptr;      // the head's rotation from this frame's view to last frame's, 3x3 row-major; null: none
    const float* headTrans = nullptr;      // its translation term (metres), used when useTrans
    bool useTrans = false;
    float depthA = 0.0f, depthB = 0.0f;    // the scene depth's encoding: written = A + B / metres
    float jitterX = 0.0f, jitterY = 0.0f;  // this frame's jitter, render pixels
    // Where the composite lands: the submitted eye after the UI resolve (true), or the upscaler's output before it (false,
    // only when no resolve can run). The resolve bounds its output by the raw input, which has no own hull, so a hull
    // composited before it is held to the background (the 21:45 flight).
    bool afterUiResolve = false;
    // The eye's own DLSS inputs (render size, the region's): colour, reversed-Z depth, motion (R16G16_FLOAT, render px,
    // current -> previous). Read only by the variant bench's second DLSS (TEMPORARY): the hull over this hull-free world.
    ID3D11Texture2D* dlColour = nullptr;
    ID3D11Texture2D* dlDepth = nullptr;
    ID3D11Texture2D* dlMv = nullptr;
    // Frame generation: S is built from this eye-frame and reads `output` itself (B's door finishes S, or no UI resolve ran): an applied
    // composite first copies `output` (the world without the own ship) for hullLayerSInput. One whole-eye copy an S build.
    bool keepWorld = false;
    // Frame generation, the S-first order (2026-11-11): with keepWorld, the caller builds S from `outputSrv` before anything else reads
    // or writes `output`, and then calls hullLayerCompositeFinish. An applied composite that goes straight onto the eye (direct) then
    // runs its TAA and tile list now and writes the hull onto the eye in the finish, so S reads the eye world-only and no copy is made.
    // Where it cannot (not direct: a dump or the TAA sequence, no UAV; the variant bench), it composites whole now with the copy.
    bool sFirst = false;
};

// Step 3: right after the upscaler wrote VR eye `eye` (temporal_pass.cpp). A no-op unless diag.hull_layer = separate. True: the
// hull's write onto `output` waits for hullLayerCompositeFinish (in.sFirst; `output` holds the world only until then).
inline bool hullLayerCompositeWanted() { return detail::g_hullLayerSeparate; }
bool hullLayerCompositeEye(ID3D11DeviceContext* ctx, int eye, const HullLayerCompositeInputs& in);
// The S-first order's second half, after S was built from the eye: the hull written onto the eye as the composite would have written
// it. A no-op (false) when hullLayerCompositeEye returned false for this eye this frame. Always call it when that returned true.
bool hullLayerCompositeFinish(ID3D11DeviceContext* ctx, int eye);

// Frame generation (separate mode), right before the synthesizer builds S of VR eye `eye` this frame, after hullLayerCompositeEye.
// `proposed` is the view S would read B through. Returns the view S must read: `proposed` itself when the composite has not yet
// written the hull onto it (sFirst), the world-only copy when `proposed` is the eye the composite wrote, else `proposed`; *overlay true when the hull layer is to be drawn into S (pass hullLayerCompositeS to the
// synthesizer). *overlay false (S warps `proposed` as before, the ship in it): separate mode off, no composite this frame, the main
// pass kept the ship, or no copy. Counted on the 30 s "framegen" line.
ID3D11ShaderResourceView* hullLayerSInput(int eye, ID3D11ShaderResourceView* proposed, bool* overlay);
// The synthesizer's overlay (FrameGenSynthFrame::overlay), right after its resample wrote S (w x h, the typed UAV s): the eye's hull
// layer moved by the head's step B -> S (x_S = rot x_B + t) and composited over S. False: nothing drawn (counted by reason).
bool hullLayerCompositeS(ID3D11DeviceContext* ctx, uint32_t eye, ID3D11UnorderedAccessView* s, uint32_t w, uint32_t h, const float rot[9], const float t[3]);

// Once per frame on the owner thread: the per-eye bookkeeping, the readbacks, the dump's write, the auto dump, the 30 s line.
void hullLayerFrameBoundary(ID3D11DeviceContext* ctx);

// The variant bench (hotkey.hull_bench, TEMPORARY; docs\research-own-hull-layer-2026-11-08.md "Variant bench"). One press runs
// every hull variant live at the same spot -- the original, the layer with its own TAA, without AA, (stage 2) a second DLSS
// on the hull -- and writes the image after the composite (8 frames an eye-0 variant, eye 1 and the headset frame once), the
// GPU ms and MB of each to edvr_logs\eyes\eye_<stamp>_Bench*.bin and _Bench.json (tools\hull_bench_report.py). Refused,
// with a toast and a line, unless diag.hull_layer = separate in the VR profile, frame generation off, the layer awake, no
// dump or eye run, and 4 GB free. A second press cancels. Everything below costs one bool load while no bench runs.
namespace detail {
extern bool g_hullBenchOn;
}  // namespace detail
inline bool hullBenchActive() { return detail::g_hullBenchOn; }
// The key (device_hook.cpp): starts a bench, or cancels the one running.
void hullBenchPress();
// Right after the hull composite (temporal_pass.cpp, outside its condition: in the original variants the layer sleeps), the
// eye's submitted texture at the output size (the eye at 0,0), the eye's frame values. dlss: the eyes' upscaler is DLSS.
void hullBenchCaptureEye(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* submit, bool dlss, uint32_t renderW, uint32_t renderH,
                         uint32_t outW, uint32_t outH, float jitterX, float jitterY, const float* tanNow, const float* headDelta);
// The headset frame (temporalPassCaptureFinalEye): the texture the runtime gets, its region (x0, y0, x1, y1) and flips.
void hullBenchCaptureFinal(uint32_t eye, ID3D11Texture2D* texture, const uint32_t region[4], bool flipU, bool flipV);
// A GPU time of kind 0 (the eyes' DLSS) or 1 (the hull's DLSS) around one evaluation; a handle, -1 when none.
int hullBenchTimeBegin(ID3D11DeviceContext* ctx, int kind);
void hullBenchTimeEnd(ID3D11DeviceContext* ctx, int handle);
template <class F>
bool hullBenchTimed(ID3D11DeviceContext* ctx, int kind, F&& f) {
    if (!detail::g_hullBenchOn) return f();
    const int t = hullBenchTimeBegin(ctx, kind);
    const bool ok = f();
    hullBenchTimeEnd(ctx, t);
    return ok;
}

void hullLayerShutdown();

}  // namespace edvr

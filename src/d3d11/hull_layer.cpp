// diag.hull_layer (TEMPORARY): the own hull as a private, lit layer -- Step 1 v2 in the eye dump, Steps 2-3 (separate) out of
// the main pass and composited back after the upscaler. hull_layer.h says what it does; docs\research-own-hull-layer-2026-11-08.md
// ("Step 1 v2 build", "Step 2 build", "Step 3 build") says what each must show.
#include "hull_layer.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>          // the reflection dump: CSGetConstantBuffers1, the offset cb1 is bound at
#include <dxgi1_4.h>          // the variant bench's VRAM sample (IDXGIAdapter3::QueryVideoMemoryInfo)

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/config.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"   // runtimeVrProfile: Step 3's discard is the VR eye pass's only
#include "binding_shadow.h"
#include "cs_stage_save.h"    // Step 3's dispatches: the compute stage put back as the game (or the temporal pass) left it
#include "dlaa.h"             // the variant bench's manifest: the eyes' DLSS preset and mode
#include "engine_velocity.h" // engineVelocityLiveNow, engineVelocityOwnDiscardPs and its mask slot (kEngineVelocityOwnMaskPsSlot)
#include "framegen_shadow.h"  // frameGenShadowOn: the variant bench stands aside while any frame generation key is on; the framegen line
#include "gpu_census.h"       // the variant bench: gpuCensusFrameSpan, the frame's GPU span
#include "hull_bench.h"       // the variant bench's manifest, file names and writer thread (TEMPORARY)
#include "hull_rig_probe.h"   // the rig probe: the own ship's pool slots from the engine's rigs, against the own mask (diagnostic only)
#include "journal_watch.h"    // the variant bench's aborts: the seat, the menu (no vehicle), the jump tunnel
#include "menu.h"             // the variant bench's toasts (menuShowNotice)
#include "native_on_foot.h"   // nativeOnFootActive: no discard on foot
#include "temporal_pass.h"    // temporalPassArmEyeDump: the auto dump arms the eye dump the hotkey arms; temporalPassFrameGenMode
#include "tonemap_admit.h"    // Step 3: the game's tonemap, admitted by structure
#include "vr_world_route.h"   // VrWorldInternalScope: the layer's own clears, copies and passes step past the game-facing hooks
#include "vscreen.h"          // the raw setters: the binding shadow keeps describing the game's bindings

namespace edvr {

namespace detail {
bool g_hullLayerOn = false;
bool g_hullLayerSeparate = false;
bool g_hullBenchOn = false;   // the variant bench runs (hotkey.hull_bench, TEMPORARY)
}  // namespace detail

namespace {

// The own ship's families (engine_velocity_families.h kFamilies: vs_66DE..., vs_61AE..., vs_AACF..., vs_DE54...).
constexpr uint64_t kHullVs[4] = {0x66DE2CADB1F4AE6Bull, 0x61AE8EB05FDC18DDull, 0xAACFDCF2FB9AD809ull, 0xDE545DC8EE4FBB87ull};
// The hull decals (engine_velocity_families.h: vs_BBE5 with ps_DB3E, the guarded overlay pair in the VR eye passes).
constexpr uint64_t kDecalVs = 0xBBE58E40FE88EC80ull;
constexpr uint64_t kShellVs = 0xBFE51414CC3024B4ull;     // the cockpit shell (engine_velocity_families.h)
constexpr uint64_t kTerrainVs = 0x72BDD292154158ADull;   // the terrain's colour pass (celestial_motion.h kCelestialPatchVs)
constexpr uint32_t kTerrainVsHigh = 0x6C320453u;         // the other terrain family, known by its prefix only (the ledgers)
constexpr uint64_t kSkyVs = 0xF8FA801F2CB1E27Cull;       // the sky, the first forward draw after the lighting (flat_projection_recipes.h)
// The lighting: the full-screen sun and ambient, then the instanced volumes (the doc's "The binding map", the mt= census).
// Then a station's lights (docs "Station fixes"; the 23:35 docking dumps): A1B7CFCD with PS 2DB678B6 (box volumes,
// 198-226 instances) and CE24A739 with PS 1F64463B (45-54 instances), drawn like the other volumes and missing from the hull.
// The last is the hangar's third box light volume (docs "Night vision state and the hangar light", 2026-11-10; dump
// 204100368002): A7339D1F with PS 93A4B9DA, 36 indices x8, drawn right after A1B7CFCD and CE24A739 and bound like A1B7CFCD's
// PS 6796562E draw (a light buffer at t0, the depth copy, G-buffer and HDR-sized inputs, the shadow map).
constexpr uint64_t kLightVs[9] = {0x7E38A6AA1269C901ull, 0x8289669D93A18C1Dull, 0x963B52C73B4143ACull,
                                  0x24DE25E496342EB8ull, 0xB121A79E457669E8ull, 0x0357BBB2DEE43C1Full,
                                  0xA1B7CFCD0BE7493Eull, 0xCE24A73943632F55ull, 0xA7339D1F8A5AC0D0ull};
// The game's night vision (2026-11-08 census 3 against 4: one draw an eye into the HDR, reading the depth copy and G-buffer
// RT0), re-issued like a light so the own hull gets it in separate mode, but not a light: it comes after both eyes' lighting
// and the sky, so it opens no lighting window and moves no stage eye (docs "Night vision state and the hangar light").
constexpr uint64_t kNightVisionVs = 0xFCF7BD2896751D96ull;
constexpr UINT kPoolSlot = 33;            // VS t33: the pool a pool draw reads
constexpr UINT kSlotTarget = 6;           // engine motion's MRT6 (dxbc_engine_velocity.h kEngineVelocityTarget): float2(2*slot+1, depth)
// The decal pair's PS t3: engine motion's snapshot of its own slot target (dxbc_engine_velocity.h
// kEngineVelocityOverlaySnapshotSlot; engine_velocity.cpp snapshotOverlayBase). Its guarded decal PS reads it for MRT6's depth only.
constexpr UINT kOverlayBaseSlot = 3;
// The eye's tonemap (2026-11-11 flight, the unmirrored-input log): PS D0A16B9E reads the LUT at t0, the HDR at t1 and, at t2,
// the game's bloom of its own (world-only, in separate mode) image: a quarter-size R11G11B10 target PS DED87960 wrote.
constexpr uint64_t kHullTonePs = 0xD0A16B9E55BF22CCull;
constexpr UINT kToneBloomSlot = 2;
constexpr uint32_t kRecordBytes = 336;    // a pool record (tools\eye_run_ledger.py REC)
constexpr uint32_t kMaxSeedInstances = 4096;
constexpr uint32_t kMaxRecords = 1u << 20;
constexpr float kOwnRadius = 120.0f;      // metres about the seed records' centroid (the five ledgers: object A at 120 m)
// The rig seed's margin past its records' reach (docs "Rig seed", 45 dumps of 2026-11-09): the own records lay at most 13-25 m
// past the rig records' reach (Anaconda 71-83 m against a reach of 54-61 m; the transport 17-28 m against 4-6 m), the nearest
// foreign record past reach + 30 in every dump. kOwnRadius stays the rig seed's ceiling.
constexpr float kRigMargin = 30.0f;
constexpr uint32_t kStatsBytes = 32;      // an eye's seed stats (the seed shader's comment says the words)
constexpr UINT kSrvSlots = 16;            // PS resource slots a stage is searched and restored over
// 48, not 32: the AO chain adds three private copies an eye (its deinterleaved depth, its HBAO slices, the 704x612 AO).
constexpr size_t kMaxMirrors = 48;
constexpr size_t kMaxViews = 128;
constexpr uint32_t kEvictFrames = 120;    // a private copy whose game texture was not seen for longer goes
constexpr uint32_t kRing = 4;             // readback staging buffers an eye
constexpr uint32_t kReadbackLag = 2;      // frames before a readback is tried (DO_NOT_WAIT)
constexpr uint32_t kLightSpan = 64;       // draws after the shadow mask that may pass with no sky before the lighting window closes
constexpr uint32_t kMaxUnknownPairs = 16;
constexpr uint32_t kMaxPendingPairs = 16; // an eye's HDR writers waiting for a later lighting draw before they count as unknown lighting
// Station fixes (docs "Station fixes"). The busy governor, a temporary safety: an eye drawing more hull-family G-buffer draws than
// the limit puts the layer to sleep for the scene (the game draws the ship), until kCalmFrames frames in a row are under it. 400
// while the re-issue needs the neighbour filter (some draw could not take the keep-own VS); 2400 when every draw was culled in the
// VS, since the re-issue then costs little (the station: 600-1,700 an eye, 7-15% of them the own ship's).
constexpr uint32_t kBusyDraws = 400;
constexpr uint32_t kBusyDrawsCulled = 2400;
constexpr uint32_t kCalmFrames = 120;
constexpr UINT kKeepOwnVsSlot = kEngineVelocityOwnMaskVsSlot;   // the keep-own VS reads the eye's own mask here
// kAutoMax 0: the auto dumps are off (the pilot, 2026-11-08 22:25: each dump's hitch in the headset is too disruptive);
// the dump_eyes key still arms one by hand. Raise it to bring them back for an unattended measurement flight.
constexpr uint32_t kAutoFrames = 600, kAutoMax = 0, kAutoOwnLo = 20, kAutoOwnHi = 200;
constexpr ULONGLONG kAutoGapMs = 60000;
constexpr ULONGLONG kReportMs = 30000;
// Step 3 (separate): the discard's safety. A mask past kOwnMaskMax records holds a station's or another ship's records
// within the radius (docking): the frame falls back. A miss (an eye seeded but not lit, tonemapped and composited) holds
// the discard off for kHoldFrames frames. 1500, not 400: the own ship's undrawn records near it (interior, LODs) put
// 260-700 records in the mask over terrain with no neighbour (ledgers 193029413006, 214124277001; 2026-11-08 21:40).
constexpr uint32_t kOwnMaskMax = 1500;
// 120, not 16 (2026-11-08 22:00): while textures streamed in, misses came every few dozen frames and each 16-frame hold
// ended in another lost frame, so the ship blinked. The layer still runs and is checked on held frames, so a hold only
// ends after 120 frames in a row without a miss. Any other fallback ending starts the same hold (decideNext).
constexpr uint32_t kHoldFrames = 120;
constexpr float kTaaCurrent = 0.1f;       // the hull layer's TAA: this frame's weight against the clamped history
constexpr UINT kTimerRing = 8;            // timestamp sets a timer keeps in flight
constexpr int kToneStage = 10;            // Pending::stage of the tonemap re-issue (not a Stage)
// Build A' (docs "Build A'", 2026-11-10): the layer's re-issued draws are scissored to a per-eye rectangle, the box of the
// shown cover (alpha > 0) of an earlier frame, read back without a stall and grown by the head's rotation since: at most 50
// render px a frame (motion.csv max 2.32 degrees), and never under 150 px (3 frames). Past kRectMaxAge frames, with no box
// since the last probe, or an empty box, the eye-frame is whole. Every kRectProbeEvery frames an eye-frame is whole, and the
// rectangle waits for that frame's box, so a part of the hull appearing far outside it (a hardpoint deploying) is found.
constexpr uint32_t kRectMarginMin = 150;
constexpr uint32_t kRectPerFrame = 50;
constexpr uint32_t kRectMaxAge = 8;
constexpr uint32_t kRectProbeEvery = 64;
constexpr uint32_t kBoxBytes = 16;        // an eye's box: max(65535 - x), max(65535 - y), max(x + 1), max(y + 1) of shown pixels
constexpr size_t kMaxScissorStates = 64;  // rasterizer clones with the scissor on, by the game's state

// The ambient occlusion chain (docs "AO on the hull layer"; census 3 of 2026-11-08 21:59, capture 220517898003): per eye, after
// the eye's depth copy, three compute steps make the 704x612 AO, and the apply draw (DEF19B03/06CD42A9, kAo) blends it into
// G-buffer RT2 before the shadow mask. Eye 0's steps run after eye 1's G-buffer. Each step is re-issued right after the
// game's with every texture that has a private copy swapped for it and its own targets given private copies, so the chain
// runs on the private depth copy and RT0. In step order (the cascade: a step runs only after the eye's steps before it).
constexpr uint64_t kAoCs[3] = {0xFB277B33F0865348ull,    // the depth copy deinterleaved into 16 slices of 176x153 (44x39 groups)
                               0x9347F8FC2DCE0248ull,    // the HBAO a slice (11x10x16): the slices, G-buffer RT0, the kernel buffer
                               0xD31E7812990B19A6ull};   // the slices put back together: the 704x612 AO the apply draw reads
constexpr int kAoSteps = 3;
// The game's reflection pass after the last light volume: two indirect dispatches over screen tiles that read the depth copy,
// the stencil and the G-buffer, sample the environment cubemap array along the reflection vector and a BRDF table, and add
// the result into the eye's HDR (u0) and an R16 target (u1) (shader dump 2026-11-10). It lights the hull wherever reflections
// matter (a station pad: the hull at 0.30-0.39 of the original without it, bench 065707/065728), so separate mode re-issues
// it like an AO step, into the private HDR, with the game's own tile list. The pair, in issue order.
constexpr uint64_t kReflCs[2] = {0x5998146D464F5C0Eull, 0xEB0245DE0BB23BB6ull};
constexpr int kReflSteps = 2;
constexpr uint8_t kAoApplyBit = static_cast<uint8_t>(1u << kAoSteps);
constexpr uint8_t kAoAllSteps = static_cast<uint8_t>(kAoApplyBit - 1u);
constexpr UINT kCsSrvSlots = 16;                               // CS resource slots a step is searched and restored over (it uses t0-t2)
constexpr UINT kCsUavSlots = D3D11_PS_CS_UAV_REGISTER_COUNT;  // u0-u7 (it uses u0)
constexpr size_t kAoOwned = 8;                                 // an eye's AO step outputs remembered this frame (it has three)

enum Stage : int { kCopy = 0, kMask = 1, kLight = 2, kAo = 3, kStages = 4 };
struct StagePair {
    uint64_t vs, ps;
    Stage stage;
    uint8_t bit;   // once per eye per frame
};
// The doc's binding map ("The binding map"); flat_forward_light.h, flat_inert_pairs.h and flat_deferred_camera.h name the same pairs.
constexpr StagePair kStagePairs[] = {
    {0xDEF19B035D5EDEDCull, 0xCB95394B50D737D6ull, kCopy, 1},   // the depth copy into f39: the end of the eye's G-buffer
    {0x53211E8C072CD02Eull, 0x7EAC71963E66C5FEull, kMask, 2},   // the shadow mask, first draw
    {0x53211E8C072CD02Eull, 0xB403F48CB35D9739ull, kMask, 4},   // ...and second
    {0xDEF19B035D5EDEDCull, 0x06CD42A941C4C7C9ull, kAo, 8},     // the AO apply into G-buffer RT2 (after the AO chain, before the shadow mask)
};

// How an eye's private G-buffer window closed.
enum End : int { kEndCopy = 0, kEndShell, kEndLight, kEndFrame, kEnds };

bool isHullVs(uint64_t vs) {
    for (uint64_t h : kHullVs)
        if (h == vs) return true;
    return false;
}
bool isLightVs(uint64_t vs) {
    for (uint64_t h : kLightVs)
        if (h == vs) return true;
    return false;
}
bool isTerrainVs(uint64_t vs) { return vs == kTerrainVs || static_cast<uint32_t>(vs >> 32) == kTerrainVsHigh; }

// ---- the shaders (compiled at the first seed with d3dcompiler_47.dll, the gpu_pass_delay_shader.h way) ----------------

// Compiled at run time; tools\hlsl_embedded_check.py compiles them in the build (build.bat gate), one line each:
// hlsl-check: kSeedHlsl hull_layer_seed_cs cs_5_0
// hlsl-check: kCountHlsl hull_layer_count_cs cs_5_0
// hlsl-check: kFilterVsHlsl hull_layer_filter_vs vs_5_0
// hlsl-check: kFilterPsHlsl hull_layer_filter_ps ps_5_0
//
// The seed: the seed records' mean position is the own ship's centre, and every pool record within the radius of it is the
// own ship's. The seed records are the rig probe's own slots (rigSeed 1, docs "Rig seed": the records the own rigs emitted,
// always on the own vehicle) or, when the probe has none, the seed draw's instance window (rigSeed 0, the old seed: wrong
// object in 14 of 45 dumps of 2026-11-09). A rig seed's radius is its records' reach from their centre (the farthest, held
// per eye while the own rigs stay the same: resetHold 1 when they changed) plus `margin`, at most radiusMax; a draw seed's is
// radiusMax. Rig seed records at exactly (0,0,0) are unused records and are skipped. One group; one uint a record in the
// mask; stats = (own records, filtered pixels, seed records read, seed records; radius, reach used, rigSeed, reach held).
constexpr char kSeedHlsl[] =
    "cbuffer Seed : register(b0) { uint seedCount; uint recordCount; float radiusMax; uint rigSeed; float margin; uint resetHold; uint2 unused; };\n"
    "ByteAddressBuffer inst : register(t0);\n"
    "ByteAddressBuffer pool : register(t1);\n"
    "RWByteAddressBuffer ownMask : register(u0);\n"
    "RWByteAddressBuffer stats : register(u1);\n"
    "groupshared float3 gSum[256];\n"
    "groupshared uint gN[256];\n"
    "groupshared uint gOwn;\n"
    "bool seedPoint(uint i, out float3 p) {\n"
    "    p = float3(0, 0, 0);\n"
    "    uint r = inst.Load(i * 8) & 0x7FFFFF;\n"
    "    if (r >= recordCount) return false;\n"
    "    p = asfloat(pool.Load3(r * 336 + 16));\n"
    "    return all(isfinite(p)) && (rigSeed == 0 || any(p != 0));\n"
    "}\n"
    "[numthreads(256, 1, 1)] void hull_layer_seed_cs(uint t : SV_GroupIndex) {\n"
    "    float3 s = float3(0, 0, 0);\n"
    "    uint n = 0;\n"
    "    float3 p;\n"
    "    for (uint i = t; i < seedCount; i += 256)\n"
    "        if (seedPoint(i, p)) { s += p; n += 1; }\n"
    "    gSum[t] = s;\n"
    "    gN[t] = n;\n"
    "    if (t == 0) gOwn = 0;\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "    [unroll] for (uint k = 128; k > 0; k >>= 1) {\n"
    "        if (t < k) { gSum[t] += gSum[t + k]; gN[t] += gN[t + k]; }\n"
    "        GroupMemoryBarrierWithGroupSync();\n"
    "    }\n"
    "    uint valid = gN[0];\n"
    "    float3 c = gSum[0] / max((float)valid, 1.0);\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    // The rig seed's reach: the farthest seed record from the centre (a non-negative float orders as its bits).
    "    uint reach = 0;\n"
    "    if (rigSeed != 0)\n"
    "        for (uint j = t; j < seedCount; j += 256)\n"
    "            if (seedPoint(j, p)) reach = max(reach, asuint(length(p - c)));\n"
    "    gN[t] = reach;\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "    [unroll] for (uint m = 128; m > 0; m >>= 1) {\n"
    "        if (t < m) gN[t] = max(gN[t], gN[t + m]);\n"
    "        GroupMemoryBarrierWithGroupSync();\n"
    "    }\n"
    "    float held = resetHold != 0 ? 0.0 : asfloat(stats.Load(28));\n"
    "    if (!(held >= 0.0 && held <= radiusMax)) held = 0.0;\n"
    "    float used = rigSeed != 0 ? max(asfloat(gN[0]), held) : held;\n"
    "    float radius = rigSeed != 0 ? min(radiusMax, used + margin) : radiusMax;\n"
    "    float radius2 = radius * radius;\n"
    "    uint mine = 0;\n"
    "    for (uint q = t; q < recordCount; q += 256) {\n"
    "        float3 r = asfloat(pool.Load3(q * 336 + 16));\n"
    "        float3 d = r - c;\n"
    // An unused pool record sits at exactly (0,0,0); with the own ship near the origin they flooded the mask (dump
    // 214124277001: 936 of 1056, never drawn), and the 400-record guard then held separate mode off all flight.
    "        uint own = (valid > 0 && all(isfinite(r)) && any(r != 0) && dot(d, d) <= radius2) ? 1u : 0u;\n"
    "        ownMask.Store(q * 4, own);\n"
    "        mine += own;\n"
    "    }\n"
    "    InterlockedAdd(gOwn, mine);\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "    if (t == 0) {\n"
    "        stats.Store4(0, uint4(gOwn, 0, valid, seedCount));\n"
    "        stats.Store4(16, uint4(asuint(radius), asuint(used), rigSeed, asuint(used)));\n"
    "    }\n"
    "}\n";

// A pixel is a neighbour's when its private slot is set (2*slot+1 >= 1; the clear is -1) and names a record outside the
// own mask (or outside the pool).
#define HULL_LAYER_PASS_HLSL                                                                       \
    "cbuffer HullLayerPassCb : register(b0) { uint recordCount; float clearDepth; uint width; uint height; };\n" \
    "Texture2D<float2> slots : register(t0);\n"                                                    \
    "ByteAddressBuffer ownMask : register(t1);\n"                                                  \
    "bool foreignSlot(float x) {\n"                                                                \
    "    if (!(x >= 1.0)) return false;\n"                                                         \
    "    uint slot = (uint(x) - 1) >> 1;\n"                                                        \
    "    return slot >= recordCount || ownMask.Load(slot * 4) == 0;\n"                             \
    "}\n"

// The filter: RT0-3 to zero and the depth to the clear value on a neighbour's pixel; every other pixel is discarded.
constexpr char kFilterVsHlsl[] =
    "float4 hull_layer_filter_vs(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n";
constexpr char kFilterPsHlsl[] = HULL_LAYER_PASS_HLSL
    "struct HullLayerFilterOut { float4 c0 : SV_Target0; float4 c1 : SV_Target1; float4 c2 : SV_Target2; float4 c3 : SV_Target3;"
    " float depth : SV_Depth; };\n"
    "HullLayerFilterOut hull_layer_filter_ps(float4 pos : SV_Position) {\n"
    "    if (!foreignSlot(slots.Load(int3(int2(pos.xy), 0)).x)) discard;\n"
    "    HullLayerFilterOut o;\n"
    "    o.c0 = 0; o.c1 = 0; o.c2 = 0; o.c3 = 0;\n"
    "    o.depth = clearDepth;\n"
    "    return o;\n"
    "}\n";
// Build B (docs "Build B"), compiled at the first prefill (a failure leaves the layer running without it); the build compiles them too:
// hlsl-check: kPrefillPsHlsl hull_layer_prefill_ps ps_5_0
// hlsl-check: kMarkPsHlsl hull_layer_mark_ps ps_5_0
// hlsl-check: kResetPsHlsl hull_layer_reset_ps ps_5_0
//
// The prefill: right before the eye's first hull draw, the game's eye depth (the prepass, the cockpit shell and every draw
// before the own ship's block, no hull: the main pass discards it) into the private depth and into P, its copy. A float
// depth, so both hold the same bits. The private stencil is left at its clear.
constexpr char kPrefillPsHlsl[] =
    "Texture2D<float> mainDepth : register(t0);\n"
    "struct HullLayerPrefillOut { float z : SV_Target0; float depth : SV_Depth; };\n"
    "HullLayerPrefillOut hull_layer_prefill_ps(float4 pos : SV_Position) {\n"
    "    HullLayerPrefillOut o;\n"
    "    o.z = mainDepth.Load(int3(int2(pos.xy), 0));\n"
    "    o.depth = o.z;\n"
    "    return o;\n"
    "}\n";
// The mark: after the hull, 1 where no hull wrote -- the private depth not nearer than P (reversed Z): equal, or a draw with
// its own depth test put something farther -- else 0.
constexpr char kMarkPsHlsl[] =
    "Texture2D<float> privDepth : register(t0);\n"
    "Texture2D<float> prefill : register(t1);\n"
    "float hull_layer_mark_ps(float4 pos : SV_Position) : SV_Target0 {\n"
    "    int3 p = int3(int2(pos.xy), 0);\n"
    "    return privDepth.Load(p) > prefill.Load(p) ? 0.0 : 1.0;\n"
    "}\n";
// The reset: where marked, the private depth and stencil back to their clear (the stencil by the state's REPLACE).
constexpr char kResetPsHlsl[] =
    "cbuffer HullLayerResetCb : register(b0) { uint resetRecords; float clearDepth; uint resetW; uint resetH; };\n"
    "Texture2D<float> resetMark : register(t0);\n"
    "float hull_layer_reset_ps(float4 pos : SV_Position) : SV_Depth {\n"
    "    if (resetMark.Load(int3(int2(pos.xy), 0)) < 0.5) discard;\n"
    "    return clearDepth;\n"
    "}\n";
// The count: the filtered pixels, into stats[1].
constexpr char kCountHlsl[] = HULL_LAYER_PASS_HLSL
    "RWByteAddressBuffer stats : register(u0);\n"
    "groupshared uint gCount;\n"
    "[numthreads(8, 8, 1)] void hull_layer_count_cs(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {\n"
    "    if (gi == 0) gCount = 0;\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "    if (id.x < width && id.y < height && foreignSlot(slots.Load(int3(id.xy, 0)).x)) InterlockedAdd(gCount, 1);\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "    if (gi == 0 && gCount != 0) stats.InterlockedAdd(4, gCount);\n"
    "}\n";
#undef HULL_LAYER_PASS_HLSL

// Step 3 (separate), compiled at the first tonemap re-issue; the build compiles them too:
// hlsl-check: kCoverHlsl hull_layer_cover_cs cs_5_0
// hlsl-check: kTaaHlsl hull_layer_taa_cs cs_5_0
//
// The cover: the re-issued tonemap's pixel where the hull is shown -- the private depth set (nearer than its clear, reversed
// Z) and at least as near as the game's main depth, which holds the cockpit shell and no longer the hull -- with alpha 1,
// else 0 (premultiplied); and the private depth wherever the hull is, for the TAA's reprojection. The tonemap is read through
// a UNORM view: the bytes the game's tonemap would have written, the encoding the upscaler's input and output carry.
// Build A': outside this frame's rectangle (rect: x0, y0, x1, y1 exclusive; the whole eye when none) the cover and Z are 0,
// since the scissored passes drew nothing there. The shown pixels' box goes into `box` (kBoxBytes: every word a max, so a
// clear to 0 starts it), one atomic set an 8x8 group, for the next rectangles.
constexpr char kCoverHlsl[] =
    "cbuffer HullLayerCoverCb : register(b0) { float clearDepth; uint width; uint height; uint coverPad; uint4 rect; };\n"
    "Texture2D<float4> tone : register(t0);\n"
    "Texture2D<float> hullDepth : register(t1);\n"
    "Texture2D<float> mainDepth : register(t2);\n"
    "RWTexture2D<float4> cover : register(u0);\n"
    "RWTexture2D<float> coverDepth : register(u1);\n"
    "RWByteAddressBuffer box : register(u2);\n"
    "groupshared uint gBox[4];\n"
    "[numthreads(8, 8, 1)] void hull_layer_cover_cs(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {\n"
    "    if (gi < 4) gBox[gi] = 0;\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "    bool inside = id.x < width && id.y < height;\n"
    "    if (inside) {\n"
    "        bool inRect = id.x >= rect.x && id.y >= rect.y && id.x < rect.z && id.y < rect.w;\n"
    "        float hz = hullDepth.Load(int3(id.xy, 0));\n"
    "        float mz = mainDepth.Load(int3(id.xy, 0));\n"
    "        bool hull = inRect && hz > clearDepth;\n"
    "        bool shown = hull && hz >= mz;\n"
    "        cover[id.xy] = shown ? float4(tone.Load(int3(id.xy, 0)).rgb, 1.0) : float4(0.0, 0.0, 0.0, 0.0);\n"
    "        coverDepth[id.xy] = hull ? hz : 0.0;\n"
    "        if (shown) {\n"
    "            InterlockedMax(gBox[0], 65535u - id.x);\n"
    "            InterlockedMax(gBox[1], 65535u - id.y);\n"
    "            InterlockedMax(gBox[2], id.x + 1u);\n"
    "            InterlockedMax(gBox[3], id.y + 1u);\n"
    "        }\n"
    "    }\n"
    "    GroupMemoryBarrierWithGroupSync();\n"
    "    if (gi < 4 && gBox[2] != 0) { uint prev; box.InterlockedMax(gi * 4, gBox[gi], prev); }\n"
    "}\n";

// The hull layer's TAA and composite, one thread an output pixel. This frame: the cover's 3x3 around the pixel's unjittered
// position, each render sample q at q - jitter weighted exp(-2.29 d^2) (the temporal pass's own filter), and its range. The
// history: the pixel carried to last frame's view by the head's delta (and its translation at the cover's nearest depth of
// the 3x3, when the pass carries it) -- temporal_shader_source.h's temporalPixelDir, temporalCarry and temporalProjectPixel,
// transcribed -- then fetched bilinearly and clamped to the range. The result (premultiplied colour, coverage in alpha) is the
// next history and goes over the upscaled eye.
constexpr char kTaaHlsl[] =
    "cbuffer HullLayerTaaCb : register(b0) {\n"
    "    float4 tanNow; float4 tanPrev; float4 dRow0; float4 dRow1; float4 dRow2;\n"
    "    float4 headT; float4 jit; float4 depthAB; int4 sizes; int4 origin;\n"
    "};\n"
    "Texture2D<float4> cover : register(t0);\n"
    "Texture2D<float> coverDepth : register(t1);\n"
    "Texture2D<float4> histIn : register(t2);\n"
    "StructuredBuffer<uint> tiles : register(t4);\n"
    "SamplerState lin : register(s0);\n"
    "RWTexture2D<float4> histOut : register(u0);\n"
    // The history read: Catmull-Rom through nine bilinear taps, not one bilinear tap. A bilinear history fetch blurs a
    // little every frame and, at a current weight of 0.1, the blur accumulated: the pilot found the hull's textures soft
    // (2026-11-08 22:20). The 3x3 range clamp after it removes the kernel's overshoot.
    "float4 histCatmullRom(float2 uv) {\n"
    "    float2 tsz; histIn.GetDimensions(tsz.x, tsz.y);\n"
    "    float2 pos = uv * tsz;\n"
    "    float2 c1 = floor(pos - 0.5) + 0.5;\n"
    "    float2 f = pos - c1;\n"
    "    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
    "    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
    "    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
    "    float2 w3 = f * f * (-0.5 + 0.5 * f);\n"
    "    float2 w12 = w1 + w2;\n"
    "    float2 o12 = w2 / w12;\n"
    "    float2 t0 = (c1 - 1.0) / tsz;\n"
    "    float2 t3 = (c1 + 2.0) / tsz;\n"
    "    float2 t12 = (c1 + o12) / tsz;\n"
    "    float4 r = 0.0;\n"
    "    r += histIn.SampleLevel(lin, float2(t0.x, t0.y), 0) * (w0.x * w0.y);\n"
    "    r += histIn.SampleLevel(lin, float2(t12.x, t0.y), 0) * (w12.x * w0.y);\n"
    "    r += histIn.SampleLevel(lin, float2(t3.x, t0.y), 0) * (w3.x * w0.y);\n"
    "    r += histIn.SampleLevel(lin, float2(t0.x, t12.y), 0) * (w0.x * w12.y);\n"
    "    r += histIn.SampleLevel(lin, float2(t12.x, t12.y), 0) * (w12.x * w12.y);\n"
    "    r += histIn.SampleLevel(lin, float2(t3.x, t12.y), 0) * (w3.x * w12.y);\n"
    "    r += histIn.SampleLevel(lin, float2(t0.x, t3.y), 0) * (w0.x * w3.y);\n"
    "    r += histIn.SampleLevel(lin, float2(t12.x, t3.y), 0) * (w12.x * w3.y);\n"
    "    r += histIn.SampleLevel(lin, float2(t3.x, t3.y), 0) * (w3.x * w3.y);\n"
    "    return r;\n"
    "}\n"
    // One group an 8x8 output tile from the tile list (kTileListHlsl): bit 31 says the hull's 3x3 reaches the tile this frame.
    // A listed tile without it (hull there one or two frames ago) gets zero history: what the whole-eye TAA wrote there, since
    // a 3x3 of empty cover clamps any history to 0. Unlisted tiles stay zero in both history textures (see kTileListHlsl).
    "[numthreads(8, 8, 1)] void hull_layer_taa_cs(uint3 gid : SV_GroupID, uint3 gt : SV_GroupThreadID) {\n"
    "    uint te = tiles[gid.x];\n"
    "    uint3 id = uint3(uint2(te & 0xFFFFu, (te >> 16) & 0x7FFFu) * 8u + gt.xy, 0u);\n"
    "    int2 rsz = sizes.xy;\n"
    "    int2 osz = sizes.zw;\n"
    "    if (id.x >= (uint)osz.x || id.y >= (uint)osz.y) return;\n"
    "    if ((te & 0x80000000u) == 0u) { histOut[id.xy] = 0.0; return; }\n"
    "    float2 pr = (float2(id.xy) + 0.5) * float2(rsz) / float2(osz) - 0.5;\n"
    "    int2 c = int2(round(pr + jit.xy));\n"
    "    float4 cur = 0.0;\n"
    "    float wsum = 0.0;\n"
    "    float4 lo = 1e9;\n"
    "    float4 hi = -1e9;\n"
    "    float4 m1 = 0.0;\n"
    "    float4 m2 = 0.0;\n"
    "    float zr = 0.0;\n"
    "    [unroll] for (int dy = -1; dy <= 1; ++dy) {\n"
    "        [unroll] for (int dx = -1; dx <= 1; ++dx) {\n"
    "            int2 q = clamp(c + int2(dx, dy), int2(0, 0), rsz - 1);\n"
    "            float4 s = cover.Load(int3(origin.xy + q, 0));\n"
    "            float2 dd = float2(q) - jit.xy - pr;\n"
    // exp(-5.0 d^2) in render pixels: sigma 0.32 render, about 0.42 output pixels at 1.33x. The temporal pass's
    // exp(-2.29 d^2) (0.62 output pixels) left the hull visibly softer than DLSS's own hull (the pilot, 22:30). The
    // jitter moves the samples every frame and the history (weight 0.9) averages them, so the narrow kernel's
    // per-frame aliasing averages out while its detail stays.
    // exp(-3.5 d^2) since 23:25: with the loose interior clamp below, the history carries the detail, and a slightly wider
    // per-frame kernel feeds it less jitter aliasing.
    "            float wt = exp(-3.5 * dot(dd, dd));\n"
    "            cur += s * wt;\n"
    "            wsum += wt;\n"
    "            lo = min(lo, s);\n"
    "            hi = max(hi, s);\n"
    "            m1 += s;\n"
    "            m2 += s * s;\n"
    "            zr = max(zr, coverDepth.Load(int3(origin.xy + q, 0)));\n"
    "        }\n"
    "    }\n"
    "    cur /= max(wsum, 1e-6);\n"
    // Variance clipping (2026-11-08 23:10): the history is held to the 3x3's mean +- 1.25 sigma, inside its min/max. A
    // min/max box alone swings with the jitter on thin high-contrast lines (the decals' panel lines), so the history
    // jumped and those lines flickered as if there were no TAA (the pilot).
    "    m1 /= 9.0;\n"
    "    float4 sd = sqrt(max(m2 / 9.0 - m1 * m1, 0.0));\n"
    // Inside the hull (the whole 3x3 is hull) the layer moves only by the head's motion, which the reprojection carries
    // exactly, so the history may be trusted far more: a loose box (mean +- 3 sigma) and a lower current weight let a
    // sub-pixel panel line converge to a steady line instead of the staircase the pilot saw flicker (23:20, dump
    // 232023684003). At the silhouette the tight box stays for a history whose coverage disagrees: there the world behind
    // changes. A history whose coverage agrees with this frame's (h.a within the 3x3's alpha range +- 0.05) is the same edge,
    // carried by the reprojection, and is taken unclamped in colour at the interior's weight (2026-11-12): the 1.25-sigma clip
    // swung with the jitter and was the edge's flicker (offline, 4 moving sequences: pixels changing > 4/255 between frames
    // 2.0% to 0.4%; the interior's lighting lag unchanged; docs "Silhouette coverage test").
    "    float weight = jit.w;\n"
    "    float aLo = lo.a, aHi = hi.a;\n"
    "    bool edge = !(lo.a > 0.999);\n"
    "    if (lo.a > 0.999) {\n"
    "        lo = m1 - 3.0 * sd;\n"
    "        hi = m1 + 3.0 * sd;\n"
    "        lo.a = 1.0; hi.a = 1.0;\n"
    "        weight = jit.w * 0.5;\n"
    "    } else {\n"
    "        lo = max(lo, m1 - 1.25 * sd);\n"
    "        hi = min(hi, m1 + 1.25 * sd);\n"
    "    }\n"
    "    float4 res = cur;\n"
    "    if (jit.z != 0.0) {\n"
    "        float3 d;\n"
    "        d.x = tanNow.x + (pr.x + 0.5) / float(rsz.x) * (tanNow.y - tanNow.x);\n"
    "        d.y = tanNow.w - (pr.y + 0.5) / float(rsz.y) * (tanNow.w - tanNow.z);\n"
    "        d.z = -1.0;\n"
    "        float3 dp = float3(dot(dRow0.xyz, d), dot(dRow1.xyz, d), dot(dRow2.xyz, d));\n"
    "        float den = zr - depthAB.x;\n"
    "        if (headT.w != 0.0 && zr > 0.0 && den > 0.0) dp = dp * (depthAB.y / den) + headT.xyz;\n"
    "        if (dp.z < -1e-6) {\n"
    "            float2 pp;\n"
    "            pp.x = (dp.x / -dp.z - tanPrev.x) / (tanPrev.y - tanPrev.x) * float(rsz.x) - 0.5;\n"
    "            pp.y = (tanPrev.w - dp.y / -dp.z) / (tanPrev.w - tanPrev.z) * float(rsz.y) - 0.5;\n"
    "            float2 uv = (pp + 0.5) / float2(rsz);\n"
    "            if (all(uv >= 0.0) && all(uv <= 1.0)) {\n"
    "                float4 h = histCatmullRom(uv);\n"
    "                if (edge && h.a >= aLo - 0.05 && h.a <= aHi + 0.05) res = lerp(saturate(h), cur, jit.w * 0.5);\n"
    "                else res = lerp(clamp(h, lo, hi), cur, weight);\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    histOut[id.xy] = res;\n"   // the composite is the sharpen's (kSharpHlsl), which always follows
    "}\n";

// The sharpen and composite, after the TAA (2026-11-08 22:35): the TAA output measured 0.7 output px softer than one frame
// (dumps 222624596001, 222629772002: 26-29% of the high-frequency energy, no history shift), and the pilot saw the hull
// softer than DLSS's. A cross unsharp mask on the TAA result, clamped to the cross's range (no halo); only where the whole
// cross is hull (alpha 1), so the silhouette's anti-aliased edge stays as the TAA left it. The history is not sharpened.
// hlsl-check: kSharpHlsl hull_layer_sharp_cs cs_5_0
// hlsl-check: kSharpDirectHlsl hull_layer_sharp_direct_cs cs_5_0
// Two entry points, one body (HULL_LAYER_SHARP_HLSL's hullLayerSharpPixel): kSharpHlsl reads the world from an SRV of the
// upscaled eye and writes the composite's own out (c.out, copied whole from the eye first and back over it after);
// kSharpDirectHlsl (2026-11-10, docs "Bench A/B variants") reads and writes the eye itself through one typed UAV, the world only
// at the pixel it writes, so the eye's pixels outside the listed tiles are never touched and neither copy runs. Same loads, same
// arithmetic, same UNORM store.
// c + amount * (c - mean of the 4 neighbours); 0.8 amplified the thin lines' flicker (23:10). 0.3 since 2026-11-12: offline, the
// share of thin-line pixels changing > 4/255 between frames was 0.39% at 0, 0.57% at 0.3, 1.14% at 0.6 (4 moving sequences).
// The frame-generation overlay reads the same constant through hullLayerSharpAt.
constexpr float kSharpAmount = 0.3f;
#define HULL_LAYER_SHARP_HLSL                                                                      \
    "Texture2D<float4> hist : register(t0);\n"                                                     \
    "StructuredBuffer<uint> tiles : register(t2);\n"                                               \
    "RWTexture2D<float4> compOut : register(u0);\n"                                                \
    "cbuffer HullLayerSharpCb : register(b0) { float4 amount; };\n"                                \
    "float4 hullLayerSharpAt(int2 p, int2 mx) {\n"                                                 \
    "    float4 c = hist.Load(int3(p, 0));\n"                                                      \
    "    float4 l = hist.Load(int3(clamp(p + int2(-1, 0), 0, mx), 0));\n"                          \
    "    float4 r = hist.Load(int3(clamp(p + int2(1, 0), 0, mx), 0));\n"                           \
    "    float4 u = hist.Load(int3(clamp(p + int2(0, -1), 0, mx), 0));\n"                          \
    "    float4 d = hist.Load(int3(clamp(p + int2(0, 1), 0, mx), 0));\n"                           \
    "    float4 res = c;\n"                                                                        \
    "    if (min(min(c.a, l.a), min(min(r.a, u.a), d.a)) > 0.999) {\n"                             \
    "        float3 m = (l.rgb + r.rgb + u.rgb + d.rgb) * 0.25;\n"                                  \
    "        float3 lo = min(c.rgb, min(min(l.rgb, r.rgb), min(u.rgb, d.rgb)));\n"                   \
    "        float3 hi = max(c.rgb, max(max(l.rgb, r.rgb), max(u.rgb, d.rgb)));\n"                   \
    "        res.rgb = clamp(c.rgb + amount.x * (c.rgb - m), lo, hi);\n"                             \
    "    }\n"                                                                                      \
    "    return res;\n"                                                                            \
    "}\n"                                                                                          \
    "bool hullLayerSharpPixel(uint3 gid, uint3 gt, out int2 p, out float4 res) {\n"                 \
    "    p = int2(0, 0);\n"                                                                        \
    "    res = float4(0.0, 0.0, 0.0, 0.0);\n"                                                      \
    "    uint te = tiles[gid.x];\n"                                                                \
    "    if ((te & 0x80000000u) == 0u) return false;\n"                                            \
    "    uint2 id = uint2(te & 0xFFFFu, (te >> 16) & 0x7FFFu) * 8u + gt.xy;\n"                     \
    "    uint ow, oh; hist.GetDimensions(ow, oh);\n"                                               \
    "    if (id.x >= ow || id.y >= oh) return false;\n"                                            \
    "    p = int2(id.xy);\n"                                                                       \
    "    res = hullLayerSharpAt(p, int2(ow, oh) - 1);\n"                                           \
    "    return true;\n"                                                                           \
    "}\n"
constexpr char kSharpHlsl[] =
    HULL_LAYER_SHARP_HLSL
    "Texture2D<float4> world : register(t1);\n"
    // The TAA's tile list: a tile the hull does not reach this frame keeps the world compOut already holds (copied whole first).
    "[numthreads(8, 8, 1)] void hull_layer_sharp_cs(uint3 gid : SV_GroupID, uint3 gt : SV_GroupThreadID) {\n"
    "    int2 p; float4 res;\n"
    "    if (!hullLayerSharpPixel(gid, gt, p, res)) return;\n"
    "    float4 w = world.Load(int3(p, 0));\n"
    "    compOut[p] = float4(w.rgb * (1.0 - res.a) + res.rgb, w.a);\n"
    "}\n";
// compOut is the eye: a tile the hull does not reach is not written, so it keeps the world.
constexpr char kSharpDirectHlsl[] =
    HULL_LAYER_SHARP_HLSL
    "[numthreads(8, 8, 1)] void hull_layer_sharp_direct_cs(uint3 gid : SV_GroupID, uint3 gt : SV_GroupThreadID) {\n"
    "    int2 p; float4 res;\n"
    "    if (!hullLayerSharpPixel(gid, gt, p, res)) return;\n"
    "    float4 w = compOut[p];\n"
    "    compOut[p] = float4(w.rgb * (1.0 - res.a) + res.rgb, w.a);\n"
    "}\n";

// Frame generation (2026-11-11, docs "Frame generation integration"): the hull layer drawn into S, the frame the synthesizer
// extrapolated from the world-only eye. The hull is rigid with the ship, so from B to S it moves by the head's step alone (x_S =
// rot x_B + t, the step S was built with; rows s0-s2, t in their w). Each S output pixel finds its B position by the step's inverse:
// the rotation first, then, when the step carries a translation, again at the hull's depth read there. The layer (the TAA result,
// premultiplied) is sharpened as B's composite sharpens it (hullLayerSharpAt) at the four texels around that position, blended
// bilinearly and put over S. One thread an S pixel over the whole eye; one without hull in its four texels returns after four loads.
// compOut is S (a typed UAV load, as kSharpDirectHlsl). depthAB: A, B, -, translation on.
// Two entry points over one body (hullSOverlayAt): hull_layer_s_cs, one thread an S pixel over the whole eye (the fallback), and
// hull_layer_s_tiles_cs (2026-11-11), one 8x8 group an S tile of the list hull_layer_s_tile_list_cs made (t2, x | y << 16). The list
// pass, one thread an S tile: the tile's corners by the step's inverse rotation give the B box its pixels map to before the
// translation (a homography maps the tile to a quad whose corners bound it); the translation moves a B position by at most
// e (|u.xy| / a + 1) / (a - e) in tangent units, e = |t| / the nearest hull distance the pixels can read (the largest cover depth over
// the render pixels the rotated box rounds to); the box, grown by that, a pixel for rounding and the bilinear texel, is listed when
// any B tile in it holds this frame's TAA result (the TAA's tile state, t3, bit 0: the TAA writes zero everywhere else). A tile near
// or behind the eye plane, or whose box is too large to scan, is listed. Pixels of unlisted tiles have no hull texel in reach, so the
// whole-eye pass leaves them as they are too: the two entries write the same S.
// hlsl-check: kSHlsl hull_layer_s_cs cs_5_0
// hlsl-check: kSHlsl hull_layer_s_tiles_cs cs_5_0
// hlsl-check: kSHlsl hull_layer_s_tile_list_cs cs_5_0
constexpr char kSHlsl[] =
    HULL_LAYER_SHARP_HLSL
    "cbuffer HullLayerSCb : register(b1) { float4 tanB; float4 s0; float4 s1; float4 s2; float4 depthAB; int4 sizes; int4 origin; };\n"
    "Texture2D<float> coverDepth : register(t1);\n"
    "Texture2D<uint> bTileState : register(t3);\n"
    "AppendStructuredBuffer<uint> sTiles : register(u1);\n"
    "SamplerState hullSClamp : register(s0);\n"
    "float2 hullSPixel(float3 b, int2 osz) {\n"
    "    if (b.z > -1e-6) return float2(-1e9, -1e9);\n"
    "    float2 q = b.xy / -b.z;\n"
    "    return float2((q.x - tanB.x) / (tanB.y - tanB.x) * float(osz.x) - 0.5, (tanB.w - q.y) / (tanB.w - tanB.z) * float(osz.y) - 0.5);\n"
    "}\n"
    "float3 hullSBack(float3 v) { return v.x * s0.xyz + v.y * s1.xyz + v.z * s2.xyz; }\n"
    "void hullSOverlayAt(uint2 id) {\n"
    "    int2 rsz = sizes.xy;\n"
    "    int2 osz = sizes.zw;\n"
    "    if (id.x >= (uint)osz.x || id.y >= (uint)osz.y) return;\n"
    "    float2 uv = (float2(id.xy) + 0.5) / float2(osz);\n"
    "    float3 dS = float3(tanB.x + uv.x * (tanB.y - tanB.x), tanB.w - uv.y * (tanB.w - tanB.z), -1.0);\n"
    "    float2 pb = hullSPixel(hullSBack(dS), osz);\n"
    "    if (depthAB.w != 0.0 && pb.x > -1e8) {\n"
    "        int2 rp = clamp(int2(round((pb + 0.5) * float2(rsz) / float2(osz) - 0.5)), int2(0, 0), rsz - 1);\n"
    "        float z = coverDepth.Load(int3(origin.xy + rp, 0));\n"
    "        float den = z - depthAB.x;\n"
    "        if (z > 0.0 && den > 0.0) pb = hullSPixel(hullSBack(dS * (depthAB.y / den) - float3(s0.w, s1.w, s2.w)), osz);\n"
    "    }\n"
    "    int2 mx = osz - 1;\n"
    "    if (pb.x < -1.0 || pb.y < -1.0 || pb.x > float(osz.x) || pb.y > float(osz.y)) return;\n"
    "    int2 i0 = int2(floor(pb));\n"
    "    float2 f = pb - float2(i0);\n"
    "    float4 h = 0.0;\n"
    "    [unroll] for (int k = 0; k < 4; ++k) {\n"
    "        int2 q = i0 + int2(k & 1, k >> 1);\n"
    "        float wq = ((k & 1) != 0 ? f.x : 1.0 - f.x) * ((k >> 1) != 0 ? f.y : 1.0 - f.y);\n"
    "        if (any(q < 0) || any(q > mx) || wq <= 0.0) continue;\n"
    "        if (hist.Load(int3(q, 0)).a <= 0.0) continue;\n"
    "        h += hullLayerSharpAt(q, mx) * wq;\n"
    "    }\n"
    "    if (h.a <= 0.0) return;\n"
    "    float4 w = compOut[id.xy];\n"
    "    compOut[id.xy] = float4(w.rgb * (1.0 - h.a) + h.rgb, w.a);\n"
    "}\n"
    "[numthreads(8, 8, 1)] void hull_layer_s_cs(uint3 id : SV_DispatchThreadID) { hullSOverlayAt(id.xy); }\n"
    "[numthreads(8, 8, 1)] void hull_layer_s_tiles_cs(uint3 gid : SV_GroupID, uint3 gt : SV_GroupThreadID) {\n"
    "    uint te = tiles[gid.x];\n"
    "    hullSOverlayAt(uint2(te & 0xFFFFu, te >> 16) * 8u + gt.xy);\n"
    "}\n"
    "[numthreads(8, 8, 1)] void hull_layer_s_tile_list_cs(uint3 id : SV_DispatchThreadID) {\n"
    "    int2 rsz = sizes.xy;\n"
    "    int2 osz = sizes.zw;\n"
    "    int2 p0 = int2(id.xy) * 8;\n"
    "    if (p0.x >= osz.x || p0.y >= osz.y) return;\n"
    "    int2 p1 = min(p0 + 7, osz - 1);\n"
    "    float2 lo = float2(1e9, 1e9), hi = float2(-1e9, -1e9);\n"
    "    float amin = 1e9, umax = 0.0;\n"
    "    bool keep = false;\n"
    "    [unroll] for (int k = 0; k < 4; ++k) {\n"
    "        int2 c = int2((k & 1) != 0 ? p1.x : p0.x, (k >> 1) != 0 ? p1.y : p0.y);\n"
    "        float2 uv = (float2(c) + 0.5) / float2(osz);\n"
    "        float3 b = hullSBack(float3(tanB.x + uv.x * (tanB.y - tanB.x), tanB.w - uv.y * (tanB.w - tanB.z), -1.0));\n"
    "        if (b.z > -0.05) keep = true;\n"
    "        float2 pb = hullSPixel(b, osz);\n"
    "        lo = min(lo, pb);\n"
    "        hi = max(hi, pb);\n"
    "        amin = min(amin, -b.z);\n"
    "        umax = max(umax, length(b.xy));\n"
    "    }\n"
    "    float r = 0.0;\n"
    "    if (!keep && depthAB.w != 0.0) {\n"
    "        float2 sc = float2(rsz) / float2(osz);\n"
    "        int2 r0 = int2(floor(clamp((lo + 0.5) * sc - 0.5, -2.0, float2(rsz)))) - 1;\n"
    "        int2 r1 = int2(ceil(clamp((hi + 0.5) * sc - 0.5, -2.0, float2(rsz)))) + 1;\n"
    "        r0 = clamp(r0, int2(0, 0), rsz - 1);\n"
    "        r1 = clamp(r1, int2(0, 0), rsz - 1);\n"
    "        float zmax = 0.0;\n"
    "        if ((r1.x - r0.x + 1) * (r1.y - r0.y + 1) > 256) {\n"
    "            keep = true;\n"
    "        } else {\n"
    "            float2 dsz; coverDepth.GetDimensions(dsz.x, dsz.y);\n"
    "            for (int y = r0.y; y <= r1.y; y += 2)\n"
    "                for (int x = r0.x; x <= r1.x; x += 2) {\n"
    "                    float4 g = coverDepth.GatherRed(hullSClamp, (float2(origin.xy + int2(x, y)) + 1.0) / dsz);\n"
    "                    zmax = max(zmax, max(max(g.x, g.y), max(g.z, g.w)));\n"
    "                }\n"
    "        }\n"
    "        float den = zmax - depthAB.x;\n"
    "        if (!keep && zmax > 0.0 && den > 0.0) {\n"
    "            float e = depthAB.y > 0.0 ? length(float3(s0.w, s1.w, s2.w)) * den / depthAB.y : 1e9;\n"
    "            if (amin - e < 0.05) keep = true;\n"
    "            else r = e * (umax / amin + 1.0) / (amin - e) * max(float(osz.x) / (tanB.y - tanB.x), float(osz.y) / (tanB.w - tanB.z));\n"
    "        }\n"
    "    }\n"
    "    if (!keep) {\n"
    "        float m = r + 2.0;\n"
    "        float2 q0 = clamp(floor(lo - m), -1.0, float2(osz));\n"
    "        float2 q1 = clamp(floor(hi + m) + 1.0, -1.0, float2(osz));\n"
    "        int2 a = max(int2(q0), int2(0, 0));\n"
    "        int2 b = min(int2(q1), osz - 1);\n"
    "        if (a.x > b.x || a.y > b.y) return;\n"
    "        int2 t0 = a / 8, t1 = b / 8;\n"
    "        if ((t1.x - t0.x + 1) * (t1.y - t0.y + 1) > 100) {\n"
    "            keep = true;\n"
    "        } else {\n"
    "            for (int ty = t0.y; ty <= t1.y && !keep; ++ty)\n"
    "                for (int tx = t0.x; tx <= t1.x; ++tx)\n"
    "                    if ((bTileState.Load(int3(tx, ty, 0)) & 1u) != 0u) keep = true;\n"
    "        }\n"
    "    }\n"
    "    if (keep) sTiles.Append(id.x | (id.y << 16));\n"
    "}\n";

// The TAA's tiles (2026-11-10): the TAA and sharpen ran on every output pixel of the eye while the shown hull is 6-11% of it.
// hull_layer_tile_mark_cs, one thread a render pixel of the eye's region: a cover pixel with the hull marks every 8x8 output
// tile whose TAA 3x3 (kTaaHlsl: render taps round(pr + jit) +- 1) can reach it, with a pixel of margin. hull_layer_tile_list_cs,
// one thread a tile: state = (state << 1 | mark) & 7 and the mark cleared; a tile with a bit set (the hull reaches it now, or
// did one or two frames ago) is appended as x | y << 16 | now << 31. A tile listed without `now` gets zero history; one
// unlisted stays zero in both textures of the pair, since its last write into each was a zero. A history reset clears the
// pair and the state.
// hlsl-check: kTileMarkHlsl hull_layer_tile_mark_cs cs_5_0
// hlsl-check: kTileListHlsl hull_layer_tile_list_cs cs_5_0
constexpr char kTileMarkHlsl[] =
    "cbuffer HullLayerTaaCb : register(b0) {\n"
    "    float4 tanNow; float4 tanPrev; float4 dRow0; float4 dRow1; float4 dRow2;\n"
    "    float4 headT; float4 jit; float4 depthAB; int4 sizes; int4 origin;\n"
    "};\n"
    "Texture2D<float4> cover : register(t0);\n"
    "RWTexture2D<uint> mark : register(u0);\n"
    "[numthreads(8, 8, 1)] void hull_layer_tile_mark_cs(uint3 id : SV_DispatchThreadID) {\n"
    "    int2 rsz = sizes.xy;\n"
    "    int2 osz = sizes.zw;\n"
    "    if (id.x >= (uint)rsz.x || id.y >= (uint)rsz.y) return;\n"
    "    if (cover.Load(int3(origin.xy + int2(id.xy), 0)).a <= 0.0) return;\n"
    "    float2 q = float2(id.xy);\n"
    "    float2 s = float2(osz) / float2(rsz);\n"
    "    int2 o0 = int2(floor((q - 1.0 - jit.xy) * s - 0.5)) - 1;\n"
    "    int2 o1 = int2(ceil((q + 2.0 - jit.xy) * s - 0.5)) + 1;\n"
    "    int2 t0 = clamp(o0, 0, osz - 1) / 8;\n"
    "    int2 t1 = clamp(o1, 0, osz - 1) / 8;\n"
    "    for (int ty = t0.y; ty <= t1.y; ++ty)\n"
    "        for (int tx = t0.x; tx <= t1.x; ++tx) InterlockedOr(mark[uint2(tx, ty)], 1u);\n"
    "}\n";
constexpr char kTileListHlsl[] =
    "cbuffer HullLayerTaaCb : register(b0) {\n"
    "    float4 tanNow; float4 tanPrev; float4 dRow0; float4 dRow1; float4 dRow2;\n"
    "    float4 headT; float4 jit; float4 depthAB; int4 sizes; int4 origin;\n"
    "};\n"
    "RWTexture2D<uint> mark : register(u0);\n"
    "RWTexture2D<uint> state : register(u1);\n"
    "AppendStructuredBuffer<uint> list : register(u2);\n"
    "[numthreads(8, 8, 1)] void hull_layer_tile_list_cs(uint3 id : SV_DispatchThreadID) {\n"
    "    uint2 tn = (uint2(sizes.zw) + 7u) / 8u;\n"
    "    if (id.x >= tn.x || id.y >= tn.y) return;\n"
    "    uint now = mark[id.xy] & 1u;\n"
    "    mark[id.xy] = 0u;\n"
    "    uint b = ((state[id.xy] << 1) | now) & 7u;\n"
    "    state[id.xy] = b;\n"
    "    if (b != 0u) list.Append(id.x | (id.y << 16) | (now << 31));\n"
    "}\n";

// A private copy of one game texture: the same desc, default usage, nothing shared. `game` is an identity only.
struct Mirror {
    const void* game = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    ID3D11Texture2D* priv = nullptr;
    uint32_t lastSeen = 0;   // the layer's frame the game texture was last bound or cleared
};
enum class ViewKind : uint8_t { Rtv, Dsv, Srv, Uav };
union ViewDesc {
    D3D11_RENDER_TARGET_VIEW_DESC rtv;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav;   // the AO chain's targets (a Texture2DArray slice range comes across as the game's view has it)
};
// A private view over a mirror, made with the game's own view desc (so a read-only depth view, an array slice or a
// typed format comes across as the game bound it).
struct ViewEntry {
    const void* game = nullptr;
    ViewKind kind = ViewKind::Rtv;
    ViewDesc desc;
    ID3D11View* priv = nullptr;
};

struct ClearValue {
    const void* res = nullptr;
    float depth = 0.0f;
    uint8_t stencil = 0;
};

struct Readback {
    ID3D11Buffer* staging = nullptr;
    uint32_t frame = 0;
    bool pending = false;
};

// The eye's private G-buffer window: before the seed, between the seed and the eye's depth copy, filtered, or off for
// this eye-frame (no slot target, no shader, a seed refused).
enum class Gb : uint8_t { Before, In, Done, Off };
struct Eye {
    const void* depth = nullptr;   // the game's eye depth texture (identity): the eye's key
    // This frame.
    bool seen = false;
    bool shell = false;            // the cockpit shell was drawn into this depth this frame
    Gb gb = Gb::Before;
    uint32_t hull = 0, noSlotDraws = 0, lightDraws = 0, lightSpan = 0;
    bool lightOpen = false;        // between the shadow mask and the sky
    // Station fixes. This frame: re-issues culled to the own instances by the keep-own VS, and those that needed the neighbour
    // filter; the hull-family G-buffer draws into this eye (the busy governor's count); the unknown-pair window (the shadow mask
    // to the sky, the span, or the first forward draw) and the HDR writers in it waiting for a later lighting draw.
    uint32_t culled = 0, filterDraws = 0, gbufHull = 0;
    bool unknownOpen = false;
    uint64_t pending[kMaxPendingPairs][2] = {};
    uint32_t pendingCount = 0;
    // Across frames: the private G-buffer's size (the seed's depth, the eye's render size: stages and mirrors are made at it only),
    // and the eye-frames since the private slot target was last needed (its private copy goes after kEvictFrames of them).
    uint32_t w = 0, h = 0, slotIdle = 0;
    // Between the eye's depth copy and its next stage: the hull decals (BBE5) are drawn there, after the copy that ends the
    // G-buffer window (census 3 of 215951: the depth copy at q1040, the decals at q1042-1065). Without this window the own
    // decals were neither discarded from the main pass nor re-issued into the layer (decal_draws=0, 22:51).
    bool decalOpen = false;
    uint8_t stagesDone = 0;
    const void* hdrGame = nullptr;   // the G-buffer's RT3: the game's HDR (identity)
    const void* slotGame = nullptr;  // engine motion's slot target, the seed's RT6 (identity)
    // The AO chain (identities this frame): the G-buffer RT0-3 (the seed's), the depth copy (its stage's target) and the
    // chain's own outputs, by which a step or the apply draw names its eye; the steps re-issued (bit a step, kAoApplyBit
    // the apply) and those the game ran for this eye.
    const void* gbufGame[4] = {};
    const void* copyGame = nullptr;
    const void* aoOut[kAoOwned] = {};
    uint8_t aoOutCount = 0, aoDone = 0, aoSeen = 0;
    uint8_t reflSeen = 0;   // the reflection pair's steps the game ran for this eye this frame (kReflCs)
    bool reflCleared = false;   // this frame: the private copies of the pass's other targets (the R16 u1) cleared before its first re-issue
    const void* reflU1Game = nullptr;   // across frames: the game's target at the pass's u1 (identity), for the game's own clears of it
    float clearDepth = 0.0f;
    uint32_t records = 0, seedInstances = 0;
    uint32_t rigGen = ~0u;   // the own rigs' generation the eye's held reach (stats word 7) belongs to
    // The seed's private views (references held from the seed to the filter).
    ID3D11RenderTargetView* privRtv[4] = {};
    ID3D11RenderTargetView* privSlot = nullptr;
    ID3D11DepthStencilView* privDsv = nullptr;
    // GPU objects kept across frames.
    ID3D11Buffer* mask = nullptr;
    ID3D11ShaderResourceView* maskSrv = nullptr;
    ID3D11UnorderedAccessView* maskUav = nullptr;
    uint32_t maskRecords = 0;
    ID3D11Buffer* stats = nullptr;
    ID3D11UnorderedAccessView* statsUav = nullptr;
    Readback ring[kRing];
    ID3D11ShaderResourceView* slotSrv = nullptr;   // over the private slot target
    const void* slotSrvOf = nullptr;
    // The dump's staging copies (the capture frame only).
    ID3D11Texture2D* stageHull = nullptr;
    ID3D11Texture2D* stageGame = nullptr;
    ID3D11Texture2D* stageDepth = nullptr;
    ID3D11Buffer* stageStats = nullptr;
    uint32_t dumpHull = 0, dumpLight = 0;
    // The reflection pass on the capture frame (kReflCs): the game's HDR right after its dispatch and the private HDR right after
    // the re-issue (the last step wins), the stencil (t4) and G-buffer (t11) the pass read, game and private, and the pass's cb1 at
    // the dispatch (first constant: the 11.1 offset it was bound at). The steps staged on each side, and why the private side was not.
    ID3D11Texture2D* stageGameRefl = nullptr;
    ID3D11Texture2D* stageHullRefl = nullptr;
    ID3D11Texture2D* stageGameStencil = nullptr;
    ID3D11Texture2D* stageHullStencil = nullptr;
    ID3D11Texture2D* stageGameT11 = nullptr;
    ID3D11Texture2D* stageHullT11 = nullptr;
    ID3D11Buffer* stageReflCb = nullptr;
    UINT reflCbFirst = 0;
    uint32_t dumpReflGame = 0, dumpReflHull = 0;
    const char* reflWhy = nullptr;
    // Separate mode. This frame: the main-pass draws given the own discard (hull, decals), and the decals re-issued.
    uint32_t ownDraws = 0, ownDecals = 0, decals = 0;
    // The capture frame only: the game's G-buffer RT0 and eye depth (references from the seed to the frame's end), and
    // their staging copies after the eye's G-buffer.
    ID3D11Texture2D* gameRt0 = nullptr;
    ID3D11Texture2D* gameDepth = nullptr;
    ID3D11Texture2D* stageGbuf0 = nullptr;
    ID3D11Texture2D* stageMainDepth = nullptr;
    // Step 3 (separate). Kept across frames (remade on a size or format change): the tonemap re-issue's target T (R8G8B8A8
    // typeless: an RTV in the game's own view format, a UNORM SRV), the cover C and the private depth beside it Z.
    ID3D11Texture2D* tone = nullptr;
    ID3D11RenderTargetView* toneRtv = nullptr;
    ID3D11ShaderResourceView* toneSrv = nullptr;
    DXGI_FORMAT toneRtvFmt = DXGI_FORMAT_UNKNOWN;
    uint32_t toneW = 0, toneH = 0;
    ID3D11Texture2D* cover = nullptr;
    ID3D11ShaderResourceView* coverSrv = nullptr;
    ID3D11UnorderedAccessView* coverUav = nullptr;
    ID3D11Texture2D* coverZ = nullptr;
    ID3D11ShaderResourceView* coverZSrv = nullptr;
    ID3D11UnorderedAccessView* coverZUav = nullptr;
    // Build A': the cover pass's box of the shown hull (kBoxBytes), its readback ring, the last box read (render px, x1/y1
    // exclusive; empty when x1 is 0) and the frame it was drawn on; the probe epoch (a box older than it is not used); and this
    // frame's rectangle, decided at the eye's first re-issue (rectOn false: whole eye).
    ID3D11Buffer* box = nullptr;
    ID3D11UnorderedAccessView* boxUav = nullptr;
    Readback boxRing[kRing];
    uint32_t boxRect[4] = {};
    uint32_t boxFrame = 0;
    bool haveBox = false;
    uint32_t rectEpoch = 0;
    uint32_t rectFrame = ~0u;
    bool rectOn = false;
    D3D11_RECT rect{};
    // Build B: the prefill's copy P (R32_FLOAT) and the reset's mark M (R8_UNORM), kept across frames at the eye's render size;
    // this frame: prefilled (the reset still owed), the rectangle both ran in (preRectOn false: the whole eye), the clear stencil.
    ID3D11Texture2D* pre = nullptr;
    ID3D11RenderTargetView* preRtv = nullptr;
    ID3D11ShaderResourceView* preSrv = nullptr;
    ID3D11Texture2D* preMark = nullptr;
    ID3D11RenderTargetView* preMarkRtv = nullptr;
    ID3D11ShaderResourceView* preMarkSrv = nullptr;
    uint32_t preW = 0, preH = 0;
    bool prefilled = false, preRectOn = false;
    D3D11_RECT preRect{};
    uint8_t clearStencil = 0;
    // Shader views over the private depth and the game's eye depth, kept while the texture is the same (a view holds its texture).
    ID3D11ShaderResourceView* privZSrv = nullptr;
    const void* privZOf = nullptr;
    ID3D11ShaderResourceView* mainZSrv = nullptr;
    const void* mainZOf = nullptr;
    // This frame: the game's eye depth (a reference from the seed to the frame's end). Across: the layer frame the tonemap
    // re-issue and the composite last ran on, the game's tonemap output then (identity), and whether the composite was applied.
    ID3D11Texture2D* mainDepth = nullptr;
    // The texture the last copy out of this eye's slot target (slotGame) went into: engine motion's overlay snapshot, the
    // decals' t3 (an identity only; hullLayerAfterCopy sets it).
    const void* overlayGame = nullptr;
    uint32_t toneFrame = ~0u, compFrame = ~0u;
    const void* toneOut = nullptr;
    bool compApplied = false;
    // The capture frame only: C and the composited output eye.
    ID3D11Texture2D* stageTone = nullptr;
    ID3D11Texture2D* stageComp = nullptr;
    bool dumpApplied = false;
};

// Step 3: the TAA's history and the composited eye, per VR eye (the temporal pass's index), at the upscaler's output size.
struct Comp {
    ID3D11Texture2D* hist[2] = {};
    ID3D11ShaderResourceView* histSrv[2] = {};
    ID3D11UnorderedAccessView* histUav[2] = {};
    ID3D11Texture2D* out = nullptr;
    ID3D11UnorderedAccessView* outUav = nullptr;
    // The TAA's 8x8 output tiles (kTileMarkHlsl, kTileListHlsl): this frame's mark, the 3-frame state, the list and its
    // indirect arguments.
    ID3D11Texture2D* tileMark = nullptr;
    ID3D11UnorderedAccessView* tileMarkUav = nullptr;
    ID3D11Texture2D* tileState = nullptr;
    ID3D11UnorderedAccessView* tileStateUav = nullptr;
    ID3D11Buffer* tileList = nullptr;
    ID3D11ShaderResourceView* tileListSrv = nullptr;
    ID3D11UnorderedAccessView* tileListUav = nullptr;
    ID3D11Buffer* tileArgs = nullptr;
    // Frame generation's S overlay over tiles (kSHlsl's hull_layer_s_tile_list_cs): the tile state read as a texture, S's tile list and
    // its indirect arguments; made on the first S that takes the layer (ensureSTiles).
    ID3D11ShaderResourceView* tileStateSrv = nullptr;
    ID3D11Buffer* sTileList = nullptr;
    ID3D11ShaderResourceView* sTileListSrv = nullptr;
    ID3D11UnorderedAccessView* sTileListUav = nullptr;
    ID3D11Buffer* sTileArgs = nullptr;
    bool sTilesTried = false, sTilesOk = false;
    bool tilesValid = false;     // the pair and the state keep the tiles' invariant (false after a bench DLSS frame)
    // The direct composite (kSharpDirectHlsl): a typed R8G8B8A8_UNORM UAV of the eye the composite writes, made for that
    // texture (directRes, an identity: the view holds the reference) and remade when the texture or its SRV's format changes;
    // directFail the reason it could not be made for directRes (kCm*), 0 when it was.
    ID3D11UnorderedAccessView* directUav = nullptr;
    const void* directRes = nullptr;
    DXGI_FORMAT directSrvFmt = DXGI_FORMAT_UNKNOWN;
    int directFail = 0;
    uint32_t outW = 0, outH = 0, renderW = 0, renderH = 0;
    uint32_t read = 0;            // hist[read] holds last frame's
    uint32_t lastFrame = ~0u;
    bool have = false;
    // Frame generation (docs "Frame generation integration"): this frame's composite, for the S the synthesizer builds right after it in
    // the same treat. sFrame: the layer frame of the last composite (hist[read] is its TAA result after the flip); sApplied: it went
    // onto the eye (the main pass discarded the own ship); sHull: the hull eye whose cover depth it read; sOutSrv: the view it wrote
    // through (an identity). world: the eye before the composite (HullLayerCompositeInputs::keepWorld), copied on frame worldFrame.
    uint32_t sFrame = ~0u;
    bool sApplied = false;
    int sHull = -1;
    float sTan[4] = {};
    float sDepthA = 0.0f, sDepthB = 0.0f;
    uint32_t sRegionX = 0, sRegionY = 0, sRenderW = 0, sRenderH = 0;
    const void* sOutSrv = nullptr;
    ID3D11Texture2D* world = nullptr;
    ID3D11ShaderResourceView* worldSrv = nullptr;
    D3D11_TEXTURE2D_DESC worldDesc{};
    DXGI_FORMAT worldSrvFmt = DXGI_FORMAT_UNKNOWN;
    uint32_t worldFrame = ~0u;
    // The S-first order (HullLayerCompositeInputs::sFirst): the composite ran its TAA and tile list on frame finishFrame and left the
    // hull's write onto the eye (direct, through directUav) to hullLayerCompositeFinish, after the S that reads the eye world-only.
    uint32_t finishFrame = ~0u;
};

// Step 3: a GPU time by D3D11 timestamps, read back a few frames later (never a stall); kTimerRing sets in flight.
struct GpuTimer {
    struct Set {
        ID3D11Query* disjoint = nullptr;
        ID3D11Query* t0 = nullptr;
        ID3D11Query* t1 = nullptr;
        bool pending = false;
        bool extra = false;   // a later part of a call already counted (timerBegin's `more`): its time adds, its sample does not
    };
    Set set[kTimerRing];
    int open = -1;
    double sumMs = 0.0;
    uint32_t samples = 0;
};

// Step 3: why a frame in separate mode kept the own ship in the main pass, and why an eye's composite declined.
enum Fallback : int { kFbFrameGen = 0, kFbOutside, kFbMaskPending, kFbMaskLarge, kFbStreak, kFallbacks };
enum Declined : int { kDcStale = 0, kDcSize, kDcGpu, kDeclines };
// Why an eye's composite went through c.out and two whole-eye copies rather than straight onto the eye: not applied this frame (the
// history kept warm), a dump or the TAA sequence reads c.out, and the reasons the eye's UAV could not be had.
enum CompMethod : int { kCmNotApplied = 0, kCmNeedsOut, kCmNoUav, kCmFormat, kCmTypedLoad, kCmView, kCmShader, kCmOff, kCompMethods };
// Frame generation (the "framegen" line): why an S was built without the hull layer drawn into it. not_applied: the main pass kept the
// own ship this frame (a fallback, the hold, a dump's reference frame), so S warps the game's ship with the world as before; no_composite:
// no composite ran for the eye this frame (asleep, declined); no_world_copy: S reads the eye the composite wrote and no world-only copy
// was kept; size: S is not the composite's size; unsupported: the shader, its constants or a typed UAV load of S's format are missing.
enum FgDecline : int { kFgNotApplied = 0, kFgNoComposite, kFgNoWorldCopy, kFgSize, kFgUnsupported, kFgDeclines };
// Frame generation (the "framegen" line's copy_why, 2026-11-11): why an applied composite whose eye S reads kept today's whole-eye copy
// rather than the S-first order (HullLayerCompositeInputs::sFirst). not_asked: the caller could not take that order (S does not read
// the composite's eye through the view it writes, or the hull bench captures the eye); not_direct: the composite went through c.out
// (a dump or the TAA sequence reads it, the eye's UAV could not be had, the A/B off); bench: the variant bench runs.
enum WorldCopyWhy : int { kWcNotAsked = 0, kWcNotDirect, kWcBench, kWorldCopyWhys };
// TEMPORARY A/B (2026-11-11): the pilot saw the planet through the wing from 5bf8f651 on, the build that added the direct
// composite, and saw it go away while a dump's frames took the copy path. Off: every applied composite takes the copy path.
// Ruled out as the cause (223a4cda: copy_why off=3396, the planet still showed); the cause was the tone's bloom. On again.
constexpr bool kDirectComposite = true;

struct Window {
    ULONGLONG startMs = 0;
    uint32_t frames = 0, eyeFrames = 0, hullSeen = 0, seeds = 0, noSeed = 0, noSlot = 0, noShader = 0, refused = 0;
    uint32_t reissued = 0, noSlotDraws = 0, clears = 0, filters = 0, readbacks = 0, readbackBusy = 0, terrainFrames = 0;
    uint32_t unknownPairs = 0, autoDumps = 0;
    // Station fixes: re-issues culled by the keep-own VS and those that needed the filter; frames the busy governor slept; stage
    // draws declined for targets not at the eye's render size; lighting draws whose scratch R16 RT1 was left unbound.
    uint32_t culledDraws = 0, filterDraws = 0, busyFrames = 0, offscreen = 0, nullRt1 = 0;
    // Night vision's draw: re-issues set up, and those that ran with EDVR's night-vision state (hullLayerNoteNightVisionState).
    uint32_t nightVision = 0, nightVisionEdvr = 0;
    uint32_t subst[kStages] = {}, noCounterpart[kStages] = {}, ended[kEnds] = {};
    std::vector<uint32_t> own, filtered, hullDraws, lightDraws;
    // Separate mode (the "main pass" line): main-pass draws given the own discard (hull, decals), draws in the window that
    // could not take it (no slot target, no variant), eye-frames whose main pass kept the own ship (no mask: the seed was
    // refused, or had no slot target or shader), and the own decals re-issued into the private layer (and those declined:
    // no slot exported).
    uint32_t ownDraws = 0, ownDecals = 0, noVariant = 0, fallbackFrames = 0, decalsReissued = 0, decalNoSlot = 0;
    uint32_t copiesMirrored = 0, decalSrvSwaps = 0;   // the decals' base: game copies mirrored, decal re-issues with a swapped SRV
    // The decals' t3 (engine motion's overlay snapshot): re-issues that read its private copy, that read nothing there (MRT6,
    // its only output, not bound in the re-issue), and that kept the main pass's (MRT6 bound, no private copy made).
    uint32_t overlaySwapped = 0, overlayUnbound = 0, overlayKept = 0;
    // The tonemap re-issue's t2 (the game's bloom): re-issues that read the zeroed texture there, and those that kept the game's
    // with the last reason.
    uint32_t bloomZeroed = 0, bloomDeclined = 0;
    const char* bloomWhy = nullptr;
    // Step 3 (the "composite" line): frames decided with the discard on, and kept by reason; eye-frames composited (the
    // discard on) or made ready (a fallback frame); misses by stage (an eye-frame whose seed failed is counted, not held);
    // composites declined; tonemap re-issues made and declined; history restarts; eyes matched by index, not identity;
    // the largest own mask a large-mask fallback saw.
    uint32_t separateFrames = 0, fallback[kFallbacks] = {};
    uint32_t compositedEyes = 0, readyEyes = 0, failSeed = 0, failLit = 0, failTone = 0, failComposite = 0;
    uint32_t declined[kDeclines] = {};
    uint32_t toned = 0, toneDeclined = 0, historyResets = 0, byIndex = 0, ownMax = 0;
    uint32_t siteAfterResolve = 0, siteUpscalerOut = 0;   // applied composites by where they landed (HullLayerCompositeInputs)
    // The composite's method (the "composite method" line): eye-composites written straight onto the eye (direct) and through
    // c.out (copy) by reason (kCm*), and those through the bench's second DLSS (its own copy path).
    uint32_t compDirect = 0, compCopy[kCompMethods] = {}, compBenchDlss = 0;
    // Frame generation (the "framegen" line): S builds the synthesizer was about to make in separate mode (hullLayerSInput), those
    // with the hull layer drawn in (hullLayerCompositeS), their world-only input by source (the eye itself before the hull's write,
    // the S-first order; the copy kept before the composite; or the eye the composite did not write), the copies made and why, the
    // S-first composites finished after S and those whose finish never came, the overlays by pass (over tiles, whole eye), and the
    // builds left without the layer by reason.
    uint32_t fgS = 0, fgHullInS = 0, fgWorldBefore = 0, fgWorldCopy = 0, fgWorldDirect = 0, fgCopies = 0, fgDeclined[kFgDeclines] = {};
    uint32_t fgCopyWhy[kWorldCopyWhys] = {}, fgFinished = 0, fgFinishLost = 0, fgOverTiles = 0, fgOverWhole = 0;
    // The AO chain (the "ao" line): eye-frames with a private G-buffer, and those whose whole chain and apply were re-issued;
    // step dispatches and apply draws re-issued; eye-frames where the game ran a step (or the apply) for the eye and it was
    // not re-issued, by step; chain dispatches whose eye could not be named (none of its resources is an eye's).
    uint32_t aoEyeFrames = 0, aoFull = 0, aoDispatches = 0, aoApplied = 0, aoMissed[kAoSteps + 1] = {}, aoNoEye = 0;
    uint32_t reflDispatches = 0, reflNoEye = 0, reflDeclined = 0;
    uint32_t reflU1Clears = 0, reflU1GameClears = 0;   // eye-frames whose private u1 was cleared before the re-issue; the game's clears of its own u1
    // The rig seed (the "own seed" line): eye-frames seeded from the rig probe's own slots and from the seed draw (the fallback),
    // the hold restarted (the own rigs changed); the rig seeds' slot counts, and from their readbacks the radius and reach (metres).
    uint32_t rigSeeds = 0, drawSeeds = 0, holdResets = 0;
    std::vector<uint32_t> rigSlots, rigRadius, rigReach;
    // Build A' (the "rect" line): eye-frames decided, those scissored and the sum of their rectangles' share of the eye; those
    // left whole by reason; boxes read back (and empty ones); re-issues scissored, those intersected with the game's own
    // scissor, and those not scissored for want of a clone.
    uint32_t rectDecided = 0, rectFrames = 0, wholeNoBox = 0, wholeEmpty = 0, wholeProbe = 0, wholeStale = 0;
    double rectShare = 0.0;
    uint32_t boxesRead = 0, boxesEmpty = 0, scissorDraws = 0, scissorGame = 0, scissorNoState = 0;
    // Build B (the "prefill" line): eye-frames seeded, prefilled (and of those in a rectangle), reset; prefilled but never reset
    // (the frame moved on first); not prefilled by reason.
    uint32_t preSeeds = 0, preFrames = 0, preRect = 0, preResets = 0, preUnreset = 0;
    uint32_t preSkipMode = 0, preSkipFormat = 0, preSkipView = 0, preSkipTargets = 0, preSkipShader = 0;
};

// Separate mode: what hullLayerBeforeMainDraw bound over the game's issue, which hullLayerAfterMainDraw puts back.
struct MainPending {
    bool active = false;
    ID3D11PixelShader* ps = nullptr;          // the substitute engine motion had bound (a reference)
    ID3D11ShaderResourceView* srv = nullptr;  // the game's PS t127 (a reference, or null)
};

// The pending re-issue: what Begin bound over, which End puts back.
struct Pending {
    bool active = false;
    int eye = -1;
    int stage = -1;   // -1: a hull draw
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11ShaderResourceView* srv[kSrvSlots] = {};
    bool srvSwapped = false;
    const void* hdrGame = nullptr;   // a lighting draw's RT0 (the game's HDR), for the dump
    const void* toneOut = nullptr;   // the tonemap re-issue (stage kToneStage): the game's tonemap output (identity)
    // A hull re-issue with the keep-own VS: the vertex shader bound before it and the game's VS t127 (references), put back by End.
    ID3D11VertexShader* vs = nullptr;
    ID3D11ShaderResourceView* vsMask = nullptr;
    bool keepOwn = false;
    // Build A': the re-issue ran scissored to the eye's rectangle; the game's rasterizer state (a reference) and scissor rects.
    bool scissored = false;
    ID3D11RasterizerState* raster = nullptr;
    UINT scissorCount = 0;
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
};

// The pending AO step re-issue: the compute slots Begin bound over, which End puts back.
struct DispatchPending {
    bool active = false;
    int reflEye = -1;   // a reflection step's re-issue: its eye, for the capture frame's private stage after it
    ID3D11ShaderResourceView* srv[kCsSrvSlots] = {};
    ID3D11UnorderedAccessView* uav[kCsUavSlots] = {};
};

// The device objects the passes need, made once at the first seed.
struct Gpu {
    bool tried = false, ok = false;
    ID3D11ComputeShader* seedCs = nullptr;
    ID3D11ComputeShader* countCs = nullptr;
    ID3D11VertexShader* filterVs = nullptr;
    ID3D11PixelShader* filterPs = nullptr;
    ID3D11DepthStencilState* depthAlways = nullptr;
    ID3D11RasterizerState* noCull = nullptr;
    ID3D11Buffer* seedCb = nullptr;
    ID3D11Buffer* passCb = nullptr;
    ID3D11Buffer* inst = nullptr;   // the seed's instance window, kMaxSeedInstances * 8 bytes, raw
    ID3D11ShaderResourceView* instSrv = nullptr;
    ID3D11Buffer* pool = nullptr;   // the pool's copy at the seed, raw
    ID3D11ShaderResourceView* poolSrv = nullptr;
    uint32_t poolBytes = 0;
};

// Step 3's device objects, made once at the first tonemap re-issue (apart from Gpu, so a failure here leaves Steps 1-2 running).
struct CompGpu {
    bool tried = false, ok = false;
    ID3D11ComputeShader* coverCs = nullptr;
    ID3D11ComputeShader* taaCs = nullptr;
    ID3D11ComputeShader* sharpCs = nullptr;
    ID3D11ComputeShader* tileMarkCs = nullptr;
    ID3D11ComputeShader* tileListCs = nullptr;
    ID3D11ComputeShader* sharpDirectCs = nullptr;   // made apart (ensureDirectCs): a failure leaves the copy path
    bool directTried = false, directOk = false;
    // Frame generation: the hull layer into S (kSHlsl, made apart by ensureSCs) and its constants; the typed UAV load of S's format,
    // checked once a format (sFmt).
    ID3D11ComputeShader* sCs = nullptr;
    ID3D11Buffer* sCb = nullptr;
    bool sTried = false, sOk = false;
    // The overlay over tiles (kSHlsl's other two entries); null when either failed: the whole-eye pass runs.
    ID3D11ComputeShader* sTilesCs = nullptr;
    ID3D11ComputeShader* sTileListCs = nullptr;
    DXGI_FORMAT sFmt = DXGI_FORMAT_UNKNOWN;
    bool sFmtOk = false;
    ID3D11SamplerState* linear = nullptr;
    ID3D11Buffer* coverCb = nullptr;
    ID3D11Buffer* taaCb = nullptr;
    ID3D11Buffer* sharpCb = nullptr;
};

// Build B's device objects, made once at the first prefill (apart from Gpu, so a failure here leaves the layer running unprefilled).
struct PrefillGpu {
    bool tried = false, ok = false;
    ID3D11PixelShader* prefillPs = nullptr;
    ID3D11PixelShader* markPs = nullptr;
    ID3D11PixelShader* resetPs = nullptr;
    ID3D11DepthStencilState* resetDss = nullptr;   // depth ALWAYS written, stencil ALWAYS REPLACE (the ref: the clear stencil)
};

// Build A': the game's rasterizer states (references held, so an address is not reused) and their clones with ScissorEnable.
struct ScissorState {
    ID3D11RasterizerState* game = nullptr;   // null: the default state
    ID3D11RasterizerState* clone = nullptr;
};

std::string g_mode = "off";
ID3D11Device* g_device = nullptr;   // a reference
std::vector<Mirror> g_mirrors;
std::vector<ViewEntry> g_views;
ClearValue g_clearValues[8];
uint32_t g_clearNext = 0;
Eye g_eyes[2];
int g_inEye = -1;      // the eye whose private G-buffer window is open
int g_stageEye = -1;   // the eye whose screen-space stage ran last this frame
Pending g_pending;
MainPending g_main;
DispatchPending g_dispatch;
Gpu g_gpu;
Window g_win;
bool g_noCpFrame[kStages] = {};
bool g_terrainFrame = false, g_seededFrame = false;
uint32_t g_frame = 0;
uint32_t g_lastOwn = 0;
bool g_haveOwn = false;
uint64_t g_unknownPairs[kMaxUnknownPairs][2] = {};
uint32_t g_unknownCount = 0;
bool g_saidMirrorCap = false, g_saidViewCap = false, g_saidCreateFail = false, g_saidMsaa = false, g_saidNoSlot = false, g_saidRefused = false,
     g_saidPool = false, g_saidUnknownCap = false, g_saidReadback = false, g_saidNoVariant = false;
// The dump: armed -> the next frame is captured -> written at the boundary after it.
enum class Dump : uint8_t { Idle, Armed, Capturing };
Dump g_dump = Dump::Idle;
wchar_t g_dumpStamp[32] = L"";
uint32_t g_dumpFrame = 0;
// The auto dump (session-wide).
uint32_t g_autoStreak = 0, g_autoDumps = 0;
ULONGLONG g_lastAutoMs = 0;
// Step 3 (separate): this frame's decision (made at the boundary before it), the hold after a miss, and the GPU objects.
bool g_discard = false;
uint32_t g_hold = 0;
Comp g_comp[2];
// The zeroed stand-in for the game's bloom at the tonemap re-issue's t2: the game texture's desc and view, cleared to zero once
// when made (remade only when the game's bloom changes size or format). Both eyes read it; nothing writes it after the clear.
struct ZeroBloom {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
};
ZeroBloom g_zeroBloom;
CompGpu g_cgpu;
PrefillGpu g_pgpu;
std::vector<ScissorState> g_scissorStates;
GpuTimer g_toneTimer, g_compTimer;
bool g_saidFrameGenS = false, g_saidMaskLarge = false, g_saidToneDecline = false, g_saidDecline[kDeclines] = {};
// Station fixes: the busy governor (decided at the frame boundary, before the next frame's draws), the calm frames counted while it
// sleeps and the limit it slept at; the keep-own VS's refusals said once.
bool g_busy = false;
uint32_t g_calm = 0, g_busyLimit = kBusyDraws;
// A dump armed while asleep wakes the layer (governBusy): one warm-up frame (the private copies, aged out while asleep, are made
// again and see the game's clears), then the capture frame and, in separate mode, the TAA sequence; then it sleeps again.
bool g_dumpWoke = false;
bool g_saidKeepOwn = false, g_saidOffscreen[kStages] = {};

// ---- dump v2 (docs "Dump v2"): the stage times, the lighting window's draws, the reference frame, the TAA sequence -----------

// GPU time by stage, both modes, always: on every kStageEvery-th frame a timestamp pair brackets each piece of the layer's own GPU
// work (a re-issued draw, a pass, a dispatch), summed by stage and read back a few frames later (never a stall). The pairs time the
// layer's work in place between the game's, so a stage's sum is the GPU time its items took, overlap with the game's work aside.
// The CPU time (bracketing each item, from the hook's entry) and the item counts are kept on every frame.
enum TStage : int { kTsGbuf = 0, kTsMask, kTsScreen, kTsLight, kTsTone, kTsTaa, kTStages };   // the line's gbuffer, mask_filter, screen, light, tone, taa
constexpr uint32_t kStageEvery = 8;      // one frame in eight is timed on the GPU
// Timestamp pairs a timed frame may use: a pair an item. 4096, not 1024: a station re-issues 1,200-3,400 items a frame over both eyes
// (the culled draws are still re-issued), and a pair per run of one stage would also time the game's draws between the items.
// The queries are made on first use, so a frame that needs fewer makes fewer.
constexpr uint32_t kStagePairCap = 4096;
constexpr uint32_t kStageSets = 3;       // timed frames in flight
struct StageSet {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* q[2 * kStagePairCap] = {};   // made on first use, kept
    uint8_t stage[kStagePairCap] = {};
    uint32_t used = 0, overflow = 0;
    uint32_t frame = 0;   // the layer frame it times (the variant bench reads its sums by frame)
    bool open = false, pending = false;
};
struct StageTimes {
    StageSet set[kStageSets];
    int cur = -1;       // the set this frame records into (-1: not a timed frame, or no set free)
    int scope = -1;     // the open item's pair, or -1
    int cpuStage = -1;  // the open item's stage, and its CPU start
    LARGE_INTEGER cpuStart{};
    // The window: each timed frame's sum by stage, and its total (ms); the items and CPU ticks of every frame.
    std::vector<float> ms[kTStages + 1];
    uint32_t items[kTStages] = {};
    int64_t cpuTicks[kTStages] = {};
    uint32_t timed = 0, overflow = 0, busy = 0, disjoint = 0;
};
StageTimes g_st;
LARGE_INTEGER g_entryQpc{};   // the hook's entry (hullLayerBegin, hullLayerBeginDispatch): an item's CPU time starts there
// The variant bench (below, "the variant bench"): while it runs every frame is timed, and a timed frame's sums go to it by frame;
// g_benchSleep puts the layer to sleep for the original variants (no seed, discard, re-issue, stage or composite), without the
// busy governor's hold or its count.
bool g_benchTimeAll = false, g_benchSleep = false;
bool g_benchNoAa = false;          // the variant bench's sep_noaa: the TAA runs with its history off (the 3x3 reconstruct, then the sharpen)
uint32_t g_benchEvictions = 0;     // private copies dropped (evictStale) while the bench runs
int g_benchDlss = 0;               // the variant bench's second DLSS on the hull: 0 none, else its variant (dlss_full, dlss_sub)
// The variant bench's A/B variants: sep_taa with one feature off. sep_norefl: the reflection re-issue (beginReflStep);
// sep_nopre: Build B's prefill and reset (prefillEye); sep_norect: Build A''s rectangle (eyeRect: the re-issues and the cover
// whole-eye). Each place that would have run the feature calls benchNoteOff, so a row whose feature never ran says so.
bool g_benchNoRefl = false, g_benchNoPre = false, g_benchNoRect = false;
void benchNoteOff();
void benchNoteStages(uint32_t frame, const double* sums);
// Right after an eye's composite, before its history pair flips: the bench's rect cover, mask and applied mark.
void benchAfterComposite(ID3D11DeviceContext* ctx, int eye, const Eye& e, const Comp& c, const HullLayerCompositeInputs& in, bool applied);

// The lighting window's draws on the dump frame (eye_<stamp>_HullLights.json): every draw between an eye's shadow mask and its sky
// that writes the eye's HDR, and every stencil-only draw there (a light volume's stencil pass), with what the layer did with it.
struct LightRec {
    int eye = -1;
    uint32_t seq = 0;   // the draw's ordinal among the frame's draws the layer saw
    uint64_t vs = 0, ps = 0;
    char kind = 0;
    uint32_t count = 0, instances = 0, startInstance = 0;
    bool hdr = false, stencilOnly = false, reissued = false, known = false, stagePair = false;
    uint8_t stencilRef = 0;
};
constexpr size_t kMaxLightRecs = 4096;
std::vector<LightRec> g_lights;
uint32_t g_drawSeq = 0;
bool g_lightsCapped = false;

// Separate mode's dump: the capture frame is a reference frame (the main pass keeps the own ship, the layer runs, the composite is
// made and not applied), so GameHdrLit holds the game's own hull beside HullHdr. g_refWould: that frame would have discarded.
bool g_refFrame = false, g_refWould = false;

// The reflection pass's u1 (an R16 target, also the lighting's RT1): the colour the game last cleared its own to, when it does
// (hullLayerNoteClearRtv), which the private copy is cleared to before the first re-issue an eye-frame; 0 until one is seen.
float g_reflU1Clear[4] = {};
bool g_reflU1ClearSeen = false;

// The TAA sequence (separate mode, eye 0, from the dump's capture frame): per frame the cover and its depth at render size (the
// eye's region, cropped on the CPU to the cover's extent), the composite at output size (cropped likewise), the TAA's constants,
// and on the first frame the history it read. Staged into kSeqRing sets and written kSeqLag frames later, so the GPU is not waited
// on and the memory stays at kSeqRing sets whatever the length.
constexpr int kSeqFrames = 16;
constexpr uint32_t kSeqRing = 3;
constexpr uint32_t kSeqLag = 2;
constexpr uint64_t kSeqBudgetBytes = 768ull << 20;   // the ring and the history's staging, at most
constexpr uint32_t kSeqMargin = 4;                    // render pixels about the cover's extent (the TAA's 3x3 and its depth)
constexpr uint32_t kSeqHistMargin = 64;               // output pixels about frame 0's composite crop (the history's reprojection)
struct SeqSet {
    ID3D11Texture2D* cover = nullptr;
    ID3D11Texture2D* depth = nullptr;
    ID3D11Texture2D* comp = nullptr;
    int index = -1;
    bool pending = false;
};
struct SeqFrame {
    int index = 0;
    uint32_t frame = 0;
    uint32_t renderW = 0, renderH = 0, outW = 0, outH = 0, regionX = 0, regionY = 0;
    float jitter[2] = {}, tanNow[4] = {}, tanPrev[4] = {}, headDelta[9] = {}, headTrans[3] = {}, depthA = 0.0f, depthB = 0.0f;
    bool havePrev = false, haveDelta = false, useTrans = false, history = false, applied = false, afterUiResolve = false;
    uint32_t crop[4] = {}, compCrop[4] = {};   // x, y, w, h written
    const char* status = "staged";
};
struct Seq {
    bool active = false;
    wchar_t stamp[32] = L"";
    int staged = 0;
    uint32_t startFrame = 0, lastFrame = ~0u;
    SeqSet ring[kSeqRing];
    ID3D11Texture2D* hist = nullptr;   // frame 0's history in (the TAA's last output), when it had one
    uint32_t histCrop[4] = {};
    const char* histStatus = "none";
    std::vector<SeqFrame> frames;
    uint64_t bytes = 0;
    const char* end = nullptr;
};
Seq g_seq;
void releaseStageTimes();   // the stage times' queries (below, with the stage functions)
void releaseSeq();          // the sequence's staging and frames (below, with the dump)

template <class T>
void releaseRef(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

const void* resourceOf(ID3D11View* view) {
    if (!view) return nullptr;
    ID3D11Resource* res = nullptr;
    view->GetResource(&res);
    if (res) res->Release();   // an identity only: the view keeps the resource alive while it is bound
    return res;
}

void releaseTargets(ID3D11RenderTargetView** rtv, ID3D11DepthStencilView*& dsv) {
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) releaseRef(rtv[i]);
    releaseRef(dsv);
}

UINT boundCount(ID3D11RenderTargetView* const* rtv) {
    UINT n = 0;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        if (rtv[i]) n = i + 1;
    return n;
}

void releaseStaging(Eye& e) {
    releaseRef(e.stageHull);
    releaseRef(e.stageGame);
    releaseRef(e.stageDepth);
    releaseRef(e.stageStats);
    releaseRef(e.stageGbuf0);
    releaseRef(e.stageMainDepth);
    releaseRef(e.stageTone);
    releaseRef(e.stageComp);
    releaseRef(e.stageGameRefl);
    releaseRef(e.stageHullRefl);
    releaseRef(e.stageGameStencil);
    releaseRef(e.stageHullStencil);
    releaseRef(e.stageGameT11);
    releaseRef(e.stageHullT11);
    releaseRef(e.stageReflCb);
    e.reflCbFirst = 0;
    e.dumpReflGame = e.dumpReflHull = 0;
    e.reflWhy = nullptr;
    e.dumpHull = e.dumpLight = 0;
    e.dumpApplied = false;
}

void releaseSeedViews(Eye& e) {
    for (auto*& v : e.privRtv) releaseRef(v);
    releaseRef(e.privSlot);
    releaseRef(e.privDsv);
    releaseRef(e.gameRt0);
    releaseRef(e.gameDepth);
    releaseRef(e.mainDepth);
}

// Step 3: the eye's tonemap target, cover and depth views.
void releaseTone(Eye& e) {
    releaseRef(e.toneRtv);
    releaseRef(e.toneSrv);
    releaseRef(e.tone);
    releaseRef(e.coverSrv);
    releaseRef(e.coverUav);
    releaseRef(e.cover);
    releaseRef(e.coverZSrv);
    releaseRef(e.coverZUav);
    releaseRef(e.coverZ);
    releaseRef(e.privZSrv);
    releaseRef(e.mainZSrv);
    e.privZOf = e.mainZOf = nullptr;
    e.toneRtvFmt = DXGI_FORMAT_UNKNOWN;
    e.toneW = e.toneH = 0;
    // Build A': the box is the cover's, so it goes with it; the next rectangles wait for a box of the remade cover.
    releaseRef(e.boxUav);
    releaseRef(e.box);
    for (auto& r : e.boxRing) {
        releaseRef(r.staging);
        r.pending = false;
    }
    e.haveBox = false;
    e.rectFrame = ~0u;
    e.rectOn = false;
}

// Build B: the eye's prefill copy and mark.
void releasePrefill(Eye& e) {
    releaseRef(e.preRtv);
    releaseRef(e.preSrv);
    releaseRef(e.pre);
    releaseRef(e.preMarkRtv);
    releaseRef(e.preMarkSrv);
    releaseRef(e.preMark);
    e.preW = e.preH = 0;
    e.prefilled = false;
}

// Build A': the rasterizer clones.
void releaseScissorStates() {
    for (auto& s : g_scissorStates) {
        releaseRef(s.game);
        releaseRef(s.clone);
    }
    g_scissorStates.clear();
}

void releaseComp(Comp& c) {
    for (int i = 0; i < 2; ++i) {
        releaseRef(c.histSrv[i]);
        releaseRef(c.histUav[i]);
        releaseRef(c.hist[i]);
    }
    releaseRef(c.outUav);
    releaseRef(c.out);
    releaseRef(c.tileMarkUav);
    releaseRef(c.tileMark);
    releaseRef(c.tileStateUav);
    releaseRef(c.tileState);
    releaseRef(c.tileListSrv);
    releaseRef(c.tileListUav);
    releaseRef(c.tileList);
    releaseRef(c.tileArgs);
    releaseRef(c.tileStateSrv);
    releaseRef(c.sTileListSrv);
    releaseRef(c.sTileListUav);
    releaseRef(c.sTileList);
    releaseRef(c.sTileArgs);
    releaseRef(c.directUav);
    releaseRef(c.worldSrv);
    releaseRef(c.world);
    c = Comp{};
}

void releaseTimer(GpuTimer& t) {
    for (auto& s : t.set) {
        releaseRef(s.disjoint);
        releaseRef(s.t0);
        releaseRef(s.t1);
        s.pending = false;
    }
    t = GpuTimer{};
}

void releaseEyeGpu(Eye& e) {
    releaseSeedViews(e);
    releaseRef(e.maskSrv);
    releaseRef(e.maskUav);
    releaseRef(e.mask);
    e.maskRecords = 0;
    releaseRef(e.statsUav);
    releaseRef(e.stats);
    for (auto& r : e.ring) {
        releaseRef(r.staging);
        r.pending = false;
    }
    releaseRef(e.slotSrv);
    e.slotSrvOf = nullptr;
    releaseTone(e);
    releasePrefill(e);
    releaseStaging(e);
}

// The per-frame fields back to the start of a frame (the identity and the GPU objects stay).
void resetFrame(Eye& e) {
    releaseSeedViews(e);
    if (e.prefilled) ++g_win.preUnreset;   // Build B: never (finishGbuffer resets it); counted so a hole in that shows
    e.prefilled = false;
    e.seen = false;
    e.shell = false;
    e.gb = Gb::Before;
    e.hull = e.noSlotDraws = e.lightDraws = e.lightSpan = 0;
    e.lightOpen = false;
    e.culled = e.filterDraws = e.gbufHull = 0;
    e.unknownOpen = false;
    e.pendingCount = 0;
    e.decalOpen = false;
    e.stagesDone = 0;
    e.hdrGame = e.slotGame = nullptr;
    for (auto& g : e.gbufGame) g = nullptr;
    e.copyGame = nullptr;
    for (auto& o : e.aoOut) o = nullptr;
    e.aoOutCount = e.aoDone = e.aoSeen = 0;
    e.reflSeen = 0;
    e.reflCleared = false;
    e.clearDepth = 0.0f;
    e.records = e.seedInstances = 0;
    e.ownDraws = e.ownDecals = e.decals = 0;
}

void releaseGpu() {
    releaseRef(g_zeroBloom.srv);
    releaseRef(g_zeroBloom.tex);
    releaseRef(g_gpu.seedCs);
    releaseRef(g_gpu.countCs);
    releaseRef(g_gpu.filterVs);
    releaseRef(g_gpu.filterPs);
    releaseRef(g_gpu.depthAlways);
    releaseRef(g_gpu.noCull);
    releaseRef(g_gpu.seedCb);
    releaseRef(g_gpu.passCb);
    releaseRef(g_gpu.instSrv);
    releaseRef(g_gpu.inst);
    releaseRef(g_gpu.poolSrv);
    releaseRef(g_gpu.pool);
    g_gpu = Gpu{};
    releaseRef(g_pgpu.prefillPs);
    releaseRef(g_pgpu.markPs);
    releaseRef(g_pgpu.resetPs);
    releaseRef(g_pgpu.resetDss);
    g_pgpu = PrefillGpu{};
    releaseRef(g_cgpu.coverCs);
    releaseRef(g_cgpu.taaCs);
    releaseRef(g_cgpu.sharpCs);
    releaseRef(g_cgpu.tileMarkCs);
    releaseRef(g_cgpu.tileListCs);
    releaseRef(g_cgpu.sharpDirectCs);
    releaseRef(g_cgpu.sCs);
    releaseRef(g_cgpu.sCb);
    releaseRef(g_cgpu.sTilesCs);
    releaseRef(g_cgpu.sTileListCs);
    releaseRef(g_cgpu.linear);
    releaseRef(g_cgpu.coverCb);
    releaseRef(g_cgpu.taaCb);
    releaseRef(g_cgpu.sharpCb);
    g_cgpu = CompGpu{};
    for (auto& c : g_comp) releaseComp(c);
    releaseScissorStates();
    releaseTimer(g_toneTimer);
    releaseTimer(g_compTimer);
    releaseStageTimes();
    releaseSeq();
}

void dropViewsOf(const void* game) {
    for (size_t i = 0; i < g_views.size();) {
        if (g_views[i].game == game) {
            if (g_views[i].priv) g_views[i].priv->Release();
            g_views.erase(g_views.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void releaseAll() {
    hullRigProbeRelease();   // its staging is made on this device
    for (auto& v : g_views)
        if (v.priv) v.priv->Release();
    g_views.clear();
    for (auto& m : g_mirrors)
        if (m.priv) m.priv->Release();
    g_mirrors.clear();
    for (auto& e : g_eyes) {
        releaseEyeGpu(e);
        e = Eye{};
    }
    releaseGpu();
    g_inEye = g_stageEye = -1;
    if (g_device) { g_device->Release(); g_device = nullptr; }
    g_dump = Dump::Idle;
    g_autoStreak = 0;
    g_haveOwn = false;
    g_discard = false;
    g_hold = 0;
    g_busy = false;
    g_calm = 0;
    g_lights.clear();
    g_lightsCapped = false;
    g_refFrame = false;
}

bool ensureDevice(ID3D11DeviceContext* ctx) {
    if (!g_device && ctx) ctx->GetDevice(&g_device);
    return g_device != nullptr;
}

Mirror* findMirror(const void* game) {
    for (auto& m : g_mirrors)
        if (m.game == game) {
            m.lastSeen = g_frame;
            return &m;
        }
    return nullptr;
}

void dropMirror(const void* game) {
    dropViewsOf(game);
    for (size_t i = 0; i < g_mirrors.size(); ++i) {
        if (g_mirrors[i].game == game) {
            if (g_mirrors[i].priv) g_mirrors[i].priv->Release();
            g_mirrors.erase(g_mirrors.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

Mirror* makeMirror(const void* game, const D3D11_TEXTURE2D_DESC& d) {
    if (g_mirrors.size() >= kMaxMirrors) {
        if (!g_saidMirrorCap) {
            g_saidMirrorCap = true;
            Log::get().note("hull layer: %u private copies held, the cap; a %ux%u format %u target gets none (said once)",
                            static_cast<unsigned>(g_mirrors.size()), d.Width, d.Height, static_cast<unsigned>(d.Format));
        }
        return nullptr;
    }
    if (d.SampleDesc.Count != 1) {
        if (!g_saidMsaa) {
            g_saidMsaa = true;
            Log::get().note("hull layer: a %ux%u format %u target is multisampled (%u); no private copy is made (said once)", d.Width, d.Height,
                            static_cast<unsigned>(d.Format), d.SampleDesc.Count);
        }
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC pd = d;
    pd.Usage = D3D11_USAGE_DEFAULT;
    pd.CPUAccessFlags = 0;
    pd.MiscFlags &= ~static_cast<UINT>(D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_GDI_COMPATIBLE |
                                       D3D11_RESOURCE_MISC_SHARED_NTHANDLE);
    ID3D11Texture2D* priv = nullptr;
    const HRESULT hr = g_device->CreateTexture2D(&pd, nullptr, &priv);
    if (FAILED(hr) || !priv) {
        if (!g_saidCreateFail) {
            g_saidCreateFail = true;
            Log::get().note("hull layer: the private copy of a %ux%u format %u target (bind 0x%X) could not be made (hr 0x%08lX; said once)", d.Width,
                            d.Height, static_cast<unsigned>(d.Format), d.BindFlags, static_cast<unsigned long>(hr));
        }
        return nullptr;
    }
    Mirror m;
    m.game = game;
    m.desc = d;
    m.priv = priv;
    m.lastSeen = g_frame;
    g_mirrors.push_back(m);
    Log::get().note("hull layer: private copy made of a %ux%u format %u target (bind 0x%X, %u held)", d.Width, d.Height,
                    static_cast<unsigned>(d.Format), d.BindFlags, static_cast<unsigned>(g_mirrors.size()));
    return &g_mirrors.back();
}

// The private counterpart of a game view: the mirror of its texture (made when `make` and missing), and a view over the
// mirror with the game view's own desc. Null when the view is not over a 2D texture, has no mirror and none is to be
// made, or a create failed. The view is the layer's (g_views holds the reference).
ID3D11View* privateView(ViewKind kind, ID3D11View* gameView, bool make) {
    if (!gameView || !g_device) return nullptr;
    ID3D11Resource* res = nullptr;
    gameView->GetResource(&res);
    if (!res) return nullptr;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        res->Release();
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC d{};
    static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
    const void* game = res;
    res->Release();
    Mirror* m = findMirror(game);
    if (m && std::memcmp(&m->desc, &d, sizeof d) != 0) {
        // The game's texture at this address is not the one mirrored (freed and remade): the old copy goes.
        dropMirror(game);
        m = nullptr;
    }
    if (!m && make) m = makeMirror(game, d);
    if (!m) return nullptr;
    ViewDesc vd;
    std::memset(&vd, 0, sizeof vd);
    switch (kind) {
    case ViewKind::Rtv: static_cast<ID3D11RenderTargetView*>(gameView)->GetDesc(&vd.rtv); break;
    case ViewKind::Dsv: static_cast<ID3D11DepthStencilView*>(gameView)->GetDesc(&vd.dsv); break;
    case ViewKind::Srv: static_cast<ID3D11ShaderResourceView*>(gameView)->GetDesc(&vd.srv); break;
    case ViewKind::Uav: static_cast<ID3D11UnorderedAccessView*>(gameView)->GetDesc(&vd.uav); break;
    }
    for (auto& v : g_views)
        if (v.game == game && v.kind == kind && std::memcmp(&v.desc, &vd, sizeof vd) == 0) return v.priv;
    if (g_views.size() >= kMaxViews) {
        if (!g_saidViewCap) {
            g_saidViewCap = true;
            Log::get().note("hull layer: %u private views held, the cap; no more are made (said once)", static_cast<unsigned>(g_views.size()));
        }
        return nullptr;
    }
    ID3D11View* out = nullptr;
    HRESULT hr = E_FAIL;
    switch (kind) {
    case ViewKind::Rtv: hr = g_device->CreateRenderTargetView(m->priv, &vd.rtv, reinterpret_cast<ID3D11RenderTargetView**>(&out)); break;
    case ViewKind::Dsv: hr = g_device->CreateDepthStencilView(m->priv, &vd.dsv, reinterpret_cast<ID3D11DepthStencilView**>(&out)); break;
    case ViewKind::Srv: hr = g_device->CreateShaderResourceView(m->priv, &vd.srv, reinterpret_cast<ID3D11ShaderResourceView**>(&out)); break;
    case ViewKind::Uav: hr = g_device->CreateUnorderedAccessView(m->priv, &vd.uav, reinterpret_cast<ID3D11UnorderedAccessView**>(&out)); break;
    }
    if (FAILED(hr) || !out) {
        if (!g_saidCreateFail) {
            g_saidCreateFail = true;
            Log::get().note("hull layer: a private view (kind %d) over a %ux%u format %u copy could not be made (hr 0x%08lX; said once)",
                            static_cast<int>(kind), d.Width, d.Height, static_cast<unsigned>(d.Format), static_cast<unsigned long>(hr));
        }
        return nullptr;
    }
    ViewEntry e;
    e.game = game;
    e.kind = kind;
    e.desc = vd;
    e.priv = out;
    g_views.push_back(e);
    return out;
}

// ---- the unmirrored-input census (the blob on the wing, 2026-11-11) -----------------------------------------------------
// On a separate frame, every input a re-issued pass reads from the game's own resource (no private copy swapped in) is one
// key {pass, stage, slot, resource, desc}, kept once a session. An eye-sized one (the eye's render size, or half or quarter
// of it) is watched for a while: the last shader that bound it as a render target, depth target or UAV, or a copy into it,
// in the frame it was read, names its writer. report() says each new key once, then a count line every window.
struct UnmirroredKey {
    uint64_t pass = 0;
    char stage = 'P';   // 'P' pixel (a draw), 'C' compute (a dispatch)
    uint32_t slot = 0;
    const void* res = nullptr;
    uint32_t dim = 0, w = 0, h = 0, fmt = 0, mips = 0, arr = 0, bind = 0;
    uint8_t eyeSized = 0;   // 0 no, 1 the eye's size, 2 half, 3 quarter
    bool mirrored = false;  // a private copy exists but this pass's binding was not swapped (the tonemap's other slots)
    char writerKind = 0;    // 0 none seen, 'r' a render target, 'd' a depth target, 'u' a compute UAV, 'y' a copy
    uint64_t writer = 0;
    uint32_t firstFrame = 0;
    bool said = false;
};
struct WriterWatch {
    const void* res = nullptr;
    char kind = 0;
    uint64_t hash = 0;
    uint32_t frame = ~0u;    // the layer's frame the writer was seen in
    uint32_t until = 0;      // the watch ends at this frame (the cost is bounded)
};
constexpr size_t kMaxUnmirrored = 256, kMaxWatch = 32;
constexpr uint32_t kWatchFrames = 600;
std::vector<UnmirroredKey> g_unm;
std::vector<WriterWatch> g_watch;
std::unordered_map<uint64_t, int32_t> g_unmIndex;   // the key's hash -> its index in g_unm, -1 not kept
uint32_t g_unmChecks = 0, g_unmWinChecks = 0, g_unmCapped = 0;

bool unmirroredWanted() { return detail::g_hullLayerSeparate && g_discard && !g_refFrame; }

WriterWatch* watchOf(const void* res) {
    for (auto& w : g_watch)
        if (w.res == res) return &w;
    return nullptr;
}

// A pass that read the watched resource this frame takes the frame's writer, if one was seen.
void takeWriter(UnmirroredKey& k) {
    const WriterWatch* w = watchOf(k.res);
    if (w && w->frame == g_frame && w->kind) {
        k.writerKind = w->kind;
        k.writer = w->hash;
    }
}

void noteUnmirrored(uint64_t pass, char stage, UINT slot, ID3D11ShaderResourceView* srv, int eye, bool sizedOnly = false) {
    if (!srv || !unmirroredWanted()) return;
    ++g_unmChecks;
    ++g_unmWinChecks;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    if (!res) return;
    const void* id = res;
    // One hash lookup a read after the first sighting (-1: a sighting not kept, the cap or a decal's own texture).
    const uint64_t hk = (pass * 0x9E3779B97F4A7C15ull) ^ (reinterpret_cast<uintptr_t>(id) * 0xC2B2AE3D27D4EB4Full) ^
                        (static_cast<uint64_t>(slot) << 1) ^ static_cast<uint64_t>(stage == 'C');
    const auto found = g_unmIndex.find(hk);
    if (found != g_unmIndex.end()) {
        if (found->second >= 0 && g_unm[static_cast<size_t>(found->second)].eyeSized) takeWriter(g_unm[static_cast<size_t>(found->second)]);
        res->Release();
        return;
    }
    UnmirroredKey k;
    k.pass = pass;
    k.stage = stage;
    k.slot = slot;
    k.res = id;
    k.firstFrame = g_frame;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    k.dim = static_cast<uint32_t>(dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
        k.w = d.Width, k.h = d.Height, k.fmt = d.Format, k.mips = d.MipLevels, k.arr = d.ArraySize, k.bind = d.BindFlags;
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
        D3D11_TEXTURE3D_DESC d{};
        static_cast<ID3D11Texture3D*>(res)->GetDesc(&d);
        k.w = d.Width, k.h = d.Height, k.fmt = d.Format, k.mips = d.MipLevels, k.arr = d.Depth, k.bind = d.BindFlags;
    } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d{};
        static_cast<ID3D11Buffer*>(res)->GetDesc(&d);
        k.w = d.ByteWidth, k.bind = d.BindFlags;
    }
    res->Release();
    for (const auto& m : g_mirrors)
        if (m.game == id) k.mirrored = true;   // not findMirror: a census must not keep a copy alive
    if (eye >= 0 && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        const Eye& e = g_eyes[eye];
        const auto fits = [](uint32_t v, uint32_t full, uint32_t div) { return full && (v == full / div || v == (full + div - 1) / div); };
        k.eyeSized = fits(k.w, e.w, 1) && fits(k.h, e.h, 1)   ? 1
                     : fits(k.w, e.w, 2) && fits(k.h, e.h, 2) ? 2
                     : fits(k.w, e.w, 4) && fits(k.h, e.h, 4) ? 3
                                                              : 0;
    }
    // Past the cap only an eye-sized key is kept (a decal's or a light's own textures must not crowd the suspects out); a
    // sized-only caller (the decals: their material textures are not the question) keeps eye-sized keys alone.
    if ((sizedOnly && !k.eyeSized) || (g_unm.size() >= kMaxUnmirrored && (!k.eyeSized || g_unm.size() >= 2 * kMaxUnmirrored))) {
        if (!sizedOnly) ++g_unmCapped;
        g_unmIndex.emplace(hk, -1);
        return;
    }
    g_unmIndex.emplace(hk, static_cast<int32_t>(g_unm.size()));
    if (k.eyeSized && !watchOf(id) && g_watch.size() < kMaxWatch) {
        WriterWatch w;
        w.res = id;
        w.until = g_frame + kWatchFrames;
        g_watch.push_back(w);
    }
    g_unm.push_back(k);
}

// Every game draw (its targets) and dispatch (its UAVs) while a watch is open: the frame's last writer of a watched resource.
void noteWriter(const void* res, char kind, uint64_t hash) {
    if (WriterWatch* w = watchOf(res)) {
        w->kind = kind;
        w->hash = hash;
        w->frame = g_frame;
    }
}

void watchDraw(ID3D11DeviceContext* ctx) {
    if (g_watch.empty()) return;
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    const uint64_t ps = bindingShaderHash(BindSlot::Ps);
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        if (rtv[i]) noteWriter(resourceOf(rtv[i]), 'r', ps);
    if (dsv) noteWriter(resourceOf(dsv), 'd', ps);
    releaseTargets(rtv, dsv);
}

void watchDispatch(ID3D11DeviceContext* ctx, uint64_t cs) {
    if (g_watch.empty()) return;
    ID3D11UnorderedAccessView* uav[8] = {};
    ctx->CSGetUnorderedAccessViews(0, 8, uav);   // the game's compute passes bind at most a few
    for (UINT i = 0; i < 8; ++i)
        if (uav[i]) {
            noteWriter(resourceOf(uav[i]), 'u', cs);
            uav[i]->Release();
        }
}

// The frame boundary: a watch past its frames ends (its keys keep the writer they took).
void unmirroredFrameEnd() {
    for (size_t i = 0; i < g_watch.size();)
        if (g_frame >= g_watch[i].until)
            g_watch.erase(g_watch.begin() + static_cast<std::ptrdiff_t>(i));
        else
            ++i;
}

void reportUnmirrored() {
    if (!detail::g_hullLayerSeparate) return;
    static const char* const kSized[4] = {"no", "yes", "half", "quarter"};
    uint32_t fresh = 0, sized = 0;
    for (auto& k : g_unm) {
        if (k.eyeSized) ++sized;
        if (k.said) continue;
        // An eye-sized key waits for its writer while its watch is open.
        if (k.eyeSized && !k.writerKind && watchOf(k.res)) continue;
        k.said = true;
        ++fresh;
        char writer[48] = "-";
        if (k.writerKind == 'y')
            std::snprintf(writer, sizeof writer, "copy");
        else if (k.writerKind)
            std::snprintf(writer, sizeof writer, "%s:%016llX%s", k.writerKind == 'u' ? "cs" : "ps", static_cast<unsigned long long>(k.writer),
                          k.writerKind == 'd' ? "(dsv)" : k.writerKind == 'r' ? "(rtv)" : "");
        Log::get().note("hull layer: unmirrored input: pass=%016llX stage=%s t%u %ux%u %u mips=%u eye_sized=%s written_by=%s dim=%u array=%u "
                        "bind=0x%X mirrored_unswapped=%s res=%p first_frame=%u",
                        static_cast<unsigned long long>(k.pass), k.stage == 'C' ? "CS" : "PS", k.slot, k.w, k.h, k.fmt, k.mips,
                        kSized[k.eyeSized], writer, k.dim, k.arr, k.bind, k.mirrored ? "yes" : "no", k.res, k.firstFrame);
    }
    Log::get().note("hull layer: unmirrored inputs: checks=%u (session %u) keys=%u new=%u eye_sized=%u watching=%u capped=%u",
                    g_unmWinChecks, g_unmChecks, static_cast<unsigned>(g_unm.size()), fresh, sized, static_cast<unsigned>(g_watch.size()),
                    g_unmCapped);
    g_unmWinChecks = 0;
}

// Mirrors whose game texture was not seen for kEvictFrames go (a re-created eye depth leaves its old targets behind).
void evictStale() {
    for (size_t i = 0; i < g_mirrors.size();) {
        Mirror& m = g_mirrors[i];
        if (g_frame - m.lastSeen > kEvictFrames) {
            const D3D11_TEXTURE2D_DESC d = m.desc;
            if (detail::g_hullBenchOn) ++g_benchEvictions;   // the variant bench: a private copy lost in its window (the manifest says)
            dropMirror(m.game);   // erases index i
            Log::get().note("hull layer: private copy of a %ux%u format %u target dropped, its game target unseen for %u frames (%u held)", d.Width,
                            d.Height, static_cast<unsigned>(d.Format), kEvictFrames, static_cast<unsigned>(g_mirrors.size()));
        } else {
            ++i;
        }
    }
}

// The eye whose depth texture this is; with `assign`, a slot is given to a depth not seen before (a slot not seen this
// frame is reused; its GPU objects stay, they are not tied to the depth). -1 when none.
int eyeFor(const void* depth, bool assign) {
    if (!depth) return -1;
    for (int i = 0; i < 2; ++i)
        if (g_eyes[i].depth == depth) {
            g_eyes[i].seen = true;
            return i;
        }
    if (!assign) return -1;
    for (int i = 0; i < 2; ++i) {
        if (!g_eyes[i].depth || !g_eyes[i].seen) {
            releaseStaging(g_eyes[i]);
            resetFrame(g_eyes[i]);
            g_eyes[i].depth = depth;
            g_eyes[i].seen = true;
            return i;
        }
    }
    return -1;
}

// The eye an AO chain resource belongs to this frame: its G-buffer RT0-3, its depth copy, or an output of its chain. -1: none.
int aoEyeOf(const void* res) {
    if (!res) return -1;
    for (int i = 0; i < 2; ++i) {
        const Eye& e = g_eyes[i];
        if (res == e.copyGame) return i;
        for (const void* g : e.gbufGame)
            if (g && g == res) return i;
        for (uint8_t k = 0; k < e.aoOutCount; ++k)
            if (e.aoOut[k] == res) return i;
    }
    return -1;
}
// Folds one resource's eye into *ei; *both when two resources name different eyes.
void noteAoEye(int found, int* ei, bool* both) {
    if (found < 0) return;
    if (*ei >= 0 && *ei != found) *both = true;
    *ei = found;
}

float clearDepthOf(const void* res, uint8_t* stencil) {
    for (const auto& c : g_clearValues)
        if (c.res && c.res == res) {
            *stencil = c.stencil;
            return c.depth;
        }
    *stencil = 0;
    return 0.0f;   // reversed Z, GEQUAL: the census's DCL lines clear the eye depth to 0
}

// A staging copy of src: made on the first call (into `stage`), copied again on every later one, so the last copy wins.
void copyToStaging(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, ID3D11Texture2D*& stage) {
    if (!src || !g_device) return;
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    if (d.SampleDesc.Count != 1) return;
    if (stage) {
        D3D11_TEXTURE2D_DESC s{};
        stage->GetDesc(&s);
        if (s.Width != d.Width || s.Height != d.Height || s.Format != d.Format) return;
    } else {
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        if (FAILED(g_device->CreateTexture2D(&d, nullptr, &stage)) || !stage) {
            stage = nullptr;
            return;
        }
    }
    vScreenCopyResourceRaw(ctx, stage, src);
}

ID3D11Buffer* makeStagingBuffer(uint32_t bytes) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* b = nullptr;
    if (FAILED(g_device->CreateBuffer(&bd, nullptr, &b))) return nullptr;
    return b;
}

// A default-usage raw buffer with an SRV and, when `uav`, a UAV over it.
bool makeRawBuffer(uint32_t bytes, bool uav, ID3D11Buffer** buf, ID3D11ShaderResourceView** srv, ID3D11UnorderedAccessView** uavOut) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = (bytes + 15u) & ~15u;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(g_device->CreateBuffer(&bd, nullptr, buf)) || !*buf) return false;
    if (srv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sd.BufferEx.FirstElement = 0;
        sd.BufferEx.NumElements = bd.ByteWidth / 4;
        sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        if (FAILED(g_device->CreateShaderResourceView(*buf, &sd, srv)) || !*srv) return false;
    }
    if (uav && uavOut) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.FirstElement = 0;
        ud.Buffer.NumElements = bd.ByteWidth / 4;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(g_device->CreateUnorderedAccessView(*buf, &ud, uavOut)) || !*uavOut) return false;
    }
    return true;
}

ID3D11Buffer* makeConstantBuffer(UINT bytes = 16) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer* b = nullptr;
    if (FAILED(g_device->CreateBuffer(&bd, nullptr, &b))) return nullptr;
    return b;
}

// d3dcompiler_47.dll's D3DCompile, loaded once (gpu_pass_profile.cpp's way). Raw types: the macro and include arguments are null.
typedef HRESULT(WINAPI* PfnD3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
bool compileShader(const char* src, size_t len, const char* entry, const char* target, ID3DBlob** code, std::string* why) {
    static PfnD3DCompile compile = nullptr;
    if (!compile) {
        HMODULE mod = LoadLibraryW(L"d3dcompiler_47.dll");
        if (mod) compile = reinterpret_cast<PfnD3DCompile>(GetProcAddress(mod, "D3DCompile"));
        if (!compile) {
            *why = "d3dcompiler_47.dll or its D3DCompile is missing";
            return false;
        }
    }
    ID3DBlob* errors = nullptr;
    const HRESULT hr = compile(src, len, entry, nullptr, nullptr, entry, target, 0x8000u /* D3DCOMPILE_OPTIMIZATION_LEVEL3 */, 0, code, &errors);
    if (errors) {
        why->assign(static_cast<const char*>(errors->GetBufferPointer()), std::min<size_t>(errors->GetBufferSize(), 300));
        errors->Release();
    }
    if (FAILED(hr) || !*code) {
        if (why->empty()) *why = "D3DCompile failed";
        return false;
    }
    return true;
}

// The shaders, states and shared buffers, once. False (and said once) when any is missing: the layer seeds nothing.
bool ensureGpu() {
    if (g_gpu.tried) return g_gpu.ok;
    g_gpu.tried = true;
    struct Item {
        const char* src;
        size_t len;
        const char* entry;
        const char* target;
    };
    const Item items[4] = {{kSeedHlsl, sizeof(kSeedHlsl) - 1, "hull_layer_seed_cs", "cs_5_0"},
                           {kCountHlsl, sizeof(kCountHlsl) - 1, "hull_layer_count_cs", "cs_5_0"},
                           {kFilterVsHlsl, sizeof(kFilterVsHlsl) - 1, "hull_layer_filter_vs", "vs_5_0"},
                           {kFilterPsHlsl, sizeof(kFilterPsHlsl) - 1, "hull_layer_filter_ps", "ps_5_0"}};
    for (int i = 0; i < 4; ++i) {
        ID3DBlob* code = nullptr;
        std::string why;
        if (!compileShader(items[i].src, items[i].len, items[i].entry, items[i].target, &code, &why)) {
            Log::get().note("hull layer: the %s shader did not compile (%s); the layer seeds nothing this session", items[i].entry, why.c_str());
            if (code) code->Release();
            return false;
        }
        HRESULT hr = E_FAIL;
        switch (i) {
        case 0: hr = g_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_gpu.seedCs); break;
        case 1: hr = g_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_gpu.countCs); break;
        case 2: hr = g_device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_gpu.filterVs); break;
        case 3: hr = g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_gpu.filterPs); break;
        }
        code->Release();
        if (FAILED(hr)) {
            Log::get().note("hull layer: the %s shader compiled but could not be created (hr 0x%08lX); the layer seeds nothing this session", items[i].entry,
                            static_cast<unsigned long>(hr));
            return false;
        }
    }
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = FALSE;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    g_gpu.seedCb = makeConstantBuffer(32);
    g_gpu.passCb = makeConstantBuffer();
    if (FAILED(g_device->CreateDepthStencilState(&dd, &g_gpu.depthAlways)) || FAILED(g_device->CreateRasterizerState(&rd, &g_gpu.noCull)) ||
        !g_gpu.seedCb || !g_gpu.passCb || !makeRawBuffer(kMaxSeedInstances * 8, false, &g_gpu.inst, &g_gpu.instSrv, nullptr)) {
        Log::get().note("hull layer: the passes' states or buffers could not be made; the layer seeds nothing this session");
        return false;
    }
    Log::get().note("hull layer: the seed, filter and count shaders compiled and made");
    g_gpu.ok = true;
    return true;
}

// The compute stage's slots the layer uses (t0-1, u0-1, b0, the shader), taken and put back.
struct CsSave {
    ID3D11ComputeShader* cs = nullptr;
    ID3D11ShaderResourceView* srv[2] = {};
    ID3D11UnorderedAccessView* uav[2] = {};
    ID3D11Buffer* cb = nullptr;
    void capture(ID3D11DeviceContext* c) {
        c->CSGetShader(&cs, nullptr, nullptr);
        c->CSGetShaderResources(0, 2, srv);
        c->CSGetUnorderedAccessViews(0, 2, uav);
        c->CSGetConstantBuffers(0, 1, &cb);
    }
    void restore(ID3D11DeviceContext* c) {
        static const UINT keep[2] = {~0u, ~0u};   // -1: an append buffer's counter stays as it is
        ID3D11UnorderedAccessView* none[2] = {};
        c->CSSetUnorderedAccessViews(0, 2, none, keep);   // the layer's UAVs off before the game's SRVs come back
        c->CSSetShader(cs, nullptr, 0);
        c->CSSetShaderResources(0, 2, srv);
        c->CSSetUnorderedAccessViews(0, 2, uav, keep);
        c->CSSetConstantBuffers(0, 1, &cb);
        releaseRef(cs);
        for (auto*& s : srv) releaseRef(s);
        for (auto*& u : uav) releaseRef(u);
        releaseRef(cb);
    }
};

// The graphics state the filter pass sets, taken and put back (desktop_window.cpp SavedState's shape, with the raw setters
// where vscreen has one so the binding shadow keeps describing the game's bindings).
struct GfxSave {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4] = {};
    UINT sampleMask = 0xFFFFFFFFu;
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencilRef = 0;
    ID3D11RasterizerState* raster = nullptr;
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;   // Build B's passes are scissored
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    ID3D11InputLayout* layout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* ds = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11ShaderResourceView* psSrv[2] = {};
    ID3D11Buffer* psCb = nullptr;
    void capture(ID3D11DeviceContext* c) {
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        c->OMGetBlendState(&blend, blendFactor, &sampleMask);
        c->OMGetDepthStencilState(&depth, &stencilRef);
        c->RSGetState(&raster);
        c->RSGetViewports(&viewportCount, viewports);
        c->RSGetScissorRects(&scissorCount, scissors);
        c->IAGetInputLayout(&layout);
        c->IAGetPrimitiveTopology(&topology);
        c->VSGetShader(&vs, nullptr, nullptr);
        c->HSGetShader(&hs, nullptr, nullptr);
        c->DSGetShader(&ds, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->PSGetShaderResources(0, 2, psSrv);
        c->PSGetConstantBuffers(0, 1, &psCb);
    }
    void restore(ID3D11DeviceContext* c) {
        vScreenSetRenderTargetsRaw(c, boundCount(rtv), rtv, dsv);
        vScreenOMSetBlendStateRaw(c, blend, blendFactor, sampleMask);
        vScreenOMSetDepthStencilStateRaw(c, depth, stencilRef);
        c->RSSetState(raster);
        vScreenRSSetViewportsRaw(c, viewportCount, viewportCount ? viewports : nullptr);
        c->RSSetScissorRects(scissorCount, scissorCount ? scissors : nullptr);
        c->IASetInputLayout(layout);
        c->IASetPrimitiveTopology(topology);
        vScreenVSSetShaderRaw(c, vs, nullptr, 0);
        c->HSSetShader(hs, nullptr, 0);
        c->DSSetShader(ds, nullptr, 0);
        c->GSSetShader(gs, nullptr, 0);
        vScreenPSSetShaderRaw(c, ps, nullptr, 0);
        vScreenPSSetShaderResourcesRaw(c, 0, 2, psSrv);
        vScreenPSSetConstantBuffersRaw(c, 0, 1, &psCb);
        releaseTargets(rtv, dsv);
        releaseRef(blend);
        releaseRef(depth);
        releaseRef(raster);
        releaseRef(layout);
        releaseRef(vs);
        releaseRef(hs);
        releaseRef(ds);
        releaseRef(gs);
        releaseRef(ps);
        for (auto*& s : psSrv) releaseRef(s);
        releaseRef(psCb);
    }
};

// ---- dump v2: the stage times ---------------------------------------------------------------------------------------------

// An item of stage `ts` begins: its CPU time from the hook's entry (fromEntry) or from now, and on a timed frame its first
// timestamp. Items never nest (one hook pair is one item); a second open while one is open is ignored.
void stageOpen(ID3D11DeviceContext* ctx, TStage ts, bool fromEntry) {
    if (g_st.cpuStage >= 0 || g_st.scope >= 0) return;
    g_st.cpuStage = ts;
    if (fromEntry)
        g_st.cpuStart = g_entryQpc;
    else
        QueryPerformanceCounter(&g_st.cpuStart);
    ++g_st.items[ts];
    if (g_st.cur < 0 || !g_device || !ctx) return;
    StageSet& s = g_st.set[g_st.cur];
    if (!s.open) {
        if (!s.disjoint) {
            D3D11_QUERY_DESC qd{};
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            if (FAILED(g_device->CreateQuery(&qd, &s.disjoint)) || !s.disjoint) {
                s.disjoint = nullptr;
                g_st.cur = -1;
                return;
            }
        }
        ctx->Begin(s.disjoint);
        s.open = true;
        s.used = s.overflow = 0;
        s.frame = g_frame;
    }
    if (s.used >= kStagePairCap) {
        ++s.overflow;
        return;
    }
    for (uint32_t k = 0; k < 2; ++k) {
        ID3D11Query*& q = s.q[2 * s.used + k];
        if (q) continue;
        D3D11_QUERY_DESC qt{};
        qt.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(g_device->CreateQuery(&qt, &q)) || !q) {
            q = nullptr;
            ++s.overflow;
            return;
        }
    }
    ctx->End(s.q[2 * s.used]);
    s.stage[s.used] = static_cast<uint8_t>(ts);
    g_st.scope = static_cast<int>(s.used);
}

// The open item ends: its second timestamp right after its GPU work, and its CPU time.
void stageClose(ID3D11DeviceContext* ctx) {
    if (g_st.scope >= 0 && g_st.cur >= 0 && ctx) {
        StageSet& s = g_st.set[g_st.cur];
        ctx->End(s.q[2 * g_st.scope + 1]);
        ++s.used;
    }
    g_st.scope = -1;
    if (g_st.cpuStage >= 0) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        g_st.cpuTicks[g_st.cpuStage] += now.QuadPart - g_st.cpuStart.QuadPart;
        g_st.cpuStage = -1;
    }
}

// At the frame boundary: the timed frame's set closed (its disjoint ended) and pending its readback.
void stageFrameEnd(ID3D11DeviceContext* ctx) {
    if (g_st.scope >= 0 || g_st.cpuStage >= 0) stageClose(ctx);   // never: an item opens and closes in one hook pair
    if (g_st.cur >= 0) {
        StageSet& s = g_st.set[g_st.cur];
        if (s.open && ctx) {
            ctx->End(s.disjoint);
            s.open = false;
            s.pending = true;
        }
        g_st.cur = -1;
    }
}

// The next frame (g_frame): timed when its number says so and a set is free.
void stageFrameStart() {
    g_st.cur = -1;
    if (g_frame % kStageEvery != 0 && !g_benchTimeAll) return;
    for (uint32_t i = 0; i < kStageSets; ++i)
        if (!g_st.set[i].pending && !g_st.set[i].open) {
            g_st.cur = static_cast<int>(i);
            return;
        }
    ++g_st.busy;
}

// The timed frames the GPU has finished: each stage's sum and the total into the window (DO_NOT_FLUSH, never a stall).
void stageCollect(ID3D11DeviceContext* ctx) {
    for (auto& s : g_st.set) {
        if (!s.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (ctx->GetData(s.disjoint, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        double sum[kTStages] = {};
        bool ready = true;
        for (uint32_t i = 0; i < s.used && ready; ++i) {
            UINT64 a = 0, b = 0;
            if (ctx->GetData(s.q[2 * i], &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                ctx->GetData(s.q[2 * i + 1], &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
                ready = false;
            else if (b >= a && dj.Frequency)
                sum[s.stage[i]] += static_cast<double>(b - a) * 1000.0 / static_cast<double>(dj.Frequency);
        }
        if (!ready) continue;
        s.pending = false;
        g_st.overflow += s.overflow;
        if (dj.Disjoint || dj.Frequency == 0) {
            ++g_st.disjoint;
            continue;
        }
        if (detail::g_hullBenchOn) benchNoteStages(s.frame, sum);
        double total = 0.0;
        for (int k = 0; k < kTStages; ++k) {
            g_st.ms[k].push_back(static_cast<float>(sum[k]));
            total += sum[k];
        }
        g_st.ms[kTStages].push_back(static_cast<float>(total));
        ++g_st.timed;
    }
}

// The window's part back to zero (the 30 s line).
void stageWindowReset() {
    for (auto& v : g_st.ms) v.clear();
    for (auto& n : g_st.items) n = 0;
    for (auto& t : g_st.cpuTicks) t = 0;
    g_st.timed = g_st.overflow = g_st.busy = g_st.disjoint = 0;
}

// Field by field, not by assignment: a StageTimes is ~200 KB (its query pointers), too much for a temporary on the render thread.
void releaseStageTimes() {
    for (auto& s : g_st.set) {
        releaseRef(s.disjoint);
        for (auto*& q : s.q) releaseRef(q);
        s.used = s.overflow = 0;
        s.open = s.pending = false;
    }
    g_st.cur = g_st.scope = g_st.cpuStage = -1;
    stageWindowReset();
}

// ---- dump v2: the lighting window's draws ----------------------------------------------------------------------------------

// A draw's bindings before the layer's hook touched them (the dump frame only): its targets' identities, whether it writes colour,
// and whether it writes stencil with no colour (a light volume's stencil pass).
struct DrawSnap {
    const void* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    uint64_t vs = 0, ps = 0;
    bool stencilOnly = false;
    uint8_t stencilRef = 0;
};

DrawSnap snapDraw(ID3D11DeviceContext* ctx) {
    DrawSnap s;
    s.vs = bindingShaderHash(BindSlot::Vs);
    s.ps = bindingShaderHash(BindSlot::Ps);
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    bool colour = false;
    {
        ID3D11BlendState* bs = nullptr;
        float factor[4] = {};
        UINT mask = 0;
        ctx->OMGetBlendState(&bs, factor, &mask);
        D3D11_BLEND_DESC bd{};
        if (bs) bs->GetDesc(&bd);
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            if (!rtv[i]) continue;
            s.rtv[i] = resourceOf(rtv[i]);
            const UINT8 writes = !bs ? D3D11_COLOR_WRITE_ENABLE_ALL : bd.RenderTarget[bd.IndependentBlendEnable ? i : 0].RenderTargetWriteMask;
            if (writes) colour = true;
        }
        releaseRef(bs);
    }
    if (dsv && !colour) {
        ID3D11DepthStencilState* ds = nullptr;
        UINT ref = 0;
        ctx->OMGetDepthStencilState(&ds, &ref);
        if (ds) {
            D3D11_DEPTH_STENCIL_DESC dd{};
            ds->GetDesc(&dd);
            const auto writesOp = [](const D3D11_DEPTH_STENCILOP_DESC& o) {
                return o.StencilFailOp != D3D11_STENCIL_OP_KEEP || o.StencilDepthFailOp != D3D11_STENCIL_OP_KEEP || o.StencilPassOp != D3D11_STENCIL_OP_KEEP;
            };
            s.stencilOnly = dd.StencilEnable && dd.StencilWriteMask && (writesOp(dd.FrontFace) || writesOp(dd.BackFace));
            s.stencilRef = static_cast<uint8_t>(ref);
        }
        releaseRef(ds);
    }
    releaseTargets(rtv, dsv);
    return s;
}

// After the hook ran for the draw: kept when it is inside an eye's lighting window (the shadow mask opened it, the sky not yet
// closed it) and writes the eye's HDR, or writes stencil only.
void recordLight(const DrawSnap& s, char kind, UINT count, UINT instances, UINT startInstance, bool reissued) {
    if (g_stageEye < 0) return;
    const Eye& e = g_eyes[g_stageEye];
    if (!e.lightOpen) return;
    bool hdr = false;
    for (const void* r : s.rtv)
        if (r && r == e.hdrGame) hdr = true;
    if (!hdr && !s.stencilOnly) return;
    if (g_lights.size() >= kMaxLightRecs) {
        g_lightsCapped = true;
        return;
    }
    LightRec r;
    r.eye = g_stageEye;
    r.seq = g_drawSeq;
    r.vs = s.vs;
    r.ps = s.ps;
    r.kind = kind;
    r.count = count;
    r.instances = instances;
    r.startInstance = startInstance;
    r.hdr = hdr;
    r.stencilOnly = s.stencilOnly;
    r.stencilRef = s.stencilRef;
    r.reissued = reissued;
    r.known = isLightVs(s.vs) || s.vs == kNightVisionVs;
    for (const auto& pair : kStagePairs)
        if (pair.vs == s.vs && pair.ps == s.ps) r.stagePair = true;
    g_lights.push_back(r);
}

// ---- the seed --------------------------------------------------------------------------------------------------------

void sayRefused(const char* why) {
    if (g_saidRefused) return;
    g_saidRefused = true;
    Log::get().note("hull layer: a seed was refused: %s (said once)", why);
}

// The bytes of one buffer element a view counts in (for its FirstElement): the structure stride, or the format's size.
uint32_t elementBytes(const D3D11_BUFFER_DESC& bd, DXGI_FORMAT f, bool raw) {
    if (bd.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) return bd.StructureByteStride;
    if (raw) return 4;
    switch (f) {
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT: case DXGI_FORMAT_R32_FLOAT: return 4;
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_FLOAT: return 8;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    default: return 0;
    }
}

// At the seed draw, with its bindings: the instance window and the pool copied, and the seed dispatch writes the eye's own
// mask and stats. False with the reason when it cannot run; nothing is left bound either way.
bool seedOwnMask(ID3D11DeviceContext* ctx, Eye& e, UINT instances, UINT startInstance, const char** why) {
    // The instance stream: the stride-8 vertex buffer among slots 0-3 (object_probe.cpp ledgerLearn: record index first).
    ID3D11Buffer* vbs[4] = {};
    UINT strides[4] = {}, offsets[4] = {};
    ctx->IAGetVertexBuffers(0, 4, vbs, strides, offsets);
    int pick = -1;
    for (int i = 0; i < 4 && pick < 0; ++i)
        if (vbs[i] && strides[i] == 8) pick = i;
    ID3D11Buffer* instBuf = pick >= 0 ? vbs[pick] : nullptr;
    const UINT instOffset = pick >= 0 ? offsets[pick] : 0;
    for (int i = 0; i < 4; ++i)
        if (i != pick) releaseRef(vbs[i]);
    // The pool at VS t33.
    ID3D11ShaderResourceView* poolSrv = nullptr;
    ctx->VSGetShaderResources(kPoolSlot, 1, &poolSrv);
    ID3D11Buffer* poolBuf = nullptr;
    if (poolSrv) {
        ID3D11Resource* res = nullptr;
        poolSrv->GetResource(&res);
        if (res) {
            res->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&poolBuf));
            res->Release();
        }
    }
    const auto fail = [&](const char* reason) {
        *why = reason;
        releaseRef(instBuf);
        releaseRef(poolBuf);
        releaseRef(poolSrv);
        return false;
    };
    if (!instBuf) return fail("no instance stream (no stride-8 vertex buffer in slots 0-3)");
    if (!poolBuf) return fail("no pool buffer at VS t33");
    D3D11_BUFFER_DESC ib{}, pb{};
    instBuf->GetDesc(&ib);
    poolBuf->GetDesc(&pb);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    poolSrv->GetDesc(&sd);
    uint32_t first = 0, count = 0;
    bool raw = false;
    if (sd.ViewDimension == D3D11_SRV_DIMENSION_BUFFER) {
        first = sd.Buffer.FirstElement;
        count = sd.Buffer.NumElements;
    } else if (sd.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX) {
        first = sd.BufferEx.FirstElement;
        count = sd.BufferEx.NumElements;
        raw = (sd.BufferEx.Flags & D3D11_BUFFEREX_SRV_FLAG_RAW) != 0;
    } else {
        return fail("VS t33 is not a buffer view");
    }
    const uint32_t elem = elementBytes(pb, sd.Format, raw);
    if (elem == 0 && first != 0) return fail("VS t33's view starts past element 0 in a format whose size is not known");
    const uint64_t base = static_cast<uint64_t>(first) * elem;
    uint64_t len = elem ? static_cast<uint64_t>(count) * elem : pb.ByteWidth - base;
    if (base >= pb.ByteWidth) return fail("VS t33's view starts past its buffer");
    len = std::min<uint64_t>(len, pb.ByteWidth - base);
    const uint32_t records = static_cast<uint32_t>(std::min<uint64_t>(len / kRecordBytes, kMaxRecords));
    if (records == 0) return fail("the pool holds no whole 336-byte record");
    const uint32_t poolBytes = records * kRecordBytes;
    const uint32_t seeds = std::min<uint32_t>(instances, kMaxSeedInstances);
    const uint64_t instBegin = static_cast<uint64_t>(instOffset) + static_cast<uint64_t>(startInstance) * 8u;
    const uint64_t instEnd = instBegin + static_cast<uint64_t>(seeds) * 8u;
    if (seeds == 0 || instEnd > ib.ByteWidth) return fail("the seed's instance window runs past the instance stream");
    if (!g_saidPool) {
        g_saidPool = true;
        Log::get().note("hull layer: the seed reads the pool at VS t33 (%u bytes, view dimension %d, format %u, first element %u, %u elements of %u bytes: "
                        "%u records of 336) and the instance stream at vertex slot %d (%u bytes, offset %u); said once",
                        pb.ByteWidth, static_cast<int>(sd.ViewDimension), static_cast<unsigned>(sd.Format), first, count, elem, records, pick,
                        ib.ByteWidth, instOffset);
    }
    // The layer's copies: the pool (made at its size), the eye's mask (one uint a record) and stats.
    if (g_gpu.poolBytes < poolBytes) {
        releaseRef(g_gpu.poolSrv);
        releaseRef(g_gpu.pool);
        g_gpu.poolBytes = 0;
        if (!makeRawBuffer(poolBytes, false, &g_gpu.pool, &g_gpu.poolSrv, nullptr)) return fail("the pool's copy could not be made");
        g_gpu.poolBytes = poolBytes;
    }
    if (e.maskRecords < records) {
        releaseRef(e.maskSrv);
        releaseRef(e.maskUav);
        releaseRef(e.mask);
        e.maskRecords = 0;
        if (!makeRawBuffer(records * 4u, true, &e.mask, &e.maskSrv, &e.maskUav)) return fail("the own mask could not be made");
        e.maskRecords = records;
    }
    if (!e.statsUav) {
        releaseRef(e.stats);
        if (!makeRawBuffer(kStatsBytes, true, &e.stats, nullptr, &e.statsUav)) return fail("the stats buffer could not be made");
        e.rigGen = ~0u;   // a new buffer holds no reach
    }
    D3D11_BOX box{};
    box.left = static_cast<UINT>(base);
    box.right = static_cast<UINT>(base + poolBytes);
    box.top = 0;
    box.bottom = 1;
    box.front = 0;
    box.back = 1;
    vScreenCopySubresourceRegionRaw(ctx, g_gpu.pool, 0, poolBuf, &box);
    // The seed records: the rig probe's own slots in this buffer, this frame (docs "Rig seed"), else the seed draw's instance window.
    // The probe's record q is the game buffer's record base/336 + q, the mask's numbering, only when the view starts on a record.
    static uint32_t s_rigSlots[kMaxSeedInstances];
    static uint32_t s_rigUpload[kMaxSeedInstances * 2];
    uint32_t rigGen = 0;
    const uint32_t rigCount = base % kRecordBytes == 0 ? hullRigProbeOwnSlots(poolBuf, static_cast<uint32_t>(base / kRecordBytes), records, s_rigSlots,
                                                                                kMaxSeedInstances, &rigGen)
                                                       : 0;
    uint32_t seedCount = seeds, resetHold = 0;
    if (rigCount) {
        for (uint32_t i = 0; i < rigCount; ++i) {
            s_rigUpload[2 * i] = s_rigSlots[i];   // the instance stream's layout: the record index first
            s_rigUpload[2 * i + 1] = 0;
        }
        D3D11_BOX ub{0, 0, 0, rigCount * 8u, 1, 1};
        vScreenUpdateSubresourceRaw(ctx, g_gpu.inst, 0, &ub, s_rigUpload, 0, 0);
        seedCount = rigCount;
        if (e.rigGen != rigGen) {
            resetHold = 1;
            e.rigGen = rigGen;
            ++g_win.holdResets;
        }
        ++g_win.rigSeeds;
        g_win.rigSlots.push_back(rigCount);
    } else {
        box.left = static_cast<UINT>(instBegin);
        box.right = static_cast<UINT>(instEnd);
        vScreenCopySubresourceRegionRaw(ctx, g_gpu.inst, 0, instBuf, &box);
        ++g_win.drawSeeds;
    }
    const float radiusMax = kOwnRadius, margin = kRigMargin;
    uint32_t cb[8] = {seedCount, records, 0u, rigCount ? 1u : 0u, 0u, resetHold, 0u, 0u};
    std::memcpy(&cb[2], &radiusMax, sizeof radiusMax);
    std::memcpy(&cb[4], &margin, sizeof margin);
    vScreenUpdateSubresourceRaw(ctx, g_gpu.seedCb, 0, nullptr, cb, 0, 0);
    CsSave save;
    save.capture(ctx);
    ID3D11ShaderResourceView* srvs[2] = {g_gpu.instSrv, g_gpu.poolSrv};
    ID3D11UnorderedAccessView* uavs[2] = {e.maskUav, e.statsUav};
    static const UINT keep[2] = {~0u, ~0u};
    ctx->CSSetShader(g_gpu.seedCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 2, srvs);
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, keep);
    ctx->CSSetConstantBuffers(0, 1, &g_gpu.seedCb);
    ctx->Dispatch(1, 1, 1);
    save.restore(ctx);
    // The rig probe (diagnostic): on its ~30 s cadence and on the dump's frame it copies this mask, and the pool copy for the dump, to staging.
    // The mask's record q is the game buffer's record base/336 + q only when the view starts on a record; otherwise it gets no buffer (no_pool).
    hullRigProbeAtSeed(ctx, g_device, g_frame, static_cast<int>(&e - g_eyes), e.mask, g_gpu.pool, base % kRecordBytes == 0 ? poolBuf : nullptr,
                       static_cast<uint32_t>(base / kRecordBytes), records, g_dump == Dump::Capturing ? g_dumpStamp : nullptr);
    e.records = records;
    e.seedInstances = seedCount;
    releaseRef(instBuf);
    releaseRef(poolBuf);
    releaseRef(poolSrv);
    return true;
}

// ---- the private G-buffer --------------------------------------------------------------------------------------------

void noteShell(ID3D11DeviceContext* ctx);
void finishGbuffer(ID3D11DeviceContext* ctx, int ei, End why);
bool eyeRect(int ei, D3D11_RECT* out);
ID3D11RasterizerState* scissorClone(ID3D11RasterizerState* game);
bool depthView(ID3D11Resource* res, ID3D11ShaderResourceView*& srv, const void*& of);

// ---- Build B: the depth prefill and its reset (docs "Build B") -------------------------------------------------------------
//
// Before the eye's first hull draw the private depth takes the game's eye depth (the prepass, the cockpit shell and the draws
// before the own ship's block; the main pass discards the own ship, so no hull), so a hull pixel behind any of it fails the
// early depth test and is not shaded. Before the eye's G-buffer window ends (finishGbuffer, ahead of the depth copy, the decals,
// the AO and every lighting stage), every pixel the hull did not make nearer goes back to the clear depth and stencil: from
// there on the private depth is hull-only, as before, except the hull pixels hidden behind what the prefill held, which are now
// clear. Both passes run in the eye's Build A' rectangle (outside it the private depth stays at its clear and nothing is drawn).

// The prefill pass's shaders and the reset's state, once.
bool ensurePrefillGpu() {
    if (g_pgpu.tried) return g_pgpu.ok;
    g_pgpu.tried = true;
    struct Item {
        const char* src;
        size_t len;
        const char* entry;
        ID3D11PixelShader** out;
    };
    const Item items[3] = {{kPrefillPsHlsl, sizeof(kPrefillPsHlsl) - 1, "hull_layer_prefill_ps", &g_pgpu.prefillPs},
                           {kMarkPsHlsl, sizeof(kMarkPsHlsl) - 1, "hull_layer_mark_ps", &g_pgpu.markPs},
                           {kResetPsHlsl, sizeof(kResetPsHlsl) - 1, "hull_layer_reset_ps", &g_pgpu.resetPs}};
    for (const Item& it : items) {
        ID3DBlob* code = nullptr;
        std::string why;
        if (!compileShader(it.src, it.len, it.entry, "ps_5_0", &code, &why)) {
            Log::get().note("hull layer: the %s shader did not compile (%s); no depth prefill this session", it.entry, why.c_str());
            if (code) code->Release();
            return false;
        }
        const HRESULT hr = g_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, it.out);
        code->Release();
        if (FAILED(hr) || !*it.out) {
            Log::get().note("hull layer: the %s shader compiled but could not be made (hr 0x%08lX); no depth prefill this session", it.entry,
                            static_cast<unsigned long>(hr));
            return false;
        }
    }
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = TRUE;
    dd.StencilReadMask = 0xFF;
    dd.StencilWriteMask = 0xFF;
    dd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    dd.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    dd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    dd.BackFace = dd.FrontFace;
    if (FAILED(g_device->CreateDepthStencilState(&dd, &g_pgpu.resetDss)) || !g_pgpu.resetDss) {
        g_pgpu.resetDss = nullptr;
        Log::get().note("hull layer: the depth prefill's reset state could not be made; no depth prefill this session");
        return false;
    }
    Log::get().note("hull layer: the depth prefill's shaders compiled and made");
    g_pgpu.ok = true;
    return true;
}

// The eye's P (R32_FLOAT) and mark (R8_UNORM), render targets and shader resources, at its render size.
bool ensurePrefillTargets(Eye& e, uint32_t w, uint32_t h) {
    if (e.pre && e.preMark && e.preW == w && e.preH == h) return true;
    releasePrefill(e);
    const auto make = [&](DXGI_FORMAT fmt, ID3D11Texture2D** tex, ID3D11RenderTargetView** rtv, ID3D11ShaderResourceView** srv) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        return SUCCEEDED(g_device->CreateTexture2D(&td, nullptr, tex)) && *tex && SUCCEEDED(g_device->CreateRenderTargetView(*tex, nullptr, rtv)) &&
               *rtv && SUCCEEDED(g_device->CreateShaderResourceView(*tex, nullptr, srv)) && *srv;
    };
    if (!make(DXGI_FORMAT_R32_FLOAT, &e.pre, &e.preRtv, &e.preSrv) || !make(DXGI_FORMAT_R8_UNORM, &e.preMark, &e.preMarkRtv, &e.preMarkSrv)) {
        releasePrefill(e);
        return false;
    }
    e.preW = w;
    e.preH = h;
    Log::get().note("hull layer: depth prefill targets made: %ux%u (R32_FLOAT copy, R8_UNORM mark)", w, h);
    return true;
}

// The graphics state Build B's full-screen passes share: the eye's viewport, the full-screen triangle, the rectangle's scissor.
void bindPrefillPass(ID3D11DeviceContext* ctx, const Eye& e) {
    vScreenOMSetBlendStateRaw(ctx, nullptr, nullptr, 0xFFFFFFFFu);
    ID3D11RasterizerState* rs = e.preRectOn ? scissorClone(g_gpu.noCull) : nullptr;
    ctx->RSSetState(rs ? rs : g_gpu.noCull);
    if (rs) ctx->RSSetScissorRects(1, &e.preRect);
    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(e.w);
    vp.Height = static_cast<float>(e.h);
    vp.MaxDepth = 1.0f;
    vScreenRSSetViewportsRaw(ctx, 1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    vScreenVSSetShaderRaw(ctx, g_gpu.filterVs, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
}

// The prefill, at the seed (separate mode only: in dump mode the main pass keeps the hull, which the prefill would then hold).
// Counted by outcome; the private depth stays at its clear when it does not run.
void prefillEye(ID3D11DeviceContext* ctx, Eye& e, int ei) {
    ++g_win.preSeeds;
    if (!detail::g_hullLayerSeparate || !e.mainDepth) {
        ++g_win.preSkipMode;
        return;
    }
    D3D11_TEXTURE2D_DESC md{};
    e.mainDepth->GetDesc(&md);
    // A float depth only: the prefill's SV_Depth and P then hold the same bits, so the mark finds every pixel no hull wrote.
    if (md.Format != DXGI_FORMAT_R32G8X24_TYPELESS && md.Format != DXGI_FORMAT_R32_TYPELESS) {
        ++g_win.preSkipFormat;
        return;
    }
    ID3D11Resource* privRes = nullptr;
    e.privDsv->GetResource(&privRes);
    const bool views = md.Width == e.w && md.Height == e.h && depthView(e.mainDepth, e.mainZSrv, e.mainZOf) && depthView(privRes, e.privZSrv, e.privZOf);
    releaseRef(privRes);
    if (!views) {
        ++g_win.preSkipView;
        return;
    }
    if (g_benchNoPre) {   // the variant bench's sep_nopre: it would have prefilled; the private depth stays at its clear (no reset owed)
        benchNoteOff();
        return;
    }
    if (!ensurePrefillGpu()) {
        ++g_win.preSkipShader;
        return;
    }
    if (!ensurePrefillTargets(e, e.w, e.h)) {
        ++g_win.preSkipTargets;
        return;
    }
    e.preRectOn = eyeRect(ei, &e.preRect);   // this frame's, decided now: the hull re-issues use the same one
    const bool timed = g_st.cpuStage < 0 && g_st.scope < 0;
    if (timed) stageOpen(ctx, kTsGbuf, false);
    {
        GfxSave save;
        save.capture(ctx);
        vScreenSetRenderTargetsRaw(ctx, 1, &e.preRtv, e.privDsv);
        vScreenOMSetDepthStencilStateRaw(ctx, g_gpu.depthAlways, 0);   // stencil off: the private stencil stays at its clear
        bindPrefillPass(ctx, e);
        vScreenPSSetShaderRaw(ctx, g_pgpu.prefillPs, nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = {e.mainZSrv, nullptr};
        vScreenPSSetShaderResourcesRaw(ctx, 0, 2, srvs);
        vScreenDrawRaw(ctx, 3, 0);
        ID3D11ShaderResourceView* none[2] = {};
        vScreenPSSetShaderResourcesRaw(ctx, 0, 2, none);   // off before the game's eye depth is bound back as its target
        save.restore(ctx);
    }
    if (timed) stageClose(ctx);
    e.prefilled = true;
    ++g_win.preFrames;
    if (e.preRectOn) ++g_win.preRect;
}

// The reset, at the end of the eye's G-buffer window (before the filter, the depth copy and everything after).
void resetPrefill(ID3D11DeviceContext* ctx, Eye& e) {
    e.prefilled = false;
    if (!e.privDsv || !e.privZSrv || !e.preSrv || !e.preMarkRtv || !g_pgpu.ok) {
        // Never expected (the prefill needed all of them). Left prefilled, the cover would take the cockpit shell for hull, so
        // the private depth goes back to its clear: no hull this eye-frame (counted as unreset on the prefill line).
        ++g_win.preUnreset;
        if (e.privDsv) ctx->ClearDepthStencilView(e.privDsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, e.clearDepth, e.clearStencil);
        return;
    }
    const bool timed = g_st.cpuStage < 0 && g_st.scope < 0;
    if (timed) stageOpen(ctx, kTsGbuf, false);
    {
        GfxSave save;
        save.capture(ctx);
        bindPrefillPass(ctx, e);
        // The mark: the private depth read, so no depth target.
        ID3D11RenderTargetView* markRtv[1] = {e.preMarkRtv};
        vScreenSetRenderTargetsRaw(ctx, 1, markRtv, nullptr);
        vScreenOMSetDepthStencilStateRaw(ctx, nullptr, 0);
        vScreenPSSetShaderRaw(ctx, g_pgpu.markPs, nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = {e.privZSrv, e.preSrv};
        vScreenPSSetShaderResourcesRaw(ctx, 0, 2, srvs);
        vScreenDrawRaw(ctx, 3, 0);
        // The reset: depth and stencil back to their clear where marked.
        ID3D11ShaderResourceView* none[2] = {};
        vScreenPSSetShaderResourcesRaw(ctx, 0, 2, none);
        vScreenSetRenderTargetsRaw(ctx, 0, nullptr, e.privDsv);
        vScreenOMSetDepthStencilStateRaw(ctx, g_pgpu.resetDss, e.clearStencil);
        uint32_t cb[4] = {e.records, 0u, e.w, e.h};
        std::memcpy(&cb[1], &e.clearDepth, sizeof e.clearDepth);
        vScreenUpdateSubresourceRaw(ctx, g_gpu.passCb, 0, nullptr, cb, 0, 0);
        vScreenPSSetConstantBuffersRaw(ctx, 0, 1, &g_gpu.passCb);
        vScreenPSSetShaderRaw(ctx, g_pgpu.resetPs, nullptr, 0);
        ID3D11ShaderResourceView* mark[2] = {e.preMarkSrv, nullptr};
        vScreenPSSetShaderResourcesRaw(ctx, 0, 2, mark);
        vScreenDrawRaw(ctx, 3, 0);
        vScreenPSSetShaderResourcesRaw(ctx, 0, 2, none);
        save.restore(ctx);
    }
    if (timed) stageClose(ctx);
    ++g_win.preResets;
}

// The seed: eye `ei`'s first hull-family G-buffer draw after the shell, with that draw's targets (`rtv`, `dsv`: the
// caller's references) and instance window bound. The own mask is dispatched, the private G-buffer made and cleared, and
// the eye's window opened (Gb::In); false leaves the eye Off for this frame (no slot target, no shader, refused).
// Dump mode runs it right after the seed draw's issue, separate mode right before it (hullLayerBeforeMainDraw).
bool seedEye(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv, int ei, UINT instances, UINT startInstance) {
    Eye& e = g_eyes[ei];
    const void* depthRes = resourceOf(dsv);
    {
        if (g_inEye >= 0 && g_inEye != ei) finishGbuffer(ctx, g_inEye, kEndShell);
        e.gb = Gb::Off;   // until the seed has run
        bool slotOk = false;
        if (engineVelocityLiveNow() && rtv[kSlotTarget]) {
            ID3D11Resource* res = nullptr;
            rtv[kSlotTarget]->GetResource(&res);
            D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            if (res) res->GetType(&dim);
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                D3D11_TEXTURE2D_DESC d{};
                static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
                slotOk = d.Format == DXGI_FORMAT_R32G32_FLOAT;
            }
            if (res) res->Release();
        }
        if (!slotOk) {
            ++g_win.noSlot;
            if (!g_saidNoSlot) {
                g_saidNoSlot = true;
                Log::get().note("hull layer: no-slot-target: the seed draw has no R32G32_FLOAT slot target at render target 6 (engine motion %s); the "
                                "layer does nothing on such an eye-frame (said once)",
                                engineVelocityLiveNow() ? "live" : "not live: it comes with fix.temporal_aa");
            }
            return false;
        }
        if (!ensureDevice(ctx) || !ensureGpu()) {
            ++g_win.noShader;
            return false;
        }
        // The private slot target is not made here (Station fixes): only an eye-frame with a re-issue the keep-own VS could not
        // take needs it, for the neighbour filter, and activateSlot makes it at that draw.
        ID3D11View* priv[5] = {};
        for (int i = 0; i < 4; ++i) priv[i] = privateView(ViewKind::Rtv, rtv[i], true);
        priv[4] = privateView(ViewKind::Dsv, dsv, true);
        for (auto* p : priv)
            if (!p) {
                ++g_win.refused;
                sayRefused("a private target could not be made");
                return false;
            }
        const char* why = nullptr;
        stageOpen(ctx, kTsMask, false);   // dump v2: the seed's copies and dispatch, timed with the filter
        const bool seeded = seedOwnMask(ctx, e, instances, startInstance, &why);
        stageClose(ctx);
        if (!seeded) {
            ++g_win.refused;
            sayRefused(why ? why : "?");
            return false;
        }
        // The private G-buffer, cleared: colour to 0, depth and stencil to the game's.
        static const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (int i = 0; i < 4; ++i) {
            e.privRtv[i] = static_cast<ID3D11RenderTargetView*>(priv[i]);
            e.privRtv[i]->AddRef();
            vScreenClearRenderTargetViewRaw(ctx, e.privRtv[i], zero);
        }
        e.privDsv = static_cast<ID3D11DepthStencilView*>(priv[4]);
        e.privDsv->AddRef();
        {
            // The eye's render size: the stages and their private copies are made at it only.
            ID3D11Resource* res = nullptr;
            e.privDsv->GetResource(&res);
            D3D11_TEXTURE2D_DESC dd{};
            if (res) static_cast<ID3D11Texture2D*>(res)->GetDesc(&dd);
            releaseRef(res);
            e.w = dd.Width;
            e.h = dd.Height;
        }
        uint8_t stencil = 0;
        e.clearDepth = clearDepthOf(depthRes, &stencil);
        ctx->ClearDepthStencilView(e.privDsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, e.clearDepth, stencil);
        e.clearStencil = stencil;
        ++g_win.clears;
        e.hdrGame = resourceOf(rtv[3]);
        e.slotGame = resourceOf(rtv[kSlotTarget]);
        for (int i = 0; i < 4; ++i) e.gbufGame[i] = resourceOf(rtv[i]);   // the AO chain names its eye by them (RT0 the HBAO's, RT2 the apply's)
        // Step 3: the game's eye depth, which the cover pass reads after the tonemap (the cockpit in front of the hull).
        if (detail::g_hullLayerSeparate) {
            ID3D11Resource* res = nullptr;
            dsv->GetResource(&res);
            if (res && FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&e.mainDepth)))) e.mainDepth = nullptr;
            releaseRef(res);
        }
        // Separate mode's dump: the game's G-buffer RT0 and eye depth, copied after the eye's G-buffer (finishGbuffer).
        if (detail::g_hullLayerSeparate && g_dump == Dump::Capturing) {
            ID3D11Resource* res = nullptr;
            rtv[0]->GetResource(&res);
            if (res && FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&e.gameRt0)))) e.gameRt0 = nullptr;
            releaseRef(res);
            dsv->GetResource(&res);
            if (res && FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&e.gameDepth)))) e.gameDepth = nullptr;
            releaseRef(res);
        }
        prefillEye(ctx, e, ei);   // Build B: the game's eye depth into the private depth, before the first hull draw
        e.gb = Gb::In;
        g_inEye = ei;
        g_seededFrame = true;
        ++g_win.seeds;
    }
    return true;
}

// After the game's issue of a hull-family G-buffer draw (or, separate mode, a hull decal): the private re-issue's
// bindings. Dump mode seeds here; separate mode seeded before the seed draw's issue, and re-issues the decals too.
bool beginHullDraw(ID3D11DeviceContext* ctx, UINT instances, UINT startInstance, bool decal) {
    QueryPerformanceCounter(&g_entryQpc);   // dump v2: the item's CPU time starts here
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    const auto decline = [&] {
        releaseTargets(rtv, dsv);
        return false;
    };
    if (!(rtv[0] && rtv[1] && rtv[2] && rtv[3] && dsv)) return decline();   // the G-buffer's four targets: not a shadow map or another pass
    if (!decal) ++g_win.hullSeen;
    const int ei = eyeFor(resourceOf(dsv), false);
    if (ei < 0) return decline();
    Eye& e = g_eyes[ei];
    if (!decal) ++e.gbufHull;   // the busy governor's count
    if (e.gb == Gb::Before) {
        // A hull draw before this eye's shell is not the own ship's pass; a decal never seeds; separate mode seeds before the issue.
        if (decal || !e.shell || detail::g_hullLayerSeparate) return decline();
        if (!seedEye(ctx, rtv, dsv, ei, instances, startInstance)) return decline();
    } else if (decal && e.gb == Gb::Done && e.decalOpen) {
        // the own decals after the depth copy (decalOpen): into the private G-buffer as well
    } else if (e.gb != Gb::In || g_inEye != ei) {
        return decline();
    }
    // A decal goes into the private layer only when it exports the slot (its slot target bound): the filter can then
    // clear it where it is a neighbour's, and an unpatched decal would paint wherever the private depth lets it.
    if (decal && !(rtv[kSlotTarget] && resourceOf(rtv[kSlotTarget]) == e.slotGame)) {
        ++g_win.decalNoSlot;
        return decline();
    }
    // Every hull-family G-buffer draw of the window: into the private RT0-3 and the private depth. Every other state is the
    // game's, still bound from its draw.
    ID3D11RenderTargetView* bind[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    for (int i = 0; i < 4; ++i) {
        bind[i] = static_cast<ID3D11RenderTargetView*>(privateView(ViewKind::Rtv, rtv[i], false));
        if (!bind[i]) return decline();
    }
    auto* bindDsv = static_cast<ID3D11DepthStencilView*>(privateView(ViewKind::Dsv, dsv, false));
    if (!bindDsv) return decline();
    // Station fixes: the keep-own VS culls the foreign instances (other ships, the station: the same families) before they are
    // rasterised, so the re-issue draws the own ship alone and needs no filter. A draw it cannot take (no variant, a geometry or
    // tessellation stage bound) is re-issued whole and needs the neighbour filter: the private slot target is made for the eye-frame.
    ID3D11VertexShader* gameVs = nullptr;
    ID3D11VertexShader* keepVs = nullptr;
    const char* keepWhy = "no own mask this eye-frame";
    if (e.maskSrv) {
        ctx->VSGetShader(&gameVs, nullptr, nullptr);
        ID3D11GeometryShader* gs = nullptr;
        ID3D11HullShader* hs = nullptr;
        ID3D11DomainShader* ds = nullptr;
        ctx->GSGetShader(&gs, nullptr, nullptr);
        ctx->HSGetShader(&hs, nullptr, nullptr);
        ctx->DSGetShader(&ds, nullptr, nullptr);
        const bool vsLast = !gs && !hs && !ds;   // the VS's SV_Position is what the rasterizer sees
        releaseRef(gs);
        releaseRef(hs);
        releaseRef(ds);
        keepWhy = !gameVs ? "no vertex shader bound" : !vsLast ? "a geometry or tessellation stage is bound" : nullptr;
        if (!keepWhy) keepVs = engineVelocityKeepOwnVs(ctx, gameVs, &keepWhy);
        if (!keepVs) releaseRef(gameVs);
    }
    // The private slot target: bound on every re-issue that exports the slot once the eye-frame needs the filter (from the first
    // draw that could not take the keep-own VS), so a later own draw over a neighbour's pixel replaces its slot too. Before that
    // draw the private layer holds only own pixels, whose slot the clear (-1, none) leaves unfiltered, as it should.
    const bool exportsSlot = rtv[kSlotTarget] && resourceOf(rtv[kSlotTarget]) == e.slotGame;
    if (!keepVs && exportsSlot && !e.privSlot) {
        if (auto* p = static_cast<ID3D11RenderTargetView*>(privateView(ViewKind::Rtv, rtv[kSlotTarget], true))) {
            static const float noSlot[4] = {-1.0f, 0.0f, 0.0f, 0.0f};
            e.privSlot = p;
            e.privSlot->AddRef();
            vScreenClearRenderTargetViewRaw(ctx, e.privSlot, noSlot);
        }
    }
    UINT n = 4;
    if (exportsSlot && e.privSlot) {
        bind[kSlotTarget] = static_cast<ID3D11RenderTargetView*>(privateView(ViewKind::Rtv, rtv[kSlotTarget], false));
        if (bind[kSlotTarget]) n = kSlotTarget + 1;
    }
    if (keepVs) {
        ++e.culled;
        ++g_win.culledDraws;
    } else {
        ++e.filterDraws;
        ++g_win.filterDraws;
        if (n <= kSlotTarget) {
            ++e.noSlotDraws;
            ++g_win.noSlotDraws;
        }
        if (!g_saidKeepOwn) {
            g_saidKeepOwn = true;
            Log::get().note("hull layer: a %s re-issue could not take the keep-own vertex shader (%s); such draws are re-issued whole and filtered "
                            "(said once)",
                            decal ? "decal" : "hull", keepWhy ? keepWhy : "?");
        }
    }
    // A decal reads the game's copy of G-buffer RT0 as its base (t0); in the layer it reads the private copy of that copy
    // (hullLayerAfterCopy), else it blends over the main pass's RT0, which no longer holds the own hull.
    ID3D11ShaderResourceView* srv[kSrvSlots] = {};
    bool srvSwapped = false;
    if (decal) {
        ctx->PSGetShaderResources(0, kSrvSlots, srv);
        ID3D11ShaderResourceView* bindSrv[kSrvSlots] = {};
        for (UINT i = 0; i < kSrvSlots; ++i) {
            bindSrv[i] = srv[i];
            if (!srv[i] || !findMirror(resourceOf(srv[i]))) continue;
            if (auto* p = static_cast<ID3D11ShaderResourceView*>(privateView(ViewKind::Srv, srv[i], false))) {
                bindSrv[i] = p;
                srvSwapped = true;
            }
        }
        // t3 is engine motion's snapshot of its slot target, taken before the game's issue (the 2026-11-11 log: an eye-sized
        // R32G32_FLOAT a copy wrote): in separate mode the world's slots behind the hull. The guarded decal PS reads it for MRT6's
        // depth alone, never for colour. With a private slot target bound, hullLayerAfterCopy made the private twin from it at
        // the same copy and the loop above swapped it in; with MRT6 not bound (no private slot target this eye-frame, as on every
        // re-issue culled by the keep-own VS) the PS's MRT6 output goes nowhere, so t3 is left empty rather than the world's.
        const bool mirrorSwap = srvSwapped;
        if (srv[kOverlayBaseSlot] && e.overlayGame && resourceOf(srv[kOverlayBaseSlot]) == e.overlayGame) {
            if (bindSrv[kOverlayBaseSlot] != srv[kOverlayBaseSlot]) {
                ++g_win.overlaySwapped;
            } else if (n <= kSlotTarget) {
                bindSrv[kOverlayBaseSlot] = nullptr;
                srvSwapped = true;
                ++g_win.overlayUnbound;
            } else {
                ++g_win.overlayKept;
            }
        }
        if (srvSwapped) vScreenPSSetShaderResourcesRaw(ctx, 0, kSrvSlots, bindSrv);
        if (mirrorSwap) ++g_win.decalSrvSwaps;
        if (unmirroredWanted()) {
            const uint64_t ps = bindingShaderHash(BindSlot::Ps);
            for (UINT i = 0; i < kSrvSlots; ++i)
                if (srv[i] && bindSrv[i] == srv[i]) noteUnmirrored(ps, 'P', i, srv[i], ei, true);
        }
    }
    vScreenSetRenderTargetsRaw(ctx, n, bind, bindDsv);
    ID3D11ShaderResourceView* gameVsMask = nullptr;
    if (keepVs) {
        // The keep-own VS and the eye's own mask at VS t127; End puts the game's (or engine motion's) VS and the game's t127 back.
        ctx->VSGetShaderResources(kKeepOwnVsSlot, 1, &gameVsMask);
        ctx->VSSetShaderResources(kKeepOwnVsSlot, 1, &e.maskSrv);
        vScreenVSSetShaderRaw(ctx, keepVs, nullptr, 0);
    }
    if (decal) {
        ++e.decals;
        ++g_win.decalsReissued;
    } else {
        ++e.hull;
    }
    g_pending = Pending{};
    g_pending.active = true;
    g_pending.eye = ei;
    g_pending.stage = -1;
    g_pending.keepOwn = keepVs != nullptr;
    g_pending.vs = keepVs ? gameVs : nullptr;   // the reference passes to End
    g_pending.vsMask = gameVsMask;
    for (UINT i = 0; i < kSrvSlots; ++i) g_pending.srv[i] = srv[i];   // the references pass to End, which restores when swapped
    g_pending.srvSwapped = srvSwapped;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) g_pending.rtv[i] = rtv[i];   // the references pass to End
    g_pending.dsv = dsv;
    stageOpen(ctx, kTsGbuf, true);   // dump v2: hullLayerEnd closes it right after the re-issue
    return true;
}

// The end of an eye's private G-buffer: the filter pass clears the neighbours' pixels, the count pass counts them, and the
// stats are copied for the readback (and, on the dump frame, the private depth and the stats for the dump).
void finishGbuffer(ID3D11DeviceContext* ctx, int ei, End why) {
    if (ei < 0) return;
    Eye& e = g_eyes[ei];
    if (g_inEye == ei) g_inEye = -1;
    if (e.gb != Gb::In) return;
    e.gb = Gb::Done;
    e.decalOpen = why == kEndCopy;   // the hull decals follow the depth copy (decalOpen)
    ++g_win.ended[why];
    if (e.prefilled) resetPrefill(ctx, e);   // Build B: hull-only again before the filter, the dump's copy and the depth copy
    if (!e.privDsv || !e.maskSrv || !e.stats) return;
    ID3D11Resource* slotRes = nullptr;
    if (e.privSlot) e.privSlot->GetResource(&slotRes);
    ID3D11Resource* depthRes = nullptr;
    e.privDsv->GetResource(&depthRes);
    if (!depthRes) {
        releaseRef(slotRes);
        return;
    }
    // Station fixes: the filter and count run only on an eye-frame that needed them (a re-issue the keep-own VS could not take
    // made the private slot target). An eye whose re-issues were all culled in the VS for kEvictFrames eye-frames in a row lets
    // the slot target's private copy go (55 MB an eye at 2816x2448).
    if (slotRes) {
        e.slotIdle = 0;
        if (e.slotSrvOf != slotRes) {
            releaseRef(e.slotSrv);
            e.slotSrvOf = nullptr;
            if (SUCCEEDED(g_device->CreateShaderResourceView(slotRes, nullptr, &e.slotSrv)) && e.slotSrv) e.slotSrvOf = slotRes;
        }
    } else if (++e.slotIdle == kEvictFrames && e.slotGame && findMirror(e.slotGame)) {
        releaseRef(e.slotSrv);   // it holds the copy's texture
        e.slotSrvOf = nullptr;
        dropMirror(e.slotGame);
        Log::get().note("hull layer: the private copy of eye %d's slot target dropped: no re-issue needed the neighbour filter for %u eye-frames (%u "
                        "copies held)",
                        ei, kEvictFrames, static_cast<unsigned>(g_mirrors.size()));
    }
    D3D11_TEXTURE2D_DESC dd{};
    static_cast<ID3D11Texture2D*>(depthRes)->GetDesc(&dd);
    if (slotRes && e.slotSrv) {
        stageOpen(ctx, kTsMask, false);   // dump v2: the filter and the count
        uint32_t cb[4] = {e.records, 0u, dd.Width, dd.Height};
        std::memcpy(&cb[1], &e.clearDepth, sizeof e.clearDepth);
        vScreenUpdateSubresourceRaw(ctx, g_gpu.passCb, 0, nullptr, cb, 0, 0);
        // The filter: RT0-3 and the depth of every neighbour pixel back to clear.
        {
            GfxSave save;
            save.capture(ctx);
            vScreenSetRenderTargetsRaw(ctx, 4, e.privRtv, e.privDsv);
            vScreenOMSetBlendStateRaw(ctx, nullptr, nullptr, 0xFFFFFFFFu);
            vScreenOMSetDepthStencilStateRaw(ctx, g_gpu.depthAlways, 0);
            ctx->RSSetState(g_gpu.noCull);
            D3D11_VIEWPORT vp{};
            vp.Width = static_cast<float>(dd.Width);
            vp.Height = static_cast<float>(dd.Height);
            vp.MaxDepth = 1.0f;
            vScreenRSSetViewportsRaw(ctx, 1, &vp);
            ctx->IASetInputLayout(nullptr);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            vScreenVSSetShaderRaw(ctx, g_gpu.filterVs, nullptr, 0);
            ctx->HSSetShader(nullptr, nullptr, 0);
            ctx->DSSetShader(nullptr, nullptr, 0);
            ctx->GSSetShader(nullptr, nullptr, 0);
            vScreenPSSetShaderRaw(ctx, g_gpu.filterPs, nullptr, 0);
            ID3D11ShaderResourceView* srvs[2] = {e.slotSrv, e.maskSrv};
            vScreenPSSetShaderResourcesRaw(ctx, 0, 2, srvs);
            vScreenPSSetConstantBuffersRaw(ctx, 0, 1, &g_gpu.passCb);
            vScreenDrawRaw(ctx, 3, 0);
            save.restore(ctx);
        }
        // The count: stats[1] = the filtered pixels.
        {
            CsSave save;
            save.capture(ctx);
            ID3D11ShaderResourceView* srvs[2] = {e.slotSrv, e.maskSrv};
            ID3D11UnorderedAccessView* uavs[2] = {e.statsUav, nullptr};
            static const UINT keep[2] = {~0u, ~0u};
            ctx->CSSetShader(g_gpu.countCs, nullptr, 0);
            ctx->CSSetShaderResources(0, 2, srvs);
            ctx->CSSetUnorderedAccessViews(0, 2, uavs, keep);
            ctx->CSSetConstantBuffers(0, 1, &g_gpu.passCb);
            ctx->Dispatch((dd.Width + 7u) / 8u, (dd.Height + 7u) / 8u, 1);
            save.restore(ctx);
        }
        stageClose(ctx);
        ++g_win.filters;
    }
    // The readback, a few frames later (never a stall).
    Readback* slot = nullptr;
    for (auto& r : e.ring)
        if (!r.pending) {
            slot = &r;
            break;
        }
    if (slot) {
        if (!slot->staging) slot->staging = makeStagingBuffer(kStatsBytes);
        if (slot->staging) {
            vScreenCopyResourceRaw(ctx, slot->staging, e.stats);
            slot->frame = g_frame;
            slot->pending = true;
        }
    } else {
        ++g_win.readbackBusy;
    }
    if (g_dump == Dump::Capturing) {
        // Separate mode: the game's G-buffer RT0 and main depth as the eye's G-buffer left them (the own ship discarded).
        if (e.gameRt0) copyToStaging(ctx, e.gameRt0, e.stageGbuf0);
        if (e.gameDepth) copyToStaging(ctx, e.gameDepth, e.stageMainDepth);
        copyToStaging(ctx, static_cast<ID3D11Texture2D*>(depthRes), e.stageDepth);
        if (!e.stageStats) e.stageStats = makeStagingBuffer(kStatsBytes);
        if (e.stageStats) vScreenCopyResourceRaw(ctx, e.stageStats, e.stats);
        e.dumpHull = e.hull;
    }
    releaseRef(slotRes);
    releaseRef(depthRes);
}

void noteShell(ID3D11DeviceContext* ctx) {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    if (rtv[0] && rtv[1] && rtv[2] && rtv[3] && dsv) {   // the G-buffer's four targets: not a shadow map or another pass
        const int e = eyeFor(resourceOf(dsv), true);
        if (e >= 0) {
            if (g_inEye >= 0 && g_inEye != e) finishGbuffer(ctx, g_inEye, kEndShell);   // the next eye's shell ends the last one's G-buffer
            if (g_eyes[e].gb == Gb::Before) g_eyes[e].shell = true;
        }
    }
    releaseTargets(rtv, dsv);
}

// ---- the stages -------------------------------------------------------------------------------------------------------

// The eye's HDR target named by a draw's RT0 (the game's), staged on the dump frame (the last call wins).
void stageGameHdr(ID3D11DeviceContext* ctx, Eye& e, ID3D11RenderTargetView* rt0) {
    if (g_dump != Dump::Capturing || !rt0) return;
    ID3D11Resource* hdr = nullptr;
    rt0->GetResource(&hdr);
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    if (hdr) hdr->GetType(&dim);
    if (hdr && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) copyToStaging(ctx, static_cast<ID3D11Texture2D*>(hdr), e.stageGame);
    releaseRef(hdr);
}

// Station fixes: the HDR writers waiting in eye `ei`'s unknown-pair window, now that a lighting draw followed them, are said as
// unknown lighting pairs (once a pair, kMaxUnknownPairs at most); the cap is spent only on these.
void commitPending(Eye& e, int ei) {
    for (uint32_t k = 0; k < e.pendingCount; ++k) {
        const uint64_t vs = e.pending[k][0], ps = e.pending[k][1];
        bool said = false;
        for (uint32_t i = 0; i < g_unknownCount && !said; ++i) said = g_unknownPairs[i][0] == vs && g_unknownPairs[i][1] == ps;
        if (said) continue;
        if (g_unknownCount >= kMaxUnknownPairs) {
            if (!g_saidUnknownCap) {
                g_saidUnknownCap = true;
                Log::get().note("hull layer: %u unknown lighting pairs said; no more are (said once)", kMaxUnknownPairs);
            }
            break;
        }
        g_unknownPairs[g_unknownCount][0] = vs;
        g_unknownPairs[g_unknownCount][1] = ps;
        ++g_unknownCount;
        ++g_win.unknownPairs;
        Log::get().note("hull layer: unknown lighting pair vs %016llX ps %016llX writes the eye's HDR between the shadow mask and a later lighting draw "
                        "(eye %d, frame %u; not re-issued; said once a pair, %u so far)",
                        static_cast<unsigned long long>(vs), static_cast<unsigned long long>(ps), ei, g_frame, g_unknownCount);
    }
    e.pendingCount = 0;
}

// nightVision: the game's night-vision draw (kNightVisionVs), a kLight stage that is re-issued and staged like a light but
// leaves the lighting window, the unknown-pair window and the stage eye as they are.
bool beginStage(ID3D11DeviceContext* ctx, Stage stage, uint8_t bit, bool nightVision = false) {
    QueryPerformanceCounter(&g_entryQpc);   // dump v2: the item's CPU time starts here
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11ShaderResourceView* srv[kSrvSlots] = {};
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    ctx->PSGetShaderResources(0, kSrvSlots, srv);
    const auto decline = [&](bool counterpartMissing) {
        if (counterpartMissing) g_noCpFrame[stage] = true;
        releaseTargets(rtv, dsv);
        for (auto*& s : srv) releaseRef(s);
        return false;
    };
    // The stage's eye: the eye depth bound as its depth target, else read as one of its resources (the depth copy); a
    // lighting draw that shows neither is the eye whose stage ran last, while its lighting window is open (a lighting
    // pass of another view, outside the window, gets no private targets).
    int ei = dsv ? eyeFor(resourceOf(dsv), false) : -1;
    for (UINT i = 0; ei < 0 && i < kSrvSlots; ++i)
        if (srv[i]) ei = eyeFor(resourceOf(srv[i]), false);
    if (ei < 0 && stage == kLight && !nightVision && g_stageEye >= 0 && g_eyes[g_stageEye].lightOpen) ei = g_stageEye;
    // The AO apply binds no depth: its eye is the one whose G-buffer RT2 it writes, or whose AO it reads.
    if (ei < 0 && stage == kAo) {
        bool both = false;
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            if (rtv[i]) noteAoEye(aoEyeOf(resourceOf(rtv[i])), &ei, &both);
        for (UINT i = 0; i < kSrvSlots; ++i)
            if (srv[i]) noteAoEye(aoEyeOf(resourceOf(srv[i])), &ei, &both);
        if (both) ei = -1;
    }
    if (ei < 0 || !g_device) return decline(true);
    Eye& e = g_eyes[ei];
    // Station fixes: a stage is the eye's only at the eye's render size (the private G-buffer's). An offscreen pass of the same
    // shaders (a station's lights into 256x256, 512x512, 612x404 or 904x404 targets) gets no private copies and opens, ends and
    // counts nothing. A lighting draw's scratch R16 at RT1 is left unbound in the re-issue (read by nothing after the lighting;
    // a null target drops the writes), so its size is not asked.
    bool nullRt1 = false;
    if (e.w) {
        const auto eyeSized = [&](ID3D11View* v, bool* r16) {
            ID3D11Resource* res = nullptr;
            v->GetResource(&res);
            D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
            if (res) res->GetType(&dim);
            D3D11_TEXTURE2D_DESC d{};
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
            releaseRef(res);
            if (r16) *r16 = d.Format == DXGI_FORMAT_R16_TYPELESS || d.Format == DXGI_FORMAT_R16_FLOAT || d.Format == DXGI_FORMAT_R16_UNORM;
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D && d.Width == e.w && d.Height == e.h) return true;
            if (!g_saidOffscreen[stage]) {
                g_saidOffscreen[stage] = true;
                Log::get().note("hull layer: a %s stage draw into a %ux%u format %u target is not at eye %d's render size %ux%u: no private copy, not "
                                "re-issued (said once a stage)",
                                stage == kCopy ? "depth copy" : stage == kMask ? "shadow mask" : stage == kLight ? "lighting" : "AO apply", d.Width,
                                d.Height, static_cast<unsigned>(d.Format), ei, e.w, e.h);
            }
            return false;
        };
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            if (!rtv[i]) continue;
            bool r16 = false;
            const bool sized = eyeSized(rtv[i], &r16);
            if (stage == kLight && i == 1 && r16) {
                nullRt1 = true;
                continue;
            }
            if (!sized) {
                ++g_win.offscreen;
                return decline(false);
            }
        }
        if (dsv && !eyeSized(dsv, nullptr)) {
            ++g_win.offscreen;
            return decline(false);
        }
    }
    if (stage == kAo) e.aoSeen |= kAoApplyBit;
    if (e.gb == Gb::In) finishGbuffer(ctx, ei, stage == kCopy ? kEndCopy : kEndLight);   // the eye's G-buffer is over; the game's state is back
    if (stage != kCopy) e.decalOpen = false;   // the decals are over once the eye's AO, shadow mask or lighting starts
    if (stage == kAo) {
        // Not a lighting stage: it does not open the lighting window or move the stage eye. It needs the eye's whole AO
        // chain this frame, or the private AO it would read is last frame's.
        if (e.gb != Gb::Done || (e.aoDone & kAoAllSteps) != kAoAllSteps) return decline(true);
    } else if (!nightVision) {
        if (g_stageEye >= 0 && g_stageEye != ei) g_eyes[g_stageEye].pendingCount = 0;   // the other eye's lighting is over: its waiting writers were not lighting
        g_stageEye = ei;
    }
    if (stage != kCopy && stage != kAo && !nightVision) {
        e.lightOpen = true;   // the shadow mask opens the lighting window; the sky closes it
        e.lightSpan = 0;
    }
    // Station fixes: the unknown-pair window opens at the shadow mask; a lighting draw in it makes the HDR writers before it unknown
    // lighting (they lit between the shadow mask and a later lighting draw); writers no lighting draw follows were forward draws.
    if (stage == kMask) {
        e.unknownOpen = true;
        e.pendingCount = 0;
    }
    if (stage == kLight && e.unknownOpen && !nightVision) commitPending(e, ei);
    if (stage == kLight) stageGameHdr(ctx, e, rtv[0]);   // the game's HDR after this lighting draw (or night vision): the last one is the comparison
    if (e.gb != Gb::Done) return decline(true);
    if (bit) {
        if (e.stagesDone & bit) return decline(false);   // once per eye per frame: a later use of the same pair is not the eye's
        e.stagesDone |= bit;
    }
    if (stage == kCopy && rtv[0]) e.copyGame = resourceOf(rtv[0]);   // the AO chain's first step reads it: that names its eye
    // Targets: every bound one gets a private counterpart (made now for the stage's own: f39, f60, the scratch R16).
    ID3D11RenderTargetView* bindRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    uint32_t swapped = 0;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        if (!rtv[i] || (nullRt1 && i == 1)) continue;   // the scratch R16: unbound (Station fixes)
        bindRtv[i] = static_cast<ID3D11RenderTargetView*>(privateView(ViewKind::Rtv, rtv[i], true));
        if (!bindRtv[i]) return decline(true);
        ++swapped;
    }
    if (nullRt1) ++g_win.nullRt1;
    // The depth target must be the eye's (mirrored at the seed); it is never made here.
    ID3D11DepthStencilView* bindDsv = nullptr;
    if (dsv) {
        bindDsv = static_cast<ID3D11DepthStencilView*>(privateView(ViewKind::Dsv, dsv, false));
        if (!bindDsv) return decline(true);
        ++swapped;
    }
    // Resources: the G-buffer, depth, depth copy, shadow mask and HDR have counterparts; everything else (the shadow map,
    // the light list, the lookups) stays the game's.
    ID3D11ShaderResourceView* bindSrv[kSrvSlots] = {};
    bool srvSwapped = false;
    for (UINT i = 0; i < kSrvSlots; ++i) {
        bindSrv[i] = srv[i];
        if (!srv[i] || !findMirror(resourceOf(srv[i]))) continue;
        auto* p = static_cast<ID3D11ShaderResourceView*>(privateView(ViewKind::Srv, srv[i], false));
        if (!p) return decline(true);
        bindSrv[i] = p;
        srvSwapped = true;
        ++swapped;
    }
    if (stage == kAo && !srvSwapped) return decline(true);   // the AO it reads has no private copy: the game's AO is not the hull's
    if (unmirroredWanted()) {
        const uint64_t ps = bindingShaderHash(BindSlot::Ps);
        for (UINT i = 0; i < kSrvSlots; ++i)
            if (srv[i] && bindSrv[i] == srv[i]) noteUnmirrored(ps, 'P', i, srv[i], ei);
    }
    if (srvSwapped) vScreenPSSetShaderResourcesRaw(ctx, 0, kSrvSlots, bindSrv);
    vScreenSetRenderTargetsRaw(ctx, boundCount(bindRtv), bindRtv, bindDsv);
    g_win.subst[stage] += swapped;
    if (stage == kLight) ++e.lightDraws;
    if (nightVision) ++g_win.nightVision;
    if (stage == kAo) {
        e.aoDone |= kAoApplyBit;
        ++g_win.aoApplied;
    }
    g_pending = Pending{};
    g_pending.active = true;
    g_pending.eye = ei;
    g_pending.stage = stage;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) g_pending.rtv[i] = rtv[i];
    g_pending.dsv = dsv;
    for (UINT i = 0; i < kSrvSlots; ++i) g_pending.srv[i] = srv[i];
    g_pending.srvSwapped = srvSwapped;
    g_pending.hdrGame = rtv[0] ? resourceOf(rtv[0]) : nullptr;
    stageOpen(ctx, stage == kLight ? kTsLight : kTsScreen, true);   // dump v2: hullLayerEnd closes it right after the re-issue
    return true;
}

// A draw inside an eye's lighting window that is no lighting or stage draw: the sky (or kLightSpan draws with no lighting draw)
// closes the window. Station fixes: one that writes the eye's HDR waits in the unknown-pair window; it is said as an unknown
// lighting pair only when a later lighting draw follows (commitPending), so the forward draws after the lighting (2CECEC30,
// 925ACEDA at the station, while no sky came) no longer spend the cap. A forward draw -- one writing the HDR with depth writes
// on: geometry, where light volumes and full-screen passes write no depth -- closes the unknown-pair window.
void noteInLightWindow(ID3D11DeviceContext* ctx, uint64_t vs) {
    Eye& e = g_eyes[g_stageEye];
    if (vs == kSkyVs || ++e.lightSpan > kLightSpan) {
        e.lightOpen = false;
        e.unknownOpen = false;
        e.pendingCount = 0;
        return;
    }
    if (!e.hdrGame || !e.unknownOpen) return;
    ID3D11RenderTargetView* rt0 = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(1, &rt0, &dsv);
    const bool hdr = rt0 && resourceOf(rt0) == e.hdrGame;
    releaseRef(rt0);
    bool depthWrites = false;
    if (hdr && dsv) {
        ID3D11DepthStencilState* ds = nullptr;
        UINT ref = 0;
        ctx->OMGetDepthStencilState(&ds, &ref);
        D3D11_DEPTH_STENCIL_DESC dd{};
        if (ds) ds->GetDesc(&dd);
        else dd.DepthEnable = TRUE, dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;   // the default state writes depth
        depthWrites = dd.DepthEnable && dd.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
        releaseRef(ds);
    }
    releaseRef(dsv);
    if (!hdr) return;
    if (depthWrites) {
        e.unknownOpen = false;   // a forward draw: the lighting is over
        e.pendingCount = 0;
        return;
    }
    const uint64_t ps = bindingShaderHash(BindSlot::Ps);
    for (uint32_t i = 0; i < g_unknownCount; ++i)
        if (g_unknownPairs[i][0] == vs && g_unknownPairs[i][1] == ps) return;   // said already
    for (uint32_t i = 0; i < e.pendingCount; ++i)
        if (e.pending[i][0] == vs && e.pending[i][1] == ps) return;
    if (e.pendingCount >= kMaxPendingPairs) return;
    e.pending[e.pendingCount][0] = vs;
    e.pending[e.pendingCount][1] = ps;
    ++e.pendingCount;
}

// ---- the AO chain's compute steps ---------------------------------------------------------------------------------------

void releaseCsSlots(ID3D11ShaderResourceView** srv, ID3D11UnorderedAccessView** uav) {
    for (UINT i = 0; i < kCsSrvSlots; ++i) releaseRef(srv[i]);
    for (UINT i = 0; i < kCsUavSlots; ++i) releaseRef(uav[i]);
}

// Right after the game's dispatch of AO step `step`, with its bindings still bound: the step's eye, by what it reads; its
// targets given private copies and its private-copied inputs swapped. True: the caller dispatches the same groups again
// and calls hullLayerEndDispatch. The shader, constant buffers and samplers stay the game's; so do inputs with no private
// copy (the HBAO's kernel buffer).
bool beginAoStep(ID3D11DeviceContext* ctx, int step) {
    QueryPerformanceCounter(&g_entryQpc);   // dump v2: the item's CPU time starts here
    ID3D11ShaderResourceView* srv[kCsSrvSlots] = {};
    ID3D11UnorderedAccessView* uav[kCsUavSlots] = {};
    ctx->CSGetShaderResources(0, kCsSrvSlots, srv);
    ctx->CSGetUnorderedAccessViews(0, kCsUavSlots, uav);
    const auto decline = [&] {
        releaseCsSlots(srv, uav);
        return false;
    };
    int ei = -1;
    bool both = false;
    for (UINT i = 0; i < kCsSrvSlots; ++i)
        if (srv[i]) noteAoEye(aoEyeOf(resourceOf(srv[i])), &ei, &both);
    if (ei < 0 || both) {
        // Counted only while an eye has a private G-buffer this frame (off it, every chain dispatch would count).
        if (g_eyes[0].gb == Gb::Done || g_eyes[1].gb == Gb::Done) ++g_win.aoNoEye;
        return decline();
    }
    Eye& e = g_eyes[ei];
    const uint8_t bit = static_cast<uint8_t>(1u << step);
    if (e.aoSeen & bit) return decline();   // once per eye per frame: a later use of the step is not the eye's chain
    e.aoSeen |= bit;
    // The eye's private G-buffer this frame, its depth copy re-issued, and each earlier step re-issued: otherwise a private
    // input is last frame's (the boundary counts the step as missed).
    const uint8_t before = static_cast<uint8_t>(bit - 1u);
    if (e.gb != Gb::Done || !(e.stagesDone & 1u) || (e.aoDone & before) != before) return decline();
    // Targets: every bound UAV gets a private copy (made at first use: the chain's own outputs). One that cannot (not a 2D
    // texture, multisampled, the cap) declines the step: the re-issue would write the game's own target.
    ID3D11UnorderedAccessView* bindUav[kCsUavSlots] = {};
    bool anyUav = false;
    for (UINT i = 0; i < kCsUavSlots; ++i) {
        if (!uav[i]) continue;
        bindUav[i] = static_cast<ID3D11UnorderedAccessView*>(privateView(ViewKind::Uav, uav[i], true));
        if (!bindUav[i]) return decline();
        anyUav = true;
    }
    if (!anyUav) return decline();
    // Inputs: those with a private copy (the depth copy, RT0, an earlier step's output) are swapped.
    ID3D11ShaderResourceView* bindSrv[kCsSrvSlots] = {};
    bool srvSwapped = false;
    for (UINT i = 0; i < kCsSrvSlots; ++i) {
        bindSrv[i] = srv[i];
        if (!srv[i] || !findMirror(resourceOf(srv[i]))) continue;
        auto* p = static_cast<ID3D11ShaderResourceView*>(privateView(ViewKind::Srv, srv[i], false));
        if (!p) return decline();
        bindSrv[i] = p;
        srvSwapped = true;
    }
    if (!srvSwapped) return decline();   // nothing private read: the re-issue would repeat the game's own result
    // The outputs name this eye for the steps after it and the apply draw.
    for (UINT i = 0; i < kCsUavSlots; ++i) {
        const void* out = uav[i] ? resourceOf(uav[i]) : nullptr;
        if (!out || aoEyeOf(out) == ei) continue;
        if (e.aoOutCount < kAoOwned) e.aoOut[e.aoOutCount++] = out;
    }
    // The game's dispatch was hazard-free and the swap is one to one, so no private view is bound twice; the inputs go first
    // (the game's UAVs, still bound, are none of the private copies).
    for (UINT i = 0; i < kCsSrvSlots; ++i)
        if (srv[i] && bindSrv[i] == srv[i]) noteUnmirrored(kAoCs[step], 'C', i, srv[i], ei);
    UINT keep[kCsUavSlots];
    for (auto& k : keep) k = ~0u;   // -1: a counter stays as it is
    ctx->CSSetShaderResources(0, kCsSrvSlots, bindSrv);
    ctx->CSSetUnorderedAccessViews(0, kCsUavSlots, bindUav, keep);
    g_dispatch = DispatchPending{};
    g_dispatch.active = true;
    for (UINT i = 0; i < kCsSrvSlots; ++i) g_dispatch.srv[i] = srv[i];   // the references pass to End
    for (UINT i = 0; i < kCsUavSlots; ++i) g_dispatch.uav[i] = uav[i];
    e.aoDone |= bit;
    ++g_win.aoDispatches;
    stageOpen(ctx, kTsScreen, true);   // dump v2: hullLayerEndDispatch closes it right after the re-dispatch
    return true;
}

// Right after the game's indirect dispatch of reflection step `step` (kReflCs), with its bindings still bound: the eye by its
// inputs (the G-buffer, the depth copy), its HDR at a UAV swapped for the private HDR, its other targets given private copies,
// its private-copied inputs swapped. Separate mode only, once per eye and step a frame, and only on an eye whose private
// G-buffer and HDR are this frame's. True: the caller dispatches the same indirect arguments again (the game's tile list)
// and calls hullLayerEndDispatch.
// The reflection dump: a staging copy of the 2D texture under a bound SRV (null, or not a 2D texture: nothing).
void stageSrvTexture(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* v, ID3D11Texture2D*& stage) {
    if (!v) return;
    ID3D11Resource* res = nullptr;
    v->GetResource(&res);
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    if (res) res->GetType(&dim);
    if (res && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) copyToStaging(ctx, static_cast<ID3D11Texture2D*>(res), stage);
    releaseRef(res);
}

// The reflection dump: the whole buffer bound at CS cb1 (the pass's stencil mask is cb1[26].y), and the constant it starts at.
void stageReflCb(ID3D11DeviceContext* ctx, Eye& e) {
    ID3D11Buffer* cb = nullptr;
    UINT first = 0, num = 0;
    ID3D11DeviceContext1* ctx1 = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) && ctx1) {
        ctx1->CSGetConstantBuffers1(1, 1, &cb, &first, &num);
        ctx1->Release();
    } else {
        ctx->CSGetConstantBuffers(1, 1, &cb);
    }
    if (!cb) return;
    D3D11_BUFFER_DESC d{};
    cb->GetDesc(&d);
    if (e.stageReflCb) {
        D3D11_BUFFER_DESC s{};
        e.stageReflCb->GetDesc(&s);
        if (s.ByteWidth != d.ByteWidth) releaseRef(e.stageReflCb);
    }
    if (!e.stageReflCb) e.stageReflCb = makeStagingBuffer(d.ByteWidth);
    if (e.stageReflCb) {
        vScreenCopyResourceRaw(ctx, e.stageReflCb, cb);
        e.reflCbFirst = first;
    }
    cb->Release();
}

bool beginReflStep(ID3D11DeviceContext* ctx, int step) {
    // Separate mode; in dump mode the capture frame only, so the dump holds the private HDR after the pass too.
    if (!detail::g_hullLayerSeparate && g_dump != Dump::Capturing) return false;
    QueryPerformanceCounter(&g_entryQpc);   // dump v2: the item's CPU time starts here
    ID3D11ShaderResourceView* srv[kCsSrvSlots] = {};
    ID3D11UnorderedAccessView* uav[kCsUavSlots] = {};
    ctx->CSGetShaderResources(0, kCsSrvSlots, srv);
    ctx->CSGetUnorderedAccessViews(0, kCsUavSlots, uav);
    Eye* ep = nullptr;
    const auto decline = [&](const char* why) {
        ++g_win.reflDeclined;
        if (ep) ep->reflWhy = why;
        releaseCsSlots(srv, uav);
        return false;
    };
    int ei = -1;
    bool both = false;
    for (UINT i = 0; i < kCsSrvSlots; ++i)
        if (srv[i]) noteAoEye(aoEyeOf(resourceOf(srv[i])), &ei, &both);
    if (ei < 0 || both) {
        if (g_eyes[0].gb == Gb::Done || g_eyes[1].gb == Gb::Done) ++g_win.reflNoEye;
        releaseCsSlots(srv, uav);
        return false;
    }
    Eye& e = g_eyes[ei];
    const uint8_t bit = static_cast<uint8_t>(1u << step);
    if (e.reflSeen & bit) {   // once per eye per frame: a later use of the shader is not the eye's pass
        releaseCsSlots(srv, uav);
        return false;
    }
    e.reflSeen |= bit;
    ep = &e;
    const bool capture = g_dump == Dump::Capturing;
    if (capture) {
        // The capture frame: the game's HDR right after the game's dispatch of this step (the last step wins: after both), and
        // once an eye what the pass reads, the stencil (t4) and the G-buffer's t11, and its cb1 (the stencil mask, cb1[26].y).
        for (UINT i = 0; i < kCsUavSlots; ++i)
            if (uav[i] && resourceOf(uav[i]) == e.hdrGame) {
                ID3D11Resource* res = nullptr;
                uav[i]->GetResource(&res);
                D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
                if (res) res->GetType(&dim);
                if (res && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
                    copyToStaging(ctx, static_cast<ID3D11Texture2D*>(res), e.stageGameRefl);
                    ++e.dumpReflGame;
                }
                releaseRef(res);
                break;
            }
        if (!e.stageGameStencil) stageSrvTexture(ctx, srv[4], e.stageGameStencil);
        if (!e.stageGameT11) stageSrvTexture(ctx, srv[11], e.stageGameT11);
        if (!e.stageReflCb) stageReflCb(ctx, e);
    }
    if (e.gb != Gb::Done || !e.hdrGame || !findMirror(e.hdrGame)) return decline("no private HDR lit this frame");
    if (g_benchNoRefl) {   // the variant bench's sep_norefl: the step would have been re-issued; it is not (not a decline)
        benchNoteOff();
        e.reflWhy = "the variant bench's sep_norefl";
        releaseCsSlots(srv, uav);
        return false;
    }
    // Targets: the eye's HDR must be one of them (the pass adds into it); it goes to its private twin, made by the lighting.
    // Every other target gets a private copy (the R16 one).
    ID3D11UnorderedAccessView* bindUav[kCsUavSlots] = {};
    bool hdr = false, other[kCsUavSlots] = {};
    for (UINT i = 0; i < kCsUavSlots; ++i) {
        if (!uav[i]) continue;
        const bool isHdr = resourceOf(uav[i]) == e.hdrGame;
        hdr = hdr || isHdr;
        other[i] = !isHdr;
        if (!isHdr && i == 1) e.reflU1Game = resourceOf(uav[i]);
        bindUav[i] = static_cast<ID3D11UnorderedAccessView*>(privateView(ViewKind::Uav, uav[i], !isHdr));
        if (!bindUav[i]) return decline("a target's private copy or view could not be made");
    }
    if (!hdr) return decline("the eye's HDR is not one of the pass's targets");
    ID3D11ShaderResourceView* bindSrv[kCsSrvSlots] = {};
    for (UINT i = 0; i < kCsSrvSlots; ++i) {
        bindSrv[i] = srv[i];
        if (!srv[i] || !findMirror(resourceOf(srv[i]))) continue;
        auto* p = static_cast<ID3D11ShaderResourceView*>(privateView(ViewKind::Srv, srv[i], false));
        if (!p) return decline("an input's private view could not be made");
        bindSrv[i] = p;
    }
    if (capture) {
        // The private side of what the pass reads (the game's own where an input has no private copy).
        if (!e.stageHullStencil) stageSrvTexture(ctx, bindSrv[4], e.stageHullStencil);
        if (!e.stageHullT11) stageSrvTexture(ctx, bindSrv[11], e.stageHullT11);
    }
    if (!e.reflCleared) {
        // The private copies of the pass's other targets (the R16 u1) are made with no data, and the pass adds into them: cleared
        // before the eye's first re-issue each frame, to the game's own clear of it when one has been seen, else 0.
        e.reflCleared = true;
        VrWorldInternalScope internal;   // the layer's own clear
        for (UINT i = 0; i < kCsUavSlots; ++i) {
            if (!other[i] || !bindUav[i]) continue;
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            bindUav[i]->GetDesc(&ud);
            const bool integer = ud.Format == DXGI_FORMAT_R16_UINT || ud.Format == DXGI_FORMAT_R16_SINT || ud.Format == DXGI_FORMAT_R32_UINT ||
                                 ud.Format == DXGI_FORMAT_R32_SINT || ud.Format == DXGI_FORMAT_R8_UINT;
            if (integer) {
                const UINT zero[4] = {0u, 0u, 0u, 0u};
                ctx->ClearUnorderedAccessViewUint(bindUav[i], zero);
            } else {
                const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                ctx->ClearUnorderedAccessViewFloat(bindUav[i], g_reflU1ClearSeen ? g_reflU1Clear : zero);
            }
        }
        ++g_win.reflU1Clears;
    }
    for (UINT i = 0; i < kCsSrvSlots; ++i)
        if (srv[i] && bindSrv[i] == srv[i]) noteUnmirrored(kReflCs[step], 'C', i, srv[i], ei);
    UINT keep[kCsUavSlots];
    for (auto& k : keep) k = ~0u;
    ctx->CSSetShaderResources(0, kCsSrvSlots, bindSrv);
    ctx->CSSetUnorderedAccessViews(0, kCsUavSlots, bindUav, keep);
    g_dispatch = DispatchPending{};
    g_dispatch.active = true;
    g_dispatch.reflEye = ei;
    for (UINT i = 0; i < kCsSrvSlots; ++i) g_dispatch.srv[i] = srv[i];   // the references pass to End
    for (UINT i = 0; i < kCsUavSlots; ++i) g_dispatch.uav[i] = uav[i];
    ++g_win.reflDispatches;
    stageOpen(ctx, kTsLight, true);   // dump v2: timed with the lighting; hullLayerEndDispatch closes it
    return true;
}

// ---- Step 3: the tonemap re-issue, the cover, the composite -----------------------------------------------------------

void sayToneDeclined(const char* why) {
    ++g_win.toneDeclined;
    if (g_saidToneDecline) return;
    g_saidToneDecline = true;
    Log::get().note("hull layer: tonemap re-issue declined: %s (said once; such an eye-frame has no hull to composite, and separate mode falls back after it)",
                    why);
}

void sayDeclined(Declined why, int eye, const char* detail) {
    ++g_win.declined[why];
    if (g_saidDecline[why]) return;
    g_saidDecline[why] = true;
    Log::get().note("hull layer: composite declined for eye %d: %s (said once a reason)", eye, detail);
}

void timerBegin(GpuTimer& t, ID3D11DeviceContext* ctx, bool more = false) {
    t.open = -1;
    for (UINT i = 0; i < kTimerRing; ++i) {
        GpuTimer::Set& s = t.set[i];
        if (s.pending) continue;
        if (!s.disjoint) {
            D3D11_QUERY_DESC qd{};
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            D3D11_QUERY_DESC qt{};
            qt.Query = D3D11_QUERY_TIMESTAMP;
            if (FAILED(g_device->CreateQuery(&qd, &s.disjoint)) || FAILED(g_device->CreateQuery(&qt, &s.t0)) || FAILED(g_device->CreateQuery(&qt, &s.t1))) {
                releaseRef(s.disjoint);
                releaseRef(s.t0);
                releaseRef(s.t1);
                return;
            }
        }
        ctx->Begin(s.disjoint);
        ctx->End(s.t0);
        s.extra = more;
        t.open = static_cast<int>(i);
        return;
    }
}

void timerEnd(GpuTimer& t, ID3D11DeviceContext* ctx) {
    if (t.open < 0) return;
    GpuTimer::Set& s = t.set[t.open];
    ctx->End(s.t1);
    ctx->End(s.disjoint);
    s.pending = true;
    t.open = -1;
}

void timerCollect(GpuTimer& t, ID3D11DeviceContext* ctx) {
    for (auto& s : t.set) {
        if (!s.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 a = 0, b = 0;
        if (ctx->GetData(s.disjoint, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ctx->GetData(s.t0, &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || ctx->GetData(s.t1, &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        s.pending = false;
        if (dj.Disjoint || dj.Frequency == 0 || b < a) continue;
        t.sumMs += static_cast<double>(b - a) * 1000.0 / static_cast<double>(dj.Frequency);
        if (!s.extra) ++t.samples;
    }
}

// The window's mean ms a timed call (-1: none timed), and the window's sums back to zero.
double timerTake(GpuTimer& t) {
    const double ms = t.samples ? t.sumMs / static_cast<double>(t.samples) : -1.0;
    t.sumMs = 0.0;
    t.samples = 0;
    return ms;
}

// The cover and TAA shaders, the sampler and their constant buffers, once.
bool ensureCompGpu() {
    if (g_cgpu.tried) return g_cgpu.ok;
    g_cgpu.tried = true;
    struct Item {
        const char* src;
        size_t len;
        const char* entry;
        ID3D11ComputeShader** out;
    };
    const Item items[5] = {{kCoverHlsl, sizeof(kCoverHlsl) - 1, "hull_layer_cover_cs", &g_cgpu.coverCs},
                           {kTaaHlsl, sizeof(kTaaHlsl) - 1, "hull_layer_taa_cs", &g_cgpu.taaCs},
                           {kSharpHlsl, sizeof(kSharpHlsl) - 1, "hull_layer_sharp_cs", &g_cgpu.sharpCs},
                           {kTileMarkHlsl, sizeof(kTileMarkHlsl) - 1, "hull_layer_tile_mark_cs", &g_cgpu.tileMarkCs},
                           {kTileListHlsl, sizeof(kTileListHlsl) - 1, "hull_layer_tile_list_cs", &g_cgpu.tileListCs}};
    for (const Item& it : items) {
        ID3DBlob* code = nullptr;
        std::string why;
        if (!compileShader(it.src, it.len, it.entry, "cs_5_0", &code, &why)) {
            Log::get().note("hull layer: the %s shader did not compile (%s); separate mode keeps the own ship in the main pass this session", it.entry, why.c_str());
            if (code) code->Release();
            return false;
        }
        const HRESULT hr = g_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, it.out);
        code->Release();
        if (FAILED(hr)) {
            Log::get().note("hull layer: the %s shader compiled but could not be created (hr 0x%08lX); separate mode keeps the own ship in the main pass this "
                            "session",
                            it.entry, static_cast<unsigned long>(hr));
            return false;
        }
    }
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_cgpu.coverCb = makeConstantBuffer(32);
    g_cgpu.taaCb = makeConstantBuffer(160);
    g_cgpu.sharpCb = makeConstantBuffer(16);
    if (FAILED(g_device->CreateSamplerState(&sd, &g_cgpu.linear)) || !g_cgpu.coverCb || !g_cgpu.taaCb || !g_cgpu.sharpCb) {
        Log::get().note("hull layer: the composite's sampler or constant buffers could not be made; separate mode keeps the own ship in the main pass this session");
        return false;
    }
    Log::get().note("hull layer: the tone cover and composite shaders compiled and made (the hull layer's own TAA, head motion, current weight %.2f)",
                    static_cast<double>(kTaaCurrent));
    g_cgpu.ok = true;
    return true;
}

bool rgba8Family(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

// The direct composite's shader, once (a typed UAV load: it may compile and not be creatable on a device without them).
bool ensureDirectCs() {
    if (g_cgpu.directTried) return g_cgpu.directOk;
    g_cgpu.directTried = true;
    ID3DBlob* code = nullptr;
    std::string why;
    if (!compileShader(kSharpDirectHlsl, sizeof(kSharpDirectHlsl) - 1, "hull_layer_sharp_direct_cs", "cs_5_0", &code, &why)) {
        Log::get().note("hull layer: the hull_layer_sharp_direct_cs shader did not compile (%s); the composite copies the eye this session", why.c_str());
        if (code) code->Release();
        return false;
    }
    const HRESULT hr = g_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_cgpu.sharpDirectCs);
    code->Release();
    if (FAILED(hr)) {
        Log::get().note("hull layer: the hull_layer_sharp_direct_cs shader compiled but could not be created (hr 0x%08lX); the composite copies the eye "
                        "this session",
                        static_cast<unsigned long>(hr));
        return false;
    }
    g_cgpu.directOk = true;
    return true;
}

// The eye's typed UAV for the direct composite, or the reason there is none (kCm*, < 0 never: kCompMethods means ok). The world
// must decode as today's SRV does it (R8G8B8A8_UNORM) and be stored as c.out's UAV stores it (R8G8B8A8_UNORM), so the result is
// c.out's bit for bit; the device must load that format through a UAV (D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD). Made once a texture.
int directUavFor(Comp& c, int eye, const HullLayerCompositeInputs& in) {
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    in.outputSrv->GetDesc(&sd);
    if (c.directRes == in.output && c.directSrvFmt == sd.Format) return c.directUav ? kCompMethods : c.directFail;
    releaseRef(c.directUav);
    c.directRes = in.output;
    c.directSrvFmt = sd.Format;
    c.directFail = kCompMethods;
    D3D11_TEXTURE2D_DESC td{};
    in.output->GetDesc(&td);
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 fs{};
    fs.InFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    const UINT need = D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;
    if (!(td.BindFlags & D3D11_BIND_UNORDERED_ACCESS))
        c.directFail = kCmNoUav;
    else if (sd.Format != DXGI_FORMAT_R8G8B8A8_UNORM || sd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || td.ArraySize != 1 || td.SampleDesc.Count != 1 ||
             (td.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && td.Format != DXGI_FORMAT_R8G8B8A8_UNORM))
        c.directFail = kCmFormat;
    else if (FAILED(g_device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &fs, sizeof fs)) || (fs.OutFormatSupport2 & need) != need)
        c.directFail = kCmTypedLoad;
    else if (!ensureDirectCs())
        c.directFail = kCmShader;
    else {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        ud.Texture2D.MipSlice = sd.Texture2D.MostDetailedMip;
        if (FAILED(g_device->CreateUnorderedAccessView(in.output, &ud, &c.directUav))) {
            c.directUav = nullptr;
            c.directFail = kCmView;
        }
    }
    static const char* const kWhy[kCompMethods] = {"", "", "the eye has no UAV bind flag", "the eye or its view is not R8G8B8A8_UNORM",
                                                   "the device cannot load R8G8B8A8_UNORM through a UAV", "the eye's UAV could not be made",
                                                   "the direct shader is missing"};
    if (c.directUav)
        Log::get().note("hull layer: composite for eye %d writes the eye directly (%ux%u, format %d, typed UAV load), no whole-eye copies", eye, td.Width,
                        td.Height, static_cast<int>(td.Format));
    else
        Log::get().note("hull layer: composite for eye %d copies the eye whole twice: %s (format %d, view %d, bind 0x%X)", eye, kWhy[c.directFail],
                        static_cast<int>(td.Format), static_cast<int>(sd.Format), td.BindFlags);
    return c.directUav ? kCompMethods : c.directFail;
}

// Frame generation: the eye's world-only copy (Comp::world), made in the eye's own desc (default usage, SRV only, so CopyResource takes
// it) with the composite's view format, and the copy itself. False: no copy (S then reads the composited eye: no_world_copy).
bool keepWorldCopy(ID3D11DeviceContext* ctx, int eye, Comp& c, const HullLayerCompositeInputs& in) {
    D3D11_TEXTURE2D_DESC td{};
    in.output->GetDesc(&td);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    in.outputSrv->GetDesc(&sd);
    if (td.SampleDesc.Count != 1) return false;
    const D3D11_TEXTURE2D_DESC& w = c.worldDesc;
    if (!c.world || !c.worldSrv || w.Width != td.Width || w.Height != td.Height || w.Format != td.Format || w.MipLevels != td.MipLevels ||
        w.ArraySize != td.ArraySize || c.worldSrvFmt != sd.Format) {
        releaseRef(c.worldSrv);
        releaseRef(c.world);
        c.worldDesc = D3D11_TEXTURE2D_DESC{};
        D3D11_TEXTURE2D_DESC wd = td;
        wd.Usage = D3D11_USAGE_DEFAULT;
        wd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        wd.CPUAccessFlags = 0;
        wd.MiscFlags = 0;
        if (FAILED(g_device->CreateTexture2D(&wd, nullptr, &c.world)) || !c.world ||
            FAILED(g_device->CreateShaderResourceView(c.world, &sd, &c.worldSrv)) || !c.worldSrv) {
            releaseRef(c.worldSrv);
            releaseRef(c.world);
            return false;
        }
        c.worldDesc = td;
        c.worldSrvFmt = sd.Format;
        Log::get().note("hull layer: framegen: world-only copy made for eye %d (%ux%u, format %d, view %d, %.1f MB): frame generation's S reads the eye "
                        "from before the hull composite",
                        eye, td.Width, td.Height, static_cast<int>(td.Format), static_cast<int>(sd.Format),
                        static_cast<double>(td.Width) * td.Height * 4.0 / (1024.0 * 1024.0));
    }
    vScreenCopyResourceRaw(ctx, c.world, in.output);
    ++g_win.fgCopies;
    return true;
}

// Frame generation: the S shader and its constants, once; and whether S's UAV format can be loaded through a UAV (checked once a format).
bool ensureSCs() {
    if (g_cgpu.sTried) return g_cgpu.sOk;
    g_cgpu.sTried = true;
    ID3DBlob* code = nullptr;
    std::string why;
    if (!compileShader(kSHlsl, sizeof(kSHlsl) - 1, "hull_layer_s_cs", "cs_5_0", &code, &why)) {
        Log::get().note("hull layer: the hull_layer_s_cs shader did not compile (%s); frame generation's S gets no hull layer this session", why.c_str());
        if (code) code->Release();
        return false;
    }
    const HRESULT hr = g_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_cgpu.sCs);
    code->Release();
    g_cgpu.sCb = makeConstantBuffer(112);
    if (FAILED(hr) || !g_cgpu.sCs || !g_cgpu.sCb) {
        Log::get().note("hull layer: the hull_layer_s_cs shader or its constants could not be made (hr 0x%08lX); frame generation's S gets no hull layer "
                        "this session",
                        static_cast<unsigned long>(hr));
        return false;
    }
    g_cgpu.sOk = true;
    // The overlay over tiles: both entries or neither (the whole-eye pass above runs without them).
    const char* const entries[2] = {"hull_layer_s_tiles_cs", "hull_layer_s_tile_list_cs"};
    ID3D11ComputeShader** outs[2] = {&g_cgpu.sTilesCs, &g_cgpu.sTileListCs};
    for (int i = 0; i < 2; ++i) {
        ID3DBlob* tcode = nullptr;
        std::string twhy;
        HRESULT thr = E_FAIL;
        if (compileShader(kSHlsl, sizeof(kSHlsl) - 1, entries[i], "cs_5_0", &tcode, &twhy))
            thr = g_device->CreateComputeShader(tcode->GetBufferPointer(), tcode->GetBufferSize(), nullptr, outs[i]);
        if (tcode) tcode->Release();
        if (FAILED(thr) || !*outs[i]) {
            Log::get().note("hull layer: framegen: the %s shader could not be made (%s, hr 0x%08lX); the hull goes into S by the whole-eye pass this session",
                            entries[i], twhy.empty() ? "-" : twhy.c_str(), static_cast<unsigned long>(thr));
            releaseRef(g_cgpu.sTilesCs);
            releaseRef(g_cgpu.sTileListCs);
            break;
        }
    }
    return true;
}

// Frame generation: the S overlay's tile list for eye `c` at the composite's size (the TAA's tile grid), once; false: the whole-eye pass.
bool ensureSTiles(Comp& c) {
    if (c.sTilesTried) return c.sTilesOk;
    c.sTilesTried = true;
    if (!g_cgpu.sTilesCs || !g_cgpu.sTileListCs || !c.tileStateSrv || !c.outW || !c.outH) return false;
    const uint32_t tw = (c.outW + 7u) / 8u, th = (c.outH + 7u) / 8u;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = tw * th * 4u;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 4u;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = tw * th;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
    D3D11_BUFFER_DESC ad{};
    ad.ByteWidth = 12u;
    ad.Usage = D3D11_USAGE_DEFAULT;
    ad.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    const uint32_t args[3] = {0u, 1u, 1u};
    D3D11_SUBRESOURCE_DATA init{args, 0, 0};
    c.sTilesOk = SUCCEEDED(g_device->CreateBuffer(&bd, nullptr, &c.sTileList)) && c.sTileList &&
                 SUCCEEDED(g_device->CreateShaderResourceView(c.sTileList, nullptr, &c.sTileListSrv)) && c.sTileListSrv &&
                 SUCCEEDED(g_device->CreateUnorderedAccessView(c.sTileList, &ud, &c.sTileListUav)) && c.sTileListUav &&
                 SUCCEEDED(g_device->CreateBuffer(&ad, &init, &c.sTileArgs)) && c.sTileArgs;
    if (!c.sTilesOk) {
        releaseRef(c.sTileListSrv);
        releaseRef(c.sTileListUav);
        releaseRef(c.sTileList);
        releaseRef(c.sTileArgs);
        Log::get().note("hull layer: framegen: the S overlay's tile list could not be made (%ux%u tiles); the hull goes into S by the whole-eye pass", tw, th);
    }
    return c.sTilesOk;
}
bool sFormatLoads(ID3D11UnorderedAccessView* s) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    s->GetDesc(&ud);
    if (ud.ViewDimension != D3D11_UAV_DIMENSION_TEXTURE2D) return false;
    if (ud.Format == g_cgpu.sFmt) return g_cgpu.sFmtOk;
    g_cgpu.sFmt = ud.Format;
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 fs{};
    fs.InFormat = ud.Format;
    const UINT need = D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE;
    g_cgpu.sFmtOk = SUCCEEDED(g_device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &fs, sizeof fs)) && (fs.OutFormatSupport2 & need) == need;
    if (!g_cgpu.sFmtOk)
        Log::get().note("hull layer: framegen: S's format %d cannot be loaded through a UAV on this device; S gets no hull layer while it is", static_cast<int>(ud.Format));
    return g_cgpu.sFmtOk;
}

// A 2D texture with a default SRV and, when `uav`, a default UAV (both in the texture's own format).
bool makeTexture(uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool uav, ID3D11Texture2D** tex, ID3D11ShaderResourceView** srv, ID3D11UnorderedAccessView** uavOut) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = (srv ? D3D11_BIND_SHADER_RESOURCE : 0u) | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
    if (FAILED(g_device->CreateTexture2D(&td, nullptr, tex)) || !*tex) return false;
    if (srv && (FAILED(g_device->CreateShaderResourceView(*tex, nullptr, srv)) || !*srv)) return false;
    if (uav && uavOut && (FAILED(g_device->CreateUnorderedAccessView(*tex, nullptr, uavOut)) || !*uavOut)) return false;
    return true;
}

// The eye's tone target T (the game's tonemap output's size, its view format for the RTV), cover C and depth Z.
bool ensureTone(int ei, uint32_t w, uint32_t h, DXGI_FORMAT rtvFmt) {
    Eye& e = g_eyes[ei];
    if (e.tone && e.toneW == w && e.toneH == h && e.toneRtvFmt == rtvFmt) return true;
    releaseTone(e);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = rtvFmt;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    const bool ok = SUCCEEDED(g_device->CreateTexture2D(&td, nullptr, &e.tone)) && e.tone &&
                    SUCCEEDED(g_device->CreateRenderTargetView(e.tone, &rd, &e.toneRtv)) && e.toneRtv &&
                    SUCCEEDED(g_device->CreateShaderResourceView(e.tone, &sd, &e.toneSrv)) && e.toneSrv &&
                    makeTexture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, true, &e.cover, &e.coverSrv, &e.coverUav) &&
                    makeTexture(w, h, DXGI_FORMAT_R32_FLOAT, true, &e.coverZ, &e.coverZSrv, &e.coverZUav);
    if (!ok) {
        releaseTone(e);
        return false;
    }
    e.toneW = w;
    e.toneH = h;
    e.toneRtvFmt = rtvFmt;
    Log::get().note("hull layer: tone targets made for eye %d: %ux%u, the tonemap's view format %u (T R8G8B8A8, cover R8G8B8A8_UNORM, depth R32_FLOAT)", ei, w,
                    h, static_cast<unsigned>(rtvFmt));
    return true;
}

// A shader view over a depth texture (its depth plane), kept while the texture is the same. False when the texture refuses one.
bool depthView(ID3D11Resource* res, ID3D11ShaderResourceView*& srv, const void*& of) {
    if (!res) return false;
    if (of == res && srv) return true;
    releaseRef(srv);
    of = nullptr;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    D3D11_TEXTURE2D_DESC d{};
    static_cast<ID3D11Texture2D*>(res)->GetDesc(&d);
    if (!(d.BindFlags & D3D11_BIND_SHADER_RESOURCE) || d.SampleDesc.Count != 1) return false;
    DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
    switch (d.Format) {
    case DXGI_FORMAT_R32G8X24_TYPELESS: f = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
    case DXGI_FORMAT_R32_TYPELESS: f = DXGI_FORMAT_R32_FLOAT; break;
    case DXGI_FORMAT_R24G8_TYPELESS: f = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
    case DXGI_FORMAT_R16_TYPELESS: f = DXGI_FORMAT_R16_UNORM; break;
    default: return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = f;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(g_device->CreateShaderResourceView(res, &sd, &srv)) || !srv) {
        srv = nullptr;
        return false;
    }
    of = res;
    return true;
}

// A zeroed texture shaped like the game's view `game` (its texture's desc, the same view), for the tonemap re-issue's bloom.
// Null with the reason when the view is not a single 2D texture or the copy cannot be made and cleared.
ID3D11ShaderResourceView* zeroedBloom(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* game, const char** why) {
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    game->GetDesc(&vd);
    if (vd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) {
        *why = "the bloom view is not a 2D texture view";
        return nullptr;
    }
    ID3D11Resource* res = nullptr;
    game->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (res && FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) tex = nullptr;
    releaseRef(res);
    if (!tex) {
        *why = "the bloom is not a 2D texture";
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    releaseRef(tex);
    ZeroBloom& z = g_zeroBloom;
    if (z.srv && std::memcmp(&z.desc, &d, sizeof d) == 0 && std::memcmp(&z.view, &vd, sizeof vd) == 0) return z.srv;
    releaseRef(z.srv);
    releaseRef(z.tex);
    if (d.ArraySize != 1 || d.SampleDesc.Count != 1) {
        *why = "the bloom is an array or multisampled";
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC pd = d;
    pd.Usage = D3D11_USAGE_DEFAULT;
    pd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;   // render target only for the one clear
    pd.CPUAccessFlags = 0;
    pd.MiscFlags = 0;
    if (FAILED(g_device->CreateTexture2D(&pd, nullptr, &z.tex)) || !z.tex || FAILED(g_device->CreateShaderResourceView(z.tex, &vd, &z.srv)) ||
        !z.srv) {
        releaseRef(z.srv);
        releaseRef(z.tex);
        *why = "the zeroed texture or its view could not be made";
        return nullptr;
    }
    static const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (UINT m = 0; m < d.MipLevels; ++m) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = vd.Format;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rd.Texture2D.MipSlice = m;
        ID3D11RenderTargetView* rtv = nullptr;
        if (FAILED(g_device->CreateRenderTargetView(z.tex, &rd, &rtv)) || !rtv) {
            releaseRef(z.srv);
            releaseRef(z.tex);
            *why = "the zeroed texture could not be cleared (no render target view)";
            return nullptr;
        }
        vScreenClearRenderTargetViewRaw(ctx, rtv, zero);
        releaseRef(rtv);
    }
    z.desc = d;
    z.view = vd;
    Log::get().note("hull layer: the tonemap re-issue's zeroed bloom made: %ux%u format %u mips=%u (view format %u)", d.Width, d.Height,
                    static_cast<unsigned>(d.Format), d.MipLevels, static_cast<unsigned>(vd.Format));
    return z.srv;
}

// Right after the game's own issue of a 3-vertex draw: is it an eye's tonemap (the structure, and a PS slot 0-3 over the
// HDR an eye's lighting just lit)? True: T is cleared and bound as RT0, the private HDR at that slot; the caller re-issues
// the draw and hullLayerEnd puts the game's bindings back and runs the cover pass.
bool beginTone(ID3D11DeviceContext* ctx) {
    const uint64_t vs = bindingShaderHash(BindSlot::Vs);
    const uint64_t ps = bindingShaderHash(BindSlot::Ps);
    ToneAdmit ta;
    if (!tonemapAdmitStructure(ctx, ps, ta) || !tonemapAdmitFull(ta, vs)) return false;
    int ei = -1;
    UINT slot = 0;
    ID3D11ShaderResourceView* hdrView = nullptr;
    for (UINT s = 0; s < 4 && ei < 0; ++s) {
        ID3D11ShaderResourceView* v = nullptr;
        ctx->PSGetShaderResources(s, 1, &v);
        if (!v) continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        v->GetDesc(&vd);
        const void* res = vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D ? resourceOf(v) : nullptr;
        for (int i = 0; i < 2 && res; ++i)
            if (g_eyes[i].hdrGame == res && g_eyes[i].gb == Gb::Done) {
                ei = i;
                slot = s;
                break;
            }
        if (ei >= 0)
            hdrView = v;   // the reference is kept until the private view is found
        else
            v->Release();
    }
    if (ei < 0) return false;
    Eye& e = g_eyes[ei];
    const auto decline = [&](const char* why) {
        if (why) sayToneDeclined(why);
        releaseRef(hdrView);
        return false;
    };
    if (e.toneFrame == g_frame) return decline(nullptr);   // once an eye a frame: the first tonemap of its HDR
    if (e.lightDraws == 0) return decline("the eye's private HDR was not lit this frame");
    ID3D11Texture2D* out = nullptr;
    D3D11_TEXTURE2D_DESC od{};
    if (ta.rtvRes && SUCCEEDED(static_cast<ID3D11Resource*>(ta.rtvRes)->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&out))) && out) {
        out->GetDesc(&od);
        out->Release();
    }
    if (!out || !rgba8Family(od.Format) || !rgba8Family(ta.rtvFmt)) return decline("the tonemap's output is not an R8G8B8A8 texture");
    if (!ensureCompGpu()) return decline("the composite's shaders are missing");
    auto* privHdr = static_cast<ID3D11ShaderResourceView*>(privateView(ViewKind::Srv, hdrView, false));
    if (!privHdr) return decline("the private HDR has no shader view");
    if (!ensureTone(ei, od.Width, od.Height, ta.rtvFmt)) return decline("a private tone target could not be made");
    releaseRef(hdrView);
    timerBegin(g_toneTimer, ctx);
    stageOpen(ctx, kTsTone, false);   // dump v2: the re-issue and the cover pass (hullLayerEnd closes it after the cover)
    g_pending = Pending{};
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, g_pending.rtv, &g_pending.dsv);
    ctx->PSGetShaderResources(0, kSrvSlots, g_pending.srv);
    ID3D11ShaderResourceView* bindSrv[kSrvSlots];
    for (UINT i = 0; i < kSrvSlots; ++i) bindSrv[i] = g_pending.srv[i];
    bindSrv[slot] = privHdr;
    // The game's bloom (t2) is the bloom of its own image: in separate mode the world behind the hull, which this tone would add
    // onto the hull (the 2026-11-11 unmirrored-input log). The re-issue reads zero there instead. The hull loses the bloom that
    // bright things spill over it (a known, small loss), and a reference frame (main pass keeps the hull) now differs from the
    // private tone there by that bloom.
    {
        const char* why = nullptr;
        bool zeroed = false;
        if (ps != kHullTonePs) {
            why = "the tonemap is not PS D0A16B9E (its bloom slot is not known)";
        } else if (slot == kToneBloomSlot) {
            why = "the HDR is at the bloom's slot";
        } else if (!bindSrv[kToneBloomSlot]) {
            why = "nothing is bound at the bloom's slot";
        } else if (ID3D11ShaderResourceView* z = zeroedBloom(ctx, bindSrv[kToneBloomSlot], &why)) {
            bindSrv[kToneBloomSlot] = z;
            zeroed = true;
        }
        if (zeroed) {
            ++g_win.bloomZeroed;
        } else {
            ++g_win.bloomDeclined;
            g_win.bloomWhy = why;
        }
    }
    for (UINT i = 0; i < kSrvSlots; ++i)   // the tonemap swaps the HDR and zeroes the bloom; the rest stay the game's
        if (i != slot && bindSrv[i] && bindSrv[i] == g_pending.srv[i]) noteUnmirrored(ps, 'P', i, bindSrv[i], ei);
    static const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    vScreenClearRenderTargetViewRaw(ctx, e.toneRtv, clear);
    vScreenPSSetShaderResourcesRaw(ctx, 0, kSrvSlots, bindSrv);
    vScreenSetRenderTargetsRaw(ctx, 1, &e.toneRtv, nullptr);
    g_pending.active = true;
    g_pending.eye = ei;
    g_pending.stage = kToneStage;
    g_pending.srvSwapped = true;
    g_pending.toneOut = ta.rtvRes;
    return true;
}

// After the tonemap re-issue, the game's bindings back: the cover pass into C and Z. The eye-frame is toned when it ran.
void coverPass(ID3D11DeviceContext* ctx, int ei, const void* toneOut) {
    Eye& e = g_eyes[ei];
    ID3D11Resource* privZ = nullptr;
    if (e.privDsv) e.privDsv->GetResource(&privZ);
    const bool views = privZ && e.mainDepth && depthView(privZ, e.privZSrv, e.privZOf) && depthView(e.mainDepth, e.mainZSrv, e.mainZOf);
    releaseRef(privZ);
    if (!views) {
        sayToneDeclined("the private or the game's eye depth refuses a shader view");
        return;
    }
    D3D11_TEXTURE2D_DESC md{};
    e.mainDepth->GetDesc(&md);
    if (md.Width != e.toneW || md.Height != e.toneH) {
        sayToneDeclined("the eye depth and the tonemap's output differ in size");
        return;
    }
    // Build A': the rectangle the eye's re-issues were scissored to this frame (the whole eye when none), and the box made
    // fresh for the readback (the buffer and its ring are made at the first cover pass of these tone targets).
    const bool rectOn = e.rectOn && e.rectFrame == g_frame;
    uint32_t cb[8] = {0u, e.toneW, e.toneH, 0u, 0u, 0u, e.toneW, e.toneH};
    if (rectOn) {
        cb[4] = static_cast<uint32_t>(e.rect.left);
        cb[5] = static_cast<uint32_t>(e.rect.top);
        cb[6] = static_cast<uint32_t>(e.rect.right);
        cb[7] = static_cast<uint32_t>(e.rect.bottom);
    }
    std::memcpy(&cb[0], &e.clearDepth, sizeof e.clearDepth);
    vScreenUpdateSubresourceRaw(ctx, g_cgpu.coverCb, 0, nullptr, cb, 0, 0);
    if (!e.box && !makeRawBuffer(kBoxBytes, true, &e.box, nullptr, &e.boxUav)) {
        releaseRef(e.boxUav);
        releaseRef(e.box);
    }
    if (e.boxUav) {
        static const UINT zero[4] = {0u, 0u, 0u, 0u};
        ctx->ClearUnorderedAccessViewUint(e.boxUav, zero);
    }
    {
        CsStageSave save;
        save.save(ctx);
        ID3D11ShaderResourceView* srvs[3] = {e.toneSrv, e.privZSrv, e.mainZSrv};
        ID3D11UnorderedAccessView* uavs[CsStageSave::kUavs] = {e.coverUav, e.coverZUav, e.boxUav};
        ctx->CSSetUnorderedAccessViews(0, CsStageSave::kUavs, uavs, nullptr);
        ctx->CSSetShader(g_cgpu.coverCs, nullptr, 0);
        ctx->CSSetShaderResources(0, 3, srvs);
        ctx->CSSetConstantBuffers(0, 1, &g_cgpu.coverCb);
        ctx->Dispatch((e.toneW + 7u) / 8u, (e.toneH + 7u) / 8u, 1);
        ID3D11UnorderedAccessView* none[CsStageSave::kUavs] = {};
        ctx->CSSetUnorderedAccessViews(0, CsStageSave::kUavs, none, nullptr);   // C and Z off before the game's views come back
        save.restore(ctx);
    }
    // The box into a free staging buffer of the ring (none free: this frame's box is not read; the readback says nothing).
    if (e.box) {
        Readback* slot = nullptr;
        for (auto& r : e.boxRing)
            if (!r.pending) {
                slot = &r;
                break;
            }
        if (slot && !slot->staging) slot->staging = makeStagingBuffer(kBoxBytes);
        if (slot && slot->staging) {
            vScreenCopyResourceRaw(ctx, slot->staging, e.box);
            slot->frame = g_frame;
            slot->pending = true;
        }
    }
    e.toneFrame = g_frame;
    e.toneOut = toneOut;
    ++g_win.toned;
}

// The TAA's history pair and the composited eye at the output size, for VR eye `eye`.
bool ensureCompTargets(int eye, uint32_t w, uint32_t h) {
    Comp& c = g_comp[eye];
    if (c.out && c.outW == w && c.outH == h) return true;
    releaseComp(c);
    bool ok = true;
    for (int i = 0; i < 2 && ok; ++i) ok = makeTexture(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, &c.hist[i], &c.histSrv[i], &c.histUav[i]);
    ok = ok && makeTexture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, true, &c.out, nullptr, &c.outUav);
    // The TAA's tiles: two R32_UINT tile maps, an append list of every tile, and the indirect arguments {count, 1, 1}.
    const uint32_t tw = (w + 7u) / 8u, th = (h + 7u) / 8u;
    ok = ok && makeTexture(tw, th, DXGI_FORMAT_R32_UINT, true, &c.tileMark, nullptr, &c.tileMarkUav);
    ok = ok && makeTexture(tw, th, DXGI_FORMAT_R32_UINT, true, &c.tileState, &c.tileStateSrv, &c.tileStateUav);   // the SRV: the S overlay's tiles
    if (ok) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = tw * th * 4u;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4u;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = tw * th;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
        D3D11_BUFFER_DESC ad{};
        ad.ByteWidth = 12u;
        ad.Usage = D3D11_USAGE_DEFAULT;
        ad.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        const uint32_t args[3] = {0u, 1u, 1u};
        D3D11_SUBRESOURCE_DATA init{args, 0, 0};
        ok = SUCCEEDED(g_device->CreateBuffer(&bd, nullptr, &c.tileList)) && c.tileList &&
             SUCCEEDED(g_device->CreateShaderResourceView(c.tileList, nullptr, &c.tileListSrv)) && c.tileListSrv &&
             SUCCEEDED(g_device->CreateUnorderedAccessView(c.tileList, &ud, &c.tileListUav)) && c.tileListUav &&
             SUCCEEDED(g_device->CreateBuffer(&ad, &init, &c.tileArgs)) && c.tileArgs;
    }
    if (!ok) {
        releaseComp(c);
        return false;
    }
    c.outW = w;
    c.outH = h;
    Log::get().note("hull layer: composite targets made for eye %d: %ux%u (history R16G16B16A16_FLOAT x2, out R8G8B8A8_UNORM, %ux%u TAA tiles)", eye,
                    w, h, tw, th);
    return true;
}

// The frame that just ended (g_frame not yet advanced), separate mode: an eye that seeded and drew the own ship must have
// been lit, tonemapped and composited (or made ready, on a fallback frame). A miss holds the discard off.
void judgeFrame() {
    bool miss = false;
    for (auto& e : g_eyes) {
        if (!e.seen || !e.shell || e.gb == Gb::Before) continue;   // no own ship drawn into this eye: nothing to put back
        if (e.gb == Gb::Off) {
            ++g_win.failSeed;   // no mask: nothing was discarded on this eye-frame (Step 2's fallback), so no hold
            continue;
        }
        if (e.lightDraws == 0) {
            ++g_win.failLit;
            miss = true;
        } else if (e.toneFrame != g_frame) {
            ++g_win.failTone;
            miss = true;
        } else if (e.compFrame != g_frame) {
            ++g_win.failComposite;
            miss = true;
        } else if (e.compApplied) {
            ++g_win.compositedEyes;
        } else {
            ++g_win.readyEyes;
        }
    }
    if (miss)
        g_hold = kHoldFrames;
    else if (g_hold)
        --g_hold;
}

bool frameGenOn() { return temporalPassFrameGenMode() != 0 || frameGenShadowOn(); }

// Station fixes: the busy governor, a temporary safety (kBusyDraws), decided at the frame boundary from the frame that just ended,
// before the next frame's draws. Awake: an eye that drew more hull-family G-buffer draws than the limit (kBusyDraws when a re-issue
// needed the neighbour filter, else kBusyDrawsCulled) puts the layer to sleep: no seed, discard, re-issue, stage or composite, and
// the game draws the ship. Asleep: kCalmFrames frames in a row at or under the limit it slept at wake it; separate mode's hold
// (kHoldFrames, re-armed by decideNext while asleep) then runs as after any fallback.
void governBusy() {
    // A dump holds the layer awake from its arming to its capture frame and, in separate mode, to the TAA sequence's last staged
    // frame: asleep, it would capture nothing. A dump that woke it puts it back to sleep after, at the limit it slept at.
    const bool dumpAwake = g_dump != Dump::Idle || (g_seq.active && !g_seq.end && g_seq.staged < kSeqFrames);
    if (dumpAwake) {
        if (g_busy) {
            g_busy = false;
            g_dumpWoke = true;
            g_calm = 0;
            Log::get().note("hull layer: busy: woken for dump %ls (one warm-up frame, then the capture frame%s; the layer sleeps again after)",
                            g_dumpStamp, detail::g_hullLayerSeparate ? " and the TAA sequence, its composites made and not applied while the hold runs" : "");
        }
        return;
    }
    if (g_dumpWoke) {
        g_dumpWoke = false;
        g_busy = true;
        g_calm = 0;
        ++g_win.busyFrames;
        Log::get().note("hull layer: busy: dump %ls done; the layer sleeps again (limit %u, until %u calm frames)", g_dumpStamp, g_busyLimit, kCalmFrames);
        return;
    }
    uint32_t busiest = 0;
    int eye = -1;
    bool filtered = false;
    for (int i = 0; i < 2; ++i) {
        if (g_eyes[i].gbufHull > busiest) {
            busiest = g_eyes[i].gbufHull;
            eye = i;
        }
        filtered = filtered || g_eyes[i].filterDraws > 0;
    }
    if (!g_busy) {
        const uint32_t limit = filtered ? kBusyDraws : kBusyDrawsCulled;
        if (busiest > limit) {
            g_busy = true;
            g_calm = 0;
            g_busyLimit = limit;
            Log::get().note("hull layer: busy: eye %d drew %u hull-family G-buffer draws, over %u (%s); the layer sleeps (no seed, discard, re-issue, "
                            "stages or composite; the game draws the own ship) until %u calm frames",
                            eye, busiest, limit, filtered ? "a re-issue needed the neighbour filter" : "every re-issue culled in the vertex shader",
                            kCalmFrames);
        }
    } else {
        g_calm = busiest <= g_busyLimit ? g_calm + 1 : 0;
        if (g_calm >= kCalmFrames) {
            g_busy = false;
            Log::get().note("hull layer: busy over: %u calm frames at or under %u hull-family G-buffer draws an eye (the last frame's busiest: %u); the "
                            "layer wakes%s",
                            g_calm, g_busyLimit, busiest, detail::g_hullLayerSeparate ? " (separate mode holds the discard off its usual hold first)" : "");
            g_calm = 0;
        }
    }
    if (g_busy) ++g_win.busyFrames;
}

// The next frame's decision, made before any of its draws. Returns the fallback reason (kFallbacks: the discard is on).
Fallback decideNext() {
    g_discard = false;
    if (g_busy) {
        g_hold = kHoldFrames;   // asleep: no discard, and the hold runs from the wake (counted as busy=, not as a fallback)
        return kFbStreak;
    }
    Fallback why = kFallbacks;
    // Frame generation no longer stands the mode aside (2026-11-11, docs "Frame generation integration"): S is built from the world-only
    // eye and the hull layer is drawn into it (hullLayerSInput, hullLayerCompositeS). kFbFrameGen stays in the composite line, always 0.
    if (!runtimeVrProfile() || nativeOnFootActive()) {
        why = kFbOutside;
    } else if (!g_haveOwn) {
        why = kFbMaskPending;
    } else if (g_lastOwn > kOwnMaskMax) {
        why = kFbMaskLarge;
        g_win.ownMax = std::max(g_win.ownMax, g_lastOwn);
        if (!g_saidMaskLarge) {
            g_saidMaskLarge = true;
            Log::get().note("hull layer: separate mode falls back: the own mask holds %u records (over %u: docking, or a station or ship within %.0f m); the own "
                            "ship stays in the main pass while it does (said once a window)",
                            g_lastOwn, kOwnMaskMax, static_cast<double>(kOwnRadius));
        }
    } else if (g_hold) {
        why = kFbStreak;
    } else {
        g_discard = true;
    }
    if (g_discard)
        ++g_win.separateFrames;
    else
        ++g_win.fallback[why];
    // A fallback other than the hold itself (mask pending or large, outside) re-arms the hold, so
    // separate resumes only after kHoldFrames clean frames, never on the first frame the reason clears.
    if (!g_discard && why != kFbStreak) g_hold = kHoldFrames;
    return g_discard ? kFallbacks : why;
}

// ---- the readbacks, the dump ------------------------------------------------------------------------------------------

void readBacks(ID3D11DeviceContext* ctx) {
    for (auto& e : g_eyes) {
        for (auto& r : e.ring) {
            if (!r.pending || !r.staging || g_frame - r.frame < kReadbackLag) continue;
            D3D11_MAPPED_SUBRESOURCE map{};
            const HRESULT hr = ctx->Map(r.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
            r.pending = false;
            if (FAILED(hr)) {
                if (!g_saidReadback) {
                    g_saidReadback = true;
                    Log::get().note("hull layer: a stats readback could not be mapped (hr 0x%08lX; said once)", static_cast<unsigned long>(hr));
                }
                continue;
            }
            uint32_t v[kStatsBytes / 4] = {};
            std::memcpy(v, map.pData, sizeof v);
            ctx->Unmap(r.staging, 0);
            if (v[6]) {   // a rig seed: its radius and reach
                float radius = 0.0f, reach = 0.0f;
                std::memcpy(&radius, &v[4], sizeof radius);
                std::memcpy(&reach, &v[5], sizeof reach);
                g_win.rigRadius.push_back(static_cast<uint32_t>(radius + 0.5f));
                g_win.rigReach.push_back(static_cast<uint32_t>(reach + 0.5f));
            }
            g_win.own.push_back(v[0]);
            g_win.filtered.push_back(v[1]);
            ++g_win.readbacks;
            g_lastOwn = v[0];
            g_haveOwn = true;
        }
        // Build A': the cover's boxes, the newest one kept (render px; an empty box keeps x1 = 0).
        for (auto& r : e.boxRing) {
            if (!r.pending || !r.staging || g_frame - r.frame < kReadbackLag) continue;
            D3D11_MAPPED_SUBRESOURCE map{};
            const HRESULT hr = ctx->Map(r.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
            r.pending = false;
            if (FAILED(hr)) continue;
            uint32_t v[kBoxBytes / 4] = {};
            std::memcpy(v, map.pData, sizeof v);
            ctx->Unmap(r.staging, 0);
            ++g_win.boxesRead;
            if (e.haveBox && r.frame < e.boxFrame) continue;   // an older box than the one held
            const bool empty = v[2] == 0u || v[3] == 0u;
            if (empty) ++g_win.boxesEmpty;
            e.boxRect[0] = empty ? 0u : 65535u - v[0];
            e.boxRect[1] = empty ? 0u : 65535u - v[1];
            e.boxRect[2] = empty ? 0u : v[2];
            e.boxRect[3] = empty ? 0u : v[3];
            e.boxFrame = r.frame;
            e.haveBox = true;
        }
    }
}

// Build A': this frame's rectangle for eye `ei`, decided at its first re-issue: the newest box read back, drawn on or after
// the probe epoch, grown by kRectPerFrame a frame of its age (at least kRectMarginMin), clamped to the eye. False: the whole eye.
bool eyeRect(int ei, D3D11_RECT* out) {
    Eye& e = g_eyes[ei];
    if (e.rectFrame != g_frame) {
        e.rectFrame = g_frame;
        e.rectOn = false;
        ++g_win.rectDecided;
        const uint32_t w = e.w, h = e.h;
        if (g_frame - e.rectEpoch >= kRectProbeEvery) {
            e.rectEpoch = g_frame;   // the probe: this frame whole, then a box of it or later
            ++g_win.wholeProbe;
        } else if (!e.haveBox || e.boxFrame < e.rectEpoch || !w || !h || e.toneW != w || e.toneH != h) {
            ++g_win.wholeNoBox;
        } else if (g_frame - e.boxFrame > kRectMaxAge) {
            ++g_win.wholeStale;
        } else if (e.boxRect[2] == 0u) {
            ++g_win.wholeEmpty;
        } else {
            const uint32_t m = (std::max)(kRectMarginMin, kRectPerFrame * (g_frame - e.boxFrame));
            const uint32_t x0 = e.boxRect[0] > m ? e.boxRect[0] - m : 0u;
            const uint32_t y0 = e.boxRect[1] > m ? e.boxRect[1] - m : 0u;
            const uint32_t x1 = (std::min)(w, e.boxRect[2] + m);
            const uint32_t y1 = (std::min)(h, e.boxRect[3] + m);
            if (x0 < x1 && y0 < y1 && g_benchNoRect) {
                benchNoteOff();   // the variant bench's sep_norect: it would have been scissored; the whole eye (not counted as rect)
            } else if (x0 < x1 && y0 < y1) {
                e.rect.left = static_cast<LONG>(x0);
                e.rect.top = static_cast<LONG>(y0);
                e.rect.right = static_cast<LONG>(x1);
                e.rect.bottom = static_cast<LONG>(y1);
                e.rectOn = true;
                ++g_win.rectFrames;
                g_win.rectShare += static_cast<double>(x1 - x0) * (y1 - y0) / (static_cast<double>(w) * h);
            } else {
                ++g_win.wholeEmpty;
            }
        }
    }
    if (e.rectOn) *out = e.rect;
    return e.rectOn;
}

// Build A': the game's rasterizer state's clone with ScissorEnable, made once and kept (null: none could be made).
ID3D11RasterizerState* scissorClone(ID3D11RasterizerState* game) {
    for (const auto& s : g_scissorStates)
        if (s.game == game) return s.clone;
    if (g_scissorStates.size() >= kMaxScissorStates) releaseScissorStates();   // a churn of the game's states: start over
    D3D11_RASTERIZER_DESC d{};
    if (game) {
        game->GetDesc(&d);
    } else {
        d.FillMode = D3D11_FILL_SOLID;   // the default state's desc
        d.CullMode = D3D11_CULL_BACK;
        d.DepthClipEnable = TRUE;
    }
    d.ScissorEnable = TRUE;
    ScissorState s;
    if (FAILED(g_device->CreateRasterizerState(&d, &s.clone))) s.clone = nullptr;
    s.game = game;
    if (game) game->AddRef();
    g_scissorStates.push_back(s);
    return s.clone;
}

// Build A': right after a re-issue was set up (g_pending), before it is issued: scissor it to its eye's rectangle, the
// game's own scissor intersected when its state has one on. The depth copy stays whole: the AO's compute steps read the
// private depth copy over the whole eye, and a scissored copy would leave last frames' depths outside the rectangle.
void scissorPending(ID3D11DeviceContext* ctx) {
    Pending& p = g_pending;
    if (!p.active || p.eye < 0 || p.stage == kCopy || !g_device) return;
    D3D11_RECT r{};
    if (!eyeRect(p.eye, &r)) return;
    ID3D11RasterizerState* game = nullptr;
    ctx->RSGetState(&game);
    ID3D11RasterizerState* clone = scissorClone(game);
    if (!clone) {
        ++g_win.scissorNoState;
        releaseRef(game);
        return;
    }
    D3D11_RASTERIZER_DESC gd{};
    if (game) game->GetDesc(&gd);
    p.scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetScissorRects(&p.scissorCount, p.scissors);
    D3D11_RECT set[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT n = 1;
    set[0] = r;
    if (game && gd.ScissorEnable && p.scissorCount) {
        // The game's scissor applies already: each of its rects intersected with the eye's (an empty one stays empty).
        n = p.scissorCount;
        for (UINT i = 0; i < n; ++i) {
            const D3D11_RECT& g = p.scissors[i];
            set[i].left = (std::max)(g.left, r.left);
            set[i].top = (std::max)(g.top, r.top);
            set[i].right = (std::max)(set[i].left, (std::min)(g.right, r.right));
            set[i].bottom = (std::max)(set[i].top, (std::min)(g.bottom, r.bottom));
        }
        ++g_win.scissorGame;
    }
    p.raster = game;   // the reference passes to hullLayerEnd
    p.scissored = true;
    ctx->RSSetState(clone);
    ctx->RSSetScissorRects(n, set);
    ++g_win.scissorDraws;
}

// Build A': the game's rasterizer state and scissor rects back after the re-issue.
void unscissorPending(ID3D11DeviceContext* ctx) {
    Pending& p = g_pending;
    if (!p.scissored) return;
    if (ctx) {
        ctx->RSSetState(p.raster);
        ctx->RSSetScissorRects(p.scissorCount, p.scissorCount ? p.scissors : nullptr);
    }
    releaseRef(p.raster);
    p.scissored = false;
}

const char* writeTexture(ID3D11DeviceContext* ctx, ID3D11Texture2D* staging, const std::wstring& dir, const wchar_t* name, int eye) {
    if (!staging) return "missing";
    D3D11_TEXTURE2D_DESC d{};
    staging->GetDesc(&d);
    uint32_t bytes = 0;
    switch (d.Format) {
    case DXGI_FORMAT_R11G11B10_FLOAT: bytes = 4; break;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: bytes = 8; break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: bytes = 8; break;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT: bytes = 4; break;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: bytes = 4; break;
    // The G-buffer's RT0 (GameGbuf0, separate mode): f23 in the census, or another 32-bit colour format.
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT: bytes = 4; break;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: bytes = 4; break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: bytes = 4; break;
    // The reflection dump's stencil, should the pass read a one-byte copy of it.
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: bytes = 1; break;
    default: return "format unsupported";
    }
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map))) return "map failed";
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\eye_%s_%s%s.bin", dir.c_str(), g_dumpStamp, name, eye ? L"_e1" : L"");
    FILE* f = nullptr;
    bool ok = false;
    if (_wfopen_s(&f, path, L"wb") == 0 && f) {
        // EDVRTEX1: the magic, then version, width, height, DXGI format, row bytes, frame, eye, UI-bound, UI-flags (tools/eye_inputs.py).
        const uint32_t header[9] = {1u, d.Width, d.Height, static_cast<uint32_t>(d.Format), d.Width * bytes, g_dumpFrame, static_cast<uint32_t>(eye), 0u, 0u};
        ok = std::fwrite("EDVRTEX1", 1, 8, f) == 8 && std::fwrite(header, sizeof(header), 1, f) == 1;
        for (uint32_t y = 0; y < d.Height && ok; ++y)
            ok = std::fwrite(static_cast<const char*>(map.pData) + static_cast<size_t>(y) * map.RowPitch, 1, d.Width * bytes, f) == d.Width * bytes;
        ok = (std::fclose(f) == 0) && ok;
    }
    ctx->Unmap(staging, 0);
    return ok ? "written" : "write failed";
}

// Dump v2: eye_<stamp>_HullLights.json, the lighting windows' draws of the dump frame (recordLight), and their summary line.
void writeLights(const std::wstring& dir) {
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\eye_%s_HullLights.json", dir.c_str(), g_dumpStamp);
    uint32_t reissued = 0, unknown = 0, stencil = 0, known = 0;
    FILE* f = nullptr;
    bool ok = false;
    if (_wfopen_s(&f, path, L"wb") == 0 && f) {
        ok = std::fprintf(f, "{\"version\": 1, \"stamp\": \"%ls\", \"frame\": %u, \"reference_frame\": %s, \"capped\": %s, \"cap\": %u,\n \"draws\": [",
                          g_dumpStamp, g_dumpFrame, g_refFrame ? "true" : "false", g_lightsCapped ? "true" : "false",
                          static_cast<unsigned>(kMaxLightRecs)) > 0;
        for (size_t i = 0; i < g_lights.size() && ok; ++i) {
            const LightRec& r = g_lights[i];
            const bool unk = r.hdr && !r.known && !r.stagePair;
            reissued += r.reissued ? 1u : 0u;
            unknown += unk ? 1u : 0u;
            stencil += r.stencilOnly ? 1u : 0u;
            known += r.known ? 1u : 0u;
            ok = std::fprintf(f,
                              "%s\n  {\"eye\": %d, \"seq\": %u, \"vs\": \"%016llX\", \"ps\": \"%016llX\", \"kind\": \"%c\", \"count\": %u, \"instances\": %u, "
                              "\"start_instance\": %u, \"writes_hdr\": %s, \"stencil_only\": %s, \"stencil_ref\": %u, \"known_light\": %s, \"stage_pair\": %s, "
                              "\"reissued\": %s, \"unknown\": %s}",
                              i ? "," : "", r.eye, r.seq, static_cast<unsigned long long>(r.vs), static_cast<unsigned long long>(r.ps),
                              r.kind ? r.kind : '?', r.count, r.instances, r.startInstance, r.hdr ? "true" : "false", r.stencilOnly ? "true" : "false",
                              static_cast<unsigned>(r.stencilRef), r.known ? "true" : "false", r.stagePair ? "true" : "false",
                              r.reissued ? "true" : "false", unk ? "true" : "false") > 0;
        }
        ok = std::fprintf(f, "\n ]\n}\n") > 0 && ok;
        ok = (std::fclose(f) == 0) && ok;
    }
    Log::get().note("hull layer: dump %ls: HullLights %s: %u draws in the eyes' lighting windows (%u known lighting, %u re-issued, %u unknown writing "
                    "the HDR, %u stencil-only)%s",
                    g_dumpStamp, ok ? "written" : "write failed", static_cast<unsigned>(g_lights.size()), known, reissued, unknown, stencil,
                    g_lightsCapped ? " (capped)" : "");
    g_lights.clear();
    g_lightsCapped = false;
}

// The reflection dump of eye `i`: eye_<stamp>_GameHdrRefl / _HullHdrRefl (the HDRs right after the game's reflection dispatch and
// the private re-issue), _GameStencil / _HullStencil (the texture under the pass's t4, raw: the stencil is its 8-bit plane) and
// _GameT11 / _HullT11 (under t11), each with _e1 for eye 1; a stage that never ran writes no file. eye_<stamp>_Refl[_e1].json:
// the steps staged a side, cb1[26] at the dispatch (y: the stencil mask the pass tests) and why the private side was not staged.
void writeReflection(ID3D11DeviceContext* ctx, Eye& e, const std::wstring& dir, int i) {
    const char* gameHdr = writeTexture(ctx, e.stageGameRefl, dir, L"GameHdrRefl", i);
    const char* hullHdr = writeTexture(ctx, e.stageHullRefl, dir, L"HullHdrRefl", i);
    const char* gameSt = writeTexture(ctx, e.stageGameStencil, dir, L"GameStencil", i);
    const char* hullSt = writeTexture(ctx, e.stageHullStencil, dir, L"HullStencil", i);
    const char* gameT11 = writeTexture(ctx, e.stageGameT11, dir, L"GameT11", i);
    const char* hullT11 = writeTexture(ctx, e.stageHullT11, dir, L"HullT11", i);
    uint32_t cb[4] = {};
    bool haveCb = false;
    D3D11_MAPPED_SUBRESOURCE map{};
    if (e.stageReflCb && SUCCEEDED(ctx->Map(e.stageReflCb, 0, D3D11_MAP_READ, 0, &map))) {
        D3D11_BUFFER_DESC d{};
        e.stageReflCb->GetDesc(&d);
        const size_t at = (static_cast<size_t>(e.reflCbFirst) + 26u) * 16u;
        if (at + sizeof cb <= d.ByteWidth) {
            std::memcpy(cb, static_cast<const char*>(map.pData) + at, sizeof cb);
            haveCb = true;
        }
        ctx->Unmap(e.stageReflCb, 0);
    }
    const char* why = e.reflWhy                                      ? e.reflWhy
                      : !e.dumpReflGame                              ? "the game ran no reflection dispatch for this eye on the capture frame"
                      : !e.dumpReflHull                              ? "the step was not re-issued"
                                                                     : "";
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\eye_%s_Refl%s.json", dir.c_str(), g_dumpStamp, i ? L"_e1" : L"");
    FILE* f = nullptr;
    bool ok = false;
    char cbText[64] = "null";
    if (haveCb) std::snprintf(cbText, sizeof cbText, "[%u, %u, %u, %u]", cb[0], cb[1], cb[2], cb[3]);
    if (_wfopen_s(&f, path, L"wb") == 0 && f) {
        ok = std::fprintf(f,
                          "{\"version\": 1, \"stamp\": \"%ls\", \"frame\": %u, \"eye\": %d, \"game_steps\": %u, \"hull_steps\": %u, \"cb1_first\": %u, "
                          "\"cb1_26\": %s, \"u1_clear\": %.6f, \"u1_clear_from_game\": %s, \"why\": \"%s\"}\n",
                          g_dumpStamp, g_dumpFrame, i, e.dumpReflGame, e.dumpReflHull, static_cast<unsigned>(e.reflCbFirst), cbText,
                          static_cast<double>(g_reflU1Clear[0]), g_reflU1ClearSeen ? "true" : "false", why) > 0;
        ok = (std::fclose(f) == 0) && ok;
    }
    Log::get().note("hull layer: dump %ls eye %d: reflection: game_steps=%u hull_steps=%u cb1[26].y=%s%u; GameHdrRefl %s, HullHdrRefl %s, GameStencil %s, "
                    "HullStencil %s, GameT11 %s, HullT11 %s, Refl.json %s%s%s",
                    g_dumpStamp, i, e.dumpReflGame, e.dumpReflHull, haveCb ? "" : "unread:", cb[1], gameHdr, hullHdr, gameSt, hullSt, gameT11, hullT11,
                    ok ? "written" : "write failed", *why ? "; not staged: " : "", why);
}

void writeDump(ID3D11DeviceContext* ctx) {
    const std::wstring dir = Log::get().dir() + L"\\eyes";
    CreateDirectoryW(dir.c_str(), nullptr);
    bool any = false;
    for (int i = 0; i < 2; ++i) {
        Eye& e = g_eyes[i];
        if (!e.stageHull && !e.stageGame && !e.stageDepth && !e.stageGbuf0 && !e.stageMainDepth && !e.stageTone && !e.stageComp && !e.stageGameRefl)
            continue;
        any = true;
        uint32_t w = 0, h = 0, fmt = 0;
        if (e.stageHull) {
            D3D11_TEXTURE2D_DESC d{};
            e.stageHull->GetDesc(&d);
            w = d.Width;
            h = d.Height;
            fmt = static_cast<uint32_t>(d.Format);
        }
        // The dump frame's own stats (a stall here is the dump's, like the textures' maps).
        long own = -1, filtered = -1, seedRead = -1, seedInst = -1;
        D3D11_MAPPED_SUBRESOURCE map{};
        if (e.stageStats && SUCCEEDED(ctx->Map(e.stageStats, 0, D3D11_MAP_READ, 0, &map))) {
            uint32_t v[4] = {};
            std::memcpy(v, map.pData, sizeof v);
            ctx->Unmap(e.stageStats, 0);
            own = static_cast<long>(v[0]);
            filtered = static_cast<long>(v[1]);
            seedRead = static_cast<long>(v[2]);
            seedInst = static_cast<long>(v[3]);
        }
        const char* hull = writeTexture(ctx, e.stageHull, dir, L"HullHdr", i);
        const char* game = writeTexture(ctx, e.stageGame, dir, L"GameHdrLit", i);
        const char* depth = writeTexture(ctx, e.stageDepth, dir, L"HullDepth", i);
        Log::get().note("hull layer: dump %ls eye %d frame %u: HullHdr %ux%u format %u %s, GameHdrLit %s, HullDepth %s; own=%ld filtered_px=%ld "
                        "seed=%ld/%ld hull_draws=%u light_draws=%u",
                        g_dumpStamp, i, g_dumpFrame, w, h, fmt, hull, game, depth, own, filtered, seedRead, seedInst, e.dumpHull, e.dumpLight);
        writeReflection(ctx, e, dir, i);
        if (detail::g_hullLayerSeparate && g_refFrame) {
            // Dump v2: the capture frame was a reference frame. The main pass kept the own ship, so GameHdrLit holds the game's own
            // hull beside HullHdr (tools\hull_layer_compare.py's lighting comparison), and the main depth after the eye's G-buffer
            // holds it too: RefDepth, the compare's mask of where the game shows the hull (both eyes; the eye run's Z is eye 0's).
            const char* gbuf = writeTexture(ctx, e.stageGbuf0, dir, L"RefGbuf0", i);
            const char* mainDepth = writeTexture(ctx, e.stageMainDepth, dir, L"RefDepth", i);
            Log::get().note("hull layer: dump %ls eye %d: reference frame: the main pass kept the own ship (the frame would have discarded it: %s), the "
                            "layer ran and its composite was made and not applied; RefGbuf0 %s, RefDepth %s (the game's G-buffer RT0 and main depth "
                            "after the eye's G-buffer, the own ship drawn; decals_reissued=%u)",
                            g_dumpStamp, i, g_refWould ? "yes" : "no", gbuf, mainDepth, e.decals);
        } else if (detail::g_hullLayerSeparate) {
            // The main pass with the own ship discarded: the game's G-buffer RT0 and its eye depth after the eye's G-buffer.
            const char* gbuf = writeTexture(ctx, e.stageGbuf0, dir, L"GameGbuf0", i);
            const char* mainDepth = writeTexture(ctx, e.stageMainDepth, dir, L"GameDepth", i);
            Log::get().note("hull layer: dump %ls eye %d: GameGbuf0 %s, GameDepth %s (separate mode: the game's G-buffer RT0 and main depth after the "
                            "eye's G-buffer; own_draws=%u decal_draws=%u decals_reissued=%u)",
                            g_dumpStamp, i, gbuf, mainDepth, e.ownDraws, e.ownDecals, e.decals);
        }
        if (detail::g_hullLayerSeparate) {
            // Step 3: the cover at render size (the tonemapped hull where shown, alpha 1) and the composited eye at output size, after the
            // composite and before the UI layer's door; on a fallback frame it is the composite that was made and not applied.
            const char* tone = writeTexture(ctx, e.stageTone, dir, L"HullTone", i);
            const char* comp = writeTexture(ctx, e.stageComp, dir, L"HullComposite", i);
            Log::get().note("hull layer: dump %ls eye %d: HullTone %s, HullComposite %s (separate mode, Step 3: the composite %s)", g_dumpStamp, i, tone, comp,
                            !e.stageComp       ? "did not run"
                            : e.dumpApplied    ? "applied"
                            : g_refFrame       ? "made, not applied (the reference frame)"
                                               : "made, not applied (a fallback frame)");
        }
        releaseStaging(e);
    }
    if (!any)
        Log::get().note("hull layer: dump %ls frame %u: nothing captured (no eye seeded its private G-buffer and drew its lighting in the frame)",
                        g_dumpStamp, g_dumpFrame);
    writeLights(dir);
}

// ---- dump v2: the TAA sequence -----------------------------------------------------------------------------------------

void releaseSeq() {
    for (auto& s : g_seq.ring) {
        releaseRef(s.cover);
        releaseRef(s.depth);
        releaseRef(s.comp);
        s.pending = false;
        s.index = -1;
    }
    releaseRef(g_seq.hist);
    g_seq.frames.clear();
    g_seq.active = false;
    g_seq.end = nullptr;
}

// A staging copy of src's (x, y, w, h), made or remade at that size and src's format.
bool stageRegion(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, uint32_t x, uint32_t y, uint32_t w, uint32_t h, ID3D11Texture2D*& stage) {
    if (!src || !g_device || !w || !h) return false;
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    if (d.SampleDesc.Count != 1 || x + w > d.Width || y + h > d.Height) return false;
    if (stage) {
        D3D11_TEXTURE2D_DESC s{};
        stage->GetDesc(&s);
        if (s.Width != w || s.Height != h || s.Format != d.Format) releaseRef(stage);
    }
    if (!stage) {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = w;
        sd.Height = h;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = d.Format;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &stage)) || !stage) {
            stage = nullptr;
            return false;
        }
    }
    const D3D11_BOX box{x, y, 0u, x + w, y + h, 1u};
    vScreenCopySubresourceRegionRaw(ctx, stage, 0, src, &box);
    return true;
}

// One EDVRTEX1 file of a mapped staging texture's crop (x, y, w, h): eye_<seq stamp>_<name>.bin, eye 0, the layer frame `frame`.
bool writeCrop(const std::wstring& dir, const wchar_t* name, uint32_t frame, const D3D11_MAPPED_SUBRESOURCE& map, DXGI_FORMAT fmt, uint32_t bpp,
               const uint32_t crop[4]) {
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\eye_%s_%s.bin", dir.c_str(), g_seq.stamp, name);
    FILE* f = nullptr;
    bool ok = false;
    const uint32_t rowBytes = crop[2] * bpp;
    if (_wfopen_s(&f, path, L"wb") == 0 && f) {
        const uint32_t header[9] = {1u, crop[2], crop[3], static_cast<uint32_t>(fmt), rowBytes, frame, 0u, 0u, 0u};
        ok = std::fwrite("EDVRTEX1", 1, 8, f) == 8 && std::fwrite(header, sizeof(header), 1, f) == 1;
        for (uint32_t y = 0; y < crop[3] && ok; ++y)
            ok = std::fwrite(static_cast<const char*>(map.pData) + static_cast<size_t>(crop[1] + y) * map.RowPitch + static_cast<size_t>(crop[0]) * bpp, 1,
                             rowBytes, f) == rowBytes;
        ok = (std::fclose(f) == 0) && ok;
    }
    if (ok) g_seq.bytes += 44ull + static_cast<uint64_t>(rowBytes) * crop[3];
    return ok;
}

// A crop grown by `m` on every side, within (0, 0, w, h).
void growCrop(const uint32_t in[4], uint32_t m, uint32_t w, uint32_t h, uint32_t out[4]) {
    const uint32_t x0 = in[0] > m ? in[0] - m : 0u, y0 = in[1] > m ? in[1] - m : 0u;
    const uint32_t x1 = std::min(in[0] + in[2] + m, w), y1 = std::min(in[1] + in[3] + m, h);
    out[0] = x0;
    out[1] = y0;
    out[2] = x1 > x0 ? x1 - x0 : 1u;
    out[3] = y1 > y0 ? y1 - y0 : 1u;
}

// A staged set to disk (a blocking map: kSeqLag frames after its copies, the GPU is normally done). The cover is cropped to its
// extent (alpha above 0) and kSeqMargin, the depth to the same crop; the composite to that crop at the output scale and 8 px;
// frame 0's history to the composite's crop and kSeqHistMargin. Outside a crop the cover, depth and history are zero.
void seqWriteSet(ID3D11DeviceContext* ctx, SeqSet& s) {
    if (!s.pending) return;
    s.pending = false;
    if (s.index < 0 || s.index >= static_cast<int>(g_seq.frames.size())) return;
    SeqFrame& f = g_seq.frames[static_cast<size_t>(s.index)];
    const std::wstring dir = Log::get().dir() + L"\\eyes";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t name[40];
    bool ok = true;
    D3D11_MAPPED_SUBRESOURCE map{};
    if (!s.cover || FAILED(ctx->Map(s.cover, 0, D3D11_MAP_READ, 0, &map))) {
        f.status = "map failed";
        return;
    }
    uint32_t x0 = f.renderW, y0 = f.renderH, x1 = 0, y1 = 0;
    for (uint32_t y = 0; y < f.renderH; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch;
        for (uint32_t x = 0; x < f.renderW; ++x)
            if (row[4 * x + 3]) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
    }
    if (x1 < x0 || y1 < y0) {
        f.crop[0] = f.crop[1] = 0;
        f.crop[2] = f.crop[3] = 1;
    } else {
        const uint32_t ext[4] = {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
        growCrop(ext, kSeqMargin, f.renderW, f.renderH, f.crop);
    }
    _snwprintf_s(name, 40, _TRUNCATE, L"HullSeq_%02d_Cover", f.index);
    ok = writeCrop(dir, name, f.frame, map, DXGI_FORMAT_R8G8B8A8_UNORM, 4, f.crop) && ok;
    ctx->Unmap(s.cover, 0);
    if (s.depth && SUCCEEDED(ctx->Map(s.depth, 0, D3D11_MAP_READ, 0, &map))) {
        _snwprintf_s(name, 40, _TRUNCATE, L"HullSeq_%02d_Depth", f.index);
        ok = writeCrop(dir, name, f.frame, map, DXGI_FORMAT_R32_FLOAT, 4, f.crop) && ok;
        ctx->Unmap(s.depth, 0);
    } else {
        ok = false;
    }
    // The composite's crop: the cover's at the output scale.
    const uint64_t ox0 = static_cast<uint64_t>(f.crop[0]) * f.outW / f.renderW, oy0 = static_cast<uint64_t>(f.crop[1]) * f.outH / f.renderH;
    const uint64_t ox1 = (static_cast<uint64_t>(f.crop[0] + f.crop[2]) * f.outW + f.renderW - 1) / f.renderW;
    const uint64_t oy1 = (static_cast<uint64_t>(f.crop[1] + f.crop[3]) * f.outH + f.renderH - 1) / f.renderH;
    const uint32_t scaled[4] = {static_cast<uint32_t>(ox0), static_cast<uint32_t>(oy0), static_cast<uint32_t>(ox1 - ox0), static_cast<uint32_t>(oy1 - oy0)};
    growCrop(scaled, 8, f.outW, f.outH, f.compCrop);
    if (s.comp && SUCCEEDED(ctx->Map(s.comp, 0, D3D11_MAP_READ, 0, &map))) {
        _snwprintf_s(name, 40, _TRUNCATE, L"HullSeq_%02d_Comp", f.index);
        ok = writeCrop(dir, name, f.frame, map, DXGI_FORMAT_R8G8B8A8_UNORM, 4, f.compCrop) && ok;
        ctx->Unmap(s.comp, 0);
    } else {
        ok = false;
    }
    if (f.index == 0 && g_seq.hist) {
        growCrop(f.compCrop, kSeqHistMargin, f.outW, f.outH, g_seq.histCrop);
        if (SUCCEEDED(ctx->Map(g_seq.hist, 0, D3D11_MAP_READ, 0, &map))) {
            g_seq.histStatus = writeCrop(dir, L"HullSeq_00_Hist", f.frame, map, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, g_seq.histCrop) ? "written" : "write failed";
            ctx->Unmap(g_seq.hist, 0);
        } else {
            g_seq.histStatus = "map failed";
        }
        releaseRef(g_seq.hist);
    }
    f.status = ok ? "written" : "write failed";
}

// Right after eye 0's composite (hullLayerCompositeEye), while the sequence runs: the cover and its depth (the eye's region),
// the composite and the TAA's constants staged; on frame 0, the history it read too.
void seqStage(ID3D11DeviceContext* ctx, const Eye& e, const Comp& c, const HullLayerCompositeInputs& in, bool history, bool applied) {
    if (!g_seq.active || g_seq.end || g_seq.staged >= kSeqFrames) return;
    if (g_seq.staged == 0) {
        const uint64_t set = static_cast<uint64_t>(in.renderW) * in.renderH * 8u + static_cast<uint64_t>(in.outW) * in.outH * 4u;
        if (set * kSeqRing + static_cast<uint64_t>(in.outW) * in.outH * 8u > kSeqBudgetBytes) {
            g_seq.end = "over the memory budget";
            return;
        }
    }
    // A free set, else the oldest staged one written now.
    SeqSet* s = nullptr;
    for (auto& r : g_seq.ring)
        if (!r.pending) {
            s = &r;
            break;
        }
    if (!s) {
        for (auto& r : g_seq.ring)
            if (!s || r.index < s->index) s = &r;
        seqWriteSet(ctx, *s);
    }
    if (!stageRegion(ctx, e.cover, in.regionX, in.regionY, in.renderW, in.renderH, s->cover) ||
        !stageRegion(ctx, e.coverZ, in.regionX, in.regionY, in.renderW, in.renderH, s->depth) ||
        !stageRegion(ctx, c.out, 0, 0, in.outW, in.outH, s->comp)) {
        g_seq.end = "a staging copy could not be made";
        return;
    }
    if (g_seq.staged == 0 && history) copyToStaging(ctx, c.hist[c.read], g_seq.hist);   // the history this TAA read
    SeqFrame f;
    f.index = g_seq.staged;
    f.frame = g_frame;
    f.renderW = in.renderW;
    f.renderH = in.renderH;
    f.outW = in.outW;
    f.outH = in.outH;
    f.regionX = in.regionX;
    f.regionY = in.regionY;
    f.jitter[0] = in.jitterX;
    f.jitter[1] = in.jitterY;
    std::memcpy(f.tanNow, in.tanNow, sizeof f.tanNow);
    std::memcpy(f.tanPrev, in.tanPrev ? in.tanPrev : in.tanNow, sizeof f.tanPrev);   // as the TAA's constants have it
    f.havePrev = in.tanPrev != nullptr;
    for (int k = 0; k < 9; ++k) f.headDelta[k] = in.headDelta ? in.headDelta[k] : (k % 4 == 0 ? 1.0f : 0.0f);
    f.haveDelta = in.headDelta != nullptr;
    f.useTrans = in.headTrans && in.useTrans;
    if (f.useTrans) std::memcpy(f.headTrans, in.headTrans, sizeof f.headTrans);
    f.depthA = in.depthA;
    f.depthB = in.depthB;
    f.history = history;
    f.applied = applied;
    f.afterUiResolve = in.afterUiResolve;
    s->index = f.index;
    s->pending = true;
    g_seq.frames.push_back(f);
    ++g_seq.staged;
    g_seq.lastFrame = g_frame;
}

// eye_<stamp>_HullSeq.json: the per-frame constants the TAA used, the crops, and the TAA's parameters from this build.
void seqWriteJson() {
    const std::wstring dir = Log::get().dir() + L"\\eyes";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\eye_%s_HullSeq.json", dir.c_str(), g_seq.stamp);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return;
    std::fprintf(f, "{\"version\": 1, \"stamp\": \"%ls\", \"eye\": 0, \"start_frame\": %u, \"frames_wanted\": %d, \"end\": \"%s\",\n", g_seq.stamp,
                 g_seq.startFrame, kSeqFrames, g_seq.end ? g_seq.end : "?");
    std::fprintf(f, " \"taa\": {\"current_weight\": %.9g, \"sharpen\": %.9g},\n", static_cast<double>(kTaaCurrent), static_cast<double>(kSharpAmount));
    std::fprintf(f, " \"history\": {\"file\": \"HullSeq_00_Hist\", \"status\": \"%s\", \"crop\": [%u, %u, %u, %u]},\n \"frames\": [", g_seq.histStatus,
                 g_seq.histCrop[0], g_seq.histCrop[1], g_seq.histCrop[2], g_seq.histCrop[3]);
    for (size_t i = 0; i < g_seq.frames.size(); ++i) {
        const SeqFrame& q = g_seq.frames[i];
        std::fprintf(f,
                     "%s\n  {\"index\": %d, \"frame\": %u, \"status\": \"%s\", \"render\": [%u, %u], \"output\": [%u, %u], \"region\": [%u, %u], "
                     "\"crop\": [%u, %u, %u, %u], \"comp_crop\": [%u, %u, %u, %u], \"jitter\": [%.9g, %.9g], ",
                     i ? "," : "", q.index, q.frame, q.status, q.renderW, q.renderH, q.outW, q.outH, q.regionX, q.regionY, q.crop[0], q.crop[1], q.crop[2],
                     q.crop[3], q.compCrop[0], q.compCrop[1], q.compCrop[2], q.compCrop[3], static_cast<double>(q.jitter[0]),
                     static_cast<double>(q.jitter[1]));
        std::fprintf(f, "\"tan_now\": [%.9g, %.9g, %.9g, %.9g], \"tan_prev\": [%.9g, %.9g, %.9g, %.9g], \"tan_prev_given\": %s, ",
                     static_cast<double>(q.tanNow[0]), static_cast<double>(q.tanNow[1]), static_cast<double>(q.tanNow[2]), static_cast<double>(q.tanNow[3]),
                     static_cast<double>(q.tanPrev[0]), static_cast<double>(q.tanPrev[1]), static_cast<double>(q.tanPrev[2]), static_cast<double>(q.tanPrev[3]),
                     q.havePrev ? "true" : "false");
        std::fprintf(f, "\"head_delta\": [");
        for (int k = 0; k < 9; ++k) std::fprintf(f, "%s%.9g", k ? ", " : "", static_cast<double>(q.headDelta[k]));
        std::fprintf(f,
                     "], \"head_delta_given\": %s, \"head_trans\": [%.9g, %.9g, %.9g], \"use_trans\": %s, \"depth_ab\": [%.9g, %.9g], \"history\": %s, "
                     "\"applied\": %s, \"after_ui_resolve\": %s}",
                     q.haveDelta ? "true" : "false", static_cast<double>(q.headTrans[0]), static_cast<double>(q.headTrans[1]),
                     static_cast<double>(q.headTrans[2]), q.useTrans ? "true" : "false", static_cast<double>(q.depthA), static_cast<double>(q.depthB),
                     q.history ? "true" : "false", q.applied ? "true" : "false", q.afterUiResolve ? "true" : "false");
    }
    std::fprintf(f, "\n ]\n}\n");
    std::fclose(f);
}

// At the frame boundary (g_frame already the next frame's): the sets kSeqLag frames old written; the sequence ends when it is
// complete, when a frame passed without eye 0's composite (the TAA's frames must follow one another), or when none came.
void seqBoundary(ID3D11DeviceContext* ctx) {
    if (!g_seq.active || !ctx) return;
    if (!g_seq.end) {
        if (g_seq.staged >= kSeqFrames)
            g_seq.end = "complete";
        else if (g_seq.staged > 0 && g_seq.lastFrame + 1 != g_frame)
            g_seq.end = "a frame without eye 0's composite";
        else if (g_seq.staged == 0 && g_frame - g_seq.startFrame > 2)
            g_seq.end = "eye 0 was not composited";
    }
    bool pending = false;
    for (auto& s : g_seq.ring) {
        if (!s.pending) continue;
        const uint32_t at = s.index >= 0 && s.index < static_cast<int>(g_seq.frames.size()) ? g_seq.frames[static_cast<size_t>(s.index)].frame : 0u;
        if (g_seq.end || g_frame - at >= kSeqLag)
            seqWriteSet(ctx, s);
        else
            pending = true;
    }
    if (!g_seq.end || pending) return;
    seqWriteJson();
    uint32_t written = 0;
    for (const auto& q : g_seq.frames)
        if (std::strcmp(q.status, "written") == 0) ++written;
    Log::get().note("hull layer: dump %ls: HullSeq %u of %d frames written (eye 0 from frame %u, %.1f MB), history %s; end: %s "
                    "(tools\\hull_taa_sim.py reproduces the TAA from them)",
                    g_seq.stamp, written, kSeqFrames, g_seq.startFrame, static_cast<double>(g_seq.bytes) / (1024.0 * 1024.0), g_seq.histStatus, g_seq.end);
    releaseSeq();
}

// The dump's capture frame begins (separate mode): the sequence starts on it.
void seqStart() {
    if (g_seq.active) {
        Log::get().note("hull layer: dump %ls: no HullSeq (the sequence of dump %ls is still being written)", g_dumpStamp, g_seq.stamp);
        return;
    }
    releaseSeq();
    g_seq.active = true;
    wcsncpy_s(g_seq.stamp, g_dumpStamp, _TRUNCATE);
    g_seq.staged = 0;
    g_seq.startFrame = g_frame;
    g_seq.lastFrame = ~0u;
    g_seq.bytes = 0;
    g_seq.histStatus = "none (frame 0 had no history)";
    for (auto& h : g_seq.histCrop) h = 0;
}

// ---- the window line --------------------------------------------------------------------------------------------------

void loMidHi(std::vector<uint32_t> v, uint32_t* lo, uint32_t* mid, uint32_t* hi) {
    *lo = *mid = *hi = 0;
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    *lo = v.front();
    *mid = v[(v.size() - 1) / 2];
    *hi = v.back();
}

// The median of a window's samples, -1 with none.
double medianOf(std::vector<float> v) {
    if (v.empty()) return -1.0;
    std::sort(v.begin(), v.end());
    return static_cast<double>(v[(v.size() - 1) / 2]);
}

// Dump v2: the stage times' line (both modes): the median GPU ms a timed frame by stage and of their total, the items and the
// CPU ms a frame by stage over the window.
void reportStages() {
    double gpu[kTStages + 1];
    for (int k = 0; k <= kTStages; ++k) gpu[k] = medianOf(g_st.ms[k]);
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    const double frames = g_win.frames ? static_cast<double>(g_win.frames) : 1.0;
    double cpu[kTStages], items[kTStages], cpuTotal = 0.0;
    for (int k = 0; k < kTStages; ++k) {
        cpu[k] = freq.QuadPart ? static_cast<double>(g_st.cpuTicks[k]) * 1000.0 / static_cast<double>(freq.QuadPart) / frames : 0.0;
        items[k] = static_cast<double>(g_st.items[k]) / frames;
        cpuTotal += cpu[k];
    }
    Log::get().note("hull layer: gpu_ms gbuffer=%.3f mask_filter=%.3f screen=%.3f light=%.3f tone=%.3f taa=%.3f total=%.3f timed_frames=%u "
                    "items=gbuffer:%.1f,mask_filter:%.1f,screen:%.1f,light:%.1f,tone:%.1f,taa:%.1f cpu_ms=gbuffer:%.3f,mask_filter:%.3f,screen:%.3f,"
                    "light:%.3f,tone:%.3f,taa:%.3f,total:%.3f overflow=%u busy=%u disjoint=%u (dump v2: the median GPU ms a timed frame (one in %u) by "
                    "stage, the layer's own work only: gbuffer the private G-buffer and decal re-issues, mask_filter the seed, filter and count, "
                    "screen the depth copy, shadow mask, AO steps and apply, light the lighting and night vision, tone the tonemap re-issue and cover, "
                    "taa the TAA, sharpen and composite; -1 when none was timed; items and cpu_ms a frame over the window)",
                    gpu[kTsGbuf], gpu[kTsMask], gpu[kTsScreen], gpu[kTsLight], gpu[kTsTone], gpu[kTsTaa], gpu[kTStages], g_st.timed, items[kTsGbuf],
                    items[kTsMask], items[kTsScreen], items[kTsLight], items[kTsTone], items[kTsTaa], cpu[kTsGbuf], cpu[kTsMask], cpu[kTsScreen],
                    cpu[kTsLight], cpu[kTsTone], cpu[kTsTaa], cpuTotal, g_st.overflow, g_st.busy, g_st.disjoint, kStageEvery);
    stageWindowReset();
}

void report() {
    uint32_t o[3], f[3], hd[3], ld[3];
    loMidHi(g_win.own, &o[0], &o[1], &o[2]);
    loMidHi(g_win.filtered, &f[0], &f[1], &f[2]);
    loMidHi(g_win.hullDraws, &hd[0], &hd[1], &hd[2]);
    loMidHi(g_win.lightDraws, &ld[0], &ld[1], &ld[2]);
    const char* state = g_win.seeds ? (g_win.readbacks == 0 ? "mask-pending" : g_win.subst[kLight] == 0 ? "partial" : "applied")
                        : g_win.busyFrames  ? "busy"   // Station fixes: the governor slept the whole window
                        : g_win.noShader    ? "no-shader"
                        : g_win.noSlot      ? "no-slot-target"
                        : g_win.refused     ? "seed-refused"
                        : g_win.eyeFrames   ? "no-seed"
                        : g_win.hullSeen    ? "no-shell"
                                            : "no-eye-pass";
    // Station fixes: culled_draws (re-issues the keep-own VS culled to the own instances) and filter_draws (re-issues that needed
    // the neighbour filter); busy (frames the busy governor slept), offscreen (stage draws declined for a target not at the eye's
    // render size) and null_rt1 (lighting re-issues with the scratch R16 left unbound). night_vision: the game's night-vision
    // draw re-issued into the private HDR, and of those the ones that ran with EDVR's night-vision state (0/0: no such draw;
    // N/0: re-issued as the game's own, night vision's fix off or declined).
    Log::get().note("hull layer: state=%s frames=%u eye_frames=%u seeds=%u no_seed=%u no_slot=%u refused=%u own=%u/%u/%u readbacks=%u "
                    "filtered_px=%u/%u/%u culled_draws=%u filter_draws=%u hull_draws=%u/%u/%u reissued=%u no_slot_draws=%u clears=%u filters=%u "
                    "subst=copy:%u,mask:%u,light:%u light_draws=%u/%u/%u no_counterpart=copy:%u,mask:%u,light:%u unknown_light_pairs=%u "
                    "ended=copy:%u,shell:%u,light:%u,frame:%u terrain_frames=%u auto_dumps=%u busy=%u offscreen=%u null_rt1=%u night_vision=%u/%u",
                    state, g_win.frames, g_win.eyeFrames, g_win.seeds, g_win.noSeed, g_win.noSlot, g_win.refused, o[0], o[1], o[2], g_win.readbacks,
                    f[0], f[1], f[2], g_win.culledDraws, g_win.filterDraws, hd[0], hd[1], hd[2], g_win.reissued, g_win.noSlotDraws, g_win.clears,
                    g_win.filters, g_win.subst[kCopy], g_win.subst[kMask], g_win.subst[kLight], ld[0], ld[1], ld[2], g_win.noCounterpart[kCopy],
                    g_win.noCounterpart[kMask], g_win.noCounterpart[kLight], g_win.unknownPairs, g_win.ended[kEndCopy], g_win.ended[kEndShell],
                    g_win.ended[kEndLight], g_win.ended[kEndFrame], g_win.terrainFrames, g_win.autoDumps, g_win.busyFrames, g_win.offscreen,
                    g_win.nullRt1, g_win.nightVision, g_win.nightVisionEdvr);
    uint32_t rs[3], rr[3], rh[3];
    loMidHi(g_win.rigSlots, &rs[0], &rs[1], &rs[2]);
    loMidHi(g_win.rigRadius, &rr[0], &rr[1], &rr[2]);
    loMidHi(g_win.rigReach, &rh[0], &rh[1], &rh[2]);
    Log::get().note("hull layer: own seed: rig=%u draw=%u hold_resets=%u rig_slots=%u/%u/%u radius=%u/%u/%u reach=%u/%u/%u (eye-frames whose own mask grew "
                    "from the rig probe's own slots, and from the seed draw, the fallback when the pool held no own slot; the hold restarted when the own "
                    "rigs changed; over the rig seeds min/median/max: slots, and from their readbacks the radius and the reach held, in metres)",
                    g_win.rigSeeds, g_win.drawSeeds, g_win.holdResets, rs[0], rs[1], rs[2], rr[0], rr[1], rr[2], rh[0], rh[1], rh[2]);
    Log::get().note("hull layer: ao: eye_frames=%u full=%u dispatches=%u per_eye_frame=%.2f applied=%u missed=deinterleave:%u,hbao:%u,interleave:%u,"
                    "apply:%u no_eye=%u (the game's AO chain re-issued on the private G-buffer: eye-frames with a private G-buffer, and those whose "
                    "three compute steps and apply draw all were; per_eye_frame is dispatches over eye-frames, %d when every step ran; missed: "
                    "eye-frames where the game ran a step for the eye and it was not re-issued; no_eye: chain dispatches no eye's resources named)",
                    g_win.aoEyeFrames, g_win.aoFull, g_win.aoDispatches,
                    g_win.aoEyeFrames ? static_cast<double>(g_win.aoDispatches) / g_win.aoEyeFrames : 0.0, g_win.aoApplied, g_win.aoMissed[0],
                    g_win.aoMissed[1], g_win.aoMissed[2], g_win.aoMissed[kAoSteps], g_win.aoNoEye, kAoSteps);
    reportStages();
    if (detail::g_hullLayerSeparate) {
        Log::get().note("hull layer: reflections: dispatches=%u no_eye=%u declined=%u u1_cleared=%u u1_game_clears=%u u1_clear=%s%.4f "
                        "(the game's reflection pass, two indirect dispatches an eye, "
                        "re-issued into the private HDR with the game's tile list; no_eye: pass dispatches no eye's G-buffer named; declined: an "
                        "eye's dispatch not re-issued, no private HDR this frame or a view that could not be made; u1_cleared: eye-frames whose "
                        "private u1 (R16) was cleared before the first re-issue; u1_game_clears: the game's own clears of its u1 in the window; "
                        "u1_clear: the value the private one is cleared to, the game's last clear or 0 before one is seen)",
                        g_win.reflDispatches, g_win.reflNoEye, g_win.reflDeclined, g_win.reflU1Clears, g_win.reflU1GameClears,
                        g_reflU1ClearSeen ? "game:" : "default:", static_cast<double>(g_reflU1Clear[0]));
        Log::get().note("hull layer: main pass: own_draws=%u decal_draws=%u no_variant_draws=%u fallback_frames=%u decals_reissued=%u decal_no_slot=%u "
                        "(separate mode: draws issued with the own discard; window draws that could not take it; eye-frames whose main pass kept the own "
                        "ship, no mask)",
                        g_win.ownDraws, g_win.ownDecals, g_win.noVariant, g_win.fallbackFrames, g_win.decalsReissued, g_win.decalNoSlot);
        Log::get().note("hull layer: decals: copies_mirrored=%u srv_swaps=%u overlay_t3=private:%u,unbound:%u,kept:%u (the game's copies out of a "
                        "mirrored target made between the private copies too; decal re-issues whose base texture was the private copy; t3, engine "
                        "motion's slot snapshot: re-issues that read its private copy, read nothing there with MRT6 unbound, or kept the main "
                        "pass's with MRT6 bound; all three 0 with decals re-issued: no decal read the snapshot or it was never seen copied)",
                        g_win.copiesMirrored, g_win.decalSrvSwaps, g_win.overlaySwapped, g_win.overlayUnbound, g_win.overlayKept);
        Log::get().note("hull layer: tone bloom: zeroed=%u declined=%u last_declined=%s (the tonemap re-issue's t2, the game's bloom of its "
                        "world-only image: re-issues that read the zeroed texture there, and those that kept the game's; both 0: no tonemap "
                        "re-issue ran)",
                        g_win.bloomZeroed, g_win.bloomDeclined, g_win.bloomWhy ? g_win.bloomWhy : "-");
    }
    if (detail::g_hullLayerSeparate) {
        const double toneMs = timerTake(g_toneTimer), compMs = timerTake(g_compTimer);
        Log::get().note("hull layer: composite: separate_frames=%u composited_eyes=%u ready_eyes=%u fallback=framegen:%u,outside:%u,mask_pending:%u,"
                        "mask_large:%u,streak:%u failed=seed:%u,lit:%u,tone:%u,composite:%u declined=stale:%u,size:%u,gpu:%u toned=%u tone_declined=%u "
                        "history_resets=%u matched_by_index=%u own_max=%u site=after_ui_resolve:%u,upscaler_out:%u aa=hull_taa gpu_ms=tone:%.3f,composite:%.3f "
                        "(separate mode, Step 3: frames whose main pass may discard the own ship, and those kept by reason; eye-frames composited after the "
                        "upscaler, or made ready on a fallback frame; misses by stage hold the discard off %u frames; site: where applied composites landed, "
                        "upscaler_out only when no UI resolve can run; gpu_ms a call, -1 when none was timed)",
                        g_win.separateFrames, g_win.compositedEyes, g_win.readyEyes, g_win.fallback[kFbFrameGen], g_win.fallback[kFbOutside],
                        g_win.fallback[kFbMaskPending], g_win.fallback[kFbMaskLarge], g_win.fallback[kFbStreak], g_win.failSeed, g_win.failLit,
                        g_win.failTone, g_win.failComposite, g_win.declined[kDcStale], g_win.declined[kDcSize], g_win.declined[kDcGpu], g_win.toned,
                        g_win.toneDeclined, g_win.historyResets, g_win.byIndex, g_win.ownMax, g_win.siteAfterResolve, g_win.siteUpscalerOut, toneMs,
                        compMs, kHoldFrames);
        Log::get().note("hull layer: composite method: direct=%u copy=%u copy_why=not_applied:%u,dump_or_seq:%u,no_uav:%u,format:%u,typed_load:%u,"
                        "view:%u,shader:%u,off:%u bench_dlss=%u (eye-composites: direct = the hull written straight onto the eye's listed tiles "
                        "through its typed UAV, no whole-eye copy; copy = through the composite's own out and two whole-eye copies, by "
                        "reason; bench_dlss = the bench's second DLSS, its own copy path; direct=0 copy=0 means no composite ran)",
                        g_win.compDirect,
                        g_win.compCopy[kCmNotApplied] + g_win.compCopy[kCmNeedsOut] + g_win.compCopy[kCmNoUav] + g_win.compCopy[kCmFormat] +
                            g_win.compCopy[kCmTypedLoad] + g_win.compCopy[kCmView] + g_win.compCopy[kCmShader] + g_win.compCopy[kCmOff],
                        g_win.compCopy[kCmNotApplied], g_win.compCopy[kCmNeedsOut], g_win.compCopy[kCmNoUav], g_win.compCopy[kCmFormat],
                        g_win.compCopy[kCmTypedLoad], g_win.compCopy[kCmView], g_win.compCopy[kCmShader], g_win.compCopy[kCmOff],
                        g_win.compBenchDlss);
        {
            const uint8_t fgMode = temporalPassFrameGenMode();
            const char* fg = fgMode == 1 ? "auto" : fgMode == 2 ? "on" : frameGenShadowOn() ? "shadow" : "off";
            Log::get().note("hull layer: framegen: fg=%s s_frames=%u hull_in_s=%u world_only_inputs=before_hull:%u,copy:%u,direct:%u "
                            "world_copies=%u copy_why=not_asked:%u,not_direct:%u,bench:%u hull_after_s=%u lost=%u overlay=tiles:%u,whole:%u "
                            "declined=not_applied:%u,no_composite:%u,no_world_copy:%u,size:%u,unsupported:%u (frame generation's S in separate "
                            "mode: S builds handed the eye, those with the hull layer drawn in by the head's step, their world-only input by "
                            "source -- the eye itself before the hull's write (S first), the copy kept before the composite, or the upscaler's "
                            "output the composite did not write -- the copies made and why; S-first composites whose hull went onto the eye "
                            "after S, and those whose finish never came (that eye went out without the hull); the overlay by pass; and S "
                            "built without the layer by reason, warping the eye as it was; s_frames=0 with fg on: S never reached the layer)",
                            fg, g_win.fgS, g_win.fgHullInS, g_win.fgWorldBefore, g_win.fgWorldCopy, g_win.fgWorldDirect, g_win.fgCopies,
                            g_win.fgCopyWhy[kWcNotAsked], g_win.fgCopyWhy[kWcNotDirect], g_win.fgCopyWhy[kWcBench], g_win.fgFinished,
                            g_win.fgFinishLost, g_win.fgOverTiles, g_win.fgOverWhole, g_win.fgDeclined[kFgNotApplied],
                            g_win.fgDeclined[kFgNoComposite], g_win.fgDeclined[kFgNoWorldCopy], g_win.fgDeclined[kFgSize],
                            g_win.fgDeclined[kFgUnsupported]);
        }
        Log::get().note("hull layer: rect:decided=%u rect=%u share=%.3f whole=no_box:%u,stale:%u,empty:%u,probe:%u boxes=%u empty_boxes=%u "
                        "scissored_draws=%u with_game_scissor=%u no_state=%u (Build A': eye-frames whose re-issues were decided, those scissored "
                        "to the shown hull's box of %u+ frames before grown %u px a frame of its age (at least %u), and their mean share of the "
                        "eye; whole-eye eye-frames by reason, a probe every %u frames; cover boxes read back; draws re-issued scissored; "
                        "decided=0 means the rectangle never ran)",
                        g_win.rectDecided, g_win.rectFrames, g_win.rectFrames ? g_win.rectShare / g_win.rectFrames : -1.0, g_win.wholeNoBox,
                        g_win.wholeStale, g_win.wholeEmpty, g_win.wholeProbe, g_win.boxesRead, g_win.boxesEmpty,
                        g_win.scissorDraws, g_win.scissorGame, g_win.scissorNoState, kReadbackLag, kRectPerFrame, kRectMarginMin,
                        kRectProbeEvery);
        Log::get().note("hull layer: prefill: seeds=%u prefilled=%u in_rect=%u reset=%u unreset=%u skip=mode:%u,format:%u,view:%u,shader:%u,"
                        "targets:%u (Build B: eye-frames seeded; those whose private depth took the game's eye depth before the hull, "
                        "and of them in the Build A' rectangle; resets of it to hull-only before the depth copy; prefilled and never "
                        "reset, expected 0; not prefilled by reason, mode = not separate; seeds=0 means it never ran)",
                        g_win.preSeeds, g_win.preFrames, g_win.preRect, g_win.preResets, g_win.preUnreset, g_win.preSkipMode,
                        g_win.preSkipFormat, g_win.preSkipView, g_win.preSkipShader, g_win.preSkipTargets);
        g_saidMaskLarge = false;   // the large-mask line once a window
    }
}

// ---- the variant bench (hotkey.hull_bench, TEMPORARY; hull_layer.h, docs "Variant bench") -----------------------------------
//
// Idle -press-> Rect (the hull's extent from both eyes' covers) -> Quiet (until the press toast is gone) -> per variant
// { Warm -> Capture (8 frames) -> Settle (2) } -> Drain (the last staging maps, the times, the writer thread) -> Idle.
// The phase always names the frame about to run: the boundary before a frame decides it (benchStep, then decideNext, then
// benchDecided), as decideNext decides the discard. The frame's hooks (hullBenchCaptureEye, benchAfterComposite,
// hullBenchCaptureFinal) only stage copies and mark what they did; the boundary after it judges it.

namespace hb = hull_bench;

enum class BenchPhase : uint8_t { Idle, Rect, Quiet, Warm, Capture, Settle, Drain };
constexpr uint32_t kBenchRectFrames = 45;     // frames the rect step waits for both eyes' covers
constexpr uint32_t kBenchGrow = 96;           // output pixels about the hull's extent
constexpr ULONGLONG kBenchQuietMs = 2800;     // from the press: the press toast (menu.cpp kToastMs, 2.5 s) gone before any capture
constexpr uint32_t kBenchWarmLong = 60;       // the history variants: the main DLSS relearns the hull, the layer's TAA converges (0.05)
constexpr uint32_t kBenchWarmShort = 8;       // no history; also the re-warm after a lost capture frame
constexpr uint32_t kBenchSettleFrames = 2;
// Fallback frames (decideNext) before a variant is refused: 150, past kHoldFrames, so one miss on waking from an original (its
// 120-frame hold) costs time and not the variant; the run's wall clock (kBenchRunMs) still bounds it.
constexpr uint32_t kBenchFallbackMax = 150;
constexpr uint32_t kBenchRestartMax = 4;      // capture restarts before a variant is refused
constexpr ULONGLONG kBenchRunMs = 20000;      // the variants' wall clock, from the end of the quiet
constexpr ULONGLONG kBenchDrainMs = 60000;    // the drain's
constexpr uint32_t kBenchDrainFrames = 8;     // drain frames at least (the stage timers' last sets come back)
constexpr uint64_t kBenchMinFreeBytes = 4ull << 30;
constexpr uint32_t kBenchSlots = 16;          // staging copies in flight
constexpr uint32_t kBenchMapLag = 2;          // frames before a staging copy is mapped without waiting
constexpr uint32_t kBenchMapForce = 8;        // frames after which it is mapped waiting
constexpr uint32_t kBenchFreeToCapture = 6;   // free staging copies a capture run needs to start
constexpr uint32_t kBenchQueries = 32;
enum : int { kBtMainDlss = 0, kBtHullDlss = 1, kBtSpan = 2, kBtCpu = 3, kBtStage = 10 };

struct BenchSlot {   // a staging copy on its way to the writer
    ID3D11Texture2D* tex = nullptr;
    uint32_t w = 0, h = 0, bpp = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN, fileFmt = DXGI_FORMAT_UNKNOWN;
    bool pending = false;
    uint32_t at = 0, frame = 0, eye = 0;
    std::wstring path;
};
struct BenchQuery {   // a GPU time around one evaluation
    ID3D11Query* dj = nullptr;
    ID3D11Query* t0 = nullptr;
    ID3D11Query* t1 = nullptr;
    uint32_t frame = 0;
    int kind = -1;
    bool open = false, pending = false;
};
struct BenchTime {
    uint32_t frame = 0;
    int kind = 0;
    double ms = 0.0;
};
struct BenchRun {
    BenchPhase phase = BenchPhase::Idle;
    int var = -1;
    uint32_t left = 0;
    int capIndex = -1;   // Capture: the frame about to run is capture frame capIndex
    uint32_t restarts[hb::kVariants] = {};   // capture runs restarted, by variant
    uint32_t offCapture = 0;   // an A/B variant's feature switched off in this capture run (benchNoteOff)
    // The frame in progress, from its hooks; judged and cleared at its boundary.
    bool staged0 = false, applied0 = false, discarded = false;
    hb::Frame pendingFrame;
    wchar_t stamp[32] = L"";
    std::string stampA;
    std::wstring dir;
    ULONGLONG pressMs = 0, runStartMs = 0, drainStartMs = 0;
    uint32_t drainFrames = 0;
    uint32_t seat = 0;
    bool sawDlss = false, sawOther = false;
    uint32_t renderW = 0, renderH = 0, outW = 0, outH = 0;
    uint32_t rectFrames = 0;
    uint32_t rectAt[2] = {};
    int rectState[2] = {};   // 0 not staged, 1 staged, 2 read
    uint32_t box[4] = {};    // the covers' extent, render px of the eye region: x0, y0, x1, y1 (exclusive)
    bool boxAny = false;
    uint32_t align = 1;
    uint32_t rectR[4] = {}, rectO[4] = {};
    std::vector<BenchTime> times;
    int64_t cpuLast = 0;
    bool finalWant[2] = {};
    bool maskTaken = false;
    uint32_t stageFails = 0;
    bool seqStarted = false;
    uint32_t lastFallback = kFallbacks;
    uint64_t ngxBefore = 0;   // NGX's DLSS memory when a second-DLSS variant started
    bool ngxBeforeOk = false;
    const char* abortWhy = nullptr;
    std::string aborted;
    hb::Manifest m;
    std::vector<uint32_t> capFrames[hb::kVariants];   // each variant's captured layer frames
};
BenchRun g_bench;
BenchSlot g_benchSlots[kBenchSlots];
BenchQuery g_benchQ[kBenchQueries];
ID3D11Texture2D* g_benchRectStage[2] = {};
hb::Writer g_benchWriter;

bool benchAsleepVariant(int v) { return v == hb::kOrigA || v == hb::kOrigB; }
bool benchAbVariant(int v) { return v == hb::kSepNorefl || v == hb::kSepNopre || v == hb::kSepNorect; }
void benchNoteOff() {
    if (g_bench.phase == BenchPhase::Capture) ++g_bench.offCapture;
}
// What this build runs: the originals, the layer with and without its TAA, the second DLSS whole and as a subrect.
const char* benchNotBuilt(int v) {
    if (v == hb::kNative || v == hb::kNativeFxaa) return "needs shader dump (feasibility builds A and C)";
    return nullptr;
}
uint32_t benchWarm(int v) { return v == hb::kSepNoaa || v == hb::kNative || v == hb::kNativeFxaa ? kBenchWarmShort : kBenchWarmLong; }
bool benchRunning() { return g_bench.phase == BenchPhase::Warm || g_bench.phase == BenchPhase::Capture || g_bench.phase == BenchPhase::Settle; }

const char* fallbackName(uint32_t f) {
    switch (f) {
        case kFbFrameGen: return "frame_gen";
        case kFbOutside: return "outside";
        case kFbMaskPending: return "mask_pending";
        case kFbMaskLarge: return "mask_large";
        case kFbStreak: return "streak";
        default: return "?";
    }
}

// Bytes a pixel, by DXGI format family (4 for anything not listed).
uint32_t formatBytes(DXGI_FORMAT f) {
    const int v = static_cast<int>(f);
    if (v >= 1 && v <= 4) return 16;
    if (v >= 5 && v <= 8) return 12;
    if (v >= 9 && v <= 22) return 8;
    if (v >= 48 && v <= 59) return 2;
    if (v >= 60 && v <= 65) return 1;
    return 4;
}
// The format a capture is written as: a typeless or sRGB view family as the UNORM one tools\eye_inputs.py reads.
DXGI_FORMAT fileFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return f;
    }
}
uint64_t texBytes(ID3D11Texture2D* t) {
    if (!t) return 0;
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    const uint64_t one = static_cast<uint64_t>(d.Width) * d.Height * formatBytes(d.Format) * std::max(1u, d.ArraySize) * std::max(1u, d.SampleDesc.Count);
    return d.MipLevels > 1 ? one * 4 / 3 : one;
}
uint64_t bufBytes(ID3D11Buffer* b) {
    if (!b) return 0;
    D3D11_BUFFER_DESC d{};
    b->GetDesc(&d);
    return d.ByteWidth;
}
// The layer's own GPU memory (its private copies, the eyes' tonemap, cover and masks, the composite's history and out), MB.
double layerVramMb() {
    uint64_t sum = 0;
    for (const auto& m : g_mirrors) sum += texBytes(m.priv);
    for (const auto& e : g_eyes) {
        sum += texBytes(e.tone) + texBytes(e.cover) + texBytes(e.coverZ);
        sum += bufBytes(e.mask) + bufBytes(e.stats);
    }
    for (const auto& c : g_comp) sum += texBytes(c.hist[0]) + texBytes(c.hist[1]) + texBytes(c.out);
    return static_cast<double>(sum) / (1024.0 * 1024.0);
}
// The adapter's local memory in use (context only: the game allocates in parallel), MB; < 0 when it cannot be read.
double dxgiUsageMb() {
    if (!g_device) return -1.0;
    IDXGIDevice* dd = nullptr;
    IDXGIAdapter* ad = nullptr;
    IDXGIAdapter3* ad3 = nullptr;
    double mb = -1.0;
    if (SUCCEEDED(g_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dd))) && dd && SUCCEEDED(dd->GetAdapter(&ad)) && ad &&
        SUCCEEDED(ad->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&ad3))) && ad3) {
        DXGI_QUERY_VIDEO_MEMORY_INFO vmi{};
        if (SUCCEEDED(ad3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vmi))) mb = static_cast<double>(vmi.CurrentUsage) / (1024.0 * 1024.0);
    }
    releaseRef(ad3);
    releaseRef(ad);
    releaseRef(dd);
    return mb;
}
// ---- the variant bench's second DLSS on the hull (dlss_full, dlss_sub) ----
//
// Per eye, in place of the layer's TAA and sharpen: hull_dlss_in_cs writes DLSS inputs at the render size -- the cover's hull
// (its tonemapped bytes, binary alpha) over the eye's own hull-free inputs, its depth (the same reversed Z), and for hull
// pixels the head-only motion of the TAA's reprojection (the hull is rigid with the head's frame), else the eye's own -- then
// a private DLSS (dlaa.cpp dlaaHullEvaluate) upscales them, whole or the bench rect as an NGX subrect, and hull_cov_comp_cs
// lays its output over the submitted eye by the cover's alpha upsampled bilinearly, inside the bench rect only. The resources
// are made on the variant's first frame and released when it ends.
// hlsl-check: kDlssInHlsl hull_dlss_in_cs cs_5_0
// hlsl-check: kCovCompHlsl hull_cov_comp_cs cs_5_0
#define HULL_BENCH_CB_HLSL                                                                                                                  \
    "cbuffer HullLayerTaaCb : register(b0) {\n"                                                                                              \
    "    float4 tanNow; float4 tanPrev; float4 dRow0; float4 dRow1; float4 dRow2;\n"                                                         \
    "    float4 headT; float4 jit; float4 depthAB; int4 sizes; int4 origin;\n"                                                               \
    "};\n"                                                                                                                                   \
    "cbuffer HullBenchCb : register(b1) { int4 rectR; int4 rectO; };\n"
constexpr char kDlssInHlsl[] =
    HULL_BENCH_CB_HLSL
    "Texture2D<float4> cover : register(t0);\n"
    "Texture2D<float> coverDepth : register(t1);\n"
    "Texture2D<float4> worldColour : register(t2);\n"
    "Texture2D<float> worldDepth : register(t3);\n"
    "Texture2D<float2> worldMv : register(t4);\n"
    "RWTexture2D<float4> colOut : register(u0);\n"
    "RWTexture2D<float> depOut : register(u1);\n"
    "RWTexture2D<float2> mvOut : register(u2);\n"
    "[numthreads(8, 8, 1)] void hull_dlss_in_cs(uint3 id : SV_DispatchThreadID) {\n"
    "    int2 rsz = sizes.xy;\n"
    "    int2 p = int2(id.xy) + rectR.xy;\n"
    "    if (any(p >= rectR.xy + rectR.zw) || any(p >= rsz)) return;\n"
    "    float4 c = cover.Load(int3(origin.xy + p, 0));\n"
    "    if (c.a > 0.0) {\n"
    "        float zr = coverDepth.Load(int3(origin.xy + p, 0));\n"
    "        float2 pr = float2(p);\n"
    "        float2 mv = float2(0.0, 0.0);\n"
    "        float3 d;\n"
    "        d.x = tanNow.x + (pr.x + 0.5) / float(rsz.x) * (tanNow.y - tanNow.x);\n"
    "        d.y = tanNow.w - (pr.y + 0.5) / float(rsz.y) * (tanNow.w - tanNow.z);\n"
    "        d.z = -1.0;\n"
    "        float3 dp = float3(dot(dRow0.xyz, d), dot(dRow1.xyz, d), dot(dRow2.xyz, d));\n"
    "        float den = zr - depthAB.x;\n"
    "        if (headT.w != 0.0 && zr > 0.0 && den > 0.0) dp = dp * (depthAB.y / den) + headT.xyz;\n"
    "        if (dp.z < -1e-6) {\n"
    "            float2 pp;\n"
    "            pp.x = (dp.x / -dp.z - tanPrev.x) / (tanPrev.y - tanPrev.x) * float(rsz.x) - 0.5;\n"
    "            pp.y = (tanPrev.w - dp.y / -dp.z) / (tanPrev.w - tanPrev.z) * float(rsz.y) - 0.5;\n"
    "            mv = pp - pr;\n"
    "        }\n"
    "        colOut[p] = float4(c.rgb, 1.0);\n"
    "        depOut[p] = zr;\n"
    "        mvOut[p] = mv;\n"
    "    } else {\n"
    "        colOut[p] = worldColour.Load(int3(p, 0));\n"
    "        depOut[p] = worldDepth.Load(int3(p, 0));\n"
    "        mvOut[p] = worldMv.Load(int3(p, 0));\n"
    "    }\n"
    "}\n";
constexpr char kCovCompHlsl[] =
    HULL_BENCH_CB_HLSL
    "Texture2D<float4> hullOut : register(t0);\n"
    "Texture2D<float4> cover : register(t1);\n"
    "Texture2D<float4> world : register(t2);\n"
    "SamplerState lin : register(s0);\n"
    "RWTexture2D<float4> compOut : register(u0);\n"
    "[numthreads(8, 8, 1)] void hull_cov_comp_cs(uint3 id : SV_DispatchThreadID) {\n"
    "    int2 o = int2(id.xy) + rectO.xy;\n"
    "    if (any(o >= rectO.xy + rectO.zw)) return;\n"
    "    float2 csz;\n"
    "    cover.GetDimensions(csz.x, csz.y);\n"
    "    float2 pr = (float2(o) + 0.5) * float2(sizes.xy) / float2(sizes.zw);\n"
    "    float cov = cover.SampleLevel(lin, (float2(origin.xy) + pr) / csz, 0).a;\n"
    "    float4 w = world.Load(int3(o, 0));\n"
    "    float4 h = hullOut.Load(int3(o, 0));\n"
    "    compOut[o] = float4(lerp(w.rgb, h.rgb, cov), w.a);\n"
    "}\n";
#undef HULL_BENCH_CB_HLSL

struct BenchDlssEye {
    ID3D11Texture2D* col = nullptr;
    ID3D11UnorderedAccessView* colUav = nullptr;
    ID3D11Texture2D* dep = nullptr;
    ID3D11UnorderedAccessView* depUav = nullptr;
    ID3D11Texture2D* mv = nullptr;
    ID3D11UnorderedAccessView* mvUav = nullptr;
    // NGX reads its inputs through shader views of its own, so the inputs need the shader-resource bind flag that
    // makeTexture sets only when it is asked for a view (without it every evaluation failed, 0xBAD00002, flight 2026-11-10).
    ID3D11ShaderResourceView* colSrv = nullptr;
    ID3D11ShaderResourceView* depSrv = nullptr;
    ID3D11ShaderResourceView* mvSrv = nullptr;
    ID3D11Texture2D* out = nullptr;
    ID3D11UnorderedAccessView* outUav = nullptr;
    ID3D11ShaderResourceView* outSrv = nullptr;
    uint32_t w = 0, h = 0, outW = 0, outH = 0;
    // Views over the eye's own inputs, kept while the textures are the same.
    const void* srvOf[3] = {};
    ID3D11ShaderResourceView* srv[3] = {};
};
struct BenchDlssGpu {
    bool tried = false, ok = false;
    ID3D11ComputeShader* inCs = nullptr;
    ID3D11ComputeShader* compCs = nullptr;
    ID3D11Buffer* cb = nullptr;
};
BenchDlssEye g_benchDlssEye[2];
BenchDlssGpu g_benchDlssGpu;

void benchDlssRelease() {
    for (auto& d : g_benchDlssEye) {
        releaseRef(d.colSrv);
        releaseRef(d.depSrv);
        releaseRef(d.mvSrv);
        releaseRef(d.colUav);
        releaseRef(d.col);
        releaseRef(d.depUav);
        releaseRef(d.dep);
        releaseRef(d.mvUav);
        releaseRef(d.mv);
        releaseRef(d.outSrv);
        releaseRef(d.outUav);
        releaseRef(d.out);
        for (auto*& s : d.srv) releaseRef(s);
        d = BenchDlssEye{};
    }
    releaseRef(g_benchDlssGpu.inCs);
    releaseRef(g_benchDlssGpu.compCs);
    releaseRef(g_benchDlssGpu.cb);
    g_benchDlssGpu = BenchDlssGpu{};
    dlaaHullRelease();
}

double benchDlssMb() {
    uint64_t sum = 0;
    for (const auto& d : g_benchDlssEye) sum += texBytes(d.col) + texBytes(d.dep) + texBytes(d.mv) + texBytes(d.out);
    return static_cast<double>(sum) / (1024.0 * 1024.0);
}

bool benchDlssEnsureGpu() {
    BenchDlssGpu& g = g_benchDlssGpu;
    if (g.tried) return g.ok;
    g.tried = true;
    const struct {
        const char* src;
        size_t len;
        const char* entry;
        ID3D11ComputeShader** out;
    } items[2] = {{kDlssInHlsl, sizeof(kDlssInHlsl) - 1, "hull_dlss_in_cs", &g.inCs}, {kCovCompHlsl, sizeof(kCovCompHlsl) - 1, "hull_cov_comp_cs", &g.compCs}};
    for (const auto& it : items) {
        ID3DBlob* code = nullptr;
        std::string why;
        if (!compileShader(it.src, it.len, it.entry, "cs_5_0", &code, &why)) {
            Log::get().note("hull layer: bench: the %s shader did not compile (%s)", it.entry, why.c_str());
            if (code) code->Release();
            return false;
        }
        const HRESULT hr = g_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, it.out);
        code->Release();
        if (FAILED(hr)) return false;
    }
    g.cb = makeConstantBuffer(32);
    g.ok = g.cb != nullptr;
    return g.ok;
}

// A shader view over one of the eye's own inputs (a typeless colour read as UNORM), kept while the texture is the same.
ID3D11ShaderResourceView* benchInputSrv(BenchDlssEye& d, int k, ID3D11Texture2D* tex) {
    if (!tex) return nullptr;
    if (d.srv[k] && d.srvOf[k] == tex) return d.srv[k];
    releaseRef(d.srv[k]);
    d.srvOf[k] = nullptr;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM
              : td.Format == DXGI_FORMAT_R32_TYPELESS      ? DXGI_FORMAT_R32_FLOAT
              : td.Format == DXGI_FORMAT_R16G16_TYPELESS   ? DXGI_FORMAT_R16G16_FLOAT
                                                           : td.Format;
    vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipLevels = 1;
    if (FAILED(g_device->CreateShaderResourceView(tex, &vd, &d.srv[k]))) {
        d.srv[k] = nullptr;
        return nullptr;
    }
    d.srvOf[k] = tex;
    return d.srv[k];
}

bool benchDlssTargets(BenchDlssEye& d, uint32_t w, uint32_t h, uint32_t outW, uint32_t outH) {
    if (d.out && d.w == w && d.h == h && d.outW == outW && d.outH == outH) return true;
    const void* keepOf[3] = {d.srvOf[0], d.srvOf[1], d.srvOf[2]};
    ID3D11ShaderResourceView* keep[3] = {d.srv[0], d.srv[1], d.srv[2]};
    releaseRef(d.colSrv);
    releaseRef(d.depSrv);
    releaseRef(d.mvSrv);
    releaseRef(d.colUav);
    releaseRef(d.col);
    releaseRef(d.depUav);
    releaseRef(d.dep);
    releaseRef(d.mvUav);
    releaseRef(d.mv);
    releaseRef(d.outSrv);
    releaseRef(d.outUav);
    releaseRef(d.out);
    bool ok = makeTexture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, true, &d.col, &d.colSrv, &d.colUav) &&
              makeTexture(w, h, DXGI_FORMAT_R32_FLOAT, true, &d.dep, &d.depSrv, &d.depUav) &&
              makeTexture(w, h, DXGI_FORMAT_R16G16_FLOAT, true, &d.mv, &d.mvSrv, &d.mvUav) &&
              makeTexture(outW, outH, DXGI_FORMAT_R8G8B8A8_UNORM, true, &d.out, &d.outSrv, &d.outUav);
    for (int k = 0; k < 3; ++k) {
        d.srvOf[k] = keepOf[k];
        d.srv[k] = keep[k];
    }
    if (!ok) return false;
    d.w = w;
    d.h = h;
    d.outW = outW;
    d.outH = outH;
    return true;
}

const char* g_benchDlssFail = nullptr;   // the second DLSS's refusal this frame (the boundary refuses the variant with it)
bool g_benchDlssReset[2] = {};           // the variant's first evaluation an eye restarts the hull feature's history

// The hull through the second DLSS into c.out (the composite's out, copied over the eye when applied, as the TAA's is). False:
// not run (the TAA path runs this frame and g_benchDlssFail says why).
bool benchHullDlss(ID3D11DeviceContext* ctx, int eye, const Eye& e, const Comp& c, const HullLayerCompositeInputs& in) {
    static char fail[192];
    auto refuse = [&](const char* why) {
        _snprintf_s(fail, sizeof fail, _TRUNCATE, "%s", why ? why : "?");
        g_benchDlssFail = fail;
        return false;
    };
    if (!in.dlColour || !in.dlDepth || !in.dlMv) return refuse("the eye's DLSS inputs were not handed over");
    if (!benchDlssEnsureGpu()) return refuse("the bench's DLSS shaders could not be made");
    BenchDlssEye& d = g_benchDlssEye[eye];
    if (!benchDlssTargets(d, in.renderW, in.renderH, in.outW, in.outH)) return refuse("the bench's DLSS targets could not be made");
    ID3D11ShaderResourceView* colSrv = benchInputSrv(d, 0, in.dlColour);
    ID3D11ShaderResourceView* depSrv = benchInputSrv(d, 1, in.dlDepth);
    ID3D11ShaderResourceView* mvSrv = benchInputSrv(d, 2, in.dlMv);
    if (!colSrv || !depSrv || !mvSrv) return refuse("no shader view over the eye's DLSS inputs");
    const BenchRun& b = g_bench;
    const bool sub = g_benchDlss == hb::kDlssSub;
    if (sub) {
        // NGX maps the render subrect to the output subrect by the feature's scale: both corners must be whole pixels.
        const uint64_t rw = in.renderW, rh = in.renderH;
        if ((uint64_t{b.rectR[0]} * in.outW) % rw || (uint64_t{b.rectR[1]} * in.outH) % rh || (uint64_t{b.rectR[2]} * in.outW) % rw ||
            (uint64_t{b.rectR[3]} * in.outH) % rh)
            return refuse("the bench rect does not map to whole output pixels (the render to output ratio)");
    }
    // The rect: the bench's for the subrect; the whole eye for the full frame's inputs (the composite stays in the bench rect).
    const int32_t rr[4] = {sub ? static_cast<int32_t>(b.rectR[0]) : 0, sub ? static_cast<int32_t>(b.rectR[1]) : 0,
                           sub ? static_cast<int32_t>(b.rectR[2]) : static_cast<int32_t>(in.renderW),
                           sub ? static_cast<int32_t>(b.rectR[3]) : static_cast<int32_t>(in.renderH)};
    const int32_t cbData[8] = {rr[0], rr[1], rr[2], rr[3], static_cast<int32_t>(b.rectO[0]), static_cast<int32_t>(b.rectO[1]),
                               static_cast<int32_t>(b.rectO[2]), static_cast<int32_t>(b.rectO[3])};
    vScreenUpdateSubresourceRaw(ctx, g_benchDlssGpu.cb, 0, nullptr, cbData, 0, 0);
    CsStageSave save;
    save.save(ctx);
    ID3D11Buffer* cbs[2] = {g_cgpu.taaCb, g_benchDlssGpu.cb};
    ID3D11UnorderedAccessView* none[CsStageSave::kUavs] = {};
    ID3D11ShaderResourceView* noSrv[5] = {};
    {
        ID3D11ShaderResourceView* srvs[5] = {e.coverSrv, e.coverZSrv, colSrv, depSrv, mvSrv};
        ID3D11UnorderedAccessView* uavs[CsStageSave::kUavs] = {d.colUav, d.depUav, d.mvUav};
        ctx->CSSetUnorderedAccessViews(0, CsStageSave::kUavs, uavs, nullptr);
        ctx->CSSetShader(g_benchDlssGpu.inCs, nullptr, 0);
        ctx->CSSetShaderResources(0, 5, srvs);
        ctx->CSSetConstantBuffers(0, 2, cbs);
        ctx->Dispatch((static_cast<uint32_t>(rr[2]) + 7u) / 8u, (static_cast<uint32_t>(rr[3]) + 7u) / 8u, 1);
        ctx->CSSetShaderResources(0, 5, noSrv);
        ctx->CSSetUnorderedAccessViews(0, CsStageSave::kUavs, none, nullptr);
    }
    save.restore(ctx);
    stageClose(ctx);   // the layer's taa stage holds the prep and the composite; the NGX evaluation is timed apart (hull_dlss)
    DlaaSubrect sr;
    sr.renderX = b.rectR[0];
    sr.renderY = b.rectR[1];
    sr.renderW = b.rectR[2];
    sr.renderH = b.rectR[3];
    sr.outX = b.rectO[0];
    sr.outY = b.rectO[1];
    const char* why = nullptr;
    double createMs = 0.0;
    const bool reset = g_benchDlssReset[eye];
    const bool ok = hullBenchTimed(ctx, 1, [&] {
        return dlaaHullEvaluate(ctx, eye, d.col, d.dep, d.mv, d.out, in.renderW, in.renderH, in.outW, in.outH, in.jitterX, in.jitterY, reset,
                                sub ? &sr : nullptr, &why, &createMs);
    });
    stageOpen(ctx, kTsTaa, false);
    if (!ok) return refuse(why);
    g_benchDlssReset[eye] = false;
    if (createMs > 0.0) Log::get().note("hull layer: bench %s: %s eye %d feature made in %.0f ms", b.stampA.c_str(), hb::variantId(g_benchDlss), eye, createMs);
    // The composite: the world copied whole, the hull's DLSS laid over it by coverage inside the bench rect.
    vScreenCopyResourceRaw(ctx, c.out, in.output);
    save.save(ctx);
    {
        ID3D11ShaderResourceView* srvs[3] = {d.outSrv, e.coverSrv, in.outputSrv};
        ID3D11UnorderedAccessView* uavs[CsStageSave::kUavs] = {c.outUav};
        ctx->CSSetUnorderedAccessViews(0, CsStageSave::kUavs, uavs, nullptr);
        ctx->CSSetShader(g_benchDlssGpu.compCs, nullptr, 0);
        ctx->CSSetShaderResources(0, 3, srvs);
        ctx->CSSetConstantBuffers(0, 2, cbs);
        ctx->CSSetSamplers(0, 1, &g_cgpu.linear);
        ctx->Dispatch((b.rectO[2] + 7u) / 8u, (b.rectO[3] + 7u) / 8u, 1);
        ctx->CSSetShaderResources(0, 3, noSrv);
        ctx->CSSetUnorderedAccessViews(0, CsStageSave::kUavs, none, nullptr);
    }
    save.restore(ctx);
    return true;
}

// The head's rotation in a 3x3 delta (row-major), degrees; < 0 without one.
float headRotDeg(const float* d) {
    if (!d) return -1.0f;
    const double c = std::max(-1.0, std::min(1.0, (static_cast<double>(d[0]) + d[4] + d[8] - 1.0) * 0.5));
    return static_cast<float>(std::acos(c) * 57.29577951308232);
}

void benchToast(const char* text) {
    static char toast[112];   // menuShowNotice keeps the pointer (device_hook.cpp's frame generation toast does the same)
    _snprintf_s(toast, sizeof toast, _TRUNCATE, "%s", text);
    menuShowNotice(toast);
}

// A staging copy of src's box, queued for the writer as dir\name once mapped. False (counted) when no copy is free or the
// source cannot be copied.
bool benchStage(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const std::string& name,
                uint32_t frame, uint32_t eye) {
    if (!ctx || !src || !g_device || !w || !h) return false;
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    if (d.SampleDesc.Count != 1 || x >= d.Width || y >= d.Height) {
        ++g_bench.stageFails;
        return false;
    }
    w = std::min(w, d.Width - x);
    h = std::min(h, d.Height - y);
    BenchSlot* s = nullptr;
    for (auto& c : g_benchSlots)   // a free copy of this size and format, else an empty one, else any free one
        if (!c.pending && c.tex && c.w == w && c.h == h && c.fmt == d.Format) {
            s = &c;
            break;
        }
    for (int pass = 0; pass < 2 && !s; ++pass)
        for (auto& c : g_benchSlots)
            if (!c.pending && (pass == 1 || !c.tex)) {
                s = &c;
                break;
            }
    if (!s) {
        ++g_bench.stageFails;
        return false;
    }
    if (s->tex && (s->w != w || s->h != h || s->fmt != d.Format)) releaseRef(s->tex);
    if (!s->tex) {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = w;
        sd.Height = h;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = d.Format;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &s->tex)) || !s->tex) {
            s->tex = nullptr;
            ++g_bench.stageFails;
            return false;
        }
        s->w = w;
        s->h = h;
        s->fmt = d.Format;
    }
    const D3D11_BOX box{x, y, 0u, x + w, y + h, 1u};
    vScreenCopySubresourceRegionRaw(ctx, s->tex, 0, src, &box);
    s->bpp = formatBytes(d.Format);
    s->fileFmt = fileFormat(d.Format);
    s->pending = true;
    s->at = g_frame;
    s->frame = frame;
    s->eye = eye;
    s->path = g_bench.dir + L"\\" + std::wstring(name.begin(), name.end());
    return true;
}

uint32_t benchFreeSlots() {
    uint32_t n = 0;
    for (const auto& s : g_benchSlots) n += s.pending ? 0u : 1u;
    return n;
}

// The staged copies old enough mapped (without waiting until kBenchMapForce) and handed to the writer; `all`: every one, waiting.
void benchMapSlots(ID3D11DeviceContext* ctx, bool all) {
    for (auto& s : g_benchSlots) {
        if (!s.pending || !s.tex) continue;
        const uint32_t age = g_frame - s.at;
        if (!all && age < kBenchMapLag) continue;
        D3D11_MAPPED_SUBRESOURCE map{};
        const UINT flags = all || age >= kBenchMapForce ? 0u : static_cast<UINT>(D3D11_MAP_FLAG_DO_NOT_WAIT);
        const HRESULT hr = ctx->Map(s.tex, 0, D3D11_MAP_READ, flags, &map);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        s.pending = false;
        if (FAILED(hr)) {
            ++g_bench.stageFails;
            continue;
        }
        const uint32_t rowBytes = s.w * s.bpp;
        std::vector<uint8_t> data(static_cast<size_t>(rowBytes) * s.h);
        for (uint32_t y = 0; y < s.h; ++y)
            std::memcpy(data.data() + static_cast<size_t>(y) * rowBytes, static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch, rowBytes);
        ctx->Unmap(s.tex, 0);
        g_benchWriter.push(s.path, s.w, s.h, static_cast<uint32_t>(s.fileFmt), rowBytes, s.frame, s.eye, std::move(data));
    }
}

void benchCollectQueries(ID3D11DeviceContext* ctx) {
    for (auto& q : g_benchQ) {
        if (!q.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 a = 0, b = 0;
        if (ctx->GetData(q.dj, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ctx->GetData(q.t0, &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || ctx->GetData(q.t1, &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        q.pending = false;
        if (!dj.Disjoint && dj.Frequency && b >= a)
            g_bench.times.push_back({q.frame, q.kind, static_cast<double>(b - a) * 1000.0 / static_cast<double>(dj.Frequency)});
    }
}

bool benchQueriesPending() {
    for (const auto& q : g_benchQ)
        if (q.pending || q.open) return true;
    return false;
}

void benchReleaseGpu() {
    for (auto& s : g_benchSlots) {
        releaseRef(s.tex);
        s = BenchSlot{};
    }
    for (auto& q : g_benchQ) {
        releaseRef(q.dj);
        releaseRef(q.t0);
        releaseRef(q.t1);
        q = BenchQuery{};
    }
    for (auto*& t : g_benchRectStage) releaseRef(t);
}

// Restore: the configured mode back and the bench's state off the layer; the variants not finished say why. Then the drain.
void benchRestore(const char* abortReason) {
    BenchRun& b = g_bench;
    g_benchSleep = false;
    g_benchNoAa = g_benchNoRefl = g_benchNoPre = g_benchNoRect = false;
    g_benchDlss = 0;
    g_benchTimeAll = false;
    benchDlssRelease();
    b.finalWant[0] = b.finalWant[1] = false;
    for (auto& c : g_comp) c.have = false;   // the layer's TAA history starts again (one history reset an eye)
    if (b.seqStarted && g_seq.active && !g_seq.end) g_seq.end = "the variant bench ended";
    if (abortReason) {
        b.aborted = abortReason;
        b.m.aborted = abortReason;
        for (int v = 0; v < hb::kVariants; ++v) {
            hb::VariantRec& r = b.m.variants[v];
            if (v == b.var && benchRunning()) {
                r.status = "aborted";
                r.reason = abortReason;
            } else if (v > b.var && !benchNotBuilt(v)) {
                r.status = "aborted";
                r.reason = "not run (the bench aborted)";
            }
        }
    }
    b.phase = BenchPhase::Drain;
    b.capIndex = -1;
    b.drainStartMs = GetTickCount64();
    b.drainFrames = 0;
}

// The next variant from v on: the ones this build does not run are refused in the manifest; the rest start warming.
void benchStartVariant(int v) {
    BenchRun& b = g_bench;
    for (; v < hb::kVariants; ++v) {
        const char* why = benchNotBuilt(v);
        if (!why) break;
        b.m.variants[v].status = "refused";
        b.m.variants[v].reason = why;
    }
    b.var = v;
    if (v >= hb::kVariants) {
        benchRestore(nullptr);
        return;
    }
    hb::VariantRec& r = b.m.variants[v];
    r = hb::VariantRec{};
    r.status = "running";
    b.capFrames[v].clear();
    b.phase = BenchPhase::Warm;
    b.left = benchWarm(v);
    b.capIndex = -1;
    b.restarts[v] = 0;
    b.offCapture = 0;
    if (v == hb::kDlssFull || v == hb::kDlssSub) {
        dlaaHullRelease();   // never two hull pairs at once: the full pair goes before the subrect pair is made
        g_benchDlssReset[0] = g_benchDlssReset[1] = true;
        g_benchDlssFail = nullptr;
        b.ngxBeforeOk = dlaaVramBytes(&b.ngxBefore);
    }
}

// The variant ends: ok with its MB, or refused (why). The next one starts.
void benchEndVariant(const char* refusedWhy) {
    BenchRun& b = g_bench;
    hb::VariantRec& r = b.m.variants[b.var];
    b.finalWant[0] = b.finalWant[1] = false;
    if (refusedWhy) {
        r.status = "refused";
        r.reason = refusedWhy;
        r.frames.clear();
        b.capFrames[b.var].clear();
    } else {
        r.status = "ok";
        const bool dlss = b.var == hb::kDlssFull || b.var == hb::kDlssSub;
        r.mbOwn.have = true;
        r.mbOwn.v = benchAsleepVariant(b.var) ? 0.0 : layerVramMb() + (dlss ? benchDlssMb() : 0.0);
        uint64_t ngxNow = 0;
        r.mbNgx.have = !dlss || (b.ngxBeforeOk && dlaaVramBytes(&ngxNow));   // no second DLSS in the others: none of the hull's
        r.mbNgx.v = dlss && r.mbNgx.have ? (static_cast<double>(ngxNow) - static_cast<double>(b.ngxBefore)) / (1024.0 * 1024.0) : 0.0;
        const double dx = dxgiUsageMb();
        r.mbDxgi.have = dx >= 0.0;
        r.mbDxgi.v = dx;
        if (benchAbVariant(b.var)) {   // the A/B row says whether its feature was in fact off (else it is sep_taa again)
            char why[160];
            if (b.offCapture)
                _snprintf_s(why, sizeof why, _TRUNCATE, "feature off %u times in the capture", b.offCapture);
            else
                _snprintf_s(why, sizeof why, _TRUNCATE, "NOT SWITCHED OFF: the feature never ran in the capture (this row is sep_taa)");
            r.reason = why;
        }
    }
    if (b.var == hb::kDlssFull || b.var == hb::kDlssSub) benchDlssRelease();   // its features and targets go with it
    benchStartVariant(b.var + 1);
}

// A capture frame was lost (no copy, a fallback, the composite not applied): the run of 8 starts again after a short re-warm.
void benchRestart(const char* why) {
    BenchRun& b = g_bench;
    hb::VariantRec& r = b.m.variants[b.var];
    r.frames.clear();
    b.capFrames[b.var].clear();
    b.finalWant[0] = b.finalWant[1] = false;
    if (++b.restarts[b.var] > kBenchRestartMax) {
        static char why2[96];
        _snprintf_s(why2, sizeof why2, _TRUNCATE, "the capture restarted %u times (%s)", kBenchRestartMax, why);
        benchEndVariant(why2);
        return;
    }
    b.phase = BenchPhase::Warm;
    b.left = kBenchWarmShort;
    b.capIndex = -1;
    b.offCapture = 0;
}

// The next frame's flags from the phase: the layer asleep for the originals, the TAA's history off for sep_noaa.
void benchApplyFrameFlags() {
    const bool run = benchRunning();
    g_benchSleep = run && benchAsleepVariant(g_bench.var);
    g_benchNoAa = run && g_bench.var == hb::kSepNoaa;
    g_benchNoRefl = run && g_bench.var == hb::kSepNorefl;
    g_benchNoPre = run && g_bench.var == hb::kSepNopre;
    g_benchNoRect = run && g_bench.var == hb::kSepNorect;
    g_benchDlss = run && (g_bench.var == hb::kDlssFull || g_bench.var == hb::kDlssSub) ? g_bench.var : 0;
}

// The rect from the covers' extent: grown by kBenchGrow output px, aligned so that render and output corners are whole pixels
// in both (x4/3: render multiples of 3), fixed for the run. False when no hull was seen.
bool benchMakeRect() {
    BenchRun& b = g_bench;
    if (!b.boxAny || !b.renderW || !b.renderH || !b.outW || !b.outH) return false;
    b.align = 1;
    for (uint32_t k = 1; k <= 8; ++k)
        if ((static_cast<uint64_t>(k) * b.outW) % b.renderW == 0 && (static_cast<uint64_t>(k) * b.outH) % b.renderH == 0) {
            b.align = k;
            break;
        }
    const uint32_t k = b.align;
    const uint32_t gx = (kBenchGrow * b.renderW + b.outW - 1) / b.outW, gy = (kBenchGrow * b.renderH + b.outH - 1) / b.outH;
    uint32_t x0 = b.box[0] > gx ? b.box[0] - gx : 0u, y0 = b.box[1] > gy ? b.box[1] - gy : 0u;
    uint32_t x1 = std::min(b.box[2] + gx, b.renderW), y1 = std::min(b.box[3] + gy, b.renderH);
    x0 -= x0 % k;
    y0 -= y0 % k;
    x1 = std::min((x1 + k - 1) / k * k, b.renderW - b.renderW % k);
    y1 = std::min((y1 + k - 1) / k * k, b.renderH - b.renderH % k);
    if (x1 <= x0 || y1 <= y0) return false;
    const uint32_t r[4] = {x0, y0, x1 - x0, y1 - y0};
    std::memcpy(b.rectR, r, sizeof r);
    b.rectO[0] = static_cast<uint32_t>(static_cast<uint64_t>(x0) * b.outW / b.renderW);
    b.rectO[1] = static_cast<uint32_t>(static_cast<uint64_t>(y0) * b.outH / b.renderH);
    b.rectO[2] = std::min(static_cast<uint32_t>(static_cast<uint64_t>(x1) * b.outW / b.renderW), b.outW) - b.rectO[0];
    b.rectO[3] = std::min(static_cast<uint32_t>(static_cast<uint64_t>(y1) * b.outH / b.renderH), b.outH) - b.rectO[1];
    return b.rectO[2] > 0 && b.rectO[3] > 0;
}

// The rect step's cover copies, read once they are old enough (alpha above 0 in the eye region).
void benchReadRect(ID3D11DeviceContext* ctx) {
    BenchRun& b = g_bench;
    for (int i = 0; i < 2; ++i) {
        if (b.rectState[i] != 1 || !g_benchRectStage[i] || g_frame - b.rectAt[i] < kBenchMapLag) continue;
        D3D11_MAPPED_SUBRESOURCE map{};
        const HRESULT hr = ctx->Map(g_benchRectStage[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        b.rectState[i] = 2;
        if (FAILED(hr)) continue;
        D3D11_TEXTURE2D_DESC d{};
        g_benchRectStage[i]->GetDesc(&d);
        for (uint32_t y = 0; y < d.Height; ++y) {
            const uint8_t* row = static_cast<const uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch;
            for (uint32_t x = 0; x < d.Width; ++x)
                if (row[4 * x + 3]) {
                    if (!b.boxAny) {
                        b.box[0] = b.box[2] = x;
                        b.box[1] = b.box[3] = y;
                        b.boxAny = true;
                    }
                    b.box[0] = std::min(b.box[0], x);
                    b.box[1] = std::min(b.box[1], y);
                    b.box[2] = std::max(b.box[2], x + 1);
                    b.box[3] = std::max(b.box[3], y + 1);
                }
        }
        ctx->Unmap(g_benchRectStage[i], 0);
    }
}

// Why the bench must stop now, or null. Read at every boundary while variants run.
const char* benchAbortReason() {
    BenchRun& b = g_bench;
    if (b.abortWhy) return b.abortWhy;
    if (!detail::g_hullLayerSeparate) return "diag.hull_layer is no longer separate";
    if (frameGenOn()) return "frame generation turned on";
    if (g_busy) return "the busy governor put the layer to sleep";
    if (!runtimeVrProfile() || nativeOnFootActive()) return "left the cockpit (the flat profile, or on foot)";
    if (journalSeatKind() != b.seat) return "the vehicle changed";
    if (journalVehicleKnown() && !journalVehicle()) return "the game's menu or a loading screen";
    if (journalInJumpTunnel()) return "hyperspace";
    if (g_dump != Dump::Idle) return "an eye dump was armed";
    if (!g_device || FAILED(g_device->GetDeviceRemovedReason())) return "the device was lost";
    const ULONGLONG now = GetTickCount64();
    if (b.runStartMs ? now - b.runStartMs > kBenchRunMs : now - b.pressMs > kBenchRunMs) return "over 20 s";
    return nullptr;
}

// The manifest, the per-variant lines and the done/aborted line, the toast; everything the bench held released.
void benchFinish() {
    BenchRun& b = g_bench;
    // Medians over each variant's capture frames, both eyes summed: the stage times, the DLSS times, the frame span, the CPU.
    for (int v = 0; v < hb::kVariants; ++v) {
        hb::VariantRec& r = b.m.variants[v];
        const auto& frames = b.capFrames[v];
        if (frames.empty()) continue;
        auto med = [&](int kind) {
            std::vector<double> vals;
            for (uint32_t f : frames) {
                double sum = 0.0;
                bool any = false;
                for (const auto& t : b.times)
                    if (t.frame == f && t.kind == kind) {
                        sum += t.ms;
                        any = true;
                    }
                if (any) vals.push_back(sum);
            }
            return hb::median(vals);
        };
        const bool asleep = benchAsleepVariant(v);
        for (int k = 0; k < hb::kLayerStages; ++k) {
            r.layerStage[k] = med(kBtStage + k);
            if (!r.layerStage[k].have && asleep) r.layerStage[k] = hb::Num{true, 0.0};   // asleep: the layer did no GPU work
        }
        r.mainDlss = med(kBtMainDlss);
        r.hullDlss = med(kBtHullDlss);
        if (!r.hullDlss.have && v != hb::kDlssFull && v != hb::kDlssSub) r.hullDlss = hb::Num{true, 0.0};
        r.frameSpan = med(kBtSpan);
        r.cpuMs = med(kBtCpu);
    }
    b.m.evicted = g_benchEvictions > 0;
    const std::string manifest = hb::manifestName(b.stampA);
    const std::wstring mpath = b.dir + L"\\" + std::wstring(manifest.begin(), manifest.end());
    FILE* f = nullptr;
    bool wrote = false;
    if (_wfopen_s(&f, mpath.c_str(), L"wb") == 0 && f) {
        const std::string text = hb::manifestJson(b.m);
        wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        wrote = (std::fclose(f) == 0) && wrote;
    }
    int ok = 0, refused = 0, aborted = 0;
    for (int v = 0; v < hb::kVariants; ++v) {
        const hb::VariantRec& r = b.m.variants[v];
        if (r.status == "ok")
            ++ok;
        else if (r.status == "refused")
            ++refused;
        else
            ++aborted;
        double layer = -1.0;
        for (const auto& s : r.layerStage)
            if (s.have) layer = (layer < 0.0 ? 0.0 : layer) + s.v;
        Log::get().note("hull layer: bench %s variant %s: %s frames=%u warm=%u fallback=%u restarts=%u layer_ms=%.3f hull_dlss_ms=%.3f "
                        "main_dlss_ms=%.3f frame_ms=%.3f cpu_ms=%.3f mb_own=%.1f mb_ngx=%.1f mb_dxgi=%.0f final=%u reason=%s",
                        b.stampA.c_str(), hb::variantId(v), r.status.c_str(), static_cast<unsigned>(r.frames.size()), r.warmFrames, r.fallbackFrames,
                        b.restarts[v], layer, r.hullDlss.have ? r.hullDlss.v : -1.0, r.mainDlss.have ? r.mainDlss.v : -1.0,
                        r.frameSpan.have ? r.frameSpan.v : -1.0, r.cpuMs.have ? r.cpuMs.v : -1.0, r.mbOwn.have ? r.mbOwn.v : -1.0,
                        r.mbNgx.have ? r.mbNgx.v : -1.0, r.mbDxgi.have ? r.mbDxgi.v : -1.0,
                        static_cast<unsigned>(!r.finalFiles[0].empty()) + static_cast<unsigned>(!r.finalFiles[1].empty()), r.reason.empty() ? "-" : r.reason.c_str());
    }
    const double mb = static_cast<double>(g_benchWriter.bytesWritten()) / (1024.0 * 1024.0);
    const bool drained = g_benchWriter.idle();
    if (b.aborted.empty()) {
        Log::get().note("hull layer: bench %s done: ok=%d refused=%d aborted=%d files=%u failed=%u stage_fails=%u mb=%.1f evicted=%u manifest=%s%s "
                        "(python tools\\hull_bench_report.py <edvr_logs>\\eyes\\eye_%s)",
                        b.stampA.c_str(), ok, refused, aborted, g_benchWriter.written(), g_benchWriter.failed(), b.stageFails, mb, g_benchEvictions,
                        wrote ? "written" : "write_failed", drained ? "" : " (the writer had not drained)", b.stampA.c_str());
        char text[96];
        _snprintf_s(text, sizeof text, _TRUNCATE, "Hull bench done: %d/%d variants (%d refused)", ok, hb::kVariants, refused + aborted);
        benchToast(text);
    } else {
        Log::get().note("hull layer: bench %s aborted: %s; ok=%d refused=%d aborted=%d files=%u failed=%u stage_fails=%u mb=%.1f evicted=%u manifest=%s",
                        b.stampA.c_str(), b.aborted.c_str(), ok, refused, aborted, g_benchWriter.written(), g_benchWriter.failed(), b.stageFails, mb,
                        g_benchEvictions, wrote ? "written" : "write_failed");
        char text[112];
        _snprintf_s(text, sizeof text, _TRUNCATE, "Hull bench aborted: %s", b.aborted.c_str());
        benchToast(text);
    }
    if (drained) g_benchWriter.stop();   // else the thread finishes its queue and is joined at the next bench or at shutdown
    g_benchWriter.resetCounts();
    benchReleaseGpu();
    g_benchSleep = g_benchNoAa = g_benchTimeAll = g_benchNoRefl = g_benchNoPre = g_benchNoRect = false;
    g_benchDlss = 0;
    detail::g_hullBenchOn = false;
    g_bench.phase = BenchPhase::Idle;
}

// At the boundary, inside the layer's own scope: the staged copies mapped, the GPU times read, the rect's covers read, the
// frame's span and CPU time noted.
void benchCollect(ID3D11DeviceContext* ctx) {
    BenchRun& b = g_bench;
    benchMapSlots(ctx, false);
    benchCollectQueries(ctx);
    if (b.phase == BenchPhase::Rect) benchReadRect(ctx);
    if (b.phase == BenchPhase::Capture || b.phase == BenchPhase::Settle) {
        double ms = 0.0;
        unsigned n = 0;
        if (gpuCensusFrameSpan(&ms, &n)) b.times.push_back({g_frame - 1, kBtSpan, ms});
    }
    int64_t ticks = 0;
    for (const auto t : g_st.cpuTicks) ticks += t;
    const int64_t d = ticks >= b.cpuLast ? ticks - b.cpuLast : ticks;   // the 30 s line zeroes them
    b.cpuLast = ticks;
    LARGE_INTEGER fq{};
    QueryPerformanceFrequency(&fq);
    if (fq.QuadPart) b.times.push_back({g_frame - 1, kBtCpu, static_cast<double>(d) * 1000.0 / static_cast<double>(fq.QuadPart)});
}

// At the boundary before decideNext: the frame that ended judged, the phase advanced, the next frame's flags set.
void benchStep() {
    BenchRun& b = g_bench;
    if (b.phase != BenchPhase::Drain && b.phase != BenchPhase::Idle) {
        if (const char* why = benchAbortReason()) {
            benchRestore(why);
            benchApplyFrameFlags();
            return;
        }
    }
    if (g_benchDlssFail && benchRunning() && (b.var == hb::kDlssFull || b.var == hb::kDlssSub)) {
        const char* why = g_benchDlssFail;
        g_benchDlssFail = nullptr;
        benchEndVariant(why);   // NGX refused the second feature, the subrect or an evaluation: the variant is refused, the bench goes on
        benchApplyFrameFlags();
        b.staged0 = b.applied0 = false;
        return;
    }
    const bool asleep = benchAsleepVariant(b.var);
    const bool good = asleep || (b.discarded && b.applied0);
    switch (b.phase) {
        case BenchPhase::Idle: break;
        case BenchPhase::Rect:
            if (b.rectState[0] == 2 && b.rectState[1] == 2) {
                if (!b.boxAny || !benchMakeRect()) {
                    benchRestore("no_hull_visible");
                    break;
                }
                b.m.render[0] = b.renderW;
                b.m.render[1] = b.renderH;
                b.m.output[0] = b.outW;
                b.m.output[1] = b.outH;
                std::memcpy(b.m.rectRender, b.rectR, sizeof b.rectR);
                std::memcpy(b.m.rectOutput, b.rectO, sizeof b.rectO);
                const DlssMode mode = dlssModeByRatio(b.renderW, b.outW);
                b.m.dlssQuality = b.renderW == b.outW ? "dlaa" : kDlssModeNames[static_cast<int>(mode)];
                Log::get().note("hull layer: bench %s rect: render %u,%u %ux%u -> output %u,%u %ux%u (eye %ux%u -> %ux%u, aligned to %u render px)",
                                b.stampA.c_str(), b.rectR[0], b.rectR[1], b.rectR[2], b.rectR[3], b.rectO[0], b.rectO[1], b.rectO[2], b.rectO[3],
                                b.renderW, b.renderH, b.outW, b.outH, b.align);
                b.phase = BenchPhase::Quiet;
            } else if (++b.rectFrames > kBenchRectFrames) {
                benchRestore(b.sawOther && !b.sawDlss ? "the eyes' upscaler is not DLSS"
                                                     : "no cover from the layer (the own ship not composited on both eyes)");
            }
            break;
        case BenchPhase::Quiet:
            if (GetTickCount64() - b.pressMs >= kBenchQuietMs) {
                b.runStartMs = GetTickCount64();
                benchStartVariant(0);
            }
            break;
        case BenchPhase::Warm:
            if (good) {
                ++b.m.variants[b.var].warmFrames;
                if (b.left) --b.left;
            }
            // dump v2's TAA sequence over sep_taa's last 8 warm frames and its 8 captures (tools\hull_taa_sim.py runs on it)
            if (b.var == hb::kSepTaa && b.left == static_cast<uint32_t>(hb::kCaptureFrames) && !b.seqStarted && !g_seq.active) {
                wcsncpy_s(g_dumpStamp, b.stamp, _TRUNCATE);
                seqStart();   // from the frame about to run
                b.seqStarted = true;
            }
            if (!b.left) {
                // A capture run needs its copies free and the writer under half its budget; else the warm holds a frame.
                if (benchFreeSlots() < kBenchFreeToCapture || g_benchWriter.queuedBytes() > hb::Writer::kQueueBudget / 2) {
                    b.left = 1;
                } else {
                    b.phase = BenchPhase::Capture;
                    b.capIndex = 0;
                    b.finalWant[0] = b.finalWant[1] = true;
                }
            }
            break;
        case BenchPhase::Capture:
            if (good && b.staged0) {
                hb::VariantRec& r = b.m.variants[b.var];
                r.frames.push_back(b.pendingFrame);
                b.capFrames[b.var].push_back(b.pendingFrame.layerFrame);
                if (++b.capIndex >= hb::kCaptureFrames) {
                    b.phase = BenchPhase::Settle;
                    b.left = kBenchSettleFrames;
                    b.capIndex = -1;
                }
            } else {
                benchRestart(!b.staged0 ? "no copy of the eye" : "the composite was not applied");
            }
            break;
        case BenchPhase::Settle:
            if (b.left) --b.left;
            if (!b.left) benchEndVariant(nullptr);
            break;
        case BenchPhase::Drain: break;   // benchDrainStep's
    }
    b.staged0 = b.applied0 = false;
    b.pendingFrame = hb::Frame{};
    benchApplyFrameFlags();
}

// After decideNext (the next frame's discard): a fallback frame in a separate variant is not captured; it extends the warm, or
// restarts a capture run, and past kBenchFallbackMax the variant is refused.
void benchDecided(uint32_t why) {
    BenchRun& b = g_bench;
    b.discarded = g_discard;
    if (!(b.phase == BenchPhase::Warm || b.phase == BenchPhase::Capture) || benchAsleepVariant(b.var) || g_discard) return;
    hb::VariantRec& r = b.m.variants[b.var];
    ++r.fallbackFrames;
    b.lastFallback = why;
    if (r.fallbackFrames > kBenchFallbackMax) {
        static char reason[64];
        _snprintf_s(reason, sizeof reason, _TRUNCATE, "fallback %s", fallbackName(why));
        benchEndVariant(reason);
    } else if (b.phase == BenchPhase::Capture) {
        benchRestart("a fallback frame");
    }
    benchApplyFrameFlags();
    if (g_benchSleep) g_discard = false;   // the next variant is an original: asleep from this frame
}

void benchDrainStep(ID3D11DeviceContext* ctx) {
    BenchRun& b = g_bench;
    ++b.drainFrames;
    const bool late = GetTickCount64() - b.drainStartMs > kBenchDrainMs;
    if (late) benchMapSlots(ctx, true);
    bool slots = false;
    for (const auto& s : g_benchSlots) slots = slots || s.pending;
    const bool queries = benchQueriesPending() && b.drainFrames < 4 * kBenchDrainFrames;
    if (!late && (slots || queries || b.drainFrames < kBenchDrainFrames || !g_benchWriter.idle())) return;
    benchFinish();
}

// A timed frame's stage sums (stageCollect), kept by frame while the bench runs.
void benchNoteStages(uint32_t frame, const double* sums) {
    for (int k = 0; k < kTStages; ++k) g_bench.times.push_back({frame, kBtStage + k, sums[k]});
}

void benchAfterComposite(ID3D11DeviceContext* ctx, int eye, const Eye& e, const Comp& c, const HullLayerCompositeInputs& in, bool applied) {
    BenchRun& b = g_bench;
    if (b.phase == BenchPhase::Rect && b.rectState[eye] == 0) {
        if (stageRegion(ctx, e.cover, in.regionX, in.regionY, in.renderW, in.renderH, g_benchRectStage[eye])) {
            b.rectState[eye] = 1;
            b.rectAt[eye] = g_frame;
        }
        return;
    }
    if (eye == 0 && applied) b.applied0 = true;
    // The mask: the layer's TAA history this frame wrote (premultiplied colour, its coverage in alpha, at the output size), on the
    // last capture frame of sep_taa (of sep_noaa only when sep_taa gave none).
    if (b.phase == BenchPhase::Capture && b.capIndex == hb::kCaptureFrames - 1 && (b.var == hb::kSepTaa || (b.var == hb::kSepNoaa && !b.maskTaken)) &&
        c.hist[c.read ^ 1u]) {
        if (benchStage(ctx, c.hist[c.read ^ 1u], b.rectO[0], b.rectO[1], b.rectO[2], b.rectO[3], hb::maskName(b.stampA, eye), g_frame,
                       static_cast<uint32_t>(eye)) &&
            eye == 1)
            b.maskTaken = true;
    }
}

}  // namespace

void hullLayerConfigure(Config& cfg) {
    std::string mode = cfg.getString("diag.hull_layer", "off");
    if (mode != "off" && mode != "dump" && mode != "separate") {
        Log::get().note("hull layer: diag.hull_layer=%s is not off, dump or separate; read as off", mode.c_str());
        mode = "off";
    }
    if (mode != g_mode) {
        g_mode = mode;
        Log::get().note("hull layer: diag.hull_layer=%s (TEMPORARY; docs\\research-own-hull-layer-2026-11-08.md, Steps 1-3: the own hull, picked per "
                        "record by a GPU own mask, re-lit into private targets and written into the eye dump, with auto dumps; dump changes nothing the game "
                        "draws, separate also takes the own ship out of the game's main G-buffer and composites it back after the upscaler)",
                        mode.c_str());
        if (mode == "separate")
            Log::get().note("hull layer: separate mode takes the own ship out of the game's main pass and puts it back after the upscaler (Step 3: the "
                            "game's tonemap re-issued on the private HDR, the layer's own TAA, the composite before the UI layer); a frame that cannot "
                            "compose keeps the ship in the main pass");
    }
    detail::g_hullLayerOn = mode == "dump" || mode == "separate";
    detail::g_hullLayerSeparate = mode == "separate";
    hullRigProbeConfigure(detail::g_hullLayerOn);   // the rig probe runs while the layer does (no key of its own)
}

bool hullLayerBeforeMainDraw(ID3D11DeviceContext* ctx, UINT instances, UINT startInstance) {
    if (!detail::g_hullLayerSeparate || !ctx || g_pending.active || g_main.active || g_busy || g_benchSleep) return false;   // asleep: no seed, no discard
    const uint64_t vs = bindingShaderHash(BindSlot::Vs);
    // Hull-family draws only. The own decals stay in the main pass: own records also carry the decals on the cockpit's
    // consoles and struts, whose surfaces the layer does not draw, so discarding them lost those details in both images
    // (bench 071557771002, 2026-11-10). A decal over a removed hull projects nothing: the world behind lies outside its box.
    // The layer still re-issues the own decals onto its own hull.
    if (!isHullVs(vs)) return false;
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    const auto decline = [&] {
        releaseTargets(rtv, dsv);
        return false;
    };
    if (!(rtv[0] && rtv[1] && rtv[2] && rtv[3] && dsv)) return decline();   // the G-buffer's four targets
    const int ei = eyeFor(resourceOf(dsv), false);
    if (ei < 0) return decline();
    Eye& e = g_eyes[ei];
    // The seed draw is the own ship's: its mask is made before it is issued, so the seed draw is discarded too.
    if (e.gb == Gb::Before && e.shell && !seedEye(ctx, rtv, dsv, ei, instances, startInstance)) return decline();
    // Step 3's safety: on a frame decided as a fallback (decideNext) the layer is still made, and the ship stays in the main pass.
    if (!g_discard) return decline();
    if (e.gb != Gb::In || g_inEye != ei || !e.maskSrv) return decline();   // no mask this eye-frame: the draw is the game's
    // The draw must be engine motion's substitute: its slot target at RT6, and its patched pixel shader bound.
    if (!(rtv[kSlotTarget] && resourceOf(rtv[kSlotTarget]) == e.slotGame)) {
        ++g_win.noVariant;
        return decline();
    }
    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    const char* why = nullptr;
    ID3D11PixelShader* variant = ps ? engineVelocityOwnDiscardPs(ctx, ps, &why) : nullptr;
    if (!variant) {
        ++g_win.noVariant;
        if (!g_saidNoVariant) {
            g_saidNoVariant = true;
            Log::get().note("hull layer: a hull draw in the window got no own-discard variant (%s); such draws keep the own ship in the main pass (said once)",
                            why ? why : "?");
        }
        releaseRef(ps);
        return decline();
    }
    g_main.active = true;
    g_main.ps = ps;   // the reference passes to AfterMainDraw
    ctx->PSGetShaderResources(kEngineVelocityOwnMaskPsSlot, 1, &g_main.srv);
    vScreenPSSetShaderResourcesRaw(ctx, kEngineVelocityOwnMaskPsSlot, 1, &e.maskSrv);
    vScreenPSSetShaderRaw(ctx, variant, nullptr, 0);
    ++e.ownDraws;
    ++g_win.ownDraws;
    releaseTargets(rtv, dsv);   // the targets' references go; the variant stays bound for the game's issue
    return true;
}

void hullLayerAfterMainDraw(ID3D11DeviceContext* ctx) {
    if (!g_main.active) return;
    if (ctx) {
        vScreenPSSetShaderRaw(ctx, g_main.ps, nullptr, 0);
        vScreenPSSetShaderResourcesRaw(ctx, kEngineVelocityOwnMaskPsSlot, 1, &g_main.srv);
    }
    releaseRef(g_main.ps);
    releaseRef(g_main.srv);
    g_main = MainPending{};
}

static bool beginDraw(ID3D11DeviceContext* ctx, char kind, UINT count, UINT instances, UINT startInstance);

// Station fixes: while the busy governor sleeps, a draw is only counted: the cockpit shell names its eye, and a hull-family draw
// into the G-buffer (RT3 bound with a depth target) counts for that eye. Nothing is bound or issued.
static bool busyDraw(ID3D11DeviceContext* ctx) {
    const uint64_t vs = bindingShaderHash(BindSlot::Vs);
    if (vs == kShellVs) {
        noteShell(ctx);   // no eye's G-buffer window is open while asleep: it only names the eye
    } else if (isHullVs(vs)) {
        ID3D11RenderTargetView* rtv[4] = {};
        ID3D11DepthStencilView* dsv = nullptr;
        ctx->OMGetRenderTargets(4, rtv, &dsv);
        if (rtv[3] && dsv) {
            const int ei = eyeFor(resourceOf(dsv), false);
            if (ei >= 0) ++g_eyes[ei].gbufHull;
        }
        for (auto*& r : rtv) releaseRef(r);
        releaseRef(dsv);
    }
    return false;
}

bool hullLayerBegin(ID3D11DeviceContext* ctx, char kind, UINT count, UINT instances, UINT startInstance) {
    if (!detail::g_hullLayerOn || !ctx || g_pending.active) return false;
    if (g_benchSleep) return false;   // the variant bench's original: asleep, and not counted as busy
    if (g_busy) return busyDraw(ctx);
    if (g_dump != Dump::Capturing) {
        if (!beginDraw(ctx, kind, count, instances, startInstance)) return false;
        scissorPending(ctx);   // Build A': the re-issue scissored to its eye's rectangle
        return true;
    }
    // Dump v2, the dump frame: the draw's bindings before the hook (it may bind the private ones), and after it the lighting
    // window's record (HullLights.json).
    ++g_drawSeq;
    const DrawSnap snap = snapDraw(ctx);
    const bool reissue = beginDraw(ctx, kind, count, instances, startInstance);
    recordLight(snap, kind, count, instances, startInstance, reissue);
    if (reissue) scissorPending(ctx);   // the capture frame too: its comparison checks the rectangle against the game's hull
    return reissue;
}

static bool beginDraw(ID3D11DeviceContext* ctx, char kind, UINT count, UINT instances, UINT startInstance) {
    watchDraw(ctx);   // the unmirrored-input census: a watched input's writer (nothing while no watch is open)
    // Step 3: the game's tonemap (a 3-vertex, one-instance draw, then admitted by structure and by the HDR it reads).
    if (detail::g_hullLayerSeparate && (kind == 'D' || kind == 'N') && count == 3 && instances == 1 && startInstance == 0 && g_device &&
        (g_eyes[0].gb == Gb::Done || g_eyes[1].gb == Gb::Done) && beginTone(ctx))
        return true;
    const uint64_t vs = bindingShaderHash(BindSlot::Vs);
    if (isTerrainVs(vs)) g_terrainFrame = true;
    if (vs == kShellVs) {
        noteShell(ctx);
        return false;
    }
    if (isHullVs(vs)) return beginHullDraw(ctx, instances, startInstance, false);
    if (vs == kDecalVs && detail::g_hullLayerSeparate) return beginHullDraw(ctx, instances, startInstance, true);   // the own decals (Step 2)
    if (isLightVs(vs)) return beginStage(ctx, kLight, 0);
    if (vs == kNightVisionVs) return beginStage(ctx, kLight, 0, true);
    for (const auto& pair : kStagePairs) {
        if (pair.vs != vs) continue;
        if (bindingShaderHash(BindSlot::Ps) == pair.ps) return beginStage(ctx, pair.stage, pair.bit);
    }
    if (g_stageEye >= 0 && g_eyes[g_stageEye].lightOpen) noteInLightWindow(ctx, vs);
    return false;
}

void hullLayerEnd(ID3D11DeviceContext* ctx) {
    if (!g_pending.active) return;
    Pending& p = g_pending;
    if (p.stage != kToneStage) stageClose(ctx);   // dump v2: right after the re-issue (the tonemap's closes after its cover pass)
    unscissorPending(ctx);   // Build A': the game's rasterizer state and scissor back
    if (p.keepOwn) {
        vScreenVSSetShaderRaw(ctx, p.vs, nullptr, 0);
        ctx->VSSetShaderResources(kKeepOwnVsSlot, 1, &p.vsMask);
    }
    releaseRef(p.vs);
    releaseRef(p.vsMask);
    if (p.srvSwapped) vScreenPSSetShaderResourcesRaw(ctx, 0, kSrvSlots, p.srv);
    vScreenSetRenderTargetsRaw(ctx, boundCount(p.rtv), p.rtv, p.dsv);
    if (p.stage < 0) ++g_win.reissued;
    if (p.stage == kToneStage && p.eye >= 0) {
        // Step 3: T is off the output merger now; the cover pass reads it.
        coverPass(ctx, p.eye, p.toneOut);
        timerEnd(g_toneTimer, ctx);
    }
    if (p.stage == kToneStage) stageClose(ctx);
    if (p.stage == kLight && p.eye >= 0 && g_dump == Dump::Capturing) {
        // The private HDR right after this private lighting draw: the last one is the comparison.
        Eye& e = g_eyes[p.eye];
        Mirror* m = findMirror(p.hdrGame);
        if (m) copyToStaging(ctx, m->priv, e.stageHull);
        e.dumpLight = e.lightDraws;
    }
    releaseTargets(p.rtv, p.dsv);
    for (auto*& s : p.srv) releaseRef(s);
    p = Pending{};
}

void hullLayerNoteNightVisionState() {
    if (g_pending.active) ++g_win.nightVisionEdvr;
}

bool hullLayerBeginDispatch(ID3D11DeviceContext* ctx, uint64_t cs) {
    if (detail::g_hullLayerOn && ctx && !g_dispatch.active) watchDispatch(ctx, cs);   // the census's writers, as watchDraw
    if (!detail::g_hullLayerOn || !ctx || !g_device || g_pending.active || g_main.active || g_dispatch.active || g_busy || g_benchSleep) return false;
    for (int s = 0; s < kAoSteps; ++s)
        if (kAoCs[s] == cs) return beginAoStep(ctx, s);
    for (int s = 0; s < kReflSteps; ++s)
        if (kReflCs[s] == cs) return beginReflStep(ctx, s);
    return false;
}

void hullLayerEndDispatch(ID3D11DeviceContext* ctx) {
    if (!g_dispatch.active) return;
    stageClose(ctx);   // dump v2: right after the re-dispatch
    if (ctx && g_dump == Dump::Capturing && g_dispatch.reflEye >= 0) {
        // The reflection dump: the private HDR right after the re-issued step (the last step wins: after both).
        Eye& e = g_eyes[g_dispatch.reflEye];
        Mirror* m = findMirror(e.hdrGame);
        if (m) {
            copyToStaging(ctx, m->priv, e.stageHullRefl);
            ++e.dumpReflHull;
        }
    }
    if (ctx) {
        // The game's inputs first (none is a private copy still bound as a UAV), then its UAVs with their counters kept.
        UINT keep[kCsUavSlots];
        for (auto& k : keep) k = ~0u;
        ctx->CSSetShaderResources(0, kCsSrvSlots, g_dispatch.srv);
        ctx->CSSetUnorderedAccessViews(0, kCsUavSlots, g_dispatch.uav, keep);
    }
    releaseCsSlots(g_dispatch.srv, g_dispatch.uav);
    g_dispatch = DispatchPending{};
}

void hullLayerAfterCopy(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src) {
    if (detail::g_hullLayerOn && dst && !g_watch.empty()) noteWriter(dst, 'y', 0);   // the census: a copy wrote a watched input
    if (!detail::g_hullLayerOn || !ctx || !dst || !src || !g_device || g_busy || g_benchSleep) return;
    for (auto& e : g_eyes)   // a copy out of the eye's slot target: engine motion's overlay snapshot, the decals' t3
        if (e.slotGame && e.slotGame == static_cast<const void*>(src)) e.overlayGame = dst;
    Mirror* ms = findMirror(src);
    if (!ms) return;   // only a copy out of a texture the layer mirrors (the G-buffer, depth, HDR) has a private twin
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    dst->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return;
    D3D11_TEXTURE2D_DESC dd{};
    static_cast<ID3D11Texture2D*>(dst)->GetDesc(&dd);
    if (dd.Width != ms->desc.Width || dd.Height != ms->desc.Height || dd.ArraySize != ms->desc.ArraySize || dd.MipLevels != ms->desc.MipLevels ||
        dd.SampleDesc.Count != 1)
        return;   // a whole-texture copy only (the decals' base copy is one); a partial or resampled one is not mirrored
    Mirror* md = findMirror(dst);
    if (md && std::memcmp(&md->desc, &dd, sizeof dd) != 0) {
        dropMirror(dst);
        md = nullptr;
    }
    if (!md) md = makeMirror(dst, dd);
    ms = findMirror(src);   // makeMirror may have grown the table
    if (!md || !ms || !md->priv || !ms->priv) return;
    VrWorldInternalScope internal;
    vScreenCopyResourceRaw(ctx, md->priv, ms->priv);
    ++g_win.copiesMirrored;
}

void hullLayerNoteClearRtv(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, const float colour[4]) {
    if (!detail::g_hullLayerOn || !rtv || !colour || !g_device || g_busy || g_benchSleep || !findMirror(resourceOf(rtv))) return;   // asleep: the copies age out
    const void* res = resourceOf(rtv);
    if (res == g_eyes[0].reflU1Game || res == g_eyes[1].reflU1Game) {   // the game clears the reflection pass's u1 itself: its colour
        ++g_win.reflU1GameClears;
        std::memcpy(g_reflU1Clear, colour, sizeof g_reflU1Clear);
        g_reflU1ClearSeen = true;
    }
    auto* priv = static_cast<ID3D11RenderTargetView*>(privateView(ViewKind::Rtv, rtv, false));
    if (priv) vScreenClearRenderTargetViewRaw(ctx, priv, colour);
}

void hullLayerNoteClearDsv(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, UINT flags, float depth, UINT8 stencil) {
    if (!detail::g_hullLayerOn || !dsv) return;
    const void* res = resourceOf(dsv);
    ClearValue* slot = nullptr;
    for (auto& c : g_clearValues)
        if (c.res == res) slot = &c;
    if (!slot) {
        slot = &g_clearValues[g_clearNext];
        g_clearNext = (g_clearNext + 1) % 8;
        *slot = ClearValue{};
        slot->res = res;
    }
    if (flags & D3D11_CLEAR_DEPTH) slot->depth = depth;
    if (flags & D3D11_CLEAR_STENCIL) slot->stencil = stencil;
    if (!g_device || g_busy || g_benchSleep || !findMirror(res)) return;
    auto* priv = static_cast<ID3D11DepthStencilView*>(privateView(ViewKind::Dsv, dsv, false));
    if (priv) {
        VrWorldInternalScope internal;   // the hook forwards this clear raw and does not see it as the game's
        ctx->ClearDepthStencilView(priv, flags, depth, stencil);
    }
}

void hullLayerArmDump(const wchar_t* stamp) {
    if (!detail::g_hullLayerOn || g_dump != Dump::Idle) return;
    if (detail::g_hullBenchOn && g_bench.phase != BenchPhase::Drain) {
        g_bench.abortWhy = "an eye dump was armed";   // its reference frame and sequence would cut into a variant
        return;
    }
    wcsncpy_s(g_dumpStamp, stamp ? stamp : L"", _TRUNCATE);
    g_dump = Dump::Armed;
    Log::get().note("hull layer: dump %ls armed for the next whole frame", g_dumpStamp);
}

// The sharpen and composite over the TAA result `hist` onto the eye's listed tiles (kSharpHlsl / kSharpDirectHlsl): direct, through
// the eye's own UAV; else into c.out, the world read from `world`. The caller saves and restores the compute stage.
void runSharpen(ID3D11DeviceContext* ctx, Comp& c, ID3D11ShaderResourceView* hist, bool direct, ID3D11ShaderResourceView* world) {
    ID3D11UnorderedAccessView* none[CsStageSave::kUavs] = {};
    const float sharpCb[4] = {kSharpAmount, 0.0f, 0.0f, 0.0f};
    vScreenUpdateSubresourceRaw(ctx, g_cgpu.sharpCb, 0, nullptr, sharpCb, 0, 0);
    ID3D11ShaderResourceView* noSrv[5] = {};
    ctx->CSSetShaderResources(0, 5, noSrv);   // the history just written comes back as a resource
    // Direct: the world is read through the eye's own UAV at the pixel written, so no SRV of the eye is bound (t1 empty).
    ID3D11ShaderResourceView* sharpSrv[3] = {hist, direct ? nullptr : world, c.tileListSrv};
    // Every saved UAV slot is set: one the caller left bound over the upscaler's output would null its view.
    ID3D11UnorderedAccessView* sharpUav[CsStageSave::kUavs] = {direct ? c.directUav : c.outUav};
    ctx->CSSetUnorderedAccessViews(0, CsStageSave::kUavs, sharpUav, nullptr);
    ctx->CSSetShader(direct ? g_cgpu.sharpDirectCs : g_cgpu.sharpCs, nullptr, 0);
    ctx->CSSetShaderResources(0, 3, sharpSrv);
    ctx->CSSetConstantBuffers(0, 1, &g_cgpu.sharpCb);
    ctx->DispatchIndirect(c.tileArgs, 0);
    ctx->CSSetShaderResources(0, 5, noSrv);
    ctx->CSSetUnorderedAccessViews(0, CsStageSave::kUavs, none, nullptr);
}

bool hullLayerCompositeEye(ID3D11DeviceContext* ctx, int eye, const HullLayerCompositeInputs& in) {
    if (eye >= 0 && eye <= 1 && g_comp[eye].finishFrame == g_frame) {
        // A finish the caller never made (hullLayerCompositeFinish): counted; this composite starts afresh.
        ++g_win.fgFinishLost;
        g_comp[eye].finishFrame = ~0u;
    }
    if (!detail::g_hullLayerSeparate || !ctx || eye < 0 || eye > 1 || !g_device || !in.output || !in.outputSrv || !in.source || !in.tanNow || g_busy || g_benchSleep)
        return false;   // asleep (Station fixes): no composite
    VrWorldInternalScope internal;   // the layer's own dispatch and copy
    // The hull eye: the one whose tonemap wrote the submitted texture this frame (the same index first, should both eyes share one
    // texture); else the eye of the same index, toned this frame.
    int hi = -1;
    for (int k = 0; k < 2 && hi < 0; ++k) {
        const int i = k == 0 ? eye : 1 - eye;
        if (g_eyes[i].toneFrame == g_frame && g_eyes[i].toneOut == in.source) hi = i;
    }
    const bool byIndex = hi < 0 && g_eyes[eye].toneFrame == g_frame;
    if (byIndex) hi = eye;
    if (hi < 0) {
        // Nothing toned this frame: the judge counts the miss. Toned on the frame before: the treat came after the frame boundary.
        for (const auto& e : g_eyes)
            if (e.toneFrame != ~0u && e.toneFrame + 1 == g_frame) {
                sayDeclined(kDcStale, eye, "the eye's tonemap re-issue is a frame old (the submit came after the frame boundary)");
                break;
            }
        return false;
    }
    Eye& e = g_eyes[hi];
    if (!e.coverSrv || !e.coverZSrv || !in.renderW || !in.renderH || !in.outW || !in.outH || in.regionX + in.renderW > e.toneW ||
        in.regionY + in.renderH > e.toneH) {
        sayDeclined(kDcSize, eye, "the eye's region does not fit the tonemap's output");
        return false;
    }
    if (!ensureCompGpu() || !ensureCompTargets(eye, in.outW, in.outH)) {
        sayDeclined(kDcGpu, eye, "the composite's shaders or targets are missing");
        return false;
    }
    Comp& c = g_comp[eye];
    const bool history = c.have && c.lastFrame != ~0u && c.lastFrame + 1 == g_frame && c.renderW == in.renderW && c.renderH == in.renderH &&
                         in.tanPrev && in.headDelta;
    if (!history) ++g_win.historyResets;
    struct TaaCb {
        float tanNow[4], tanPrev[4], dRow0[4], dRow1[4], dRow2[4], headT[4], jit[4], depthAB[4];
        int32_t sizes[4], origin[4];
    };
    static_assert(sizeof(TaaCb) == 160, "the TAA constant buffer is HullLayerTaaCb's 160 bytes");
    TaaCb cb{};
    std::memcpy(cb.tanNow, in.tanNow, sizeof cb.tanNow);
    std::memcpy(cb.tanPrev, in.tanPrev ? in.tanPrev : in.tanNow, sizeof cb.tanPrev);
    float* rows[3] = {cb.dRow0, cb.dRow1, cb.dRow2};
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) rows[r][k] = in.headDelta ? in.headDelta[r * 3 + k] : (r == k ? 1.0f : 0.0f);
    if (in.headTrans && in.useTrans) {
        for (int k = 0; k < 3; ++k) cb.headT[k] = in.headTrans[k];
        cb.headT[3] = 1.0f;
    }
    cb.jit[0] = in.jitterX;
    cb.jit[1] = in.jitterY;
    cb.jit[2] = history && !g_benchNoAa ? 1.0f : 0.0f;
    cb.jit[3] = kTaaCurrent;
    cb.depthAB[0] = in.depthA;
    cb.depthAB[1] = in.depthB;
    cb.sizes[0] = static_cast<int32_t>(in.renderW);
    cb.sizes[1] = static_cast<int32_t>(in.renderH);
    cb.sizes[2] = static_cast<int32_t>(in.outW);
    cb.sizes[3] = static_cast<int32_t>(in.outH);
    cb.origin[0] = static_cast<int32_t>(in.regionX);
    cb.origin[1] = static_cast<int32_t>(in.regionY);
    // Applied only when this frame's main pass discarded the own ship on this eye; otherwise it is made (the history stays warm,
    // and a miss is seen) and the upscaled eye keeps the game's own ship.
    const bool apply = g_discard && (e.ownDraws + e.ownDecals) > 0;
    timerBegin(g_compTimer, ctx);
    // The method (docs "Bench A/B variants"): an applied composite writes the hull straight onto the eye's listed tiles through a
    // typed UAV of the eye (directUavFor), and neither whole-eye copy runs; through c.out when not applied (the eye is not to
    // change), when the dump or the TAA sequence reads c.out this frame, and when the eye's UAV cannot be had. Decided before the
    // world copy below, which the S-first order needs only when the composite is not direct (the bench's DLSS decides it later).
    const bool needsOut = g_dump == Dump::Capturing || (eye == 0 && g_seq.active && !g_seq.end && g_seq.staged < kSeqFrames);
    const auto methodWhy = [&]() { return !apply ? kCmNotApplied : needsOut ? kCmNeedsOut : !kDirectComposite ? kCmOff : directUavFor(c, eye, in); };
    const int earlyWhy = g_benchDlss ? -1 : methodWhy();
    // Frame generation, the S-first order (in.sFirst): an applied direct composite leaves the hull's write onto the eye to
    // hullLayerCompositeFinish, after the S that reads the eye; anything else keeps the copy below (copy_why).
    const bool sFirst = apply && in.keepWorld && in.sFirst && earlyWhy == kCompMethods && !detail::g_hullBenchOn;
    if (apply && in.keepWorld && !sFirst)
        ++g_win.fgCopyWhy[g_benchDlss || detail::g_hullBenchOn ? kWcBench : !in.sFirst ? kWcNotAsked : kWcNotDirect];
    // Frame generation: the eye before the hull goes onto it, for the S that reads this very texture (keepWorld). Inside the composite's
    // timer, so its gpu_ms carries the copy.
    const bool worldKept = apply && in.keepWorld && !sFirst && !g_benchDlss && keepWorldCopy(ctx, eye, c, in);
    stageOpen(ctx, kTsTaa, false);   // dump v2: the TAA, the sharpen and the composite's copy
    vScreenUpdateSubresourceRaw(ctx, g_cgpu.taaCb, 0, nullptr, &cb, 0, 0);
    // The variant bench's dlss_full / dlss_sub: the hull through a second DLSS over the hull-free world in place of the TAA and sharpen.
    const bool benchDlss = g_benchDlss && benchHullDlss(ctx, eye, e, c, in);
    if (benchDlss) c.tilesValid = false;   // the TAA's pair and tile state were not kept this frame
    bool direct = false;
    if (benchDlss) {
        ++g_win.compBenchDlss;
    } else {
        const int why = earlyWhy >= 0 ? earlyWhy : methodWhy();
        direct = why == kCompMethods;
        if (direct)
            ++g_win.compDirect;
        else
            ++g_win.compCopy[why];
    }
    if (!benchDlss) {
        CsStageSave save;
        save.save(ctx);
        ID3D11UnorderedAccessView* none[CsStageSave::kUavs] = {};
        // The TAA's tiles (kTileMarkHlsl): a history reset zeroes the pair and the tile state, so every unlisted tile is zero;
        // so does the first TAA frame after the bench's second DLSS, which wrote neither.
        if (!history || !c.tilesValid) {
            const float zf[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            const UINT zu[4] = {0u, 0u, 0u, 0u};
            ctx->ClearUnorderedAccessViewFloat(c.histUav[0], zf);
            ctx->ClearUnorderedAccessViewFloat(c.histUav[1], zf);
            ctx->ClearUnorderedAccessViewUint(c.tileStateUav, zu);
            ctx->ClearUnorderedAccessViewUint(c.tileMarkUav, zu);
            c.tilesValid = true;
        }
        // The out starts as the upscaled eye: the sharpen writes only the tiles the hull reaches (direct: the eye is the out).
        if (!direct) vScreenCopyResourceRaw(ctx, c.out, in.output);
        {
            ID3D11ShaderResourceView* srv = e.coverSrv;
            ID3D11UnorderedAccessView* uavs[CsStageSave::kUavs] = {c.tileMarkUav};
            ctx->CSSetUnorderedAccessViews(0, CsStageSave::kUavs, uavs, nullptr);
            ctx->CSSetShader(g_cgpu.tileMarkCs, nullptr, 0);
            ctx->CSSetShaderResources(0, 1, &srv);
            ctx->CSSetConstantBuffers(0, 1, &g_cgpu.taaCb);
            ctx->Dispatch((in.renderW + 7u) / 8u, (in.renderH + 7u) / 8u, 1);
            ID3D11ShaderResourceView* noSrv = nullptr;
            ctx->CSSetShaderResources(0, 1, &noSrv);
            ctx->CSSetUnorderedAccessViews(0, CsStageSave::kUavs, none, nullptr);
        }
        {
            static_assert(CsStageSave::kUavs >= 3, "the tile list binds three UAVs");
            ID3D11UnorderedAccessView* uavs[CsStageSave::kUavs] = {c.tileMarkUav, c.tileStateUav, c.tileListUav};
            UINT counts[CsStageSave::kUavs];
            for (UINT& n : counts) n = ~0u;
            counts[2] = 0u;   // the append list starts empty
            ctx->CSSetUnorderedAccessViews(0, CsStageSave::kUavs, uavs, counts);
            ctx->CSSetShader(g_cgpu.tileListCs, nullptr, 0);
            ctx->CSSetConstantBuffers(0, 1, &g_cgpu.taaCb);
            ctx->Dispatch((in.outW + 63u) / 64u, (in.outH + 63u) / 64u, 1);
            ctx->CSSetUnorderedAccessViews(0, CsStageSave::kUavs, none, nullptr);
            ctx->CopyStructureCount(c.tileArgs, 0, c.tileListUav);
        }
        ID3D11ShaderResourceView* srvs[5] = {e.coverSrv, e.coverZSrv, c.histSrv[c.read], nullptr, c.tileListSrv};
        // Every saved UAV slot is set: one the caller left bound over the upscaler's output would null its view.
        ID3D11UnorderedAccessView* uavs[CsStageSave::kUavs] = {c.histUav[c.read ^ 1u]};
        ctx->CSSetUnorderedAccessViews(0, CsStageSave::kUavs, uavs, nullptr);
        ctx->CSSetShader(g_cgpu.taaCs, nullptr, 0);
        ctx->CSSetShaderResources(0, 5, srvs);
        ctx->CSSetConstantBuffers(0, 1, &g_cgpu.taaCb);
        ctx->CSSetSamplers(0, 1, &g_cgpu.linear);
        ctx->DispatchIndirect(c.tileArgs, 0);
        ctx->CSSetUnorderedAccessViews(0, CsStageSave::kUavs, none, nullptr);   // the history and the out off before the caller's views come back
        // The sharpen and composite over the TAA result just written (kSharpHlsl): it rewrites the out the TAA made. The S-first order
        // runs it in hullLayerCompositeFinish instead, over the same history (hist[read] after the flip) and the same tile list.
        if (!sFirst) {
            runSharpen(ctx, c, c.histSrv[c.read ^ 1u], direct, in.outputSrv);
        } else {
            ID3D11ShaderResourceView* noSrv[5] = {};
            ctx->CSSetShaderResources(0, 5, noSrv);
        }
        save.restore(ctx);
    }
    if (apply && !direct) vScreenCopyResourceRaw(ctx, in.output, c.out);
    stageClose(ctx);
    timerEnd(g_compTimer, ctx);
    if (g_dump == Dump::Capturing) {
        copyToStaging(ctx, e.cover, e.stageTone);
        copyToStaging(ctx, c.out, e.stageComp);
        e.dumpApplied = apply;
    }
    if (eye == 0) seqStage(ctx, e, c, in, history, apply);   // dump v2: the TAA sequence (before the history pair flips)
    if (detail::g_hullBenchOn) benchAfterComposite(ctx, eye, e, c, in, apply);   // the variant bench (before the history pair flips)
    c.read ^= 1u;
    c.lastFrame = g_frame;
    c.renderW = in.renderW;
    c.renderH = in.renderH;
    c.have = true;
    // Frame generation: what the S built right after this (same treat) needs; hist[read] is now this frame's TAA result. A frame the
    // bench's second DLSS made has no TAA result, so S takes nothing from it.
    c.sFrame = g_frame;
    c.sApplied = apply && !benchDlss;
    c.sHull = hi;
    std::memcpy(c.sTan, in.tanNow, sizeof c.sTan);
    c.sDepthA = in.depthA;
    c.sDepthB = in.depthB;
    c.sRegionX = in.regionX;
    c.sRegionY = in.regionY;
    c.sRenderW = in.renderW;
    c.sRenderH = in.renderH;
    c.sOutSrv = in.outputSrv;
    if (worldKept) c.worldFrame = g_frame;
    if (sFirst) c.finishFrame = g_frame;   // the hull's write waits for hullLayerCompositeFinish
    e.compFrame = g_frame;
    e.compApplied = apply;
    if (byIndex) ++g_win.byIndex;
    if (apply) ++(in.afterUiResolve ? g_win.siteAfterResolve : g_win.siteUpscalerOut);
    return sFirst;
}

bool hullLayerCompositeFinish(ID3D11DeviceContext* ctx, int eye) {
    if (!ctx || eye < 0 || eye > 1 || g_comp[eye].finishFrame != g_frame) return false;
    Comp& c = g_comp[eye];
    c.finishFrame = ~0u;
    if (!c.directUav || !g_cgpu.sharpDirectCs || !c.tileArgs) {   // never: the composite chose the S-first order on these
        ++g_win.fgFinishLost;
        return false;
    }
    VrWorldInternalScope internal;   // the layer's own dispatch
    // The composite's gpu_ms carries this part too (one call, two timed spans).
    timerBegin(g_compTimer, ctx, true);
    CsStageSave save;
    save.save(ctx);
    runSharpen(ctx, c, c.histSrv[c.read], true, nullptr);   // hist[read]: this frame's TAA result, the history pair having flipped
    save.restore(ctx);
    timerEnd(g_compTimer, ctx);
    ++g_win.fgFinished;
    return true;
}

ID3D11ShaderResourceView* hullLayerSInput(int eye, ID3D11ShaderResourceView* proposed, bool* overlay) {
    if (overlay) *overlay = false;
    if (!detail::g_hullLayerSeparate || eye < 0 || eye > 1 || !proposed || !overlay) return proposed;
    ++g_win.fgS;
    const Comp& c = g_comp[eye];
    if (c.sFrame != g_frame || c.sHull < 0) {
        ++g_win.fgDeclined[kFgNoComposite];
        return proposed;
    }
    if (!c.sApplied) {
        ++g_win.fgDeclined[kFgNotApplied];
        return proposed;
    }
    if (proposed == c.sOutSrv && c.finishFrame == g_frame) {   // the S-first order: the eye holds the world only until the finish
        ++g_win.fgWorldBefore;
        *overlay = true;
        return proposed;
    }
    if (proposed == c.sOutSrv) {   // the very eye the composite wrote: S reads the copy from before it
        if (c.worldFrame != g_frame || !c.worldSrv) {
            ++g_win.fgDeclined[kFgNoWorldCopy];
            return proposed;
        }
        ++g_win.fgWorldCopy;
        *overlay = true;
        return c.worldSrv;
    }
    ++g_win.fgWorldDirect;   // another texture (the upscaler's output while the composite went onto the submitted eye): world-only as it is
    *overlay = true;
    return proposed;
}

bool hullLayerCompositeS(ID3D11DeviceContext* ctx, uint32_t eye, ID3D11UnorderedAccessView* s, uint32_t w, uint32_t h, const float rot[9], const float t[3]) {
    if (!ctx || eye > 1 || !s || !rot || !t || !g_device) return false;
    const Comp& c = g_comp[eye];
    if (c.sFrame != g_frame || !c.sApplied || c.sHull < 0) {
        ++g_win.fgDeclined[kFgNoComposite];
        return false;
    }
    const Eye& e = g_eyes[c.sHull];
    if (w != c.outW || h != c.outH || !c.histSrv[c.read] || !e.coverZSrv) {
        ++g_win.fgDeclined[kFgSize];
        return false;
    }
    if (!g_cgpu.ok || !g_cgpu.sharpCb || !ensureSCs() || !sFormatLoads(s)) {
        ++g_win.fgDeclined[kFgUnsupported];
        return false;
    }
    VrWorldInternalScope internal;   // the layer's own dispatch
    struct SCb {
        float tan[4], s0[4], s1[4], s2[4], depthAB[4];
        int32_t sizes[4], origin[4];
    };
    static_assert(sizeof(SCb) == 112, "kSHlsl's HullLayerSCb is 112 bytes");
    SCb cb{};
    std::memcpy(cb.tan, c.sTan, sizeof cb.tan);
    float* rows[3] = {cb.s0, cb.s1, cb.s2};
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) rows[r][k] = rot[r * 3 + k];
        rows[r][3] = t[r];
    }
    cb.depthAB[0] = c.sDepthA;
    cb.depthAB[1] = c.sDepthB;
    cb.depthAB[3] = (std::fabs(t[0]) + std::fabs(t[1]) + std::fabs(t[2])) > 1e-7f ? 1.0f : 0.0f;
    cb.sizes[0] = static_cast<int32_t>(c.sRenderW);
    cb.sizes[1] = static_cast<int32_t>(c.sRenderH);
    cb.sizes[2] = static_cast<int32_t>(w);
    cb.sizes[3] = static_cast<int32_t>(h);
    cb.origin[0] = static_cast<int32_t>(c.sRegionX);
    cb.origin[1] = static_cast<int32_t>(c.sRegionY);
    vScreenUpdateSubresourceRaw(ctx, g_cgpu.sCb, 0, nullptr, &cb, 0, 0);
    // The caller's (the synthesizer's) passes bind S as a UAV elsewhere: every slot is cleared before S goes to u0. The caller restores its
    // own stage after (framegen_synth.cpp re-binds its b0; its CsStageSave puts the rest back).
    ID3D11UnorderedAccessView* none[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
    ctx->CSSetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, none, nullptr);
    ID3D11Buffer* cbs[2] = {g_cgpu.sharpCb, g_cgpu.sCb};
    ctx->CSSetConstantBuffers(0, 2, cbs);
    ID3D11ShaderResourceView* noSrv[4] = {};
    // Over the S tiles the hull can reach (kSHlsl's list pass, then one group a listed tile, by DispatchIndirect); the whole eye when
    // the tile shaders or buffers are missing or this frame's TAA tile state is not kept (the same pixels either way: the bench, docs
    // "S first, S tiles").
    const bool tiles = c.tilesValid && g_cgpu.linear && ensureSTiles(g_comp[eye]);
    if (tiles) {
        const uint32_t tw = (w + 7u) / 8u, th = (h + 7u) / 8u;
        ID3D11ShaderResourceView* listSrvs[4] = {nullptr, e.coverZSrv, nullptr, c.tileStateSrv};
        ID3D11UnorderedAccessView* listUavs[2] = {nullptr, c.sTileListUav};
        const UINT counts[2] = {~0u, 0u};   // the append list starts empty
        ctx->CSSetShader(g_cgpu.sTileListCs, nullptr, 0);
        ctx->CSSetShaderResources(0, 4, listSrvs);
        ctx->CSSetSamplers(0, 1, &g_cgpu.linear);   // the cover depth's gather (address clamp; the filter is not used)
        ctx->CSSetUnorderedAccessViews(0, 2, listUavs, counts);
        ctx->Dispatch((tw + 7u) / 8u, (th + 7u) / 8u, 1);
        ctx->CSSetUnorderedAccessViews(0, 2, none, nullptr);
        ID3D11SamplerState* noSampler = nullptr;
        ctx->CSSetSamplers(0, 1, &noSampler);
        ctx->CopyStructureCount(c.sTileArgs, 0, c.sTileListUav);
        ID3D11ShaderResourceView* srvs[3] = {c.histSrv[c.read], e.coverZSrv, c.sTileListSrv};
        ctx->CSSetShader(g_cgpu.sTilesCs, nullptr, 0);
        ctx->CSSetShaderResources(0, 4, noSrv);
        ctx->CSSetShaderResources(0, 3, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, &s, nullptr);
        ctx->DispatchIndirect(c.sTileArgs, 0);
        ++g_win.fgOverTiles;
    } else {
        ID3D11ShaderResourceView* srvs[2] = {c.histSrv[c.read], e.coverZSrv};
        ctx->CSSetShader(g_cgpu.sCs, nullptr, 0);
        ctx->CSSetShaderResources(0, 2, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, &s, nullptr);
        ctx->Dispatch((w + 7u) / 8u, (h + 7u) / 8u, 1);
        ++g_win.fgOverWhole;
    }
    ctx->CSSetShaderResources(0, 4, noSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, none, nullptr);
    ++g_win.fgHullInS;
    if (!g_saidFrameGenS) {
        g_saidFrameGenS = true;
        Log::get().note("hull layer: framegen: the hull layer drawn into frame generation's S (eye %u, %ux%u): S warps the world-only eye and the hull "
                        "moves by the head's step alone (said once)",
                        eye, w, h);
    }
    return true;
}

void hullLayerFrameBoundary(ID3D11DeviceContext* ctx) {
    if (g_main.active) hullLayerAfterMainDraw(ctx);   // never: Before and After are one call chain
    if (!detail::g_hullLayerOn) {
        if (detail::g_hullBenchOn) {   // the variant bench ends with the layer: its copies so far written, the manifest says why
            benchRestore("diag.hull_layer turned off");
            if (ctx) benchMapSlots(ctx, true);
            benchFinish();
        }
        if (g_device || !g_mirrors.empty()) releaseAll();
        g_win = Window{};
        return;
    }
    if (g_pending.active && ctx) hullLayerEnd(ctx);   // never: Begin and End are one call chain
    if (g_dispatch.active && ctx) hullLayerEndDispatch(ctx);   // never, likewise
    {
        VrWorldInternalScope internal;   // the passes, copies and maps here are the layer's own
        // The frame that just ended: an eye whose G-buffer window is still open is filtered now.
        if (ctx)
            for (int i = 0; i < 2; ++i)
                if (g_eyes[i].gb == Gb::In) finishGbuffer(ctx, i, kEndFrame);
        stageFrameEnd(ctx);   // dump v2: after the last item of the frame (the filter above)
        if (detail::g_hullLayerSeparate) judgeFrame();   // Step 3: before the frame number moves on (the tone and composite marks name it)
        for (Comp& fc : g_comp)
            if (fc.finishFrame == g_frame) {   // an S-first composite whose finish never came: that eye went out without the hull
                ++g_win.fgFinishLost;
                fc.finishFrame = ~0u;
            }
        ++g_frame;
        unmirroredFrameEnd();
        hullRigProbeFrameEnd(ctx, g_frame);   // the rig probe's readbacks and lines (ctx null: no readback this frame)
        if (ctx) {
            readBacks(ctx);
            timerCollect(g_toneTimer, ctx);
            timerCollect(g_compTimer, ctx);
            stageCollect(ctx);
            seqBoundary(ctx);
            if (detail::g_hullBenchOn) benchCollect(ctx);   // the variant bench: its copies mapped, its times read
        }
        if (g_dump == Dump::Capturing && ctx) writeDump(ctx);
    }
    g_drawSeq = 0;
    for (auto& e : g_eyes) {
        if (!e.seen || !e.shell) continue;
        ++g_win.eyeFrames;
        if (e.gb == Gb::Before) ++g_win.noSeed;
        if (e.gb == Gb::Off && detail::g_hullLayerSeparate) ++g_win.fallbackFrames;   // the seed ran and failed: the main pass drew the own ship
        if (e.gb == Gb::Done) {
            g_win.hullDraws.push_back(e.hull);
            g_win.lightDraws.push_back(e.lightDraws);
            ++g_win.aoEyeFrames;
            if (e.aoDone == (kAoAllSteps | kAoApplyBit)) ++g_win.aoFull;
            for (int s = 0; s <= kAoSteps; ++s) {
                const uint8_t bit = static_cast<uint8_t>(1u << s);
                if ((e.aoSeen & bit) && !(e.aoDone & bit)) ++g_win.aoMissed[s];
            }
        }
    }
    ++g_win.frames;
    if (g_terrainFrame) ++g_win.terrainFrames;
    for (int s = 0; s < kStages; ++s) {
        if (g_noCpFrame[s]) ++g_win.noCounterpart[s];
        g_noCpFrame[s] = false;
    }
    // The auto dump's streak: a seeded frame over terrain with an own mask of a ship's size.
    const bool good = g_seededFrame && g_terrainFrame && g_haveOwn && g_lastOwn >= kAutoOwnLo && g_lastOwn <= kAutoOwnHi;
    g_autoStreak = good ? g_autoStreak + 1 : 0;
    if (g_dump == Dump::Capturing) {
        g_dump = Dump::Idle;   // written above
    } else if (g_dump == Dump::Armed && g_busy) {
        // Asleep: governBusy below wakes the layer, the next frame warms it up (its private copies made again), the one after is captured.
    } else if (g_dump == Dump::Armed) {
        g_dump = Dump::Capturing;
        g_dumpFrame = g_frame;
        g_lights.clear();   // dump v2: the capture frame's lighting windows (HullLights.json)
        g_lightsCapped = false;
        if (detail::g_hullLayerSeparate) seqStart();   // dump v2: the TAA sequence starts on the capture frame
    }
    const ULONGLONG now = GetTickCount64();
    if (g_autoStreak >= kAutoFrames && g_autoDumps < kAutoMax && g_dump == Dump::Idle && !detail::g_hullBenchOn && (g_lastAutoMs == 0 || now - g_lastAutoMs >= kAutoGapMs)) {
        temporalPassArmEyeDump();   // the hotkey's own call: the eye run, its census and ledger, and hullLayerArmDump
        if (g_dump == Dump::Armed) {
            ++g_autoDumps;
            ++g_win.autoDumps;
            g_lastAutoMs = now;
            Log::get().note("hull layer: auto dump armed (reason: own mask %u records, seeded over terrain for %u frames in a row; %u of %u this session)",
                            g_lastOwn, g_autoStreak, g_autoDumps, kAutoMax);
            g_autoStreak = 0;
        }
    }
    governBusy();   // Station fixes: from this frame's counts, before they reset; decideNext below reads it
    // The next frame's eyes: the depth identities stay, so an eye keeps its index.
    for (auto& e : g_eyes) resetFrame(e);
    g_inEye = g_stageEye = -1;
    g_terrainFrame = g_seededFrame = false;
    // The variant bench: the frame that ended judged, the next frame's variant (an original sleeps: g_benchSleep).
    if (detail::g_hullBenchOn) {
        if (g_bench.phase == BenchPhase::Drain && ctx)
            benchDrainStep(ctx);
        else
            benchStep();
    }
    // Step 3: whether the next frame's main pass may discard the own ship, from what is known before its first draw.
    Fallback why = kFallbacks;
    if (detail::g_hullLayerSeparate && !g_benchSleep)
        why = decideNext();
    else
        g_discard = false;
    if (detail::g_hullBenchOn) benchDecided(why);
    // Dump v2: separate mode's capture frame is a reference frame: the main pass keeps the own ship (as on a fallback frame, the
    // layer still runs and its composite is made, not applied), so the dump holds the game's own hull to compare the layer with.
    // One frame of the ship drawn by the game; the hold and the miss judge are untouched.
    g_refFrame = false;
    if (detail::g_hullLayerSeparate && g_dump == Dump::Capturing) {
        g_refFrame = true;
        g_refWould = g_discard;
        if (g_discard) --g_win.separateFrames;   // decideNext counted it as a discard frame
        g_discard = false;
    }
    stageFrameStart();   // dump v2: whether the next frame is timed on the GPU
    evictStale();
    if (!g_win.startMs) g_win.startMs = now;
    if (now - g_win.startMs >= kReportMs) {
        report();
        reportUnmirrored();
        g_win = Window{};
        g_win.startMs = now;
    }
}

void hullLayerShutdown() {
    g_pending = Pending{};   // references, if any, die with the device
    g_main = MainPending{};
    g_dispatch = DispatchPending{};
    if (detail::g_hullBenchOn) {
        g_benchWriter.stop();   // its queue written first: the files on disk are whole
        benchReleaseGpu();
        benchDlssRelease();
        g_benchSleep = g_benchNoAa = g_benchTimeAll = g_benchNoRefl = g_benchNoPre = g_benchNoRect = false;
        g_benchDlss = 0;
        g_bench.phase = BenchPhase::Idle;
        detail::g_hullBenchOn = false;
    }
    releaseAll();
    detail::g_hullLayerOn = false;
}

// ---- the variant bench: its key and hooks (TEMPORARY) ---------------------------------------------------------------------

void hullBenchPress() {
    BenchRun& b = g_bench;
    if (detail::g_hullBenchOn) {
        if (b.phase == BenchPhase::Drain) {
            Log::get().note("hull layer: bench %s: press ignored, the bench is writing its files", b.stampA.c_str());
        } else {
            b.abortWhy = "cancelled (a second press)";
            Log::get().note("hull layer: bench %s: cancel pressed", b.stampA.c_str());
        }
        return;
    }
    const char* refuse = nullptr;
    char why[160] = "";
    ULARGE_INTEGER freeBytes{};
    const std::wstring dir = Log::get().dir() + L"\\eyes";
    if (!detail::g_hullLayerSeparate)
        refuse = "diag.hull_layer is not separate";
    else if (!runtimeVrProfile())
        refuse = "not the VR profile";
    else if (nativeOnFootActive())
        refuse = "on foot";
    else if (frameGenOn())
        refuse = "frame generation is on (the layer stands aside)";
    else if (g_busy)
        refuse = "the layer sleeps (the busy governor)";
    else if (!g_device)
        refuse = "the layer has not run yet (no own ship composited)";
    else if (g_dump != Dump::Idle || g_seq.active)
        refuse = "a hull dump is being taken";
    else if (temporalPassEyeRunBusy())
        refuse = "an eye run is still being taken or written";
    else if (!g_benchWriter.idle())
        refuse = "the last bench's files are still being written";
    else if (!GetDiskFreeSpaceExW(Log::get().dir().c_str(), &freeBytes, nullptr, nullptr) || freeBytes.QuadPart < kBenchMinFreeBytes) {
        _snprintf_s(why, sizeof why, _TRUNCATE, "%.1f GB free on the log drive, under 4", static_cast<double>(freeBytes.QuadPart) / (1024.0 * 1024.0 * 1024.0));
        refuse = why;
    }
    if (refuse) {
        Log::get().note("hull layer: bench refused: %s (hotkey.hull_bench, TEMPORARY)", refuse);
        char text[112];
        _snprintf_s(text, sizeof text, _TRUNCATE, "Hull bench refused: %s", refuse);
        benchToast(text);
        return;
    }
    g_benchWriter.stop();   // a thread left from the last bench (its queue was empty)
    g_benchWriter.resetCounts();
    b = BenchRun{};
    g_benchEvictions = 0;
    temporalPassMakeEyeStamp(b.stamp, 32);
    for (const wchar_t* p = b.stamp; *p; ++p) b.stampA += static_cast<char>(*p);
    b.dir = dir;
    CreateDirectoryW(dir.c_str(), nullptr);
    b.pressMs = GetTickCount64();
    b.seat = journalSeatKind();
    b.phase = BenchPhase::Rect;
    b.m.stamp = b.stampA;
#ifndef EDVR_VERSION_STRING
#define EDVR_VERSION_STRING "unversioned test build"
#endif
    b.m.build = EDVR_VERSION_STRING;
    const unsigned preset = dlaaPresetNow();
    b.m.dlssPreset = preset >= 10 && preset <= 13 ? std::string(1, static_cast<char>('J' + (preset - 10))) : std::to_string(preset);
    b.m.dlssVersion = "the pinned NGX runtime (tools\\fetch_ngx.py)";
    b.m.overlayNote = "the eye captures precede the UI layer; the headset frames are taken after the press toast (2.5 s) is gone";
    int built = 0;
    for (int v = 0; v < hb::kVariants; ++v) {
        const char* nb = benchNotBuilt(v);
        b.m.variants[v].status = nb ? "refused" : "pending";
        b.m.variants[v].reason = nb ? nb : "";
        built += nb ? 0 : 1;
    }
    g_benchTimeAll = true;
    detail::g_hullBenchOn = true;
    Log::get().note("hull layer: bench %s armed (hotkey.hull_bench, TEMPORARY; docs\\research-own-hull-layer-2026-11-08.md \"Variant bench\"): hold still; "
                    "%d variants, %d built; files to %ls",
                    b.stampA.c_str(), static_cast<int>(hb::kVariants), built, dir.c_str());
    benchToast("Hull bench: hold still ~10 s");
}

void hullBenchCaptureEye(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* submit, bool dlss, uint32_t renderW, uint32_t renderH,
                         uint32_t outW, uint32_t outH, float jitterX, float jitterY, const float* tanNow, const float* headDelta) {
    if (!detail::g_hullBenchOn || !ctx || !submit || eye < 0 || eye > 1) return;
    BenchRun& b = g_bench;
    (dlss ? b.sawDlss : b.sawOther) = true;
    if (b.phase == BenchPhase::Rect) {
        if (eye == 0) {
            b.renderW = renderW;
            b.renderH = renderH;
            b.outW = outW;
            b.outH = outH;
        }
        return;
    }
    if (!benchRunning()) return;
    if (renderW != b.renderW || renderH != b.renderH || outW != b.outW || outH != b.outH) {
        b.abortWhy = "the eye size changed";
        return;
    }
    if (!dlss) {
        b.abortWhy = "the eyes' upscaler is no longer DLSS";
        return;
    }
    if (b.phase != BenchPhase::Capture || b.capIndex < 0) return;
    VrWorldInternalScope internal;   // the bench's own copy
    if (eye == 0) {
        const std::string name = hb::frameName(b.stampA, b.var, b.capIndex, 0);
        if (benchStage(ctx, submit, b.rectO[0], b.rectO[1], b.rectO[2], b.rectO[3], name, g_frame, 0)) {
            b.staged0 = true;
            hb::Frame& f = b.pendingFrame;
            f.index = b.capIndex;
            f.layerFrame = g_frame;
            f.file = name;
            f.jitter[0] = jitterX;
            f.jitter[1] = jitterY;
            f.headRotDeg = headRotDeg(headDelta);
            if (tanNow) std::memcpy(f.tanNow, tanNow, sizeof f.tanNow);
        }
    } else if (b.capIndex == 0) {
        benchStage(ctx, submit, b.rectO[0], b.rectO[1], b.rectO[2], b.rectO[3], hb::frameName(b.stampA, b.var, 0, 1), g_frame, 1);
    }
}

void hullBenchCaptureFinal(uint32_t eye, ID3D11Texture2D* texture, const uint32_t region[4], bool flipU, bool flipV) {
    if (!detail::g_hullBenchOn || eye > 1 || !texture || !region) return;
    BenchRun& b = g_bench;
    if (!b.finalWant[eye] || b.var < 0 || b.var >= hb::kVariants || !b.outW || !b.outH) return;
    b.finalWant[eye] = false;
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    if (d.ArraySize != 1 || region[2] <= region[0] || region[3] <= region[1]) return;
    // The rect in the headset frame: the region is the eye at its submitted size (the output size, or scaled), flipped as told.
    const double sx = static_cast<double>(region[2] - region[0]) / b.outW, sy = static_cast<double>(region[3] - region[1]) / b.outH;
    uint32_t x = static_cast<uint32_t>(b.rectO[0] * sx), y = static_cast<uint32_t>(b.rectO[1] * sy);
    const uint32_t w = std::max(1u, static_cast<uint32_t>(b.rectO[2] * sx)), h = std::max(1u, static_cast<uint32_t>(b.rectO[3] * sy));
    const uint32_t rw = region[2] - region[0], rh = region[3] - region[1];
    if (flipU) x = rw > x + w ? rw - (x + w) : 0u;
    if (flipV) y = rh > y + h ? rh - (y + h) : 0u;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    texture->GetDevice(&dev);
    if (dev) dev->GetImmediateContext(&ctx);
    if (ctx && dev == g_device) {
        VrWorldInternalScope internal;
        const std::string name = hb::finalName(b.stampA, b.var, static_cast<int>(eye));
        if (benchStage(ctx, texture, region[0] + x, region[1] + y, w, h, name, g_frame, eye)) {
            b.m.variants[b.var].finalFiles[eye] = name;
            b.m.variants[b.var].finalFlip[eye][0] = flipU;
            b.m.variants[b.var].finalFlip[eye][1] = flipV;
        }
    }
    releaseRef(ctx);
    releaseRef(dev);
}

int hullBenchTimeBegin(ID3D11DeviceContext* ctx, int kind) {
    if (!detail::g_hullBenchOn || !ctx || !g_device || !benchRunning()) return -1;
    for (uint32_t i = 0; i < kBenchQueries; ++i) {
        BenchQuery& q = g_benchQ[i];
        if (q.open || q.pending) continue;
        if (!q.dj) {
            D3D11_QUERY_DESC qd{};
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            D3D11_QUERY_DESC qt{};
            qt.Query = D3D11_QUERY_TIMESTAMP;
            if (FAILED(g_device->CreateQuery(&qd, &q.dj)) || FAILED(g_device->CreateQuery(&qt, &q.t0)) || FAILED(g_device->CreateQuery(&qt, &q.t1))) {
                releaseRef(q.dj);
                releaseRef(q.t0);
                releaseRef(q.t1);
                return -1;
            }
        }
        ctx->Begin(q.dj);
        ctx->End(q.t0);
        q.open = true;
        q.kind = kind;
        q.frame = g_frame;
        return static_cast<int>(i);
    }
    return -1;
}

void hullBenchTimeEnd(ID3D11DeviceContext* ctx, int handle) {
    if (handle < 0 || handle >= static_cast<int>(kBenchQueries) || !ctx) return;
    BenchQuery& q = g_benchQ[handle];
    if (!q.open) return;
    ctx->End(q.t1);
    ctx->End(q.dj);
    q.open = false;
    q.pending = true;
}

}  // namespace edvr

#pragma once

namespace edvr {

// SMAA 1x over the HUD layer's finished 8-bit content, in the flat profile, before the composite downsamples it (fix.hud_antialiasing;
// docs/design-flat-ui-quality-2026-10-05.md, "2026-10-24: edge smoothing for the HUD layer"). The layer is composited after the game's anti-aliasing, so
// the game's own SMAA or DLSS never sees the HUD; this runs the same SMAA on the layer image instead.
//
// The three passes are the map's (map_smaa_shader.h): the same prologue, the same SMAA chunks (SMAA.hlsl, unmodified, SMAA_PRESET_HIGH), and for
// passes 2 and 3 the map's own entries (smaaWeights, smaaBlend), which are plain SMAA with no colour transform. Only the edge test differs, and it is
// the one entry here. The layer holds premultiplied light in rgb and TRANSMITTANCE in alpha, cleared to (0, 0, 0, 1) and composited as
// out = L.rgb + F.rgb * L.a: a dark opaque backing is rgb 0 and a 0 against the clear's rgb 0 and a 1, so only alpha shows that edge. SMAA's own colour
// edge test looks at rgb alone; this one takes the colour distance over all four channels. Everything else in it -- the threshold, the steps, the local
// contrast adaptation -- is SMAAColorEdgeDetectionPS line for line (SMAA.hlsl), so SMAA.hlsl itself stays as upstream wrote it.
//
// The blend writes all four channels with no transform: a linear mix of premultiplied rgb and transmittance is what the composite's own box filter
// does to them, so it is exact for this composite. The layer is read with Load (the point sampler's clamp becomes an explicit clamp), and a texel with no
// edge writes zero edges, as the map's compute edge entry does, so the pass covers every pixel and nothing is left over from the last frame.
inline constexpr char kUiLayerSmaaEdgesHlsl[] = R"HLSL(
Texture2D UiSmaaLayer : register(t0);
RWTexture2D<float2> UiSmaaEdges : register(u0);

float4 uiSmaaTexel(int2 p) {
    return UiSmaaLayer.Load(int3(clamp(p, int2(0, 0), int2(smaaMetrics.zw) - 1), 0));
}

float uiSmaaDelta(float4 a, float4 b) {
    const float4 t = abs(a - b);
    return max(max(t.r, t.g), max(t.b, t.a));
}

float2 uiSmaaEdgeTest(int2 p) {
    const float2 threshold = float2(SMAA_THRESHOLD, SMAA_THRESHOLD);
    const float4 C = uiSmaaTexel(p);
    float4 delta;
    delta.x = uiSmaaDelta(C, uiSmaaTexel(p + int2(-1, 0)));
    delta.y = uiSmaaDelta(C, uiSmaaTexel(p + int2(0, -1)));
    float2 edges = step(threshold, delta.xy);
    if (dot(edges, float2(1.0, 1.0)) == 0.0)
        return float2(0.0, 0.0);
    delta.z = uiSmaaDelta(C, uiSmaaTexel(p + int2(1, 0)));
    delta.w = uiSmaaDelta(C, uiSmaaTexel(p + int2(0, 1)));
    float2 maxDelta = max(delta.xy, delta.zw);
    delta.z = uiSmaaDelta(C, uiSmaaTexel(p + int2(-2, 0)));
    delta.w = uiSmaaDelta(C, uiSmaaTexel(p + int2(0, -2)));
    maxDelta = max(maxDelta.xy, delta.zw);
    const float finalDelta = max(maxDelta.x, maxDelta.y);
    edges.xy *= step(finalDelta, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta.xy);
    return edges;
}

[numthreads(8, 8, 1)]
void uiSmaaEdges(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(smaaMetrics.z) || id.y >= uint(smaaMetrics.w)) return;
    UiSmaaEdges[id.xy] = uiSmaaEdgeTest(int2(id.xy));
}
)HLSL";

// The tiled path (2026-11-11, docs/ui-layer-2026-09-23.md "2026-11-11: the HUD SMAA tiled"): the same SMAA, bit for bit, with the weights, the blend and a
// copy back into the layer run only over the 8x8 tiles that need them. A HUD layer is mostly the clear, and the full passes' cost was group launch over it.
//
// Why a tile list is exact (SMAA.hlsl as upstream wrote it):
//   - SMAABlendingWeightCalculationPS is zero at a pixel whose own edges (the texel at texcoord, a pixel centre) are both zero: both branches test e.g and
//     e.r of that texel, and weights start at zero. So the weights are non-zero only at edge pixels.
//   - SMAANeighborhoodBlendingPS at q reads the weights at q (.x, .z), q + (1, 0) (.a) and q + (0, 1) (.g), all at pixel centres; with all four zero it
//     returns the layer's own texel at q. So the blend changes q only when q, q + (1, 0) or q + (0, 1) has an edge.
// Hence: an edge pixel p marks tile(p), tile(p - (1, 0)) and tile(p - (0, 1)); a marked ("now") tile gets the weights, the blend and the copy back
// as the full passes would compute them; every other pixel's blend would return the layer's texel, which the layer already holds.
//
// The edges pass stays a full dispatch (the weights' searches read edges far outside a tile), in 16x16 groups (a quarter of the 8x8 launches; same
// per-pixel arithmetic, the shared uiSmaaEdgeTest above), and marks tiles through a group-shared 3x3 (the group's four tiles, the two to its left
// and the two above it; the top-left corner is never marked).
inline constexpr char kUiLayerSmaaEdgesMarkHlsl[] = R"HLSL(
RWTexture2D<uint> UiSmaaMark : register(u1);
groupshared uint uiSmaaGroupMarks[9];

void uiSmaaMarkLocal(int2 local) {
    // local: a pixel's position within the group, -1 for the column left of it or the row above it. (local + 8) / 8 - 1 is floor(local / 8);
    // local + 8 is never negative, so the divide is unsigned.
    const int2 t = int2(uint2(local + 8) / 8u) - 1;
    InterlockedOr(uiSmaaGroupMarks[(t.y + 1) * 3 + (t.x + 1)], 1u);
}

[numthreads(16, 16, 1)]
void uiSmaaEdgesMark(uint3 id : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    if (gi < 9u) uiSmaaGroupMarks[gi] = 0u;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < uint(smaaMetrics.z) && id.y < uint(smaaMetrics.w)) {
        const float2 e = uiSmaaEdgeTest(int2(id.xy));
        UiSmaaEdges[id.xy] = e;
        if (e.x > 0.0 || e.y > 0.0) {
            const int2 l = int2(gtid.xy);
            uiSmaaMarkLocal(l);
            uiSmaaMarkLocal(l - int2(1, 0));
            uiSmaaMarkLocal(l - int2(0, 1));
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi < 9u && uiSmaaGroupMarks[gi] != 0u) {
        const int2 t = int2(gid.xy) * 2 + int2(int(gi % 3u), int(gi / 3u)) - 1;
        if (t.x >= 0 && t.y >= 0) InterlockedOr(UiSmaaMark[uint2(t)], 1u);
    }
}
)HLSL";

// One thread a tile: state = (state << 1 | mark) & 3, the mark cleared. A tile with state != 0 (edges this run, or last run) is appended as
// x | y << 16 | now << 31. A listed tile without `now` gets zero weights; an unlisted tile's weights are already zero, since its last write was a zero
// (the weights texture is cleared whenever the tiled state is (re)started: made, re-made, or after a full run).
inline constexpr char kUiLayerSmaaTileListHlsl[] = R"HLSL(
RWTexture2D<uint> UiSmaaMarkIn : register(u0);
RWTexture2D<uint> UiSmaaState : register(u1);
AppendStructuredBuffer<uint> UiSmaaList : register(u2);

[numthreads(8, 8, 1)]
void uiSmaaTileList(uint3 id : SV_DispatchThreadID) {
    const uint2 tn = (uint2(smaaMetrics.zw) + 7u) / 8u;
    if (id.x >= tn.x || id.y >= tn.y) return;
    const uint now = UiSmaaMarkIn[id.xy] & 1u;
    UiSmaaMarkIn[id.xy] = 0u;
    const uint b = ((UiSmaaState[id.xy] << 1) | now) & 3u;
    UiSmaaState[id.xy] = b;
    if (b != 0u) UiSmaaList.Append(id.x | (id.y << 16) | (now << 31));
}
)HLSL";

// The list, read by the three indirect passes: one 8x8 group a listed tile.
inline constexpr char kUiLayerSmaaTilesHlsl[] = R"HLSL(
StructuredBuffer<uint> UiSmaaTiles : register(t3);

uint2 uiSmaaTilePixel(uint3 gid, uint3 gtid, out bool now) {
    const uint t = UiSmaaTiles[gid.x];
    now = (t >> 31) != 0u;
    return uint2(t & 0xFFFFu, (t >> 16) & 0x7FFFu) * 8u + gtid.xy;
}
)HLSL";

// Built after kMapSmaaWeightsHlsl and kUiLayerSmaaTilesHlsl: smaaWeights' body for a `now` tile, zero for a tile that had edges last run only.
inline constexpr char kUiLayerSmaaWeightsTiledHlsl[] = R"HLSL(
[numthreads(8, 8, 1)]
void uiSmaaWeightsTiled(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    bool now;
    const uint2 p = uiSmaaTilePixel(gid, gtid, now);
    if (p.x >= uint(smaaMetrics.z) || p.y >= uint(smaaMetrics.w)) return;
    if (!now) {
        MapSmaaWeights[p] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    const float2 texcoord = (float2(p) + 0.5) * smaaMetrics.xy;
    float2 pixcoord;
    float4 offset[3];
    SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);
    MapSmaaWeights[p] = SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, MapSmaaEdgesIn, MapSmaaArea, MapSmaaSearch,
                                                       float4(0.0, 0.0, 0.0, 0.0));
}
)HLSL";

// Built after kMapSmaaBlendHlsl and kUiLayerSmaaTilesHlsl: smaaBlend's body over the `now` tiles, into the output.
inline constexpr char kUiLayerSmaaBlendTiledHlsl[] = R"HLSL(
[numthreads(8, 8, 1)]
void uiSmaaBlendTiled(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    bool now;
    const uint2 p = uiSmaaTilePixel(gid, gtid, now);
    if (!now || p.x >= uint(smaaMetrics.z) || p.y >= uint(smaaMetrics.w)) return;
    const float2 texcoord = (float2(p) + 0.5) * smaaMetrics.xy;
    float4 offset;
    SMAANeighborhoodBlendingVS(texcoord, offset);
    MapSmaaBlended[p] = SMAANeighborhoodBlendingPS(texcoord, offset, MapSmaaColorIn, MapSmaaWeightsIn);
}
)HLSL";

// Built after kUiLayerSmaaTilesHlsl: the output's `now` tiles stored back into the layer (a typed store; nothing loads the layer's UAV).
inline constexpr char kUiLayerSmaaCopyBackHlsl[] = R"HLSL(
Texture2D<float4> UiSmaaOut : register(t0);
RWTexture2D<unorm float4> UiSmaaLayerOut : register(u0);

[numthreads(8, 8, 1)]
void uiSmaaCopyBack(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    bool now;
    const uint2 p = uiSmaaTilePixel(gid, gtid, now);
    if (!now || p.x >= uint(smaaMetrics.z) || p.y >= uint(smaaMetrics.w)) return;
    UiSmaaLayerOut[p] = UiSmaaOut.Load(int3(p, 0));
}
)HLSL";

}  // namespace edvr

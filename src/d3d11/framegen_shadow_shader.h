#pragma once
// The frame-generation shadow scorer's compute shaders (fix.frame_generation_shadow, TEMPORARY; docs/design-frame-generation-2026-10-26.md, Phase 1; the plan's
// "GPU passes"). One source, five entry points, one constant block (src\common\framegen_shadow_math.h FrameGenShadowCb):
//   fgShadowCapture  every frame: the quarter-size luma of the upscaler's output and the quarter-size dilated depth of the frame, into this frame's slot;
//   fgShadowClear    the destination key grid and the stats words to 0;
//   fgShadowSplat    each previous-frame cell carried FORWARD by its class's transform, scattered as a key into the cells of its bilinear footprint (nearest wins);
//   fgShadowScore    each cell of this frame carried BACKWARD by the key's class, the previous luma sampled there, the error binned.
//   fgShadowScoreB   dispatched after it on every scored frame (the second kernel): each cell of this frame classed by this frame's OWN depth and carried back to the
//                    previous frame by the DLSS motion vector (the mv set: the ceiling the vectors give a warp), scored as above. It does not use the key grid.
// The Phase 1 measurement sets that were done (the oracle set, the bdepth set and the landings' disagreement, the ex2d and ex2dh extrapolation sets with their vector
// capture, the rows-off shares) were taken out on 2026-11-07 for the VRAM they held; the journal has their results.
// The pixel class and the carry are the temporal pass's own text: the source the build compiles is the text between the two EDVR_TEMPORAL_CARRY markers of
// kTemporalCsHlsl, then kFrameGenShadowCs (frameGenShadowShaderSource). Not a copy: the markers are read out of kTemporalCsHlsl by the build tool and the rig,
// so an edit to the pass's functions is an edit to the scorer's, and a missing marker fails the build.
//
// Bindings: t0 the upscaler output, t1 the depth copy, t2 the previous luma, t3 the previous depth, t4 this frame's luma, t5 the DLSS motion vectors (R16G16_FLOAT,
// render size, pixels, this frame -> previous, jitter-free: the vector pass adds the raster jitter difference back so DLSS gets the unjittered motion), t6 this frame's
// captured depth; u0 luma out, u1 depth out, u2 the key grid (R32_UINT), u3 the stats words; b0 P.
//
// The key: 0 a hole, 4 the far plane, else (max(asuint(1 / metres), 8) & ~3) | class, class 1 the cockpit (the head's rows), 2 the world (the camera's).
// A larger key is a nearer surface, so InterlockedMax is the depth test.
//
// Stats words (src\common\framegen_shadow_math.h): two blocks (the main score, the mv set), each per class 48 bins of the error, 48 of the no-warp error, pixels,
// sum x 1024, no-warp sum x 1024, then the block's holes and cells.
#include <cstring>
#include <stdexcept>
#include <string>

#include "temporal_shader_source.h"

namespace edvr {

inline constexpr char kFrameGenShadowCs[] = R"HLSL(
cbuffer P : register(b0) {
    int4   sz;       // render width, height, output width, height
    int4   qz;       // quarter grid width, height
    float4 tanA;     // the previous frame's tangents
    float4 tanB;     // this frame's
    float4 dk;       // depth A, depth B, split metres
    float4 fw[9];    // forward rows, [class * 3 + row], w the translation
    float4 bw[9];    // backward rows
};
Texture2D<float4> Out    : register(t0);
Texture2D<float>  Depth  : register(t1);
Texture2D<float>  LumaA  : register(t2);
Texture2D<float>  DepthA : register(t3);
Texture2D<float>  LumaB  : register(t4);
Texture2D<float2> Mv     : register(t5);
Texture2D<float>  DepthB : register(t6);
RWTexture2D<float> LumaOut  : register(u0);
RWTexture2D<float> DepthOut : register(u1);
RWTexture2D<uint>  Key      : register(u2);
RWStructuredBuffer<uint> Stats : register(u3);

static const uint kBins = 48;
static const uint kPerClass = 99;       // 2 * kBins + 3
static const uint kHoles = 297;         // 3 * kPerClass
static const uint kBlockUints = 299;    // one block: 3 classes, holes, cells
static const uint kMvBase = 299;
static const uint kStatsUints = 598;    // src\common\framegen_shadow_math.h kFrameGenShadowStatsUints

float fgLuma(float3 c) { return clamp(dot(c, float3(0.2126, 0.7152, 0.0722)), 0.0, 1.0); }

// A bilinear read of the output at a continuous output-pixel position (texel centres at +0.5).
float3 fgOutBilinear(float2 u) {
    float2 f = u - 0.5;
    int2 i0 = int2(floor(f));
    float2 w = f - float2(i0);
    int2 hi = int2(sz.z, sz.w) - 1;
    int2 a = clamp(i0, int2(0, 0), hi);
    int2 b = clamp(i0 + 1, int2(0, 0), hi);
    float3 c00 = Out.Load(int3(a.x, a.y, 0)).rgb;
    float3 c10 = Out.Load(int3(b.x, a.y, 0)).rgb;
    float3 c01 = Out.Load(int3(a.x, b.y, 0)).rgb;
    float3 c11 = Out.Load(int3(b.x, b.y, 0)).rgb;
    return lerp(lerp(c00, c10, w.x), lerp(c01, c11, w.x), w.y);
}

// A bilinear read of a quarter-grid texture at a continuous cell position (cell centres at integers).
float fgCellBilinear(Texture2D<float> t, float2 c) {
    int2 i0 = int2(floor(c));
    float2 w = c - float2(i0);
    int2 hi = int2(qz.x, qz.y) - 1;
    int2 a = clamp(i0, int2(0, 0), hi);
    int2 b = clamp(i0 + 1, int2(0, 0), hi);
    float v00 = t.Load(int3(a.x, a.y, 0));
    float v10 = t.Load(int3(b.x, a.y, 0));
    float v01 = t.Load(int3(a.x, b.y, 0));
    float v11 = t.Load(int3(b.x, b.y, 0));
    return lerp(lerp(v00, v10, w.x), lerp(v01, v11, w.x), w.y);
}

bool fgInImage(float2 pp) {
    return pp.x >= 0.0 && pp.y >= 0.0 && pp.x <= float(sz.x) - 1.0 && pp.y <= float(sz.y) - 1.0;
}

uint fgBin(float e) {
    if (!(e >= 1.0 / 1024.0)) return 0u;
    float l = 4.0 * log2(e * 1024.0);
    if (!(l < 46.0)) return kBins - 1;
    return uint(clamp(1 + int(floor(l)), 1, int(kBins) - 1));
}

[numthreads(8, 8, 1)]
void fgShadowCapture(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(qz.x) || id.y >= uint(qz.y)) return;
    float2 scale = float2(sz.z, sz.w) / float2(sz.x, sz.y);
    float2 r0 = float2(id.xy) * 4.0;
    float lum = 0.0;
    [unroll] for (int ty = 0; ty < 2; ++ty) {
        [unroll] for (int tx = 0; tx < 2; ++tx) {
            float2 r = r0 + float2(1.0 + 2.0 * tx, 1.0 + 2.0 * ty);
            lum += fgLuma(fgOutBilinear(r * scale));
        }
    }
    float z = 0.0;
    [unroll] for (int y = 0; y < 4; ++y) {
        [unroll] for (int x = 0; x < 4; ++x) {
            int2 q = clamp(int2(id.xy) * 4 + int2(x, y), int2(0, 0), int2(sz.x, sz.y) - 1);
            z = max(z, Depth.Load(int3(q.x, q.y, 0)));
        }
    }
    LumaOut[id.xy] = lum * 0.25;
    DepthOut[id.xy] = z;
}

[numthreads(8, 8, 1)]
void fgShadowClear(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(qz.x) || id.y >= uint(qz.y)) return;
    Key[id.xy] = 0u;
    // The grid may be smaller than the stats words (the rig's is 768 cells): each cell clears every (cells)-th word.
    uint cells = uint(qz.x) * uint(qz.y);
    for (uint i = id.y * uint(qz.x) + id.x; i < kStatsUints; i += cells) Stats[i] = 0u;
}

[numthreads(8, 8, 1)]
void fgShadowSplat(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(qz.x) || id.y >= uint(qz.y)) return;
    float zr = DepthA.Load(int3(id.x, id.y, 0));
    bool far;
    float z = temporalDepthMetres(zr, dk.x, dk.y, far);
    uint cls = far ? 0u : (temporalWorldPath(true, far, z, dk.z, false) ? 2u : 1u);
    float3 d = temporalPixelDir(float2(id.xy) * 4.0 + 1.5, tanA, int2(sz.x, sz.y));
    float3 t = float3(fw[cls * 3].w, fw[cls * 3 + 1].w, fw[cls * 3 + 2].w);
    float3 dp = temporalCarry(d, z, far, fw[cls * 3].xyz, fw[cls * 3 + 1].xyz, fw[cls * 3 + 2].xyz, t);
    float2 pp;
    if (!temporalProjectPixel(dp, tanB, int2(sz.x, sz.y), pp)) return;
    uint key = 4u;
    if (!far) {
        float zB = -dp.z;
        key = (max(asuint(1.0 / max(zB, 1e-6)), 8u) & ~3u) | cls;
    }
    float2 cq = (pp - 1.5) * 0.25;
    int2 base = int2(floor(cq));
    [unroll] for (int dy = 0; dy < 2; ++dy) {
        [unroll] for (int dx = 0; dx < 2; ++dx) {
            int2 c = base + int2(dx, dy);
            if (c.x >= 0 && c.y >= 0 && c.x < qz.x && c.y < qz.y) InterlockedMax(Key[uint2(c)], key);
        }
    }
}

groupshared uint gStats[299];   // kBlockUints

[numthreads(8, 8, 1)]
void fgShadowScore(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    for (uint i = gi; i < kBlockUints; i += 64) gStats[i] = 0u;
    GroupMemoryBarrierWithGroupSync();
    bool active = id.x < uint(qz.x) && id.y < uint(qz.y);
    uint hole = 0u, cls = 0u, bin = 0u, binId = 0u, sumE = 0u, sumId = 0u;
    if (active) {
        uint key = Key[id.xy];
        float lumaB = LumaB.Load(int3(id.x, id.y, 0));
        hole = 1u;
        if (key != 0u) {
            bool far = key == 4u;
            cls = far ? 0u : (key & 3u);
            float z = far ? 0.0 : 1.0 / max(asfloat(key & ~3u), 1e-20);
            float3 d = temporalPixelDir(float2(id.xy) * 4.0 + 1.5, tanB, int2(sz.x, sz.y));
            float3 t = float3(bw[cls * 3].w, bw[cls * 3 + 1].w, bw[cls * 3 + 2].w);
            float3 dp = temporalCarry(d, z, far, bw[cls * 3].xyz, bw[cls * 3 + 1].xyz, bw[cls * 3 + 2].xyz, t);
            float2 pp;
            if (temporalProjectPixel(dp, tanA, int2(sz.x, sz.y), pp) && pp.x >= 0.0 && pp.y >= 0.0 &&
                pp.x <= float(sz.x) - 1.0 && pp.y <= float(sz.y) - 1.0) {
                float pred = fgCellBilinear(LumaA, (pp - 1.5) * 0.25);
                float e = abs(lumaB - pred);
                float eId = abs(lumaB - LumaA.Load(int3(id.x, id.y, 0)));
                bin = fgBin(e);
                binId = fgBin(eId);
                sumE = uint(e * 1024.0 + 0.5);
                sumId = uint(eId * 1024.0 + 0.5);
                hole = 0u;
            }
        }
    }
    // The shared-memory adds sit after every read and before the barrier, with no early exit in the group.
    if (active) {
        InterlockedAdd(gStats[kBlockUints - 1], 1u);
        if (hole != 0u) InterlockedAdd(gStats[kHoles], 1u);
        else {
            uint b = cls * kPerClass;
            InterlockedAdd(gStats[b + bin], 1u);
            InterlockedAdd(gStats[b + kBins + binId], 1u);
            InterlockedAdd(gStats[b + 2 * kBins], 1u);
            InterlockedAdd(gStats[b + 2 * kBins + 1], sumE);
            InterlockedAdd(gStats[b + 2 * kBins + 2], sumId);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint j = gi; j < kBlockUints; j += 64) {
        uint v = gStats[j];
        if (v != 0u) InterlockedAdd(Stats[j], v);
    }
}

// ---- the second kernel ----
groupshared uint gStatsB[299];   // kBlockUints: the mv set's block

// One landing's score: the error against the previous luma at pp, the way fgShadowScore bins it (and the no-warp error at the same cell).
void fgScoreLanding(uint cls, bool inside, float2 pp, float lumaB, float lumaA0) {
    if (!inside) {
        InterlockedAdd(gStatsB[kHoles], 1u);
        return;
    }
    float e = abs(lumaB - fgCellBilinear(LumaA, (pp - 1.5) * 0.25));
    float eId = abs(lumaB - lumaA0);
    uint b = cls * kPerClass;
    InterlockedAdd(gStatsB[b + fgBin(e)], 1u);
    InterlockedAdd(gStatsB[b + kBins + fgBin(eId)], 1u);
    InterlockedAdd(gStatsB[b + 2 * kBins], 1u);
    InterlockedAdd(gStatsB[b + 2 * kBins + 1], uint(e * 1024.0 + 0.5));
    InterlockedAdd(gStatsB[b + 2 * kBins + 2], uint(eId * 1024.0 + 0.5));
}

[numthreads(8, 8, 1)]
void fgShadowScoreB(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    for (uint i = gi; i < kBlockUints; i += 64) gStatsB[i] = 0u;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < uint(qz.x) && id.y < uint(qz.y)) {
        // The class from this frame's own captured depth, by the pass's own rules (as fgShadowSplat does for the previous frame's).
        float zr = DepthB.Load(int3(id.x, id.y, 0));
        bool far;
        float z = temporalDepthMetres(zr, dk.x, dk.y, far);
        uint cls = far ? 0u : (temporalWorldPath(true, far, z, dk.z, false) ? 2u : 1u);
        float lumaB = LumaB.Load(int3(id.x, id.y, 0));
        float lumaA0 = LumaA.Load(int3(id.x, id.y, 0));
        float2 c = float2(id.xy) * 4.0 + 1.5;
        // mv: the DLSS vector (this frame -> previous, pixels) at the render texel nearest the cell centre; a vector the vector pass hid (twice the size) lands outside.
        int2 rt = clamp(int2(floor(c + 0.5)), int2(0, 0), int2(sz.x, sz.y) - 1);
        float2 pm = c + Mv.Load(int3(rt.x, rt.y, 0));
        InterlockedAdd(gStatsB[kBlockUints - 1], 1u);
        fgScoreLanding(cls, fgInImage(pm), pm, lumaB, lumaA0);
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint j = gi; j < kBlockUints; j += 64) {
        uint v = gStatsB[j];
        if (v != 0u) InterlockedAdd(Stats[kMvBase + j], v);
    }
}
)HLSL";

// The text the five entries compile: the pass's carry functions, cut out of kTemporalCsHlsl between the two markers, then the scorer's own. Throws when a
// marker is missing, doubled or out of order, so the build and the rig fail rather than compile a scorer without its carry.
inline std::string frameGenCarryText() {
    static const char kBegin[] = "// EDVR_TEMPORAL_CARRY begin\n";
    static const char kEnd[] = "// EDVR_TEMPORAL_CARRY end";
    const std::string source = kTemporalCsHlsl;
    const size_t b = source.find(kBegin), e = source.find(kEnd);
    if (b == std::string::npos || e == std::string::npos || e < b || source.find(kBegin, b + 1) != std::string::npos ||
        source.find(kEnd, e + 1) != std::string::npos)
        throw std::runtime_error("the EDVR_TEMPORAL_CARRY markers must appear exactly once each, in order");
    const size_t from = b + std::strlen(kBegin);
    std::string carry = source.substr(from, e - from);
    if (carry.find("register(") != std::string::npos)
        throw std::runtime_error("the EDVR_TEMPORAL_CARRY block must declare no resource (it is compiled into two shaders)");
    return carry;
}

inline const std::string& frameGenShadowShaderSource() {
    static const std::string s = frameGenCarryText() + kFrameGenShadowCs;
    return s;
}

}  // namespace edvr

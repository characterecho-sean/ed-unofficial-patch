#pragma once
// The frame-generation synthesizer's compute shaders (fix.frame_generation_shadow, TEMPORARY; docs/design-frame-generation-2026-10-26.md, Phase 2.1, "The synthesis pass").
// One source, seven entry points (and Phase 2.3b's eighth, fgSynFinish, outside the pass sequence), one constant block (src\common\framegen_synth_math.h FrameGenSynthCb). Nothing they write is shown in the headset. Before them the key grid and the
// stats words are cleared to 0 by the API (ClearUnorderedAccessViewUint, framegen_synth_pass.h); the read offsets are set to all ones by fgSynSplat (each thread its own
// pixel; the S grid is the render grid): a pixel whose key is 0 has kNoRead written by fgSynResolve, and a pixel whose key is not 0 has its offset written by fgSynSrc (the
// pixel that set the key finds it there again and offers it; of tied offers the smallest packed value stands, InterlockedMin, so S does not depend on the order they run in).
//   fgSynSplat     each B pixel carried to S by its class's motion (fgSynTarget; its class and depth are the nearest of the 3x3 around it, synDepth9, or of the 5x5's ring for the far plane beside a surface, synHalo, taken where they are read) and
//                  scattered as a key into the four pixels of its bilinear footprint (InterlockedMax: the nearer surface wins; a pixel that is its own nearest beats one that borrowed
//                  the dilated depth);
//   fgSynSrc       the second pass over the same keys: each pixel writes, to every footprint pixel whose final key is its own, that S pixel's READ OFFSET: the B position to read,
//                  as an offset from the S pixel's own centre, two half floats packed in a uint (kMaxOffset, kNoRead; synPackOffset rounds to nearest even as a typed half store
//                  does). The read is the pixel's own position moved by the landing offset (the S pixel minus where the pixel landed), or the pixel's own position itself when the
//                  guard says so (a nearer 3x3 neighbour on the side the offset points to would be read across the edge), then an integer pair stored exactly with the guard's
//                  mark (synPackGuarded, larger than any unguarded value, so a tie keeps the unguarded read); a read past kMaxOffset is clamped and counted;
//   fgSynResolve   every S pixel no key reached is a hole: kNoRead written, counted as left, its index listed in the hole list (u0, an eighth of the render pixels; the count in the
//                  stats words); so is a pixel with a key and no read offer (an orphan, the splat's all-ones still there: fgSynSrc's landing disagreeing with fgSynSplat's,
//                  FG_ORPHAN), counted as an orphan too; the S pixels whose stored offset carries the guard's mark are counted as guarded, one each;
//   fgSynHoles     one thread per listed hole, in full 64-thread groups: the farthest key found within 8 px of it is the background, and the hole is carried back into B by that
//                  key's read offset (its motion); B there the background: read there (reprojected); occluded there too: a stretch along the background's motion relative to the
//                  occluder to the first background texel (stretched); neither: the neighbour's read 2 px further into its own surface (the background pull, the fill before
//                  2026-11-07). That read becomes its offset, counted as filled and no longer left. The neighbour's read is taken in full precision, as fgSynSrc computed it before the store rounded it
//                  (synWinnerRead finds the neighbour's source pixel again from the stored offset and recomputes its read), so the fill is the same as when every read was computed
//                  in one pass; the counters (filled, left, guard, clamped) of the three passes add up in the stats words;
//   fgSynResample  output size: each output pixel to its S pixel, its read offset, B's output there, Catmull-Rom in 8 fetches with its ringing clamped (synSharp, FG_SHARP),
//                  into the S texture (dlOut's format);
//   fgSynLuma      quarter grid: S's luma the way the scorer's capture takes it, B's luma and B's class (the f = 1 S is kept for the next real frame);
//   fgSynScore     quarter grid, on the next real frame C: C's luma against the kept S luma and against B's (the no-warp comparison), binned per class of B as the scorer bins; and again
//                  against the kept S luma TURNED by the residual head rotation (the compositor's step: C's cell direction rotated into the eye S was built for, projected with the
//                  tangents S was built with, a bilinear read of the quarter luma; outside S: not scored, counted).
// The pixel class and the carry are the temporal pass's own text: the source the build compiles is the text between the two EDVR_TEMPORAL_CARRY markers of kTemporalCsHlsl, then
// kFrameGenSynthCs (frameGenSynthShaderSource); the markers are read out of kTemporalCsHlsl by frameGenCarryText (framegen_shadow_shader.h).
//
// Bindings: t0 B's output (the upscaler's, before the interface; on C's frame the score reads C's there), t1 B's depth copy, t2 the DLSS vectors (R16G16_FLOAT, render size, pixels,
// this frame to the previous, jitter-free), t3 and t5 fgSynFinish's frame and UI layer (t10 its multiply, u2 its output: Phase 2.3b, at the end of the text), t4 the key grid, t6 the read offsets, t7 the S texture, t8 the kept S luma, t9 the kept B luma (x) and class (y);
// u0 the hole list (a structured buffer of uints), u1 the key grid (R32_UINT), u2 fgSynFinish's output, u3 the read offsets (R32_UINT: two packed half floats, or a guarded read's integer
// pair with the mark; the resolve and the hole pass read other pixels' through this view, a typed uint load every D3D11 part does), u4 the S texture, u5 the stats words, u6 the
// S luma, u7 B's luma and class; b0 P; s0 a linear clamped sampler (framegen_synth_pass.h frameGenSynthCreateSampler), the resample's paired taps.
//
// The key: 0 a hole, 4 the far plane, else (max(asuint(1 / metres), 8) & ~7) | 4 when the pixel is its own nearest | class (1 cockpit, 2 world). A larger key is a nearer surface.
//
// The thirteen #defines at the top of kFrameGenSynthCs are the rig's mutation points (tools\framegen_synth_test replaces one with 0 and the case it guards must fail); production
// compiles them all 1.
#include <cstring>
#include <string>

#include "framegen_shadow_shader.h"

namespace edvr {

inline constexpr char kFrameGenSynthCs[] = R"HLSL(
#define FG_USE_F 1
#define FG_REMOVE_HEAD 1
#define FG_GUARD 1
#define FG_FILL_FAR 1
#define FG_FILL_REPROJ 1   // a hole is carried back by the background's motion, stretched along it where B is occluded there too (0: the background pull alone)
#define FG_DILATE_MV 1   // a pixel moves by the vector of the 3x3 pixel whose depth it took (0: its own)
#define FG_HALO 1       // a far-plane pixel 2 px from a nearer surface (outside the 3x3) takes that surface's depth and vector (0: the 3x3 alone)
#define FG_SENTINEL 1   // a rejected-history vector (the size * 2 sentinel) borrows its background's (0: taken as motion)
#define FG_SENTINEL_SURFACE 1   // ...from a ring pixel on its own surface first (0: the farthest ring pixel, whatever surface)
#define FG_TURN 1        // the score turns S by the residual head rotation (0: it scores it unturned)
#define FG_TURN_SIGN 1   // the residual is applied the right way round (0: its transpose)
#define FG_ORPHAN 1      // a pixel with a key and no read offer is a hole, filled (0: its all-ones read as a guarded offset, 2047 px off)
#define FG_SHARP 1       // S's resample is Catmull-Rom in 8 fetches, its corners dropped, its ringing clamped to the 2x2 around the read (0: bilinear; the bench's comparisons: 2 Lanczos-2 clamped, 3 Catmull-Rom unclamped, 4 clamped to the 4x4, 5 the Catmull-Rom as 16 loads, 6 with its corners in 12 fetches)
cbuffer P : register(b0) {
    int4   sz;       // render width, height, output width, height
    int4   qz;       // quarter grid width, height
    float4 tanB;     // this frame's tangents
    float4 dk;       // depth A, depth B, split metres, f
    float4 rs[3];    // the head's step B to S: rotation rows, w the translation component
    float4 hp[3];    // the head's rotation B to A (Hd_B) rows
    float4 rr[3];    // the score's residual rotation: C's eye space to the eye space the kept S was built for
    float4 tanK;     // the tangents the kept S was built with
};
Texture2D<float4> Out     : register(t0);
Texture2D<float>  Depth   : register(t1);
Texture2D<float2> Mv      : register(t2);
Texture2D<uint>   KeyT    : register(t4);
Texture2D<uint>   ReadT   : register(t6);   // the read offsets, packed
Texture2D<float4> SIn     : register(t7);
Texture2D<float>  SLumaIn : register(t8);
Texture2D<float2> BInfoIn : register(t9);
RWStructuredBuffer<uint> Holes : register(u0);   // the render pixel index of every hole the resolve found, for fgSynHoles; the count is Stats[kHoleCount]
RWTexture2D<uint>   Key       : register(u1);
RWTexture2D<uint>   ReadOut   : register(u3);   // R32_UINT: two half floats (synPackOffset)
RWTexture2D<float4> SOut      : register(u4);
RWStructuredBuffer<uint> Stats : register(u5);
RWTexture2D<float>  SLumaOut  : register(u6);
RWTexture2D<float2> BInfoOut  : register(u7);
SamplerState LinClamp : register(s0);   // linear, clamp (framegen_synth_pass.h frameGenSynthCreateSampler): the resample's paired taps

static const uint  kBins = 48;
static const uint  kPerClass = 99;        // 2 * kBins + 3
static const uint  kHoles = 297;          // 3 * kPerClass
static const uint  kBlockUints = 299;     // the score block: 3 classes, holes, cells
static const uint  kCountBase = 299;      // filled, left, guard, clamped
static const uint  kTurnBase = 303;       // the turned score block
static const uint  kHoleCount = 602;      // the holes listed in Holes (kFrameGenSynthStatsHoles)
static const uint  kFillReproj = 603;     // the holes filled reprojected, then stretched (kFrameGenSynthStatsFillReproj)
static const uint  kOrphan = 605;         // the pixels with a key and no read offer, made holes (kFrameGenSynthStatsOrphan)
static const uint  kStatsUints = 606;     // src\common\framegen_synth_math.h kFrameGenSynthStatsUints
static const uint  kNoOffer = 0xFFFFFFFFu;   // fgSynSplat's clear of the read offsets: never an offer (the largest guarded value is 0xFCFFFFFF)
static const float kGuardMargin = 0.9;    // a 3x3 neighbour nearer by more than a tenth of the pixel's own distance
static const int   kFillRadius = 8;
static const float kFillShift = 2.0;
static const int   kFillStretch = 32;     // the fill's steps along the background's motion, 1 px each
static const float kFillBackFar = 4.0 / 3.0;   // the read is no farther than this many times the background's distance
static const float kFillBackMargin = 0.75;   // B is the background's surface at a read when no texel of its 2x2 is nearer than this share of the background neighbour's distance
static const float kNoRead = -4096.0;       // framegen_synth_math.h kFrameGenSynthNoRead
static const float kMaxOffset = 2000.0;    // kFrameGenSynthMaxOffset
static const uint  kGuardMark = 0x7C000000u;  // a packed offset whose y half has this exponent field (a NaN's, never a clamped offset's) is a guarded read's: an integer pair, stored as such

float synLuma(float3 c) { return clamp(dot(c, float3(0.2126, 0.7152, 0.0722)), 0.0, 1.0); }

// A float's half-float bits, rounded toward zero: what the typed R16G16_FLOAT store the offsets went through before they shared a uint with the source grid's slot did on the
// pilot's NVIDIA card (D3D leaves a store's float16 rounding to the implementation; the replays of 2026-11-07 match that store byte for byte with truncation and not with
// round-to-nearest-even), so the offsets packed here are the ones that store kept. Finite inputs (the offsets are clamped). synUnpackOffset is exact (f16tof32).
uint synHalfBits(float v) {
    uint b = asuint(v);
    uint sign = (b >> 16) & 0x8000u;
    uint a = b & 0x7FFFFFFFu;
    if (a >= 0x47800000u) return sign | 0x7C00u;                             // 65536 and up: infinity
    if (a < 0x38800000u) return sign | uint(asfloat(a) * 16777216.0);       // under 2^-14: a half denormal or zero, in units of 2^-24, truncated
    return sign | (((a >> 23) - 112u) << 10) | ((a >> 13) & 0x3FFu);
}
uint synPackOffset(float2 o) { return synHalfBits(o.x) | (synHalfBits(o.y) << 16); }

// A guarded read's offset (the source pixel minus the S pixel: integers, within the clamp) with the mark and the sign bit above it, x + 2048 in bits 0-11, y + 2048 in bits
// 12-23: every guarded value is larger than every unguarded one, so of two sources that tie for a pixel fgSynSrc's InterlockedMin keeps an unguarded read. The mark is how
// fgSynResolve counts the guarded pixels, once each.
uint synPackGuarded(float2 o) {
    uint x = uint(int(o.x) + 2048), y = uint(int(o.y) + 2048);
    return 0x80000000u | kGuardMark | (x & 0xFFFu) | ((y & 0xFFFu) << 12);
}
bool synIsGuarded(uint p) { return (p & kGuardMark) == kGuardMark; }
float2 synUnpackOffset(uint p) {
    if (synIsGuarded(p)) return float2(int(p & 0xFFFu) - 2048, int((p >> 12) & 0xFFFu) - 2048);
    return float2(f16tof32(p & 0xFFFFu), f16tof32(p >> 16));
}

// A bilinear read of a render-or-output size float4 texture at a continuous position (texel centres at +0.5), edges clamped.
float4 synBilinear(Texture2D<float4> t, float2 u, int2 size) {
    float2 f = u - 0.5;
    int2 i0 = int2(floor(f));
    float2 w = f - float2(i0);
    int2 hi = size - 1;
    int2 a = clamp(i0, int2(0, 0), hi);
    int2 b = clamp(i0 + 1, int2(0, 0), hi);
    float4 c00 = t.Load(int3(a.x, a.y, 0));
    float4 c10 = t.Load(int3(b.x, a.y, 0));
    float4 c01 = t.Load(int3(a.x, b.y, 0));
    float4 c11 = t.Load(int3(b.x, b.y, 0));
    return lerp(lerp(c00, c10, w.x), lerp(c01, c11, w.x), w.y);
}

// S's resample (fgSynResample): a 4x4 read of B's output at a continuous position, Catmull-Rom (FG_SHARP), the result clamped per channel to the 2x2 texels around the
// position so an edge does not ring. A bilinear read of an 8-bit source at a sub-pixel position kept about 80% of B's high-frequency energy (the eye dumps of 2026-11-07); the
// cubic keeps it. A position on a texel centre returns that texel exactly (weights 0, 1, 0, 0). Edges clamped.
// The pass is bound by its fetches, not by memory (2026-11-08: 4 fetches 0.089 ms, 16 loads 0.161 at the pilot's sizes, the same 98 MB either way), so the production form
// (synSharpPaired) takes the 4x4 in 8: the centre 2x2 as point loads (exact, and the clamp's bounds), each edge pair's two centre taps as one bilinear fetch through LinClamp
// at the ratio of their weights (an edge tap is at most a sixteenth of the sum and the sampler's weights are 8-bit, so a result can differ from the 16-load sum by one step of
// an 8-bit channel: 0.05% of the S pixels of the 22 replays), and the four corners, each at most a sixteenth squared (under 1.6% in all), dropped and the rest renormalised: on
// the 22 replays no class mean or p95 moved, the hole error at most +0.1%, S/B sharpness +0.003 to +0.006 (a shade more peaked). 5 is the 16-load form (0.161 ms), 6 the same
// 12 fetches with the corners (0.142), both for the bench's comparison; 1 runs in 0.126.
float4 synCatmullW(float t) {
    return float4(t * (-0.5 + t * (1.0 - 0.5 * t)), 1.0 + t * t * (-2.5 + 1.5 * t), t * (0.5 + t * (2.0 - 1.5 * t)), t * t * (-0.5 + 0.5 * t));
}
float synLanczos2(float x) {
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= 2.0) return 0.0;
    float px = 3.14159265 * x;
    return 2.0 * sin(px) * sin(px * 0.5) / (px * px);
}
float4 synLanczosW(float t) {
    if (t == 0.0) return float4(0.0, 1.0, 0.0, 0.0);
    float4 w = float4(synLanczos2(t + 1.0), synLanczos2(t), synLanczos2(1.0 - t), synLanczos2(2.0 - t));
    return w / dot(w, float4(1.0, 1.0, 1.0, 1.0));
}
// The Catmull-Rom 4x4 in 8 fetches with the corners dropped (above; FG_SHARP 1), or in 12 with them (6, the bench's comparison).
float4 synSharpPaired(Texture2D<float4> t, float2 u, int2 size) {
    float2 f = u - 0.5;
    int2 i0 = int2(floor(f));
    float2 w = f - float2(i0);
    float4 wx = synCatmullW(w.x), wy = synCatmullW(w.y);
    int2 hi = size - 1;
    int2 a = clamp(i0, int2(0, 0), hi), b = clamp(i0 + 1, int2(0, 0), hi);
    float4 c00 = t.Load(int3(a.x, a.y, 0)), c10 = t.Load(int3(b.x, a.y, 0)), c01 = t.Load(int3(a.x, b.y, 0)), c11 = t.Load(int3(b.x, b.y, 0));
    float4 lo = min(min(c00, c10), min(c01, c11)), up = max(max(c00, c10), max(c01, c11));
    float4 acc = wy[1] * (wx[1] * c00 + wx[2] * c10) + wy[2] * (wx[1] * c01 + wx[2] * c11);
    float wxc = wx[1] + wx[2], wyc = wy[1] + wy[2];   // at least 1: the outer weights are never positive
    float2 inv = 1.0 / float2(size);
    float2 pc = float2(i0) + 0.5 + float2(wx[2] / wxc, wy[2] / wyc);   // the paired position between texels 1 and 2, texel centres at +0.5
    acc += wx[0] * wyc * t.SampleLevel(LinClamp, float2(float(i0.x) - 0.5, pc.y) * inv, 0) + wx[3] * wyc * t.SampleLevel(LinClamp, float2(float(i0.x) + 2.5, pc.y) * inv, 0) +
           wy[0] * wxc * t.SampleLevel(LinClamp, float2(pc.x, float(i0.y) - 0.5) * inv, 0) + wy[3] * wxc * t.SampleLevel(LinClamp, float2(pc.x, float(i0.y) + 2.5) * inv, 0);
#if FG_SHARP == 6
    int2 m = clamp(i0 - 1, int2(0, 0), hi), p = clamp(i0 + 2, int2(0, 0), hi);
    acc += wx[0] * wy[0] * t.Load(int3(m.x, m.y, 0)) + wx[3] * wy[0] * t.Load(int3(p.x, m.y, 0)) + wx[0] * wy[3] * t.Load(int3(m.x, p.y, 0)) + wx[3] * wy[3] * t.Load(int3(p.x, p.y, 0));
#else
    acc /= 1.0 - (wx[0] + wx[3]) * (wy[0] + wy[3]);
#endif
    return clamp(acc, lo, up);
}

float4 synSharp(Texture2D<float4> t, float2 u, int2 size) {
#if FG_SHARP == 0
    return synBilinear(t, u, size);
#elif FG_SHARP == 1 || FG_SHARP == 6
    return synSharpPaired(t, u, size);
#else
    float2 f = u - 0.5;
    int2 i0 = int2(floor(f));
    float2 w = f - float2(i0);
#if FG_SHARP == 2
    float4 wx = synLanczosW(w.x), wy = synLanczosW(w.y);
#else
    float4 wx = synCatmullW(w.x), wy = synCatmullW(w.y);
#endif
    int2 hi = size - 1;
    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    float4 lo = float4(1e30, 1e30, 1e30, 1e30), up = -lo;
    [unroll] for (int y = 0; y < 4; ++y) {
        int ry = clamp(i0.y - 1 + y, 0, hi.y);
        float4 row = float4(0.0, 0.0, 0.0, 0.0);
        [unroll] for (int x = 0; x < 4; ++x) {
            float4 c = t.Load(int3(clamp(i0.x - 1 + x, 0, hi.x), ry, 0));
            row += wx[x] * c;
            if (FG_SHARP == 4 || ((x == 1 || x == 2) && (y == 1 || y == 2))) { lo = min(lo, c); up = max(up, c); }
        }
        acc += wy[y] * row;
    }
#if FG_SHARP == 3
    return acc;
#else
    return clamp(acc, lo, up);
#endif
#endif
}

)HLSL" R"HLSL(
uint synBin(float e) {
    if (!(e >= 1.0 / 1024.0)) return 0u;
    float l = 4.0 * log2(e * 1024.0);
    if (!(l < 46.0)) return kBins - 1;
    return uint(clamp(1 + int(floor(l)), 1, int(kBins) - 1));
}

// The class of a depth: 0 far, 1 cockpit (the head's), 2 world; metres out (0 for far).
uint synClass(float zr, out float z) {
    bool far;
    z = temporalDepthMetres(zr, dk.x, dk.y, far);
    return far ? 0u : (temporalWorldPath(true, far, z, dk.z, false) ? 2u : 1u);
}

// The 3x3 depths around a pixel (z9, row by row, the pixel's own at 4) and their nearest (the larger reversed-Z value): the class and depth an edge pixel takes are the near
// object's, and the guard compares the neighbours with the centre.
float synDepth9(int2 q, out float z9[9]) {
    float z = 0.0;
    [unroll] for (int i = 0; i < 9; ++i) {
        int2 n = clamp(q + int2(i % 3 - 1, i / 3 - 1), int2(0, 0), int2(sz.x, sz.y) - 1);
        z9[i] = Depth.Load(int3(n.x, n.y, 0));
        z = max(z, z9[i]);
    }
    return z;
}

// The pixel whose depth synDepth9 took (the pixel's own when it ties): its DLSS vector is the one the pixel moves by, so an edge pixel the near object's depth reached
// moves with the near object and does not carry its colour (the outline the game draws just outside a hull) away at the background's motion (2026-11-07).
int2 synNearest9(int2 q, float z9[9], float zr) {
#if FG_DILATE_MV
    if (z9[4] == zr) return q;
    int k = 4;
    [unroll] for (int i = 8; i >= 0; --i) if (z9[i] == zr) k = i;
    return clamp(q + int2(k % 3 - 1, k / 3 - 1), int2(0, 0), int2(sz.x, sz.y) - 1);
#else
    return q;
#endif
}

bool synFar(float zr) {
    bool far;
    temporalDepthMetres(zr, dk.x, dk.y, far);
    return far;
}

// FG_HALO (2026-11-08, eye dump 120812713001): the outline the game draws around the own hull against space writes no depth, and B's output carries it (and its glow) to 2
// render pixels from the hull, past the 3x3. A pixel whose 3x3 is all the far plane but whose 5x5 outer ring holds a nearer surface takes the nearest of that ring's depths
// and that pixel's vector, so the outline moves with the hull and not with the sky. Elsewhere zr and vq stand.
float synHalo(int2 q, float zr, inout int2 vq) {
#if FG_HALO
    if (!synFar(zr)) return zr;
    float zb = zr;
    int2 best = vq;
    [unroll] for (int i = 0; i < 16; ++i) {
        int2 d = i < 5 ? int2(i - 2, -2) : (i < 10 ? int2(i - 7, 2) : (i < 13 ? int2(-2, i - 11) : int2(2, i - 14)));
        int2 n = clamp(q + d, int2(0, 0), int2(sz.x, sz.y) - 1);
        float zn = Depth.Load(int3(n.x, n.y, 0));
        if (zn > zb) { zb = zn; best = n; }
    }
    if (!synFar(zb)) { vq = best; return zb; }
#endif
    return zr;
}

// A pixel's dilated depth (the 3x3's nearest, else the halo's) with its 3x3 depths and the pixel whose vector it moves by.
float synDilated(int2 q, out float z9[9], out int2 vq) {
    float zr = synDepth9(q, z9);
    vq = synNearest9(q, z9, zr);
    return synHalo(q, zr, vq);
}

// A written depth's metres, 1e9 for the far plane (the guard's comparisons).
float synMetresOrFar(float zr) {
    bool far;
    float z = temporalDepthMetres(zr, dk.x, dk.y, far);
    return far ? 1.0e9 : z;
}

// B's DLSS vector at q, for the ex2dh rule. Where the temporal pass rejected the history (a background just uncovered beside a nearer edge, a masked engine record, a fresh
// screen edit) it writes the size * 2 sentinel instead of the motion (temporal_shader_source.h, `written`): not a vector, and taken as one it throws the pixel off the image
// and leaves a hole band beside every moving edge, filled as a lattice (2026-11-07, the eye dump 222433569001). Such a pixel borrows the vector of the farthest pixel
// without the sentinel on a sparse ring (2 to 32 px, 8 directions): the uncovered background moves as the background beside it. None found: the head's motion alone.
bool synRejected(float2 mv) { return any(abs(mv) >= float2(sz.x, sz.y)); }
float2 synVector(int2 q, float2 headPrevMinusB) {
    float2 mv = Mv.Load(int3(q.x, q.y, 0));
#if FG_SENTINEL
    if (!synRejected(mv)) return mv;
    float bestZ = 2.0, sameZ = 2.0;
    float2 best = headPrevMinusB, same = headPrevMinusB;
#if FG_SENTINEL_SURFACE
    float mq = synMetresOrFar(Depth.Load(int3(q.x, q.y, 0)));
    float mLo = mq * kFillBackMargin, mHi = mq >= 1.0e9 ? 2.0e9 : mq * kFillBackFar;
#endif
    [loop] for (int r = 2; r <= 32; r *= 2) {
        [unroll] for (int j = 0; j < 8; ++j) {
            int2 d = int2(j < 3 ? -1 : (j > 4 ? 1 : 0), (j == 0 || j == 3 || j == 5) ? -1 : ((j == 2 || j == 4 || j == 7) ? 1 : 0));
            int2 n = clamp(q + d * r, int2(0, 0), int2(sz.x, sz.y) - 1);
            float2 v = Mv.Load(int3(n.x, n.y, 0));
            float zn = Depth.Load(int3(n.x, n.y, 0));
            bool ok = !synRejected(v);
            if (ok && zn < bestZ) { bestZ = zn; best = v; }
#if FG_SENTINEL_SURFACE
            float mn = synMetresOrFar(zn);
            if (ok && mn >= mLo && mn <= mHi && zn < sameZ) { sameZ = zn; same = v; }
#endif
        }
    }
    return sameZ < 2.0 ? same : best;
#else
    return mv;
#endif
}

// Where B's pixel id lands in S, and its key, from its dilated depth zr (synDepth9's nearest) and `own` (4 when the pixel's own depth is zr, else 0). The class and depth are
// the dilated depth's. false: behind the eye. The motion is src\common\framegen_synth_math.h frameGenSynthTarget's: cockpit the head's rigid step with the depth; far and world
// the ex2dh rule on B's vector, rotation only.
bool fgSynTargetZ(uint2 id, float zr, uint own, int2 vq, out float2 pp, out uint key) {
    pp = float2(0.0, 0.0);
    key = 0u;
    float z;
    uint cls = synClass(zr, z);
    float2 pB = float2(id);
    float f = FG_USE_F ? dk.w : 1.0;
    float3 dB = temporalPixelDir(pB, tanB, int2(sz.x, sz.y));
    float zKey = z;
    if (cls == 1u) {
        float3 t = float3(rs[0].w, rs[1].w, rs[2].w);
        float3 dp = temporalCarry(dB, z, false, rs[0].xyz, rs[1].xyz, rs[2].xyz, t);
        if (!temporalProjectPixel(dp, tanB, int2(sz.x, sz.y), pp)) return false;
        zKey = -dp.z;
    } else {
#if FG_REMOVE_HEAD
        float3 dH = float3(dot(hp[0].xyz, dB), dot(hp[1].xyz, dB), dot(hp[2].xyz, dB));
        float2 pHeadPrev;
        if (!temporalProjectPixel(dH, tanB, int2(sz.x, sz.y), pHeadPrev)) return false;
#else
        float2 pHeadPrev = pB;
#endif
        float2 mv = synVector(vq, pHeadPrev - pB);
        float2 m = (pB + mv) - pHeadPrev;
        float3 dS = temporalPixelDir(pB - f * m, tanB, int2(sz.x, sz.y));
        float3 dR = float3(dot(rs[0].xyz, dS), dot(rs[1].xyz, dS), dot(rs[2].xyz, dS));
        if (!temporalProjectPixel(dR, tanB, int2(sz.x, sz.y), pp)) return false;
    }
    key = 4u;
    if (cls != 0u) key = (max(asuint(1.0 / max(zKey, 1e-6)), 8u) & ~7u) | own | cls;
    return true;
}

// fgSynTargetZ with the pixel's 3x3 read here (the splat).
bool fgSynTarget(uint2 id, out float2 pp, out uint key) {
    float z9[9];
    int2 vq;
    float zr = synDilated(int2(id), z9, vq);
    return fgSynTargetZ(id, zr, (z9[4] == zr) ? 4u : 0u, vq, pp, key);
}

// The 3x3 depths' metres (synMetresOrFar of each), once per source pixel: the guard's comparisons.
void synMetres9(float z9[9], out float m9[9]) {
    [unroll] for (int j = 0; j < 9; ++j) m9[j] = synMetresOrFar(z9[j]);
}

// FG_HALO's side of the guard: a far-plane pixel whose 5x5 is all the far plane but whose 7x7 ring holds a nearer surface offers reads up to a pixel toward it, into that
// surface's halo (the outline, read back 4 px into the sky in 120812713001's turn). Each such ring pixel marks the 3x3 neighbour in its direction as nearer, so the guard
// keeps the read on the pixel itself when its offset points that way.
void synHaloMetres(int2 q, float zr, inout float m9[9]) {
#if FG_HALO
    if (!synFar(zr)) return;
    [unroll] for (int i = 0; i < 24; ++i) {
        int2 d = i < 7 ? int2(i - 3, -3) : (i < 14 ? int2(i - 10, 3) : (i < 19 ? int2(-3, i - 16) : int2(3, i - 21)));
        int2 n = clamp(q + d, int2(0, 0), int2(sz.x, sz.y) - 1);
        float m = synMetresOrFar(Depth.Load(int3(n.x, n.y, 0)));
        int j = (sign(d.y) + 1) * 3 + sign(d.x) + 1;
        m9[j] = min(m9[j], m);
    }
#endif
}

// The guard: a source pixel (its 3x3 metres m9) with a nearer neighbour (by more than kGuardMargin) on the side its landing offset `off` points to would read across the
// edge, so it reads itself.
bool synGuarded(float m9[9], float2 off) {
#if FG_GUARD
    float zw = m9[4] * kGuardMargin;
    bool g = false;
    [unroll] for (int j = 0; j < 9; ++j) {
        if (j != 4) {
            float2 d = float2(j % 3 - 1, j / 3 - 1);
            if (m9[j] < zw && dot(off, d) > 0.0) g = true;
        }
    }
    return g;
#else
    return false;
#endif
}

[numthreads(8, 8, 1)]
void fgSynSplat(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(sz.x) || id.y >= uint(sz.y)) return;
    ReadOut[id.xy] = kNoOffer;   // the read offsets cleared for fgSynSrc's InterlockedMin (the S grid is the render grid)
    float2 pp;
    uint key;
    if (!fgSynTarget(id.xy, pp, key)) return;
    int2 base = int2(floor(pp));
    [unroll] for (int dy = 0; dy < 2; ++dy) {
        [unroll] for (int dx = 0; dx < 2; ++dx) {
            int2 c = base + int2(dx, dy);
            if (c.x >= 0 && c.y >= 0 && c.x < sz.x && c.y < sz.y) InterlockedMax(Key[uint2(c)], key);
        }
    }
}

)HLSL" R"HLSL(
groupshared uint gSrc;   // clamped

// The winner's read offset for every S pixel its key won: its own position moved by the landing offset (c minus where it landed), guarded, as an offset from c. Two sources
// that tie for a pixel (the splat's tie: the same key) both offer theirs and the smaller packed value stands (InterlockedMin over fgSynSplat's all-ones), whatever order they
// run in: an unguarded read before a guarded one, then by the y half's bits (until 2026-11-08 the last write stood, and the S pixels and the guard count of a tie varied from
// run to run). A guarded read is stored with the mark, for the resolve to count once per S pixel; a clamped read is counted here, per offer.
[numthreads(8, 8, 1)]
void fgSynSrc(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi == 0) gSrc = 0u;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < uint(sz.x) && id.y < uint(sz.y)) {
        float z9[9];
        int2 vq;
        float zr = synDilated(int2(id.xy), z9, vq);
        float2 pp;
        uint key;
        if (fgSynTargetZ(id.xy, zr, (z9[4] == zr) ? 4u : 0u, vq, pp, key)) {
            float m9[9];
            synMetres9(z9, m9);
            synHaloMetres(int2(id.xy), zr, m9);
            int2 base = int2(floor(pp));
            [unroll] for (int dy = 0; dy < 2; ++dy) {
                [unroll] for (int dx = 0; dx < 2; ++dx) {
                    int2 c = base + int2(dx, dy);
                    if (c.x >= 0 && c.y >= 0 && c.x < sz.x && c.y < sz.y && KeyT.Load(int3(c.x, c.y, 0)) == key) {
                        float2 off = float2(c) - pp;
                        float2 rd = float2(id.xy) + off;
                        bool guarded = synGuarded(m9, off);
                        if (guarded) rd = float2(id.xy);
                        float2 o = rd - float2(c);
                        if (any(abs(o) > kMaxOffset)) InterlockedAdd(gSrc, 1u);
                        o = clamp(o, -kMaxOffset, kMaxOffset);
                        InterlockedMin(ReadOut[uint2(c)], guarded ? synPackGuarded(o) : synPackOffset(o));
                    }
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0 && gSrc != 0u) InterlockedAdd(Stats[kCountBase + 3], gSrc);
}

// The read of a pixel n that has a winner, in full precision (what fgSynSrc computed before the store rounded it to halves): the winner's source pixel is found again as the B
// pixel whose key is n's and whose footprint holds n, within the window the stored offset leaves (the source is the offset's end moved by the landing, under a pixel, and the
// rounding, under half a pixel at the clamp), and its read is recomputed. Of tied sources (the same key, n in both footprints) the one whose read rounds to the stored offset is
// taken; the stored offset itself stands in when none is found (a clamped read).
bool synWinnerRead(int2 n, out float2 rd) {
    rd = float2(0.0, 0.0);
    uint key = KeyT.Load(int3(n.x, n.y, 0));
    float2 o = synUnpackOffset(ReadOut[uint2(n)]);
    if (key == 0u || o.x < kNoRead * 0.5) return false;
    float2 approx = float2(n) + o;
    rd = approx;
    int2 lo = int2(floor(approx - 1.5));
    float bestD = 1.0e9;
    [loop] for (int j = 0; j < 16; ++j) {
        int2 w = lo + int2(j & 3, j >> 2);
        if (w.x < 0 || w.y < 0 || w.x >= sz.x || w.y >= sz.y) continue;
        float z9[9];
        int2 vq;
        float zr = synDilated(w, z9, vq);
        float2 pp;
        uint kw;
        if (!fgSynTargetZ(uint2(w), zr, (z9[4] == zr) ? 4u : 0u, vq, pp, kw) || kw != key) continue;
        int2 dd = n - int2(floor(pp));
        if (dd.x < 0 || dd.x > 1 || dd.y < 0 || dd.y > 1) continue;
        float2 off = float2(n) - pp;
        float2 r = float2(w) + off;
        float m9[9];
        synMetres9(z9, m9);
        synHaloMetres(w, zr, m9);
        if (synGuarded(m9, off)) r = float2(w);
        float2 oc = clamp(r - float2(n), -kMaxOffset, kMaxOffset);
        float d = abs(oc.x - o.x) + abs(oc.y - o.y);
        if (d < bestD) { bestD = d; rd = r; }
    }
    return true;
}

// The metres a key says (the far plane's 1e9): the background neighbour's distance, for the fill's depth test.
float synKeyMetres(uint k) { return (k & ~7u) == 0u ? 1.0e9 : 1.0 / asfloat(k & ~7u); }

// Whether B at the read position p (render pixels, centres at integers) is the background's surface (the neighbour at mBg metres): every texel of the 2x2 the resample's
// clamp takes there from kFillBackMargin of that distance to mFar (kFillBackFar times it; the far plane only for the far plane). A nearer texel is the occluder; a much
// farther one another surface beyond a silhouette, not this one's texture (read for it, the hole region's error over 21 replays was 3 milli-luma worse). Outside B: false.
bool synBackAt(float2 p, float mBg, float mFar) {
    int2 i0 = int2(floor(p));
    if (i0.x < 0 || i0.y < 0 || i0.x + 1 >= sz.x || i0.y + 1 >= sz.y) return false;
    [unroll] for (int j = 0; j < 4; ++j) {
        int2 q = i0 + int2(j & 1, j >> 1);
        float m = synMetresOrFar(Depth.Load(int3(q.x, q.y, 0)));
        if (m < mBg * kFillBackMargin || m > mFar) return false;
    }
#if FG_HALO
    // The far plane within 2 px of a nearer surface is FG_HALO's: the outline drawn there without depth, not the sky.
    if (mBg >= 1.0e9) {
        float zMax = 0.0;
        [loop] for (int y = -2; y <= 3; ++y) {
            [unroll] for (int x = -2; x <= 3; ++x) {
                int2 h = clamp(i0 + int2(x, y), int2(0, 0), int2(sz.x, sz.y) - 1);
                zMax = max(zMax, Depth.Load(int3(h.x, h.y, 0)));
            }
        }
        if (!synFar(zMax)) return false;
    }
#endif
    return true;
}

// The disocclusion fill for a hole at c (FG_FILL_REPROJ, 2026-11-07, eye dump 230157175005: terrain uncovered behind the own hull's strake in a ship turn showed as a sawtooth
// of torn strips, each row pulled from a different neighbour). Among the neighbours within kFillRadius that have a winner, the one with the farthest key (the smallest; the
// nearest of equals) is the background; its read offset is the background's motion there (the S pixel to the B position the synth gives that surface). The hole is carried
// back by that motion: where B there is the background's surface (synBackAt), S reads it (kind 2, reprojected). Where the occluder covered that point in B too, the read steps
// along the background's motion relative to the occluder (the occluder pixel's own landing taken out), away from it, up to kFillStretch px, to the first texel of the
// background's surface, and reads kFillShift past it (kind 3, a stretch along the motion). Neither (no relative motion, nothing within the steps): the background pull, the
// neighbour's read kFillShift further into its own surface, away from the hole (kind 1; the whole fill when FG_FILL_REPROJ is 0). 0: nothing to pull from.
uint fgSynFill(int2 c, out float2 rd) {
    rd = float2(0.0, 0.0);
    uint bestKey = FG_FILL_FAR ? 0xFFFFFFFFu : 0u;
    float bestD = 1.0e9;
    int2 bestN = int2(0, 0);
    bool found = false;
    [loop] for (int dy = -kFillRadius; dy <= kFillRadius; ++dy) {
        [unroll] for (int dx = -kFillRadius; dx <= kFillRadius; ++dx) {   // a row's 17 loads in flight together
            if (dx == 0 && dy == 0) continue;
            int2 n = c + int2(dx, dy);
            if (n.x < 0 || n.y < 0 || n.x >= sz.x || n.y >= sz.y) continue;
            uint kn = KeyT.Load(int3(n.x, n.y, 0));   // a key that is not 0 has its read offset written (fgSynSrc, the header)
            if (kn == 0u) continue;
            float d = float(dx * dx + dy * dy);
            bool better = FG_FILL_FAR ? (kn < bestKey) : (kn > bestKey);
            if (!found || better || (kn == bestKey && d < bestD)) {
                found = true;
                bestKey = kn;
                bestD = d;
                bestN = n;
            }
        }
    }
    if (!found) return 0u;
#if FG_FILL_REPROJ
    // The farthest key within kFillRadius may be the occluder's own side beside a hole wider than the search (the strake's in 230157175005, read for the hole it pasted the
    // strake beside itself): a sparse ring further out (12 and 16 px, 8 directions) offers a farther key with a winner.
    {
        [unroll] for (int rr = 12; rr <= 16; rr += 4) {
            [unroll] for (int j = 0; j < 8; ++j) {
                int2 d = int2(j < 3 ? -1 : (j > 4 ? 1 : 0), (j == 0 || j == 3 || j == 5) ? -1 : ((j == 2 || j == 4 || j == 7) ? 1 : 0));
                int2 n = c + d * rr;
                if (n.x < 0 || n.y < 0 || n.x >= sz.x || n.y >= sz.y) continue;
                uint kn = KeyT.Load(int3(n.x, n.y, 0));
                if (kn != 0u && (FG_FILL_FAR ? kn < bestKey : kn > bestKey)) { bestKey = kn; bestN = n; }
            }
        }
    }
#endif
    float2 rn;
    if (!synWinnerRead(bestN, rn)) return 0u;
#if FG_FILL_REPROJ
    float2 o = rn - float2(bestN);   // the background's read offset: S pixel to B position
    float mBg = synKeyMetres(bestKey);
    float mFar = (bestKey & 3u) == 0u ? 2.0e9 : mBg * kFillBackFar;   // the far plane only for the far plane
    float2 r = float2(c) + o;
    if (synBackAt(r, mBg, mFar)) { rd = r; return 2u; }
#if FG_FILL_REPROJ != 2   // the bench's comparison: 2 reprojects without the stretch
    // The occluder at r: where it lands in S (its own motion), so the step follows the background's motion relative to it.
    int2 q = clamp(int2(floor(r + 0.5)), int2(0, 0), int2(sz.x, sz.y) - 1);
    float z9[9];
    int2 vq;
    float zr = synDilated(q, z9, vq);
    float2 pq;
    uint kq;
    float2 occ = float2(0.0, 0.0);
    if (fgSynTargetZ(uint2(q), zr, (z9[4] == zr) ? 4u : 0u, vq, pq, kq)) occ = pq - float2(q);
    float2 rel = -o - occ;   // the background's step B to S, less the occluder's
    if (dot(rel, rel) > 0.0625) {
        float2 dir = normalize(rel);
        [loop] for (int i = 1; i <= kFillStretch; ++i) {
            float2 p = r + dir * float(i);
            if (synBackAt(p, mBg, mFar)) { rd = p + dir * kFillShift; return 3u; }
        }
    }
#endif
#endif
    rd = rn + kFillShift * normalize(float2(bestN - c));
    return 1u;
}

uint fgSynHoleCapacity() { return uint(sz.x) * uint(sz.y) / 8u; }   // src\common\framegen_synth_math.h frameGenSynthHoleCapacity

groupshared uint gCount[4];   // left, guard; the holes of this group; the orphans
groupshared uint gHoleBase;   // where this group's holes start in Holes

// Every S pixel no key reached is a hole: kNoRead for now, counted as left, and its index listed in Holes for fgSynHoles (the group reserves one run of the list; a hole past
// the list's capacity stays left). So is a pixel with a key and no read offer (FG_ORPHAN: fgSynSrc's recompute of the landing disagreed with the splat's, so no source offered
// it, and its all-ones would unpack as a guarded read 2047 px off), counted as an orphan too. A pixel whose stored offset carries the guard's mark is counted as guarded.
[numthreads(8, 8, 1)]
void fgSynResolve(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi < 4) gCount[gi] = 0u;
    GroupMemoryBarrierWithGroupSync();
    bool hole = false;
    uint rank = 0u;
    if (id.x < uint(sz.x) && id.y < uint(sz.y)) {
        bool keyed = KeyT.Load(int3(id.x, id.y, 0)) != 0u;
        uint rd = ReadOut[id.xy];
        bool orphan = FG_ORPHAN && keyed && rd == kNoOffer;
        if (!keyed || orphan) {
            hole = true;
            ReadOut[id.xy] = synPackOffset(float2(kNoRead, 0.0));
            InterlockedAdd(gCount[0], 1u);
            InterlockedAdd(gCount[2], 1u, rank);
            if (orphan) InterlockedAdd(gCount[3], 1u);
        } else if (synIsGuarded(rd)) {
            InterlockedAdd(gCount[1], 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0 && gCount[2] != 0u) InterlockedAdd(Stats[kHoleCount], gCount[2], gHoleBase);
    if (gi == 1 && gCount[0] != 0u) InterlockedAdd(Stats[kCountBase + 1], gCount[0]);
    if (gi == 2 && gCount[1] != 0u) InterlockedAdd(Stats[kCountBase + 2], gCount[1]);
    if (gi == 3 && gCount[3] != 0u) InterlockedAdd(Stats[kOrphan], gCount[3]);
    GroupMemoryBarrierWithGroupSync();
    if (hole) {
        uint slot = gHoleBase + rank;
        if (slot < fgSynHoleCapacity()) Holes[slot] = id.y * uint(sz.x) + id.x;
    }
}

groupshared uint gFill[4];   // filled, clamped, reprojected, stretched

// The fill for the listed holes, one thread each in full groups: run by every lane of an 8x8 group that held one hole, fgSynFill's 289-neighbour search cost more than the rest
// of the resolve. A filled hole takes its read offset (clamped and counted as the winners' are), is counted as filled (and as reprojected or stretched, by its kind) and is no
// longer left; the groups past the count return at once.
[numthreads(64, 1, 1)]
void fgSynHoles(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    if (gi < 4) gFill[gi] = 0u;
    GroupMemoryBarrierWithGroupSync();
    uint count = min(Stats[kHoleCount], fgSynHoleCapacity());
    if (id.x < count) {
        uint p = Holes[id.x];
        int2 c = int2(int(p % uint(sz.x)), int(p / uint(sz.x)));
        float2 rd;
        uint kind = fgSynFill(c, rd);
        if (kind != 0u) {
            float2 off = rd - float2(c);
            if (any(abs(off) > kMaxOffset)) InterlockedAdd(gFill[1], 1u);
            ReadOut[uint2(c)] = synPackOffset(clamp(off, -kMaxOffset, kMaxOffset));
            InterlockedAdd(gFill[0], 1u);
            if (kind >= 2u) InterlockedAdd(gFill[kind], 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0 && gFill[0] != 0u) {
        InterlockedAdd(Stats[kCountBase], gFill[0]);
        InterlockedAdd(Stats[kCountBase + 1], 0u - gFill[0]);   // the resolve counted every hole as left; the sum stays a count
    }
    if (gi == 1 && gFill[1] != 0u) InterlockedAdd(Stats[kCountBase + 3], gFill[1]);
    if (gi == 2 && gFill[2] != 0u) InterlockedAdd(Stats[kFillReproj], gFill[2]);
    if (gi == 3 && gFill[3] != 0u) InterlockedAdd(Stats[kFillReproj + 1], gFill[3]);
}

[numthreads(8, 8, 1)]
void fgSynResample(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(sz.z) || id.y >= uint(sz.w)) return;
    float2 scale = float2(sz.z, sz.w) / float2(sz.x, sz.y);
    float2 ci = (float2(id.xy) + 0.5) / scale - 0.5;       // this output pixel's centre in render pixels
    int2 c = clamp(int2(floor(ci + 0.5)), int2(0, 0), int2(sz.x, sz.y) - 1);
    float2 off = synUnpackOffset(ReadT.Load(int3(c.x, c.y, 0)));
    if (off.x < kNoRead * 0.5) off = float2(0.0, 0.0);
    // The read position is the pixel's own plus its offset, in output pixels: (ci + off + 0.5) * scale, taken from the pixel's own centre so a zero offset lands on it exactly.
    SOut[id.xy] = synSharp(Out, float2(id.xy) + 0.5 + off * scale, int2(sz.z, sz.w));
}

)HLSL" R"HLSL(
// The scorer's capture, on the S texture or the output: four taps a cell at (1, 3) of its 4 x 4 render pixels, converted to output pixels.
float synCellLuma(Texture2D<float4> t, uint2 cell) {
    float2 scale = float2(sz.z, sz.w) / float2(sz.x, sz.y);
    float2 r0 = float2(cell) * 4.0;
    float lum = 0.0;
    [unroll] for (int ty = 0; ty < 2; ++ty) {
        [unroll] for (int tx = 0; tx < 2; ++tx) {
            float2 r = r0 + float2(1.0 + 2.0 * tx, 1.0 + 2.0 * ty);
            lum += synLuma(synBilinear(t, r * scale, int2(sz.z, sz.w)).rgb);
        }
    }
    return lum * 0.25;
}

[numthreads(8, 8, 1)]
void fgSynLuma(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(qz.x) || id.y >= uint(qz.y)) return;
    float z = 0.0;
    [unroll] for (int y = 0; y < 4; ++y) {
        [unroll] for (int x = 0; x < 4; ++x) {
            int2 q = clamp(int2(id.xy) * 4 + int2(x, y), int2(0, 0), int2(sz.x, sz.y) - 1);
            z = max(z, Depth.Load(int3(q.x, q.y, 0)));
        }
    }
    float metres;
    uint cls = synClass(z, metres);
    SLumaOut[id.xy] = synCellLuma(SIn, id.xy);
    BInfoOut[id.xy] = float2(synCellLuma(Out, id.xy), float(cls));
}

// The f = 1 S as the compositor would show it: C's cell direction (C's tangents) turned by the residual head rotation into the eye S was built for, projected with the tangents S
// was built with, and the kept quarter luma read there bilinearly. false: the direction falls outside S (or behind the eye).
bool synTurnedLuma(uint2 cell, out float lum) {
    lum = 0.0;
    float3 d = temporalPixelDir(float2(cell) * 4.0 + 1.5, tanB, int2(sz.x, sz.y));
#if FG_TURN
#if FG_TURN_SIGN
    float3 dS = float3(dot(rr[0].xyz, d), dot(rr[1].xyz, d), dot(rr[2].xyz, d));
#else
    float3 dS = rr[0].xyz * d.x + rr[1].xyz * d.y + rr[2].xyz * d.z;
#endif
#else
    float3 dS = d;
#endif
    float2 pS;
    if (!temporalProjectPixel(dS, tanK, int2(sz.x, sz.y), pS)) return false;
    if (pS.x < -0.5 || pS.y < -0.5 || pS.x > float(sz.x) - 0.5 || pS.y > float(sz.y) - 0.5) return false;
    float2 f = (pS - 1.5) * 0.25;                       // in cells, centres at integers
    int2 i0 = int2(floor(f));
    float2 w = f - float2(i0);
    int2 hi = int2(qz.x, qz.y) - 1;
    int2 a = clamp(i0, int2(0, 0), hi);
    int2 b = clamp(i0 + 1, int2(0, 0), hi);
    float l00 = SLumaIn.Load(int3(a.x, a.y, 0));
    float l10 = SLumaIn.Load(int3(b.x, a.y, 0));
    float l01 = SLumaIn.Load(int3(a.x, b.y, 0));
    float l11 = SLumaIn.Load(int3(b.x, b.y, 0));
    lum = lerp(lerp(l00, l10, w.x), lerp(l01, l11, w.x), w.y);
    return true;
}

groupshared uint gScore[299];   // kBlockUints
groupshared uint gTurn[299];    // the same, S turned

[numthreads(8, 8, 1)]
void fgSynScore(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    for (uint i = gi; i < kBlockUints; i += 64) { gScore[i] = 0u; gTurn[i] = 0u; }
    GroupMemoryBarrierWithGroupSync();
    bool active = id.x < uint(qz.x) && id.y < uint(qz.y);
    uint cls = 0u, bin = 0u, binId = 0u, sumE = 0u, sumId = 0u;
    bool turnedIn = false;
    uint binT = 0u, sumT = 0u;
    if (active) {
        float lumC = synCellLuma(Out, id.xy);
        float lumS = SLumaIn.Load(int3(id.x, id.y, 0));
        float2 bi = BInfoIn.Load(int3(id.x, id.y, 0));
        cls = min(uint(bi.y + 0.5), 2u);
        float e = abs(lumC - lumS);
        float eId = abs(lumC - bi.x);
        bin = synBin(e);
        binId = synBin(eId);
        sumE = uint(e * 1024.0 + 0.5);
        sumId = uint(eId * 1024.0 + 0.5);
        float lumT;
        turnedIn = synTurnedLuma(id.xy, lumT);
        if (turnedIn) {
            float eT = abs(lumC - lumT);
            binT = synBin(eT);
            sumT = uint(eT * 1024.0 + 0.5);
        }
    }
    // The shared-memory adds sit after every read and before the barrier, with no early exit in the group.
    if (active) {
        uint b = cls * kPerClass;
        InterlockedAdd(gScore[kBlockUints - 1], 1u);
        InterlockedAdd(gScore[b + bin], 1u);
        InterlockedAdd(gScore[b + kBins + binId], 1u);
        InterlockedAdd(gScore[b + 2 * kBins], 1u);
        InterlockedAdd(gScore[b + 2 * kBins + 1], sumE);
        InterlockedAdd(gScore[b + 2 * kBins + 2], sumId);
        if (turnedIn) {
            InterlockedAdd(gTurn[kBlockUints - 1], 1u);
            InterlockedAdd(gTurn[b + binT], 1u);
            InterlockedAdd(gTurn[b + kBins + binId], 1u);
            InterlockedAdd(gTurn[b + 2 * kBins], 1u);
            InterlockedAdd(gTurn[b + 2 * kBins + 1], sumT);
            InterlockedAdd(gTurn[b + 2 * kBins + 2], sumId);
        } else {
            InterlockedAdd(gTurn[kHoles], 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint j = gi; j < kBlockUints; j += 64) {
        uint v = gScore[j];
        if (v != 0u) InterlockedAdd(Stats[j], v);
        uint vt = gTurn[j];
        if (vt != 0u) InterlockedAdd(Stats[kTurnBase + j], vt);
    }
}
)HLSL" R"HLSL(
// Phase 2.3b, S finished like B (fgSynFinish): the UI layer B's door composited over B, composited over S the same way (ui_layer's kUiLayerCompositeHlsl, line for line: the
// layer and the multiply averaged over the output pixel's exact footprint, at most 4 x 4 taps, out = L.rgb + F.rgb * L.a * M.rgb, the frame's alpha kept), except that each S
// pixel reads the layer where B's eye saw the same cockpit-fixed point: S's pixel centre carried to B's output pixels by the head's step alone (a homography, the layer taken as a
// plane at a fixed panel distance; exact for the rotation). Its own block in P (framegen_synth_math.h frameGenSynthFinishFillCb): sz.xy the output (the region's) size, sz.zw the
// region's origin in FinFrame; qz.x 1 when a multiply drew into M this frame; dk.xy the layer's size in texels; tanK the layer's rectangle (u0, v0, u1, v1) for the region; rs[0..2]
// the homography's rows, S output pixel (centre at integers) to B's. A homography that is the identity reads exactly the texels B's composite read.
Texture2D<float4>   FinFrame : register(t3);
Texture2D<float4>   FinLayer : register(t5);
Texture2D<float4>   FinMult  : register(t10);
RWTexture2D<float4> FinOut   : register(u2);
[numthreads(8, 8, 1)]
void fgSynFinish(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(sz.x) || id.y >= uint(sz.y)) return;
    float4 f = FinFrame.Load(int3(sz.zw + int2(id.xy), 0));
    float3 p = float3(float2(id.xy), 1.0);
    float3 hq = float3(dot(rs[0].xyz, p), dot(rs[1].xyz, p), dot(rs[2].xyz, p));
    float4 l = float4(0, 0, 0, 1);
    float3 m = float3(1, 1, 1);
    if (hq.z > 1e-6) {
        float2 q = hq.xy / hq.z;
        float2 layerSize = dk.xy;
        float2 span = (tanK.zw - tanK.xy) * layerSize / float2(sz.xy);
        float2 x0 = tanK.xy * layerSize + q * span;
        float2 x1 = x0 + span;
        int2 k0 = max(int2(floor(x0)), int2(0, 0));
        int2 k1 = min(int2(ceil(x1)) - 1, int2(layerSize) - 1);
        float4 acc = float4(0, 0, 0, 0);
        float3 accM = float3(0, 0, 0);
        float wsum = 0;
        [loop] for (int j = k0.y; j <= min(k1.y, k0.y + 3); ++j) {
            float wy = min(x1.y, (float)j + 1.0) - max(x0.y, (float)j);
            if (wy <= 0) continue;
            [loop] for (int i = k0.x; i <= min(k1.x, k0.x + 3); ++i) {
                float wx = min(x1.x, (float)i + 1.0) - max(x0.x, (float)i);
                if (wx <= 0) continue;
                acc += FinLayer.Load(int3(i, j, 0)) * (wx * wy);
                if (qz.x != 0) accM += FinMult.Load(int3(i, j, 0)).rgb * (wx * wy);
                wsum += wx * wy;
            }
        }
        l = wsum > 0 ? acc / wsum : float4(0, 0, 0, 1);
        m = (qz.x != 0 && wsum > 0) ? accM / wsum : float3(1, 1, 1);
    }
    float3 c = l.rgb + f.rgb * l.a * m;
    FinOut[id.xy] = float4(saturate(c), f.a);
}
)HLSL";

inline const std::string& frameGenSynthShaderSource() {
    static const std::string s = frameGenCarryText() + kFrameGenSynthCs;
    return s;
}

}  // namespace edvr

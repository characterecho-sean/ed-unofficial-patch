#pragma once
// The frame-generation synthesizer's dispatch sequence and binding table (fix.frame_generation_shadow, TEMPORARY; docs/design-frame-generation-2026-10-26.md, Phase 2.1), the one
// copy the module (framegen_synth.cpp), the WARP rig (tools\framegen_synth_test) and the bench (tools\framegen_synth_bench) all run, so a pass or a binding changed in one cannot
// drift from the others. The entries, their slots and what each pass reads and writes are documented in framegen_synth_shader.h; the callers own the resources, the constant
// buffer (b0, set before the call) and the shaders (the rig runs mutated builds through the same table).
#include <d3d11.h>

#include <cstdint>

#include "../common/framegen_synth_math.h"   // frameGenSynthHoleCapacity

namespace edvr {

constexpr int kFrameGenSynthEntries = 7;
constexpr int kFrameGenSynthSteps = kFrameGenSynthEntries + 1;   // the entries and the API clear before them, for timing
constexpr int kFrameGenSynthSrvSlots = 10;   // t0..t9
constexpr int kFrameGenSynthUavSlots = 8;    // u0..u7

enum FrameGenSynthEntry : int { kFgSynSplat = 0, kFgSynSrc, kFgSynResolve, kFgSynHoles, kFgSynResample, kFgSynLuma, kFgSynScore, kFgSynClear /* a step, not a shader */ };

// The HLSL entry points, in entry order (tools\temporal_shader_build compiles them to kFrameGenSynth*Bytecode in the same order).
constexpr const char* const kFrameGenSynthEntryNames[kFrameGenSynthEntries] = {"fgSynSplat", "fgSynSrc", "fgSynResolve", "fgSynHoles", "fgSynResample", "fgSynLuma", "fgSynScore"};
constexpr const char* const kFrameGenSynthStepLabels[kFrameGenSynthSteps] = {"splat", "src", "resolve", "holes", "resample", "luma", "score", "clear"};

// One eye's views for a call. The inputs: B's output (the upscaler's, before the interface; C's on a score), B's depth copy, the DLSS vectors. The synth's own: the key grid
// (R32_UINT), the read offsets (R32_UINT, two packed half floats) and the hole list (render size, scratch), S (output size), the kept S luma and B's luma and class (quarter
// grid), the stats words.
struct FrameGenSynthPassViews {
    ID3D11ShaderResourceView* out = nullptr;
    ID3D11ShaderResourceView* depth = nullptr;
    ID3D11ShaderResourceView* mv = nullptr;
    ID3D11ShaderResourceView* keySrv = nullptr;
    ID3D11ShaderResourceView* readSrv = nullptr;
    ID3D11ShaderResourceView* sSrv = nullptr;
    ID3D11ShaderResourceView* sLumaSrv = nullptr;
    ID3D11ShaderResourceView* bInfoSrv = nullptr;
    ID3D11UnorderedAccessView* keyUav = nullptr;
    ID3D11UnorderedAccessView* readUav = nullptr;
    ID3D11UnorderedAccessView* holesUav = nullptr;   // frameGenSynthHoleCapacity(rw, rh) uints
    ID3D11UnorderedAccessView* sUav = nullptr;
    ID3D11UnorderedAccessView* statsUav = nullptr;
    ID3D11UnorderedAccessView* sLumaUav = nullptr;
    ID3D11UnorderedAccessView* bInfoUav = nullptr;
    ID3D11SamplerState* linear = nullptr;   // frameGenSynthCreateSampler's: the resample's paired taps (s0)
    uint32_t rw = 0, rh = 0;   // render size
    uint32_t ow = 0, oh = 0;   // output size
    uint32_t qw = 0, qh = 0;   // the quarter grid
};

// The one sampler the passes use (s0, the resample): linear, clamped at the edges, no mips. The callers own it (made once with their shaders).
inline bool frameGenSynthCreateSampler(ID3D11Device* dev, ID3D11SamplerState** out) {
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    return SUCCEEDED(dev->CreateSamplerState(&sd, out)) && *out;
}

// The sequence of one call: the clear (the key grid and the stats words to 0 by the API; the read offsets are set by the splat, framegen_synth_shader.h); the score of the kept f = 1 S
// against this frame when `score`; then, when `build`, splat, source (the winners' read offsets), resolve (the holes listed), holes, resample and, when `keep`, the luma pass
// that keeps this f = 1 S for the next frame.
// `after(int step)` runs after the clear and after each dispatch (the bench's timestamps). Every dispatch starts with nothing bound from the last (a resource still bound as a
// UAV would silently null the SRV set for it), and nothing stays bound at the end.
template <class After>
inline void frameGenSynthRunPasses(ID3D11DeviceContext* ctx, ID3D11ComputeShader* const cs[kFrameGenSynthEntries], const FrameGenSynthPassViews& v, bool score, bool build, bool keep,
                                   After&& after) {
    ID3D11ShaderResourceView* const noSrv[kFrameGenSynthSrvSlots] = {};
    ID3D11UnorderedAccessView* const noUav[kFrameGenSynthUavSlots] = {};
    const auto dispatch = [&](int entry, ID3D11ShaderResourceView* const srv[kFrameGenSynthSrvSlots], ID3D11UnorderedAccessView* const uav[kFrameGenSynthUavSlots], uint32_t gx,
                              uint32_t gy) {
        ctx->CSSetShader(cs[entry], nullptr, 0);
        ctx->CSSetUnorderedAccessViews(0, kFrameGenSynthUavSlots, noUav, nullptr);
        ctx->CSSetShaderResources(0, kFrameGenSynthSrvSlots, srv);
        ctx->CSSetUnorderedAccessViews(0, kFrameGenSynthUavSlots, uav, nullptr);
        ctx->Dispatch(gx, gy, 1);
        after(entry);
    };
    const uint32_t rgx = (v.rw + 7u) / 8u, rgy = (v.rh + 7u) / 8u;
    const uint32_t ogx = (v.ow + 7u) / 8u, ogy = (v.oh + 7u) / 8u;
    const uint32_t qgx = (v.qw + 7u) / 8u, qgy = (v.qh + 7u) / 8u;
    {   // clear: the key grid (0 = a hole) and the stats words
        const UINT zeros[4] = {};
        ctx->ClearUnorderedAccessViewUint(v.keyUav, zeros);
        ctx->ClearUnorderedAccessViewUint(v.statsUav, zeros);
        after(kFgSynClear);
    }
    if (score) {   // the f = 1 S kept from the frame before against this frame, which is C: its luma, S's and B's, by B's class
        ID3D11ShaderResourceView* srv[kFrameGenSynthSrvSlots] = {v.out, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, v.sLumaSrv, v.bInfoSrv};
        ID3D11UnorderedAccessView* uav[kFrameGenSynthUavSlots] = {nullptr, nullptr, nullptr, nullptr, nullptr, v.statsUav};
        dispatch(kFgSynScore, srv, uav, qgx, qgy);
    }
    if (build) {
        {   // splat: B's pixels carried to S, the nearer surface's key kept (and the read offsets set to all ones for the source pass's InterlockedMin)
            ID3D11ShaderResourceView* srv[kFrameGenSynthSrvSlots] = {nullptr, v.depth, v.mv};
            ID3D11UnorderedAccessView* uav[kFrameGenSynthUavSlots] = {nullptr, v.keyUav, nullptr, v.readUav};
            dispatch(kFgSynSplat, srv, uav, rgx, rgy);
        }
        {   // source: each footprint pixel whose final key is the pixel's own takes the pixel's read offset, guarded (a guarded one marked); the clamp count
            ID3D11ShaderResourceView* srv[kFrameGenSynthSrvSlots] = {nullptr, v.depth, v.mv, nullptr, v.keySrv};
            ID3D11UnorderedAccessView* uav[kFrameGenSynthUavSlots] = {nullptr, nullptr, nullptr, v.readUav, nullptr, v.statsUav};
            dispatch(kFgSynSrc, srv, uav, rgx, rgy);
        }
        {   // resolve: the holes (no key) marked, counted and listed; the guarded pixels counted
            ID3D11ShaderResourceView* srv[kFrameGenSynthSrvSlots] = {nullptr, nullptr, nullptr, nullptr, v.keySrv};
            ID3D11UnorderedAccessView* uav[kFrameGenSynthUavSlots] = {v.holesUav, nullptr, nullptr, v.readUav, nullptr, v.statsUav};
            dispatch(kFgSynResolve, srv, uav, rgx, rgy);
        }
        {   // holes: the background pull for the listed holes, one thread each (the neighbours' offsets read through the UAV); the groups past the count return at once
            ID3D11ShaderResourceView* srv[kFrameGenSynthSrvSlots] = {nullptr, v.depth, v.mv, nullptr, v.keySrv};
            ID3D11UnorderedAccessView* uav[kFrameGenSynthUavSlots] = {v.holesUav, nullptr, nullptr, v.readUav, nullptr, v.statsUav};
            dispatch(kFgSynHoles, srv, uav, (frameGenSynthHoleCapacity(v.rw, v.rh) + 63u) / 64u, 1u);
        }
        {   // resample B's output into S at the output size
            ID3D11ShaderResourceView* srv[kFrameGenSynthSrvSlots] = {v.out, nullptr, nullptr, nullptr, nullptr, nullptr, v.readSrv};
            ID3D11UnorderedAccessView* uav[kFrameGenSynthUavSlots] = {nullptr, nullptr, nullptr, nullptr, v.sUav};
            ID3D11SamplerState* const smp[1] = {v.linear};
            ctx->CSSetSamplers(0, 1, smp);
            dispatch(kFgSynResample, srv, uav, ogx, ogy);
            ID3D11SamplerState* const noSmp[1] = {};
            ctx->CSSetSamplers(0, 1, noSmp);
        }
        if (keep) {   // the f = 1 S's luma, B's luma and B's class, for the next real frame to score
            ID3D11ShaderResourceView* srv[kFrameGenSynthSrvSlots] = {v.out, v.depth, nullptr, nullptr, nullptr, nullptr, nullptr, v.sSrv};
            ID3D11UnorderedAccessView* uav[kFrameGenSynthUavSlots] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, v.sLumaUav, v.bInfoUav};
            dispatch(kFgSynLuma, srv, uav, qgx, qgy);
        }
    }
    ctx->CSSetShaderResources(0, kFrameGenSynthSrvSlots, noSrv);
    ctx->CSSetUnorderedAccessViews(0, kFrameGenSynthUavSlots, noUav, nullptr);
}

inline void frameGenSynthRunPasses(ID3D11DeviceContext* ctx, ID3D11ComputeShader* const cs[kFrameGenSynthEntries], const FrameGenSynthPassViews& v, bool score, bool build, bool keep) {
    frameGenSynthRunPasses(ctx, cs, v, score, build, keep, [](int) {});
}

}  // namespace edvr

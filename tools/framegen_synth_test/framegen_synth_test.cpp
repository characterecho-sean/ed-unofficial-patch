// framegen_synth_test: the frame-generation synthesizer (fix.frame_generation_shadow, TEMPORARY; docs/design-frame-generation-2026-10-26.md, Phase 2.1) on WARP.
//
//   --dry-run    the same run (the rig never writes a file), for the gate's --dry-run convention
//   --self-test  every check, then the mutations (each must make its case fail)
//   --verbose    the numbers of each case
//
// The production bytecode (kFrameGenSynth*Bytecode, from tools\temporal_shader_build) is run on a WARP device over a scene whose upscaled output is a gradient (red the
// render x, green the render y of the texel, in render pixels, so a pixel of S tells exactly where in B it was read) and the S texture it writes is held to the CPU
// reference in src\common\framegen_synth_math.h: for each output pixel c that read B at r, the reference's landing of r must be c, to under 0.1 px.
//
// Cases:
//   1  a cockpit plane at 1 m under a head translation and turn (f = 0.5), at 1:1 and at 2:1 output
//   2  a world plane at 50 m with the ship's screen motion constant and the head turning (the ex2dh rule), and with the head still the plain p - f m
//   3  the far plane under the same
//   4  the on-foot 1 mm split: a plane at 0.5 mm is the cockpit and moves with the head, one at 2 mm is the world and, its vectors zero and the head still, does not
//   5  the guard and the fill: a near object's edge over the sky as the object moves away. No sky pixel takes the near colour (the guard), the strip the object left is
//      filled from the sky (the background pull, never from the object), and the counters say so
//   6  the module itself (framegen_synth.cpp) on the same device: the key off creates nothing; on, it builds the two variants in turn, scores the f = 1 one against the
//      next frame (a static scene scores in bin 0, a scene scrolling at the vectors' rate scores well against a no-warp comparison that does not), counts the frames it
//      could not build, leaves a gap unscored, hands out the f = 0.5 S, and releases everything when the key goes off
//   7  the f = 1 S scored the way it would be seen (the compositor's turn): a far scene seen from a head turning at a changing rate, the S built for the extrapolated turn,
//      scored against the next frame raw and turned by the residual rotation (the true head against the predicted): the turned score is near zero where the raw one is
//      clearly not, and the residual's angle is the sequence's own
//   8  the read offsets' storage (two halves in a uint, rounded toward zero): exact to 1/4 px up to 512 px, and the clamp's bounds
//   10 Phase 2.2: the head step from the runtime's slot pose (frameGenSlotStep from B's and S's heads and the eye): a cockpit plane at 1 m and the far plane land where S's
//      own head sees them, by a reference that does not use the step (to under 0.1 px); the slot's pose ignored, or the step taken the wrong way round, must fail it
//   11 Phase 2.2, S's transfer on two WARP devices: the module writes the f = 0.5 S into the producer texture and publishes it, the runtime's side (framegen_slot_transfer.h)
//      opens and consumes it and holds the picture; the f = 1 S is kept; an unconsumed S makes the next one busy; a consume before a release times out at 0 ms; a run of
//      busy builds remakes the producer texture; no consumer lets it go; the key off releases everything
//   12 Phase 2.3b, S finished like B at B's door (fgSynFinish): held back until the door; with a still head the runtime's S is the UI layer's composite over S (and over B, to the
//      resample's rounding) through the exact identity; under a head turn and step the layer's block lands where a direction reference puts it (within 0.5 px) and the image
//      is the homography's composite; an unfinished S (a named reason, a door that never came, a door that did not sharpen an S built to be) is not handed over and gives the
//      producer texture's key back; a sharpening door's frame is the one finished over; the eye dump's sink sees the finished S
//   14 S's resample (FG_SHARP): a still scene and an integer read return B's texels exactly; a 5 px period read a quarter pixel off keeps 0.95 of its high-frequency energy;
//      a step read off a texel rings nowhere outside its two levels
//   15 the disocclusion fill (FG_FILL_REPROJ): a world plane, farther up the image, uncovered beside a near bar carries on its texture row for row (the hole carried back by the
//      plane's motion and stretched along it past the bar), where the background pull read each row from a different neighbour
//   16 an orphan (FG_ORPHAN): fgSynSrc's offer withheld from one pixel of a still plane (an edit of the rig's own build) leaves a key and no read offer; the resolve counts it and
//      makes it a hole the fill reads right, not the all-ones read 2047 px off; the production build makes none
//   17 the hull's outline against space (FG_HALO, FG_SENTINEL_SURFACE): an outline drawn without depth 2 px outside a still hull over a moving far plane stays on the hull and
//      nowhere in the sky; a patch of the hull with a rejected history moves with the hull, not with the sky past the silhouette
// Mutations (each replaces one of the shader's #define switches, or the host's f, and the case it guards must fail): f ignored, the head turn not taken out of the
// vector, the guard off, the fill from the nearer side, the score's turn not applied, the score's turn applied the wrong way round, an edge pixel on its own vector, the
// sentinel as a vector, S resampled bilinear, the cubic without its clamp (FG_SHARP=3), the hole fill by the background pull alone (FG_FILL_REPROJ), an orphan read as its all-ones (FG_ORPHAN), the halo off (FG_HALO), a rejected vector from any
// surface (FG_SENTINEL_SURFACE).
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <d3dcompiler.h>
#include "framegen_synth_shader.h"
#include "framegen_synth_pass.h"

#include "../../src/common/framegen_synth_math.h"
#include "framegen_synth.h"
#include "temporal_shader_bytecode.h"
#include "../../src/openxr/framegen_slot_transfer.h"   // case 11: the runtime's side of S's transfer

using Microsoft::WRL::ComPtr;
using namespace edvr;

// shader_swap.cpp reports its compile times to the overlay's counters; the rig has no overlay.
namespace edvr { void perfMonitorNoteEvent(uint32_t, double) {} }

namespace {

bool g_verbose = false;
unsigned g_checks = 0, g_failures = 0;

// A scope collects a case's failures: a mutation runs the case quietly and expects it to fail.
struct Scope {
    bool quiet = false;
    unsigned fails = 0;
    void ok(bool cond, const char* label) {
        if (quiet) { if (!cond) ++fails; return; }
        ++g_checks;
        if (!cond) {
            ++g_failures;
            ++fails;
            std::printf("FAIL: %s\n", label);
            std::fflush(stdout);
        }
    }
};

constexpr int RW = 128, RH = 96;
constexpr int QW = RW / 4, QH = RH / 4;
constexpr float kPi = 3.14159265358979f;
const float kTan[4] = {-1.0f, 1.0f, -0.75f, 0.75f};
const float kDepthB = 0.025f;   // Elite's infinite-far reversed-Z: written = B / metres, A = 0

uint16_t floatToHalf(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    const uint32_t sign = (u >> 16) & 0x8000u;
    int e = static_cast<int>((u >> 23) & 0xFF) - 127 + 15;
    uint32_t m = u & 0x7FFFFFu;
    if (e <= 0) return static_cast<uint16_t>(sign);
    if (e >= 31) return static_cast<uint16_t>(sign | 0x7BFFu);
    uint32_t h = (static_cast<uint32_t>(e) << 10) | (m >> 13);
    if (m & 0x1000u) ++h;
    return static_cast<uint16_t>(sign | h);
}

void rotY(float deg, float m[9]) {
    const float r = deg * kPi / 180.0f, c = std::cos(r), s = std::sin(r);
    const float v[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
    std::memcpy(m, v, sizeof(v));
}

// ---- the GPU harness -------------------------------------------------------------------------------------------------------------------------------------

struct Tex {
    ComPtr<ID3D11Texture2D> t;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};

constexpr int kEntries = kFrameGenSynthEntries;   // the entries, their names and the dispatch sequence: framegen_synth_pass.h
const char* const* const kEntryNames = kFrameGenSynthEntryNames;

struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11ComputeShader> cs[kEntries];
    ComPtr<ID3D11SamplerState> linear;
    ComPtr<ID3D11Buffer> cb;

    bool init() {
        D3D_FEATURE_LEVEL level{};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx))) return false;
        const struct { const unsigned char* code; size_t size; } codes[kEntries] = {
            {kFrameGenSynthSplatBytecode, sizeof(kFrameGenSynthSplatBytecode)}, {kFrameGenSynthSrcBytecode, sizeof(kFrameGenSynthSrcBytecode)},
            {kFrameGenSynthResolveBytecode, sizeof(kFrameGenSynthResolveBytecode)}, {kFrameGenSynthHolesBytecode, sizeof(kFrameGenSynthHolesBytecode)},
            {kFrameGenSynthResampleBytecode, sizeof(kFrameGenSynthResampleBytecode)},
            {kFrameGenSynthLumaBytecode, sizeof(kFrameGenSynthLumaBytecode)}, {kFrameGenSynthScoreBytecode, sizeof(kFrameGenSynthScoreBytecode)}};
        for (int i = 0; i < kEntries; ++i)
            if (FAILED(dev->CreateComputeShader(codes[i].code, codes[i].size, nullptr, &cs[i]))) return false;
        if (!frameGenSynthCreateSampler(dev.Get(), &linear)) return false;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(FrameGenSynthCb);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &cb));
    }

    bool makeTex(int w, int h, DXGI_FORMAT fmt, const void* data, UINT pitch, Tex& out, bool wantUav) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (wantUav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
        D3D11_SUBRESOURCE_DATA sd{data, pitch, 0};
        if (FAILED(dev->CreateTexture2D(&td, data ? &sd : nullptr, &out.t))) return false;
        if (FAILED(dev->CreateShaderResourceView(out.t.Get(), nullptr, &out.srv))) return false;
        if (wantUav && FAILED(dev->CreateUnorderedAccessView(out.t.Get(), nullptr, &out.uav))) return false;
        return true;
    }

    template <class T>
    std::vector<T> readTex(ID3D11Texture2D* src, int w, int h) {
        D3D11_TEXTURE2D_DESC td{};
        src->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> st;
        std::vector<T> v(static_cast<size_t>(w) * h);
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &st))) return v;
        ctx->CopyResource(st.Get(), src);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return v;
        for (int y = 0; y < h; ++y) std::memcpy(&v[static_cast<size_t>(y) * w], static_cast<const char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, sizeof(T) * w);
        ctx->Unmap(st.Get(), 0);
        return v;
    }

    void setCb(const FrameGenSynthCb& c) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, &c, sizeof(c));
            ctx->Unmap(cb.Get(), 0);
        }
        ID3D11Buffer* b = cb.Get();
        ctx->CSSetConstantBuffers(0, 1, &b);
    }

    // The module's dispatch sequence (framegen_synth_pass.h) on this device's shaders (the production bytecode, or a mutated build swapped into cs).
    void run(const FrameGenSynthPassViews& v, bool score, bool build, bool keep) {
        ID3D11ComputeShader* raw[kEntries];
        for (int i = 0; i < kEntries; ++i) raw[i] = cs[i].Get();
        frameGenSynthRunPasses(ctx.Get(), raw, v, score, build, keep);
    }
};

// Case 16's forced orphan: fgSynSrc's offer withheld from the S pixel (kOrphanX, kOrphanY), which keeps its key and no read offer, as a landing fgSynSrc recomputed differently
// from fgSynSplat's would leave it. An edit of the text the rig compiles (compileMutated's `orphan`), never of the production text.
constexpr int kOrphanX = 40, kOrphanY = 30;
const char* const kSrcOfferAnchor = "InterlockedMin(ReadOut[uint2(c)], ";

// The production text with one or more of its switches turned off, compiled for every entry; `defineName` is "FG_GUARD", or "FG_GUARD,FG_DILATE_MV", and so on, or "" for
// none. `orphan`: fgSynSrc never offers a read to the pixel (kOrphanX, kOrphanY) (case 16).
bool compileMutated(Gpu& g, const char* defineName, ComPtr<ID3D11ComputeShader> out[kEntries], bool orphan = false) {
    HMODULE lib = LoadLibraryA("d3dcompiler_47.dll");
    if (!lib) { std::printf("  d3dcompiler_47.dll did not load\n"); return false; }
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    const CompileFn compile = reinterpret_cast<CompileFn>(GetProcAddress(lib, "D3DCompile"));
    if (!compile) return false;
    std::string text = frameGenSynthShaderSource();
    if (orphan) {
        const size_t at = text.find(kSrcOfferAnchor);
        if (at == std::string::npos || text.find(kSrcOfferAnchor, at + 1) != std::string::npos) return false;   // exactly one offer
        text.insert(at, "if (any(c != int2(" + std::to_string(kOrphanX) + ", " + std::to_string(kOrphanY) + "))) ");
    }
    std::string names = defineName;   // one switch, or several separated by commas, all turned off
    for (size_t start = 0; !names.empty() && start <= names.size();) {
        const size_t comma = names.find(',', start);
        std::string name = names.substr(start, comma == std::string::npos ? std::string::npos : comma - start), value = "0";
        const size_t eq = name.find('=');   // NAME=V: the switch set to V instead of 0
        if (eq != std::string::npos) { value = name.substr(eq + 1); name.resize(eq); }
        const std::string from = std::string("#define ") + name + " 1";
        const size_t at = text.find(from);
        if (at == std::string::npos) return false;
        text.replace(at, from.size(), std::string("#define ") + name + " " + value);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    for (int i = 0; i < kEntries; ++i) {
        ComPtr<ID3DBlob> code, err;
        if (FAILED(compile(text.data(), text.size(), "framegen_synth_mutated", nullptr, nullptr, kEntryNames[i], "cs_5_0", 0, 0, &code, &err))) {
            if (err) std::printf("  %s: %s\n", kEntryNames[i], static_cast<const char*>(err->GetBufferPointer()));
            return false;
        }
        const HRESULT created = g.dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &out[i]);
        if (FAILED(created)) {
            std::printf("  %s: CreateComputeShader failed (0x%08X; the device's removed reason is 0x%08X)\n", kEntryNames[i], static_cast<unsigned>(created), static_cast<unsigned>(g.dev->GetDeviceRemovedReason()));
            return false;
        }
    }
    return true;
}

// ---- a scene and one run of the pass ---------------------------------------------------------------------------------------------------------------------

struct Scene {
    int w = RW, h = RH, ow = RW, oh = RH;
    std::vector<float> rgba;    // the upscaler's output, output size, RGBA32F
    std::vector<float> depth;   // the written depth, render size
    std::vector<float> mv;      // the DLSS vectors, 2 floats a render pixel, B -> A, pixels
    Scene(int ow_ = RW, int oh_ = RH) : ow(ow_), oh(oh_), rgba(static_cast<size_t>(ow_) * oh_ * 4, 0.0f), depth(static_cast<size_t>(RW) * RH, 0.0f), mv(static_cast<size_t>(RW) * RH * 2, 0.0f) {}
    float scale() const { return static_cast<float>(ow) / static_cast<float>(w); }
    // The gradient: red the render x, green the render y of the output texel's centre.
    void gradient() {
        const float s = scale();
        for (int y = 0; y < oh; ++y)
            for (int x = 0; x < ow; ++x) {
                float* p = &rgba[(static_cast<size_t>(y) * ow + x) * 4];
                p[0] = (static_cast<float>(x) + 0.5f) / s - 0.5f;
                p[1] = (static_cast<float>(y) + 0.5f) / s - 0.5f;
                p[2] = 0.5f;
                p[3] = 1.0f;
            }
    }
    void grey(const std::function<float(int, int)>& f) {
        for (int y = 0; y < oh; ++y)
            for (int x = 0; x < ow; ++x) {
                float* p = &rgba[(static_cast<size_t>(y) * ow + x) * 4];
                p[0] = p[1] = p[2] = f(x, y);
                p[3] = 1.0f;
            }
    }
};

struct Run {
    std::vector<float> s;           // S, output size, RGBA32F
    std::vector<uint32_t> stats;
    bool ok = false;
    float at(int x, int y, int ch, int ow) const { return s[(static_cast<size_t>(y) * ow + x) * 4 + ch]; }
};

bool makeInputs(Gpu& g, const Scene& sc, Tex& out, Tex& depth, Tex& mv) {
    std::vector<uint16_t> half(sc.mv.size());
    for (size_t i = 0; i < sc.mv.size(); ++i) half[i] = floatToHalf(sc.mv[i]);
    return g.makeTex(sc.ow, sc.oh, DXGI_FORMAT_R32G32B32A32_FLOAT, sc.rgba.data(), static_cast<UINT>(sc.ow * 16), out, false) &&
           g.makeTex(sc.w, sc.h, DXGI_FORMAT_R32_FLOAT, sc.depth.data(), static_cast<UINT>(sc.w * 4), depth, false) &&
           g.makeTex(sc.w, sc.h, DXGI_FORMAT_R16G16_FLOAT, half.data(), static_cast<UINT>(sc.w * 4), mv, false);
}

// What a built f = 1 S leaves for the next frame's score: the quarter-grid luma of S, and B's luma and class.
struct Keep {
    Tex sLuma, bInfo;
};

// The module's order of dispatches for one eye-frame, up to and including the resample (and, with `keep`, the luma that is kept for the score). The key grid and the read
// offsets are the module's: R32_UINT both (the offsets two packed half floats).
Run runSynth(Gpu& g, const Scene& sc, const FrameGenSynthCb& cb, Keep* keep = nullptr) {
    Run r;
    Tex out, depth, mv, key, read, s;
    if (!makeInputs(g, sc, out, depth, mv)) return r;
    if (!g.makeTex(sc.w, sc.h, DXGI_FORMAT_R32_UINT, nullptr, 0, key, true) || !g.makeTex(sc.w, sc.h, DXGI_FORMAT_R32_UINT, nullptr, 0, read, true) ||
        !g.makeTex(sc.ow, sc.oh, DXGI_FORMAT_R32G32B32A32_FLOAT, nullptr, 0, s, true))
        return r;
    if (keep && (!g.makeTex((sc.w + 3) / 4, (sc.h + 3) / 4, DXGI_FORMAT_R32_FLOAT, nullptr, 0, keep->sLuma, true) ||
                 !g.makeTex((sc.w + 3) / 4, (sc.h + 3) / 4, DXGI_FORMAT_R32G32_FLOAT, nullptr, 0, keep->bInfo, true)))
        return r;
    ComPtr<ID3D11Buffer> stats, staging;
    ComPtr<ID3D11UnorderedAccessView> statsUav;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = kFrameGenSynthStatsUints * 4; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 4;
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &stats))) return r;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = kFrameGenSynthStatsUints;
    if (FAILED(g.dev->CreateUnorderedAccessView(stats.Get(), &ud, &statsUav))) return r;
    bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &staging))) return r;
    ComPtr<ID3D11Buffer> holes;   // the hole list (the module's ensureScratch)
    ComPtr<ID3D11UnorderedAccessView> holesUav;
    bd = D3D11_BUFFER_DESC{};
    bd.ByteWidth = frameGenSynthHoleCapacity(static_cast<uint32_t>(sc.w), static_cast<uint32_t>(sc.h)) * 4; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 4;
    ud.Buffer.NumElements = frameGenSynthHoleCapacity(static_cast<uint32_t>(sc.w), static_cast<uint32_t>(sc.h));
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &holes)) || FAILED(g.dev->CreateUnorderedAccessView(holes.Get(), &ud, &holesUav))) return r;

    g.setCb(cb);
    FrameGenSynthPassViews v;
    v.out = out.srv.Get(); v.depth = depth.srv.Get(); v.mv = mv.srv.Get();
    v.keySrv = key.srv.Get(); v.keyUav = key.uav.Get();
    v.readSrv = read.srv.Get(); v.readUav = read.uav.Get();
    v.holesUav = holesUav.Get();
    v.sSrv = s.srv.Get(); v.sUav = s.uav.Get();
    if (keep) { v.sLumaUav = keep->sLuma.uav.Get(); v.bInfoUav = keep->bInfo.uav.Get(); }
    v.statsUav = statsUav.Get();
    v.linear = g.linear.Get();
    v.rw = static_cast<uint32_t>(sc.w); v.rh = static_cast<uint32_t>(sc.h); v.ow = static_cast<uint32_t>(sc.ow); v.oh = static_cast<uint32_t>(sc.oh);
    v.qw = static_cast<uint32_t>((sc.w + 3) / 4); v.qh = static_cast<uint32_t>((sc.h + 3) / 4);
    g.run(v, false, true, keep != nullptr);
    g.ctx->CopyResource(staging.Get(), stats.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return r;
    r.stats.assign(static_cast<const uint32_t*>(m.pData), static_cast<const uint32_t*>(m.pData) + kFrameGenSynthStatsUints);
    g.ctx->Unmap(staging.Get(), 0);
    r.s = g.readTex<float>(s.t.Get(), sc.ow * 4, sc.oh);
    r.ok = true;
    return r;
}

// The score of frame C against the S kept from B (the module's order: clear the stats, then the score entry on C's output), decoded.
bool runScore(Gpu& g, const Scene& scC, const FrameGenSynthCb& cbC, const Keep& keep, FrameGenSynthEyeResult& res) {
    Tex out, depth, mv, key;
    if (!makeInputs(g, scC, out, depth, mv)) return false;
    if (!g.makeTex(scC.w, scC.h, DXGI_FORMAT_R32_UINT, nullptr, 0, key, true)) return false;
    ComPtr<ID3D11Buffer> stats, staging;
    ComPtr<ID3D11UnorderedAccessView> statsUav;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = kFrameGenSynthStatsUints * 4; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 4;
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &stats))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = kFrameGenSynthStatsUints;
    if (FAILED(g.dev->CreateUnorderedAccessView(stats.Get(), &ud, &statsUav))) return false;
    bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &staging))) return false;
    g.setCb(cbC);
    FrameGenSynthPassViews v;
    v.out = out.srv.Get(); v.depth = depth.srv.Get(); v.mv = mv.srv.Get();
    v.keyUav = key.uav.Get();
    v.sLumaSrv = keep.sLuma.srv.Get(); v.bInfoSrv = keep.bInfo.srv.Get();
    v.statsUav = statsUav.Get();
    v.rw = static_cast<uint32_t>(scC.w); v.rh = static_cast<uint32_t>(scC.h);
    v.qw = static_cast<uint32_t>((scC.w + 3) / 4); v.qh = static_cast<uint32_t>((scC.h + 3) / 4);
    g.run(v, true, false, false);
    g.ctx->CopyResource(staging.Get(), stats.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    frameGenSynthDecodeStats(static_cast<const uint32_t*>(m.pData), res);
    g.ctx->Unmap(staging.Get(), 0);
    return true;
}

// ---- the references --------------------------------------------------------------------------------------------------------------------------------------

// Where the head's B-to-A rotation alone puts the pixel (the shader's pHeadPrev).
bool headPrev(const FrameGenSynthCb& cb, float px, float py, float out[2]) {
    float d[3], dh[3], hd[9];
    temporalPixelToDir(px, py, cb.tanB, RW, RH, d);
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) hd[r * 3 + k] = cb.hp[r][k];
    temporalApply3(hd, d, dh);
    return temporalDirToPixel(dh, cb.tanB, RW, RH, &out[0], &out[1]);
}

// The vector field of a surface the ship moves at screen velocity m (B to A, pixels) under the head rotation of `cb`: the head's part plus m.
void fillVectors(Scene& sc, const FrameGenSynthCb& cb, float mx, float my, const std::function<bool(int, int)>& inside) {
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) {
            float h[2];
            if (!inside(x, y) || !headPrev(cb, static_cast<float>(x), static_cast<float>(y), h)) continue;
            sc.mv[(static_cast<size_t>(y) * RW + x) * 2] = h[0] + mx - static_cast<float>(x);
            sc.mv[(static_cast<size_t>(y) * RW + x) * 2 + 1] = h[1] + my - static_cast<float>(y);
        }
}

struct Worst { float err = 0.0f; int n = 0; };

// For every output pixel whose render position is `margin` or more from the edges: the reference's landing of the position S read B at must be the pixel's own render
// position. `landing` is the reference for a render position (it knows the class, the depth and the vector there).
Worst mappingError(const Run& r, const Scene& sc, int margin, const std::function<bool(float, float, float[2])>& landing) {
    Worst w;
    const float s = sc.scale();
    for (int oy = 0; oy < sc.oh; ++oy)
        for (int ox = 0; ox < sc.ow; ++ox) {
            const float cx = (static_cast<float>(ox) + 0.5f) / s - 0.5f, cy = (static_cast<float>(oy) + 0.5f) / s - 0.5f;
            if (cx < margin || cy < margin || cx > RW - 1 - margin || cy > RH - 1 - margin) continue;
            float pp[2];
            if (!landing(r.at(ox, oy, 0, sc.ow), r.at(ox, oy, 1, sc.ow), pp)) { w.err = 1e9f; continue; }
            w.err = std::max(w.err, std::hypot(pp[0] - cx, pp[1] - cy));
            ++w.n;
        }
    return w;
}

struct Deltas {
    FrameGenDeltas d;
    Deltas(float turnDeg, float tx, float ty, float tz) { rotY(turnDeg, d.Hd); d.th[0] = tx; d.th[1] = ty; d.th[2] = tz; }
};

float depthOf(float metres) { return kDepthB / metres; }

// Case 1: a cockpit plane at 1 m under a head translation and turn.
void case1Cockpit(Gpu& g, Scope& sc, int outScale) {
    char label[200];
    Scene S(RW * outScale, RH * outScale);
    S.gradient();
    std::fill(S.depth.begin(), S.depth.end(), depthOf(1.0f));
    FrameGenSynthCb cb;
    frameGenSynthFillCb(cb, RW, RH, S.ow, S.oh, kTan, 0.0f, kDepthB, 10.0f, 0.5f, Deltas(3.0f, -0.05f, 0.02f, -0.01f).d);
    const Run r = runSynth(g, S, cb);
    sc.ok(r.ok, "case 1: the synthesizer ran");
    if (!r.ok) return;
    const Worst w = mappingError(r, S, 10, [&](float x, float y, float pp[2]) { return frameGenSynthTarget(cb, x, y, 1.0f, false, 0.0f, 0.0f, pp); });
    std::snprintf(label, sizeof(label), "case 1: a cockpit plane at 1 m under a head step lands where the reference says, at %d:1 output (worst %.4f px over %d pixels)", outScale, w.err, w.n);
    sc.ok(w.n > 1000 && w.err < 0.1f, label);
    // The head moved the plane: the shift of the centre is the reference's, and it is not nothing.
    float pp[2];
    frameGenSynthTarget(cb, 64.0f, 48.0f, 1.0f, false, 0.0f, 0.0f, pp);
    std::snprintf(label, sizeof(label), "case 1: the plane really moves (the centre pixel lands %.2f px away)", std::hypot(pp[0] - 64.0f, pp[1] - 48.0f));
    sc.ok(std::hypot(pp[0] - 64.0f, pp[1] - 48.0f) > 1.0f, label);
    if (g_verbose && !sc.quiet) std::printf("  case 1 (%d:1): worst %.4f px over %d pixels, holes left %u, filled %u\n", outScale, w.err, w.n, r.stats[kFrameGenSynthStatsLeft], r.stats[kFrameGenSynthStatsFilled]);
}

// Case 2: a world plane at 50 m; mutation hooks run it with a turning head (m non-zero) and compare to the reference.
void case2World(Gpu& g, Scope& sc) {
    char label[200];
    {   // the head turning, the ship's screen motion constant: the ex2dh rule
        Scene S;
        S.gradient();
        std::fill(S.depth.begin(), S.depth.end(), depthOf(50.0f));
        FrameGenSynthCb cb;
        frameGenSynthFillCb(cb, RW, RH, RW, RH, kTan, 0.0f, kDepthB, 10.0f, 0.5f, Deltas(2.0f, 0.0f, 0.0f, 0.0f).d);
        const float mx = 3.0f, my = -2.0f;
        fillVectors(S, cb, mx, my, [](int, int) { return true; });
        const Run r = runSynth(g, S, cb);
        sc.ok(r.ok, "case 2: the synthesizer ran");
        if (!r.ok) return;
        const Worst w = mappingError(r, S, 12, [&](float x, float y, float pp[2]) {
            float h[2];
            if (!headPrev(cb, x, y, h)) return false;
            return frameGenSynthTarget(cb, x, y, 50.0f, false, h[0] + mx - x, h[1] + my - y, pp);
        });
        std::snprintf(label, sizeof(label), "case 2: a world plane at a constant ship screen velocity under a head turn lands where the ex2dh reference says (worst %.4f px over %d pixels)", w.err, w.n);
        sc.ok(w.n > 1000 && w.err < 0.1f, label);
        // The head's turn is a real part of the vector here (so a shader that left it in would double it).
        float h[2];
        headPrev(cb, 64.0f, 48.0f, h);
        std::snprintf(label, sizeof(label), "case 2: the head's part of the vector is %.2f px at the centre", std::hypot(h[0] - 64.0f, h[1] - 48.0f));
        sc.ok(std::hypot(h[0] - 64.0f, h[1] - 48.0f) > 1.5f, label);
        if (g_verbose && !sc.quiet) std::printf("  case 2: worst %.4f px over %d pixels\n", w.err, w.n);
    }
    {   // the head still: S(c) = B(c + f m), independent of the reference
        Scene S;
        S.gradient();
        std::fill(S.depth.begin(), S.depth.end(), depthOf(50.0f));
        FrameGenSynthCb cb;
        frameGenSynthFillCb(cb, RW, RH, RW, RH, kTan, 0.0f, kDepthB, 10.0f, 0.5f, Deltas(0.0f, 0.0f, 0.0f, 0.0f).d);
        for (size_t i = 0; i < S.mv.size(); i += 2) { S.mv[i] = 6.0f; S.mv[i + 1] = -2.0f; }
        const Run r = runSynth(g, S, cb);
        sc.ok(r.ok, "case 2b: the synthesizer ran");
        if (!r.ok) return;
        float worst = 0.0f;
        int n = 0;
        for (int y = 12; y < RH - 12; ++y)
            for (int x = 12; x < RW - 12; ++x) {
                worst = std::max(worst, std::hypot(r.at(x, y, 0, RW) - (x + 3.0f), r.at(x, y, 1, RW) - (y - 1.0f)));
                ++n;
            }
        std::snprintf(label, sizeof(label), "case 2b: with the head still a world pixel moves by -f of its vector: S(c) reads B at c + (3, -1) for the vector (6, -2) and f 0.5 (worst %.4f px)", worst);
        sc.ok(n > 1000 && worst < 0.05f, label);
    }
}

// Case 3: the far plane (the written depth 0) under the same.
void case3Far(Gpu& g, Scope& sc) {
    char label[200];
    Scene S;
    S.gradient();
    FrameGenSynthCb cb;
    frameGenSynthFillCb(cb, RW, RH, RW, RH, kTan, 0.0f, kDepthB, 10.0f, 0.5f, Deltas(-1.5f, 0.0f, 0.0f, 0.0f).d);
    const float mx = -2.0f, my = 1.5f;
    fillVectors(S, cb, mx, my, [](int, int) { return true; });
    const Run r = runSynth(g, S, cb);
    sc.ok(r.ok, "case 3: the synthesizer ran");
    if (!r.ok) return;
    const Worst w = mappingError(r, S, 12, [&](float x, float y, float pp[2]) {
        float h[2];
        if (!headPrev(cb, x, y, h)) return false;
        return frameGenSynthTarget(cb, x, y, 0.0f, true, h[0] + mx - x, h[1] + my - y, pp);
    });
    std::snprintf(label, sizeof(label), "case 3: the far plane rides the vectors' rotation like the world (worst %.4f px over %d pixels)", w.err, w.n);
    sc.ok(w.n > 1000 && w.err < 0.1f, label);
}

// Case 4: the on-foot 1 mm split.
void case4Split(Gpu& g, Scope& sc) {
    char label[220];
    Scene S;
    S.gradient();
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) S.depth[static_cast<size_t>(y) * RW + x] = x < 64 ? depthOf(0.0005f) : depthOf(0.002f);
    FrameGenSynthCb cb;
    frameGenSynthFillCb(cb, RW, RH, RW, RH, kTan, 0.0f, kDepthB, 0.001f, 0.5f, Deltas(0.0f, 4e-5f, 0.0f, 0.0f).d);
    const Run r = runSynth(g, S, cb);
    sc.ok(r.ok, "case 4: the synthesizer ran");
    if (!r.ok) return;
    float worstL = 0.0f, worstR = 0.0f;
    int nl = 0, nr = 0;
    for (int y = 12; y < RH - 12; ++y) {
        for (int x = 12; x < 52; ++x) {   // the cockpit half: the reference's landing of what it read is its own position
            float pp[2];
            if (!frameGenSynthTarget(cb, r.at(x, y, 0, RW), r.at(x, y, 1, RW), 0.0005f, false, 0.0f, 0.0f, pp)) { worstL = 1e9f; continue; }
            worstL = std::max(worstL, std::hypot(pp[0] - x, pp[1] - y));
            ++nl;
        }
        for (int x = 76; x < RW - 12; ++x) {   // the world half: the head still and the vectors zero, S is B unmoved
            worstR = std::max(worstR, std::hypot(r.at(x, y, 0, RW) - x, r.at(x, y, 1, RW) - y));
            ++nr;
        }
    }
    float pp[2];
    frameGenSynthTarget(cb, 30.0f, 48.0f, 0.0005f, false, 0.0f, 0.0f, pp);
    std::snprintf(label, sizeof(label), "case 4: 0.5 mm is the cockpit and moves with the head (the reference moves it %.2f px; worst %.4f px), 2 mm is the world and, its vectors zero, does not (worst %.4f px)",
                  std::fabs(pp[0] - 30.0f), worstL, worstR);
    sc.ok(nl > 500 && nr > 500 && worstL < 0.1f && worstR < 0.05f && std::fabs(pp[0] - 30.0f) > 1.0f, label);
}

// Case 5: the guard and the fill. The sky (left, written depth 0, grey 0.2) and an object at 50 m (right of x = 64, grey 0.9) whose vectors say it moves away from the
// sky edge: at f = 0.5 it lands 3.5 px to the right.
struct Edge { bool ran = false; float skyWorst = 0.0f, objWorst = 0.0f; unsigned filled = 0, left = 0, guard = 0; };

Edge case5Edge(Gpu& g, Scope& sc, bool report) {
    Edge e;
    char label[260];
    Scene S;
    S.grey([](int x, int) { return x < 64 ? 0.2f : 0.9f; });
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) {
            S.depth[static_cast<size_t>(y) * RW + x] = x < 64 ? 0.0f : depthOf(50.0f);
            if (x >= 64) S.mv[(static_cast<size_t>(y) * RW + x) * 2] = -7.0f;
        }
    FrameGenSynthCb cb;
    frameGenSynthFillCb(cb, RW, RH, RW, RH, kTan, 0.0f, kDepthB, 10.0f, 0.5f, Deltas(0.0f, 0.0f, 0.0f, 0.0f).d);
    const Run r = runSynth(g, S, cb);
    sc.ok(r.ok, "case 5: the synthesizer ran");
    if (!r.ok) return e;
    e.ran = true;
    for (int y = 4; y < RH - 4; ++y) {
        for (int x = 8; x <= 66; ++x) e.skyWorst = std::max(e.skyWorst, std::fabs(r.at(x, y, 0, RW) - 0.2f));      // up to 1.5 px before the object's new edge at 67.5
        for (int x = 69; x < RW - 8; ++x) e.objWorst = std::max(e.objWorst, std::fabs(r.at(x, y, 0, RW) - 0.9f));  // and from 1.5 px after it
    }
    e.filled = r.stats[kFrameGenSynthStatsFilled];
    e.left = r.stats[kFrameGenSynthStatsLeft];
    e.guard = r.stats[kFrameGenSynthStatsGuard];
    std::snprintf(label, sizeof(label), "case 5: no sky pixel takes the object's colour (worst deviation from the sky %.4f, guard hits %u) and the strip the object left is sky (filled %u, left %u)", e.skyWorst, e.guard,
                  e.filled, e.left);
    sc.ok(e.skyWorst < 0.01f, label);
    std::snprintf(label, sizeof(label), "case 5: the object keeps its colour past its edge (worst deviation %.4f)", e.objWorst);
    sc.ok(e.objWorst < 0.01f, label);
    // The sky pixel beside the object takes the object's depth and, since 2026-11-07, its vector (FG_DILATE_MV): it moves with the object and loses its S pixels to the
    // object's own, so the 3x3 guard moves no read here (one a row before, when it moved by the sky's vector). Since 2026-11-08 (FG_HALO) the sky pixel 2 px out moves with
    // the object too, and the sky pixel 3 px out keeps on itself the footprint read that would reach into that halo: one guarded read a row, the sky's own colour.
    std::snprintf(label, sizeof(label), "case 5: the counters: the strip of 2 px by %d rows was filled from the background (%u), nothing was left (%u), the guard moved at most one read a row (%u)", RH, e.filled, e.left, e.guard);
    sc.ok(e.filled >= 2u * (RH - 2) && e.filled <= 2u * RH + 4 && e.left == 0 && e.guard <= static_cast<unsigned>(RH), label);
    if (g_verbose && report) std::printf("  case 5: sky worst %.4f, object worst %.4f, filled %u, left %u, guard %u\n", e.skyWorst, e.objWorst, e.filled, e.left, e.guard);
    return e;
}

// Case 13 (2026-11-07, eye dump 222433569001, the own hull smeared in a ship turn): the hull (right of x = 64, 60 m, still in the eye: vectors 0) over a background at
// 4000 m that the ship's turn moves away from it (vectors +8 px: at f = 0.5 S reads the background 4 px to the right). Red is the render x read: the background a gradient,
// the outline the game draws just outside the hull (x = 63, on the background's depth) -100, the hull 1000. The background just uncovered beside the hull (x 56 to 63) carries
// the temporal pass's rejection sentinel (size * 2), not a vector. Expected: the background moves as the background up to the hole the hull leaves (S reads x + 4 for x <= 58),
// and the outline stays on the hull (S at 63 reads -100).
struct HullEdge { bool ran = false; float bgWorst = 0.0f, outline = 0.0f; unsigned filled = 0; };

HullEdge case13HullEdge(Gpu& g, Scope& sc, bool report) {
    HullEdge e;
    char label[260];
    Scene S;
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) {
            float* p = &S.rgba[(static_cast<size_t>(y) * RW + x) * 4];
            p[0] = x < 63 ? static_cast<float>(x) : (x == 63 ? -100.0f : 1000.0f);
            p[1] = static_cast<float>(y); p[2] = 0.5f; p[3] = 1.0f;
            const size_t i = static_cast<size_t>(y) * RW + x;
            S.depth[i] = x < 64 ? depthOf(4000.0f) : depthOf(60.0f);
            const bool sentinel = x >= 56 && x < 64;
            S.mv[i * 2] = x >= 64 ? 0.0f : (sentinel ? 2.0f * RW : 8.0f);
            S.mv[i * 2 + 1] = sentinel ? 2.0f * RH : 0.0f;
        }
    FrameGenSynthCb cb;
    frameGenSynthFillCb(cb, RW, RH, RW, RH, kTan, 0.0f, kDepthB, 10.0f, 0.5f, Deltas(0.0f, 0.0f, 0.0f, 0.0f).d);
    const Run r = runSynth(g, S, cb);
    sc.ok(r.ok, "case 13: the synthesizer ran");
    if (!r.ok) return e;
    e.ran = true;
    e.outline = -1e9f;
    for (int y = 4; y < RH - 4; ++y) {
        for (int x = 8; x <= 58; ++x) e.bgWorst = std::max(e.bgWorst, std::fabs(r.at(x, y, 0, RW) - (x + 4.0f)));
        e.outline = std::max(e.outline, r.at(63, y, 0, RW));
    }
    std::snprintf(label, sizeof(label), "case 13: the background uncovered beside the hull (the rejection sentinel in its vectors) moves as the background (worst %.4f px)", e.bgWorst);
    sc.ok(e.bgWorst < 0.1f, label);
    std::snprintf(label, sizeof(label), "case 13: the outline just outside the hull stays on the hull (S at x 63 reads %.2f, the outline -100)", e.outline);
    sc.ok(e.outline < -99.0f, label);
    // Since the fill carries a hole back by the background's motion (FG_FILL_REPROJ), a sentinel pixel thrown off the image leaves a hole the fill reads right: the holes
    // themselves say so. Only the strip the hull uncovers (4 px a row) is a hole.
    e.filled = r.stats[kFrameGenSynthStatsFilled] + r.stats[kFrameGenSynthStatsLeft];
    std::snprintf(label, sizeof(label), "case 13: only the strip the hull uncovers is a hole (%u holes, at most %d)", e.filled, 5 * RH);
    sc.ok(e.filled <= 5u * RH, label);
    if (g_verbose && report) std::printf("  case 13: background worst %.4f px, outline column %.2f, holes %u\n", e.bgWorst, e.outline, e.filled);
    return e;
}

// Case 15 (2026-11-07, eye dump 230157175005: the terrain uncovered behind the hull's strake in a ship turn torn into a sawtooth of strips): a near bar (x 60 to 67, 5 m, the
// cockpit: still with the head still) over a world plane whose distance falls down the image (200 m at the top to 105 m at the bottom, as terrain does) and whose vectors
// (+8, 0) carry it 4 px left at f = 0.5. Red is the render x read, green the render y. The strip the plane leaves beside the bar's left edge is a hole whose farthest
// neighbour within 8 px is up to 8 rows above it; the fill must carry on the plane's texture: every S pixel from x 50 to 59 reads B on the plane's row (green the row's own,
// within 0.6) and on the plane's side of the bar (red under 59.5), and the hole pixels are stretched along the motion (counted so): none is read where it is (a read of B at
// the hole itself, as a fill from the bar's side would take, is the plane 4 px off), since B has the bar where the plane's motion carries every hole back. The background pull alone read each row
// from 8 rows up and 2 px beyond: a sawtooth of rows.
struct UncoverFill { bool ran = false; float rowWorst = 0.0f, readMax = 0.0f; unsigned filled = 0, reproj = 0, stretch = 0, left = 0; };

UncoverFill case15Uncover(Gpu& g, Scope& sc, bool report) {
    UncoverFill e;
    char label[300];
    Scene S;
    S.gradient();
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) {
            const size_t i = static_cast<size_t>(y) * RW + x;
            const bool bar = x >= 60 && x < 68;
            S.depth[i] = bar ? depthOf(5.0f) : depthOf(200.0f - static_cast<float>(y));
            S.mv[i * 2] = bar ? 0.0f : 8.0f;
        }
    FrameGenSynthCb cb;
    frameGenSynthFillCb(cb, RW, RH, RW, RH, kTan, 0.0f, kDepthB, 10.0f, 0.5f, Deltas(0.0f, 0.0f, 0.0f, 0.0f).d);
    const Run r = runSynth(g, S, cb);
    sc.ok(r.ok, "case 15: the synthesizer ran");
    if (!r.ok) return e;
    e.ran = true;
    e.readMax = -1e9f;
    for (int y = 12; y < RH - 12; ++y)
        for (int x = 50; x < 59; ++x) {   // x 59, the plane's edge pixel that took the bar's depth, stays (and the guard's)
            e.rowWorst = std::max(e.rowWorst, std::fabs(r.at(x, y, 1, RW) - static_cast<float>(y)));
            e.readMax = std::max(e.readMax, r.at(x, y, 0, RW));
        }
    e.filled = r.stats[kFrameGenSynthStatsFilled];
    e.reproj = r.stats[kFrameGenSynthStatsFillReproj];
    e.stretch = r.stats[kFrameGenSynthStatsFillStretch];
    e.left = r.stats[kFrameGenSynthStatsLeft];
    std::snprintf(label, sizeof(label), "case 15: the plane uncovered beside the bar carries on its texture row for row (worst row error %.3f px) from the plane's side of the bar (largest x read %.2f)",
                  e.rowWorst, e.readMax);
    sc.ok(e.rowWorst < 0.6f && e.readMax < 59.5f, label);
    std::snprintf(label, sizeof(label), "case 15: the strip's holes are stretched along the plane's motion (filled %u: reprojected %u, stretched %u; left %u)", e.filled, e.reproj, e.stretch, e.left);
    sc.ok(e.stretch >= 3u * (RH - 2) && e.reproj == 0u && e.stretch <= e.filled && e.left == 0, label);
    if (g_verbose && report) std::printf("  case 15: row worst %.4f px, largest x read %.2f, filled %u, reprojected %u, stretched %u, left %u\n", e.rowWorst, e.readMax, e.filled, e.reproj, e.stretch, e.left);
    return e;
}

// ---- case 14: S's resample keeps B's sharpness ------------------------------------------------------------------------------------------------------------
//
// A world plane at 50 m, the head still, every pixel's vector (m, 0), f = 0.5: S reads B m / 2 px along x. B is grey on the 8-bit grid. A still scene (m = 0) and an
// integer read (m = 4: 2 px) give B's texels exactly; a sinusoid of period 5 px read a quarter pixel off keeps its high-frequency energy (the RMS of the 1-D Laplacian, S
// over B: 0.965 Catmull-Rom with the clamp, 0.861 bilinear, computed offline on the same 8-bit pattern); a step read 0.3 px off rings nowhere outside the step's two levels.
struct SharpRead { bool ran = false; float still = 0.0f, shifted = 0.0f, energy = 0.0f, lo = 0.0f, hi = 0.0f; };

Run sharpRun(Gpu& g, const std::function<float(int)>& grey, float m, int outScale = 1) {
    Scene S(RW * outScale, RH * outScale);
    S.grey([&](int x, int) { return std::round(grey(x / outScale) * 255.0f) / 255.0f; });
    std::fill(S.depth.begin(), S.depth.end(), depthOf(50.0f));
    for (size_t i = 0; i < static_cast<size_t>(RW) * RH; ++i) { S.mv[i * 2] = m; S.mv[i * 2 + 1] = 0.0f; }
    FrameGenSynthCb cb;
    frameGenSynthFillCb(cb, RW, RH, S.ow, S.oh, kTan, 0.0f, kDepthB, 10.0f, 0.5f, Deltas(0.0f, 0.0f, 0.0f, 0.0f).d);
    return runSynth(g, S, cb);
}

SharpRead case14Sharp(Gpu& g, Scope& sc, bool report) {
    SharpRead e;
    char label[260];
    const auto sine = [](int x) { return 0.5f + 0.4f * std::sin(2.0f * kPi * static_cast<float>(x) / 5.0f); };
    const auto step = [](int x) { return x < 64 ? 0.1f : 0.9f; };
    const auto b8 = [](float v) { return std::round(v * 255.0f) / 255.0f; };
    // Identity: a still scene, at 1:1 and 2:1, and an integer read, return B's texels bit for bit.
    for (int scale = 1; scale <= 2; ++scale) {
        const Run r = sharpRun(g, sine, 0.0f, scale);
        sc.ok(r.ok, "case 14: the synthesizer ran (still)");
        if (!r.ok) return e;
        for (int y = 0; y < RH * scale; ++y)
            for (int x = 0; x < RW * scale; ++x) e.still = std::max(e.still, std::fabs(r.at(x, y, 0, RW * scale) - b8(sine(x / scale))));
    }
    std::snprintf(label, sizeof(label), "case 14: a still scene's S is B exactly, at 1:1 and 2:1 output (worst %.3g)", e.still);
    sc.ok(e.still == 0.0f, label);
    {
        const Run r = sharpRun(g, sine, 4.0f);
        sc.ok(r.ok, "case 14: the synthesizer ran (integer read)");
        if (!r.ok) return e;
        float fwd = 0.0f, back = 0.0f;   // S(x) against B(x - 2) and B(x + 2)
        for (int y = 0; y < RH; ++y)
            for (int x = 8; x < RW - 8; ++x) {
                fwd = std::max(fwd, std::fabs(r.at(x, y, 0, RW) - b8(sine(x - 2))));
                back = std::max(back, std::fabs(r.at(x, y, 0, RW) - b8(sine(x + 2))));
            }
        e.shifted = std::min(fwd, back);
        std::snprintf(label, sizeof(label), "case 14: a read 2 px off returns B's texels exactly (worst %.3g)", e.shifted);
        sc.ok(e.shifted == 0.0f, label);
    }
    {
        const Run r = sharpRun(g, sine, 0.5f);
        sc.ok(r.ok, "case 14: the synthesizer ran (quarter-pixel read)");
        if (!r.ok) return e;
        double es = 0.0, eb = 0.0;
        for (int y = 0; y < RH; ++y)
            for (int x = 8; x < RW - 8; ++x) {
                const double ls = 2.0 * r.at(x, y, 0, RW) - r.at(x - 1, y, 0, RW) - r.at(x + 1, y, 0, RW);
                const double lb = 2.0 * b8(sine(x)) - b8(sine(x - 1)) - b8(sine(x + 1));
                es += ls * ls;
                eb += lb * lb;
            }
        e.energy = static_cast<float>(std::sqrt(es / eb));
        std::snprintf(label, sizeof(label), "case 14: a 5 px period read a quarter pixel off keeps its high-frequency energy (%.3f of B's; Catmull-Rom 0.965, bilinear 0.861)", e.energy);
        sc.ok(e.energy >= 0.95f && e.energy <= 1.05f, label);
    }
    {
        const Run r = sharpRun(g, step, 0.6f);
        sc.ok(r.ok, "case 14: the synthesizer ran (step)");
        if (!r.ok) return e;
        e.lo = 1e9f; e.hi = -1e9f;
        for (int y = 0; y < RH; ++y)
            for (int x = 0; x < RW; ++x) { e.lo = std::min(e.lo, r.at(x, y, 0, RW)); e.hi = std::max(e.hi, r.at(x, y, 0, RW)); }
        std::snprintf(label, sizeof(label), "case 14: a step read 0.3 px off rings nowhere outside its two levels (S %.4f to %.4f, B %.4f to %.4f)", e.lo, e.hi, b8(0.1f), b8(0.9f));
        sc.ok(e.lo >= b8(0.1f) && e.hi <= b8(0.9f), label);
    }
    e.ran = true;
    if (g_verbose && report) std::printf("  case 14: still %.3g, integer read %.3g, energy %.4f, step %.4f..%.4f\n", e.still, e.shifted, e.energy, e.lo, e.hi);
    return e;
}

// ---- case 6: the module ----------------------------------------------------------------------------------------------------------------------------------

struct ModuleFrame {
    Tex out, depth, mv;
};

void case6Module(Gpu& g, Scope& sc) {
    char label[260];
    Scene base;
    base.depth.assign(base.depth.size(), 0.0f);
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) base.depth[static_cast<size_t>(y) * RW + x] = x < 40 ? 0.0f : (x < 84 ? depthOf(0.8f) : depthOf(30.0f));
    // A scrolling pattern: frame k shows the pattern shifted right by 4 k px; the vectors (B to A) say so.
    const auto frameScene = [&](int k, float shift, bool vectors) {
        Scene s = base;
        s.grey([&](int x, int y) { return 0.5f + 0.3f * std::sin(0.21f * (static_cast<float>(x) - shift * k)) * std::cos(0.17f * y); });
        if (vectors)
            for (size_t i = 0; i < s.mv.size(); i += 2) s.mv[i] = -shift;
        return s;
    };
    FrameGenSynthFrame f;
    f.renderW = RW; f.renderH = RH; f.outW = RW; f.outH = RH;
    f.tanNow = kTan;
    f.depthA = 0.0f; f.depthB = kDepthB; f.splitMetres = 10.0f;
    f.depthWritten = true;

    // The key off: nothing is created, whatever the pass calls.
    frameGenSynthConfigure(false);
    {
        ModuleFrame m;
        const Scene s = frameScene(0, 0.0f, false);
        sc.ok(makeInputs(g, s, m.out, m.depth, m.mv), "case 6: the module's inputs were made");
        f.outSrv = m.out.srv.Get(); f.depthSrv = m.depth.srv.Get(); f.mvSrv = m.mv.srv.Get();
        for (uint32_t n = 1; n <= 6; ++n) { frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), n & 1u, n, f); frameGenSynthFrameBoundary(); }
        sc.ok(!frameGenSynthResident(), "case 6: with the key off nothing is created (no shader, buffer or texture is resident)");
        const FrameGenSynthWindow w = frameGenSynthOpenWindow();
        sc.ok(w.frames[0] == 0 && w.frames[1] == 0 && w.skipped == 0 && frameGenSynthShown(0, 6) == nullptr, "case 6: with the key off nothing is built, counted or handed out");
    }

    // On, a static scene on both eyes: the variants alternate, the f = 1 ones are scored against the next frame, and a static scene scores in bin 0 with no hole.
    frameGenSynthConfigure(true);
    uint32_t frame = 100;
    {
        ModuleFrame m;
        const Scene s = frameScene(0, 0.0f, false);
        sc.ok(makeInputs(g, s, m.out, m.depth, m.mv), "case 6: the static inputs were made");
        f.outSrv = m.out.srv.Get(); f.depthSrv = m.depth.srv.Get(); f.mvSrv = m.mv.srv.Get();
        f.deltas = FrameGenDeltas{};
        int shown = 0, shownOnOdd = 0;
        for (int n = 0; n < 12; ++n, ++frame) {
            for (uint32_t eye = 0; eye < 2; ++eye) {
                frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), eye, frame, f);
                if (ID3D11Texture2D* st = frameGenSynthShown(eye, frame)) {
                    ++shown;
                    if (n & 1) ++shownOnOdd;
                    if (g_verbose && n == 0 && eye == 0) {
                        const std::vector<float> px = g.readTex<float>(st, RW * 4, RH);
                        const std::vector<float> in = s.rgba;
                        std::printf("  case 6: first S centre %.4f %.4f %.4f (B %.4f %.4f %.4f), corner %.4f (B %.4f)\n", px[(48 * RW + 64) * 4], px[(48 * RW + 64) * 4 + 1], px[(48 * RW + 64) * 4 + 2],
                                    in[(48 * RW + 64) * 4], in[(48 * RW + 64) * 4 + 1], in[(48 * RW + 64) * 4 + 2], px[0], in[0]);
                    }
                }
            }
            g.ctx->Flush();
            Sleep(15);
        }
        sc.ok(frameGenSynthResident(), "case 6: with the key on the synthesizer's resources are resident");
        sc.ok(shown == 12 && shownOnOdd == 0, "case 6: the f = 0.5 S is handed out on the even frames of each eye only (12 of 24 calls, none of the odd)");
        sc.ok(frameGenSynthShown(0, 9999) == nullptr, "case 6: a frame the module did not build has no S to hand out");
        // The dump asks for the f = 0.5 S on a frame that would build the f = 1 one: the next call of the eye (frame n, an odd turn) with wantShown.
        FrameGenSynthFrame ask = f;
        ask.wantShown = true;
        frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame++, f);
        frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame, ask);
        sc.ok(frameGenSynthShown(0, frame) != nullptr, "case 6: wantShown builds the f = 0.5 S on a turn that would have built the f = 1 one");
        ++frame;
        for (int k = 0; k < 4; ++k) { g.ctx->Flush(); Sleep(15); frame += 10; frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame, f); }   // gaps: read the rest back
        const FrameGenSynthWindow w = frameGenSynthOpenWindow();
        uint64_t px = 0, bin0 = 0;
        for (int c = 0; c < kFrameGenShadowClasses; ++c) { px += w.cls[c].pixels; bin0 += w.cls[c].hist[0]; }
        std::snprintf(label, sizeof(label), "case 6: the two variants alternate (%u built at f 0.5, %u at f 1) and the f = 1 ones are scored against the next frame (%u scored)", w.frames[0], w.frames[1], w.scored);
        sc.ok(w.frames[0] >= 13 && w.frames[1] >= 11 && w.scored >= 10 && w.scored <= w.frames[1], label);
        // The cells on the far/cockpit seam (a far pixel beside the cockpit takes the cockpit's class and motion, and the guard may read it a pixel off) differ by a few thousandths
        // of luma at most: almost every cell is in bin 0 and none is over 0.01.
        bool noneOver = true;
        for (int c = 0; c < kFrameGenShadowClasses; ++c) noneOver = noneOver && frameGenShadowShareOver(w.cls[c].hist, 0.01f) == 0.0f;
        std::snprintf(label, sizeof(label), "case 6: a static scene scores in bin 0 in every class (%llu of the %llu cells of %u eye-frames; none over 0.01)", static_cast<unsigned long long>(bin0),
                      static_cast<unsigned long long>(px), w.scored);
        sc.ok(px == static_cast<uint64_t>(w.scored) * QW * QH && bin0 * 100 >= px * 99 && noneOver && w.cells == px && w.cls[0].pixels > 0 && w.cls[1].pixels > 0 && w.cls[2].pixels > 0, label);
        if (g_verbose)
            for (int c = 0; c < kFrameGenShadowClasses; ++c) {
                std::printf("  case 6: class %d: %u cells, not in bin 0:", c, w.cls[c].pixels);
                for (int b = 1; b < kFrameGenShadowBins; ++b) if (w.cls[c].hist[b]) std::printf(" bin%d x%u", b, w.cls[c].hist[b]);
                std::printf("\n");
            }
        const uint64_t pixels = w.pixels;
        std::snprintf(label, sizeof(label), "case 6: a static scene fills no hole from the background (%llu) and leaves none (%llu); the guard moved %llu reads in %llu render pixels",
                      static_cast<unsigned long long>(w.filled), static_cast<unsigned long long>(w.left), static_cast<unsigned long long>(w.guard), static_cast<unsigned long long>(pixels));
        sc.ok(w.left == 0 && w.filled == 0 && pixels == static_cast<uint64_t>(w.frames[0] + w.frames[1]) * RW * RH, label);
        // The head did not move, so S was built for the head C has: the residual is nothing, the turned score is the raw one and every cell is inside S.
        std::snprintf(label, sizeof(label), "case 6: a still head has no residual rotation (%u samples, median %.4f, max %.4f deg) and the turned score is the raw one over the same cells (%llu of %llu, %llu outside)",
                      w.rotSamples, static_cast<double>(w.rotMedian), static_cast<double>(w.rotMax), static_cast<unsigned long long>(w.tcells), static_cast<unsigned long long>(w.cells),
                      static_cast<unsigned long long>(w.tout));
        sc.ok(w.rotSamples >= w.scored && w.rotMax < 0.01f && w.tcells == w.cells && w.tout == 0 && w.clamped == 0, label);
        // A gap is not scored: after an f = 1 frame, the next call 5 frames later scores nothing.
        frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 1, 4000, f);   // reads back what eye 1 still had in flight
        g.ctx->Flush();
        Sleep(15);
        frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 1, 4100, f);
        const unsigned scoredBefore = frameGenSynthOpenWindow().scored;
        for (int k = 0; k < 6; ++k) frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 1, 5000 + 7u * k, f);   // every call a gap from the one before it
        for (int k = 0; k < 4; ++k) { g.ctx->Flush(); Sleep(15); frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 1, 6000 + 7u * k, f); }
        sc.ok(frameGenSynthOpenWindow().scored == scoredBefore, "case 6: an f = 1 frame followed by a gap in the frame numbers is not scored");
        // A frame the module cannot build from is counted, not built.
        const FrameGenSynthWindow w0 = frameGenSynthOpenWindow();
        FrameGenSynthFrame bad = f;
        bad.depthWritten = false;
        frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame + 100, bad);
        bad = f;
        bad.mvSrv = nullptr;
        frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame + 101, bad);
        const FrameGenSynthWindow w1 = frameGenSynthOpenWindow();
        sc.ok(w1.skipped == w0.skipped + 2 && w1.frames[0] == w0.frames[0] && w1.frames[1] == w0.frames[1], "case 6: a frame with no depth, or no vectors, is counted as skipped and builds nothing");
    }

    // A scene scrolling 4 px a frame at the vectors' rate (a world surface at 30 m): the f = 1 S lands on the next frame, the no-warp comparison does not.
    if (g_verbose) std::printf("  case 6: device after the static block: 0x%08X\n", static_cast<unsigned>(g.dev->GetDeviceRemovedReason()));
    frameGenSynthConfigure(false);
    frameGenSynthFrameBoundary();
    if (g_verbose) std::printf("  case 6: device after the release: 0x%08X\n", static_cast<unsigned>(g.dev->GetDeviceRemovedReason()));
    sc.ok(!frameGenSynthResident(), "case 6: turning the key off releases every resource at the next frame boundary");
    frameGenSynthConfigure(true);
    {
        for (size_t i = 0; i < base.depth.size(); ++i) base.depth[i] = depthOf(30.0f);
        ModuleFrame m;   // outside the loop: the gap calls below still hold the last frame's views
        for (int k = 0; k < 14; ++k) {
            const Scene s = frameScene(k, 4.0f, true);
            if (!makeInputs(g, s, m.out, m.depth, m.mv)) { sc.ok(false, "case 6: the scrolling inputs were made"); return; }
            f.outSrv = m.out.srv.Get(); f.depthSrv = m.depth.srv.Get(); f.mvSrv = m.mv.srv.Get();
            frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, 300 + static_cast<uint32_t>(k), f);
            g.ctx->Flush();
            Sleep(15);
        }
        for (int k = 0; k < 4; ++k) { g.ctx->Flush(); Sleep(15); frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, 400 + 10u * k, f); }
        const FrameGenSynthWindow w = frameGenSynthOpenWindow();
        const FrameGenShadowClassStats& wc = w.cls[2];
        const double px = wc.pixels ? static_cast<double>(wc.pixels) : 1.0;
        const double mean = static_cast<double>(wc.sumErr1024) / 1024.0 / px, meanId = static_cast<double>(wc.sumErrId1024) / 1024.0 / px;
        std::snprintf(label, sizeof(label), "case 6: a world surface scrolling at the vectors' rate is predicted by the f = 1 S (mean error %.4f over %llu cells, %u scored) and not by the frame before it (no warp %.4f)", mean,
                      static_cast<unsigned long long>(wc.pixels), w.scored, meanId);
        sc.ok(w.scored >= 5 && wc.pixels > 0 && mean < 0.02 && meanId > 0.05 && meanId > 4.0 * mean, label);
        if (g_verbose && !sc.quiet) std::printf("  case 6: scrolling mean error %.4f vs no-warp %.4f, %u scored\n", mean, meanId, w.scored);
    }
    if (g_verbose) std::printf("  case 6: device after the scrolling block: 0x%08X\n", static_cast<unsigned>(g.dev->GetDeviceRemovedReason()));
    frameGenSynthConfigure(false);
    frameGenSynthFrameBoundary();
    sc.ok(!frameGenSynthResident(), "case 6: the key off again releases the resources");
}

// ---- case 7: the f = 1 S turned the way the compositor would turn it -------------------------------------------------------------------------------------------

void mul3(const float a[9], const float b[9], float out[9]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
}

struct TurnedScore { bool ran = false; double rawMean = 0, turnedMean = 0; uint64_t cells = 0, tcells = 0, tout = 0; float resMed = 0, resMax = 0; float wantMed = 0, wantMax = 0; };

// A far scene seen from a head that yaws by a_k degrees in frame k (the eye space of frame k is Q_k = rotY(a_k) of a fixed world): the picture is a function of the world
// direction, the vectors are the head's own (B to A, the ship still). The step a_k - a_(k-1) changes from frame to frame, so the head's extrapolation misses; the f = 1 S built
// at B for the head's constant-velocity step, scored at C raw and turned by the residual rotation.
TurnedScore case7Turned(Gpu& g) {
    TurnedScore t;
    const float angles[12] = {0.0f, 2.0f, 5.0f, 6.0f, 10.0f, 12.0f, 13.0f, 18.0f, 20.0f, 22.0f, 27.0f, 28.0f};   // steps 2 3 1 4 2 1 5 2 2 5 1
    float Q[12][9];
    for (int k = 0; k < 12; ++k) rotY(angles[k], Q[k]);
    const auto frameAt = [&](int k) {
        Scene sc;
        sc.grey([&](int x, int y) {
            float d[3], w[3];
            temporalPixelToDir(static_cast<float>(x), static_cast<float>(y), kTan, RW, RH, d);
            float qt[9];
            temporalTranspose3(Q[k], qt);
            temporalApply3(qt, d, w);
            return 0.5f + 0.35f * std::sin(7.0f * std::atan2(w[0], -w[2]));
        });
        return sc;
    };
    const auto hdOf = [&](int k, float hd[9]) {   // x_(k-1) = Hd x_k
        float qt[9];
        temporalTranspose3(Q[k], qt);
        mul3(Q[k - 1], qt, hd);
    };
    std::vector<float> angs;
    for (int k = 2; k < 12; k += 1) {
        // B = frame k - 1, C = frame k. The vectors of B are the head's own: its backward delta is hd(B).
        Scene B = frameAt(k - 1), C = frameAt(k);
        FrameGenDeltas dB, dC;
        hdOf(k - 1, dB.Hd);
        hdOf(k, dC.Hd);
        for (int y = 0; y < RH; ++y)
            for (int x = 0; x < RW; ++x) {
                float d[3], dh[3], pp[2];
                temporalPixelToDir(static_cast<float>(x), static_cast<float>(y), kTan, RW, RH, d);
                temporalApply3(dB.Hd, d, dh);
                if (!temporalDirToPixel(dh, kTan, RW, RH, &pp[0], &pp[1])) continue;
                B.mv[(static_cast<size_t>(y) * RW + x) * 2] = pp[0] - static_cast<float>(x);
                B.mv[(static_cast<size_t>(y) * RW + x) * 2 + 1] = pp[1] - static_cast<float>(y);
            }
        FrameGenSynthCb cbB;
        frameGenSynthFillCb(cbB, RW, RH, RW, RH, kTan, 0.0f, kDepthB, 10.0f, 1.0f, dB);
        Keep keep;
        const Run rb = runSynth(g, B, cbB, &keep);
        if (!rb.ok) return t;
        float kept[9], resid[9];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) kept[r * 3 + c] = cbB.rs[r][c];
        frameGenSynthResidual(kept, dC.Hd, resid);
        angs.push_back(temporalRotationAngleDeg(resid));
        FrameGenSynthCb cbC;
        frameGenSynthFillCb(cbC, RW, RH, RW, RH, kTan, 0.0f, kDepthB, 10.0f, 1.0f, dC, resid, kTan);
        FrameGenSynthEyeResult res;
        if (!runScore(g, C, cbC, keep, res)) return t;
        const FrameGenShadowClassStats& cls = res.cls[0];
        const FrameGenShadowClassStats& tc = res.tcls[0];
        t.rawMean += static_cast<double>(cls.sumErr1024) / 1024.0;
        t.turnedMean += static_cast<double>(tc.sumErr1024) / 1024.0;
        t.cells += cls.pixels;
        t.tcells += tc.pixels;
        t.tout += res.tout;
    }
    t.rawMean = t.cells ? t.rawMean / static_cast<double>(t.cells) : 0.0;
    t.turnedMean = t.tcells ? t.turnedMean / static_cast<double>(t.tcells) : 0.0;
    std::sort(angs.begin(), angs.end());
    t.wantMed = angs[angs.size() / 2];
    t.wantMax = angs.back();
    t.resMed = t.wantMed;
    t.resMax = t.wantMax;
    t.ran = true;
    return t;
}

void case7Check(Gpu& g, Scope& sc) {
    char label[300];
    const TurnedScore t = case7Turned(g);
    sc.ok(t.ran, "case 7: the turned score ran");
    if (!t.ran) return;
    std::snprintf(label, sizeof(label), "case 7: the head's extrapolation misses (raw mean error %.4f over %llu cells) and the turn by the residual rotation puts it right (turned mean %.4f over %llu cells, %llu outside S)",
                  t.rawMean, static_cast<unsigned long long>(t.cells), t.turnedMean, static_cast<unsigned long long>(t.tcells), static_cast<unsigned long long>(t.tout));
    sc.ok(t.cells > 5000 && t.rawMean > 0.03 && t.turnedMean < 0.012 && t.turnedMean * 4.0 < t.rawMean, label);
    if (g_verbose && !sc.quiet) std::printf("  case 7: raw %.4f, turned %.4f, %llu cells, %llu turned, %llu outside, the residual median %.3f max %.3f deg\n", t.rawMean, t.turnedMean,
                                            static_cast<unsigned long long>(t.cells), static_cast<unsigned long long>(t.tcells), static_cast<unsigned long long>(t.tout), t.wantMed, t.wantMax);
}

float halfToFloat(uint16_t h) {
    const int e = (h >> 10) & 31;
    const float m = static_cast<float>(h & 1023);
    const float v = e == 0 ? std::ldexp(m, -24) : std::ldexp(1024.0f + m, e - 25);
    return (h & 0x8000u) ? -v : v;
}

// The module's packing of an offset component (framegen_synth_shader.h synHalfBits): the half's bits, rounded toward zero. Finite inputs.
uint16_t truncToHalf(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    const uint32_t sign = (u >> 16) & 0x8000u, a = u & 0x7FFFFFFFu;
    if (a >= 0x47800000u) return static_cast<uint16_t>(sign | 0x7C00u);
    if (a < 0x38800000u) { float d; std::memcpy(&d, &a, 4); return static_cast<uint16_t>(sign | static_cast<uint32_t>(d * 16777216.0f)); }
    return static_cast<uint16_t>(sign | (((a >> 23) - 112u) << 10) | ((a >> 13) & 0x3FFu));
}

// Case 8: the offsets' storage, two halves packed in a uint and rounded toward zero as the module packs them (truncToHalf is synHalfBits's arithmetic).
void case8Offsets(Scope& sc) {
    char label[260];
    float worst512 = 0.0f, worst256 = 0.0f, worst2000 = 0.0f;
    bool towardZero = true;
    for (int i = -20000; i <= 20000; ++i) {
        const float v = static_cast<float>(i) * 0.1f * (1.0f + 0.000137f * static_cast<float>(i % 7));   // up to +-2000, off the grid of halves
        const float back = halfToFloat(truncToHalf(v));
        const float e = std::fabs(back - v);
        if (std::fabs(back) > std::fabs(v)) towardZero = false;
        if (std::fabs(v) <= 256.0f) worst256 = std::max(worst256, e);
        if (std::fabs(v) <= 512.0f) worst512 = std::max(worst512, e);
        if (std::fabs(v) <= kFrameGenSynthMaxOffset) worst2000 = std::max(worst2000, e);
    }
    std::snprintf(label, sizeof(label), "case 8: a read offset packed as a half, rounded toward zero, is exact to %.4f px up to 256 px, %.4f px up to 512 px and %.4f px up to the clamp (%.0f px)", worst256,
                  worst512, worst2000, static_cast<double>(kFrameGenSynthMaxOffset));
    sc.ok(towardZero && worst256 < 0.125f && worst512 < 0.25f && worst2000 < 1.0f && std::fabs(halfToFloat(truncToHalf(kFrameGenSynthNoRead)) - kFrameGenSynthNoRead) == 0.0f &&
              halfToFloat(truncToHalf(kFrameGenSynthMaxOffset)) == kFrameGenSynthMaxOffset && halfToFloat(truncToHalf(0.5f)) == 0.5f && halfToFloat(truncToHalf(1.0e-6f)) < 1.0e-6f &&
              kFrameGenSynthNoRead < -kFrameGenSynthMaxOffset * 1.5f,
          label);
}

// ---- case 9: the replay capture's writer (.agents/framegen-replay-format.md) ----------------------------------------------------------------------------------

std::string readWholeFile(const std::wstring& path) {
    std::string out;
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") || !fp) return out;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
    std::fclose(fp);
    return out;
}

bool pumpUntilIdle() {
    for (int i = 0; i < 4000 && !frameGenReplayIdle(); ++i) { frameGenReplayPump(); Sleep(5); }
    return frameGenReplayIdle();
}

std::wstring widen(const char* s) { return std::wstring(s, s + std::strlen(s)); }

void case9Replay(Gpu& g, Scope& sc) {
    char label[260];
    {   // the hash is a real SHA-256
        Sha256 a; a.update("abc", 3);
        sc.ok(a.hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "case 9: SHA-256 of \"abc\" is the published value");
        Sha256 e;
        sc.ok(e.hex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "case 9: SHA-256 of nothing is the published value");
        Sha256 m; const std::string block(1000, 'a'); for (int i = 0; i < 1000; ++i) m.update(block.data(), block.size());
        sc.ok(m.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "case 9: SHA-256 of a million a's (fed in pieces) is the published value");
    }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring root = std::wstring(tmp) + L"edvr_replay_test_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(root.c_str(), nullptr);
    Scene s;
    s.gradient();
    for (size_t i = 0; i < s.depth.size(); ++i) s.depth[i] = 0.001f * static_cast<float>(i % 97);
    for (size_t i = 0; i < s.mv.size(); ++i) s.mv[i] = 0.25f * static_cast<float>(i % 13);
    ModuleFrame m;
    if (!makeInputs(g, s, m.out, m.depth, m.mv)) { sc.ok(false, "case 9: inputs were made"); return; }
    FrameGenSynthFrame f;
    f.renderW = RW; f.renderH = RH; f.outW = RW; f.outH = RH;
    f.tanNow = kTan;
    f.depthA = 0.0f; f.depthB = kDepthB; f.splitMetres = 10.0f;
    f.depthWritten = true;
    f.outSrv = m.out.srv.Get(); f.depthSrv = m.depth.srv.Get(); f.mvSrv = m.mv.srv.Get();
    f.wantShown = true;
    f.worldValid = true;
    f.deltas = Deltas(1.0f, 0.5f, 0.25f, -0.125f).d;
    f.deltas.W[1] = 0.5f; f.deltas.tv[2] = 3.0f;
    const wchar_t* stamp = L"20260101-000000";
    uint32_t frame = 9000;

    // The trigger off (replay never set): nothing is captured, nothing is written, not even the folder.
    frameGenSynthConfigure(true);
    for (uint32_t k = 0; k < 8; ++k) frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame++, f);
    sc.ok(!frameGenReplayFinish(root.c_str(), stamp) && frameGenReplayIdle() && GetFileAttributesW((root + L"\\fgreplay_" + stamp).c_str()) == INVALID_FILE_ATTRIBUTES,
          "case 9: without the dump trigger nothing is captured and nothing is written");

    // A full run of six frames (a seventh asked for is ignored).
    f.replay = true;
    for (uint32_t k = 0; k < 7; ++k) { f.replayIndex = k; f.deltas.Hd[0] = 1.0f - 0.001f * k; frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame++, f); }
    sc.ok(frameGenReplayCaptured() == 6, "case 9: six consecutive frames were captured (not seven)");
    sc.ok(frameGenReplayFinish(root.c_str(), stamp), "case 9: the capture was handed to the writer");
    const std::wstring dir = root + L"\\fgreplay_" + stamp;
    sc.ok(GetFileAttributesW((dir + L"\\f0_out.bin").c_str()) == INVALID_FILE_ATTRIBUTES, "case 9: nothing is written on the frame it was handed over (the writer is the pump's)");
    sc.ok(pumpUntilIdle(), "case 9: the pump wrote everything and went idle");
    const std::string manifest = readWholeFile(dir + L"\\manifest.json");
    sc.ok(manifest.find("\"frames\": 6") != std::string::npos && manifest.find("\"stopped_reason\": null") != std::string::npos && manifest.find("\"eye\": \"left\"") != std::string::npos &&
              manifest.find("\"stamp\": \"20260101-000000\"") != std::string::npos && manifest.find("\"build\": \"") != std::string::npos,
          "case 9: the manifest names the stamp, the build, the eye, six frames and no early stop");
    int goodFiles = 0, files = 0;
    const size_t want[3] = {static_cast<size_t>(RW) * RH * 16, static_cast<size_t>(RW) * RH * 4, static_cast<size_t>(RW) * RH * 4};
    const char* part[3] = {"out", "depth", "mv"};
    std::vector<uint16_t> mvHalf(s.mv.size());
    for (size_t i = 0; i < s.mv.size(); ++i) mvHalf[i] = floatToHalf(s.mv[i]);
    const void* ref[3] = {s.rgba.data(), s.depth.data(), mvHalf.data()};
    for (uint32_t k = 0; k < 6; ++k)
        for (int i = 0; i < 3; ++i) {
            char name[40];
            std::snprintf(name, sizeof(name), "f%u_%s.bin", k, part[i]);
            ++files;
            const std::string body = readWholeFile(dir + L"\\" + widen(name));
            Sha256 h; h.update(body.data(), body.size());
            char probe[200];
            std::snprintf(probe, sizeof(probe), "{\"name\": \"%s\", \"bytes\": %zu, \"sha256\": \"%s\"}", name, want[i], h.hex().c_str());
            if (body.size() == want[i] && std::memcmp(body.data(), ref[i], want[i]) == 0 && manifest.find(probe) != std::string::npos) ++goodFiles;
        }
    std::snprintf(label, sizeof(label), "case 9: all 18 bins have the right byte size, the exact texels and a manifest entry with their SHA-256 (%d of %d)", goodFiles, files);
    sc.ok(goodFiles == 18, label);
    const std::string j3 = readWholeFile(dir + L"\\f3.json");
    if (g_verbose) std::printf("%s", j3.c_str());
    sc.ok(j3.find("\"render_w\": 128") != std::string::npos && j3.find("\"out_format_name\": \"R32G32B32A32_FLOAT\"") != std::string::npos && j3.find("\"depth_format_name\": \"R32_FLOAT\"") != std::string::npos &&
              j3.find("\"mv_format_name\": \"R16G16_FLOAT\"") != std::string::npos && j3.find("\"variant\": 0") != std::string::npos && j3.find("\"world_valid\": true") != std::string::npos &&
              j3.find("\"tan_prev\": null") != std::string::npos && j3.find("\"hd\": [0.99699") != std::string::npos && j3.find("\"tv_cam\": [0, 0, 3]") != std::string::npos && j3.find("\"frame\": 9011") != std::string::npos &&
              j3.find("\"qpc_freq\"") != std::string::npos && j3.find("\"world_w\": [1, 0.5, 0,") != std::string::npos && j3.find("\"split_metres\": 10") != std::string::npos,
          "case 9: f3.json carries the frame's sizes, formats, variant, head and world deltas, pass frame and clock");

    // A broken run (frame 2 never came): the capture stops at 2 frames and the manifest says why.
    const std::wstring dir2 = root + L"\\fgreplay_20260101-000001";
    for (uint32_t k : {0u, 1u, 3u, 4u}) { f.replayIndex = k; frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame++, f); }
    sc.ok(frameGenReplayFinish(root.c_str(), L"20260101-000001") && pumpUntilIdle(), "case 9: a run with a missing frame was written");
    const std::string manifest2 = readWholeFile(dir2 + L"\\manifest.json");
    sc.ok(manifest2.find("\"frames\": 2") != std::string::npos && manifest2.find("was not captured") != std::string::npos && GetFileAttributesW((dir2 + L"\\f2_out.bin").c_str()) == INVALID_FILE_ATTRIBUTES,
          "case 9: the broken run stops at the missing frame and the manifest says why");

    // The key off: a run in progress is dropped at the next boundary and nothing more is captured.
    f.replayIndex = 0; frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame++, f);
    frameGenSynthConfigure(false);
    frameGenSynthFrameBoundary();
    f.replayIndex = 1; frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame++, f);
    sc.ok(frameGenReplayCaptured() == 0 && !frameGenReplayFinish(root.c_str(), L"20260101-000002") && frameGenReplayIdle(), "case 9: with the key off nothing is captured and a run in progress is dropped");

    for (const std::wstring& d : {dir, dir2}) {   // clean up
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((d + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(d.c_str());
    }
    RemoveDirectoryW(root.c_str());
}

// ---- case 10: the head step from the runtime's slot pose (Phase 2.2) --------------------------------------------------------------------------------------------
// B's head and S's head are given as the runtime hands them (rigid row-major 3x4, head to the seated space) with the eye's eye-to-head; the module's step is frameGenSlotStep.
// The reference here does not use it: a B pixel at depth z is put in the seated space through B's head and eye, brought back through S's, and projected.
struct SlotPoses {
    float headB[12], headS[12], eye[12];
};
SlotPoses slotPoses() {
    SlotPoses p{};
    const float a = 1.5f * kPi / 180.0f, b = 0.5f * kPi / 180.0f;
    // B: turned 10 degrees about y, at (0.1, 1.2, -0.3); S: B turned a further 1.5 degrees about y and 0.5 about x (case 1's size of step), and moved (2, 1, -1) cm. The eye 32 mm left of the head.
    float ry[9], rb[9];
    rotY(10.0f, rb);
    const float ra[9] = {std::cos(a), 0, std::sin(a), 0, 1, 0, -std::sin(a), 0, std::cos(a)};
    const float rx[9] = {1, 0, 0, 0, std::cos(b), -std::sin(b), 0, std::sin(b), std::cos(b)};
    float rab[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            rab[r * 3 + c] = 0.0f;
            for (int k = 0; k < 3; ++k) rab[r * 3 + c] += ra[r * 3 + k] * rx[k * 3 + c];
        }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            ry[r * 3 + c] = 0.0f;
            for (int k = 0; k < 3; ++k) ry[r * 3 + c] += rb[r * 3 + k] * rab[k * 3 + c];
        }
    const float tB[3] = {0.1f, 1.2f, -0.3f}, tS[3] = {0.12f, 1.21f, -0.31f};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            p.headB[r * 4 + c] = rb[r * 3 + c];
            p.headS[r * 4 + c] = ry[r * 3 + c];
            p.eye[r * 4 + c] = r == c ? 1.0f : 0.0f;
        }
        p.headB[r * 4 + 3] = tB[r];
        p.headS[r * 4 + 3] = tS[r];
        p.eye[r * 4 + 3] = r == 0 ? -0.032f : 0.0f;
    }
    return p;
}
// Where a B pixel at depth z (farPlane: a direction only) lands in S, from the poses alone.
bool slotLanding(const SlotPoses& p, float px, float py, float z, bool farPlane, float out[2]) {
    float d[3];
    temporalPixelToDir(px, py, kTan, RW, RH, d);
    const auto world = [](const float head[12], const float eye[12], const float v[3], bool point, float w[3]) {
        float e[3];
        for (int r = 0; r < 3; ++r) e[r] = eye[r * 4 + 0] * v[0] + eye[r * 4 + 1] * v[1] + eye[r * 4 + 2] * v[2] + (point ? eye[r * 4 + 3] : 0.0f);
        for (int r = 0; r < 3; ++r) w[r] = head[r * 4 + 0] * e[0] + head[r * 4 + 1] * e[1] + head[r * 4 + 2] * e[2] + (point ? head[r * 4 + 3] : 0.0f);
    };
    const auto local = [](const float head[12], const float eye[12], const float w[3], bool point, float v[3]) {
        float h[3], e[3];
        for (int r = 0; r < 3; ++r) h[r] = w[r] - (point ? head[r * 4 + 3] : 0.0f);
        for (int c = 0; c < 3; ++c) e[c] = head[0 * 4 + c] * h[0] + head[1 * 4 + c] * h[1] + head[2 * 4 + c] * h[2];   // the transpose
        for (int r = 0; r < 3; ++r) e[r] -= point ? eye[r * 4 + 3] : 0.0f;
        for (int c = 0; c < 3; ++c) v[c] = eye[0 * 4 + c] * e[0] + eye[1 * 4 + c] * e[1] + eye[2 * 4 + c] * e[2];
    };
    float v[3] = {d[0] * z, d[1] * z, d[2] * z};
    if (farPlane) { v[0] = d[0]; v[1] = d[1]; v[2] = d[2]; }
    float w[3], s[3];
    world(p.headB, p.eye, v, !farPlane, w);
    local(p.headS, p.eye, w, !farPlane, s);
    return temporalDirToPixel(s, kTan, RW, RH, &out[0], &out[1]);
}
// How the host builds the block: 0 the step from the poses (the module's), 1 the pose ignored (the extrapolation of a different head delta), 2 the step the wrong way round.
FrameGenSynthCb slotCb(const SlotPoses& p, const FrameGenDeltas& d, int how) {
    float rot[9], t[3];
    if (how == 2) frameGenSlotStep(p.headB, p.headS, p.eye, rot, t);
    else frameGenSlotStep(p.headS, p.headB, p.eye, rot, t);
    FrameGenSynthCb cb;
    frameGenSynthFillCb(cb, RW, RH, RW, RH, kTan, 0.0f, kDepthB, 10.0f, 0.5f, d, nullptr, nullptr, how == 1 ? nullptr : rot, how == 1 ? nullptr : t);
    return cb;
}
void case10Slot(Gpu& g, Scope& sc, int how) {
    char label[220];
    const SlotPoses p = slotPoses();
    const Deltas d(1.0f, -0.01f, 0.0f, 0.0f);   // the pass's own head delta: what the extrapolation would carry (not S's pose)
    {   // a cockpit plane at 1 m: rotation and translation
        Scene S;
        S.gradient();
        std::fill(S.depth.begin(), S.depth.end(), depthOf(1.0f));
        const FrameGenSynthCb cb = slotCb(p, d.d, how);
        float cpu = 0.0f;   // the block's own CPU landing (frameGenSynthTarget) against the poses' at every render pixel
        for (int y = 0; y < RH; ++y)
            for (int x = 0; x < RW; ++x) {
                float a[2], b[2];
                if (frameGenSynthTarget(cb, static_cast<float>(x), static_cast<float>(y), 1.0f, false, 0.0f, 0.0f, a) && slotLanding(p, static_cast<float>(x), static_cast<float>(y), 1.0f, false, b))
                    cpu = std::max(cpu, std::hypot(a[0] - b[0], a[1] - b[1]));
            }
        std::snprintf(label, sizeof(label), "case 10: the block's step lands every pixel where the poses do (CPU, worst %.5f px)", cpu);
        sc.ok(cpu < 0.01f, label);
        const Run r = runSynth(g, S, cb);
        sc.ok(r.ok, "case 10: the synthesizer ran");
        if (!r.ok) return;
        const Worst w = mappingError(r, S, 12, [&](float x, float y, float pp[2]) { return slotLanding(p, x, y, 1.0f, false, pp); });
        std::snprintf(label, sizeof(label), "case 10: a cockpit plane at 1 m lands where S's own head (the runtime's pose) sees it (worst %.4f px over %d pixels)", w.err, w.n);
        sc.ok(w.n > 1000 && w.err < 0.1f, label);
        if (g_verbose && !sc.quiet) std::printf("  case 10 (cockpit): worst %.4f px over %d pixels\n", w.err, w.n);
    }
    {   // the far plane: the rotation alone; the vectors are the head's own (hp), so the ship's motion is nothing
        Scene S;
        S.gradient();
        const FrameGenSynthCb cb = slotCb(p, d.d, how);
        fillVectors(S, cb, 0.0f, 0.0f, [](int, int) { return true; });
        const Run r = runSynth(g, S, cb);
        sc.ok(r.ok, "case 10: the synthesizer ran (far)");
        if (!r.ok) return;
        const Worst w = mappingError(r, S, 12, [&](float x, float y, float pp[2]) { return slotLanding(p, x, y, 1.0f, true, pp); });
        std::snprintf(label, sizeof(label), "case 10: the far plane turns by S's own head's rotation (worst %.4f px over %d pixels)", w.err, w.n);
        sc.ok(w.n > 1000 && w.err < 0.1f, label);
        if (g_verbose && !sc.quiet) std::printf("  case 10 (far): worst %.4f px over %d pixels\n", w.err, w.n);
    }
}

// ---- case 11: S's transfer, the module and the runtime's side on two WARP devices (Phase 2.2) ------------------------------------------------------------------
bool readRgba32(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* src, std::vector<float>& out) {
    D3D11_TEXTURE2D_DESC td{};
    src->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &st))) return false;
    ctx->CopyResource(st.Get(), src);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    out.resize(static_cast<size_t>(td.Width) * td.Height * 4);
    for (UINT y = 0; y < td.Height; ++y) std::memcpy(&out[static_cast<size_t>(y) * td.Width * 4], static_cast<const char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, td.Width * 16u);
    ctx->Unmap(st.Get(), 0);
    return true;
}
void case11Transfer(Gpu& g, Scope& sc) {
    char label[240];
    ComPtr<ID3D11Device> xr;
    ComPtr<ID3D11DeviceContext> xrCtx;
    D3D_FEATURE_LEVEL level{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &xr, &level, &xrCtx))) { sc.ok(false, "case 11: a second WARP device (the XR side)"); return; }
    frameGenSynthConfigure(false);
    frameGenSynthFrameBoundary();   // a fresh arm: the eyes' alternation starts on f = 0.5
    Scene s;
    s.gradient();
    std::fill(s.depth.begin(), s.depth.end(), depthOf(1.0f));
    ModuleFrame m;
    if (!makeInputs(g, s, m.out, m.depth, m.mv)) { sc.ok(false, "case 11: inputs were made"); return; }
    FrameGenSynthFrame f;
    f.renderW = RW; f.renderH = RH; f.outW = RW; f.outH = RH;
    f.tanNow = kTan;
    f.depthA = 0.0f; f.depthB = kDepthB; f.splitMetres = 10.0f;
    f.depthWritten = true;
    f.outSrv = m.out.srv.Get(); f.depthSrv = m.depth.srv.Get(); f.mvSrv = m.mv.srv.Get();
    for (int i = 0; i < 9; ++i) f.slotRot[i] = (i % 4 == 0) ? 1.0f : 0.0f;   // S's head is B's: S is B
    f.slotPose = true;
    f.consumer = true;
    frameGenSynthConfigure(true);
    edvr::openxr::FrameGenSlotTransfer tr;
    uint64_t seq = 500;
    uint32_t frame = 9000;
    const auto build = [&](EdvrNativeFrameGenS& a) {
        f.slotSequence = ++seq;
        frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, ++frame, f);
        a = EdvrNativeFrameGenS{};
        a.size = EDVR_NATIVE_FRAMEGEN_S_SIZE_1; a.version = EDVR_NATIVE_FRAMEGEN_VERSION_1;
        return edvrFrameGenTakeS(0, &a);
    };
    EdvrNativeFrameGenS a{};
    // 1: the f = 0.5 S is written into the producer texture and published; the runtime opens it, consumes it, and holds B's picture (S is B here).
    sc.ok(build(a) == S_OK && a.sequence == seq && a.eye == 0 && a.state == EDVR_NATIVE_FRAMEGEN_S_PRODUCED && a.sharedHandle && a.textureSerial && a.poseRuntime == 1 &&
              a.width == RW && a.height == RH && a.gameBytes == static_cast<uint64_t>(RW) * RH * 16u * (a.path == EDVR_NATIVE_FRAMEGEN_PATH_COPY ? 2u : 1u),
          "case 11: the f = 0.5 S is produced for the slot's sequence, with its handle, serial, size, the runtime's pose, and its game-device bytes");
    const uint32_t path = a.path;
    std::snprintf(label, sizeof(label), "case 11: the path is uav or copy (here %s)", path == 1 ? "uav" : path == 2 ? "copy" : "none");
    sc.ok(path == EDVR_NATIVE_FRAMEGEN_PATH_UAV || path == EDVR_NATIVE_FRAMEGEN_PATH_COPY, label);
    if (g_verbose) std::printf("  case 11: the transfer's path on this device is %s (game-device bytes %llu)\n", path == 1 ? "uav" : path == 2 ? "copy" : "none", static_cast<unsigned long long>(a.gameBytes));
    EdvrNativeFrameGenS again{};
    again.size = EDVR_NATIVE_FRAMEGEN_S_SIZE_1; again.version = EDVR_NATIVE_FRAMEGEN_VERSION_1;
    sc.ok(edvrFrameGenTakeS(0, &again) == S_FALSE && again.sequence == 0, "case 11: an answer is taken once");
    sc.ok(path == 2 || frameGenSynthShown(0, frame) == nullptr, "case 11: on the uav path no private S is handed out");
    sc.ok(tr.open(xr.Get(), static_cast<HANDLE>(a.sharedHandle), a.textureSerial) == S_OK && tr.xrBytes() == static_cast<uint64_t>(RW) * RH * 16u, "case 11: the runtime opens the producer texture and makes its private copy");
    tr.markProduced(123);
    sc.ok(tr.pending() && tr.consume(0) == S_OK && !tr.pending(), "case 11: the runtime consumes the produced S at once (AcquireSync(1, 0))");
    std::vector<float> px;
    float worst = 0.0f;
    if (readRgba32(xr.Get(), xrCtx.Get(), tr.privateTexture(), px))
        for (int y = 12; y < RH - 12; ++y)
            for (int x = 12; x < RW - 12; ++x)
                for (int c = 0; c < 2; ++c) worst = std::max(worst, std::fabs(px[(static_cast<size_t>(y) * RW + x) * 4 + c] - s.rgba[(static_cast<size_t>(y) * RW + x) * 4 + c]));
    std::snprintf(label, sizeof(label), "case 11: the runtime's copy holds S (B itself under an identity step; worst %.4f px)", worst);
    sc.ok(!px.empty() && worst < 0.05f, label);
    // 2: the f = 1 S stays on the game device.
    sc.ok(build(a) == S_OK && a.state == EDVR_NATIVE_FRAMEGEN_S_KEPT && a.poseRuntime == 0, "case 11: the f = 1 S is kept (not handed over) and takes no slot pose");
    // 3: an S the runtime does not consume holds the next one back: busy, counted, not handed over.
    const uint32_t busy0 = frameGenSynthOpenWindow().busy;
    sc.ok(build(a) == S_OK && a.state == EDVR_NATIVE_FRAMEGEN_S_PRODUCED, "case 11: the next f = 0.5 S is produced");
    tr.markProduced(124);
    build(a);   // f = 1: on the uav path it cannot write the producer texture either (busy); on the copy path it builds privately
    sc.ok(build(a) == S_OK && a.state == EDVR_NATIVE_FRAMEGEN_S_BUSY && frameGenSynthOpenWindow().busy > busy0, "case 11: with the last S unconsumed the next f = 0.5 S is busy, counted, and not handed over");
    sc.ok(tr.consume(0) == S_OK, "case 11: the held S is consumed late");
    build(a);   // f = 1
    sc.ok(build(a) == S_OK && a.state == EDVR_NATIVE_FRAMEGEN_S_PRODUCED, "case 11: after it, S is produced again");
    tr.markProduced(125);
    sc.ok(tr.consume(0) == S_OK, "case 11: and consumed");
    // 4: a consume with nothing released times out at 0 ms and stays pending; let go, it gives nothing back.
    tr.markProduced(126);
    sc.ok(tr.consume(0) == static_cast<HRESULT>(WAIT_TIMEOUT) && tr.pending(), "case 11: a consume before the producer released times out (0 ms) and stays pending");
    tr.release();
    sc.ok(!tr.pending() && tr.xrBytes() == 0 && !tr.privateTexture(), "case 11: the runtime's side lets go of everything");
    // 5: the runtime gone (nothing consumes): after kBusyRemake builds held back the producer texture is made again, with a new serial the runtime would open.
    const uint64_t serialWas = a.textureSerial;
    bool remade = false;
    for (int k = 0; k < 24 && !remade; ++k) {
        build(a);
        if (a.state == EDVR_NATIVE_FRAMEGEN_S_PRODUCED && a.textureSerial != serialWas) remade = true;
    }
    sc.ok(remade, "case 11: an S never consumed is let go after its run of busy builds and the producer texture is made again (a new serial)");
    // 6: the runtime stops consuming (no consumer): the producer texture goes and S is built privately; the slot's pose counts as the runtime's or the fallback's.
    FrameGenSynthWindow w0 = frameGenSynthOpenWindow();
    f.consumer = false;
    build(a);
    build(a);
    sc.ok(a.state == EDVR_NATIVE_FRAMEGEN_S_KEPT && a.path == EDVR_NATIVE_FRAMEGEN_PATH_NONE && !a.sharedHandle && a.gameBytes == static_cast<uint64_t>(RW) * RH * 16u,
          "case 11: with no consumer the producer texture is let go and S is the private texture alone");
    f.slotPose = false;
    build(a);
    build(a);
    FrameGenSynthWindow w1 = frameGenSynthOpenWindow();
    sc.ok(w1.poseRuntime > w0.poseRuntime && w1.poseFallback > w0.poseFallback, "case 11: the f = 0.5 builds are counted by their head step: the slot's pose, or the extrapolation");
    sc.ok((w1.pathBits & 1u) && (w1.pathBits & (path == 1 ? 2u : 4u)), "case 11: the window says where S was written (private, and the transfer's path)");
    // 7: the key off: everything goes, and nothing is answered.
    frameGenSynthConfigure(false);
    frameGenSynthFrameBoundary();
    sc.ok(!frameGenSynthResident() && edvrFrameGenTakeS(0, &again) == S_FALSE, "case 11: the key off releases the producer texture with the rest, and nothing is answered");
    frameGenSynthConfigure(true);
    f.slotPose = true;
    f.consumer = true;
    // 8: Phase 2.3, Lock's key (everyHalf): the f = 0.5 S on every frame, no f = 1 S between them, each one produced for its own pair and consumed as that pair's.
    frameGenSynthFrameBoundary();
    f.everyHalf = true;
    bool every = true;
    for (int k = 0; k < 4; ++k) {
        every = every && build(a) == S_OK && a.state == EDVR_NATIVE_FRAMEGEN_S_PRODUCED && a.poseRuntime == 1;
        every = every && tr.open(xr.Get(), static_cast<HANDLE>(a.sharedHandle), a.textureSerial) == S_OK;
        tr.markProduced(200 + k, seq);
        every = every && tr.consume(0) == S_OK && tr.consumedSequence() == seq;
    }
    sc.ok(every, "case 11: Lock's key: four frames in a row each produce an f = 0.5 S (none kept as f = 1), consumed as its own pair's");
    f.everyHalf = false;
    EdvrNativeFrameGenS first{};
    bool built = build(first) == S_OK;
    if (first.state == EDVR_NATIVE_FRAMEGEN_S_PRODUCED && tr.open(xr.Get(), static_cast<HANDLE>(first.sharedHandle), first.textureSerial) == S_OK) {
        tr.markProduced(300, seq);
        tr.consume(0);
    }
    built = built && build(a) == S_OK;
    sc.ok(built && (first.state == EDVR_NATIVE_FRAMEGEN_S_KEPT) != (a.state == EDVR_NATIVE_FRAMEGEN_S_KEPT), "case 11: everyHalf off: the alternation is back (of two builds one f = 1 S is kept)");
    tr.release();
    sc.ok(tr.consumedSequence() == 0, "case 11: released, the private texture holds no pair's S");
    frameGenSynthConfigure(false);
    frameGenSynthFrameBoundary();
}

// ---- case 12: S finished like B at B's door (Phase 2.3b) ----------------------------------------------------------------------------------------------------
// The CPU reference of fgSynFinish (and of ui_layer's composite, which it is line for line): the layer over `frame` at S pixel (x, y), read where H carries it in B.
void finishRef(const std::vector<float>& frame, const std::vector<float>& layer, int lw, int lh, const float uv[4], const float H[9], int x, int y, float out[4]) {
    const float* f = &frame[(static_cast<size_t>(y) * RW + x) * 4];
    float l[4] = {0, 0, 0, 1};
    float q[2];
    if (lw > 0 && frameGenSynthFinishLanding(H, static_cast<float>(x), static_cast<float>(y), q)) {
        const float spanX = (uv[2] - uv[0]) * lw / RW, spanY = (uv[3] - uv[1]) * lh / RH;
        const float x0 = uv[0] * lw + q[0] * spanX, y0 = uv[1] * lh + q[1] * spanY, x1 = x0 + spanX, y1 = y0 + spanY;
        const int k0x = std::max(static_cast<int>(std::floor(x0)), 0), k0y = std::max(static_cast<int>(std::floor(y0)), 0);
        const int k1x = std::min(static_cast<int>(std::ceil(x1)) - 1, lw - 1), k1y = std::min(static_cast<int>(std::ceil(y1)) - 1, lh - 1);
        float acc[4] = {}, wsum = 0;
        for (int j = k0y; j <= std::min(k1y, k0y + 3); ++j) {
            const float wy = std::min(y1, j + 1.0f) - std::max(y0, static_cast<float>(j));
            if (wy <= 0) continue;
            for (int i = k0x; i <= std::min(k1x, k0x + 3); ++i) {
                const float wx = std::min(x1, i + 1.0f) - std::max(x0, static_cast<float>(i));
                if (wx <= 0) continue;
                for (int c = 0; c < 4; ++c) acc[c] += layer[(static_cast<size_t>(j) * lw + i) * 4 + c] * wx * wy;
                wsum += wx * wy;
            }
        }
        if (wsum > 0) for (int c = 0; c < 4; ++c) l[c] = acc[c] / wsum;
    }
    for (int c = 0; c < 3; ++c) out[c] = std::min(1.0f, std::max(0.0f, l[c] + f[c] * l[3]));
    out[3] = f[3];
}

// The UI block's coverage-weighted centroid in an image: where 1 - alpha-like darkening shows it (the block is pure red over a scene with no red above 0.6).
bool redCentroid(const std::vector<float>& img, float out[2]) {
    double sx = 0, sy = 0, sw = 0;
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) {
            const float r = img[(static_cast<size_t>(y) * RW + x) * 4 + 0];
            const float w = std::max(0.0f, r - 0.7f);
            sx += w * x; sy += w * y; sw += w;
        }
    if (sw <= 0) return false;
    out[0] = static_cast<float>(sx / sw);
    out[1] = static_cast<float>(sy / sw);
    return true;
}

int g_dumpK = -1;
ID3D11Texture2D* g_dumpS = nullptr;
ID3D11Texture2D* g_dumpB = nullptr;
void dumpSink(ID3D11DeviceContext*, int k, ID3D11Texture2D* s, ID3D11Texture2D* b) { g_dumpK = k; g_dumpS = s; g_dumpB = b; }
// EDVR's menu on S (native_menu.cpp nativeMenuFrameGenDraw stands here as a sink that marks S): writes zeros over the finished S in place and keeps what it was handed.
int g_menuCalls = 0, g_menuDumpKAtCall = -2;
float g_menuRot[9] = {}, g_menuT[3] = {}, g_menuBox[4] = {};
uint32_t g_menuAnswer = kFrameGenFinishDone;
uint32_t menuSink(ID3D11DeviceContext* ctx, uint32_t, ID3D11Texture2D* s, const float box[4], const float rot[9], const float t[3]) {
    ++g_menuCalls;
    g_menuDumpKAtCall = g_dumpK;
    std::memcpy(g_menuRot, rot, sizeof(g_menuRot)); std::memcpy(g_menuT, t, sizeof(g_menuT)); std::memcpy(g_menuBox, box, sizeof(g_menuBox));
    if (g_menuAnswer != kFrameGenFinishDone) return g_menuAnswer;
    D3D11_TEXTURE2D_DESC d{};
    s->GetDesc(&d);
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = 0; d.CPUAccessFlags = 0; d.Usage = D3D11_USAGE_DEFAULT;
    std::vector<uint8_t> zeros(static_cast<size_t>(d.Width) * d.Height * 16, 0);
    D3D11_SUBRESOURCE_DATA init{zeros.data(), d.Width * 16, 0};
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    ComPtr<ID3D11Texture2D> z;
    if (FAILED(dev->CreateTexture2D(&d, &init, &z))) return kFrameGenFinishFault;
    ctx->CopyResource(s, z.Get());
    return kFrameGenFinishDone;
}

void case12Finish(Gpu& g, Scope& sc) {
    char label[260];
    ComPtr<ID3D11Device> xr;
    ComPtr<ID3D11DeviceContext> xrCtx;
    D3D_FEATURE_LEVEL level{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &xr, &level, &xrCtx))) { sc.ok(false, "case 12: a second WARP device"); return; }
    frameGenSynthConfigure(false);
    frameGenSynthFrameBoundary();
    // B: a scene in [0, 1] (the 8-bit range the composite works in) on a cockpit plane at 1 m; the layer: nothing (0,0,0,1) but a red block, opaque in its middle and 40 %
    // over the scene at its rim, the way a premultiplied HUD is.
    Scene s;
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) {
            float* p = &s.rgba[(static_cast<size_t>(y) * RW + x) * 4];
            p[0] = 0.1f + 0.4f * x / RW; p[1] = 0.2f + 0.6f * y / RH; p[2] = 0.5f; p[3] = 1.0f;
        }
    std::fill(s.depth.begin(), s.depth.end(), depthOf(1.0f));
    ModuleFrame m;
    if (!makeInputs(g, s, m.out, m.depth, m.mv)) { sc.ok(false, "case 12: inputs were made"); return; }
    std::vector<float> layer(static_cast<size_t>(RW) * RH * 4, 0.0f);
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) {
            float* p = &layer[(static_cast<size_t>(y) * RW + x) * 4];
            const bool core = x >= 60 && x < 72 && y >= 40 && y < 52, rim = x >= 58 && x < 74 && y >= 38 && y < 54;
            if (core) { p[0] = 1.0f; p[3] = 0.0f; }
            else if (rim) { p[0] = 0.6f; p[3] = 0.4f; }
            else p[3] = 1.0f;
        }
    Tex lt;
    if (!g.makeTex(RW, RH, DXGI_FORMAT_R32G32B32A32_FLOAT, layer.data(), RW * 16, lt, false)) { sc.ok(false, "case 12: the layer was made"); return; }
    const float uv[4] = {0, 0, 1, 1};
    FrameGenSynthFrame f;
    f.renderW = RW; f.renderH = RH; f.outW = RW; f.outH = RH;
    f.tanNow = kTan;
    f.depthA = 0.0f; f.depthB = kDepthB; f.splitMetres = 10.0f;
    f.depthWritten = true;
    f.outSrv = m.out.srv.Get(); f.depthSrv = m.depth.srv.Get(); f.mvSrv = m.mv.srv.Get();
    for (int i = 0; i < 9; ++i) f.slotRot[i] = (i % 4 == 0) ? 1.0f : 0.0f;
    f.slotPose = true;
    f.consumer = true;
    f.everyHalf = true;
    f.finish = true;
    frameGenSynthConfigure(true);
    edvr::openxr::FrameGenSlotTransfer tr;
    uint64_t seq = 900;
    uint32_t frame = 19000;
    const auto take = [&](EdvrNativeFrameGenS& a) {
        a = EdvrNativeFrameGenS{};
        a.size = EDVR_NATIVE_FRAMEGEN_S_SIZE_1; a.version = EDVR_NATIVE_FRAMEGEN_VERSION_1;
        return edvrFrameGenTakeS(0, &a);
    };
    const auto build = [&] {
        f.slotSequence = ++seq;
        frameGenSynthEyeFrame(g.dev.Get(), g.ctx.Get(), 0, ++frame, f);
    };
    FrameGenSynthFinishInputs in;
    in.why = kFrameGenFinishDone;
    in.layer = lt.srv.Get();
    in.layerW = RW; in.layerH = RH;
    std::memcpy(in.layerUv, uv, sizeof(uv));
    in.dumpSink = dumpSink;
    // Runs one build and its door; returns the runtime's copy of the finished S (empty when nothing crossed) and the unfinished S the door started from.
    const auto finishOnce = [&](std::vector<float>& unfinished, EdvrNativeFrameGenS& a) {
        build();
        std::vector<float> got;
        EdvrNativeFrameGenS before{};
        take(before);
        sc.ok(before.state == EDVR_NATIVE_FRAMEGEN_S_KEPT && before.finish == kFrameGenFinishNoDoor, "case 12: built for the door, S is held back until it (kept, no_door)");
        ID3D11Texture2D* src = frameGenSynthFinishSource(g.dev.Get(), g.ctx.Get(), 0);
        if (!src) return got;
        readRgba32(g.dev.Get(), g.ctx.Get(), src, unfinished);
        frameGenSynthFinish(g.ctx.Get(), 0, in);
        take(a);
        if (a.state == EDVR_NATIVE_FRAMEGEN_S_PRODUCED && tr.open(xr.Get(), static_cast<HANDLE>(a.sharedHandle), a.textureSerial) == S_OK) {
            tr.markProduced(1, seq);
            if (tr.consume(0) == S_OK) readRgba32(xr.Get(), xrCtx.Get(), tr.privateTexture(), got);
        }
        return got;
    };
    float H[9];
    const auto worstAgainstRef = [&](const std::vector<float>& got, const std::vector<float>& frameIn, const float* Hm) {
        float worst = 0.0f;
        for (int y = 0; y < RH; ++y)
            for (int x = 0; x < RW; ++x) {
                float r[4];
                finishRef(frameIn, layer, RW, RH, uv, Hm, x, y, r);
                for (int c = 0; c < 4; ++c) worst = std::max(worst, std::fabs(got[(static_cast<size_t>(y) * RW + x) * 4 + c] - r[c]));
            }
        return worst;
    };
    // 1: no head motion. S is B (to the resample's rounding); finished, it is B's composite: what the runtime takes equals the reference over S exactly and B's within the
    // resample's rounding, the layer read texel for texel (the identity homography).
    std::vector<float> uf;
    EdvrNativeFrameGenS a{};
    g_dumpK = -1;
    f.wantShown = true; f.dumpIndex = 3;
    std::vector<float> got = finishOnce(uf, a);
    f.wantShown = false; f.dumpIndex = -1;
    const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    frameGenSynthFinishHomography(f.slotRot, f.slotT, kTan, RW, RH, kFrameGenFinishPanelMetres, H);
    sc.ok(std::memcmp(H, ident, sizeof(H)) == 0, "case 12: no head step gives exactly the identity homography (S's composite reads B's texels)");
    sc.ok(a.state == EDVR_NATIVE_FRAMEGEN_S_PRODUCED && a.finish == kFrameGenFinishDone && !got.empty(), "case 12: finished at the door, S is handed over (produced, finished)");
    if (!got.empty()) {
        const float wS = worstAgainstRef(got, uf, H), wB = worstAgainstRef(got, s.rgba, H);
        std::snprintf(label, sizeof(label), "case 12: a still head: the runtime's S is the composite over S (worst %.6f) and B's finished frame (worst %.5f, the resample's rounding)", wS, wB);
        sc.ok(wS < 1e-5f && wB < 2e-3f, label);
    }
    sc.ok(g_dumpK == 3 && g_dumpS != nullptr, "case 12: the eye dump's frame stages the finished S at the door");
    // 2: a head turn of 5 degrees and a 2 cm step. The block lands where S's eye sees the plane B's eye saw it on: the centroid against a reference from directions alone
    // (no homography), within 0.5 px; the image against the homography's reference exactly; and it moved (the case is not vacuous).
    float c0[2] = {}, c1[2] = {};
    const bool had0 = redCentroid(got, c0);
    rotY(5.0f, f.slotRot);
    f.slotT[0] = 0.02f; f.slotT[1] = 0.0f; f.slotT[2] = 0.0f;
    got = finishOnce(uf, a);
    frameGenSynthFinishHomography(f.slotRot, f.slotT, kTan, RW, RH, kFrameGenFinishPanelMetres, H);
    bool had1 = !got.empty() && redCentroid(got, c1);
    if (had0 && had1) {
        // The reference: B's centroid direction on the panel plane at D in B's eye, carried into S's eye by x_S = R x_B + t, projected.
        float dB[3], dS[3], want[2];
        temporalPixelToDir(c0[0], c0[1], kTan, RW, RH, dB);
        const float D = kFrameGenFinishPanelMetres;
        float pB[3] = {dB[0] * D, dB[1] * D, dB[2] * D}, pS[3];
        temporalApply3(f.slotRot, pB, pS);
        for (int i = 0; i < 3; ++i) dS[i] = pS[i] + f.slotT[i];
        const bool front = temporalDirToPixel(dS, kTan, RW, RH, &want[0], &want[1]);
        const float err = std::hypot(c1[0] - want[0], c1[1] - want[1]), moved = std::hypot(c1[0] - c0[0], c1[1] - c0[1]);
        std::snprintf(label, sizeof(label), "case 12: a head turn and step: the UI lands where S's eye sees it (%.3f px off a direction reference; moved %.2f px)", err, moved);
        sc.ok(front && err <= 0.5f && moved > 2.0f, label);
        const float wS = worstAgainstRef(got, uf, H);
        std::snprintf(label, sizeof(label), "case 12: ...and the image is the homography's composite over S (worst %.6f)", wS);
        sc.ok(wS < 1e-5f, label);
        if (g_verbose) std::printf("  case 12: centroid B (%.3f, %.3f) -> S (%.3f, %.3f), want (%.3f, %.3f)\n", c0[0], c0[1], c1[0], c1[1], want[0], want[1]);
    } else {
        sc.ok(false, "case 12: the block shows in both finished S");
    }
    // 3: the door could not finish S (here: the scope lens): nothing crosses, the runtime is told why, and the key is back with the game device (the next S is not busy).
    in.why = kFrameGenFinishScope;
    got = finishOnce(uf, a);
    sc.ok(got.empty() && a.state == EDVR_NATIVE_FRAMEGEN_S_KEPT && a.finish == kFrameGenFinishScope, "case 12: an S the door cannot finish is not handed over, and says why (scope)");
    in.why = kFrameGenFinishDone;
    got = finishOnce(uf, a);
    sc.ok(a.state == EDVR_NATIVE_FRAMEGEN_S_PRODUCED && !got.empty(), "case 12: ...the producer texture's key came back: the next S is finished and handed over, not busy");
    // 4: no door before the next build: that S was never handed over, and the build after it is not held back by it.
    const uint32_t unf0 = frameGenSynthOpenWindow().unfinished;
    build();
    build();
    EdvrNativeFrameGenS last{};
    take(last);
    sc.ok(last.state == EDVR_NATIVE_FRAMEGEN_S_KEPT && last.finish == kFrameGenFinishNoDoor && frameGenSynthOpenWindow().unfinished > unf0,
          "case 12: an S whose door never came is counted unfinished, and the next build goes ahead (kept for its own door, not busy)");
    sc.ok(frameGenSynthFinishSource(g.dev.Get(), g.ctx.Get(), 0) != nullptr, "case 12: ...and waits for its door");
    frameGenSynthFinish(g.ctx.Get(), 0, in);
    take(last);
    if (last.state == EDVR_NATIVE_FRAMEGEN_S_PRODUCED && tr.open(xr.Get(), static_cast<HANDLE>(last.sharedHandle), last.textureSerial) == S_OK) {
        tr.markProduced(2, seq);
        tr.consume(0);
    }
    // 4b: EDVR's menu on S. The sink runs on the finished S before it is handed over and before the dump stages it (what crosses is what it wrote), with the head's step S was
    // built with and the door's bounds; a size 2 answer says the menu is on S. A sink that cannot draw leaves S unfinished (menu), not handed over. Without a sink: menu 0.
    {
        const auto take2 = [&](EdvrNativeFrameGenS& r) {
            r = EdvrNativeFrameGenS{};
            r.size = EDVR_NATIVE_FRAMEGEN_S_SIZE_2; r.version = EDVR_NATIVE_FRAMEGEN_VERSION_1;
            return edvrFrameGenTakeS(0, &r);
        };
        const auto consumeIt = [&](const EdvrNativeFrameGenS& r, int64_t slot) {
            if (r.state == EDVR_NATIVE_FRAMEGEN_S_PRODUCED && tr.open(xr.Get(), static_cast<HANDLE>(r.sharedHandle), r.textureSerial) == S_OK) { tr.markProduced(slot, seq); tr.consume(0); }
        };
        in.menuSink = menuSink;
        in.menuBox[0] = 1.0f; in.menuBox[2] = 0.0f;   // a B flipped across
        rotY(5.0f, f.slotRot); f.slotT[0] = 0.02f;
        g_menuCalls = 0; g_dumpK = -1;
        f.wantShown = true; f.dumpIndex = 2;
        got = finishOnce(uf, a);
        f.wantShown = false; f.dumpIndex = -1;
        bool zeros = !got.empty();
        for (float v : got) zeros = zeros && v == 0.0f;
        sc.ok(g_menuCalls == 1 && g_menuDumpKAtCall == -1 && g_dumpK == 2 && zeros && a.state == EDVR_NATIVE_FRAMEGEN_S_PRODUCED && a.finish == kFrameGenFinishDone,
              "case 12: the menu sink runs on the finished S before the dump stages it and before it is handed over: what crosses is what it drew");
        sc.ok(std::memcmp(g_menuRot, f.slotRot, sizeof(g_menuRot)) == 0 && std::memcmp(g_menuT, f.slotT, sizeof(g_menuT)) == 0 && g_menuBox[0] == 1.0f && g_menuBox[2] == 0.0f,
              "case 12: ...handed the head's step S was built with and B's bounds");
        sc.ok(a.size == EDVR_NATIVE_FRAMEGEN_S_SIZE_1, "case 12: ...a size 1 reader (a runtime from before) is written size 1's bytes only");
        EdvrNativeFrameGenS a2{};
        build();
        take2(a2);
        frameGenSynthFinish(g.ctx.Get(), 0, in);
        take2(a2);
        sc.ok(a2.size == EDVR_NATIVE_FRAMEGEN_S_SIZE_2 && a2.menu == 1u && a2.state == EDVR_NATIVE_FRAMEGEN_S_PRODUCED, "case 12: ...a size 2 reader is told the menu is on S (menu 1)");
        consumeIt(a2, 3);
        g_menuAnswer = kFrameGenFinishMenu;
        build();
        frameGenSynthFinish(g.ctx.Get(), 0, in);
        take2(a2);
        sc.ok(a2.state == EDVR_NATIVE_FRAMEGEN_S_KEPT && a2.finish == kFrameGenFinishMenu && a2.menu == 0u, "case 12: a menu that cannot be drawn on S leaves it unfinished (menu), not handed over");
        g_menuAnswer = kFrameGenFinishDone;
        in.menuSink = nullptr;
        in.menuBox[0] = 0.0f; in.menuBox[2] = 1.0f;
        build();
        frameGenSynthFinish(g.ctx.Get(), 0, in);
        take2(a2);
        sc.ok(a2.state == EDVR_NATIVE_FRAMEGEN_S_PRODUCED && a2.finish == kFrameGenFinishDone && a2.menu == 0u, "case 12: ...with no sink S is finished and handed over, menu 0");
        consumeIt(a2, 4);
    }
    // 5: the door sharpens: S is built in the producer texture and finished over the sharpened frame the door hands (a flat grey frame stands for it here).
    f.finishSharpen = true;
    std::fill(f.slotRot, f.slotRot + 9, 0.0f); f.slotRot[0] = f.slotRot[4] = f.slotRot[8] = 1.0f; f.slotT[0] = 0.0f;
    std::vector<float> greyFrame(static_cast<size_t>(RW) * RH * 4, 0.25f);
    Tex gt;
    g.makeTex(RW, RH, DXGI_FORMAT_R32G32B32A32_FLOAT, greyFrame.data(), RW * 16, gt, false);
    in.frame = gt.t.Get();
    got = finishOnce(uf, a);
    frameGenSynthFinishHomography(f.slotRot, f.slotT, kTan, RW, RH, kFrameGenFinishPanelMetres, H);
    sc.ok(a.state == EDVR_NATIVE_FRAMEGEN_S_PRODUCED && !got.empty() && worstAgainstRef(got, greyFrame, H) < 1e-5f,
          "case 12: B's door sharpens: S is finished over the sharpened frame it hands (the producer texture written from it)");
    in.frame = nullptr;
    got = finishOnce(uf, a);
    sc.ok(got.empty() && a.finish == kFrameGenFinishSharpen, "case 12: ...a door that then does not sharpen leaves that S unfinished (sharpen), not finished over itself");
    const FrameGenSynthWindow w = frameGenSynthOpenWindow();
    sc.ok(w.finished >= 4 && w.unfinished >= 3 && w.unfinishedWhy == kFrameGenFinishSharpen, "case 12: the window counts finished and unfinished S and the last reason");
    tr.release();
    frameGenSynthConfigure(false);
    frameGenSynthFrameBoundary();
    sc.ok(!frameGenSynthResident(), "case 12: the key off releases the finish's shader and textures with the rest");
}

// ---- the run ---------------------------------------------------------------------------------------------------------------------------------------------

void swapIn(Gpu& g, ComPtr<ID3D11ComputeShader> saved[kEntries], const ComPtr<ID3D11ComputeShader> mutated[kEntries]) {
    for (int i = 0; i < kEntries; ++i) { saved[i] = g.cs[i]; g.cs[i] = mutated[i]; }
}

void swapOut(Gpu& g, const ComPtr<ID3D11ComputeShader> saved[kEntries]) {
    for (int i = 0; i < kEntries; ++i) g.cs[i] = saved[i];
}

// Case 16 (FG_ORPHAN, 2026-11-08): a pixel with a key and no read offer. A still world plane at 50 m (every pixel lands on itself, so S is B). The production build makes no
// orphan there; the build with fgSynSrc's offer withheld from (kOrphanX, kOrphanY) leaves that pixel keyed with the splat's all-ones, which the resolve must make a hole: counted
// as an orphan, filled from its neighbours (B there is their surface, so the fill reads the pixel itself), and not read 2047 px off as the all-ones unpacks. `define`: the
// switches the forced build turns off ("" none; the mutation's "FG_ORPHAN").
struct Orphan { bool ran = false; unsigned orphan = 0, filled = 0, left = 0, guard = 0; float err = 0.0f; };

Orphan case16Orphan(Gpu& g, Scope& sc, bool report, const char* define = "") {
    Orphan e;
    char label[300];
    Scene S;
    S.gradient();
    std::fill(S.depth.begin(), S.depth.end(), depthOf(50.0f));
    FrameGenSynthCb cb;
    frameGenSynthFillCb(cb, RW, RH, RW, RH, kTan, 0.0f, kDepthB, 10.0f, 0.5f, Deltas(0.0f, 0.0f, 0.0f, 0.0f).d);
    const auto errAt = [&](const Run& r) { return std::hypot(r.at(kOrphanX, kOrphanY, 0, RW) - kOrphanX, r.at(kOrphanX, kOrphanY, 1, RW) - kOrphanY); };
    {
        const Run r = runSynth(g, S, cb);
        sc.ok(r.ok, "case 16: the synthesizer ran (production)");
        if (!r.ok) return e;
        std::snprintf(label, sizeof(label), "case 16: the production build leaves no orphan on a still plane (orphan %u, holes %u, S at the pixel %.3f px off)",
                      r.stats[kFrameGenSynthStatsOrphan], r.stats[kFrameGenSynthStatsFilled] + r.stats[kFrameGenSynthStatsLeft], errAt(r));
        sc.ok(r.stats[kFrameGenSynthStatsOrphan] == 0 && r.stats[kFrameGenSynthStatsFilled] + r.stats[kFrameGenSynthStatsLeft] == 0 && errAt(r) < 1e-3f, label);
    }
    ComPtr<ID3D11ComputeShader> forced[kEntries], saved[kEntries];
    const bool built = compileMutated(g, define, forced, true);
    sc.ok(built, "case 16: the build with fgSynSrc's offer withheld from one pixel compiled (is the offer's InterlockedMin still there, once?)");
    if (!built) return e;
    swapIn(g, saved, forced);
    const Run r = runSynth(g, S, cb);
    swapOut(g, saved);
    sc.ok(r.ok, "case 16: the synthesizer ran (the offer withheld)");
    if (!r.ok) return e;
    e.ran = true;
    e.orphan = r.stats[kFrameGenSynthStatsOrphan];
    e.filled = r.stats[kFrameGenSynthStatsFilled];
    e.left = r.stats[kFrameGenSynthStatsLeft];
    e.guard = r.stats[kFrameGenSynthStatsGuard];
    e.err = errAt(r);
    std::snprintf(label, sizeof(label), "case 16: a pixel with a key and no read offer is counted as an orphan and made a filled hole (orphan %u, filled %u, left %u, guarded %u)",
                  e.orphan, e.filled, e.left, e.guard);
    sc.ok(e.orphan == 1 && e.filled == 1 && e.left == 0 && e.guard == 0, label);
    std::snprintf(label, sizeof(label), "case 16: the orphan's S reads B at its own surface, not 2047 px off (%.3f px from the pixel)", e.err);
    sc.ok(e.err < 0.05f, label);
    if (g_verbose && report) std::printf("  case 16: orphan %u, filled %u, left %u, guarded %u, read %.4f px off\n", e.orphan, e.filled, e.left, e.guard, e.err);
    return e;
}

// Case 17 (2026-11-08, eye dump 120812713001, the own hull's outline torn against space in a ship turn): the hull (right of x = 64, 60 m, still in the eye: vectors 0) over
// the far plane that the ship's turn moves away from it (vectors +8 px: at f = 0.5 S reads the sky 4 px to the right). Red is the render x read: the sky a gradient, the
// outline the game draws without depth just outside the hull (x 62 and 63, the far plane, the sky's vectors: B's output carries it 2 render px out) -100, the hull 1000,
// and a patch on the hull (x 70 to 73, 58 m) whose history was rejected (the sentinel in its vectors, within 32 px of the sky) 500 + x. Expected: the outline stays on the
// hull (S at 62 and 63 reads -100) and nowhere in the sky (no S pixel left of 62 reads under -50: neither carried with the sky nor read back by the hole fill), the sky
// moves as the sky (S reads x + 4 up to x 53), and the patch moves with the hull, its own surface, not with the sky beyond the silhouette (S at 70 to 73 reads 500 + x).
struct HullHalo { bool ran = false; float outline = 0.0f, skyMin = 0.0f, skyWorst = 0.0f, patchWorst = 0.0f; };

HullHalo case17HullHalo(Gpu& g, Scope& sc, bool report) {
    HullHalo e;
    char label[300];
    Scene S;
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) {
            const size_t i = static_cast<size_t>(y) * RW + x;
            const bool hull = x >= 64, outline = x == 62 || x == 63, patch = x >= 70 && x <= 73;
            float* p = &S.rgba[i * 4];
            p[0] = patch ? 500.0f + static_cast<float>(x) : (hull ? 1000.0f : (outline ? -100.0f : static_cast<float>(x)));
            p[1] = static_cast<float>(y); p[2] = 0.5f; p[3] = 1.0f;
            S.depth[i] = patch ? depthOf(58.0f) : (hull ? depthOf(60.0f) : 0.0f);
            S.mv[i * 2] = patch ? 2.0f * RW : (hull ? 0.0f : 8.0f);
            S.mv[i * 2 + 1] = patch ? 2.0f * RH : 0.0f;
        }
    FrameGenSynthCb cb;
    frameGenSynthFillCb(cb, RW, RH, RW, RH, kTan, 0.0f, kDepthB, 10.0f, 0.5f, Deltas(0.0f, 0.0f, 0.0f, 0.0f).d);
    const Run r = runSynth(g, S, cb);
    sc.ok(r.ok, "case 17: the synthesizer ran");
    if (!r.ok) return e;
    e.ran = true;
    e.outline = -1e9f;
    e.skyMin = 1e9f;
    for (int y = 4; y < RH - 4; ++y) {
        e.outline = std::max(e.outline, std::max(r.at(62, y, 0, RW), r.at(63, y, 0, RW)));
        for (int x = 8; x <= 61; ++x) e.skyMin = std::min(e.skyMin, r.at(x, y, 0, RW));
        for (int x = 8; x <= 53; ++x) e.skyWorst = std::max(e.skyWorst, std::fabs(r.at(x, y, 0, RW) - (x + 4.0f)));
        for (int x = 70; x <= 73; ++x) e.patchWorst = std::max(e.patchWorst, std::fabs(r.at(x, y, 0, RW) - (500.0f + x)));
    }
    std::snprintf(label, sizeof(label), "case 17: the outline drawn without depth 2 px outside the hull stays on the hull (S at x 62 and 63 read at most %.2f, the outline -100)", e.outline);
    sc.ok(e.outline < -99.0f, label);
    std::snprintf(label, sizeof(label), "case 17: no outline in the sky, carried with it or read back by the fill (the least S left of x 62 reads %.2f; the outline -100)", e.skyMin);
    sc.ok(e.skyMin > -50.0f, label);
    std::snprintf(label, sizeof(label), "case 17: the sky moves as the sky (worst %.4f px)", e.skyWorst);
    sc.ok(e.skyWorst < 0.1f, label);
    std::snprintf(label, sizeof(label), "case 17: the hull's patch with a rejected history moves with the hull, not the sky past the silhouette (worst %.4f px)", e.patchWorst);
    sc.ok(e.patchWorst < 0.1f, label);
    if (g_verbose && report) std::printf("  case 17: outline column %.2f, least sky read %.2f, sky worst %.4f px, patch worst %.4f px\n", e.outline, e.skyMin, e.skyWorst, e.patchWorst);
    return e;
}

int run() {
    Gpu g;
    if (!g.init()) {
        std::printf("FAIL: WARP could not start or the production shaders did not create\n");
        return 1;
    }
    Scope sc;
    const auto alive = [&](const char* after) {
        const HRESULT hr = g.dev->GetDeviceRemovedReason();
        if (hr != S_OK) { ++g_failures; std::printf("FAIL: the device was removed (0x%08X) by %s\n", static_cast<unsigned>(hr), after); }
        return hr == S_OK;
    };
    case1Cockpit(g, sc, 1);
    case1Cockpit(g, sc, 2);
    case2World(g, sc);
    case3Far(g, sc);
    case4Split(g, sc);
    const Edge edge = case5Edge(g, sc, true);
    case13HullEdge(g, sc, true);
    case14Sharp(g, sc, true);
    case15Uncover(g, sc, true);
    case16Orphan(g, sc, true);
    case17HullHalo(g, sc, true);
    if (!alive("cases 1 to 5 and 13 to 17")) return 1;
    case6Module(g, sc);
    if (!alive("case 6")) return 1;
    case7Check(g, sc);
    case8Offsets(sc);
    case9Replay(g, sc);
    if (!alive("cases 7 to 9")) return 1;
    case10Slot(g, sc, 0);
    case11Transfer(g, sc);
    case12Finish(g, sc);
    if (!alive("cases 10 to 12")) return 1;
    (void)edge;
    // The host's mutations of case 10 (the block, not the shader): the slot's pose ignored (the extrapolation built instead), and the step taken the wrong way round (S to B).
    unsigned poseIgnored = 0, poseInverted = 0;
    {
        Scope ms;
        ms.quiet = true;
        case10Slot(g, ms, 1);
        poseIgnored = ms.fails;
        ++g_checks;
        if (!ms.fails) { ++g_failures; std::printf("FAIL: mutation: the slot's pose ignored still passes case 10\n"); }
    }
    {
        Scope ms;
        ms.quiet = true;
        case10Slot(g, ms, 2);
        poseInverted = ms.fails;
        ++g_checks;
        if (!ms.fails) { ++g_failures; std::printf("FAIL: mutation: the slot's step the wrong way round still passes case 10\n"); }
    }

    // The mutations: each must break its case.
    struct Mutation { const char* define; const char* what; std::function<void(Scope&)> run; unsigned fails = 0; };
    Mutation muts[14] = {
        {"FG_USE_F", "f ignored (the landing uses f = 1 whatever the block says)", [&](Scope& s) { case2World(g, s); }},
        {"FG_REMOVE_HEAD", "the head turn not taken out of the vector", [&](Scope& s) { case2World(g, s); }},
        // Since the edge pixels move by the vector of the pixel whose depth they took (FG_DILATE_MV), the guard has nothing to move in case 5: it is checked on the
        // edge it was built for, the sky pixel moving by its own vector.
        {"FG_GUARD,FG_DILATE_MV", "the guard off (an edge pixel on its own vector)", [&](Scope& s) { case5Edge(g, s, false); }},
        // Since the fill reads B where the chosen neighbour's motion carries the hole and tests the depth there, case 5's sky is found from the nearer side too; case 15's
        // bar is still, so a fill from its side reads the plane where the hole is, not where the plane came from.
        {"FG_FILL_FAR", "the fill from the nearer side", [&](Scope& s) { case5Edge(g, s, false); case15Uncover(g, s, false); }},
        {"FG_TURN", "the score's turn not applied (the residual ignored)", [&](Scope& s) { case7Check(g, s); }},
        {"FG_TURN_SIGN", "the score's turn applied the wrong way round (the residual transposed)", [&](Scope& s) { case7Check(g, s); }},
        {"FG_DILATE_MV", "an edge pixel moved by its own vector, not the one whose depth it took", [&](Scope& s) { case13HullEdge(g, s, false); }},
        {"FG_SENTINEL", "the rejection sentinel taken as a vector", [&](Scope& s) { case13HullEdge(g, s, false); }},
        {"FG_SHARP", "S resampled bilinear", [&](Scope& s) { case14Sharp(g, s, false); }},
        {"FG_SHARP=3", "S's cubic resample without its ringing clamp", [&](Scope& s) { case14Sharp(g, s, false); }},
        {"FG_FILL_REPROJ", "the hole fill by the background pull alone (no reprojection, no stretch)", [&](Scope& s) { case15Uncover(g, s, false); }},
        {"FG_ORPHAN", "an orphan (a key and no read offer) read as its all-ones, not made a hole", [&](Scope& s) { case16Orphan(g, s, false, "FG_ORPHAN"); }},
        {"FG_HALO", "the far plane 2 px from a hull on the sky's motion and read by the fill as sky", [&](Scope& s) { case17HullHalo(g, s, false); }},
        {"FG_SENTINEL_SURFACE", "a rejected vector borrowed from the farthest ring pixel, past the silhouette", [&](Scope& s) { case17HullHalo(g, s, false); }},
    };
    for (Mutation& m : muts) {
        ComPtr<ID3D11ComputeShader> mutated[kEntries], saved[kEntries];
        ++g_checks;
        if (!compileMutated(g, m.define, mutated)) {
            ++g_failures;
            std::printf("FAIL: mutation: the shader with %s could not be built (is `#define %s 1` still in the text?)\n", m.what, m.define);
            continue;
        }
        Scope ms;
        ms.quiet = true;
        swapIn(g, saved, mutated);
        m.run(ms);
        swapOut(g, saved);
        m.fails = ms.fails;
        if (ms.fails == 0) { ++g_failures; std::printf("FAIL: mutation: %s still passes\n", m.what); }
    }
    // Case 3 reads the head's part the same way as case 2: the head-turn mutation must break it too (a second look, not a second count).
    std::printf("%s: %u checks, %u failures (mutations: f ignored broke %u checks, head turn not removed broke %u, guard off broke %u, fill from the nearer side broke %u, turn not applied broke %u, turn the wrong way round broke %u, own vector at an edge broke %u, sentinel as a vector broke %u, bilinear resample broke %u, unclamped cubic broke %u, background pull alone broke %u, orphan not a hole broke %u, halo off broke %u, sentinel from any surface broke %u, slot pose ignored broke %u, slot step inverted broke %u)\n",
                g_failures ? "FAIL" : "PASS", g_checks, g_failures, muts[0].fails, muts[1].fails, muts[2].fails, muts[3].fails, muts[4].fails, muts[5].fails, muts[6].fails, muts[7].fails,
                muts[8].fails, muts[9].fails, muts[10].fails, muts[11].fails, muts[12].fails, muts[13].fails, poseIgnored, poseInverted);
    return g_failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false, dry = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--self-test")) selfTest = true;
        else if (!std::strcmp(argv[i], "--dry-run")) dry = true;
        else if (!std::strcmp(argv[i], "--verbose")) g_verbose = true;
        else { std::printf("usage: framegen_synth_test [--dry-run | --self-test] [--verbose]\n"); return 2; }
    }
    if (!selfTest && !dry) { std::printf("usage: framegen_synth_test [--dry-run | --self-test] [--verbose]\n"); return 2; }
    return run();
}

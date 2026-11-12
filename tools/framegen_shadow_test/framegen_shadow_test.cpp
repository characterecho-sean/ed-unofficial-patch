// framegen_shadow_test: the frame-generation shadow scorer (fix.frame_generation_shadow, TEMPORARY; docs/design-frame-generation-2026-10-26.md, Phase 1) on WARP.
//
//   --dry-run    the same run (the rig never writes a file), for the gate's --dry-run convention
//   --self-test  every check, then the mutations (each must make its case fail)
//   --verbose    the numbers of each case
//
// The production bytecode (kFrameGenShadow*Bytecode, from tools\temporal_shader_build) is run on a WARP device and its key grid, quarter-size luma and depth
// and stats words are read back and held to a CPU reference built from src\common\framegen_shadow_math.h and temporal_math.h.
//
// Cases:
//   0  capture: the quarter-size luma (a 4x4 box mean at 1:1, the same area at 2:1 output) and the nearest-wins depth of each cell
//   1  a static scene, identity deltas: every cell is scored, no hole, every error is in bin 0
//   2  a pure head rotation of 3 degrees on the far plane: the forward carry is the inverse of the backward one to a hundredth of a pixel and lands where the
//      plan's closed form says (under 0.1 px), the keys are the far key, the score is near 0 while the no-warp comparison is not
//   3  a cockpit plane at 1 m under an 8 cm head translation: the cockpit class, the right shift, and a disocclusion strip of the expected width (one cell column)
//   4  a world plane at 50 m, ship motion constant over the two frames: the world class, the rig miss 0 and the score near 0; then with an acceleration the
//      rig miss is the figure worked out by hand and the score is worse
//   5  reversed-Z far (the written depth at or under A) and the on-foot 1 mm split: 0.5 mm is the cockpit, 2 mm the world; the ship's 10 m split puts both in the cockpit
//   6  the module itself (framegen_shadow.cpp) on WARP: the key off creates nothing; on, a static scene scores with every error in bin 0 (the mv set too), each skip
//      reason counts, and a camera_jump frame goes to the jump set
//   7  the second score kernel (fgShadowScoreB): a world plane at constant ship motion with the DLSS vectors filled in exactly (the mv set scores near 0), then every
//      vector offset by +2 px (the mv set is worse), then a cockpit plane under a head translation (the cockpit's mv set). Mutations: vectors of the wrong sign (the
//      same as a kernel that reads the sign wrong) and the shader itself recompiled with the sign flipped must each fail the exact case
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <d3dcompiler.h>
#include "framegen_shadow_shader.h"

#include "../../src/common/framegen_shadow_math.h"
#include "../../src/common/framegen_shadow_report.h"
#include "framegen_shadow.h"
#include "temporal_shader_bytecode.h"

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

constexpr int RW = 128, RH = 96;   // the render size; the quarter grid is 32 x 24
constexpr int QW = RW / 4, QH = RH / 4;
constexpr float kPi = 3.14159265358979f;

float halfToFloat(uint16_t h) {
    const uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
    float v;
    if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
    else if (e == 31) v = INFINITY;
    else v = std::ldexp(static_cast<float>(m | 1024u), static_cast<int>(e) - 25);
    return s ? -v : v;
}

uint32_t asUint(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
float asFloat(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

// ---- scenes ----------------------------------------------------------------------------------------------------------------------------------------------

struct Scene {
    int w = RW, h = RH, ow = RW, oh = RH;
    std::vector<float> out;     // grey, output size
    std::vector<float> depth;   // the written depth, render size
    Scene(int w_ = RW, int h_ = RH, int ow_ = RW, int oh_ = RH) : w(w_), h(h_), ow(ow_), oh(oh_), out(static_cast<size_t>(ow_) * oh_, 0.0f), depth(static_cast<size_t>(w_) * h_, 0.0f) {}
};

const float kTan[4] = {-1.0f, 1.0f, -0.75f, 0.75f};   // left, right, bottom, top

float contentF(float tx, float ty) {
    return 0.5f + 0.25f * std::sin(2.0f * kPi * tx + 0.7f) + 0.15f * std::sin(2.0f * kPi * 0.8f * ty + 0.3f);
}

void pixelDir(int px, int py, float d[3]) { temporalPixelToDir(static_cast<float>(px), static_cast<float>(py), kTan, RW, RH, d); }

void rotY(float deg, float m[9]) {
    const float r = deg * kPi / 180.0f, c = std::cos(r), s = std::sin(r);
    const float v[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
    std::memcpy(m, v, sizeof(v));
}

const float kDepthB = 0.025f;   // Elite's infinite-far reversed-Z: written = B / metres, A = 0

// ---- the GPU harness -------------------------------------------------------------------------------------------------------------------------------------

struct Tex {
    ComPtr<ID3D11Texture2D> t;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};

struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11ComputeShader> cs[5];
    ComPtr<ID3D11Buffer> cb;

    bool init() {
        D3D_FEATURE_LEVEL level{};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &level, &ctx))) return false;
        const struct { const unsigned char* code; size_t size; } codes[5] = {
            {kFrameGenShadowCaptureBytecode, sizeof(kFrameGenShadowCaptureBytecode)}, {kFrameGenShadowClearBytecode, sizeof(kFrameGenShadowClearBytecode)},
            {kFrameGenShadowSplatBytecode, sizeof(kFrameGenShadowSplatBytecode)}, {kFrameGenShadowScoreBytecode, sizeof(kFrameGenShadowScoreBytecode)},
            {kFrameGenShadowScoreBBytecode, sizeof(kFrameGenShadowScoreBBytecode)}};
        for (int i = 0; i < 5; ++i)
            if (FAILED(dev->CreateComputeShader(codes[i].code, codes[i].size, nullptr, &cs[i]))) return false;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(FrameGenShadowCb);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &cb));
    }

    bool makeTex(int w, int h, DXGI_FORMAT fmt, UINT bind, const void* data, UINT pitch, Tex& out, bool wantSrv, bool wantUav) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA sd{data, pitch, 0};
        if (FAILED(dev->CreateTexture2D(&td, data ? &sd : nullptr, &out.t))) return false;
        if (wantSrv && FAILED(dev->CreateShaderResourceView(out.t.Get(), nullptr, &out.srv))) return false;
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

    void setCb(const FrameGenShadowCb& c) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, &c, sizeof(c));
            ctx->Unmap(cb.Get(), 0);
        }
        ID3D11Buffer* b = cb.Get();
        ctx->CSSetConstantBuffers(0, 1, &b);
    }

    void run(int entry, ID3D11ShaderResourceView* const srv[7], ID3D11UnorderedAccessView* const uav[4]) {
        ctx->CSSetShader(cs[entry].Get(), nullptr, 0);
        // Nothing stays bound from the last dispatch: a resource still bound as a UAV would silently null its SRV here (A's capture is a UAV, then the splat reads it).
        ID3D11ShaderResourceView* const noSrv[7] = {};
        ID3D11UnorderedAccessView* const noUav[4] = {};
        ctx->CSSetShaderResources(0, 7, noSrv);
        ctx->CSSetUnorderedAccessViews(0, 4, noUav, nullptr);
        ctx->CSSetShaderResources(0, 7, srv);
        ctx->CSSetUnorderedAccessViews(0, 4, uav, nullptr);
        ctx->Dispatch((QW + 7) / 8, (QH + 7) / 8, 1);
    }
};

uint16_t floatToHalf(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    const uint32_t sign = (u >> 16) & 0x8000u;
    int e = static_cast<int>((u >> 23) & 0xFF) - 127 + 15;
    uint32_t m = u & 0x7FFFFFu;
    if (e <= 0) return static_cast<uint16_t>(sign);   // the rig's vectors are never denormal-small and a flushed one reads 0
    if (e >= 31) return static_cast<uint16_t>(sign | 0x7BFFu);
    uint32_t h = (static_cast<uint32_t>(e) << 10) | (m >> 13);
    if (m & 0x1000u) ++h;   // round to nearest
    return static_cast<uint16_t>(sign | h);
}

struct GpuResult {
    std::vector<float> lumaA, lumaB, depthA, depthB;
    std::vector<uint32_t> key;
    std::vector<uint32_t> stats;
    bool ok = false;
};

// One frame's inputs to the capture: the output as an RGBA32F texture and the depth as R32F.
bool makeInputs(Gpu& g, const Scene& s, Tex& out, Tex& depth) {
    std::vector<float> rgba(static_cast<size_t>(s.ow) * s.oh * 4);
    for (size_t i = 0; i < s.out.size(); ++i) { rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = s.out[i]; rgba[i * 4 + 3] = 1.0f; }
    return g.makeTex(s.ow, s.oh, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, rgba.data(), static_cast<UINT>(s.ow * 16), out, true, false) &&
           g.makeTex(s.w, s.h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, s.depth.data(), static_cast<UINT>(s.w * 4), depth, true, false);
}

// Frame A is captured, then frame B is captured and scored against it with `cb`, in the module's order of dispatches.
// `mv`, when given, is the DLSS vector field (2 floats a render pixel, pixels, B -> A) and the second kernel runs after the score.
GpuResult runGpu(Gpu& g, const Scene& A, const Scene& B, const FrameGenShadowCb& cb, const std::vector<float>* mv = nullptr) {
    GpuResult r;
    Tex outA, depA, outB, depB;
    if (!makeInputs(g, A, outA, depA) || !makeInputs(g, B, outB, depB)) return r;
    Tex luma[2], dq[2], key;
    ComPtr<ID3D11Buffer> stats, staging;
    ComPtr<ID3D11UnorderedAccessView> statsUav;
    for (int i = 0; i < 2; ++i)
        if (!g.makeTex(QW, QH, DXGI_FORMAT_R16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0, luma[i], true, true) ||
            !g.makeTex(QW, QH, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0, dq[i], true, true))
            return r;
    if (!g.makeTex(QW, QH, DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0, key, false, true)) return r;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = kFrameGenShadowStatsUints * 4; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 4;
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &stats))) return r;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = kFrameGenShadowStatsUints;
    if (FAILED(g.dev->CreateUnorderedAccessView(stats.Get(), &ud, &statsUav))) return r;
    bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g.dev->CreateBuffer(&bd, nullptr, &staging))) return r;

    const auto halfField = [&](const std::vector<float>& v, Tex& t) {
        std::vector<uint16_t> half(v.size());
        for (size_t i = 0; i < v.size(); ++i) half[i] = floatToHalf(v[i]);
        return g.makeTex(RW, RH, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, half.data(), RW * 4, t, true, false);
    };
    Tex mvTex;
    if (mv && !halfField(*mv, mvTex)) return r;
    g.setCb(cb);
    ID3D11ShaderResourceView* none[7] = {};
    for (int f = 0; f < 2; ++f) {
        ID3D11ShaderResourceView* srv[7] = {f == 0 ? outA.srv.Get() : outB.srv.Get(), f == 0 ? depA.srv.Get() : depB.srv.Get()};
        ID3D11UnorderedAccessView* uav[4] = {luma[f].uav.Get(), dq[f].uav.Get()};
        g.run(0, srv, uav);
    }
    {
        ID3D11UnorderedAccessView* uav[4] = {nullptr, nullptr, key.uav.Get(), statsUav.Get()};
        g.run(1, none, uav);
    }
    {
        ID3D11ShaderResourceView* srv[7] = {nullptr, nullptr, nullptr, dq[0].srv.Get()};
        ID3D11UnorderedAccessView* uav[4] = {nullptr, nullptr, key.uav.Get()};
        g.run(2, srv, uav);
    }
    {
        ID3D11ShaderResourceView* srv[7] = {nullptr, nullptr, luma[0].srv.Get(), nullptr, luma[1].srv.Get()};
        ID3D11UnorderedAccessView* uav[4] = {nullptr, nullptr, key.uav.Get(), statsUav.Get()};
        g.run(3, srv, uav);
    }
    if (mv) {
        ID3D11ShaderResourceView* srv[7] = {nullptr, nullptr, luma[0].srv.Get(), nullptr, luma[1].srv.Get(), mvTex.srv.Get(), dq[1].srv.Get()};
        ID3D11UnorderedAccessView* uav[4] = {nullptr, nullptr, nullptr, statsUav.Get()};
        g.run(4, srv, uav);
    }
    ID3D11UnorderedAccessView* noUav[4] = {};
    g.ctx->CSSetShaderResources(0, 7, none);
    g.ctx->CSSetUnorderedAccessViews(0, 4, noUav, nullptr);
    g.ctx->CopyResource(staging.Get(), stats.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return r;
    r.stats.assign(static_cast<const uint32_t*>(m.pData), static_cast<const uint32_t*>(m.pData) + kFrameGenShadowStatsUints);
    g.ctx->Unmap(staging.Get(), 0);
    const auto h0 = g.readTex<uint16_t>(luma[0].t.Get(), QW, QH), h1 = g.readTex<uint16_t>(luma[1].t.Get(), QW, QH);
    for (uint16_t h : h0) r.lumaA.push_back(halfToFloat(h));
    for (uint16_t h : h1) r.lumaB.push_back(halfToFloat(h));
    r.depthA = g.readTex<float>(dq[0].t.Get(), QW, QH);
    r.depthB = g.readTex<float>(dq[1].t.Get(), QW, QH);
    r.key = g.readTex<uint32_t>(key.t.Get(), QW, QH);
    r.ok = true;
    return r;
}

// ---- the CPU reference -----------------------------------------------------------------------------------------------------------------------------------

struct Ref {
    std::vector<uint32_t> key;
    FrameGenShadowEyeResult res;
};

void carryCpu(const float d[3], float z, bool isFar, const float rows[][4], float dp[3]) {
    for (int r = 0; r < 3; ++r) dp[r] = rows[r][0] * d[0] + rows[r][1] * d[1] + rows[r][2] * d[2];
    if (!isFar) for (int r = 0; r < 3; ++r) dp[r] = dp[r] * z + rows[r][3];
}

float cellBilinear(const std::vector<float>& t, float cx, float cy) {
    const int ix = static_cast<int>(std::floor(cx)), iy = static_cast<int>(std::floor(cy));
    const float wx = cx - static_cast<float>(ix), wy = cy - static_cast<float>(iy);
    const auto at = [&](int x, int y) { return t[static_cast<size_t>(std::clamp(y, 0, QH - 1)) * QW + std::clamp(x, 0, QW - 1)]; };
    const float a = at(ix, iy), b = at(ix + 1, iy), c = at(ix, iy + 1), d = at(ix + 1, iy + 1);
    const float top = a + (b - a) * wx, bottom = c + (d - c) * wx;
    return top + (bottom - top) * wy;
}

void depthClass(float zr, const FrameGenShadowCb& cb, float* z, bool* isFar, uint32_t* cls) {
    const float den = zr - cb.dk[0];
    *isFar = zr <= 0.0f || den <= 0.0f;
    *z = *isFar ? 0.0f : cb.dk[1] / den;
    *cls = *isFar ? 0u : (*z > cb.dk[2] ? 2u : 1u);
}

// The destination of A's cell (cx, cy) in B's render pixels by the forward rows of `cb`; false when it does not project.
bool forwardPixel(const FrameGenShadowCb& cb, int cx, int cy, float zr, float* ppx, float* ppy, float* zB, uint32_t* cls, bool* isFar) {
    float z;
    depthClass(zr, cb, &z, isFar, cls);
    float d[3];
    temporalPixelToDir(static_cast<float>(cx * 4) + 1.5f, static_cast<float>(cy * 4) + 1.5f, cb.tanA, RW, RH, d);
    float dp[3];
    carryCpu(d, z, *isFar, cb.fw + *cls * 3, dp);
    *zB = -dp[2];
    return temporalDirToPixel(dp, cb.tanB, RW, RH, ppx, ppy);
}

Ref cpuScore(const FrameGenShadowCb& cb, const std::vector<float>& lumaA, const std::vector<float>& depthA, const std::vector<float>& lumaB) {
    Ref r;
    r.key.assign(static_cast<size_t>(QW) * QH, 0u);
    for (int cy = 0; cy < QH; ++cy) {
        for (int cx = 0; cx < QW; ++cx) {
            float px, py, zB;
            uint32_t cls;
            bool isFar;
            if (!forwardPixel(cb, cx, cy, depthA[static_cast<size_t>(cy) * QW + cx], &px, &py, &zB, &cls, &isFar)) continue;
            uint32_t key = 4u;
            if (!isFar) key = (std::max(asUint(1.0f / std::max(zB, 1e-6f)), 8u) & ~3u) | cls;
            const float qx = (px - 1.5f) * 0.25f, qy = (py - 1.5f) * 0.25f;
            const int bx = static_cast<int>(std::floor(qx)), by = static_cast<int>(std::floor(qy));
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const int x = bx + dx, y = by + dy;
                    if (x >= 0 && y >= 0 && x < QW && y < QH) {
                        uint32_t& k = r.key[static_cast<size_t>(y) * QW + x];
                        k = std::max(k, key);
                    }
                }
        }
    }
    for (int cy = 0; cy < QH; ++cy) {
        for (int cx = 0; cx < QW; ++cx) {
            ++r.res.cells;
            const uint32_t key = r.key[static_cast<size_t>(cy) * QW + cx];
            bool hole = true;
            if (key != 0u) {
                const bool isFar = key == 4u;
                const uint32_t cls = isFar ? 0u : (key & 3u);
                const float z = isFar ? 0.0f : 1.0f / std::max(asFloat(key & ~3u), 1e-20f);
                float d[3];
                temporalPixelToDir(static_cast<float>(cx * 4) + 1.5f, static_cast<float>(cy * 4) + 1.5f, cb.tanB, RW, RH, d);
                float dp[3], px, py;
                carryCpu(d, z, isFar, cb.bw + cls * 3, dp);
                if (temporalDirToPixel(dp, cb.tanA, RW, RH, &px, &py) && px >= 0.0f && py >= 0.0f && px <= RW - 1.0f && py <= RH - 1.0f) {
                    const float pred = cellBilinear(lumaA, (px - 1.5f) * 0.25f, (py - 1.5f) * 0.25f);
                    const size_t i = static_cast<size_t>(cy) * QW + cx;
                    const float e = std::fabs(lumaB[i] - pred), eId = std::fabs(lumaB[i] - lumaA[i]);
                    FrameGenShadowClassStats& s = r.res.cls[cls];
                    ++s.pixels;
                    ++s.hist[frameGenShadowBin(e)];
                    ++s.histId[frameGenShadowBin(eId)];
                    s.sumErr1024 += static_cast<uint32_t>(e * 1024.0f + 0.5f);
                    s.sumErrId1024 += static_cast<uint32_t>(eId * 1024.0f + 0.5f);
                    hole = false;
                }
            }
            if (hole) ++r.res.holes;
        }
    }
    return r;
}

// ---- comparing and summarising ---------------------------------------------------------------------------------------------------------------------------

struct Compare {
    unsigned keyMismatch = 0, histL1 = 0, holeDiff = 0, pixelDiff = 0;
};

Compare compareGpuCpu(const GpuResult& g, const Ref& c) {
    Compare k;
    for (size_t i = 0; i < g.key.size() && i < c.key.size(); ++i) {
        // The low two bits are the class and the rest the nearness; a one-ulp difference in 1 / z is not a different key in kind.
        if ((g.key[i] & 3u) != (c.key[i] & 3u) || (g.key[i] == 0u) != (c.key[i] == 0u) || (g.key[i] == 4u) != (c.key[i] == 4u)) ++k.keyMismatch;
    }
    FrameGenShadowEyeResult res;
    frameGenDecodeStats(g.stats.data(), res);
    k.holeDiff = static_cast<unsigned>(res.holes > c.res.holes ? res.holes - c.res.holes : c.res.holes - res.holes);
    for (int cl = 0; cl < kFrameGenShadowClasses; ++cl) {
        const uint32_t a = res.cls[cl].pixels, b = c.res.cls[cl].pixels;
        k.pixelDiff += a > b ? a - b : b - a;
        for (int i = 0; i < kFrameGenShadowBins; ++i) {
            k.histL1 += res.cls[cl].hist[i] > c.res.cls[cl].hist[i] ? res.cls[cl].hist[i] - c.res.cls[cl].hist[i] : c.res.cls[cl].hist[i] - res.cls[cl].hist[i];
            k.histL1 += res.cls[cl].histId[i] > c.res.cls[cl].histId[i] ? res.cls[cl].histId[i] - c.res.cls[cl].histId[i] : c.res.cls[cl].histId[i] - res.cls[cl].histId[i];
        }
    }
    return k;
}

double meanErr(const FrameGenShadowEyeResult& r, int cls, bool noWarp) {
    const FrameGenShadowClassStats& s = r.cls[cls];
    if (!s.pixels) return 0.0;
    return static_cast<double>(noWarp ? s.sumErrId1024 : s.sumErr1024) / 1024.0 / s.pixels;
}

// The GPU result held to the CPU reference (the keys, the holes, the per-class counts and the error histograms agree to a few cells of the 768).
void expectAgree(Scope& sc, const GpuResult& g, const Ref& c, const char* tag) {
    const Compare k = compareGpuCpu(g, c);
    char label[160];
    std::snprintf(label, sizeof(label), "%s: the shader's key grid is the CPU reference's (%u of %d cells differ)", tag, k.keyMismatch, QW * QH);
    sc.ok(k.keyMismatch <= 8, label);
    std::snprintf(label, sizeof(label), "%s: the shader's holes, class counts and error histograms are the CPU reference's (holes %u, counts %u, bins %u)", tag, k.holeDiff, k.pixelDiff, k.histL1);
    sc.ok(k.holeDiff <= 8 && k.pixelDiff <= 8 && k.histL1 <= 32, label);
}

FrameGenShadowCb makeCb(const Scene& a, const FrameGenDeltas& dA, const FrameGenDeltas& dB, float split, bool mutateForward = false, bool identityBackward = false, float depthA = 0.0f) {
    (void)a;
    float Wp[9], tp[3];
    frameGenPredictWorld(dA, dB, Wp, tp);
    FrameGenAffine bwd[kFrameGenShadowClasses];
    frameGenBackward(dB, Wp, tp, bwd);
    if (identityBackward) for (auto& b : bwd) b = FrameGenAffine{};
    FrameGenShadowCb cb;
    frameGenFillCb(cb, RW, RH, RW, RH, kTan, kTan, depthA, kDepthB, split, bwd);
    if (mutateForward) std::memcpy(cb.fw, cb.bw, sizeof(cb.fw));
    return cb;
}

// ---- cases -----------------------------------------------------------------------------------------------------------------------------------------------

void case0Capture(Gpu& g, Scope& sc) {
    // 1:1: a 4x4 checker of cells with a distinct value in each cell, and one near depth in a block.
    for (int pass = 0; pass < 2; ++pass) {
        const int s = pass == 0 ? 1 : 2;   // the output is s times the render size on each axis
        Scene A(RW, RH, RW * s, RH * s), B(RW, RH, RW * s, RH * s);
        for (int y = 0; y < RH * s; ++y)
            for (int x = 0; x < RW * s; ++x) {
                const int cx = x / (4 * s), cy = y / (4 * s);
                const float v = 0.1f + 0.8f * static_cast<float>((cx * 7 + cy * 13) % 11) / 10.0f;
                A.out[static_cast<size_t>(y) * RW * s + x] = v;
                B.out[static_cast<size_t>(y) * RW * s + x] = 1.0f - v;
            }
        for (int y = 0; y < RH; ++y)
            for (int x = 0; x < RW; ++x) A.depth[static_cast<size_t>(y) * RW + x] = ((x / 4) % 5 == 0 && (y / 4) % 3 == 0) ? 0.0f : 0.01f;
        A.depth[static_cast<size_t>(6) * RW + 9] = 0.5f;   // one nearer pixel in the block (2, 1): the cell takes it
        FrameGenShadowCb cb;
        FrameGenAffine ident[kFrameGenShadowClasses];
        frameGenFillCb(cb, RW, RH, RW * s, RH * s, kTan, kTan, 0.0f, kDepthB, 10.0f, ident);
        const GpuResult r = runGpu(g, A, B, cb);
        sc.ok(r.ok, "case 0: the capture ran");
        if (!r.ok) return;
        double worst = 0.0;
        bool depthOk = true;
        for (int cy = 0; cy < QH; ++cy)
            for (int cx = 0; cx < QW; ++cx) {
                const float expectA = 0.1f + 0.8f * static_cast<float>((cx * 7 + cy * 13) % 11) / 10.0f;
                worst = std::max(worst, static_cast<double>(std::fabs(r.lumaA[static_cast<size_t>(cy) * QW + cx] - expectA)));
                worst = std::max(worst, static_cast<double>(std::fabs(r.lumaB[static_cast<size_t>(cy) * QW + cx] - (1.0f - expectA))));
                float expectD = ((cx % 5 == 0) && (cy % 3 == 0)) ? 0.0f : 0.01f;
                if (cx == 2 && cy == 1) expectD = 0.5f;
                if (r.depthA[static_cast<size_t>(cy) * QW + cx] != expectD) depthOk = false;
            }
        char label[160];
        std::snprintf(label, sizeof(label), "case 0 (output x%d): each cell's luma is its area's mean (worst %.5f, half-float storage)", s, worst);
        sc.ok(worst < 0.0012, label);
        std::snprintf(label, sizeof(label), "case 0 (output x%d): each cell's depth is the nearest of its 4x4 (a far cell stays far)", s);
        sc.ok(depthOk, label);
        if (g_verbose) std::printf("  case 0 x%d: worst luma error %.6f\n", s, worst);
    }
}

void case1Static(Gpu& g, Scope& sc) {
    Scene A, B;
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) {
            const float v = 0.5f + 0.3f * std::sin(0.21f * x) * std::cos(0.17f * y);
            A.out[static_cast<size_t>(y) * RW + x] = B.out[static_cast<size_t>(y) * RW + x] = v;
            const float zr = x < 40 ? 0.0f : (x < 84 ? kDepthB / 0.8f : kDepthB / 30.0f);
            A.depth[static_cast<size_t>(y) * RW + x] = B.depth[static_cast<size_t>(y) * RW + x] = zr;
        }
    FrameGenDeltas id;
    const FrameGenShadowCb cb = makeCb(A, id, id, 10.0f);
    const GpuResult r = runGpu(g, A, B, cb);
    sc.ok(r.ok, "case 1: the scorer ran");
    if (!r.ok) return;
    FrameGenShadowEyeResult res;
    frameGenDecodeStats(r.stats.data(), res);
    uint64_t scored = 0, inBin0 = 0, inBin0Id = 0;
    for (int c = 0; c < kFrameGenShadowClasses; ++c) { scored += res.cls[c].pixels; inBin0 += res.cls[c].hist[0]; inBin0Id += res.cls[c].histId[0]; }
    char label[200];
    std::snprintf(label, sizeof(label), "case 1: a static scene with identity deltas has no hole (%llu holes) and scores every cell (%llu of %llu)",
                  static_cast<unsigned long long>(res.holes), static_cast<unsigned long long>(scored), static_cast<unsigned long long>(res.cells));
    sc.ok(res.holes == 0 && scored == static_cast<uint64_t>(QW * QH) && res.cells == static_cast<uint64_t>(QW * QH), label);
    sc.ok(inBin0 == scored && inBin0Id == scored, "case 1: every error, warped and not, is in bin 0 (the predicted luma equals the real one)");
    sc.ok(res.cls[0].pixels > 0 && res.cls[1].pixels > 0 && res.cls[2].pixels > 0, "case 1: all three classes are present (far, cockpit at 0.8 m, world at 30 m)");
    expectAgree(sc, r, cpuScore(cb, r.lumaA, r.depthA, r.lumaB), "case 1");
    if (g_verbose) std::printf("  case 1: far %u cockpit %u world %u holes %llu\n", res.cls[0].pixels, res.cls[1].pixels, res.cls[2].pixels, static_cast<unsigned long long>(res.holes));
}

void case2FarRotation(Gpu& g, Scope& sc, bool mutate) {
    float R[9];
    rotY(3.0f, R);
    FrameGenDeltas dA, dB;
    std::memcpy(dB.Hd, R, sizeof(R));
    std::memcpy(dB.W, R, sizeof(R));
    Scene A, B;   // the far plane: depth 0 everywhere
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) {
            float dir[3], world[3];
            pixelDir(x, y, dir);
            A.out[static_cast<size_t>(y) * RW + x] = contentF(dir[0], dir[1]);
            temporalApply3(R, dir, world);   // B's direction in A's eye space: x_A = R x_B
            B.out[static_cast<size_t>(y) * RW + x] = contentF(world[0] / -world[2], world[1] / -world[2]);
        }
    const FrameGenShadowCb cb = makeCb(A, dA, dB, 10.0f, mutate);
    const GpuResult r = runGpu(g, A, B, cb);
    sc.ok(r.ok, "case 2: the scorer ran");
    if (!r.ok) return;
    // The carry: every cell's forward destination against the plan's closed form, dir_B = R^T dir_A, in double precision; and the round trip back.
    double worstClosed = 0.0, worstRound = 0.0;
    float RT[9];
    temporalTranspose3(R, RT);
    for (int cy = 0; cy < QH; ++cy)
        for (int cx = 0; cx < QW; ++cx) {
            float px, py, zB;
            uint32_t cls;
            bool isFar;
            if (!forwardPixel(cb, cx, cy, 0.0f, &px, &py, &zB, &cls, &isFar)) continue;
            double d[3], e[3];
            const double tx = kTan[0] + (cx * 4 + 1.5 + 0.5) / RW * (kTan[1] - kTan[0]);
            const double ty = kTan[3] - (cy * 4 + 1.5 + 0.5) / RH * (kTan[3] - kTan[2]);
            d[0] = tx; d[1] = ty; d[2] = -1.0;
            for (int i = 0; i < 3; ++i) e[i] = RT[i * 3] * d[0] + RT[i * 3 + 1] * d[1] + RT[i * 3 + 2] * d[2];
            const double ex = ((e[0] / -e[2]) - kTan[0]) / (kTan[1] - kTan[0]) * RW - 0.5;
            const double ey = (kTan[3] - (e[1] / -e[2])) / (kTan[3] - kTan[2]) * RH - 0.5;
            worstClosed = std::max(worstClosed, std::hypot(px - ex, py - ey));
            // back by the backward rows
            float d2[3], dp[3], bx, by;
            temporalPixelToDir(px, py, kTan, RW, RH, d2);
            carryCpu(d2, 0.0f, true, cb.bw, dp);
            if (temporalDirToPixel(dp, kTan, RW, RH, &bx, &by)) worstRound = std::max(worstRound, std::hypot(bx - (cx * 4 + 1.5), by - (cy * 4 + 1.5)));
        }
    char label[200];
    std::snprintf(label, sizeof(label), "case 2: the forward carry lands within 0.1 px of the closed form R^T (worst %.4f px)", worstClosed);
    sc.ok(worstClosed < 0.1, label);
    std::snprintf(label, sizeof(label), "case 2: forward then backward returns to the cell centre (worst %.4f px)", worstRound);
    sc.ok(worstRound < 0.1, label);
    FrameGenShadowEyeResult res;
    frameGenDecodeStats(r.stats.data(), res);
    bool allFar = true;
    for (uint32_t k : r.key) if (k != 0u && k != 4u) allFar = false;
    sc.ok(allFar && res.cls[1].pixels == 0 && res.cls[2].pixels == 0, "case 2: every key is the far key and only the far class is scored");
    // The holes are the strip the rotation uncovers: a few columns on one side, and nothing in the middle.
    unsigned midHoles = 0;
    for (int cy = 0; cy < QH; ++cy) for (int cx = 4; cx < QW - 4; ++cx) if (r.key[static_cast<size_t>(cy) * QW + cx] == 0u) ++midHoles;
    sc.ok(midHoles == 0 && res.holes < static_cast<uint64_t>(QH) * 4, "case 2: no hole away from the border, and the strip the rotation uncovers is under four cell columns");
    const double mean = meanErr(res, 0, false), meanId = meanErr(res, 0, true);
    std::snprintf(label, sizeof(label), "case 2: the warped error is near 0 (mean %.4f) and 8x under the no-warp error (mean %.4f)", mean, meanId);
    sc.ok(mean < 0.006 && meanId > 8.0 * mean, label);
    expectAgree(sc, r, cpuScore(cb, r.lumaA, r.depthA, r.lumaB), "case 2");
    if (g_verbose && !mutate) std::printf("  case 2: closed form %.5f px, round trip %.5f px, mean %.5f vs no-warp %.5f, holes %llu\n", worstClosed, worstRound, mean, meanId, static_cast<unsigned long long>(res.holes));
}

void case3Cockpit(Gpu& g, Scope& sc) {
    FrameGenDeltas dA, dB;
    dB.th[0] = 0.08f;
    dB.tv[0] = 0.08f;   // the ship is still: the camera's translation is the head's
    Scene A, B;
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) {
            float dir[3];
            pixelDir(x, y, dir);
            A.out[static_cast<size_t>(y) * RW + x] = contentF(dir[0], dir[1]);
            B.out[static_cast<size_t>(y) * RW + x] = contentF(dir[0] + dB.th[0], dir[1]);   // the plane's point: x_A = x_B + th
            A.depth[static_cast<size_t>(y) * RW + x] = B.depth[static_cast<size_t>(y) * RW + x] = kDepthB / 1.0f;
        }
    const FrameGenShadowCb cb = makeCb(A, dA, dB, 10.0f);
    const GpuResult r = runGpu(g, A, B, cb);
    sc.ok(r.ok, "case 3: the scorer ran");
    if (!r.ok) return;
    FrameGenShadowEyeResult res;
    frameGenDecodeStats(r.stats.data(), res);
    // 0.08 tangent is 5.12 px = 1.28 cells: each source cell lands 1.28 cells left and covers the two cells below that, so only the last column is never reached.
    bool strip = true;
    unsigned holes = 0;
    for (int cy = 0; cy < QH; ++cy)
        for (int cx = 0; cx < QW; ++cx) {
            const bool hole = r.key[static_cast<size_t>(cy) * QW + cx] == 0u;
            if (hole) ++holes;
            if (hole != (cx == QW - 1)) strip = false;
        }
    char label[200];
    std::snprintf(label, sizeof(label), "case 3: the disocclusion strip is the last cell column (%u holes, %d expected)", holes, QH);
    sc.ok(strip && holes == static_cast<unsigned>(QH) && res.holes == static_cast<uint64_t>(QH), label);
    sc.ok(res.cls[1].pixels == static_cast<uint32_t>(QW * QH - QH) && res.cls[0].pixels == 0 && res.cls[2].pixels == 0, "case 3: every scored cell is the cockpit class");
    const double mean = meanErr(res, 1, false), meanId = meanErr(res, 1, true);
    std::snprintf(label, sizeof(label), "case 3: the cockpit's shift is right: warped mean %.4f, no-warp mean %.4f", mean, meanId);
    sc.ok(mean < 0.006 && meanId > 8.0 * mean, label);
    float Wp[9], tp[3], deg = -1.0f, mm = -1.0f;
    frameGenPredictWorld(dA, dB, Wp, tp);
    frameGenRigMiss(Wp, tp, dB, &deg, &mm);
    sc.ok(deg < 0.001f && mm < 0.01f, "case 3: the rig miss is 0 for a still ship under a head translation");
    expectAgree(sc, r, cpuScore(cb, r.lumaA, r.depthA, r.lumaB), "case 3");
    if (g_verbose) std::printf("  case 3: holes %u, cockpit %u, mean %.5f vs no-warp %.5f\n", holes, res.cls[1].pixels, mean, meanId);
}

// A world plane 50 m ahead in B's eye space, seen from A: `truthW`, `truthTv` carry a point of B's space to A's (x_A = W x_B + tv).
void worldPlane(Scene& A, Scene& B, const float truthW[9], const float truthTv[3]) {
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) {
            float d[3];
            pixelDir(x, y, d);
            const size_t i = static_cast<size_t>(y) * RW + x;
            B.out[i] = contentF(d[0], d[1]);   // u = d * 50, the texture is a function of u / 50
            B.depth[i] = kDepthB / 50.0f;
            // A: the point at distance t along d_A that lies on the plane u.z = -50 with u = W^T (P_A - tv)
            double num = -50.0, den = 0.0;
            for (int j = 0; j < 3; ++j) { num += truthW[j * 3 + 2] * truthTv[j]; den += truthW[j * 3 + 2] * d[j]; }
            const double t = num / den;
            double pa[3] = {d[0] * t - truthTv[0], d[1] * t - truthTv[1], d[2] * t - truthTv[2]}, u[3];
            for (int c = 0; c < 3; ++c) u[c] = truthW[0 * 3 + c] * pa[0] + truthW[1 * 3 + c] * pa[1] + truthW[2 * 3 + c] * pa[2];
            A.out[i] = contentF(static_cast<float>(u[0] / 50.0), static_cast<float>(u[1] / 50.0));
            A.depth[i] = kDepthB / static_cast<float>(t);
        }
}

struct World4 {
    double meanConst = 0.0, meanAccel = 0.0;
    float degAccel = 0.0f, mmAccel = 0.0f, degConst = 0.0f, mmConst = 0.0f;
    bool ran = false;
};

World4 case4World(Gpu& g, Scope& sc, bool identityBackward) {
    World4 out;
    // Constant ship motion: 2 degrees of yaw and 2 m of travel a frame (the turn and the travel move the plane the same way on the image), in both frames.
    float W[9];
    rotY(2.0f, W);
    const float tv[3] = {-2.0f, 0.0f, 0.5f};
    FrameGenDeltas dA, dB;
    std::memcpy(dA.W, W, sizeof(W)); std::memcpy(dB.W, W, sizeof(W));
    std::memcpy(dA.tv, tv, sizeof(tv)); std::memcpy(dB.tv, tv, sizeof(tv));
    Scene A, B;
    worldPlane(A, B, W, tv);
    FrameGenShadowCb cb = makeCb(A, dA, dB, 10.0f, false, identityBackward);
    GpuResult r = runGpu(g, A, B, cb);
    sc.ok(r.ok, "case 4: the scorer ran");
    if (!r.ok) return out;
    out.ran = true;
    FrameGenShadowEyeResult res;
    frameGenDecodeStats(r.stats.data(), res);
    float Wp[9], tp[3];
    frameGenPredictWorld(dA, dB, Wp, tp);
    frameGenRigMiss(Wp, tp, dB, &out.degConst, &out.mmConst);
    char label[220];
    sc.ok(out.degConst < 0.001f && out.mmConst < 0.01f, "case 4: constant ship motion has rig miss 0");
    out.meanConst = meanErr(res, 2, false);
    const double idMean = meanErr(res, 2, true);
    const uint32_t worldPixels = res.cls[2].pixels;
    std::snprintf(label, sizeof(label), "case 4: the 50 m plane is the world class (%u of %d cells)", worldPixels, QW * QH);
    sc.ok(worldPixels > static_cast<uint32_t>(QW * QH) * 9 / 10 && res.cls[1].pixels == 0, label);
    std::snprintf(label, sizeof(label), "case 4: with the true Hd the prediction reproduces B: warped mean %.4f, no-warp mean %.4f", out.meanConst, idMean);
    sc.ok(out.meanConst < 0.008 && idMean > 5.0 * out.meanConst, label);
    expectAgree(sc, r, cpuScore(cb, r.lumaA, r.depthA, r.lumaB), "case 4");

    // An acceleration: the ship turns 2.8 degrees and travels 3 m this frame, the previous frame's 2.0 and 2. The images are the true ones.
    float W2[9];
    rotY(2.8f, W2);
    const float tv2[3] = {-3.0f, 0.0f, 0.5f};
    FrameGenDeltas dB2;
    std::memcpy(dB2.W, W2, sizeof(W2));
    std::memcpy(dB2.tv, tv2, sizeof(tv2));
    Scene A2, B2;
    worldPlane(A2, B2, W2, tv2);
    cb = makeCb(A2, dA, dB2, 10.0f, false, identityBackward);
    r = runGpu(g, A2, B2, cb);
    sc.ok(r.ok, "case 4 (acceleration): the scorer ran");
    if (!r.ok) return out;
    frameGenDecodeStats(r.stats.data(), res);
    frameGenPredictWorld(dA, dB2, Wp, tp);
    frameGenRigMiss(Wp, tp, dB2, &out.degAccel, &out.mmAccel);
    out.meanAccel = meanErr(res, 2, false);
    std::snprintf(label, sizeof(label), "case 4 (acceleration): the rig miss is the hand figure, 0.8 deg and 1000 mm (%.4f deg, %.3f mm)", static_cast<double>(out.degAccel), static_cast<double>(out.mmAccel));
    sc.ok(std::fabs(out.degAccel - 0.8f) < 0.003f && std::fabs(out.mmAccel - 1000.0f) < 1.0f, label);
    std::snprintf(label, sizeof(label), "case 4 (acceleration): the score is worse than at constant velocity (%.4f against %.4f)", out.meanAccel, out.meanConst);
    sc.ok(out.meanAccel > 2.5 * out.meanConst, label);
    expectAgree(sc, r, cpuScore(cb, r.lumaA, r.depthA, r.lumaB), "case 4 (acceleration)");
    if (g_verbose && !identityBackward) std::printf("  case 4: constant mean %.5f (no-warp %.5f), acceleration mean %.5f, rig miss %.4f deg %.3f mm\n", out.meanConst, idMean, out.meanAccel, static_cast<double>(out.degAccel), static_cast<double>(out.mmAccel));
    return out;
}

void case5ReversedZAndSplit(Gpu& g, Scope& sc) {
    FrameGenDeltas id;
    // Reversed-Z far: with A = 0.001, a written depth equal to A is the far plane (no metres), and one a hair over is 100 km away: the world.
    {
        Scene A, B;
        for (int y = 0; y < RH; ++y)
            for (int x = 0; x < RW; ++x) {
                const size_t i = static_cast<size_t>(y) * RW + x;
                A.out[i] = B.out[i] = 0.5f + 0.2f * std::sin(0.3f * x);
                A.depth[i] = B.depth[i] = x < 64 ? 0.001f : 0.001f + kDepthB / 1.0e5f;
            }
        const FrameGenShadowCb cb = makeCb(A, id, id, 10.0f, false, false, 0.001f);
        const GpuResult r = runGpu(g, A, B, cb);
        sc.ok(r.ok, "case 5: the reversed-Z run happened");
        if (r.ok) {
            FrameGenShadowEyeResult res;
            frameGenDecodeStats(r.stats.data(), res);
            char label[200];
            std::snprintf(label, sizeof(label), "case 5: a written depth at A is the far plane and a hair over it is the world (far %u, cockpit %u, world %u)", res.cls[0].pixels, res.cls[1].pixels, res.cls[2].pixels);
            sc.ok(res.cls[0].pixels >= static_cast<uint32_t>(QW / 2 * QH) && res.cls[0].pixels <= static_cast<uint32_t>((QW / 2 + 1) * QH) && res.cls[1].pixels == 0 &&
                      res.cls[2].pixels >= static_cast<uint32_t>((QW / 2 - 1) * QH),
                  label);
            expectAgree(sc, r, cpuScore(cb, r.lumaA, r.depthA, r.lumaB), "case 5 (reversed-Z)");
        }
    }
    // The split: 0.5 mm and 2 mm.
    for (int pass = 0; pass < 2; ++pass) {
        const float split = pass == 0 ? 0.001f : 10.0f;
        Scene A, B;
        for (int y = 0; y < RH; ++y)
            for (int x = 0; x < RW; ++x) {
                const size_t i = static_cast<size_t>(y) * RW + x;
                A.out[i] = B.out[i] = 0.5f + 0.2f * std::sin(0.3f * x);
                A.depth[i] = B.depth[i] = kDepthB / (x < 64 ? 0.0005f : 0.002f);
            }
        const FrameGenShadowCb cb = makeCb(A, id, id, split);
        const GpuResult r = runGpu(g, A, B, cb);
        sc.ok(r.ok, "case 5: the split run happened");
        if (!r.ok) continue;
        FrameGenShadowEyeResult res;
        frameGenDecodeStats(r.stats.data(), res);
        char label[200];
        if (pass == 0) {
            std::snprintf(label, sizeof(label), "case 5: on foot (split 1 mm) 0.5 mm is the cockpit and 2 mm the world (cockpit %u, world %u of %d)", res.cls[1].pixels, res.cls[2].pixels, QW * QH);
            sc.ok(res.cls[1].pixels >= static_cast<uint32_t>((QW / 2 - 1) * QH) && res.cls[1].pixels <= static_cast<uint32_t>((QW / 2 + 1) * QH) &&
                      res.cls[2].pixels >= static_cast<uint32_t>((QW / 2 - 1) * QH) && res.cls[0].pixels == 0,
                  label);
        } else {
            std::snprintf(label, sizeof(label), "case 5: aboard (split 10 m) both are the cockpit (cockpit %u, world %u)", res.cls[1].pixels, res.cls[2].pixels);
            sc.ok(res.cls[1].pixels == static_cast<uint32_t>(QW * QH) && res.cls[2].pixels == 0, label);
        }
        expectAgree(sc, r, cpuScore(cb, r.lumaA, r.depthA, r.lumaB), pass == 0 ? "case 5 (1 mm split)" : "case 5 (10 m split)");
    }
}

// ---- the module on WARP ----------------------------------------------------------------------------------------------------------------------------------

struct ModuleScene {
    Tex out, depth;
};

void case6Module(Gpu& g, Scope& sc) {
    ModuleScene ms;
    Scene S;
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) {
            const size_t i = static_cast<size_t>(y) * RW + x;
            S.out[i] = 0.5f + 0.3f * std::sin(0.21f * x) * std::cos(0.17f * y);
            S.depth[i] = x < 40 ? 0.0f : (x < 84 ? kDepthB / 0.8f : kDepthB / 30.0f);
        }
    sc.ok(makeInputs(g, S, ms.out, ms.depth), "case 6: the module's inputs were made");
    FrameGenShadowFrame f;
    f.renderW = RW; f.renderH = RH; f.outW = RW; f.outH = RH;
    f.outSrv = ms.out.srv.Get();
    f.depthSrv = ms.depth.srv.Get();
    Tex zeroMv;
    {
        std::vector<uint16_t> zero(static_cast<size_t>(RW) * RH * 2, 0);
        sc.ok(g.makeTex(RW, RH, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, zero.data(), RW * 4, zeroMv, true, false), "case 6: the zero vector field was made");
    }
    f.mvSrv = zeroMv.srv.Get();   // a static scene: the vectors are 0
    f.tanNow = kTan;
    f.tanPrev = kTan;
    f.depthA = 0.0f; f.depthB = kDepthB; f.splitMetres = 10.0f;
    f.depthWritten = true; f.prevDepthValid = true; f.worldValid = true; f.cameraJump = false;

    // The key off: nothing is created, whatever the pass calls.
    frameGenShadowConfigure(false);
    for (uint32_t n = 1; n <= 6; ++n) { frameGenShadowEyeFrame(g.dev.Get(), g.ctx.Get(), n & 1u, n, f); frameGenShadowFrameBoundary(); }
    sc.ok(!frameGenShadowResident(), "case 6: with the key off nothing is created (no shader, buffer or texture is resident)");
    sc.ok(frameGenShadowOpenWindow().framesScored == 0, "case 6: with the key off nothing is scored");

    // On: eight frames of the same scene on both eyes. Frame 1 has no previous; the rest score, with every error in bin 0 and no hole.
    frameGenShadowConfigure(true);
    uint32_t frame = 100;
    for (int n = 0; n < 10; ++n, ++frame) {
        for (uint32_t eye = 0; eye < 2; ++eye) frameGenShadowEyeFrame(g.dev.Get(), g.ctx.Get(), eye, frame, f);
        g.ctx->Flush();
        Sleep(15);
    }
    sc.ok(frameGenShadowResident(), "case 6: with the key on the scorer's resources are resident");
    // two more calls to read back the last frames, with nothing new to score (the previous frame is not the one before)
    FrameGenShadowWindow w = frameGenShadowOpenWindow();
    uint64_t scored = 0, bin0 = 0;
    for (int c = 0; c < kFrameGenShadowClasses; ++c) { scored += w.cls[c].pixels; bin0 += w.cls[c].hist[0]; }
    char label[220];
    std::snprintf(label, sizeof(label), "case 6: a static scene scores through the module: %u eye-frames read back, skipped %u/%u/%u/%u/%u", w.framesScored, w.skipped[0], w.skipped[1], w.skipped[2], w.skipped[3], w.skipped[4]);
    sc.ok(w.framesScored >= 8 && w.skipped[kSkipNoPrevDepth] == 2, label);
    std::snprintf(label, sizeof(label), "case 6: every scored cell's error is in bin 0 (%llu of %llu) with no hole (%llu)", static_cast<unsigned long long>(bin0), static_cast<unsigned long long>(scored),
                  static_cast<unsigned long long>(w.holes));
    sc.ok(scored > 0 && bin0 == scored && w.holes == 0, label);
    sc.ok(scored == static_cast<uint64_t>(w.framesScored) * QW * QH, "case 6: each scored eye-frame covers all 768 cells");

    // The second kernel runs on every scored frame, whatever its set: a static scene with zero vectors lands every cell on itself.
    {
        uint64_t mvPx = 0, mvBin0 = 0;
        for (int c = 0; c < kFrameGenShadowClasses; ++c) { mvPx += w.mv.cls[c].pixels; mvBin0 += w.mv.cls[c].hist[0]; }
        std::snprintf(label, sizeof(label), "case 6: the mv set covers every scored eye-frame (%u of %u + %u) with every error in bin 0 (%llu of %llu)", w.mv.frames, w.framesScored, w.jump.frames,
                      static_cast<unsigned long long>(mvBin0), static_cast<unsigned long long>(mvPx));
        sc.ok(w.mv.frames == w.framesScored + w.jump.frames && mvPx > 0 && mvBin0 == mvPx && mvPx == static_cast<uint64_t>(w.mv.frames) * QW * QH, label);
    }

    // The skip reasons, each counted once on its own frame.
    FrameGenShadowWindow before = frameGenShadowOpenWindow();
    FrameGenShadowFrame bad = f;
    bad.worldValid = false;
    frameGenShadowEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame++, bad);       // rows invalid now
    frameGenShadowEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame++, f);         // ...and the last one's rows were invalid
    frameGenShadowEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame++, f);         // clean: scored
    bad = f;
    bad.cameraJump = true;
    frameGenShadowEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame++, bad);       // jump now
    frameGenShadowEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame++, f);         // ...and the last one jumped
    frameGenShadowEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame + 5, f);       // a gap in the frames: no previous capture
    bad = f;
    bad.depthWritten = false;
    frameGenShadowEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame + 6, bad);     // no depth this frame
    FrameGenShadowWindow after = frameGenShadowOpenWindow();
    std::snprintf(label, sizeof(label), "case 6: the skip reasons count (rows %u, jump %u, no previous depth %u)", after.skipped[kSkipRowsInvalid] - before.skipped[kSkipRowsInvalid],
                  after.skipped[kSkipCameraJump] - before.skipped[kSkipCameraJump], after.skipped[kSkipNoPrevDepth] - before.skipped[kSkipNoPrevDepth]);
    sc.ok(after.skipped[kSkipRowsInvalid] - before.skipped[kSkipRowsInvalid] == 2 && after.skipped[kSkipCameraJump] - before.skipped[kSkipCameraJump] == 2 &&
              after.skipped[kSkipNoPrevDepth] - before.skipped[kSkipNoPrevDepth] == 2,
          label);
    // The two jump frames were scored, into the jump set and not the normal one: a few more calls (each a gap, so nothing new is scored) read them back.
    for (int k = 0; k < 6; ++k) {
        g.ctx->Flush();
        Sleep(15);
        frame += 10;
        frameGenShadowEyeFrame(g.dev.Get(), g.ctx.Get(), 0, frame, f);
    }
    const FrameGenShadowWindow done = frameGenShadowOpenWindow();
    uint64_t jScored = 0, jBin0 = 0;
    for (int c = 0; c < kFrameGenShadowClasses; ++c) { jScored += done.jump.cls[c].pixels; jBin0 += done.jump.cls[c].hist[0]; }
    std::snprintf(label, sizeof(label), "case 6: a camera_jump frame is scored into the jump set (%u frames, %llu of %llu cells in bin 0), not skipped and not in the normal set (%u frames)",
                  done.jump.frames, static_cast<unsigned long long>(jBin0), static_cast<unsigned long long>(jScored), done.framesScored);
    sc.ok(done.jump.frames == 2 && jScored == 2ull * QW * QH && jBin0 == jScored, label);

    // The key off again: the next boundary releases everything.
    frameGenShadowConfigure(false);
    frameGenShadowFrameBoundary();
    sc.ok(!frameGenShadowResident(), "case 6: turning the key off releases every resource at the next frame boundary");
}

// ---- case 7: the second kernel -----------------------------------------------------------------------------------------------------------------------------

// The exact vector field of a plane under B's TRUE rows (frameGenBackward of B's measured deltas): for each render pixel of B at depth z metres (class `cls`), where it is in
// A minus where it is.
std::vector<float> exactMv(const FrameGenDeltas& dB, float z, int cls) {
    FrameGenAffine truth[kFrameGenShadowClasses];
    frameGenBackward(dB, dB.W, dB.tv, truth);
    float rows[3][4];
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) rows[r][k] = truth[cls].R[r * 3 + k];
        rows[r][3] = truth[cls].t[r];
    }
    std::vector<float> mv(static_cast<size_t>(RW) * RH * 2);
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) {
            float d[3], dp[3], px = 0.0f, py = 0.0f;
            pixelDir(x, y, d);
            carryCpu(d, z, false, rows, dp);
            if (!temporalDirToPixel(dp, kTan, RW, RH, &px, &py)) { px = static_cast<float>(x) + 1000.0f; py = static_cast<float>(y); }
            mv[(static_cast<size_t>(y) * RW + x) * 2] = px - static_cast<float>(x);
            mv[(static_cast<size_t>(y) * RW + x) * 2 + 1] = py - static_cast<float>(y);
        }
    return mv;
}

struct Case7Out {
    double mvErr = 0.0;
    bool ran = false;
};

// mode 0 exact vectors, 1 every vector offset by +2 px in x, 2 every vector negated (what a kernel reading the sign wrong sees)
Case7Out case7World(Gpu& g, Scope& sc, int mode) {
    Case7Out out;
    float W[9];
    rotY(2.0f, W);
    const float tv[3] = {-2.0f, 0.0f, 0.5f};
    FrameGenDeltas dA, dB;
    std::memcpy(dA.W, W, sizeof(W)); std::memcpy(dB.W, W, sizeof(W));
    std::memcpy(dA.tv, tv, sizeof(tv)); std::memcpy(dB.tv, tv, sizeof(tv));
    Scene A, B;
    worldPlane(A, B, W, tv);
    const FrameGenShadowCb cb = makeCb(A, dA, dB, 10.0f);
    std::vector<float> mv = exactMv(dB, 50.0f, 2);
    for (size_t i = 0; i < mv.size(); i += 2) {
        if (mode == 1) mv[i] += 2.0f;
        if (mode == 2) { mv[i] = -mv[i]; mv[i + 1] = -mv[i + 1]; }
    }
    const GpuResult r = runGpu(g, A, B, cb, &mv);
    sc.ok(r.ok, "case 7: the scorer ran");
    if (!r.ok) return out;
    out.ran = true;
    FrameGenShadowEyeResult res;
    frameGenDecodeStats(r.stats.data(), res);
    const double mvErr = res.mv.cls[2].pixels ? static_cast<double>(res.mv.cls[2].sumErr1024) / 1024.0 / res.mv.cls[2].pixels : 1.0;
    out.mvErr = mvErr;
    char label[240];
    const char* tag = mode == 0 ? "case 7 (exact vectors)" : (mode == 1 ? "case 7 (+2 px)" : "case 7 (negated vectors)");
    std::snprintf(label, sizeof(label), "%s: the plane is the world class in the mv set (%u of %d cells, all %llu cells looked at)", tag, res.mv.cls[2].pixels, QW * QH,
                  static_cast<unsigned long long>(res.mv.cells));
    sc.ok(res.mv.cls[2].pixels > static_cast<uint32_t>(QW * QH) * 8 / 10 && res.mv.cls[1].pixels == 0 && res.mv.cells == static_cast<uint64_t>(QW * QH), label);
    if (mode != 1) {
        std::snprintf(label, sizeof(label), "%s: the mv landing reproduces B (mean error %.4f)", tag, mvErr);
        sc.ok(mvErr < 0.008, label);
    } else {
        std::snprintf(label, sizeof(label), "%s: the mv landing is off (mean error %.4f)", tag, mvErr);
        sc.ok(mvErr > 0.012, label);
    }
    return out;
}

// A cockpit plane 1 m ahead under an 8 cm head translation, exact vectors: the cockpit's mv set reproduces B.
void case7Cockpit(Gpu& g, Scope& sc) {
    FrameGenDeltas dA, dB;
    dB.th[0] = 0.08f;
    dB.tv[0] = 0.08f;
    Scene A, B;
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) {
            float dir[3];
            pixelDir(x, y, dir);
            A.out[static_cast<size_t>(y) * RW + x] = contentF(dir[0], dir[1]);
            B.out[static_cast<size_t>(y) * RW + x] = contentF(dir[0] + dB.th[0], dir[1]);
            A.depth[static_cast<size_t>(y) * RW + x] = B.depth[static_cast<size_t>(y) * RW + x] = kDepthB / 1.0f;
        }
    const FrameGenShadowCb cb = makeCb(A, dA, dB, 10.0f);
    const std::vector<float> mv = exactMv(dB, 1.0f, 1);
    const GpuResult r = runGpu(g, A, B, cb, &mv);
    sc.ok(r.ok, "case 7 (cockpit): the scorer ran");
    if (!r.ok) return;
    FrameGenShadowEyeResult res;
    frameGenDecodeStats(r.stats.data(), res);
    char label[240];
    const double mvErr = res.mv.cls[1].pixels ? static_cast<double>(res.mv.cls[1].sumErr1024) / 1024.0 / res.mv.cls[1].pixels : 1.0;
    std::snprintf(label, sizeof(label), "case 7 (cockpit): the cockpit's mv set reproduces B (mean error %.4f, %u cells)", mvErr, res.mv.cls[1].pixels);
    sc.ok(mvErr < 0.008 && res.mv.cls[1].pixels > static_cast<uint32_t>(QW * QH) * 8 / 10 && res.mv.cls[0].pixels == 0 && res.mv.cls[2].pixels == 0, label);
}

// The shader itself with the sign flipped: fgShadowScoreB recompiled from the production text with `c + Mv.Load` made `c - Mv.Load`.
bool compileFlippedScoreB(Gpu& g, ComPtr<ID3D11ComputeShader>& out) {
    HMODULE lib = LoadLibraryA("d3dcompiler_47.dll");
    if (!lib) return false;
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    const CompileFn compile = reinterpret_cast<CompileFn>(GetProcAddress(lib, "D3DCompile"));
    if (!compile) return false;
    std::string text = frameGenShadowShaderSource();
    const std::string from = "float2 pm = c + Mv.Load", to = "float2 pm = c - Mv.Load";
    const size_t at = text.find(from);
    if (at == std::string::npos) return false;
    text.replace(at, from.size(), to);
    ComPtr<ID3DBlob> code, err;
    if (FAILED(compile(text.data(), text.size(), "framegen_flipped", nullptr, nullptr, "fgShadowScoreB", "cs_5_0", 0, 0, &code, &err))) return false;
    return SUCCEEDED(g.dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &out));
}

// ---- the run ---------------------------------------------------------------------------------------------------------------------------------------------

int run() {
    Gpu g;
    if (!g.init()) {
        std::printf("FAIL: WARP could not start or the production shaders did not create\n");
        return 1;
    }
    Scope sc;
    case0Capture(g, sc);
    case1Static(g, sc);
    case2FarRotation(g, sc, false);
    case3Cockpit(g, sc);
    const World4 w4 = case4World(g, sc, false);
    case5ReversedZAndSplit(g, sc);
    case6Module(g, sc);
    const Case7Out c7exact = case7World(g, sc, 0);
    const Case7Out c7off = case7World(g, sc, 1);
    case7Cockpit(g, sc);
    {
        char label[200];
        std::snprintf(label, sizeof(label), "case 7: the vectors' error moves with the vectors (exact mv %.4f, +2 px mv %.4f)", c7exact.mvErr, c7off.mvErr);
        sc.ok(c7exact.ran && c7off.ran && c7off.mvErr > 3.0 * c7exact.mvErr, label);
    }

    // The mutations: each must break its case.
    Scope m1;
    m1.quiet = true;
    case2FarRotation(g, m1, true);
    ++g_checks;
    if (m1.fails == 0) { ++g_failures; std::printf("FAIL: mutation: a forward transform that is not inverted still passes case 2\n"); }
    Scope m2;
    m2.quiet = true;
    case4World(g, m2, true);
    ++g_checks;
    if (m2.fails == 0) { ++g_failures; std::printf("FAIL: mutation: an identity backward transform still passes case 4\n"); }
    Scope m3;
    m3.quiet = true;
    case7World(g, m3, 2);   // the vectors' sign wrong, which is what a kernel reading the sign wrong would see
    ++g_checks;
    if (m3.fails == 0) { ++g_failures; std::printf("FAIL: mutation: vectors of the wrong sign still pass case 7\n"); }
    Scope m4;
    m4.quiet = true;
    {
        ComPtr<ID3D11ComputeShader> flipped;
        ComPtr<ID3D11ComputeShader> saved = g.cs[4];
        ++g_checks;
        if (!compileFlippedScoreB(g, flipped)) {
            ++g_failures;
            std::printf("FAIL: mutation: the sign-flipped shader could not be built (is `float2 pm = c + Mv.Load` still in the kernel?)\n");
        } else {
            g.cs[4] = flipped;
            case7World(g, m4, 0);
            g.cs[4] = saved;
            if (m4.fails == 0) { ++g_failures; std::printf("FAIL: mutation: the shader reading the vector with the wrong sign still passes case 7\n"); }
        }
    }
    (void)w4;
    std::printf("%s: %u checks, %u failures (mutations: forward-not-inverted broke %u checks, identity-backward broke %u, wrong-sign vectors broke %u, wrong-sign shader broke %u)\n", g_failures ? "FAIL" : "PASS", g_checks,
                g_failures, m1.fails, m2.fails, m3.fails, m4.fails);
    return g_failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool selfTest = false, dry = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--self-test")) selfTest = true;
        else if (!std::strcmp(argv[i], "--dry-run")) dry = true;
        else if (!std::strcmp(argv[i], "--verbose")) g_verbose = true;
        else { std::printf("usage: framegen_shadow_test [--dry-run | --self-test] [--verbose]\n"); return 2; }
    }
    if (!selfTest && !dry) { std::printf("usage: framegen_shadow_test [--dry-run | --self-test] [--verbose]\n"); return 2; }
    return run();
}

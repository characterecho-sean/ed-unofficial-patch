// The HUD layer's edge smoothing (docs/design-flat-ui-quality-2026-10-05.md, "2026-10-24: edge smoothing for the HUD layer"): the production SMAA passes of
// src/d3d11/ui_layer_smaa.h -- the build's bytecode, the lookup textures, the metrics, the samplers -- on WARP, over a 64x64 layer in the layer's own
// convention: premultiplied light in rgb, transmittance in alpha, cleared to (0, 0, 0, 1).
//
// What the pass must do to such a layer, and what it must not:
//   (a) a shallow staircase (rise 1 over run 4) of opaque white (rgb 1, a 0) on the clear: the steps take intermediate values in rgb AND in alpha, and the
//       summed opacity (the sum of 1 - a, which is what the composite's box filter would keep) holds within a few percent;
//   (b) the same staircase in dark opaque (rgb 0, a 0): rgb stays 0 and alpha takes intermediate values. That edge shows in alpha alone, so this is the
//       four-channel edge signal; the map's colour-only edge pass over the same layer, run through the same weights and blend, leaves it a staircase;
//   (c) an all-clear layer comes out exactly (0, 0, 0, 1) everywhere;
//   (d) a straight band across the whole layer, horizontal and vertical, comes out unchanged;
// plus the rest of the contract: the compute-stage slots a pass touched come back (the two samplers) or are left null, a size change re-makes the targets, and
// a creation that failed at a size is told once and not retried at that size.

#pragma once

#include "../../src/d3d11/ui_layer_smaa.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace uilayersmaagpu {

struct Px {
    uint8_t r, g, b, a;
};
constexpr uint32_t kSize = 64;

inline std::vector<Px> clearLayer(uint32_t w, uint32_t h) { return std::vector<Px>(size_t(w) * h, Px{0, 0, 0, 255}); }

// Everything on or below the line y = 16 + x / 4 (a step every four columns, one row high) is `fill`; the rest is the clear.
inline std::vector<Px> staircase(uint32_t w, uint32_t h, Px fill) {
    std::vector<Px> px = clearLayer(w, h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            if (y >= 16 + x / 4) px[size_t(y) * w + x] = fill;
    return px;
}

// A band across the whole layer: rows y0..y1 (horizontal) or columns x0..x1 (vertical), inclusive.
inline std::vector<Px> band(uint32_t w, uint32_t h, bool horizontal, uint32_t from, uint32_t to, Px fill) {
    std::vector<Px> px = clearLayer(w, h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t c = horizontal ? y : x;
            if (c >= from && c <= to) px[size_t(y) * w + x] = fill;
        }
    return px;
}

// The pass over `in`: ensure, run, and the output read back. Returns false when the pass could not be had.
inline bool smooth(ID3D11Device* device, ID3D11DeviceContext* context, edvr::UiLayerSmaa& s, const std::vector<Px>& in, uint32_t w, uint32_t h,
                   std::vector<Px>& out) {
    ComPtr<ID3D11Texture2D> layer = ::texture(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, in.data(), w * 4);
    if (!layer) return false;
    ComPtr<ID3D11ShaderResourceView> srv = ::view(device, layer.Get());
    if (!srv) return false;
    edvr::UiLayerSmaaFail fail;
    const edvr::UiLayerSmaaState state = edvr::uiLayerSmaaEnsure(s, device, w, h, &fail);
    if (state != edvr::UiLayerSmaaState::kReady && state != edvr::UiLayerSmaaState::kMade) {
        std::printf("  (SMAA layer pass: %s hr=0x%08X)\n", fail.what ? fail.what : "stood down", static_cast<unsigned>(fail.hr));
        return false;
    }
    ID3D11ShaderResourceView* result = edvr::uiLayerSmaaRun(s, context, srv.Get());
    if (!result || result != s.outSrv.Get()) return false;
    std::vector<unsigned char> bytes;
    if (!readWhole(context, s.out.Get(), bytes, 4) || bytes.size() != size_t(w) * h * 4) return false;
    out.resize(size_t(w) * h);
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return true;
}

// Pixels strictly between 0 and 255 in a channel.
inline unsigned between(const std::vector<Px>& px, uint8_t Px::*channel) {
    unsigned n = 0;
    for (const Px& p : px)
        if (p.*channel > 0 && p.*channel < 255) ++n;
    return n;
}
// The layer's opacity, the sum of 1 - a.
inline double opacity(const std::vector<Px>& px) {
    double sum = 0.0;
    for (const Px& p : px) sum += (255.0 - p.a) / 255.0;
    return sum;
}
inline bool same(const std::vector<Px>& a, const std::vector<Px>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(Px)) == 0;
}

// A HUD-like frame for the tiled path's sequences: a translucent panel with a border and glyph strokes, a slanted line and a dark opaque staircase, all
// shifted by `shift` pixels (content that moves between runs), on the clear. `none` leaves only the clear (content that disappears).
inline std::vector<Px> hudFrame(uint32_t w, uint32_t h, int shift, bool none) {
    std::vector<Px> px = clearLayer(w, h);
    if (none) return px;
    const auto put = [&](int x, int y, Px c) {
        if (x >= 0 && y >= 0 && x < int(w) && y < int(h)) px[size_t(y) * w + x] = c;
    };
    const Px back{6, 10, 14, 150}, line{255, 140, 20, 0}, glyph{240, 170, 60, 30}, dark{0, 0, 0, 0};
    const int x0 = 13 + shift, y0 = 9 + shift / 2;
    for (int y = y0; y < y0 + 40; ++y)
        for (int x = x0; x < x0 + 70; ++x) put(x, y, (x == x0 || y == y0 || x == x0 + 69 || y == y0 + 39) ? line : back);
    for (int g = 0; g < 9; ++g)
        for (int yy = 0; yy < 9; ++yy)
            for (int xx = 0; xx < 5; ++xx)
                if (((g * 7 + yy * 3 + xx * 5) % 4) != 0) put(x0 + 4 + g * 7 + xx, y0 + 6 + yy, glyph);
    for (int t = 0; t < 90; ++t) put(100 + t - shift, 20 + (t * 3) / 7, line);
    for (int y = 0; y < int(h); ++y)
        for (int x = 0; x < int(w); ++x)
            if (y >= 90 + shift + x / 5 && x > 120) put(x, y, dark);
    return px;
}

// Random texels in a few rectangles (`density` of 255: the share of texels set), each channel random: edges everywhere they fall.
inline std::vector<Px> randomFrame(uint32_t w, uint32_t h, uint32_t seed, unsigned density) {
    std::vector<Px> px = clearLayer(w, h);
    uint32_t s = seed * 2654435761u + 1u;
    const auto next = [&]() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    };
    for (int r = 0; r < 4; ++r) {
        const uint32_t rx = next() % w, ry = next() % h, rw = 4 + next() % 40, rh = 4 + next() % 30;
        for (uint32_t y = ry; y < ry + rh && y < h; ++y)
            for (uint32_t x = rx; x < rx + rw && x < w; ++x)
                if ((next() & 255u) < density) {
                    const uint32_t v = next();
                    px[size_t(y) * w + x] = Px{uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
                }
    }
    return px;
}

// The tiled path against the full path, run after run: `tiled` smooths its own layer (SRV and UAV) in place, `full` runs the three full passes on a copy of
// the same frame; the tiled layer must equal the full output byte for byte. `forceFull` runs the tiled instance's full passes instead (no UAV handed), which
// must still match and must restart the tiled invariant for the run after it.
struct TiledPair {
    edvr::UiLayerSmaa tiled, full;
    ComPtr<ID3D11Texture2D> layer, fullLayer;
    ComPtr<ID3D11ShaderResourceView> layerSrv, fullSrv;
    ComPtr<ID3D11UnorderedAccessView> layerUav;
    uint32_t w = 0, h = 0;
};

inline bool tiledPairMake(ID3D11Device* device, TiledPair& p, uint32_t w, uint32_t h) {
    p.w = w;
    p.h = h;
    p.layer = ::texture(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    p.fullLayer = ::texture(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    if (!p.layer || !p.fullLayer) return false;
    p.layerSrv = ::view(device, p.layer.Get());
    p.fullSrv = ::view(device, p.fullLayer.Get());
    if (FAILED(device->CreateUnorderedAccessView(p.layer.Get(), nullptr, p.layerUav.GetAddressOf()))) return false;
    edvr::UiLayerSmaaFail fail;
    return edvr::uiLayerSmaaEnsure(p.tiled, device, w, h, &fail, true) == edvr::UiLayerSmaaState::kMade && p.tiled.tiledReady() &&
           edvr::uiLayerSmaaEnsure(p.full, device, w, h, &fail) == edvr::UiLayerSmaaState::kMade && !p.full.tiledReady();
}

// One run on `frame`; true when the tiled layer equals the full output. `mismatches` counts the texels that differ.
inline bool tiledPairRun(ID3D11DeviceContext* context, TiledPair& p, const std::vector<Px>& frame, bool forceFull, unsigned* mismatches,
                         edvr::UiLayerSmaaPath* took) {
    context->UpdateSubresource(p.layer.Get(), 0, nullptr, frame.data(), p.w * 4, 0);
    context->UpdateSubresource(p.fullLayer.Get(), 0, nullptr, frame.data(), p.w * 4, 0);
    ID3D11ShaderResourceView* got = edvr::uiLayerSmaaRun(p.tiled, context, p.layerSrv.Get(), forceFull ? nullptr : p.layerUav.Get(), took);
    ID3D11ShaderResourceView* want = edvr::uiLayerSmaaRun(p.full, context, p.fullSrv.Get());
    if (!got || !want) return false;
    std::vector<unsigned char> a, b;
    ComPtr<ID3D11Resource> gotRes;
    got->GetResource(gotRes.GetAddressOf());
    ComPtr<ID3D11Texture2D> gotTex;
    gotRes.As(&gotTex);
    if (!gotTex || !readWhole(context, gotTex.Get(), a, 4) || !readWhole(context, p.full.out.Get(), b, 4) || a.size() != b.size()) return false;
    unsigned n = 0;
    for (size_t i = 0; i < a.size(); i += 4) n += std::memcmp(&a[i], &b[i], 4) != 0;
    *mismatches = n;
    return n == 0;
}

}  // namespace uilayersmaagpu

inline void uiLayerSmaaTiledGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    using namespace uilayersmaagpu;
    // Odd sizes: the last row and column of tiles are partial, and the 16x16 edge groups overhang both.
    const uint32_t sizes[2][2] = {{203, 141}, {64, 64}};
    for (const auto& sz : sizes) {
        TiledPair p;
        const bool made = tiledPairMake(device, p, sz[0], sz[1]);
        check(made, "HUD layer SMAA tiled: the tiled resources are made with the full ones, and an ensure without `tiled` makes none");
        if (!made) continue;
        struct Step {
            int kind;   // 0 HUD frame, 1 random dense, 2 random sparse, 3 the clear
            int arg;
            bool forceFull;
        };
        const Step steps[] = {{0, 0, false}, {0, 3, false}, {0, 9, false}, {3, 0, false}, {3, 0, false}, {0, 17, false}, {1, 1, false}, {1, 2, false},
                              {2, 3, false}, {0, 5, true},  {2, 4, false}, {0, 6, false}, {3, 0, false}, {1, 5, false}, {0, 0, false}};
        unsigned failed = 0, tiledRuns = 0, fullRuns = 0, worst = 0;
        for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
            const Step& s = steps[i];
            const std::vector<Px> frame = s.kind == 0   ? hudFrame(p.w, p.h, s.arg, false)
                                          : s.kind == 1 ? randomFrame(p.w, p.h, uint32_t(s.arg), 255u)
                                          : s.kind == 2 ? randomFrame(p.w, p.h, uint32_t(s.arg), 20u)
                                                        : hudFrame(p.w, p.h, 0, true);
            unsigned mismatches = ~0u;
            edvr::UiLayerSmaaPath took = edvr::UiLayerSmaaPath::kNone;
            const bool same = tiledPairRun(context, p, frame, s.forceFull, &mismatches, &took);
            if (!same) {
                ++failed;
                if (mismatches > worst) worst = mismatches;
                std::printf("  (tiled %ux%u step %zu kind %d: %u texels differ)\n", p.w, p.h, i, s.kind, mismatches);
            }
            if (took == edvr::UiLayerSmaaPath::kTiled) ++tiledRuns;
            if (took == edvr::UiLayerSmaaPath::kFull) ++fullRuns;
        }
        std::printf("  (tiled %ux%u: %u tiled runs, %u full, %u differing)\n", p.w, p.h, tiledRuns, fullRuns, failed);
        check(failed == 0, "HUD layer SMAA tiled: run after run (moving HUD content, random texels, content that disappears, a full run between), the tiled layer equals the full output byte for byte");
        check(tiledRuns == 14 && fullRuns == 1, "HUD layer SMAA tiled: the tiled path runs when the layer's view is handed, the full passes when it is not");
    }

    // The compute stage: the tiled path's extra slots (resource 3, unordered access 1 and 2) come back as the caller had them; slots 0-2 and 0 are left null.
    {
        TiledPair p;
        check(tiledPairMake(device, p, 64, 64), "HUD layer SMAA tiled: the slot check's resources are made");
        // Three marker views, each on a texture of its own (a texture bound as both a resource and an unordered-access view would lose one binding).
        ComPtr<ID3D11Texture2D> markerTex = ::texture(device, 4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_UNORDERED_ACCESS);
        ComPtr<ID3D11Texture2D> markerTex2 = ::texture(device, 4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_UNORDERED_ACCESS);
        ComPtr<ID3D11Texture2D> srvTex = ::texture(device, 4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        ComPtr<ID3D11UnorderedAccessView> markerUav1, markerUav2;
        check(SUCCEEDED(device->CreateUnorderedAccessView(markerTex.Get(), nullptr, markerUav1.GetAddressOf())) &&
                  SUCCEEDED(device->CreateUnorderedAccessView(markerTex2.Get(), nullptr, markerUav2.GetAddressOf())),
              "HUD layer SMAA tiled: the marker unordered-access views are made");
        ComPtr<ID3D11ShaderResourceView> markerSrv = ::view(device, srvTex.Get());
        ID3D11UnorderedAccessView* uavs[2] = {markerUav1.Get(), markerUav2.Get()};
        context->CSSetUnorderedAccessViews(1, 2, uavs, nullptr);
        ID3D11ShaderResourceView* srv3 = markerSrv.Get();
        context->CSSetShaderResources(3, 1, &srv3);
        unsigned mismatches = 0;
        edvr::UiLayerSmaaPath took = edvr::UiLayerSmaaPath::kNone;
        tiledPairRun(context, p, hudFrame(64, 64, 0, false), false, &mismatches, &took);
        check(took == edvr::UiLayerSmaaPath::kTiled, "HUD layer SMAA tiled: the slot check ran the tiled path");
        ID3D11ShaderResourceView* srvs[4] = {};
        ID3D11UnorderedAccessView* after[3] = {};
        context->CSGetShaderResources(0, 4, srvs);
        context->CSGetUnorderedAccessViews(0, 3, after);
        check(!srvs[0] && !srvs[1] && !srvs[2] && srvs[3] == markerSrv.Get() && !after[0] && after[1] == markerUav1.Get() && after[2] == markerUav2.Get(),
              "HUD layer SMAA tiled: resource slot 3 and unordered-access slots 1 and 2 are put back; slots 0-2 and unordered access 0 are left null");
        for (ID3D11ShaderResourceView* v : srvs)
            if (v) v->Release();
        for (ID3D11UnorderedAccessView* v : after)
            if (v) v->Release();
        ID3D11ShaderResourceView* noSrv[4] = {};
        ID3D11UnorderedAccessView* noUav[3] = {};
        context->CSSetShaderResources(0, 4, noSrv);
        context->CSSetUnorderedAccessViews(0, 3, noUav, nullptr);
        // Releasing the targets takes the tiled ones too; releasing all takes the tiled passes.
        p.tiled.releaseTargets();
        check(!p.tiled.tileList && !p.tiled.tileArgs && !p.tiled.tileMark && !p.tiled.tileState && !p.tiled.tiledLive,
              "HUD layer SMAA tiled: releasing the targets leaves no tile grid, list or arguments");
        p.tiled.releaseAll();
        check(!p.tiled.edgesMarkCs && !p.tiled.tileListCs && !p.tiled.weightsTiledCs && !p.tiled.blendTiledCs && !p.tiled.copyBackCs,
              "HUD layer SMAA tiled: releasing all leaves no tiled pass");
    }
}

inline void uiLayerSmaaGpuTests(ID3D11Device* device, ID3D11DeviceContext* context) {
    using namespace uilayersmaagpu;
    uiLayerSmaaTiledGpuTests(device, context);
    const uint32_t n = kSize;
    const Px white{255, 255, 255, 0}, dark{0, 0, 0, 0};
    std::vector<Px> out;

    {
        edvr::UiLayerSmaa s;
        // (a) the white staircase.
        const std::vector<Px> in = staircase(n, n, white);
        const bool ran = smooth(device, context, s, in, n, n, out);
        check(ran, "HUD layer SMAA: the passes run on a 64x64 layer");
        if (ran) {
            const unsigned rgb = between(out, &Px::r), alpha = between(out, &Px::a);
            const double before = opacity(in), after = opacity(out);
            std::printf("  (white staircase: %u pixels between in r, %u in a; opacity %.2f -> %.2f)\n", rgb, alpha, before, after);
            check(rgb >= 24 && alpha >= 24, "HUD layer SMAA: a shallow white staircase takes intermediate values along its steps, in rgb and in alpha");
            check(std::abs(after - before) <= 0.03 * before, "HUD layer SMAA: the white staircase's summed opacity holds within 3%");
            check(!same(out, in), "HUD layer SMAA: the staircase is changed");
            // The pixels away from the steps are what they were.
            check(out[size_t(0) * n + 0].r == 0 && out[size_t(0) * n + 0].a == 255 && out[size_t(n - 1) * n + (n - 1)].r == 255 &&
                      out[size_t(n - 1) * n + (n - 1)].a == 0,
                  "HUD layer SMAA: the clear and the opaque fill, away from the steps, are as they were");
        }

        // (b) the dark staircase: the edge is in alpha alone.
        const std::vector<Px> darkIn = staircase(n, n, dark);
        const bool darkRan = smooth(device, context, s, darkIn, n, n, out);
        check(darkRan, "HUD layer SMAA: the passes run on the dark staircase");
        if (darkRan) {
            const unsigned alpha = between(out, &Px::a);
            unsigned colour = 0;
            for (const Px& p : out) colour += p.r + p.g + p.b;
            const double before = opacity(darkIn), after = opacity(out);
            std::printf("  (dark staircase: %u pixels between in a; rgb sum %u; opacity %.2f -> %.2f)\n", alpha, colour, before, after);
            check(alpha >= 24, "HUD layer SMAA: a dark opaque staircase takes intermediate alpha along its steps (the four-channel edge signal)");
            check(colour == 0, "HUD layer SMAA: the dark staircase's rgb stays 0");
            check(std::abs(after - before) <= 0.03 * before, "HUD layer SMAA: the dark staircase's summed opacity holds within 3%");

            // The control: the map's colour-only edge pass in the edge pass's place. Dark against the clear is no edge in rgb, so the same weights and
            // blend leave the layer as it was. This is what the four-channel pass is for.
            ComPtr<ID3D11ComputeShader> colourOnly;
            check(SUCCEEDED(device->CreateComputeShader(edvr::kFlatMapSmaaEdgesBytecode, sizeof(edvr::kFlatMapSmaaEdgesBytecode), nullptr,
                                                        colourOnly.GetAddressOf())),
                  "HUD layer SMAA: the map's colour-only edge shader is made for the control");
            if (colourOnly) {
                edvr::UiLayerSmaa control;
                edvr::UiLayerSmaaFail fail;
                check(edvr::uiLayerSmaaEnsure(control, device, n, n, &fail) == edvr::UiLayerSmaaState::kMade, "HUD layer SMAA: the control's targets are made");
                control.edgesCs = colourOnly;
                std::vector<Px> controlOut;
                ComPtr<ID3D11Texture2D> layer = ::texture(device, n, n, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, darkIn.data(), n * 4);
                ComPtr<ID3D11ShaderResourceView> srv = ::view(device, layer.Get());
                std::vector<unsigned char> bytes;
                if (edvr::uiLayerSmaaRun(control, context, srv.Get()) && readWhole(context, control.out.Get(), bytes, 4)) {
                    controlOut.resize(size_t(n) * n);
                    std::memcpy(controlOut.data(), bytes.data(), bytes.size());
                }
                check(same(controlOut, darkIn), "HUD layer SMAA: control, the colour-only edge pass leaves the dark staircase untouched (so the alpha channel in the edge test is what smooths it)");
            }
        }

        // (c) the all-clear layer, through the same resources.
        const std::vector<Px> clear = clearLayer(n, n);
        const bool clearRan = smooth(device, context, s, clear, n, n, out);
        check(clearRan, "HUD layer SMAA: the passes run on an all-clear layer");
        if (clearRan) {
            bool exact = true;
            for (const Px& p : out) exact = exact && p.r == 0 && p.g == 0 && p.b == 0 && p.a == 255;
            check(exact, "HUD layer SMAA: an all-clear layer comes out exactly (0, 0, 0, 1) everywhere");
        }

        // (d) straight bands across the whole layer.
        for (int pass = 0; pass < 4; ++pass) {
            const bool horizontal = (pass & 1) == 0;
            const Px fill = pass < 2 ? white : dark;
            const std::vector<Px> bandIn = band(n, n, horizontal, 24, 39, fill);
            const bool ok = smooth(device, context, s, bandIn, n, n, out);
            check(ok && same(out, bandIn),
                  pass == 0   ? "HUD layer SMAA: a full-width horizontal white band comes out unchanged"
                  : pass == 1 ? "HUD layer SMAA: a full-height vertical white band comes out unchanged"
                  : pass == 2 ? "HUD layer SMAA: a full-width horizontal dark band comes out unchanged"
                              : "HUD layer SMAA: a full-height vertical dark band comes out unchanged");
        }
    }

    // The compute stage: the two samplers come back, the resource slots and the unordered-access slot a pass used are left empty.
    {
        edvr::UiLayerSmaa s;
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> marker0, marker1;
        check(SUCCEEDED(device->CreateSamplerState(&sd, marker0.GetAddressOf())), "HUD layer SMAA: the first marker sampler is made");
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
        check(SUCCEEDED(device->CreateSamplerState(&sd, marker1.GetAddressOf())), "HUD layer SMAA: the second marker sampler is made");
        ID3D11SamplerState* markers[2] = {marker0.Get(), marker1.Get()};
        context->CSSetSamplers(0, 2, markers);
        const bool ran = smooth(device, context, s, staircase(n, n, white), n, n, out);
        check(ran, "HUD layer SMAA: the passes run with the caller's samplers bound");
        ID3D11SamplerState* after[2] = {};
        context->CSGetSamplers(0, 2, after);
        check(after[0] == marker0.Get() && after[1] == marker1.Get(), "HUD layer SMAA: the caller's samplers on s0 and s1 are put back");
        for (ID3D11SamplerState* sampler : after)
            if (sampler) sampler->Release();
        ID3D11ShaderResourceView* srvs[3] = {};
        ID3D11UnorderedAccessView* uav = nullptr;
        context->CSGetShaderResources(0, 3, srvs);
        context->CSGetUnorderedAccessViews(0, 1, &uav);
        check(!srvs[0] && !srvs[1] && !srvs[2] && !uav, "HUD layer SMAA: no resource view and no unordered-access view is left bound on the compute stage");
        for (ID3D11ShaderResourceView* view : srvs)
            if (view) view->Release();
        if (uav) uav->Release();
        ID3D11SamplerState* none[2] = {};
        context->CSSetSamplers(0, 2, none);
    }

    // The sizes: the targets follow the layer, the same size is left alone, and what failed at a size is told once and not tried again there.
    {
        edvr::UiLayerSmaa s;
        edvr::UiLayerSmaaFail fail;
        check(edvr::uiLayerSmaaEnsure(s, device, 64, 64, &fail) == edvr::UiLayerSmaaState::kMade && s.w == 64 && s.h == 64, "HUD layer SMAA: the first use makes the targets");
        ID3D11Texture2D* first = s.out.Get();
        check(edvr::uiLayerSmaaEnsure(s, device, 64, 64, &fail) == edvr::UiLayerSmaaState::kReady && s.out.Get() == first,
              "HUD layer SMAA: the same size leaves the targets as they are");
        check(edvr::uiLayerSmaaEnsure(s, device, 48, 40, &fail) == edvr::UiLayerSmaaState::kMade && s.w == 48 && s.h == 40, "HUD layer SMAA: a new size re-makes the targets");
        D3D11_TEXTURE2D_DESC od{}, ed{}, wd{};
        s.out->GetDesc(&od);
        s.edges->GetDesc(&ed);
        s.weights->GetDesc(&wd);
        check(od.Width == 48 && od.Height == 40 && od.Format == DXGI_FORMAT_R8G8B8A8_UNORM && ed.Width == 48 && ed.Height == 40 &&
                  ed.Format == DXGI_FORMAT_R8G8_UNORM && wd.Width == 48 && wd.Height == 40 && wd.Format == DXGI_FORMAT_R8G8B8A8_UNORM,
              "HUD layer SMAA: the output and weights are R8G8B8A8 and the edges R8G8, all at the layer's size");
        s.releaseTargets();
        check(!s.ready() && !s.out && !s.edges && !s.weights && !s.metrics, "HUD layer SMAA: releasing the targets leaves none");
        check(s.edgesCs && s.weightsCs && s.blendCs && s.areaSrv && s.searchSrv && s.linear && s.point, "HUD layer SMAA: the passes, samplers and lookups stay with the device");
        s.releaseAll();
        check(!s.edgesCs && !s.weightsCs && !s.blendCs && !s.areaSrv && !s.searchSrv && !s.linear && !s.point && !s.area && !s.search, "HUD layer SMAA: releasing all leaves nothing");

        // A creation that fails (no device to make it on) is told once for the size, and not tried again there, even with a device.
        edvr::UiLayerSmaaFail told;
        check(edvr::uiLayerSmaaEnsure(s, nullptr, 96, 96, &told) == edvr::UiLayerSmaaState::kFailed && told.what && FAILED(told.hr),
              "HUD layer SMAA: a creation that fails at a size answers kFailed with what failed and the HRESULT");
        edvr::UiLayerSmaaFail again;
        check(edvr::uiLayerSmaaEnsure(s, device, 96, 96, &again) == edvr::UiLayerSmaaState::kStoodDown && !again.what && again.hr == S_OK && !s.ready(),
              "HUD layer SMAA: the next call at that size stands down without trying again and without saying it twice");
        s.releaseTargets();
        check(edvr::uiLayerSmaaEnsure(s, device, 96, 96, &again) == edvr::UiLayerSmaaState::kStoodDown,
              "HUD layer SMAA: releasing the layer's targets does not clear what failed at a size");
        check(edvr::uiLayerSmaaEnsure(s, device, 80, 80, &again) == edvr::UiLayerSmaaState::kMade && s.ready(), "HUD layer SMAA: another size tries again and succeeds");
        s.releaseAll();
        check(edvr::uiLayerSmaaEnsure(s, device, 96, 96, &again) == edvr::UiLayerSmaaState::kMade,
              "HUD layer SMAA: a device change (release all) clears what failed, so the size is tried afresh");
    }
}

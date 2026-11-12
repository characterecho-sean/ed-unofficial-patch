#pragma once

// Pixel-level WARP gate for the own-discard variant (diag.hull_layer = separate, TEMPORARY;
// docs\research-own-hull-layer-2026-11-08.md "Step 2 build"): the engine-motion patched pixel shader that also reads the
// hull layer's own mask at t127 (one uint a pool record) and discards a pixel whose record is the own ship's.
//  - Synthetic: four instances, four strips, four slots. Slots 7 and 9 are own; 3 is not; 1000 is past the mask's end
//    (not own). Every pixel of a non-own strip writes SV_Target0..3, depth and MRT6 bit-identically to the normal patched
//    shader; every pixel of an own strip keeps the targets', the depth's and MRT6's clears. The same with the guarded
//    decal variant (t3 snapshot) underneath. A shader that names t127, or forces the early depth test, is refused.
//  - Real: the two hull-family pixel shaders kept in the repository (66DE + BBDE4E71, AACF + CF534B32,
//    tools\flat_temporal_test\fixtures) through corpus_identity::ownDiscard.
#include <fstream>

#include "corpus_identity.h"
#include "shader_tests.h"

namespace own_discard_gpu_tests {
using Microsoft::WRL::ComPtr;

constexpr UINT kWidth = 32, kHeight = 16;
constexpr uint32_t kSlots[4] = {3u, 7u, 1000u, 9u};   // strip i draws record kSlots[i]; 7 and 9 are own
constexpr char kVs[] = R"HLSL(
struct Out { nointerpolation uint id : __USER_VERTEX_FACEINVARIANT; float4 p : SV_Position; };
static const uint kSlots[4] = {3u, 7u, 1000u, 9u};
Out main(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    Out o;
    const float2 c[6] = {float2(0,0), float2(0,1), float2(1,0), float2(1,0), float2(0,1), float2(1,1)};
    float2 q = c[vertex];
    o.p = float4(-1 + (instance + q.x) * 0.5, q.y * 2 - 1, 0.25, 1);
    o.id = 0x80000000u | kSlots[instance];
    return o;
}
)HLSL";
constexpr char kPs[] = R"HLSL(
struct In { nointerpolation uint id : __USER_VERTEX_FACEINVARIANT; float4 p : SV_Position; };
struct Out { float4 a : SV_Target0; float4 b : SV_Target1; float4 c : SV_Target2; float4 d : SV_Target3; };
Out main(In i) {
    Out o;
    float r = frac(i.p.x * 0.173 + i.p.y * 0.311);
    o.a = float4(frac(i.p.x / 32), frac(i.p.y / 16), ddx(r * r), 1);
    o.b = float4((i.id & 15u) / 15.0, 0.5, ddy(r * i.p.x), 1);
    o.c = float4(i.p.z, frac(i.p.x / 8), frac(i.p.y / 8), 1);
    o.d = float4(0.125, 0.375, 0.625, 1);
    return o;
}
)HLSL";
constexpr char kPsOccupied[] = R"HLSL(
ByteAddressBuffer busy : register(t127);
struct In { nointerpolation uint id : __USER_VERTEX_FACEINVARIANT; float4 p : SV_Position; };
float4 main(In i) : SV_Target0 { return float4(busy.Load(0), i.id & 1u, 0, 1); }
)HLSL";
constexpr char kPsEarly[] = R"HLSL(
struct In { nointerpolation uint id : __USER_VERTEX_FACEINVARIANT; float4 p : SV_Position; };
[earlydepthstencil] float4 main(In i) : SV_Target0 { return float4(i.p.x, i.id & 1u, 0, 1); }
)HLSL";

struct Frame { std::vector<BYTE> game[4], depth, slots; };

inline std::vector<BYTE> read(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* source, UINT bytesPerPixel) {
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&desc, nullptr, &staging))) return {};
    ctx->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return {};
    std::vector<BYTE> result(size_t(kWidth) * kHeight * bytesPerPixel);
    for (UINT y = 0; y < kHeight; ++y)
        std::memcpy(result.data() + size_t(y) * kWidth * bytesPerPixel, static_cast<const BYTE*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                    size_t(kWidth) * bytesPerPixel);
    ctx->Unmap(staging.Get(), 0);
    return result;
}

// Four instanced strips into RT0-3 (R32G32B32A32, cleared to -1), MRT6 (R32G32, cleared to (-1, 0)) and a D32 depth
// cleared to 0, tested GEQUAL with writes on; t3 the snapshot (guarded), t127 the mask.
inline bool draw(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11VertexShader* vs, ID3D11PixelShader* ps, ID3D11ShaderResourceView* snapshot,
                 ID3D11ShaderResourceView* mask, Frame& result) {
    ctx->ClearState();
    ComPtr<ID3D11Texture2D> targets[7];
    ComPtr<ID3D11RenderTargetView> rtv[7];
    ID3D11RenderTargetView* bound[7]{};
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kWidth;
    td.Height = kHeight;
    td.MipLevels = td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    const float clear[4] = {-1, 0, 0, 0};
    for (UINT i = 0; i < 7; ++i) {
        if (i == 4 || i == 5) continue;
        td.Format = i == 6 ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &targets[i])) || FAILED(dev->CreateRenderTargetView(targets[i].Get(), nullptr, &rtv[i])))
            return false;
        ctx->ClearRenderTargetView(rtv[i].Get(), clear);
        bound[i] = rtv[i].Get();
    }
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11DepthStencilView> dsv;
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd{};
    dvd.Format = DXGI_FORMAT_D32_FLOAT;
    dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &depth)) || FAILED(dev->CreateDepthStencilView(depth.Get(), &dvd, &dsv))) return false;
    ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0, 0);
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    ComPtr<ID3D11DepthStencilState> depthState;
    if (FAILED(dev->CreateDepthStencilState(&ds, &depthState))) return false;
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rasterState;
    if (FAILED(dev->CreateRasterizerState(&raster, &rasterState))) return false;
    const D3D11_VIEWPORT vp{0, 0, float(kWidth), float(kHeight), 0, 1};
    ctx->OMSetRenderTargets(7, bound, dsv.Get());
    ctx->OMSetDepthStencilState(depthState.Get(), 0);
    ctx->RSSetState(rasterState.Get());
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    if (snapshot) ctx->PSSetShaderResources(edvr::kEngineVelocityOverlaySnapshotSlot, 1, &snapshot);
    if (mask) ctx->PSSetShaderResources(edvr::kEngineVelocityOwnMaskSlot, 1, &mask);
    ctx->DrawInstanced(6, 4, 0, 0);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    for (UINT i = 0; i < 4; ++i) result.game[i] = read(dev, ctx, targets[i].Get(), 16);
    result.depth = read(dev, ctx, depth.Get(), 4);
    result.slots = read(dev, ctx, targets[6].Get(), 8);
    return !result.game[0].empty() && !result.depth.empty() && !result.slots.empty();
}

inline ComPtr<ID3D11ShaderResourceView> rawMask(ID3D11Device* dev, const std::vector<uint32_t>& words) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = UINT(words.size() * 4);
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_SUBRESOURCE_DATA init{words.data(), 0, 0};
    ComPtr<ID3D11Buffer> buf;
    ComPtr<ID3D11ShaderResourceView> srv;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sd.BufferEx.NumElements = UINT(words.size());
    sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    if (FAILED(dev->CreateBuffer(&bd, &init, &buf)) || FAILED(dev->CreateShaderResourceView(buf.Get(), &sd, &srv))) return nullptr;
    return srv;
}

// Per pixel: a non-own strip's pixel equals the reference in RT0-3, depth and MRT6; an own strip's keeps the clears.
// Returns false (with counts printed) on any difference.
inline bool compareStrips(const Frame& reference, const Frame& variant, const char* label, unsigned* keptOut, unsigned* goneOut) {
    unsigned kept = 0, keptBad = 0, gone = 0, goneBad = 0;
    const float clear4[4] = {-1, 0, 0, 0};
    const float clear2[2] = {-1, 0};
    const float zero = 0.0f;
    for (UINT y = 0; y < kHeight; ++y)
        for (UINT x = 0; x < kWidth; ++x) {
            const size_t p = size_t(y) * kWidth + x;
            const uint32_t slot = kSlots[x / 8];
            const bool own = slot == 7u || slot == 9u;
            bool same = true, cleared = true;
            for (UINT t = 0; t < 4; ++t) {
                same = same && std::memcmp(&variant.game[t][p * 16], &reference.game[t][p * 16], 16) == 0;
                cleared = cleared && std::memcmp(&variant.game[t][p * 16], clear4, 16) == 0;
            }
            same = same && std::memcmp(&variant.depth[p * 4], &reference.depth[p * 4], 4) == 0 &&
                   std::memcmp(&variant.slots[p * 8], &reference.slots[p * 8], 8) == 0;
            cleared = cleared && std::memcmp(&variant.depth[p * 4], &zero, 4) == 0 && std::memcmp(&variant.slots[p * 8], clear2, 8) == 0;
            if (own) { ++gone; goneBad += cleared ? 0u : 1u; }
            else { ++kept; keptBad += same ? 0u : 1u; }
        }
    std::printf("  own discard (%s): %u non-own pixels, %u differ from the patched shader; %u own pixels, %u written\n", label, kept, keptBad, gone,
                goneBad);
    if (keptOut) *keptOut = kept;
    if (goneOut) *goneOut = gone;
    return keptBad == 0 && goneBad == 0 && kept > 0 && gone > 0;
}

inline std::vector<BYTE> blobBytes(ID3DBlob* b) {
    return std::vector<BYTE>(static_cast<const BYTE*>(b->GetBufferPointer()), static_cast<const BYTE*>(b->GetBufferPointer()) + b->GetBufferSize());
}

inline std::vector<BYTE> readFixture(const char* name) {
    std::ifstream f(std::string("tools\\flat_temporal_test\\fixtures\\") + name, std::ios::binary);
    if (!f) return {};
    return std::vector<BYTE>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

inline void run(ID3D11Device* dev, ID3D11DeviceContext* ctx, void (*check)(bool, const char*)) {
    const shader_tests::Harness h{dev, ctx, check};
    auto vsBlob = shader_tests::compile(h, kVs, "vs_5_0");
    auto psBlob = shader_tests::compile(h, kPs, "ps_5_0");
    if (!vsBlob || !psBlob) return;
    const std::vector<BYTE> psBytes = blobBytes(psBlob.Get());
    edvr::EngineVelocityInputs inputs{};
    inputs.identityRegister = 0;
    inputs.identityComponent = 0;
    inputs.positionRegister = 1;
    std::string reason;
    std::vector<BYTE> normal, discard, guarded, guardedDiscard;
    check(edvr::engineVelocityPatchPs(psBytes.data(), psBytes.size(), inputs, normal, reason), "own discard: the synthetic PS patches");
    check(edvr::engineVelocityPatchPs(psBytes.data(), psBytes.size(), inputs, discard, reason, false,
                                      edvr::dxbc_engine_velocity_detail::FlatMarkerKind::None, nullptr, true),
          "own discard: the synthetic PS takes the own discard");
    check(edvr::engineVelocityPatchPs(psBytes.data(), psBytes.size(), inputs, guarded, reason, true), "own discard: the synthetic PS takes the guard");
    check(edvr::engineVelocityPatchPs(psBytes.data(), psBytes.size(), inputs, guardedDiscard, reason, true,
                                      edvr::dxbc_engine_velocity_detail::FlatMarkerKind::None, nullptr, true),
          "own discard: the synthetic PS takes the guard and the own discard together");
    if (normal.empty() || discard.empty() || guarded.empty() || guardedDiscard.empty()) {
        std::printf("  own discard: the patch said: %s\n", reason.c_str());
        return;
    }
    {
        std::vector<BYTE> refused;
        check(!edvr::engineVelocityPatchPs(psBytes.data(), psBytes.size(), inputs, refused, reason, false,
                                           edvr::dxbc_engine_velocity_detail::FlatMarkerKind::ForeignPool, nullptr, true) &&
                  reason.find("own discard") != std::string::npos,
              "own discard: refused with a flat marker");
        auto occupied = shader_tests::compile(h, kPsOccupied, "ps_5_0");
        if (occupied) {
            const auto b = blobBytes(occupied.Get());
            check(!edvr::engineVelocityPatchPs(b.data(), b.size(), inputs, refused, reason, false, edvr::dxbc_engine_velocity_detail::FlatMarkerKind::None,
                                               nullptr, true) &&
                      reason.find("t127 occupied") != std::string::npos,
                  "own discard: a shader that names t127 is refused");
            check(edvr::engineVelocityPatchPs(b.data(), b.size(), inputs, refused, reason), "own discard: ...and still takes the normal patch");
        }
        auto early = shader_tests::compile(h, kPsEarly, "ps_5_0");
        if (early) {
            const auto b = blobBytes(early.Get());
            check(!edvr::engineVelocityPatchPs(b.data(), b.size(), inputs, refused, reason, false, edvr::dxbc_engine_velocity_detail::FlatMarkerKind::None,
                                               nullptr, true) &&
                      reason.find("early depth") != std::string::npos,
                  "own discard: a shader that forces the early depth test is refused");
        }
    }
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> normalPs, discardPs, guardedPs, guardedDiscardPs;
    check(SUCCEEDED(dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs)) &&
              SUCCEEDED(dev->CreatePixelShader(normal.data(), normal.size(), nullptr, &normalPs)) &&
              SUCCEEDED(dev->CreatePixelShader(discard.data(), discard.size(), nullptr, &discardPs)) &&
              SUCCEEDED(dev->CreatePixelShader(guarded.data(), guarded.size(), nullptr, &guardedPs)) &&
              SUCCEEDED(dev->CreatePixelShader(guardedDiscard.data(), guardedDiscard.size(), nullptr, &guardedDiscardPs)),
          "own discard: the four synthetic shaders create on WARP");
    if (!vs || !normalPs || !discardPs || !guardedPs || !guardedDiscardPs) return;
    // The mask: 16 records, 7 and 9 own (1000 is past its end: a raw load there reads 0).
    std::vector<uint32_t> words(16, 0u);
    words[7] = 1u;
    words[9] = 1u;
    const auto mask = rawMask(dev, words);
    const auto noneOwn = rawMask(dev, std::vector<uint32_t>(16, 0u));
    // The guard's snapshot: every texel names record 3 at the substrate depth 0.5 (so the guard picks 0.5 there).
    struct Pair { float code, depth; };
    std::vector<Pair> texels(size_t(kWidth) * kHeight, Pair{7.0f, 0.5f});
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kWidth;
    td.Height = kHeight;
    td.MipLevels = td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R32G32_FLOAT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{texels.data(), UINT(kWidth * sizeof(Pair)), 0};
    ComPtr<ID3D11Texture2D> snapshot;
    ComPtr<ID3D11ShaderResourceView> snapshotView;
    check(mask && noneOwn && SUCCEEDED(dev->CreateTexture2D(&td, &init, &snapshot)) &&
              SUCCEEDED(dev->CreateShaderResourceView(snapshot.Get(), nullptr, &snapshotView)),
          "own discard: the masks and the guard's snapshot created");
    if (!mask || !noneOwn || !snapshotView) return;
    Frame ref, var, varNone, gref, gvar;
    check(draw(dev, ctx, vs.Get(), normalPs.Get(), nullptr, nullptr, ref) && draw(dev, ctx, vs.Get(), discardPs.Get(), nullptr, mask.Get(), var) &&
              draw(dev, ctx, vs.Get(), discardPs.Get(), nullptr, noneOwn.Get(), varNone) &&
              draw(dev, ctx, vs.Get(), guardedPs.Get(), snapshotView.Get(), nullptr, gref) &&
              draw(dev, ctx, vs.Get(), guardedDiscardPs.Get(), snapshotView.Get(), mask.Get(), gvar),
          "own discard: the five synthetic draws read back");
    // The reference must itself have drawn the slots it claims (2 * slot + 1) on every strip.
    unsigned refOk = 0;
    for (UINT x = 0; x < kWidth; ++x) {
        float s[2];
        std::memcpy(s, &ref.slots[(size_t(5) * kWidth + x) * 8], 8);
        refOk += s[0] == float(2u * kSlots[x / 8] + 1u) && s[1] == 0.25f ? 1u : 0u;
    }
    check(refOk == kWidth, "own discard: the normal patched shader wrote 2 * slot + 1 at MRT6 on every strip (the reference is not vacuous)");
    unsigned kept = 0, gone = 0;
    check(compareStrips(ref, var, "plain", &kept, &gone), "own discard: non-own pixels bit-identical, own pixels discarded (plain)");
    check(kept == 256 && gone == 256, "own discard: two strips kept, two discarded (plain)");
    check(ref.game[0] == varNone.game[0] && ref.game[1] == varNone.game[1] && ref.game[2] == varNone.game[2] && ref.game[3] == varNone.game[3] &&
              ref.depth == varNone.depth && ref.slots == varNone.slots,
          "own discard: with no record own the variant's frame is the patched shader's, bit for bit");
    check(compareStrips(gref, gvar, "guarded decal", nullptr, nullptr),
          "own discard: non-own pixels bit-identical to the guarded variant, own pixels discarded (guarded decal)");
    // The real hull-family pixel shaders kept in the repository.
    struct Fixture { const char* vs; const char* ps; const char* name; };
    const Fixture fixtures[] = {{"vs_66DE2CADB1F4AE6B.dxbc", "ps_BBDE4E71FB78528A.dxbc", "vs_66DE + ps_BBDE4E71"},
                                {"vs_AACFDCF2FB9AD809.dxbc", "ps_CF534B32F491561A.dxbc", "vs_AACF + ps_CF534B32"}};
    for (const auto& fx : fixtures) {
        const auto rvs = readFixture(fx.vs);
        const auto rps = readFixture(fx.ps);
        check(!rvs.empty() && !rps.empty(), "own discard: the real hull fixtures are present (tools\\flat_temporal_test\\fixtures; run from the repo root)");
        if (rvs.empty() || rps.empty()) continue;
        edvr::EngineVelocityInputs in;
        std::string why;
        const bool derived = edvr::engineVelocityDeriveInputs(rvs.data(), rvs.size(), in, why);
        check(derived, "own discard: the real hull VS derives its inputs");
        if (!derived) continue;
        std::vector<BYTE> pps, dps;
        const bool p1 = edvr::engineVelocityPatchPs(rps.data(), rps.size(), in, pps, why);
        const bool p2 = p1 && edvr::engineVelocityPatchPs(rps.data(), rps.size(), in, dps, why, false,
                                                          edvr::dxbc_engine_velocity_detail::FlatMarkerKind::None, nullptr, true);
        if (!p2) std::printf("  own discard: %s: %s\n", fx.name, why.c_str());
        check(p1 && p2, "own discard: the real hull PS takes the patch and the own discard");
        if (!p2) continue;
        const corpus_identity::Result r = corpus_identity::ownDiscard(dev, ctx, rps, pps, dps, in, check, fx.name);
        check(r.driven, "own discard: the real hull PS was driven (not skipped)");
    }
    ctx->ClearState();
}
}  // namespace own_discard_gpu_tests

#pragma once

// WARP gate for the keep-own vertex shader (diag.hull_layer, TEMPORARY; docs\research-own-hull-layer-2026-11-08.md
// "Station fixes"): a pool family's vertex shader that also reads the hull layer's own mask at VS t127 (one uint a pool
// record) and puts every vertex of a foreign instance outside the clip volume, so the layer's re-issue draws only the own
// ship's instances and needs no neighbour filter.
//  - Synthetic (a family-shaped VS: INSTANCEANDMODELDATAINDEX at v0, the t33 pool read by v0.x, the slot exported): four
//    instances, four strips, slots 3, 7, 1000 (past the mask's end) and 9, of which 7 and 9 are own. Every pixel of an own
//    strip writes the target and depth bit-identically to the plain VS; every pixel of a foreign strip keeps the clears.
//    With no record own nothing is drawn. Stream output: an own instance's every output, every vertex, bit-identical to the
//    plain VS; a foreign instance's SV_Position is the culled point. A VS that names t127 is refused.
//  - Real: the two hull-family vertex shaders kept in the repository (vs_66DE, vs_AACF; tools\flat_temporal_test\fixtures),
//    driven with a layout from their own input signature and patterned resources: the same stream-output proof.
//    --corpus runs the same on every hull-family and decal VS of a dump (keepOwnStreamCheck).
#include <fstream>

#include "corpus_identity.h"
#include "shader_tests.h"

namespace keep_own_vs_tests {
using Microsoft::WRL::ComPtr;

constexpr UINT kWidth = 32, kHeight = 16, kInstances = 4;
constexpr uint32_t kSlots[kInstances] = {3u, 7u, 1000u, 9u};   // instance i draws record kSlots[i]; 7 and 9 are own
inline bool ownSlot(uint32_t s) { return s == 7u || s == 9u; }

constexpr char kVs[] = R"HLSL(
struct Rec { float4 a[21]; };
StructuredBuffer<Rec> pool : register(t33);
struct In { uint2 id : INSTANCEANDMODELDATAINDEX; uint vertex : SV_VertexID; uint instance : SV_InstanceID; };
struct Out { nointerpolation uint id : __USER_VERTEX_FACEINVARIANT; float4 p : SV_Position; float extra : EXTRA; };
Out main(In i) {
    Out o;
    const float2 c[6] = {float2(0,0), float2(0,1), float2(1,0), float2(1,0), float2(0,1), float2(1,1)};
    float2 q = c[i.vertex];
    o.p = float4(-1 + (i.instance + q.x) * 0.5, q.y * 2 - 1, 0.25 + 0.5 * q.x * q.y, 1);
    o.id = 0x80000000u | i.id.x;
    o.extra = pool[i.id.x].a[1].x;
    return o;
}
)HLSL";
constexpr char kPs[] = R"HLSL(
struct In { nointerpolation uint id : __USER_VERTEX_FACEINVARIANT; float4 p : SV_Position; float extra : EXTRA; };
float4 main(In i) : SV_Target0 { return float4(i.p.x * 0.031, i.p.z, i.extra, float(i.id & 0xffffu)); }
)HLSL";
constexpr char kVsOccupied[] = R"HLSL(
ByteAddressBuffer busy : register(t127);
struct In { uint2 id : INSTANCEANDMODELDATAINDEX; uint vertex : SV_VertexID; };
float4 main(In i) : SV_Position { return float4(busy.Load(0), i.id.x, i.vertex, 1); }
)HLSL";

inline std::vector<BYTE> blobBytes(ID3DBlob* b) {
    return std::vector<BYTE>(static_cast<const BYTE*>(b->GetBufferPointer()), static_cast<const BYTE*>(b->GetBufferPointer()) + b->GetBufferSize());
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

inline ComPtr<ID3D11Buffer> buffer(ID3D11Device* dev, const void* data, UINT bytes, UINT bind) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init{data, 0, 0};
    ComPtr<ID3D11Buffer> b;
    if (FAILED(dev->CreateBuffer(&bd, &init, &b))) return nullptr;
    return b;
}

inline std::vector<BYTE> readBuffer(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Buffer* source, UINT bytes) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &staging))) return {};
    ctx->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return {};
    std::vector<BYTE> out(static_cast<const BYTE*>(m.pData), static_cast<const BYTE*>(m.pData) + bytes);
    ctx->Unmap(staging.Get(), 0);
    return out;
}

// The VS's every output, every vertex of every instance, by stream output (a pass-through GS made from the VS bytecode
// itself, the D3D11 way to stream a VS out). The caller has bound the input layout, buffers and resources. `posOffset` is
// SV_Position's byte offset in a vertex of `stride` bytes.
inline bool streamOut(ID3D11Device* dev, ID3D11DeviceContext* ctx, const std::vector<BYTE>& code, UINT vertices, UINT instances, std::vector<BYTE>& out,
                      UINT& stride, UINT& posOffset, std::string& why) {
    ComPtr<ID3D11ShaderReflection> r;
    D3D11_SHADER_DESC desc{};
    if (!corpus_identity::detail::reflect(code.data(), code.size(), r, desc)) { why = "the VS does not reflect"; return false; }
    std::vector<D3D11_SO_DECLARATION_ENTRY> entries;
    std::vector<std::string> names;
    names.reserve(desc.OutputParameters);
    stride = 0;
    posOffset = ~0u;
    for (UINT i = 0; i < desc.OutputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC p{};
        r->GetOutputParameterDesc(i, &p);
        if (!p.Mask) continue;
        BYTE first = 0, last = 0;
        for (BYTE c = 0; c < 4; ++c)
            if (p.Mask & (1u << c)) { if (!(p.Mask & ((1u << c) - 1u))) first = c; last = c; }
        names.push_back(p.SemanticName);
        D3D11_SO_DECLARATION_ENTRY e{};
        e.Stream = 0;
        e.SemanticName = names.back().c_str();
        e.SemanticIndex = p.SemanticIndex;
        e.StartComponent = first;
        e.ComponentCount = static_cast<BYTE>(last - first + 1);
        e.OutputSlot = 0;
        if (p.SystemValueType == D3D_NAME_POSITION) {
            if (first != 0 || last != 3) { why = "SV_Position is not xyzw"; return false; }
            posOffset = stride;
        }
        entries.push_back(e);
        stride += 4u * e.ComponentCount;
    }
    if (posOffset == ~0u || entries.empty()) { why = "no SV_Position output"; return false; }
    ComPtr<ID3D11GeometryShader> gs;
    if (FAILED(dev->CreateGeometryShaderWithStreamOutput(code.data(), code.size(), entries.data(), UINT(entries.size()), &stride, 1,
                                                         D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &gs))) {
        why = "the stream-output GS could not be made from the VS";
        return false;
    }
    ComPtr<ID3D11VertexShader> vs;
    if (FAILED(dev->CreateVertexShader(code.data(), code.size(), nullptr, &vs))) { why = "the VS does not create"; return false; }
    const UINT bytes = stride * vertices * instances;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    ComPtr<ID3D11Buffer> so;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &so))) { why = "the stream-output buffer could not be made"; return false; }
    const UINT zero = 0;
    ID3D11Buffer* target = so.Get();
    ctx->SOSetTargets(1, &target, &zero);
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->GSSetShader(gs.Get(), nullptr, 0);
    ctx->PSSetShader(nullptr, nullptr, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ctx->DrawInstanced(vertices, instances, 0, 0);
    ID3D11Buffer* none = nullptr;
    ctx->SOSetTargets(1, &none, &zero);
    ctx->GSSetShader(nullptr, nullptr, 0);
    out = readBuffer(dev, ctx, so.Get(), bytes);
    if (out.size() != bytes) { why = "the stream output did not read back"; return false; }
    return true;
}

// Plain against keep-own, both streamed out: an own instance's vertices bit for bit, a foreign instance's SV_Position the
// culled point. `own[i]` says which instance is own.
inline bool compareStreams(const std::vector<BYTE>& plain, const std::vector<BYTE>& keep, UINT stride, UINT posOffset, UINT vertices, const bool* own,
                           UINT instances, unsigned* ownVertices, unsigned* culledVertices, const char* label) {
    unsigned ownSame = 0, ownBad = 0, culled = 0, notCulled = 0;
    for (UINT i = 0; i < instances; ++i)
        for (UINT v = 0; v < vertices; ++v) {
            const size_t at = (size_t(i) * vertices + v) * stride;
            if (own[i]) {
                (std::memcmp(&plain[at], &keep[at], stride) == 0 ? ownSame : ownBad) += 1;
            } else {
                (std::memcmp(&keep[at + posOffset], edvr::kEngineVelocityCulledPosition, 16) == 0 ? culled : notCulled) += 1;
            }
        }
    std::printf("  keep-own VS (%s): %u own vertices bit-identical, %u differ; %u foreign vertices at the culled point, %u not\n", label, ownSame, ownBad,
                culled, notCulled);
    if (ownVertices) *ownVertices = ownSame;
    if (culledVertices) *culledVertices = culled;
    return ownBad == 0 && notCulled == 0 && ownSame > 0 && culled > 0;
}

// A real (or corpus) pool-family VS: its input layout from its own input signature (INSTANCEANDMODELDATAINDEX per instance
// from slot 1, every other element 16 bytes from slot 0), patterned vertices and resources (corpus_identity's dummies at the
// VS stage, data sets 0 and 1), four instances of slots 3, 7, 1000 and 9 with 7 and 9 own: plain and keep-own streamed out.
// `plain` is the shader as bound (the game's, or a slot or skin patch of it); the keep-own variant is made from it here.
inline bool keepOwnStreamCheck(ID3D11Device* dev, ID3D11DeviceContext* ctx, const std::vector<BYTE>& plain, const edvr::EngineVelocityInputs& in,
                               void (*check)(bool, const char*), const char* name) {
    std::vector<BYTE> keep;
    std::string why;
    const bool patched = edvr::engineVelocityPatchVsKeepOwn(plain.data(), plain.size(), in, keep, why);
    if (!patched) std::printf("  keep-own VS: %s: %s\n", name, why.c_str());
    check(patched, "keep-own VS: the real family VS takes the keep-own patch");
    if (!patched) return false;
    ComPtr<ID3D11ShaderReflection> r;
    D3D11_SHADER_DESC desc{};
    if (!corpus_identity::detail::reflect(plain.data(), plain.size(), r, desc)) {
        check(false, "keep-own VS: the real family VS reflects");
        return false;
    }
    std::vector<D3D11_INPUT_ELEMENT_DESC> layout;
    std::vector<std::string> names;
    names.reserve(desc.InputParameters);
    UINT vertexBytes = 0;
    bool instanceIndex = false;
    for (UINT i = 0; i < desc.InputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC p{};
        r->GetInputParameterDesc(i, &p);
        if (p.SystemValueType != D3D_NAME_UNDEFINED) continue;   // SV_VertexID and the like come from the input assembler
        names.push_back(p.SemanticName);
        D3D11_INPUT_ELEMENT_DESC e{};
        e.SemanticName = names.back().c_str();
        e.SemanticIndex = p.SemanticIndex;
        const bool isIndex = names.back() == "INSTANCEANDMODELDATAINDEX" && p.SemanticIndex == 0;
        const DXGI_FORMAT four = p.ComponentType == D3D_REGISTER_COMPONENT_UINT32   ? DXGI_FORMAT_R32G32B32A32_UINT
                                 : p.ComponentType == D3D_REGISTER_COMPONENT_SINT32 ? DXGI_FORMAT_R32G32B32A32_SINT
                                                                                    : DXGI_FORMAT_R32G32B32A32_FLOAT;
        e.Format = isIndex ? DXGI_FORMAT_R32G32_UINT : four;
        e.InputSlot = isIndex ? 1u : 0u;
        e.AlignedByteOffset = isIndex ? 0u : vertexBytes;
        e.InputSlotClass = isIndex ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA;
        e.InstanceDataStepRate = isIndex ? 1u : 0u;
        if (isIndex) instanceIndex = true;
        else vertexBytes += 16u;
        layout.push_back(e);
    }
    check(instanceIndex, "keep-own VS: the real family VS reads INSTANCEANDMODELDATAINDEX");
    ComPtr<ID3D11InputLayout> il;
    const bool made = SUCCEEDED(dev->CreateInputLayout(layout.data(), UINT(layout.size()), plain.data(), plain.size(), &il));
    check(made, "keep-own VS: the real family VS's input layout creates");
    if (!made || !instanceIndex) return false;
    std::vector<corpus_identity::detail::Decl> decls;
    const bool declared = corpus_identity::detail::declarations(plain, decls, why);
    if (!declared) std::printf("  keep-own VS: %s: %s\n", name, why.c_str());
    check(declared, "keep-own VS: the real family VS's declarations are read");
    if (!declared) return false;
    std::vector<uint32_t> mask(16, 0u);
    mask[7] = mask[9] = 1u;
    const auto maskView = rawMask(dev, mask);
    const uint32_t instanceWords[kInstances * 2] = {kSlots[0], 0u, kSlots[1], 0u, kSlots[2], 0u, kSlots[3], 0u};
    const auto instanceBuffer = buffer(dev, instanceWords, sizeof instanceWords, D3D11_BIND_VERTEX_BUFFER);
    const bool own[kInstances] = {ownSlot(kSlots[0]), ownSlot(kSlots[1]), ownSlot(kSlots[2]), ownSlot(kSlots[3])};
    constexpr UINT kVertices = 3;
    bool all = true;
    unsigned ownTotal = 0, culledTotal = 0;
    for (unsigned seed = 0; seed < 2; ++seed) {
        ctx->ClearState();
        for (const auto& d : decls)
            if (!corpus_identity::detail::bind(dev, ctx, d, seed, why, true)) {
                std::printf("  keep-own VS: %s: %s\n", name, why.c_str());
                check(false, "keep-own VS: the real family VS's resources are bound");
                return false;
            }
        const auto vertexWords = corpus_identity::detail::words(DXGI_FORMAT_UNKNOWN, seed, size_t(vertexBytes ? vertexBytes : 16u) * kVertices / 4u, 77u);
        const auto vertexBuffer = buffer(dev, vertexWords.data(), UINT(vertexWords.size() * 4u), D3D11_BIND_VERTEX_BUFFER);
        if (!vertexBuffer || !instanceBuffer || !maskView) {
            check(false, "keep-own VS: the real family VS's buffers create");
            return false;
        }
        ctx->IASetInputLayout(il.Get());
        ID3D11Buffer* buffers[2] = {vertexBuffer.Get(), instanceBuffer.Get()};
        const UINT strides[2] = {vertexBytes ? vertexBytes : 16u, 8u}, offsets[2] = {0u, 0u};
        ctx->IASetVertexBuffers(0, 2, buffers, strides, offsets);
        ID3D11ShaderResourceView* maskSrv = maskView.Get();
        ctx->VSSetShaderResources(edvr::kEngineVelocityKeepOwnMaskSlot, 1, &maskSrv);
        std::vector<BYTE> a, b;
        UINT strideA = 0, strideB = 0, posA = 0, posB = 0;
        const bool streamed = streamOut(dev, ctx, plain, kVertices, kInstances, a, strideA, posA, why) &&
                              streamOut(dev, ctx, keep, kVertices, kInstances, b, strideB, posB, why);
        if (!streamed) std::printf("  keep-own VS: %s: %s\n", name, why.c_str());
        check(streamed && strideA == strideB && posA == posB, "keep-own VS: the real family VS and its variant stream out the same layout");
        if (!streamed || strideA != strideB) return false;
        char label[96];
        std::snprintf(label, sizeof label, "%s, data set %u", name, seed);
        unsigned ownV = 0, culledV = 0;
        const bool ok = compareStreams(a, b, strideA, posA, kVertices, own, kInstances, &ownV, &culledV, label);
        check(ok, "keep-own VS: real family VS: own instances bit-identical, foreign instances at the culled point");
        all = all && ok;
        ownTotal += ownV;
        culledTotal += culledV;
    }
    ctx->ClearState();
    return all && ownTotal == 2u * 2u * kVertices && culledTotal == 2u * 2u * kVertices;
}

struct Frame { std::vector<BYTE> colour, depth; };

inline std::vector<BYTE> readTexture(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* source, UINT bytesPerPixel) {
    D3D11_TEXTURE2D_DESC d{};
    source->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging))) return {};
    ctx->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return {};
    std::vector<BYTE> out(size_t(kWidth) * kHeight * bytesPerPixel);
    for (UINT y = 0; y < kHeight; ++y)
        std::memcpy(out.data() + size_t(y) * kWidth * bytesPerPixel, static_cast<const BYTE*>(m.pData) + size_t(y) * m.RowPitch, size_t(kWidth) * bytesPerPixel);
    ctx->Unmap(staging.Get(), 0);
    return out;
}

// The four instanced strips into an R32G32B32A32 target cleared to -1 and a D32 depth cleared to 0 (GEQUAL, writes on).
inline bool drawStrips(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11VertexShader* vs, ID3D11PixelShader* ps, ID3D11InputLayout* il,
                       ID3D11Buffer* instances, ID3D11ShaderResourceView* pool, ID3D11ShaderResourceView* mask, Frame& out) {
    ctx->ClearState();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kWidth;
    td.Height = kHeight;
    td.MipLevels = td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> colour, depth;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &colour)) || FAILED(dev->CreateRenderTargetView(colour.Get(), nullptr, &rtv))) return false;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd{};
    dvd.Format = DXGI_FORMAT_D32_FLOAT;
    dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &depth)) || FAILED(dev->CreateDepthStencilView(depth.Get(), &dvd, &dsv))) return false;
    const float clear[4] = {-1, -1, -1, -1};
    ctx->ClearRenderTargetView(rtv.Get(), clear);
    ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0, 0);
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;   // the cull must not depend on the depth clip
    ComPtr<ID3D11DepthStencilState> dss;
    ComPtr<ID3D11RasterizerState> rs;
    if (FAILED(dev->CreateDepthStencilState(&ds, &dss)) || FAILED(dev->CreateRasterizerState(&rd, &rs))) return false;
    const D3D11_VIEWPORT vp{0, 0, float(kWidth), float(kHeight), 0, 1};
    ID3D11RenderTargetView* bound = rtv.Get();
    ctx->OMSetRenderTargets(1, &bound, dsv.Get());
    ctx->OMSetDepthStencilState(dss.Get(), 0);
    ctx->RSSetState(rs.Get());
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(il);
    const UINT stride = 8, offset = 0;
    ID3D11Buffer* none = nullptr;
    const UINT zero = 0;
    ctx->IASetVertexBuffers(0, 1, &none, &zero, &zero);
    ctx->IASetVertexBuffers(1, 1, &instances, &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->VSSetShaderResources(33, 1, &pool);
    if (mask) ctx->VSSetShaderResources(edvr::kEngineVelocityKeepOwnMaskSlot, 1, &mask);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->DrawInstanced(6, kInstances, 0, 0);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    out.colour = readTexture(dev, ctx, colour.Get(), 16);
    out.depth = readTexture(dev, ctx, depth.Get(), 4);
    return !out.colour.empty() && !out.depth.empty();
}

// Per pixel: an own strip's pixel equals the reference; a foreign strip's keeps the clears.
inline bool compareStrips(const Frame& reference, const Frame& variant, const char* label, unsigned* keptOut, unsigned* goneOut) {
    unsigned kept = 0, keptBad = 0, gone = 0, goneBad = 0;
    const float clear4[4] = {-1, -1, -1, -1};
    const float zero = 0.0f;
    for (UINT y = 0; y < kHeight; ++y)
        for (UINT x = 0; x < kWidth; ++x) {
            const size_t p = size_t(y) * kWidth + x;
            const bool own = ownSlot(kSlots[x / 8]);
            const bool same = std::memcmp(&variant.colour[p * 16], &reference.colour[p * 16], 16) == 0 &&
                              std::memcmp(&variant.depth[p * 4], &reference.depth[p * 4], 4) == 0;
            const bool cleared = std::memcmp(&variant.colour[p * 16], clear4, 16) == 0 && std::memcmp(&variant.depth[p * 4], &zero, 4) == 0;
            if (own) { ++kept; keptBad += same ? 0u : 1u; }
            else { ++gone; goneBad += cleared ? 0u : 1u; }
        }
    std::printf("  keep-own VS (%s): %u own pixels, %u differ from the plain VS; %u foreign pixels, %u written\n", label, kept, keptBad, gone, goneBad);
    if (keptOut) *keptOut = kept;
    if (goneOut) *goneOut = gone;
    return keptBad == 0 && goneBad == 0 && kept > 0 && gone > 0;
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
    const std::vector<BYTE> vsBytes = blobBytes(vsBlob.Get());
    edvr::EngineVelocityInputs in;
    std::string why;
    const bool derived = edvr::engineVelocityDeriveInputs(vsBytes.data(), vsBytes.size(), in, why);
    if (!derived) std::printf("  keep-own VS: the synthetic VS: %s\n", why.c_str());
    check(derived && !in.slotFromVsPatch, "keep-own VS: the synthetic VS derives as a family that exports the slot");
    if (!derived) return;
    std::vector<BYTE> keep;
    const bool patched = edvr::engineVelocityPatchVsKeepOwn(vsBytes.data(), vsBytes.size(), in, keep, why);
    if (!patched) std::printf("  keep-own VS: the synthetic VS: %s\n", why.c_str());
    check(patched, "keep-own VS: the synthetic VS takes the keep-own patch");
    if (!patched) return;
    {
        std::vector<BYTE> refused;
        auto occupied = shader_tests::compile(h, kVsOccupied, "vs_5_0");
        if (occupied) {
            const auto b = blobBytes(occupied.Get());
            edvr::EngineVelocityInputs o{};
            o.positionRegister = 0;
            check(!edvr::engineVelocityPatchVsKeepOwn(b.data(), b.size(), o, refused, why) && why.find("t127 occupied") != std::string::npos,
                  "keep-own VS: a shader that names t127 is refused");
        }
        edvr::EngineVelocityInputs wrong = in;
        wrong.positionRegister = in.identityRegister;   // not SV_Position
        check(!edvr::engineVelocityPatchVsKeepOwn(vsBytes.data(), vsBytes.size(), wrong, refused, why) && why.find("SV_Position") != std::string::npos,
              "keep-own VS: a position register that is not SV_Position is refused");
    }
    ComPtr<ID3D11VertexShader> plainVs, keepVs;
    ComPtr<ID3D11PixelShader> ps;
    check(SUCCEEDED(dev->CreateVertexShader(vsBytes.data(), vsBytes.size(), nullptr, &plainVs)) &&
              SUCCEEDED(dev->CreateVertexShader(keep.data(), keep.size(), nullptr, &keepVs)) &&
              SUCCEEDED(dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps)),
          "keep-own VS: the synthetic shaders create on WARP");
    if (!plainVs || !keepVs || !ps) return;
    const D3D11_INPUT_ELEMENT_DESC element{"INSTANCEANDMODELDATAINDEX", 0, DXGI_FORMAT_R32G32_UINT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1};
    ComPtr<ID3D11InputLayout> il;
    check(SUCCEEDED(dev->CreateInputLayout(&element, 1, vsBytes.data(), vsBytes.size(), &il)), "keep-own VS: the synthetic input layout creates");
    if (!il) return;
    const uint32_t instanceWords[kInstances * 2] = {kSlots[0], 0u, kSlots[1], 0u, kSlots[2], 0u, kSlots[3], 0u};
    const auto instances = buffer(dev, instanceWords, sizeof instanceWords, D3D11_BIND_VERTEX_BUFFER);
    // The pool: 16 records of 336 bytes, patterned (record r's word k = r + k / 64); slot 1000 is past its end (reads 0).
    std::vector<float> poolWords(16u * 84u);
    for (size_t k = 0; k < poolWords.size(); ++k) poolWords[k] = float(k / 84u) + float(k % 84u) / 64.0f;
    D3D11_BUFFER_DESC pd{};
    pd.ByteWidth = UINT(poolWords.size() * 4u);
    pd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    pd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    pd.StructureByteStride = 336;
    D3D11_SUBRESOURCE_DATA pinit{poolWords.data(), 0, 0};
    ComPtr<ID3D11Buffer> pool;
    ComPtr<ID3D11ShaderResourceView> poolView;
    std::vector<uint32_t> words(16, 0u);
    words[7] = words[9] = 1u;
    const auto mask = rawMask(dev, words);
    const auto noneOwn = rawMask(dev, std::vector<uint32_t>(16, 0u));
    check(instances && mask && noneOwn && SUCCEEDED(dev->CreateBuffer(&pd, &pinit, &pool)) &&
              SUCCEEDED(dev->CreateShaderResourceView(pool.Get(), nullptr, &poolView)),
          "keep-own VS: the instance stream, pool and masks create");
    if (!instances || !mask || !noneOwn || !poolView) return;
    Frame ref, var, varNone;
    check(drawStrips(dev, ctx, plainVs.Get(), ps.Get(), il.Get(), instances.Get(), poolView.Get(), nullptr, ref) &&
              drawStrips(dev, ctx, keepVs.Get(), ps.Get(), il.Get(), instances.Get(), poolView.Get(), mask.Get(), var) &&
              drawStrips(dev, ctx, keepVs.Get(), ps.Get(), il.Get(), instances.Get(), poolView.Get(), noneOwn.Get(), varNone),
          "keep-own VS: the three synthetic draws read back");
    if (ref.colour.empty() || var.colour.empty() || varNone.colour.empty()) return;
    // The reference must have drawn every strip (not vacuous): no pixel keeps the clear.
    unsigned refDrawn = 0;
    const float clear4[4] = {-1, -1, -1, -1};
    for (size_t p = 0; p < size_t(kWidth) * kHeight; ++p) refDrawn += std::memcmp(&ref.colour[p * 16], clear4, 16) != 0 ? 1u : 0u;
    check(refDrawn == kWidth * kHeight, "keep-own VS: the plain VS drew every pixel of the four strips (the reference is not vacuous)");
    unsigned kept = 0, gone = 0;
    check(compareStrips(ref, var, "synthetic", &kept, &gone), "keep-own VS: own pixels bit-identical, foreign pixels never written (synthetic)");
    check(kept == 256 && gone == 256, "keep-own VS: two strips kept, two culled (synthetic)");
    unsigned noneDrawn = 0;
    for (size_t p = 0; p < size_t(kWidth) * kHeight; ++p) noneDrawn += std::memcmp(&varNone.colour[p * 16], clear4, 16) != 0 ? 1u : 0u;
    check(noneDrawn == 0, "keep-own VS: with no record own, nothing is drawn");
    // Stream output: every output of every vertex.
    {
        ctx->ClearState();
        ctx->IASetInputLayout(il.Get());
        const UINT stride = 8, offset = 0;
        ID3D11Buffer* ib = instances.Get();
        ctx->IASetVertexBuffers(1, 1, &ib, &stride, &offset);
        ID3D11ShaderResourceView* views[1] = {poolView.Get()};
        ctx->VSSetShaderResources(33, 1, views);
        ID3D11ShaderResourceView* m = mask.Get();
        ctx->VSSetShaderResources(edvr::kEngineVelocityKeepOwnMaskSlot, 1, &m);
        std::vector<BYTE> a, b;
        UINT sa = 0, sb = 0, pa = 0, pb = 0;
        const bool streamed = streamOut(dev, ctx, vsBytes, 6, kInstances, a, sa, pa, why) && streamOut(dev, ctx, keep, 6, kInstances, b, sb, pb, why);
        if (!streamed) std::printf("  keep-own VS: synthetic stream output: %s\n", why.c_str());
        check(streamed && sa == sb && pa == pb, "keep-own VS: the synthetic VS and its variant stream out the same layout");
        const bool own[kInstances] = {ownSlot(kSlots[0]), ownSlot(kSlots[1]), ownSlot(kSlots[2]), ownSlot(kSlots[3])};
        if (streamed && sa == sb)
            check(compareStreams(a, b, sa, pa, 6, own, kInstances, nullptr, nullptr, "synthetic, stream output"),
                  "keep-own VS: synthetic stream output: own vertices bit-identical, foreign vertices at the culled point");
    }
    // The real hull-family vertex shaders kept in the repository.
    const char* fixtures[] = {"vs_66DE2CADB1F4AE6B.dxbc", "vs_AACFDCF2FB9AD809.dxbc"};
    for (const char* fx : fixtures) {
        const auto rvs = readFixture(fx);
        check(!rvs.empty(), "keep-own VS: the real hull fixtures are present (tools\\flat_temporal_test\\fixtures; run from the repo root)");
        if (rvs.empty()) continue;
        edvr::EngineVelocityInputs rin;
        const bool d = edvr::engineVelocityDeriveInputs(rvs.data(), rvs.size(), rin, why);
        check(d, "keep-own VS: the real hull VS derives its inputs");
        if (!d) continue;
        check(keepOwnStreamCheck(dev, ctx, rvs, rin, check, fx), "keep-own VS: the real hull VS is proven (own bit-identical, foreign culled)");
    }
    ctx->ClearState();
}
}  // namespace keep_own_vs_tests

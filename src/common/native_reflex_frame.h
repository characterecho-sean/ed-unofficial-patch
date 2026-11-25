#pragma once

#include <cstdint>

#define EDVR_NATIVE_REFLEX_FRAME_VERSION_1 1u

// A source identifies one qualified LateRender invocation and the exact
// runtime render pose returned to that invocation. It does not identify world
// simulation state or a display/photon timestamp. Zero means unowned.
struct EdvrNativeReflexSource {
    uint32_t size = 0, version = 0;
    uint64_t generation = 0, frameToken = 0, poseSequence = 0;
};
static_assert(sizeof(EdvrNativeReflexSource) == 32, "fixed paired-module source layout");

namespace edvr::native_reflex {
constexpr bool validSource(const EdvrNativeReflexSource& source) noexcept {
    return source.size == sizeof(source) && source.version == EDVR_NATIVE_REFLEX_FRAME_VERSION_1 &&
        source.generation && source.frameToken && source.poseSequence;
}
constexpr bool sameSource(const EdvrNativeReflexSource& a, const EdvrNativeReflexSource& b) noexcept {
    return validSource(a) && validSource(b) && a.generation == b.generation &&
        a.frameToken == b.frameToken && a.poseSequence == b.poseSequence;
}
// Select only the eyes actually drawn. An absent/mixed pair or a repeated
// source cannot assign a new Present to an old rendered frame.
inline EdvrNativeReflexSource selectMirrorSource(const EdvrNativeReflexSource (&eyes)[2],
                                                unsigned mask, const EdvrNativeReflexSource& previous) noexcept {
    EdvrNativeReflexSource selected{};
    if (!mask || (mask & ~3u)) return selected;
    for (unsigned eye = 0; eye < 2; ++eye) if (mask & (1u << eye)) {
        if (!validSource(eyes[eye]) || (validSource(selected) && !sameSource(selected, eyes[eye]))) return {};
        selected = eyes[eye];
    }
    if (validSource(previous) && previous.generation == selected.generation && selected.frameToken <= previous.frameToken) return {};
    return selected;
}
} // namespace edvr::native_reflex

#ifdef _WIN32
#include <windows.h>
#include <d3d11.h>

struct EdvrNativeReflexFrameRequest {
    uint32_t size, version;
    ID3D11Device* device;
    uint64_t generation;
};
struct EdvrNativeReflexFrameTable {
    uint32_t size, version;
    void* context;
    // Game caller thread, after a valid render pose was actually copied by
    // WaitGetPoses. The provider accepts only its strict active Late job.
    uint64_t (WINAPI *poseConsumed)(void*, uint64_t poseSequence);
    // Game Submit caller, before marshaling to the XR owner. The provider
    // verifies the same job/pose receipt, stamps texture and returns its source.
    HRESULT (WINAPI *eyeSource)(void*, ID3D11Texture2D*, uint32_t eye,
                               uint64_t poseSequence, EdvrNativeReflexSource*);
    // Actual game Submit return, with acceptance reported separately for
    // each eye. A source stamp alone proves an attempt, not an accepted pair.
    void (WINAPI *eyeSubmitted)(void*, uint32_t eye, uint64_t poseSequence, uint32_t accepted);
    HRESULT (WINAPI *close)(void*);
};
extern "C" HRESULT WINAPI edvrAcquireNativeReflexFrame(
    const EdvrNativeReflexFrameRequest*, EdvrNativeReflexFrameTable*);

namespace edvr::native_reflex {
inline constexpr GUID kSourceGuid = {0x5f924c8b, 0xdcc6, 0x4705, {0xbc, 0xd7, 0x2b, 0xee, 0x50, 0x9f, 0xb1, 0x76}};
// CPU-only COM private data. No context API or GPU synchronization is issued.
// Copy this value with a retained image; rereading a reused pointer is not an
// immutable frame identity. Failed reads always clear the caller's output.
inline bool readSource(ID3D11Texture2D* texture, EdvrNativeReflexSource& source) noexcept {
    source = {};
    if (!texture) return false;
    UINT size = sizeof(source);
    if (texture->GetPrivateData(kSourceGuid, &size, &source) != S_OK || size != sizeof(source) || !validSource(source)) {
        source = {}; return false;
    }
    return true;
}
inline void clearSource(ID3D11Texture2D* texture) noexcept {
    if (texture) texture->SetPrivateData(kSourceGuid, 0, nullptr);
}
inline bool writeSource(ID3D11Texture2D* texture, const EdvrNativeReflexSource& source) noexcept {
    if (!texture) return false;
    if (!validSource(source)) { clearSource(texture); return false; }
    if (texture->SetPrivateData(kSourceGuid, sizeof(source), &source) == S_OK) return true;
    clearSource(texture); return false;
}
} // namespace edvr::native_reflex
#endif

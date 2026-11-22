// Flat frame generation's engage and present-permission decisions. Header-only so a rig can call the
// shipped comparisons without a device. The graphics DLL fills FrameGenFacts from the session and
// asks these functions; it does not keep a second copy of the rules.
#pragma once

#include <cstdio>
#include <cstring>
#include <string>

namespace edvr {

enum class FrameGenKey { Off, Auto, On, Unknown };

struct FrameGenFacts {
    bool vr = false;
    FrameGenKey key = FrameGenKey::Off;
    bool temporalAaOff = false;
    bool providerDllMissing = false;
    bool queryHasNoVersion = false;
    bool createRejectedOffer = false;
    bool xessOwnsSwapChain = false;
    bool hdrWithoutLuminance = false;
    bool probeFailed = false;
    bool probeOk = false;
    bool refreshRead = false;
    double renderedMs = 0;
    double periodMs = 0;
    double realGpuMs = -1;      // < 0: the real frame's GPU time has not been read
    double dispatchGpuMs = -1;  // < 0: the dispatch's GPU time has not been read
    bool haveScene = true;
    bool haveInputs = true;
    bool haveBasis = true;
    bool exclusiveFullscreen = false;
    bool reset = false;
    const char* offeredVersion = nullptr;
    const char* providerText = nullptr;
};

// The hard-refusal line, or nullptr when the key is off (off writes nothing) or the probe may run.
// scratch is used when the line names a version or a provider sentence.
inline const char* frameGenHardRefusal(const FrameGenFacts& f, char* scratch, size_t scratchBytes) {
    if (f.key == FrameGenKey::Off || f.key == FrameGenKey::Unknown) return nullptr;
    if (f.vr) return "frame generation: not started: VR session, flat only";
    if (f.providerDllMissing)
        return "frame generation: not started: amd_fidelityfx_framegeneration_dx12.dll is missing beside EDVR";
    if (f.queryHasNoVersion) {
        if (scratch && scratchBytes && f.providerText && f.providerText[0]) {
            std::snprintf(scratch, scratchBytes, "frame generation: not started: %s", f.providerText);
            return scratch;
        }
        return "frame generation: not started: frame-generation query offered no version";
    }
    if (f.createRejectedOffer) {
        const char* version = f.offeredVersion && f.offeredVersion[0] ? f.offeredVersion : "no version";
        if (scratch && scratchBytes) {
            std::snprintf(scratch, scratchBytes,
                          "frame generation: not started: frame-generation query offered %s, create rejected it",
                          version);
            return scratch;
        }
        return "frame generation: not started: frame-generation query offered no version, create rejected it";
    }
    if (f.temporalAaOff) return "frame generation: not started: fix.temporal_aa is off";
    if (f.xessOwnsSwapChain)
        return "frame generation: stood down, XeSS frame generation owns the swap chain";
    if (f.hdrWithoutLuminance) return "frame generation: flat HDR route has no luminance range yet";
    if (f.probeFailed) {
        const char* why = f.providerText && f.providerText[0] ? f.providerText : "probe dispatch failed";
        if (scratch && scratchBytes) {
            std::snprintf(scratch, scratchBytes, "frame generation: not started: %s", why);
            return scratch;
        }
        return "frame generation: not started: probe dispatch failed";
    }
    return nullptr;
}

// auto's three conditions: the monitor period was read, this rendered frame missed it, and one real
// frame's GPU time plus the dispatch fits in two periods with 1 ms of slack. A missing GPU time is
// not zero.
inline bool frameGenAutoEngaged(const FrameGenFacts& f) {
    char scratch[256];
    if (f.key != FrameGenKey::Auto) return false;
    if (frameGenHardRefusal(f, scratch, sizeof(scratch))) return false;
    if (!f.probeOk || !f.refreshRead) return false;
    if (!(f.periodMs > 0.0) || !(f.renderedMs > f.periodMs)) return false;
    if (!(f.realGpuMs >= 0.0) || !(f.dispatchGpuMs >= 0.0)) return false;
    return (f.realGpuMs + f.dispatchGpuMs) <= (2.0 * f.periodMs - 1.0);
}

// True when this frame may Present a generated image. off and a VR session never do. auto does only
// while frameGenAutoEngaged is true. A skip (no scene, no inputs, no camera basis, exclusive
// fullscreen, a reset) does not present one either.
inline bool frameGenMayPresentGenerated(const FrameGenFacts& f) {
    char scratch[256];
    if (f.key != FrameGenKey::On && f.key != FrameGenKey::Auto) return false;
    if (frameGenHardRefusal(f, scratch, sizeof(scratch))) return false;
    if (!f.probeOk) return false;
    if (!f.haveScene || !f.haveInputs || !f.haveBasis || f.exclusiveFullscreen || f.reset) return false;
    if (f.key == FrameGenKey::On) return true;
    return frameGenAutoEngaged(f);
}

// A generated Present counts as applied only when the presenting chain's frame-latency wait ran
// before that Present.
inline bool frameGenPairCountsApplied(bool generatedPresented, bool waitRan) {
    return generatedPresented && waitRan;
}

// What one resolver snapshot does before any copy. internalHdrRoute is the flat HDR route: the
// HUD-less image is the game's R11G11B10 target after the resolve writes back into it. On an
// 8-bit window that image is copied and may be presented. hdrWithoutLuminance is an fp16
// presenting chain, which this build has no nit range for: it is not copied and not presented.
enum class FrameGenResolveAction { CopyInputs, SkipMissingInputs, RefuseNoLuminance };

inline FrameGenResolveAction frameGenResolveAction(bool internalHdrRoute, bool hdrWithoutLuminance,
                                                    bool haveColour, bool haveDepth, bool haveMotion,
                                                    bool haveSize) {
    if (hdrWithoutLuminance) return FrameGenResolveAction::RefuseNoLuminance;
    if (!haveColour || !haveDepth || !haveMotion || !haveSize)
        return FrameGenResolveAction::SkipMissingInputs;
    // The internal route and the copy route both copy once the three inputs and the size are present.
    return internalHdrRoute ? FrameGenResolveAction::CopyInputs : FrameGenResolveAction::CopyInputs;
}

// DXGI formats the dispatch contract names. frame_gen.cpp static_asserts these against DXGI_FORMAT.
constexpr uint32_t kFrameGenR11G11B10 = 26;
constexpr uint32_t kFrameGenR8G8B8A8Unorm = 28;
constexpr uint32_t kFrameGenB8G8R8A8Unorm = 87;

// Values match FfxApiBackbufferTransferFunction. frame_gen.cpp static_asserts that.
enum class FrameGenTransfer { Srgb = 0, Pq = 1, ScRgb = 2 };

// How one dispatch describes the copied colour. The 8-bit copy route and the 64x64 probe stay
// sRGB with luminance 0 and 1 and do not bind a HUD-less colour. The default flat path is an
// 8-bit window whose HUD-less image is R11G11B10 scene radiance: that image is the presentation
// source, so the context is high-dynamic-range, its back-buffer format is the image's format,
// and configure binds the image as HUD-less colour.
//
// The provider converts scRGB with (value - min/80) / ((max - min) / 80). Min 0 and max 80
// make that the identity, so the interpolator sees the radiance. This is the scRGB reference
// (1.0 = 80 nits), not a measured display range and not the game's tone curve.
struct FrameGenColourContract {
    bool highDynamicRange = false;
    bool bindHudlessColor = false;
    bool backBufferIsColour = false;
    FrameGenTransfer transfer = FrameGenTransfer::Srgb;
    float minLuminance = 0.0f;
    float maxLuminance = 1.0f;
};

inline bool frameGenPresenterIs8Bit(uint32_t presenterFormat) {
    return presenterFormat == kFrameGenR8G8B8A8Unorm || presenterFormat == kFrameGenB8G8R8A8Unorm;
}

inline FrameGenColourContract frameGenColourContract(uint32_t colourFormat, uint32_t presenterFormat) {
    FrameGenColourContract contract;
    if (colourFormat != kFrameGenR11G11B10 || !frameGenPresenterIs8Bit(presenterFormat)) return contract;
    contract.highDynamicRange = true;
    contract.bindHudlessColor = true;
    contract.backBufferIsColour = true;
    contract.transfer = FrameGenTransfer::ScRgb;
    contract.minLuminance = 0.0f;
    contract.maxLuminance = 80.0f;
    return contract;
}

// frame_gen.cpp must describe the HUD-less image through frameGenColourContract. A source that
// hardcodes an sRGB transfer, or that never binds HUDLessColor, is not that dispatch.
inline bool frameGenDispatchSourceWired(const char* src) {
    if (!src) return false;
    if (std::strstr(src, "frameGenColourContract(") == nullptr) return false;
    if (std::strstr(src, "FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE") == nullptr) return false;
    if (std::strstr(src, "HUDLessColor") == nullptr) return false;
    if (std::strstr(src, "backBufferIsColour") == nullptr) return false;
    if (std::strstr(src, "backbufferTransferFunction = static_cast<uint32_t>(contract.transfer)") == nullptr)
        return false;
    if (std::strstr(src, "minMaxLuminance[0] = contract.minLuminance") == nullptr) return false;
    if (std::strstr(src, "minMaxLuminance[1] = contract.maxLuminance") == nullptr) return false;
    if (std::strstr(src, "generate.presentColor = ffxApiGetResourceDX12(s.colour.d12.Get()") == nullptr)
        return false;
    if (std::strstr(src, "backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB") != nullptr)
        return false;
    return true;
}

// Reads the graphics DLL source the caller loaded. The twins are the rejected dispatch: a hardcoded
// sRGB transfer, no colour-contract call, or presentColor with no HUDLessColor.
inline int frameGenDispatchWiringSelfTest(const char* src) {
    int failed = 0;
    auto expect = [&](bool cond, const char* name) {
        std::printf("frame_gen_dispatch: %s %s\n", name, cond ? "ok" : "FAIL");
        if (!cond) ++failed;
    };
    expect(frameGenDispatchSourceWired(src), "dispatch interpolates the hudless image as the presentation source");
    if (!src) {
        std::printf("frame_gen_dispatch: %s\n", failed ? "FAIL" : "OK");
        return failed;
    }
    const std::string text(src);
    const std::string srgbTwin =
        text + "\ngenerate.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;\n";
    expect(!frameGenDispatchSourceWired(srgbTwin.c_str()), "twin: a hardcoded sRGB transfer fails the dispatch check");
    std::string stripped = text;
    const std::string call = "frameGenColourContract(";
    const size_t at = stripped.find(call);
    if (at != std::string::npos) stripped.erase(at, call.size());
    expect(at != std::string::npos && !frameGenDispatchSourceWired(stripped.c_str()),
           "twin: dispatch without the colour contract fails");
    std::string noHudless = text;
    const std::string hudless = "HUDLessColor";
    const size_t hudAt = noHudless.find(hudless);
    if (hudAt != std::string::npos) noHudless.erase(hudAt, hudless.size());
    expect(hudAt != std::string::npos && !frameGenDispatchSourceWired(noHudless.c_str()),
           "twin: presentColor alone, with no HUDLessColor, fails the dispatch check");
    std::printf("frame_gen_dispatch: %s\n", failed ? "FAIL" : "OK");
    return failed;
}

// Present permission for one frame. The internal HDR route is not an extra block: an 8-bit window
// may present when frameGenMayPresentGenerated is true. A chain with no luminance range may not.
inline bool frameGenPresentAllowsGenerated(const FrameGenFacts& facts, bool internalHdrRoute) {
    if (frameGenResolveAction(internalHdrRoute, facts.hdrWithoutLuminance, true, true, true, true) !=
        FrameGenResolveAction::CopyInputs)
        return false;
    return frameGenMayPresentGenerated(facts);
}

inline int frameGenDecisionSelfTest() {
    int failed = 0;
    auto expect = [&](bool cond, const char* name) {
        std::printf("frame_gen_decision: %s %s\n", name, cond ? "ok" : "FAIL");
        if (!cond) ++failed;
    };
    char scratch[256];
    FrameGenFacts off{};
    expect(frameGenHardRefusal(off, scratch, sizeof(scratch)) == nullptr, "off writes no line");
    expect(!frameGenMayPresentGenerated(off), "off does not present");

    FrameGenFacts vr{};
    vr.key = FrameGenKey::On;
    vr.vr = true;
    vr.probeOk = true;
    const char* vrLine = frameGenHardRefusal(vr, scratch, sizeof(scratch));
    expect(vrLine && std::strcmp(vrLine, "frame generation: not started: VR session, flat only") == 0, "vr line");
    expect(!frameGenMayPresentGenerated(vr), "vr does not present");

    FrameGenFacts missing = vr;
    missing.vr = false;
    missing.providerDllMissing = true;
    const char* missingLine = frameGenHardRefusal(missing, scratch, sizeof(scratch));
    expect(missingLine && std::strstr(missingLine, "amd_fidelityfx_framegeneration_dx12.dll is missing") != nullptr,
           "missing dll line");
    expect(!frameGenMayPresentGenerated(missing), "missing dll does not present");

    FrameGenFacts nover = missing;
    nover.providerDllMissing = false;
    nover.queryHasNoVersion = true;
    expect(frameGenHardRefusal(nover, scratch, sizeof(scratch)) != nullptr, "no version line");
    expect(!frameGenMayPresentGenerated(nover), "no version does not present");

    FrameGenFacts rejected = nover;
    rejected.queryHasNoVersion = false;
    rejected.createRejectedOffer = true;
    rejected.offeredVersion = "4.0.1";
    const char* rejectedLine = frameGenHardRefusal(rejected, scratch, sizeof(scratch));
    expect(rejectedLine && std::strcmp(rejectedLine,
               "frame generation: not started: frame-generation query offered 4.0.1, create rejected it") == 0,
           "create rejected line");
    expect(!frameGenMayPresentGenerated(rejected), "create rejected does not present");

    FrameGenFacts aa = rejected;
    aa.createRejectedOffer = false;
    aa.temporalAaOff = true;
    expect(frameGenHardRefusal(aa, scratch, sizeof(scratch)) &&
               std::strstr(frameGenHardRefusal(aa, scratch, sizeof(scratch)), "fix.temporal_aa is off") != nullptr,
           "temporal_aa line");

    FrameGenFacts xess = aa;
    xess.temporalAaOff = false;
    xess.xessOwnsSwapChain = true;
    const char* xessLine = frameGenHardRefusal(xess, scratch, sizeof(scratch));
    expect(xessLine && std::strcmp(xessLine,
               "frame generation: stood down, XeSS frame generation owns the swap chain") == 0,
           "xess stand-down line");

    FrameGenFacts hdr = xess;
    hdr.xessOwnsSwapChain = false;
    hdr.hdrWithoutLuminance = true;
    expect(frameGenHardRefusal(hdr, scratch, sizeof(scratch)) &&
               std::strstr(frameGenHardRefusal(hdr, scratch, sizeof(scratch)),
                           "flat HDR route has no luminance range yet") != nullptr,
           "hdr line");

    FrameGenFacts on{};
    on.key = FrameGenKey::On;
    on.probeOk = true;
    expect(frameGenHardRefusal(on, scratch, sizeof(scratch)) == nullptr, "on has no hard refusal");
    expect(frameGenMayPresentGenerated(on), "on may present");

    FrameGenFacts basis = on;
    basis.haveBasis = false;
    expect(!frameGenMayPresentGenerated(basis), "no camera basis does not present");

    FrameGenFacts automatic{};
    automatic.key = FrameGenKey::Auto;
    automatic.probeOk = true;
    automatic.refreshRead = true;
    automatic.periodMs = 8.0;
    automatic.renderedMs = 10.0;
    automatic.realGpuMs = 6.0;
    automatic.dispatchGpuMs = 4.0;  // 10 <= 16 - 1
    expect(frameGenAutoEngaged(automatic), "auto engages inside the budget");
    expect(frameGenMayPresentGenerated(automatic), "auto may present");

    FrameGenFacts onTime = automatic;
    onTime.renderedMs = 8.0;
    expect(!frameGenAutoEngaged(onTime) && !frameGenMayPresentGenerated(onTime), "auto on time holds off");

    FrameGenFacts unread = automatic;
    unread.refreshRead = false;
    expect(!frameGenMayPresentGenerated(unread), "auto without a refresh does not present");

    FrameGenFacts gpuNa = automatic;
    gpuNa.dispatchGpuMs = -1.0;
    expect(!frameGenMayPresentGenerated(gpuNa), "auto treats a missing dispatch time as not zero");

    FrameGenFacts over = automatic;
    over.realGpuMs = 12.0;
    over.dispatchGpuMs = 4.0;  // 16 > 15
    expect(!frameGenMayPresentGenerated(over), "auto over budget does not present");

    expect(frameGenPairCountsApplied(true, true), "wait ran counts applied");
    expect(!frameGenPairCountsApplied(true, false), "wait missed is not applied");
    expect(!frameGenPairCountsApplied(false, true), "a real-only present is not applied");

    // The default flat resolve is the internal HDR route. Its HUD-less colour is copied. The
    // one-session line stays the fp16 chain, which has no luminance range.
    expect(frameGenResolveAction(true, false, true, true, true, true) == FrameGenResolveAction::CopyInputs,
           "internal hdr route copies hudless colour");
    expect(frameGenResolveAction(false, false, true, true, true, true) == FrameGenResolveAction::CopyInputs,
           "copy route copies hudless colour");
    expect(frameGenResolveAction(true, false, false, true, true, true) == FrameGenResolveAction::SkipMissingInputs,
           "internal hdr route without colour does not copy");
    expect(frameGenResolveAction(false, false, true, false, true, true) == FrameGenResolveAction::SkipMissingInputs,
           "missing depth does not copy");
    expect(frameGenResolveAction(true, true, true, true, true, true) == FrameGenResolveAction::RefuseNoLuminance,
           "no luminance range refuses the copy");
    expect(frameGenPresentAllowsGenerated(on, true), "internal hdr route may present");
    FrameGenFacts hdrRoute = on;
    hdrRoute.hdrWithoutLuminance = true;
    expect(!frameGenPresentAllowsGenerated(hdrRoute, true), "no luminance range does not present");
    expect(frameGenHardRefusal(on, scratch, sizeof(scratch)) == nullptr,
           "internal hdr route is not the luminance line");

    // The provider's scRGB conversion. The shipped luminance pair must make it the identity on
    // a radiance sample above 1, which an sRGB 0..1 range does not.
    const FrameGenColourContract scene = frameGenColourContract(kFrameGenR11G11B10, kFrameGenR8G8B8A8Unorm);
    const float radiance = 4.0f;
    const float span = (scene.maxLuminance - scene.minLuminance) / 80.0f;
    const float linear = span != 0.0f ? (radiance - scene.minLuminance / 80.0f) / span : 0.0f;
    expect(scene.highDynamicRange && scene.bindHudlessColor && scene.backBufferIsColour,
           "internal hdr route dispatch binds the hudless image");
    expect(scene.transfer == FrameGenTransfer::ScRgb && linear == radiance,
           "internal hdr route scRGB range leaves radiance unchanged");
    const FrameGenColourContract bgra = frameGenColourContract(kFrameGenR11G11B10, kFrameGenB8G8R8A8Unorm);
    expect(bgra.highDynamicRange && bgra.bindHudlessColor && bgra.backBufferIsColour &&
               bgra.transfer == scene.transfer && bgra.minLuminance == scene.minLuminance &&
               bgra.maxLuminance == scene.maxLuminance,
           "internal hdr route on a BGRA window uses the same contract");
    const FrameGenColourContract copy = frameGenColourContract(kFrameGenR8G8B8A8Unorm, kFrameGenR8G8B8A8Unorm);
    expect(!copy.highDynamicRange && !copy.bindHudlessColor && !copy.backBufferIsColour &&
               copy.transfer == FrameGenTransfer::Srgb && copy.minLuminance == 0.0f && copy.maxLuminance == 1.0f,
           "8-bit copy route dispatch stays sRGB 0 to 1");

    std::printf("frame_gen_decision: %s\n", failed ? "FAIL" : "OK");
    return failed;
}

}  // namespace edvr

// Flat frame generation's engage and present-permission decisions. Header-only so a rig can call the
// shipped comparisons without a device. The graphics DLL fills FrameGenFacts from the session and
// asks these functions; it does not keep a second copy of the rules.
#pragma once

#include <cstdio>
#include <cstring>

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

    std::printf("frame_gen_decision: %s\n", failed ? "FAIL" : "OK");
    return failed;
}

}  // namespace edvr

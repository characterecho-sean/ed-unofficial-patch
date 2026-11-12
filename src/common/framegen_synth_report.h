#pragma once
// The frame-generation synthesizer's statistics and its two log lines (fix.frame_generation_shadow, TEMPORARY; docs/design-frame-generation-2026-10-26.md, Phase 2.1,
// "The synthesis pass"). Pure: no Windows or D3D includes, so the test rig and tools\edvr_log.py's --health self-test read the same format strings the DLL logs with.
//
// The synthesizer builds, per eye and real frame B, the frame S one display period later; nothing is shown. Every other real frame it builds the f = 0.5 S (the one a
// later phase would show: Elite's step is two periods) and on the frames between it the f = 1 S, which lands on the next real frame C's time and is scored against C the way
// the shadow scorer scores a warp: the luma error per class of B, with the no-warp comparison (B against C) beside it. The classes, bins and class fields are the scorer's
// (framegen_shadow_report.h), so the two lines read side by side.
#include "framegen_shadow_report.h"
#include "framegen_slot_report.h"   // Phase 2.3b: kFrameGenFinish*, frameGenFinishName

#include <cstdint>
#include <cstdio>
#include <string>

namespace edvr {

// One eye-frame's decoded result.
struct FrameGenSynthEyeResult {
    FrameGenShadowClassStats cls[kFrameGenShadowClasses];   // the score against C, by B's class (zero when this frame scored nothing)
    uint64_t cells = 0;                                     // the cells scored
    uint64_t filled = 0, left = 0, guard = 0;               // the render pixels this frame's S filled from the background, left holes, and read by the guard
    uint64_t clamped = 0;                                   // the render pixels whose read offset was past the stored range and was clamped (framegen_synth_math.h kFrameGenSynthMaxOffset)
    uint64_t fillReproj = 0, fillStretch = 0;               // of the filled: carried back by the background's motion onto B's background, stretched along it past the occluder
    uint64_t orphan = 0;                                    // the render pixels with a key and no read offer, made holes (framegen_synth_math.h kFrameGenSynthStatsOrphan)
    FrameGenShadowClassStats tcls[kFrameGenShadowClasses];  // the same score with S turned by the head's residual rotation (the compositor's step), by B's class of the cell
    uint64_t tcells = 0, tout = 0;                          // the cells scored turned, and the cells whose turned position fell outside S (not scored)
};

struct FrameGenSynthWindow {
    uint32_t frames[2] = {};      // eye-frames built: [0] the f = 0.5 S, [1] the f = 1 S
    uint32_t scored = 0;          // f = 1 eye-frames scored against the next real frame
    uint32_t skipped = 0;         // eye-frames the module was called for and built nothing (no depth, no vectors, a jump...)
    FrameGenShadowClassStats cls[kFrameGenShadowClasses];
    uint64_t cells = 0;
    uint64_t pixels = 0;          // render pixels of the built eye-frames
    uint64_t filled = 0, left = 0, guard = 0, clamped = 0;
    uint64_t fillReproj = 0, fillStretch = 0;
    uint64_t orphan = 0;
    FrameGenShadowClassStats tcls[kFrameGenShadowClasses];   // the f = 1 S turned by the residual head rotation, scored against C: the raw fields above stay as they were
    uint64_t tcells = 0, tout = 0;
    float rotMedian = 0.0f, rotMax = 0.0f;                   // the residual rotation's angle (true B -> C head rotation against the one S was built for), degrees
    uint32_t rotSamples = 0;
    float gpuMedian[3] = {}, gpuMax[3] = {};
    uint32_t gpuSamples[3] = {};  // the timed frames behind each pair; 0 reads "-(n=0)". [2]: Phase 2.3b's finish of one eye's S (S's sharpening and its UI composite)
    // Phase 2.2: the f = 0.5 builds by the head step they took (the runtime's slot pose / the synthesizer's own extrapolation), where S was written (bits: 1 the game-device private
    // texture, 2 the producer texture directly (uav), 4 a private S copied into it), and the f = 0.5 S not built or not handed over because the previous one was still unconsumed.
    uint32_t poseRuntime = 0, poseFallback = 0;
    uint32_t pathBits = 0;
    uint32_t busy = 0;
    // Phase 2.3b: the f = 0.5 S finished as B is (B's sharpening, B's UI layer by the head's step) / left unfinished (not handed over), and the last reason one was not
    // (framegen_slot_report.h kFrameGenFinish*).
    uint32_t finished = 0, unfinished = 0, unfinishedWhy = 0;
};

inline const char* frameGenSynthPathName(uint32_t bits) {
    switch (bits) {
        case 0: return "none";
        case 1: return "private";
        case 2: return "uav";
        case 4: return "copy";
        default: return "mixed";
    }
}

// Adds one eye-frame's decoded counters (`built` of the kind 0 or 1, `renderPixels` of its render size); `r.cells` is non-zero only on a scored frame.
inline void frameGenSynthMergeFrame(FrameGenSynthWindow& w, int kind, uint64_t renderPixels, const FrameGenSynthEyeResult& r) {
    ++w.frames[kind ? 1 : 0];
    w.pixels += renderPixels;
    w.filled += r.filled;
    w.left += r.left;
    w.guard += r.guard;
    w.clamped += r.clamped;
    w.fillReproj += r.fillReproj;
    w.fillStretch += r.fillStretch;
    w.orphan += r.orphan;
}

inline void frameGenSynthMergeScore(FrameGenSynthWindow& w, const FrameGenSynthEyeResult& r) {
    ++w.scored;
    for (int c = 0; c < kFrameGenShadowClasses; ++c) frameGenShadowAddClass(w.cls[c], r.cls[c]);
    w.cells += r.cells;
    for (int c = 0; c < kFrameGenShadowClasses; ++c) frameGenShadowAddClass(w.tcls[c], r.tcls[c]);
    w.tcells += r.tcells;
    w.tout += r.tout;
}

// ---- the log lines ----
// The format strings are the single source: the DLL logs with them, the rig renders from them, and tools\edvr_log.py --health reads them from this file.
constexpr const char* kFrameGenSynthArmedFmt =
    "frame generation (synth): armed, render %ux%u output %ux%u per eye, S format %u, f=0.5 built on the even frames and f=1 scored against the next real frame (nothing is shown)";
// frames=<f=0.5 built>/<f=1 built> scored=<f=1 frames scored against the next real frame> skipped=<calls that built nothing> gpu_ms=<f=0.5 median/max(n=)>/<f=1 median/max(n=)>
// holes=<filled>%/<left>% of the render pixels guard=<read by the guard>% of them clamp=<read offset clamped>% orphan=<a key and no read offer, made a hole>% (2026-11-08; a line
// from before it ends the tail at clamp=), then the scorer's three class fields: the f = 1 S against C by B's class.
constexpr const char* kFrameGenSynthReportHeadFmt = "frame generation (synth): frames=%u/%u scored=%u skipped=%u";
constexpr const char* kFrameGenSynthReportTailFmt = " gpu_ms=%s/%s holes=%.2f%%/%.2f%% guard=%.2f%% clamp=%.3f%% orphan=%.3f%%";
// Then (2026-11-07) the fill's kinds as shares of the filled pixels: fill_reproj=<carried back by the background's motion onto B's background>% fill_stretch=<stretched along that
// motion past the occluder>% (the rest is the background pull; 0 and 0 when nothing was filled). A line from before it goes from clamp= to the class fields.
constexpr const char* kFrameGenSynthFillTailFmt = " fill_reproj=%.1f%% fill_stretch=%.1f%%";
// Then the same three class fields with the f = 1 S turned by the residual head rotation before it is scored (what the compositor does to a submitted frame: the rotation between the
// pose the frame was built for and the true head): " turned:" far=... cockpit=... world=..., then rot_res_deg=<median>/<max> of that residual and out=<share of the cells whose turned
// position fell outside S, not scored>. One line: the widest it takes is far under Log::note's 1165 characters (framegen_synth_report_test holds it).
constexpr const char* kFrameGenSynthTurnedHeadFmt = " turned:";
constexpr const char* kFrameGenSynthTurnedTailFmt = " rot_res_deg=%s/%s out=%.2f%%";
// Phase 2.2, last on the line: pose=<f=0.5 builds on the runtime's slot pose>/<on the extrapolation> s=<where S was written: private, uav, copy, mixed, none> busy=<f=0.5 S held
// back because the previous one was not consumed>. A line from before it ends at out=.
constexpr const char* kFrameGenSynthSlotTailFmt = " pose=%u/%u s=%s busy=%u";
// Phase 2.3b, after busy= and only in a window that finished or refused to finish an S: finish=<S finished like B>/<left unfinished, not handed over> finish_ms=<the finish's GPU
// time per eye, median/max(n=)> why=<the last reason an S was left unfinished, or finished>.
constexpr const char* kFrameGenSynthFinishTailFmt = " finish=%u/%u finish_ms=%s why=%s";
constexpr const char* kFrameGenSynthRotNumFmt = "%.3f";
constexpr const char* kFrameGenSynthRotNoneFmt = "-";

inline std::string formatFrameGenSynthArmed(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, uint32_t sFormat) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), kFrameGenSynthArmedFmt, static_cast<unsigned>(rw), static_cast<unsigned>(rh), static_cast<unsigned>(ow), static_cast<unsigned>(oh),
                  static_cast<unsigned>(sFormat));
    return buf;
}

// ---- the VRAM line ----
// Said once per eye when the eye's textures are made (framegen_synth.cpp createEye, then ensureScratch), from the descriptors of what was made, as the scorer's line is
// (framegen_shadow_report.h FrameGenShadowVram); tools\framegen_synth_bench prints the same line from its own textures. The eye's groups: S (output size, the upscaler's format),
// the kept S luma, B's luma and class (both the quarter grid), the stats words with their staging copies. The scratch, shared by the eyes: the key grid, the read offsets (render
// size, both R32_UINT) and the hole list.
// Phase 2.2: `shared` is the producer texture S crosses to the runtime in (output size, S's format); on the uav path it replaces S (s is then 0), on the copy path it is beside it.
// The line is said again for an eye whenever that texture is made or let go.
struct FrameGenSynthVram {
    uint64_t s = 0, luma = 0, cls = 0, stats = 0, shared = 0;
    uint64_t key = 0, read = 0, holes = 0;
    uint64_t eye() const { return s + luma + cls + stats + shared; }
    uint64_t scratch() const { return key + read + holes; }
    uint64_t total() const { return 2u * eye() + scratch(); }
};

constexpr const char* kFrameGenSynthVramFmt =
    "frame generation (synth): eye %u VRAM %llu B (%.1f MiB): %s; shared scratch %llu B (%.1f MiB): %s; two eyes and the scratch %llu B (%.1f MiB)";

inline std::string formatFrameGenSynthVram(uint32_t eye, const FrameGenSynthVram& v) {
    const FrameGenVramGroup eg[] = {{"s", v.s}, {"luma", v.luma}, {"class", v.cls}, {"stats", v.stats}, {"shared", v.shared}};
    const FrameGenVramGroup sg[] = {{"key", v.key}, {"read", v.read}, {"holes", v.holes}};
    const std::string e = formatFrameGenVramGroups(eg, 5), s = formatFrameGenVramGroups(sg, 3);
    char buf[400];
    std::snprintf(buf, sizeof(buf), kFrameGenSynthVramFmt, static_cast<unsigned>(eye), static_cast<unsigned long long>(v.eye()), frameGenMiB(v.eye()), e.c_str(),
                  static_cast<unsigned long long>(v.scratch()), frameGenMiB(v.scratch()), s.c_str(), static_cast<unsigned long long>(v.total()), frameGenMiB(v.total()));
    return buf;
}

inline std::string formatFrameGenSynthGpu(const FrameGenSynthWindow& w, int kind) {
    if (!w.gpuSamples[kind]) return kFrameGenShadowGpuNoneFmt;
    char b[48];
    std::snprintf(b, sizeof(b), kFrameGenShadowGpuFmt, static_cast<double>(w.gpuMedian[kind]), static_cast<double>(w.gpuMax[kind]), static_cast<unsigned>(w.gpuSamples[kind]));
    return b;
}

// The one-second line. The caller says it when the module was called in the window (frames built or calls skipped); a window it was never called in says nothing.
inline std::string formatFrameGenSynthReport(const FrameGenSynthWindow& w) {
    char buf[1400];
    int n = std::snprintf(buf, sizeof(buf), kFrameGenSynthReportHeadFmt, static_cast<unsigned>(w.frames[0]), static_cast<unsigned>(w.frames[1]), static_cast<unsigned>(w.scored),
                          static_cast<unsigned>(w.skipped));
    const double px = w.pixels ? static_cast<double>(w.pixels) : 1.0;
    const std::string g0 = formatFrameGenSynthGpu(w, 0), g1 = formatFrameGenSynthGpu(w, 1);
    if (n > 0 && n < static_cast<int>(sizeof(buf)))
        n += std::snprintf(buf + n, sizeof(buf) - n, kFrameGenSynthReportTailFmt, g0.c_str(), g1.c_str(), 100.0 * static_cast<double>(w.filled) / px,
                           100.0 * static_cast<double>(w.left) / px, 100.0 * static_cast<double>(w.guard) / px, 100.0 * static_cast<double>(w.clamped) / px,
                           100.0 * static_cast<double>(w.orphan) / px);
    if (n > 0 && n < static_cast<int>(sizeof(buf))) {
        const double fl = w.filled ? static_cast<double>(w.filled) : 1.0;
        n += std::snprintf(buf + n, sizeof(buf) - n, kFrameGenSynthFillTailFmt, 100.0 * static_cast<double>(w.fillReproj) / fl, 100.0 * static_cast<double>(w.fillStretch) / fl);
    }
    n = formatFrameGenShadowClasses(buf, sizeof(buf), n, w.cls, w.cells);
    if (n > 0 && n < static_cast<int>(sizeof(buf))) n += std::snprintf(buf + n, sizeof(buf) - n, "%s", kFrameGenSynthTurnedHeadFmt);
    n = formatFrameGenShadowClasses(buf, sizeof(buf), n, w.tcls, w.tcells);
    if (n > 0 && n < static_cast<int>(sizeof(buf))) {
        char lo[24], hi[24];
        if (w.rotSamples) {
            std::snprintf(lo, sizeof(lo), kFrameGenSynthRotNumFmt, static_cast<double>(w.rotMedian));
            std::snprintf(hi, sizeof(hi), kFrameGenSynthRotNumFmt, static_cast<double>(w.rotMax));
        } else {
            std::snprintf(lo, sizeof(lo), "%s", kFrameGenSynthRotNoneFmt);
            std::snprintf(hi, sizeof(hi), "%s", kFrameGenSynthRotNoneFmt);
        }
        const double all = static_cast<double>(w.tcells + w.tout);
        n += std::snprintf(buf + n, sizeof(buf) - n, kFrameGenSynthTurnedTailFmt, lo, hi, 100.0 * static_cast<double>(w.tout) / (all > 0.0 ? all : 1.0));
    }
    if (n > 0 && n < static_cast<int>(sizeof(buf)))
        n += std::snprintf(buf + n, sizeof(buf) - n, kFrameGenSynthSlotTailFmt, static_cast<unsigned>(w.poseRuntime), static_cast<unsigned>(w.poseFallback), frameGenSynthPathName(w.pathBits),
                           static_cast<unsigned>(w.busy));
    if ((w.finished || w.unfinished) && n > 0 && n < static_cast<int>(sizeof(buf))) {
        const std::string gf = formatFrameGenSynthGpu(w, 2);
        std::snprintf(buf + n, sizeof(buf) - n, kFrameGenSynthFinishTailFmt, static_cast<unsigned>(w.finished), static_cast<unsigned>(w.unfinished), gf.c_str(),
                      frameGenFinishName(w.unfinished ? w.unfinishedWhy : kFrameGenFinishDone));
    }
    return buf;
}

}  // namespace edvr

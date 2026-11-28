#pragma once
// The frame-generation shadow scorer's statistics and its log lines (fix.frame_generation_shadow, TEMPORARY; docs/design-frame-generation-2026-10-26.md,
// Phase 1): the armed line, the VRAM line and the one-second report. Pure: no Windows or D3D includes, so the test rig and tools\edvr_log.py's --health self-test read
// the same format strings the DLL logs with.
//
// An error is the absolute luma difference between the warped previous frame and the real one, 0..1. The scorer bins it into 48 log bins (frameGenShadowBin),
// keeps a pixel count and a fixed-point sum (error x 1024) per class, and the 1 s report reads percentiles back from the histogram. Errors are printed as
// integers in thousandths of luma ("milli-luma") to keep the line short.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace edvr {

constexpr int kFrameGenShadowBins = 48;
constexpr int kFrameGenShadowClasses = 3;   // 0 far, 1 cockpit, 2 world

enum FrameGenShadowSkip : int {
    kSkipNoPrevDepth = 0,
    kSkipNoPrevDelta,
    kSkipRowsInvalid,
    kSkipCameraJump,
    kSkipReadbackLate,
    kFrameGenShadowSkipCount
};

struct FrameGenShadowClassStats {
    uint32_t pixels = 0;
    uint64_t sumErr1024 = 0;                    // sum of the warped error x 1024
    uint32_t hist[kFrameGenShadowBins] = {};    // the warped error
    uint64_t sumErrId1024 = 0;                  // the same for the no-warp comparison (the same cell of the previous frame)
    uint32_t histId[kFrameGenShadowBins] = {};
};

// The per-class stats of one extra score set of one eye-frame (the mv set of the second kernel).
struct FrameGenShadowEyeBlock {
    FrameGenShadowClassStats cls[kFrameGenShadowClasses];
    uint64_t holes = 0;
    uint64_t cells = 0;
};

// One eye's scored result for one frame.
struct FrameGenShadowEyeResult {
    FrameGenShadowClassStats cls[kFrameGenShadowClasses];
    uint64_t holes = 0;
    uint64_t cells = 0;   // every cell looked at, holes included
    // The second kernel (fgShadowScoreB, scored on every scored frame whatever its set): each B cell classed by B's own depth and carried back to A by the DLSS motion
    // vector (the mv set: the ceiling the vectors give a warp).
    FrameGenShadowEyeBlock mv;
};

// Which stats set a scored frame belongs to: the normal one (predicted transforms) and the jump one (camera_jump frames, predicted transforms).
enum FrameGenShadowSet : int {
    kSetPredicted = 0,
    kSetJump,
    kFrameGenShadowSetCount
};

// The jump and mv sets: the same per-class stats as the window's own, plus how many frames went in.
struct FrameGenShadowSetStats {
    uint32_t frames = 0;
    FrameGenShadowClassStats cls[kFrameGenShadowClasses];
    uint64_t holes = 0;
    uint64_t cells = 0;
};

// The per-frame CPU facts of the window, each already summarised; an n of 0 reads "-" in the line.
struct FrameGenShadowMotion {
    uint32_t motionN = 0;     // |tvCam_B| in mm
    float motionMedian = 0.0f, motionMax = 0.0f;
    uint32_t magN = 0;        // |tvCam_B| / |tvCam_A|, where |tvCam_A| > 1 mm
    float magP5 = 0.0f, magP50 = 0.0f, magP95 = 0.0f;
    uint32_t dtRatioN = 0;    // dt_B / dt_A
    float dtRatioP5 = 0.0f, dtRatioP50 = 0.0f, dtRatioP95 = 0.0f;
    uint32_t dtN = 0;         // dt_B in ms
    float dtMedian = 0.0f, dtMax = 0.0f;
    uint32_t rigDtN = 0;      // the miss in mm of the translation prediction with the ship's step scaled by dt_B / dt_A
    float rigDtMedian = 0.0f, rigDtMax = 0.0f;
};

struct FrameGenShadowWindow {
    uint32_t framesScored = 0;   // the normal (predicted) set's frames
    uint32_t skipped[kFrameGenShadowSkipCount] = {};   // kSkipCameraJump counts the frames routed to the jump set, not frames dropped
    FrameGenShadowClassStats cls[kFrameGenShadowClasses];
    uint64_t holes = 0;
    uint64_t cells = 0;
    FrameGenShadowSetStats jump;
    FrameGenShadowSetStats mv;                          // frames = eye-frames merged, every scored frame of either set
    FrameGenShadowMotion motion;
    float gpuMsMedian = 0.0f, gpuMsMax = 0.0f;
    uint32_t gpuSamples = 0;   // the timed frames behind the two numbers; 0 reads "-" in the line
    float rigDegMedian = 0.0f, rigDegMax = 0.0f, rigMmMedian = 0.0f, rigMmMax = 0.0f;
};

// Bin 0 for e < 1/1024, else clamp(1 + floor(4 * log2(e * 1024)), 1, 47).
inline int frameGenShadowBin(float e) {
    if (!(e >= 1.0f / 1024.0f)) return 0;   // also catches NaN
    const float l = 4.0f * std::log2(e * 1024.0f);
    if (!(l < 46.0f)) return kFrameGenShadowBins - 1;
    const int b = 1 + static_cast<int>(std::floor(l));
    return b < 1 ? 1 : (b > kFrameGenShadowBins - 1 ? kFrameGenShadowBins - 1 : b);
}

// The bin's representative error: the geometric middle of its edges, 2^((bin - 0.5) / 4) / 1024. Bin 0 is 0.
inline float frameGenShadowBinValue(int bin) {
    if (bin <= 0) return 0.0f;
    if (bin > kFrameGenShadowBins - 1) bin = kFrameGenShadowBins - 1;
    return std::exp2((static_cast<float>(bin) - 0.5f) * 0.25f) / 1024.0f;
}

inline uint64_t frameGenShadowHistTotal(const uint32_t hist[kFrameGenShadowBins]) {
    uint64_t n = 0;
    for (int i = 0; i < kFrameGenShadowBins; ++i) n += hist[i];
    return n;
}

// The representative error of the bin where the cumulative count first reaches q of the total (q in 0..1). An empty histogram reads 0.
inline float frameGenShadowPercentile(const uint32_t hist[kFrameGenShadowBins], float q) {
    const uint64_t total = frameGenShadowHistTotal(hist);
    if (total == 0) return 0.0f;
    if (q < 0.0f) q = 0.0f;
    if (q > 1.0f) q = 1.0f;
    uint64_t need = static_cast<uint64_t>(std::ceil(static_cast<double>(q) * static_cast<double>(total)));
    if (need < 1) need = 1;
    uint64_t run = 0;
    for (int i = 0; i < kFrameGenShadowBins; ++i) {
        run += hist[i];
        if (run >= need) return frameGenShadowBinValue(i);
    }
    return frameGenShadowBinValue(kFrameGenShadowBins - 1);
}

// The share (0..1) of the histogram's pixels in whole bins whose representative error is above threshold. An empty histogram reads 0.
inline float frameGenShadowShareOver(const uint32_t hist[kFrameGenShadowBins], float threshold) {
    const uint64_t total = frameGenShadowHistTotal(hist);
    if (total == 0) return 0.0f;
    uint64_t over = 0;
    for (int i = 0; i < kFrameGenShadowBins; ++i)
        if (frameGenShadowBinValue(i) > threshold) over += hist[i];
    return static_cast<float>(static_cast<double>(over) / static_cast<double>(total));
}

inline void frameGenShadowAddClass(FrameGenShadowClassStats& into, const FrameGenShadowClassStats& from) {
    into.pixels += from.pixels;
    into.sumErr1024 += from.sumErr1024;
    into.sumErrId1024 += from.sumErrId1024;
    for (int i = 0; i < kFrameGenShadowBins; ++i) {
        into.hist[i] += from.hist[i];
        into.histId[i] += from.histId[i];
    }
}

// Adds one eye's result into the window: counts, sums and histograms. The caller counts framesScored.
inline void frameGenShadowMerge(FrameGenShadowWindow& w, const FrameGenShadowEyeResult& eye) {
    for (int c = 0; c < kFrameGenShadowClasses; ++c) frameGenShadowAddClass(w.cls[c], eye.cls[c]);
    w.holes += eye.holes;
    w.cells += eye.cells;
}

// The same for the jump set; counts the frame.
inline void frameGenShadowMergeSet(FrameGenShadowSetStats& s, const FrameGenShadowEyeResult& eye) {
    for (int c = 0; c < kFrameGenShadowClasses; ++c) frameGenShadowAddClass(s.cls[c], eye.cls[c]);
    s.holes += eye.holes;
    s.cells += eye.cells;
    ++s.frames;
}

// The same for the second kernel's result: the mv set. The caller picks the frame's own set (above) separately.
inline void frameGenShadowMergeProbe(FrameGenShadowWindow& w, const FrameGenShadowEyeResult& eye) {
    for (int c = 0; c < kFrameGenShadowClasses; ++c) frameGenShadowAddClass(w.mv.cls[c], eye.mv.cls[c]);
    w.mv.holes += eye.mv.holes;
    w.mv.cells += eye.mv.cells;
    ++w.mv.frames;
}

// ---- the log lines ----
// The format strings are the single source: the DLL logs with them, the rig renders from them, and tools\edvr_log.py --health reads them from this file.
constexpr const char* kFrameGenShadowArmedFmt =
    "frame generation (shadow): armed, quarter %ux%u per eye, classes far cockpit world (mover, reactive not scored)";
// The report is one line: head, then one class field per class, then the tail.
constexpr const char* kFrameGenShadowReportHeadFmt = "frame generation (shadow): frames=%u skipped=%u/%u/%u/%u/%u";
// name=share%,mean,p50,p95,over0.02%,over0.05%|id:mean,p95 -- errors in milli-luma, the shares in percent (the class's share is of all cells, the over shares of the class)
constexpr const char* kFrameGenShadowReportClassFmt = " %s=%.1f%%,%u,%u,%u,%.1f%%,%.1f%%|id:%u,%u";
constexpr const char* kFrameGenShadowReportTailFmt = " holes=%.1f%% gpu_ms=%s rig_deg=%.3f/%.3f rig_mm=%.2f/%.2f";
// Appended after rig_mm, so a parser of the older line still matches its head. The jump set (camera_jump frames, predicted transforms; frames=n) followed by the same
// three class fields as above, then the CPU motion facts: motion_mm median/max, mag_ratio p5/p50/p95, dt_ratio p5/p50/p95, dt_ms median/max, rig_dt_mm median/max (a
// number is "-" when nothing was sampled). The main class fields cover only the normal predicted set; in the skipped field the fourth count (camera_jump) is the number
// of frames routed to the jump set. (Lines of the builds before 2026-11-07 carried an oracle set before the jump set, and a bdepth set and the mvdis fields after the mv
// set: tools\edvr_log.py still reads them.)
constexpr const char* kFrameGenShadowJumpHeadFmt = " jump=%u";
// Appended after rig_dt_mm: the second kernel's set as the jump set's (a count of eye-frames merged, then far cockpit world in the class format): mv (B's cell carried
// to A by the DLSS motion vector read at the cell's centre texel), the ceiling the vectors give a warp.
constexpr const char* kFrameGenShadowMvHeadFmt = " mv=%u";
constexpr const char* kFrameGenShadowMotionFmt = " motion_mm=%s/%s mag_ratio=%s/%s/%s dt_ratio=%s/%s/%s dt_ms=%s/%s rig_dt_mm=%s/%s";
constexpr const char* kFrameGenShadowNumFmtMotion = "%.1f";   // motion_mm, dt_ms
constexpr const char* kFrameGenShadowNumFmtRatio = "%.2f";    // mag_ratio, dt_ratio, rig_dt_mm
// The gpu_ms field: median/max over the timed frames and how many there were (the timer runs only on frames the GPU census has its scope open), or "-" when none were.
constexpr const char* kFrameGenShadowGpuFmt = "%.2f/%.2f(n=%u)";
constexpr const char* kFrameGenShadowGpuNoneFmt = "-(n=0)";
constexpr const char* kFrameGenShadowClassNames[kFrameGenShadowClasses] = {"far", "cockpit", "world"};

inline std::string formatFrameGenShadowArmed(uint32_t qw, uint32_t qh) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), kFrameGenShadowArmedFmt, static_cast<unsigned>(qw), static_cast<unsigned>(qh));
    return buf;
}

// ---- the VRAM line ----
// Said once per eye when the eye's resources are made, from the descriptors of what was made (width x height x bytes per texel; a buffer's byte width), by group:
// "<label> eye N VRAM <bytes> B (<MiB>): name=bytes name=bytes ...; <tail>". The synthesizer's line (framegen_synth_report.h) is built the same way.
struct FrameGenVramGroup {
    const char* name;
    uint64_t bytes;
};

inline std::string formatFrameGenVramGroups(const FrameGenVramGroup* groups, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) {
        char b[64];
        std::snprintf(b, sizeof(b), "%s%s=%llu", i ? " " : "", groups[i].name, static_cast<unsigned long long>(groups[i].bytes));
        s += b;
    }
    return s;
}

inline double frameGenMiB(uint64_t bytes) { return static_cast<double>(bytes) / 1048576.0; }

// The scorer's groups: the captures (the quarter-size luma and depth of this frame and the previous, ping-ponged), the key grid and the stats words with their staging copies.
struct FrameGenShadowVram {
    uint64_t captures = 0, keys = 0, stats = 0;
    uint64_t eye() const { return captures + keys + stats; }
};

constexpr const char* kFrameGenShadowVramFmt = "frame generation (shadow): eye %u VRAM %llu B (%.1f MiB): %s; two eyes %llu B (%.1f MiB)";

inline std::string formatFrameGenShadowVram(uint32_t eye, const FrameGenShadowVram& v) {
    const FrameGenVramGroup groups[] = {{"captures", v.captures}, {"keys", v.keys}, {"stats", v.stats}};
    const std::string g = formatFrameGenVramGroups(groups, 3);
    char buf[320];
    std::snprintf(buf, sizeof(buf), kFrameGenShadowVramFmt, static_cast<unsigned>(eye), static_cast<unsigned long long>(v.eye()), frameGenMiB(v.eye()), g.c_str(),
                  static_cast<unsigned long long>(2u * v.eye()), frameGenMiB(2u * v.eye()));
    return buf;
}

inline unsigned frameGenShadowMilli(double luma) {
    return luma <= 0.0 ? 0u : static_cast<unsigned>(luma * 1000.0 + 0.5);
}

// The three class fields of one set, appended at buf + n; returns the new length.
inline int formatFrameGenShadowClasses(char* buf, size_t cap, int n, const FrameGenShadowClassStats cls[kFrameGenShadowClasses], uint64_t cellsTotal) {
    const double cells = cellsTotal ? static_cast<double>(cellsTotal) : 1.0;
    for (int c = 0; c < kFrameGenShadowClasses && n > 0 && n < static_cast<int>(cap); ++c) {
        const FrameGenShadowClassStats& s = cls[c];
        const double px = s.pixels ? static_cast<double>(s.pixels) : 1.0;
        const double mean = static_cast<double>(s.sumErr1024) / 1024.0 / px;
        const double meanId = static_cast<double>(s.sumErrId1024) / 1024.0 / px;
        n += std::snprintf(buf + n, cap - n, kFrameGenShadowReportClassFmt, kFrameGenShadowClassNames[c], 100.0 * static_cast<double>(s.pixels) / cells,
                           frameGenShadowMilli(s.pixels ? mean : 0.0), frameGenShadowMilli(frameGenShadowPercentile(s.hist, 0.50f)),
                           frameGenShadowMilli(frameGenShadowPercentile(s.hist, 0.95f)), 100.0 * frameGenShadowShareOver(s.hist, 0.02f),
                           100.0 * frameGenShadowShareOver(s.hist, 0.05f), frameGenShadowMilli(s.pixels ? meanId : 0.0),
                           frameGenShadowMilli(frameGenShadowPercentile(s.histId, 0.95f)));
    }
    return n;
}

// One number of the motion field, or "-" when nothing was sampled.
inline std::string formatFrameGenShadowNum(const char* fmt, uint32_t samples, float v) {
    if (!samples) return "-";
    char b[32];
    std::snprintf(b, sizeof(b), fmt, static_cast<double>(v));
    return b;
}

inline std::string formatFrameGenShadowMotion(const FrameGenShadowMotion& m) {
    const char* const f1 = kFrameGenShadowNumFmtMotion;
    const char* const f2 = kFrameGenShadowNumFmtRatio;
    const std::string a[12] = {
        formatFrameGenShadowNum(f1, m.motionN, m.motionMedian), formatFrameGenShadowNum(f1, m.motionN, m.motionMax),
        formatFrameGenShadowNum(f2, m.magN, m.magP5), formatFrameGenShadowNum(f2, m.magN, m.magP50), formatFrameGenShadowNum(f2, m.magN, m.magP95),
        formatFrameGenShadowNum(f2, m.dtRatioN, m.dtRatioP5), formatFrameGenShadowNum(f2, m.dtRatioN, m.dtRatioP50), formatFrameGenShadowNum(f2, m.dtRatioN, m.dtRatioP95),
        formatFrameGenShadowNum(f1, m.dtN, m.dtMedian), formatFrameGenShadowNum(f1, m.dtN, m.dtMax),
        formatFrameGenShadowNum(f2, m.rigDtN, m.rigDtMedian), formatFrameGenShadowNum(f2, m.rigDtN, m.rigDtMax)};
    char buf[320];
    std::snprintf(buf, sizeof(buf), kFrameGenShadowMotionFmt, a[0].c_str(), a[1].c_str(), a[2].c_str(), a[3].c_str(), a[4].c_str(), a[5].c_str(), a[6].c_str(), a[7].c_str(),
                  a[8].c_str(), a[9].c_str(), a[10].c_str(), a[11].c_str());
    return buf;
}

inline std::string formatFrameGenShadowReport(const FrameGenShadowWindow& w) {
    char buf[1400];
    int n = std::snprintf(buf, sizeof(buf), kFrameGenShadowReportHeadFmt, static_cast<unsigned>(w.framesScored), static_cast<unsigned>(w.skipped[0]),
                          static_cast<unsigned>(w.skipped[1]), static_cast<unsigned>(w.skipped[2]), static_cast<unsigned>(w.skipped[3]),
                          static_cast<unsigned>(w.skipped[4]));
    n = formatFrameGenShadowClasses(buf, sizeof(buf), n, w.cls, w.cells);
    const double cells = w.cells ? static_cast<double>(w.cells) : 1.0;
    char gpu[48];
    if (w.gpuSamples)
        std::snprintf(gpu, sizeof(gpu), kFrameGenShadowGpuFmt, static_cast<double>(w.gpuMsMedian), static_cast<double>(w.gpuMsMax), static_cast<unsigned>(w.gpuSamples));
    else
        std::snprintf(gpu, sizeof(gpu), "%s", kFrameGenShadowGpuNoneFmt);
    if (n > 0 && n < static_cast<int>(sizeof(buf)))
        std::snprintf(buf + n, sizeof(buf) - n, kFrameGenShadowReportTailFmt, 100.0 * static_cast<double>(w.holes) / cells, gpu,
                      static_cast<double>(w.rigDegMedian), static_cast<double>(w.rigDegMax), static_cast<double>(w.rigMmMedian),
                      static_cast<double>(w.rigMmMax));
    n = static_cast<int>(std::strlen(buf));
    if (n < static_cast<int>(sizeof(buf))) {
        n += std::snprintf(buf + n, sizeof(buf) - n, kFrameGenShadowJumpHeadFmt, static_cast<unsigned>(w.jump.frames));
        n = formatFrameGenShadowClasses(buf, sizeof(buf), n, w.jump.cls, w.jump.cells);
    }
    std::string out = buf;
    out += formatFrameGenShadowMotion(w.motion);
    // The second kernel's set, last.
    char tail[320];
    int m = std::snprintf(tail, sizeof(tail), kFrameGenShadowMvHeadFmt, static_cast<unsigned>(w.mv.frames));
    formatFrameGenShadowClasses(tail, sizeof(tail), m, w.mv.cls, w.mv.cells);
    out += tail;
    return out;
}

}  // namespace edvr

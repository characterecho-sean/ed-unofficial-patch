#pragma once
// Frame generation, Phase 2.4 (fix.frame_generation = auto; docs/design-frame-generation-2026-10-26.md, "### The gate"): the performance gate. Lock (framegen_lock_report.h) engages
// in `auto` only while this gate admits it, on top of Lock's own conditions; `on` bypasses it (Lock forced) and `off` never reaches it.
// Pure: no D3D, no OpenXR, no clock, no config. The d3d11 half (src\d3d11\perf_monitor.cpp, frameGenGateFrame) feeds it once a frame from what it already measures (the Monitor page's
// GPU TIME and CPU TIME series, the synthesizer's last window, the journal's Status.json, the runtime's last frame generation slot) and carries the decision to the runtime in word 17
// of the frame answer (frameGenLockWord). Its one-second line (kFrameGenGateReportFmt) is the H1 evidence the design asked for; tools\edvr_log.py --health reads the formats here.
// The rig is tools\framegen_synth_report_test (the gate's table).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace edvr {

// fix.frame_generation's three values. The ini's text is the functionality; anything else reads off.
enum class FrameGenMode : uint8_t { Off = 0, Auto = 1, On = 2 };
inline FrameGenMode frameGenModeParse(const char* v) {
    if (!v) return FrameGenMode::Off;
    if (std::strcmp(v, "on") == 0) return FrameGenMode::On;
    if (std::strcmp(v, "auto") == 0) return FrameGenMode::Auto;
    return FrameGenMode::Off;
}
inline const char* frameGenModeName(FrameGenMode m) { return m == FrameGenMode::On ? "on" : m == FrameGenMode::Auto ? "auto" : "off"; }

// Word 17 of a version 12 frame answer (native_frame.h EDVR_NATIVE_FRAME_FRAMEGEN_LOCK_WORD). A runtime from 2.3 reads only 1 as on, so both auto values read off there; a d3d11.dll
// from 2.3 writes only 0 and 1 (it reads `auto` in the ini as off).
constexpr uint32_t kFrameGenWordOff = 0u;
constexpr uint32_t kFrameGenWordOn = 1u;          // Lock forced (fix.frame_generation = on)
constexpr uint32_t kFrameGenWordAutoClosed = 2u;  // auto, the gate does not admit Lock now: the synthesizer and the slot run, Lock does not engage (reason auto_gate)
constexpr uint32_t kFrameGenWordAutoOpen = 3u;    // auto, the gate admits Lock: Lock engages on its own conditions
inline uint32_t frameGenLockWord(FrameGenMode m, bool admit) {
    return m == FrameGenMode::On ? kFrameGenWordOn : m == FrameGenMode::Auto ? (admit ? kFrameGenWordAutoOpen : kFrameGenWordAutoClosed) : kFrameGenWordOff;
}
inline bool frameGenWordArmed(uint32_t w) { return w >= kFrameGenWordOn && w <= kFrameGenWordAutoOpen; }      // the runtime runs the slot and arms Lock
inline bool frameGenWordAdmits(uint32_t w) { return w == kFrameGenWordOn || w == kFrameGenWordAutoOpen; }    // Lock may engage
inline FrameGenMode frameGenWordMode(uint32_t w) {
    return w == kFrameGenWordOn ? FrameGenMode::On : (w == kFrameGenWordAutoClosed || w == kFrameGenWordAutoOpen) ? FrameGenMode::Auto : FrameGenMode::Off;
}

// The hotkey (hotkey.frame_generation): off goes to the last value that was not off (auto until one was seen), anything else to off, remembered.
inline FrameGenMode frameGenToggle(FrameGenMode current, FrameGenMode& lastOn) {
    if (current == FrameGenMode::Off) return lastOn == FrameGenMode::Off ? FrameGenMode::Auto : lastOn;
    lastOn = current;
    return FrameGenMode::Off;
}

enum class FrameGenGateState : uint8_t { Off = 0, Waiting, Engaged };
inline const char* frameGenGateStateName(FrameGenGateState s) { return s == FrameGenGateState::Engaged ? "engaged" : s == FrameGenGateState::Waiting ? "waiting" : "off"; }

// Why the gate does not admit Lock (or why it let go), in the order frameGenGateTerms tests them.
enum class FrameGenGateReason : uint8_t {
    None = 0,      // engaged: the gate admits Lock
    KeyOff,        // fix.frame_generation is off (or the flat profile)
    Forced,        // fix.frame_generation = on: the performance terms are bypassed
    NoRuntime,     // the runtime handed no frame generation slot that knows auto (an openvr_api.dll older than 2.4, or no pair treated for 2 s)
    NoDisplay,     // the display's period is not known
    NoGpu,         // no GPU frame time in the last 2 s (the Monitor page's GPU TIME series)
    NoCpu,         // no game-thread CPU time in the last 2 s (the CPU TIME series)
    NoSynth,       // no synthesizer cost in its last window (it built no S)
    OnFoot,        // on foot: not admitted in v1 (unflown)
    Context,       // neither in a ship nor in an SRV (Status.json does not say: a menu, a taxi, a load)
    Cut,           // a cut in the last 2 s (the line names it), or one now while engaged
    FullRate,      // Elite holds full rate: its real rate is at least 0.9 of the display's and the runtime has not halved its period
    OverGpu,       // H1 fails on the GPU: Elite's GPU frame (median) plus the synthesizer's cost is not under 0.85 of two display periods
    OverCpu,       // H1 fails on the CPU: Elite's game-thread time (median) is not under 0.85 of two display periods
    FitsFullRate,  // engaged, Elite's work (GPU and CPU medians, no synthesizer) stayed under 0.75 of one period for 3 s: it could hold full rate
    TooSlow,       // engaged, Elite missed its two-period budget (late_real) on more than 5% of the pairs over 2 s (3:1 is not offered)
    Dwell,         // every term holds, for less than 2 s so far
    Rearm,         // every term has held 2 s, but less than 5 s have passed since the gate last let go
    Count
};
inline const char* frameGenGateReasonName(FrameGenGateReason r) {
    switch (r) {
        case FrameGenGateReason::None: return "none";
        case FrameGenGateReason::KeyOff: return "key_off";
        case FrameGenGateReason::Forced: return "forced";
        case FrameGenGateReason::NoRuntime: return "no_runtime";
        case FrameGenGateReason::NoDisplay: return "no_display_rate";
        case FrameGenGateReason::NoGpu: return "no_gpu_time";
        case FrameGenGateReason::NoCpu: return "no_cpu_time";
        case FrameGenGateReason::NoSynth: return "no_synth_cost";
        case FrameGenGateReason::OnFoot: return "on_foot";
        case FrameGenGateReason::Context: return "context";
        case FrameGenGateReason::Cut: return "cut";
        case FrameGenGateReason::FullRate: return "full_rate";
        case FrameGenGateReason::OverGpu: return "over_budget_gpu";
        case FrameGenGateReason::OverCpu: return "over_budget_cpu";
        case FrameGenGateReason::FitsFullRate: return "fits_full_rate";
        case FrameGenGateReason::TooSlow: return "too_slow";
        case FrameGenGateReason::Dwell: return "dwell";
        case FrameGenGateReason::Rearm: return "rearm";
        default: return "unknown";
    }
}

// Where the player is (journal Status.json Flags). v1 admits a ship (main ship or fighter, supercruise included) and the SRV: flown in Phase 1 and 2 (flights 6 and 12). Not on
// foot: unflown. A deliberate widening of the design's surface-only v1 (the journal's 2026-11-08 gate entry).
enum class FrameGenGateContext : uint8_t { Unknown = 0, Vehicle, OnFoot };
inline const char* frameGenGateContextName(FrameGenGateContext c) { return c == FrameGenGateContext::Vehicle ? "vehicle" : c == FrameGenGateContext::OnFoot ? "on_foot" : "unknown"; }

// A cut seen this frame. Never synthesized across: the gate lets go at once and needs 2 s without one before it counts again.
enum class FrameGenGateCut : uint8_t {
    None = 0,
    Screen,       // Status.json GuiFocus is a full screen (5 station services and up: the galaxy and system maps, the orrery, the FSS, the codex)
    Tunnel,       // the hyperspace jump tunnel (the journal's jump latch)
    Camera,       // the temporal pass's camera gate dropped or carried the frame's rows, or the origin jumped
    Stall,        // no pair from the runtime for over kFrameGenGateStallMs (a load, a hitch, the game paused)
    Projection,   // the runtime says the last frame was not a fresh pair (Lock's no_projection: a load, a withheld transition frame, a flat clear)
    Count
};
inline const char* frameGenGateCutName(FrameGenGateCut c) {
    switch (c) {
        case FrameGenGateCut::None: return "none";
        case FrameGenGateCut::Screen: return "screen";
        case FrameGenGateCut::Tunnel: return "tunnel";
        case FrameGenGateCut::Camera: return "camera";
        case FrameGenGateCut::Stall: return "stall";
        case FrameGenGateCut::Projection: return "projection";
        default: return "unknown";
    }
}

constexpr uint64_t kFrameGenGateEngageMs = 2000;   // every term holds this long before the gate admits Lock
constexpr uint64_t kFrameGenGateRearmMs = 5000;    // after the gate lets go: at least this long before it admits Lock again
constexpr uint64_t kFrameGenGateCutMs = 2000;      // no cut for this long before the terms count again
constexpr uint64_t kFrameGenGateFitsMs = 3000;     // engaged, Elite fits one period this long: the gate lets go (fits_full_rate)
constexpr uint64_t kFrameGenGateWindowMs = 2000;   // the medians, maxima, real rate and misses are over this window
constexpr uint64_t kFrameGenGateStallMs = 250;     // no pair for this long is a cut (stall)
constexpr double kFrameGenGateRate = 0.9;          // Elite cannot hold full rate: its real rate under this share of the display's
constexpr double kFrameGenGateHalved = 1.5;        // the runtime has halved its predicted period: over this many display periods
constexpr double kFrameGenGateBudget = 0.85;       // H1: Elite's frame fits under this share of two display periods
constexpr double kFrameGenGateFull = 0.75;         // engaged: Elite's work under this share of one period is full rate
constexpr double kFrameGenGateLateShare = 0.05;    // engaged: late_real over this share of the pairs is too slow
constexpr uint32_t kFrameGenGateLateMinPairs = 10; // ... judged only over at least this many pairs

// What the gate is given, once a frame. The measurements are over the last kFrameGenGateWindowMs; a count of 0 is a measurement that is missing.
struct FrameGenGateInputs {
    FrameGenMode mode = FrameGenMode::Off;
    bool runtime = false;            // the runtime's last slot (fresh) knows auto (slot size 4)
    double displayMs = 0.0;          // the display's period (the runtime's published refresh, else the session's first predicted period); 0 unknown
    double predictedMs = 0.0;        // the runtime's predicted period now; 0 unknown
    double realFps = 0.0;            // Elite's real frames a second (the CPU series' count over the window)
    uint32_t gpuN = 0;               // Elite's GPU frame (the Monitor page's GPU TIME series: the application render, else the frame's GPU work span)
    double gpuMedMs = 0.0, gpuMaxMs = 0.0;
    uint32_t cpuN = 0;               // Elite's game-thread time per frame (the CPU TIME series: caller work, else the pre-submit time of an older runtime)
    double cpuMedMs = 0.0, cpuMaxMs = 0.0;
    bool synthKnown = false;         // the synthesizer built S in its last window
    double synthMs = 0.0;            // its cost a pair: both eyes, the build plus the finish (medians)
    // The share of Elite's frames in the GPU window on which the synthesizer ran (0..1). It runs inside the treat, which lies inside the GPU figure (the application segment
    // closes after the treat; the work span ends after the door), so on a frame that built S the GPU figure already holds synthMs: frameGenGateEliteGpuMs takes it out again.
    double synthShare = 0.0;
    uint32_t pairs = 0, lateReal = 0;   // Lock's last two one-second windows: S slots run while engaged, and the display periods Elite's frame missed (late_real)
    FrameGenGateContext context = FrameGenGateContext::Unknown;
    FrameGenGateCut cut = FrameGenGateCut::None;   // a cut this frame
};

// The instantaneous terms, no history. engaged: the gate admits Lock now (the rate and H1 terms are the engage test only; fits and too_slow are the engaged test only).
struct FrameGenGateTerms {
    FrameGenGateReason blocker = FrameGenGateReason::None;
    double budgetMs = 0.0;   // 0.85 x two display periods (0: unknown)
    double workMs = 0.0;     // the larger of GPU median + synthesizer and CPU median
    double marginMs = 0.0;   // budget - work
    bool fitsFull = false;   // GPU and CPU medians under 0.75 of one period
    bool tooSlow = false;    // late_real over 5% of at least 10 pairs
};
// Elite's own GPU frame: the GPU median less the synthesizer's cost when the median is a frame that built S (more than half the window's frames did: on, auto engaged, the
// shadow key), so the synthesizer is counted once, by synthMs. A window where it ran on few frames (auto closed: one a second) has a median without it, taken as it is.
inline double frameGenGateEliteGpuMs(const FrameGenGateInputs& in) {
    if (!in.synthKnown || !(in.synthShare > 0.5)) return in.gpuMedMs;
    const double g = in.gpuMedMs - in.synthMs;
    return g > 0.0 ? g : 0.0;
}
inline FrameGenGateTerms frameGenGateTerms(const FrameGenGateInputs& in, bool engaged) {
    FrameGenGateTerms t;
    const double eliteGpu = frameGenGateEliteGpuMs(in);
    if (in.displayMs > 0.0) {
        t.budgetMs = kFrameGenGateBudget * 2.0 * in.displayMs;
        const double gpu = eliteGpu + (in.synthKnown ? in.synthMs : 0.0);
        t.workMs = gpu > in.cpuMedMs ? gpu : in.cpuMedMs;
        t.marginMs = t.budgetMs - t.workMs;
        t.fitsFull = in.gpuN && in.cpuN && eliteGpu < kFrameGenGateFull * in.displayMs && in.cpuMedMs < kFrameGenGateFull * in.displayMs;
    }
    t.tooSlow = in.pairs >= kFrameGenGateLateMinPairs && static_cast<double>(in.lateReal) > kFrameGenGateLateShare * static_cast<double>(in.pairs);
    if (!in.runtime) t.blocker = FrameGenGateReason::NoRuntime;
    else if (in.displayMs <= 0.0) t.blocker = FrameGenGateReason::NoDisplay;
    else if (!in.gpuN) t.blocker = FrameGenGateReason::NoGpu;
    else if (!in.cpuN) t.blocker = FrameGenGateReason::NoCpu;
    else if (!in.synthKnown) t.blocker = FrameGenGateReason::NoSynth;
    else if (in.context == FrameGenGateContext::OnFoot) t.blocker = FrameGenGateReason::OnFoot;
    else if (in.context != FrameGenGateContext::Vehicle) t.blocker = FrameGenGateReason::Context;
    else if (!engaged) {
        const bool slow = in.realFps < kFrameGenGateRate * 1000.0 / in.displayMs;
        const bool halved = in.predictedMs > kFrameGenGateHalved * in.displayMs;
        if (!slow && !halved) t.blocker = FrameGenGateReason::FullRate;
        else if (!(eliteGpu + in.synthMs < t.budgetMs)) t.blocker = FrameGenGateReason::OverGpu;
        else if (!(in.cpuMedMs < t.budgetMs)) t.blocker = FrameGenGateReason::OverCpu;
    }
    return t;
}

// The gate with its dwell. step() once a frame (or at any cadence: every hold is wall time). Admits Lock after every term has held kFrameGenGateEngageMs with no cut in the last
// kFrameGenGateCutMs and at least kFrameGenGateRearmMs since it last let go; lets go at once on a cut, a missing measurement or a context it does not admit, on too_slow, and on
// fits_full_rate after kFrameGenGateFitsMs. `on` reads engaged (forced) and `off` off, and both start the dwell over.
struct FrameGenGateStep {
    FrameGenGateState state = FrameGenGateState::Off;
    FrameGenGateReason reason = FrameGenGateReason::KeyOff;
    bool edge = false;                                   // engaged changed at this step (auto only)
    FrameGenGateCut cut = FrameGenGateCut::None;         // the last cut seen while auto (none before one)
    FrameGenGateTerms terms;
};
class FrameGenGate {
public:
    FrameGenGateStep step(const FrameGenGateInputs& in, uint64_t nowMs) {
        FrameGenGateStep s;
        if (in.mode != FrameGenMode::Auto) {
            *this = FrameGenGate{};
            s.state = in.mode == FrameGenMode::On ? FrameGenGateState::Engaged : FrameGenGateState::Off;
            s.reason = in.mode == FrameGenMode::On ? FrameGenGateReason::Forced : FrameGenGateReason::KeyOff;
            s.terms = frameGenGateTerms(in, true);
            return last_ = s;
        }
        if (in.cut != FrameGenGateCut::None) {
            cutSeen_ = true;
            lastCutMs_ = nowMs;
            lastCut_ = in.cut;
        }
        const FrameGenGateTerms t = frameGenGateTerms(in, engaged_);
        FrameGenGateReason reason = FrameGenGateReason::None;
        if (engaged_) {
            if (t.blocker != FrameGenGateReason::None) reason = t.blocker;
            else if (in.cut != FrameGenGateCut::None) reason = FrameGenGateReason::Cut;
            else if (t.tooSlow) reason = FrameGenGateReason::TooSlow;
            else if (t.fitsFull) {
                if (!fitting_) { fitting_ = true; fitsSinceMs_ = nowMs; }
                if (nowMs >= fitsSinceMs_ && nowMs - fitsSinceMs_ >= kFrameGenGateFitsMs) reason = FrameGenGateReason::FitsFullRate;
            } else {
                fitting_ = false;
            }
            if (reason != FrameGenGateReason::None) {
                engaged_ = holding_ = fitting_ = false;
                disengagedAtMs_ = nowMs;
                everLetGo_ = true;
                s.edge = true;
            }
        } else {
            const bool recentCut = cutSeen_ && nowMs >= lastCutMs_ && nowMs - lastCutMs_ < kFrameGenGateCutMs;
            reason = t.blocker != FrameGenGateReason::None ? t.blocker : recentCut ? FrameGenGateReason::Cut : FrameGenGateReason::None;
            if (reason != FrameGenGateReason::None) {
                holding_ = false;
            } else {
                if (!holding_) { holding_ = true; holdSinceMs_ = nowMs; }
                const bool held = nowMs >= holdSinceMs_ && nowMs - holdSinceMs_ >= kFrameGenGateEngageMs;
                const bool rearmed = !everLetGo_ || (nowMs >= disengagedAtMs_ && nowMs - disengagedAtMs_ >= kFrameGenGateRearmMs);
                if (held && rearmed) {
                    engaged_ = true;
                    fitting_ = false;
                    s.edge = true;
                } else {
                    reason = held ? FrameGenGateReason::Rearm : FrameGenGateReason::Dwell;
                }
            }
        }
        s.state = engaged_ ? FrameGenGateState::Engaged : FrameGenGateState::Waiting;
        s.reason = engaged_ ? FrameGenGateReason::None : reason;
        s.cut = lastCut_;
        s.terms = t;
        return last_ = s;
    }
    bool engaged() const { return engaged_; }
    const FrameGenGateStep& last() const { return last_; }

private:
    bool engaged_ = false, holding_ = false, fitting_ = false, cutSeen_ = false, everLetGo_ = false;
    uint64_t holdSinceMs_ = 0, fitsSinceMs_ = 0, lastCutMs_ = 0, disengagedAtMs_ = 0;
    FrameGenGateCut lastCut_ = FrameGenGateCut::None;
    FrameGenGateStep last_;
};

// ---- the lines (the graphics log, untagged; tools\edvr_log.py --health reads these formats) ----
// Once a second while fix.frame_generation is auto or on. state and reason: the gate's (on: engaged, forced). real_fps: Elite's real frames a second over 2 s / the display's rate;
// period_ms: the runtime's predicted period / the display's; gpu_ms, cpu_ms: median/max over 2 s; synth_ms: the synthesizer's cost a pair (both eyes, build + finish); budget_ms:
// 0.85 x two display periods; margin_ms: the budget less the larger of gpu + synth and cpu (the H1 margin); late_real: Lock's misses / its S slots over 2 s; context: vehicle,
// on_foot or unknown; cut: the last cut seen while auto; lock: Lock's own reason as the runtime last said it (none: engaged; unknown: no slot). "-": not measured.
constexpr const char* kFrameGenGateReportFmt =
    "frame generation gate: state=%s reason=%s mode=%s real_fps=%s/%s period_ms=%s/%s gpu_ms=%s/%s cpu_ms=%s/%s synth_ms=%s budget_ms=%s margin_ms=%s late_real=%u/%u "
    "context=%s cut=%s lock=%s";
// Each edge of the gate (auto only).
constexpr const char* kFrameGenGateEngageFmt = "frame generation gate: engage reason=held_2s margin_ms=%.2f";
constexpr const char* kFrameGenGateDisengageFmt = "frame generation gate: disengage reason=%s cut=%s";
// Said once a session: auto with a runtime that never handed a slot of 2.4's size while Elite rendered in VR for kFrameGenGateOldRuntimeS.
constexpr uint32_t kFrameGenGateOldRuntimeS = 10;
constexpr const char* kFrameGenGateOldRuntimeFmt =
    "frame generation gate: auto needs this release's openvr_api.dll (the runtime handed no frame generation slot that knows auto in %u s of VR frames); frame generation stays off "
    "and the synthesizer is held";

inline std::string frameGenGateNum(bool known, double v) {
    if (!known) return "-";
    char b[24];
    std::snprintf(b, sizeof(b), "%.2f", v);
    return b;
}

// lockReason: the runtime's FrameGenLockReason name (the caller maps it; "unknown" with no slot).
inline std::string formatFrameGenGateReport(const FrameGenGateStep& s, const FrameGenGateInputs& in, FrameGenMode mode, const char* lockReason) {
    char buf[512];
    const bool display = in.displayMs > 0.0;
    const std::string fps = frameGenGateNum(in.cpuN != 0, in.realFps), hz = frameGenGateNum(display, display ? 1000.0 / in.displayMs : 0.0);
    const std::string pp = frameGenGateNum(in.predictedMs > 0.0, in.predictedMs), pd = frameGenGateNum(display, in.displayMs);
    const std::string g0 = frameGenGateNum(in.gpuN != 0, in.gpuMedMs), g1 = frameGenGateNum(in.gpuN != 0, in.gpuMaxMs);
    const std::string c0 = frameGenGateNum(in.cpuN != 0, in.cpuMedMs), c1 = frameGenGateNum(in.cpuN != 0, in.cpuMaxMs);
    const std::string sy = frameGenGateNum(in.synthKnown, in.synthMs), bu = frameGenGateNum(display, s.terms.budgetMs);
    const std::string ma = frameGenGateNum(display && in.gpuN && in.cpuN, s.terms.marginMs);
    std::snprintf(buf, sizeof(buf), kFrameGenGateReportFmt, frameGenGateStateName(s.state), frameGenGateReasonName(s.reason), frameGenModeName(mode), fps.c_str(), hz.c_str(),
                  pp.c_str(), pd.c_str(), g0.c_str(), g1.c_str(), c0.c_str(), c1.c_str(), sy.c_str(), bu.c_str(), ma.c_str(), static_cast<unsigned>(in.lateReal),
                  static_cast<unsigned>(in.pairs), frameGenGateContextName(in.context), frameGenGateCutName(s.cut), lockReason ? lockReason : "unknown");
    return buf;
}
inline std::string formatFrameGenGateEdge(const FrameGenGateStep& s) {
    char buf[160];
    if (s.state == FrameGenGateState::Engaged)
        std::snprintf(buf, sizeof(buf), kFrameGenGateEngageFmt, s.terms.marginMs);
    else
        std::snprintf(buf, sizeof(buf), kFrameGenGateDisengageFmt, frameGenGateReasonName(s.reason), frameGenGateCutName(s.cut));
    return buf;
}

// The Monitor page's gate line (common/framegen_monitor_text.h), auto only.
inline void formatFrameGenGateMonitor(FrameGenGateState state, FrameGenGateReason reason, char* out, size_t size) {
    if (state == FrameGenGateState::Engaged)
        std::snprintf(out, size, "%s", "Gate: engaged (Lock allowed)");
    else if (reason == FrameGenGateReason::NoRuntime)
        std::snprintf(out, size, "%s", "Gate: waiting (no_runtime: openvr_api.dll older than auto?)");
    else
        std::snprintf(out, size, "Gate: waiting (%s)", frameGenGateReasonName(reason));
}

}  // namespace edvr

#pragma once
// Frame generation on the settings menu's Monitor page (docs/settings-menu.md, "FRAME GENERATION"): the block's text, from what the two halves already keep. Pure: no D3D, no
// clock, no config. The d3d11 half gathers the input once a second while the page shows (src\d3d11\perf_monitor.cpp) and the menu draws the lines; the rig
// (tools\framegen_synth_report_test) holds the exact text. Drawn only while fix.frame_generation or fix.frame_generation_shadow is on: with both off there is no block.
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "framegen_gate.h"   // the gate's line (auto)
#include "framegen_lock_report.h"

namespace edvr {

// The runtime's half, from the last slot it handed (common/native_temporal.h EdvrNativeFrameGenSlot).
constexpr uint32_t kFrameGenMonitorNoSlot = 0;     // no slot handed since the d3d11.dll loaded (no runtime of EDVR's, or no pair treated yet)
constexpr uint32_t kFrameGenMonitorOldSlot = 1;    // the last slot was size 1 or 2: a runtime from before the Monitor fields; its Lock state is unknown here
constexpr uint32_t kFrameGenMonitorSlot = 2;       // a size 3 slot: the fields below are the runtime's
constexpr uint64_t kFrameGenMonitorStaleMs = 2000; // a slot older than this is not the state now (no pair treated: a load, the game paused)
struct FrameGenMonitorRuntime {
    uint32_t known = kFrameGenMonitorNoSlot;
    uint64_t ageMs = 0;                 // since the last slot was handed
    uint32_t reason = 0;                // FrameGenLockReason
    bool lastSecond = false;
    uint32_t sSlots = 0, fromS = 0, lateS = 0, lateReal = 0;
    float slotDegMedian = -1.0f;
    uint64_t xrBytes = 0;
    bool autoKnown = false;             // a size 4 slot: the runtime reads fix.frame_generation = auto (Phase 2.4)
};
// The synthesizer's last closed one-second window (src\d3d11\framegen_synth.cpp, its log line's gpu_ms and finish_ms) and the game device's bytes the synthesizer and the scorer
// hold (their VRAM lines' groups).
struct FrameGenMonitorSynth {
    bool valid = false;                 // a window has closed with the synthesizer called in it
    uint32_t built = 0;                 // f = 0.5 S built in it (both eyes)
    float gpuMs = 0.0f;                 // the f = 0.5 build per eye, median
    uint32_t gpuN = 0;
    float finishMs = 0.0f;              // the finish per eye, median
    uint32_t finishN = 0;
    uint64_t gameBytes = 0;
};
struct FrameGenMonitorInput {
    bool lockKey = false, shadowKey = false;   // lockKey: fix.frame_generation is auto or on
    bool autoMode = false;                      // fix.frame_generation = auto: the gate's line is drawn (Phase 2.4)
    FrameGenGateState gateState = FrameGenGateState::Off;
    FrameGenGateReason gateReason = FrameGenGateReason::KeyOff;
    FrameGenMonitorRuntime runtime;
    FrameGenMonitorSynth synth;
};

constexpr const char* kFrameGenMonitorHeading = "FRAME GENERATION";
constexpr int kFrameGenMonitorMaxLines = 5;
constexpr size_t kFrameGenMonitorLineChars = 96;   // MenuLine::left

inline double frameGenMonitorMiB(uint64_t b) { return static_cast<double>(b) / (1024.0 * 1024.0); }

// The block's lines under its heading, at most kFrameGenMonitorMaxLines; 0 with both keys off.
inline int formatFrameGenMonitor(const FrameGenMonitorInput& in, char (*lines)[kFrameGenMonitorLineChars], int max) {
    if ((!in.lockKey && !in.shadowKey) || max <= 0) return 0;
    int n = 0;
    const auto put = [&](const char* fmt, auto... a) {
        if (n < max) std::snprintf(lines[n++], kFrameGenMonitorLineChars, fmt, a...);
    };
    const FrameGenMonitorRuntime& r = in.runtime;
    const bool rtKnown = r.known == kFrameGenMonitorSlot && r.ageMs <= kFrameGenMonitorStaleMs;
    // The state.
    if (!in.lockKey) {
        put("%s", "State: shadow (S is synthesized, nothing is shown)");
    } else if (r.known == kFrameGenMonitorNoSlot) {
        put("%s", "State: Lock unknown (no slot from the runtime yet)");
    } else if (r.known == kFrameGenMonitorOldSlot) {
        put("%s", "State: Lock unknown (the runtime is older than this d3d11.dll)");
    } else if (!rtKnown) {
        put("State: Lock unknown (no slot from the runtime for %.0f s)", static_cast<double>(r.ageMs) / 1000.0);
    } else if (r.reason == static_cast<uint32_t>(FrameGenLockReason::None)) {
        put("%s", "State: Lock engaged");
    } else {
        put("State: Lock waiting (%s)", frameGenLockReasonName(static_cast<FrameGenLockReason>(r.reason < static_cast<uint32_t>(FrameGenLockReason::Count) ? r.reason : 0xFFu)));
    }
    // The performance gate (auto only): whether it allows Lock now and, if not, why.
    if (in.lockKey && in.autoMode && n < max) {
        formatFrameGenGateMonitor(in.gateState, in.gateReason, lines[n], kFrameGenMonitorLineChars);
        ++n;
    }
    // Lock's last second, as its log line counted it.
    if (in.lockKey && rtKnown) {
        if (r.lastSecond)
            put("Last second: S shown %u/%u, B replayed %u, late real %u", static_cast<unsigned>(r.fromS), static_cast<unsigned>(r.sSlots), static_cast<unsigned>(r.lateS),
                static_cast<unsigned>(r.lateReal));
        else
            put("%s", "Last second: no window closed yet");
    }
    // The synthesizer's last second, and the slot pose's miss when the runtime gave one.
    {
        char miss[40] = "";
        if (rtKnown && r.slotDegMedian >= 0.0f) std::snprintf(miss, sizeof(miss), ", slot pose miss %.2f deg", static_cast<double>(r.slotDegMedian));
        const FrameGenMonitorSynth& s = in.synth;
        if (!s.valid || !s.built) {
            put("Synth: no S built in the last second%s", miss);
        } else {
            char g[16] = "-", f[16] = "-";
            if (s.gpuN) std::snprintf(g, sizeof(g), "%.2f", static_cast<double>(s.gpuMs));
            if (s.finishN) std::snprintf(f, sizeof(f), "%.2f", static_cast<double>(s.finishMs));
            put("Synth %s ms/eye, finish %s ms/eye%s", g, f, miss);
        }
    }
    // VRAM.
    if (rtKnown)
        put("VRAM: game %.1f MiB, XR %.1f MiB", frameGenMonitorMiB(in.synth.gameBytes), frameGenMonitorMiB(r.xrBytes));
    else
        put("VRAM: game %.1f MiB, XR unknown", frameGenMonitorMiB(in.synth.gameBytes));
    return n;
}

}  // namespace edvr

#pragma once
// Frame generation, Phase 2.3 (fix.frame_generation = on, TEMPORARY, hidden; docs/design-frame-generation-2026-10-26.md, "### Pacing: Lock and Fill"): Lock 2:1, the runtime's
// half. Elite's frame B is shown at slot T, the synthetic frame S_B at T + P, Elite's next frame C at T + 2P. The runtime runs the S slot (its own xrWaitFrame, xrBeginFrame and
// xrEndFrame) at the head of C's WaitGetPoses, before C's own wait, so Elite is told C's real predicted time.
// Pure: no OpenXR, no D3D, no clock. The gate (when Lock is engaged), the slot's time, S's delivery, and the one-second and edge lines, whose format strings tools\edvr_log.py
// --health reads from this file. The runtime's frame loop (src/openxr/native_runtime_host.h, framegen_lock.h) feeds it.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "framegen_slot_report.h"

namespace edvr {

// Why Lock is not engaged (or why it let go), in the order the gate tests them: the first that fails is the one said.
enum class FrameGenLockReason : uint8_t {
    None = 0,
    KeyOff,          // fix.frame_generation is off (or the d3d11.dll is too old to carry it)
    NoChannel,       // the d3d11 half has no slot channel (an older d3d11.dll): no S can cross
    NotSeparate,     // the XR device is the game's: the transfer needs two
    NoProjection,    // the last frame was not a fresh pair the runtime composed (a menu, a load, a flat clear, a withheld or failed frame, a frame still open)
    NoReplay,        // B is not kept for a replay (resubmit off, or the exchange failed): a missing S would have nothing to show
    Menu,            // EDVR's own menu was drawn into the last pair's B and the d3d11.dll has not said it draws it into S (one from before the menu on S): S would blink it.
                     // A d3d11.dll that draws it and cannot for one S leaves that S unfinished instead (s_unfinished, why=menu).
    Unfinished,      // Phase 2.3b: the last pair's S was not finished as B is (framegen_slot_report.h kFrameGenFinish*: the why is said on its own line)
    NoS,             // the synthesizer has not delivered an S for several pairs
    DeferredPacing,  // deferred pacing (turbo, fix.weapon_stability's on-foot deferral) is on this frame: Lock stands aside for it in 2.3
    PeriodHalved,    // while engaged, the runtime's predicted period stayed about twice the display's for over kFrameGenLockHalvedUs (the vendor's smoothing fighting Lock); said
                     // again through the backoff after it (kFrameGenLockBackoffUs of full period). A halved period alone never stops Lock engaging.
    FinishPending,   // the previous pair's overlapped finish has not run (the owner FIFO says it cannot be; a guard)
    SlotFailed,      // the S slot itself was refused or failed (no frame was begun, or the runtime returned an error)
    Dwell,           // every condition holds, for less than kFrameGenLockDwellUs so far
    Gate,            // Phase 2.4, fix.frame_generation = auto: every condition of Lock's own holds, but the d3d11 half's performance gate does not admit Lock now (framegen_gate.h:
                     // its line says why). Last in the table so an older d3d11.dll reads any other reason as before.
    Count
};

inline const char* frameGenLockReasonName(FrameGenLockReason r) {
    switch (r) {
        case FrameGenLockReason::None: return "none";
        case FrameGenLockReason::KeyOff: return "key_off";
        case FrameGenLockReason::NoChannel: return "no_channel";
        case FrameGenLockReason::NotSeparate: return "not_separate";
        case FrameGenLockReason::NoProjection: return "no_projection";
        case FrameGenLockReason::NoReplay: return "no_replay";
        case FrameGenLockReason::Menu: return "menu_unfinished";
        case FrameGenLockReason::Unfinished: return "s_unfinished";
        case FrameGenLockReason::NoS: return "no_s";
        case FrameGenLockReason::DeferredPacing: return "deferred_pacing";
        case FrameGenLockReason::PeriodHalved: return "period_halved";
        case FrameGenLockReason::FinishPending: return "finish_pending";
        case FrameGenLockReason::SlotFailed: return "slot_failed";
        case FrameGenLockReason::Dwell: return "dwell";
        case FrameGenLockReason::Gate: return "auto_gate";
        default: return "unknown";
    }
}

// What the runtime knows at a pair boundary (WaitGetPoses, before any wait). Each field is true when its condition HOLDS.
struct FrameGenLockConditions {
    bool keyOn = false;
    bool channel = false;
    bool separate = false;
    bool projection = false;
    bool replay = false;
    bool noMenu = true;       // true unless EDVR's menu was drawn into the last pair's B and the d3d11 half does not draw it into S (a host from before 2.3b never sets it)
    bool sFinished = true;    // Phase 2.3b: true unless the last pair's S was left unfinished (frameGenSFinished)
    bool sDelivering = false;
    bool notDeferred = false;
    bool periodDisplay = false;   // not a blocker: the gate reads it itself (a halved period while engaged, and the backoff after it)
    bool finishDone = false;
    bool admitted = true;     // Phase 2.4: word 17 admits Lock (on, or auto with the gate engaged: framegen_gate.h frameGenWordAdmits); tested last
};

inline FrameGenLockReason frameGenLockBlocker(const FrameGenLockConditions& c) {
    if (!c.keyOn) return FrameGenLockReason::KeyOff;
    if (!c.channel) return FrameGenLockReason::NoChannel;
    if (!c.separate) return FrameGenLockReason::NotSeparate;
    if (!c.projection) return FrameGenLockReason::NoProjection;
    if (!c.replay) return FrameGenLockReason::NoReplay;
    if (!c.noMenu) return FrameGenLockReason::Menu;
    if (!c.sFinished) return FrameGenLockReason::Unfinished;
    // 2.4: auto with the gate closed builds S once per eye a second only (temporal_pass.cpp frameGenSynthEveryFrame), so S's delivery is a blocker only once the gate admits
    // Lock; until then the reason is auto_gate, or a later blocker of Lock's own.
    if (!c.sDelivering && c.admitted) return FrameGenLockReason::NoS;
    if (!c.notDeferred) return FrameGenLockReason::DeferredPacing;
    if (!c.finishDone) return FrameGenLockReason::FinishPending;
    if (!c.admitted) return FrameGenLockReason::Gate;
    return FrameGenLockReason::None;
}

// The runtime's predicted period is the display's: not above 1.5 display periods. displayNs is the display's period (from its refresh rate, else the shortest predicted period
// the session has seen); 0 (nothing known yet) is not the display's.
inline bool frameGenLockPeriodIsDisplay(int64_t periodNs, int64_t displayNs) {
    return periodNs > 0 && displayNs > 0 && 2 * periodNs <= 3 * displayNs;
}

constexpr uint64_t kFrameGenLockDwellUs = 1000000;     // the conditions hold this long before Lock engages (and again after any disengage)
constexpr uint64_t kFrameGenLockHalvedUs = 1000000;    // engaged, the runtime's period halved for longer than this: Lock lets go (period_halved)
constexpr uint64_t kFrameGenLockBackoffUs = 3000000;   // after a period_halved disengage: this long of full period (and every other condition) before Lock engages again

// Engage and disengage, only ever at a pair boundary. Engaged while every condition holds; any failure disengages at once; it engages again once they have held for
// kFrameGenLockDwellUs. step() returns whether this boundary is an edge, and the reason: the blocker while one stands, Dwell while waiting, None while engaged.
// The runtime's period is not a blocker. Disengaged, a halved period (the vendor throttling a game that cannot hold the display rate: flight 11 saw SteamVR flip 11.1 / 22.2 ms
// every second) does not stop the dwell: Lock engaging is what brings the period back. Engaged, the period halved at every boundary for more than kFrameGenLockHalvedUs (wall
// time, boundary to boundary; one full-period boundary starts it over) is the vendor's smoothing fighting Lock: it lets go (period_halved) and then needs kFrameGenLockBackoffUs of
// full period, a halved boundary starting it over, before it engages again; the reason through that backoff stays period_halved.
class FrameGenLockGate {
public:
    struct Step {
        bool engaged = false;
        bool edge = false;   // engaged changed at this boundary
        FrameGenLockReason reason = FrameGenLockReason::None;
    };
    Step step(const FrameGenLockConditions& c, uint64_t nowUs) {
        Step s;
        const FrameGenLockReason blocker = frameGenLockBlocker(c);
        if (blocker != FrameGenLockReason::None) {
            holding_ = false;
            halved_ = false;
            s.edge = engaged_;
            engaged_ = false;
            reason_ = blocker;
        } else if (!engaged_) {
            if (backoff_ && !c.periodDisplay) holding_ = false;   // the backoff counts full period only
            if (!holding_ && (!backoff_ || c.periodDisplay)) {
                holding_ = true;
                holdSinceUs_ = nowUs;
            }
            const uint64_t need = backoff_ ? kFrameGenLockBackoffUs : kFrameGenLockDwellUs;
            if (holding_ && nowUs >= holdSinceUs_ && nowUs - holdSinceUs_ >= need) {
                engaged_ = true;
                backoff_ = false;
                halved_ = false;
                s.edge = true;
                reason_ = FrameGenLockReason::None;
            } else {
                reason_ = backoff_ ? FrameGenLockReason::PeriodHalved : FrameGenLockReason::Dwell;
            }
        } else if (!c.periodDisplay) {
            if (!halved_) {
                halved_ = true;
                halvedSinceUs_ = nowUs;
            }
            if (nowUs >= halvedSinceUs_ && nowUs - halvedSinceUs_ > kFrameGenLockHalvedUs) {
                engaged_ = false;
                holding_ = false;
                halved_ = false;
                backoff_ = true;
                s.edge = true;
                reason_ = FrameGenLockReason::PeriodHalved;
            } else {
                reason_ = FrameGenLockReason::None;
            }
        } else {
            halved_ = false;
            reason_ = FrameGenLockReason::None;
        }
        s.engaged = engaged_;
        s.reason = reason_;
        return s;
    }
    // The S slot was refused or failed: disengaged at once, and the dwell starts over. True when this was an edge.
    bool fail(FrameGenLockReason why) {
        const bool edge = engaged_;
        engaged_ = false;
        holding_ = false;
        halved_ = false;
        reason_ = why;
        return edge;
    }
    bool engaged() const { return engaged_; }
    bool backingOff() const { return backoff_; }
    FrameGenLockReason reason() const { return reason_; }
    void reset() { *this = FrameGenLockGate{}; }

private:
    bool engaged_ = false, holding_ = false, halved_ = false, backoff_ = false;
    uint64_t holdSinceUs_ = 0, halvedSinceUs_ = 0;
    FrameGenLockReason reason_ = FrameGenLockReason::KeyOff;
};

// Whether the synthesizer is delivering S: an S was consumed for one of the last kFrameGenLockSMissLimit pairs. One missing S is a late S (B is replayed); a run of them
// disengages. Under the shadow key with Lock not engaged S is built on every other frame only, which still delivers.
constexpr uint32_t kFrameGenLockSMissLimit = 4;
struct FrameGenSDelivery {
    uint32_t missRun = kFrameGenLockSMissLimit;   // nothing delivered yet
    void notePair(bool sConsumed) { missRun = sConsumed ? 0 : (missRun < kFrameGenLockSMissLimit ? missRun + 1 : missRun); }
    bool delivering() const { return missRun < kFrameGenLockSMissLimit; }
};

// The slot S is made for. While Lock is engaged it is the next display slot, T + P (the slot the runtime will run S in); otherwise Phase 2.2's T + (T - T_A) / 2, which is the
// same T + P once Lock has held for a pair. Valid from 1 to 50 ms past T, as 2.2's.
inline bool frameGenLockSlotTime(int64_t displayTime, int64_t previousDisplayTime, bool havePrevious, bool engaged, int64_t periodNs, int64_t& slotTime) {
    if (engaged && periodNs > 0 && displayTime > 0) {
        slotTime = 0;
        if (periodNs < kFrameGenSlotMinLeadNs || periodNs > kFrameGenSlotMaxLeadNs) return false;
        slotTime = displayTime + periodNs;
        return true;
    }
    return frameGenSlotTime(displayTime, previousDisplayTime, havePrevious, slotTime);
}

// S can be shown in the slot after pair `pair`: both eyes' S consumed for that pair, its pose located for that pair, and B's own head known (S's views are B's turned by the
// head's step B -> S). Anything else replays B with B's views and counts late_s.
inline bool frameGenSReady(uint64_t pair, const uint64_t consumed[2], uint64_t poseFor, bool poseValid, bool headBValid, bool formatShown) {
    return pair != 0 && consumed[0] == pair && consumed[1] == pair && poseFor == pair && poseValid && headBValid && formatShown;
}

// Phase 2.3b: one eye's answer for a pair says its S was NOT finished as B is: handed over without the finish (a d3d11.dll from before 2.3b, finish 0), or held back with a reason
// (finish 2 and up). An S not built, or held back busy, says nothing about finishing. `why` is the finish word to name (kFrameGenFinish*).
inline bool frameGenSUnfinished(uint32_t state, uint32_t finish, uint32_t& why) {
    const bool produced = state == 1u;   // EDVR_NATIVE_FRAMEGEN_S_PRODUCED
    if ((produced && finish != kFrameGenFinishDone) || finish > kFrameGenFinishDone) {
        why = finish;
        return true;
    }
    return false;
}

// Why an S slot replayed B instead of showing S (the flight 11 instrument): the first of these that holds, eye 0 before eye 1.
enum class FrameGenReplayWhy : uint8_t {
    NotBuilt = 0,   // the d3d11 half answered nothing for this pair, or answered that it built no S (no inputs: e.g. the temporal pass wrote no depth)
    Kept,           // S was built and not handed over (the f = 1 variant of the shadow key's alternation, or no consumer)
    Busy,           // the previous S was still the runtime's: this one was not handed over
    Unfinished,     // S was not finished as B is (2.3b) and is not shown
    NotConsumed,    // S was handed over for this pair and is still pending (its key not yet released at the slot)
    SeqMismatch,    // S was handed over for this pair, but the private copy holds another pair's S
    NoPose,         // S's head (T_S) was not located for this pair, or B's head is not known
    Format,         // S's private texture is missing or in a format the stereo renderer cannot show
    Adopt,          // everything was ready but adopting S's textures or rendering them failed
    Count
};
inline const char* frameGenReplayWhyName(FrameGenReplayWhy w) {
    switch (w) {
        case FrameGenReplayWhy::NotBuilt: return "not_built";
        case FrameGenReplayWhy::Kept: return "kept";
        case FrameGenReplayWhy::Busy: return "busy";
        case FrameGenReplayWhy::Unfinished: return "unfinished";
        case FrameGenReplayWhy::NotConsumed: return "not_consumed";
        case FrameGenReplayWhy::SeqMismatch: return "seq_mismatch";
        case FrameGenReplayWhy::NoPose: return "no_pose";
        case FrameGenReplayWhy::Format: return "format";
        case FrameGenReplayWhy::Adopt: return "adopt";
        default: return "unknown";
    }
}
// What the runtime knows about one eye's S at the S slot: the pair its last answer was for and that answer (state: EDVR_NATIVE_FRAMEGEN_S_*; finish: kFrameGenFinish*), the pair
// whose S is pending (handed over, not yet taken; 0: none), and the pair whose S the private copy holds (0: none).
struct FrameGenSEyeFacts {
    uint64_t answerFor = 0;
    uint32_t state = 0, finish = 0;
    uint64_t pending = 0, consumed = 0;
};
// The reason the slot after pair `pair` cannot show S, or Count when it can (frameGenSReady and the finish test both hold: then only Adopt remains). `had` is the pair the
// private copy holds on a SeqMismatch.
inline FrameGenReplayWhy frameGenReplayWhy(uint64_t pair, const FrameGenSEyeFacts eyes[2], uint64_t poseFor, bool poseValid, bool headBValid, bool formatOk, uint64_t& had) {
    had = 0;
    for (int e = 0; e < 2; ++e) {
        const FrameGenSEyeFacts& f = eyes[e];
        uint32_t why = 0;
        if (pair != 0 && f.consumed == pair && f.answerFor == pair && f.finish == kFrameGenFinishDone) continue;
        if (pair == 0 || f.answerFor != pair || f.state == 0u) return FrameGenReplayWhy::NotBuilt;
        if (f.state == 2u) return FrameGenReplayWhy::Busy;
        if (frameGenSUnfinished(f.state, f.finish, why)) return FrameGenReplayWhy::Unfinished;
        if (f.state != 1u) return FrameGenReplayWhy::Kept;
        if (f.pending == pair) return FrameGenReplayWhy::NotConsumed;
        had = f.consumed;
        return FrameGenReplayWhy::SeqMismatch;
    }
    if (poseFor != pair || !poseValid || !headBValid) return FrameGenReplayWhy::NoPose;
    if (!formatOk) return FrameGenReplayWhy::Format;
    return FrameGenReplayWhy::Count;
}

// ---- the lines ----
// Said once when the runtime first reads the key on (and again after it was off).
constexpr const char* kFrameGenLockArmedFmt =
    "frame generation lock: armed (fix.frame_generation = on: Lock 2:1, forced; it engages once its conditions have held for 1 s and says each engage and disengage)";
// Phase 2.4: the same, for fix.frame_generation = auto (said again when the key moves between on and auto).
constexpr const char* kFrameGenLockArmedAutoFmt =
    "frame generation lock: armed (fix.frame_generation = auto: Lock 2:1 while the d3d11 half's performance gate admits it; it engages once its conditions have held for 1 s and says each engage and disengage)";
// Each edge, at the pair boundary it happens at. period_ms: the runtime's last predicted period.
constexpr const char* kFrameGenLockEngageFmt = "frame generation lock: engage reason=held_1s period_ms=%.2f";
constexpr const char* kFrameGenLockDisengageFmt = "frame generation lock: disengage reason=%s period_ms=%.2f%s";
// The note a deferred-pacing disengage (or block) carries: in 2.3 Lock stands aside for it.
constexpr const char* kFrameGenLockDeferredNote = " (Lock stands aside for deferred pacing in 2.3)";
// The S slot could not show S because of its format (the stereo renderer takes 8-bit RGBA/BGRA only): said once; the slot replays B.
constexpr const char* kFrameGenLockFormatFmt = "frame generation lock: S's format %u cannot be shown (8-bit RGBA/BGRA only); the S slot replays B";
// Phase 2.3b: an S was left unfinished (Lock lets go with reason s_unfinished, or does not engage): said when the why changes from the last one said.
constexpr const char* kFrameGenLockUnfinishedFmt = "frame generation lock: S is not finished like B (why=%s); the S slot does not show it";
// Once a second while the key is on. engaged: the seconds of the window Lock was engaged; slots: the real pairs composed while engaged / the S slots run; late_real: the display
// periods the boundary counted missed across the engaged pairs (Elite's frame late); late_s: S slots that showed B again (S missing, not consumed, busy, no pose, or a format
// it cannot show); s_from: S slots that showed S / that replayed B; wait_ms: the S slot's xrWaitFrame, median/max; period_ms: the runtime's predicted period, median;
// disengages: the edges to off in the window; reason: why it is off at the line (none while engaged).
constexpr const char* kFrameGenLockReportFmt =
    "frame generation lock: engaged=%.2f slots=%u/%u late_real=%u late_s=%u s_from=%u/%u wait_ms=%s/%s period_ms=%s disengages=%u reason=%s";

// The flight 11 instrument: in a window where an S slot replayed B, why each one did (FrameGenReplayWhy, counted), and on a seq_mismatch the last pair asked for and the pair the
// private copy held. Said right after the one-second line, only when fromB is not zero.
constexpr const char* kFrameGenLockReplayFmt =
    "frame generation lock: replayed B: not_built=%u kept=%u busy=%u unfinished=%u not_consumed=%u seq_mismatch=%u (asked %llu had %llu) no_pose=%u format=%u adopt=%u";

struct FrameGenLockWindow {
    double engagedSeconds = 0.0;
    uint32_t realSlots = 0, sSlots = 0, lateReal = 0, lateS = 0, fromS = 0, fromB = 0, disengages = 0;
    std::vector<float> waitMs, periodMs;
    uint32_t replayWhy[static_cast<int>(FrameGenReplayWhy::Count) + 1] = {};   // the last entry: Count (no reason found: counted as adopt)
    uint64_t mismatchAsked = 0, mismatchHad = 0;
    void noteReplay(FrameGenReplayWhy why, uint64_t asked, uint64_t had) {
        ++replayWhy[static_cast<int>(why)];
        if (why == FrameGenReplayWhy::SeqMismatch) { mismatchAsked = asked; mismatchHad = had; }
    }
    void reset() { *this = FrameGenLockWindow{}; }
};

inline std::string formatFrameGenLockReplay(const FrameGenLockWindow& w) {
    char buf[320];
    const auto n = [&](FrameGenReplayWhy r) { return static_cast<unsigned>(w.replayWhy[static_cast<int>(r)]); };
    std::snprintf(buf, sizeof(buf), kFrameGenLockReplayFmt, n(FrameGenReplayWhy::NotBuilt), n(FrameGenReplayWhy::Kept), n(FrameGenReplayWhy::Busy), n(FrameGenReplayWhy::Unfinished),
                  n(FrameGenReplayWhy::NotConsumed), n(FrameGenReplayWhy::SeqMismatch), static_cast<unsigned long long>(w.mismatchAsked), static_cast<unsigned long long>(w.mismatchHad),
                  n(FrameGenReplayWhy::NoPose), n(FrameGenReplayWhy::Format), n(FrameGenReplayWhy::Adopt) + static_cast<unsigned>(w.replayWhy[static_cast<int>(FrameGenReplayWhy::Count)]));
    return buf;
}

inline std::string formatFrameGenLockReport(const FrameGenLockWindow& w, FrameGenLockReason reason) {
    char buf[512];
    const std::string w0 = frameGenSlotMs(w.waitMs, false), w1 = frameGenSlotMs(w.waitMs, true), p0 = frameGenSlotMs(w.periodMs, false);
    std::snprintf(buf, sizeof(buf), kFrameGenLockReportFmt, w.engagedSeconds, static_cast<unsigned>(w.realSlots), static_cast<unsigned>(w.sSlots), static_cast<unsigned>(w.lateReal),
                  static_cast<unsigned>(w.lateS), static_cast<unsigned>(w.fromS), static_cast<unsigned>(w.fromB), w0.c_str(), w1.c_str(), p0.c_str(), static_cast<unsigned>(w.disengages),
                  frameGenLockReasonName(reason));
    return buf;
}

inline std::string formatFrameGenLockEdge(bool engage, FrameGenLockReason reason, double periodMs) {
    char buf[256];
    if (engage)
        std::snprintf(buf, sizeof(buf), kFrameGenLockEngageFmt, periodMs);
    else
        std::snprintf(buf, sizeof(buf), kFrameGenLockDisengageFmt, frameGenLockReasonName(reason), periodMs,
                      reason == FrameGenLockReason::DeferredPacing ? kFrameGenLockDeferredNote : "");
    return buf;
}

}  // namespace edvr

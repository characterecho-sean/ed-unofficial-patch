#pragma once
// Frame generation, Phase 1 (fix.frame_generation_shadow, TEMPORARY; docs/design-frame-generation-2026-10-26.md, H6 and failure class 11): how far the runtime's
// head pose for the synthetic slot's display time T_S = T + P (the frame's display time plus one display period) is from the head the runtime reports once T_S
// has come, beside the same miss for a velocity fallback (the position carried at the velocity between the last two real frames' heads, the rotation at the
// angular velocity at T). Pure: no OpenXR, no clock. The runtime's frame loop (src/openxr/native_runtime_host.h) feeds it poses and times and writes its one line.
// tools\edvr_log.py --health reads the line from the format string below, so it stays one constant.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace edvr {

struct FgPose {
    float pos[3] = {0, 0, 0};
    float q[4] = {0, 0, 0, 1};   // x y z w
};

// How a locate went: Ok, Refused (the runtime said the time is invalid: it will not locate there), Invalid (any other failure, or a pose without both validity bits).
enum class FgLocate { Ok, Refused, Invalid };

// The line, once a second. runtime_* is the runtime's own answer for T_S made at t0; velocity_* is the fallback; both are measured against the runtime's
// answer for T_S made after T_S had come (median/p95/max over the second's scored frames; position in millimetres, rotation in degrees). vel_src counts the
// scored frames by where the fallback's angular velocity came from: the runtime's XrSpaceVelocity / two locates 5 ms apart / none (no fallback that frame).
// expired counts frames dropped unscored (the ring was full, or the clock was more than a second past T_S); refused the frames whose truth the runtime would not locate;
// unpredicted the frames whose own T_S the runtime would not locate at t0 (it declines to predict that far).
// Phase 2.3, H6 at the slot: slot= the slot poses scored (the head the runtime located at the synthetic slot's T_S in capture(), Phase 2.2, before the synthesizer ran), and their
// miss against the head located once the clock passed T_S: slot_mm, slot_deg (median/p95/max) and slot_lead_ms (T_S less the clock at that locate, median). A line from before
// 2.3 ends at unpredicted.
constexpr const char* kFrameGenPoseReportFmt =
    "frame generation (shadow) pose: frames=%u lead_ms=%.1f runtime_mm=%.2f/%.2f/%.2f runtime_deg=%.3f/%.3f/%.3f velocity_mm=%.2f/%.2f/%.2f "
    "velocity_deg=%.3f/%.3f/%.3f vel_src=%u/%u/%u expired=%u refused=%u unpredicted=%u slot=%u slot_mm=%.2f/%.2f/%.2f slot_deg=%.3f/%.3f/%.3f slot_lead_ms=%.1f";

struct FrameGenPoseWindow {
    uint32_t frames = 0;
    float leadMsMedian = 0.0f;
    float rtMm[3] = {}, rtDeg[3] = {}, vMm[3] = {}, vDeg[3] = {};   // median, p95, max
    uint32_t srcXr = 0, srcPair = 0, srcNone = 0;
    uint32_t expired = 0, refused = 0, unpredicted = 0;
    uint32_t slotFrames = 0;
    float slotMm[3] = {}, slotDeg[3] = {}, slotLeadMsMedian = 0.0f;
};

inline std::string formatFrameGenPoseReport(const FrameGenPoseWindow& w) {
    char buf[640];
    std::snprintf(buf, sizeof(buf), kFrameGenPoseReportFmt, static_cast<unsigned>(w.frames), static_cast<double>(w.leadMsMedian), static_cast<double>(w.rtMm[0]),
                  static_cast<double>(w.rtMm[1]), static_cast<double>(w.rtMm[2]), static_cast<double>(w.rtDeg[0]), static_cast<double>(w.rtDeg[1]),
                  static_cast<double>(w.rtDeg[2]), static_cast<double>(w.vMm[0]), static_cast<double>(w.vMm[1]), static_cast<double>(w.vMm[2]),
                  static_cast<double>(w.vDeg[0]), static_cast<double>(w.vDeg[1]), static_cast<double>(w.vDeg[2]), static_cast<unsigned>(w.srcXr),
                  static_cast<unsigned>(w.srcPair), static_cast<unsigned>(w.srcNone), static_cast<unsigned>(w.expired), static_cast<unsigned>(w.refused), static_cast<unsigned>(w.unpredicted),
                  static_cast<unsigned>(w.slotFrames), static_cast<double>(w.slotMm[0]), static_cast<double>(w.slotMm[1]), static_cast<double>(w.slotMm[2]),
                  static_cast<double>(w.slotDeg[0]), static_cast<double>(w.slotDeg[1]), static_cast<double>(w.slotDeg[2]), static_cast<double>(w.slotLeadMsMedian));
    return buf;
}

// ---- the miss ----
inline float fgPositionMissMm(const float a[3], const float b[3]) {
    const double dx = double(a[0]) - b[0], dy = double(a[1]) - b[1], dz = double(a[2]) - b[2];
    return static_cast<float>(1000.0 * std::sqrt(dx * dx + dy * dy + dz * dz));
}

inline void fgQuatMul(const float a[4], const float b[4], float out[4]) {   // a * b, x y z w
    const double x = double(a[3]) * b[0] + double(a[0]) * b[3] + double(a[1]) * b[2] - double(a[2]) * b[1];
    const double y = double(a[3]) * b[1] - double(a[0]) * b[2] + double(a[1]) * b[3] + double(a[2]) * b[0];
    const double z = double(a[3]) * b[2] + double(a[0]) * b[1] - double(a[1]) * b[0] + double(a[2]) * b[3];
    const double w = double(a[3]) * b[3] - double(a[0]) * b[0] - double(a[1]) * b[1] - double(a[2]) * b[2];
    out[0] = static_cast<float>(x);
    out[1] = static_cast<float>(y);
    out[2] = static_cast<float>(z);
    out[3] = static_cast<float>(w);
}

// The angle (degrees) of the rotation taking b to a, well-conditioned for small angles.
inline float fgRotationMissDeg(const float a[4], const float b[4]) {
    const float bc[4] = {-b[0], -b[1], -b[2], b[3]};
    float d[4];
    fgQuatMul(a, bc, d);
    const double v = std::sqrt(double(d[0]) * d[0] + double(d[1]) * d[1] + double(d[2]) * d[2]);
    return static_cast<float>(2.0 * std::atan2(v, std::fabs(double(d[3]))) * 180.0 / 3.14159265358979323846);
}

// exp(omega * dt) * q: q turned by the angular velocity omega (radians a second, about axes of the space q is in) for dt seconds.
inline void fgRotateByVelocity(const float q[4], const float omega[3], double dt, float out[4]) {
    const double rx = double(omega[0]) * dt, ry = double(omega[1]) * dt, rz = double(omega[2]) * dt;
    const double angle = std::sqrt(rx * rx + ry * ry + rz * rz);
    float d[4] = {0, 0, 0, 1};
    if (angle > 1e-12) {
        const double s = std::sin(angle * 0.5) / angle;
        d[0] = static_cast<float>(rx * s);
        d[1] = static_cast<float>(ry * s);
        d[2] = static_cast<float>(rz * s);
        d[3] = static_cast<float>(std::cos(angle * 0.5));
    }
    fgQuatMul(d, q, out);
}

// The angular velocity (radians a second) that takes q0 to q1 in dt seconds: log(q1 * q0^-1) / dt.
inline void fgVelocityBetween(const float q0[4], const float q1[4], double dt, float omega[3]) {
    const float c[4] = {-q0[0], -q0[1], -q0[2], q0[3]};
    float d[4];
    fgQuatMul(q1, c, d);
    if (d[3] < 0) {
        d[0] = -d[0];
        d[1] = -d[1];
        d[2] = -d[2];
        d[3] = -d[3];
    }
    const double v = std::sqrt(double(d[0]) * d[0] + double(d[1]) * d[1] + double(d[2]) * d[2]);
    const double angle = 2.0 * std::atan2(v, double(d[3]));
    const double k = v > 1e-12 && dt > 0 ? angle / v / dt : (dt > 0 ? 2.0 / dt : 0.0);
    omega[0] = static_cast<float>(d[0] * k);
    omega[1] = static_cast<float>(d[1] * k);
    omega[2] = static_cast<float>(d[2] * k);
}

// median (the element at n/2 of the sorted values), p95 (the element at ceil(0.95 n) - 1) and max; all 0 for no values. Sorts v.
inline void fgSummarise(std::vector<float>& v, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    size_t p95 = static_cast<size_t>(std::ceil(0.95 * static_cast<double>(n)));
    if (p95 < 1) p95 = 1;
    out[0] = v[n / 2];
    out[1] = v[p95 - 1];
    out[2] = v[n - 1];
}

// The frame loop's half. Times are the runtime's own ticks (nanoseconds); the caller supplies each locate as locate(time, pose) -> FgLocate. No locate is made
// that the caller's lambda does not make, and nothing here waits.
class FrameGenPoseProbe {
public:
    static constexpr size_t kPending = 8;
    static constexpr int64_t kPairNs = 5000000;           // the two-locate angular velocity's spacing, 5 ms
    static constexpr int64_t kGapPeriods = 3;             // the last real frame's head is used only within this many periods of this one
    static constexpr int64_t kExpireNs = 1000000000;      // a pending frame whose T_S is this far behind the clock is dropped unscored

    struct Frame {
        int64_t displayTime = 0, period = 0;   // T and P
        int64_t now = 0;                       // the clock at the moment of the prediction, t0
        bool angularValid = false;             // the runtime's XrSpaceVelocity at T carries an angular velocity
        float omega[3] = {0, 0, 0};            // radians a second, in the space the poses are in
    };

    // One real frame. atT is the head at T (the caller's locate); the rest is located here through `locate`.
    template <class Locate>
    void onFrame(const Frame& f, const FgPose& atT, Locate&& locate) {
        if (f.displayTime <= 0 || f.period <= 0 || f.now <= 0) return;
        Pending p;
        p.target = f.displayTime + f.period;
        p.leadMs = static_cast<float>(static_cast<double>(p.target - f.now) / 1.0e6);
        p.runtime = locate(p.target, p.runtimePose) == FgLocate::Ok;
        if (!p.runtime) ++unpredicted_;
        // The velocity fallback: position from the last real frame's head and this one's; rotation from the runtime's angular velocity at T, else from two locates.
        p.source = Source::None;
        float omega[3] = {0, 0, 0};
        if (f.angularValid) {
            for (int i = 0; i < 3; ++i) omega[i] = f.omega[i];
            p.source = Source::Xr;
        } else {
            FgPose before;
            const int64_t earlier = f.displayTime - kPairNs;
            if (locate(earlier, before) == FgLocate::Ok) {
                fgVelocityBetween(before.q, atT.q, static_cast<double>(kPairNs) * 1e-9, omega);
                p.source = Source::Pair;
            }
        }
        const bool havePrev = havePrev_ && f.displayTime > prevTime_ && f.displayTime - prevTime_ <= kGapPeriods * f.period;
        if (p.source != Source::None && havePrev) {
            const double span = static_cast<double>(f.displayTime - prevTime_);
            const double ahead = static_cast<double>(p.target - f.displayTime);
            for (int i = 0; i < 3; ++i)
                p.velocityPose.pos[i] = atT.pos[i] + static_cast<float>((double(atT.pos[i]) - prev_.pos[i]) * ahead / span);
            fgRotateByVelocity(atT.q, omega, ahead * 1e-9, p.velocityPose.q);
            p.velocity = true;
        } else {
            p.source = Source::None;
        }
        prev_ = atT;
        prevTime_ = f.displayTime;
        havePrev_ = true;
        if (count_ == kPending) {   // the oldest goes unscored
            ++expired_;
            for (size_t i = 1; i < kPending; ++i) pending_[i - 1] = pending_[i];
            --count_;
        }
        pending_[count_++] = p;
    }

    // Phase 2.3, H6 at the slot: the head the runtime located at the synthetic slot's T_S (target) when the clock read now, in the space the probe's locates use. Scored by
    // resolve() once the clock has passed T_S. A full ring drops its oldest unscored (counted expired).
    void onSlot(int64_t target, int64_t now, const FgPose& located) {
        if (target <= 0 || now <= 0) return;
        SlotPending s;
        s.target = target;
        s.leadMs = static_cast<float>(static_cast<double>(target - now) / 1.0e6);
        s.pose = located;
        if (slotCount_ == kPending) {
            ++expired_;
            for (size_t i = 1; i < kPending; ++i) slotPending_[i - 1] = slotPending_[i];
            --slotCount_;
        }
        slotPending_[slotCount_++] = s;
    }

    // Scores every pending frame whose T_S the clock has passed, truth located through `locate`; drops the ones that expired. Call once per real frame.
    template <class Locate>
    void resolve(int64_t now, Locate&& locate) {
        size_t slotKeep = 0;
        for (size_t i = 0; i < slotCount_; ++i) {
            const SlotPending& s = slotPending_[i];
            if (now < s.target) {
                slotPending_[slotKeep++] = s;
                continue;
            }
            if (now - s.target > kExpireNs) {
                ++expired_;
                continue;
            }
            FgPose truth;
            if (locate(s.target, truth) != FgLocate::Ok) {
                ++refused_;
                continue;
            }
            ++slotFrames_;
            slotLead_.push_back(s.leadMs);
            slotMm_.push_back(fgPositionMissMm(s.pose.pos, truth.pos));
            slotDeg_.push_back(fgRotationMissDeg(s.pose.q, truth.q));
        }
        slotCount_ = slotKeep;
        size_t keep = 0;
        for (size_t i = 0; i < count_; ++i) {
            const Pending& p = pending_[i];
            if (now < p.target) {
                pending_[keep++] = p;
                continue;
            }
            if (now - p.target > kExpireNs) {
                ++expired_;
                continue;
            }
            FgPose truth;
            const FgLocate r = locate(p.target, truth);
            if (r != FgLocate::Ok) {
                ++refused_;
                continue;
            }
            ++frames_;
            lead_.push_back(p.leadMs);
            if (p.runtime) {
                rtMm_.push_back(fgPositionMissMm(p.runtimePose.pos, truth.pos));
                rtDeg_.push_back(fgRotationMissDeg(p.runtimePose.q, truth.q));
            }
            if (p.velocity) {
                vMm_.push_back(fgPositionMissMm(p.velocityPose.pos, truth.pos));
                vDeg_.push_back(fgRotationMissDeg(p.velocityPose.q, truth.q));
            }
            if (p.source == Source::Xr) ++srcXr_;
            else if (p.source == Source::Pair) ++srcPair_;
            else ++srcNone_;
        }
        count_ = keep;
    }

    // True when the window holds anything to say; fills w and starts a new window.
    bool takeWindow(FrameGenPoseWindow& w) {
        w = FrameGenPoseWindow{};
        w.frames = frames_;
        w.expired = expired_;
        w.refused = refused_;
        w.unpredicted = unpredicted_;
        w.srcXr = srcXr_;
        w.srcPair = srcPair_;
        w.srcNone = srcNone_;
        float lead[3];
        fgSummarise(lead_, lead);
        w.leadMsMedian = lead[0];
        fgSummarise(rtMm_, w.rtMm);
        fgSummarise(rtDeg_, w.rtDeg);
        fgSummarise(vMm_, w.vMm);
        fgSummarise(vDeg_, w.vDeg);
        w.slotFrames = slotFrames_;
        fgSummarise(slotMm_, w.slotMm);
        fgSummarise(slotDeg_, w.slotDeg);
        float slotLead[3];
        fgSummarise(slotLead_, slotLead);
        w.slotLeadMsMedian = slotLead[0];
        const bool any = frames_ || expired_ || refused_ || unpredicted_ || slotFrames_;
        frames_ = expired_ = refused_ = unpredicted_ = srcXr_ = srcPair_ = srcNone_ = slotFrames_ = 0;
        slotLead_.clear();
        slotMm_.clear();
        slotDeg_.clear();
        lead_.clear();
        rtMm_.clear();
        rtDeg_.clear();
        vMm_.clear();
        vDeg_.clear();
        return any;
    }

    // The key went off, or the frame stream broke: nothing carried across.
    void reset() {
        FrameGenPoseWindow ignored;
        takeWindow(ignored);
        count_ = 0;
        slotCount_ = 0;
        havePrev_ = false;
    }

private:
    enum class Source { None, Xr, Pair };
    struct Pending {
        int64_t target = 0;
        float leadMs = 0.0f;
        bool runtime = false, velocity = false;
        Source source = Source::None;
        FgPose runtimePose, velocityPose;
    };
    Pending pending_[kPending];
    size_t count_ = 0;
    FgPose prev_;
    int64_t prevTime_ = 0;
    bool havePrev_ = false;
    uint32_t frames_ = 0, expired_ = 0, refused_ = 0, unpredicted_ = 0, srcXr_ = 0, srcPair_ = 0, srcNone_ = 0;
    std::vector<float> lead_, rtMm_, rtDeg_, vMm_, vDeg_;
    struct SlotPending {
        int64_t target = 0;
        float leadMs = 0.0f;
        FgPose pose;
    };
    SlotPending slotPending_[kPending];
    size_t slotCount_ = 0;
    uint32_t slotFrames_ = 0;
    std::vector<float> slotLead_, slotMm_, slotDeg_;
};

}  // namespace edvr

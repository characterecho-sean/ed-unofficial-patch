// framegen_shadow_report_test: the pure half of the frame-generation shadow scorer (src/common/framegen_shadow_report.h; fix.frame_generation_shadow,
// TEMPORARY, docs/design-frame-generation-2026-10-26.md Phase 1): the histogram binning, the percentile and share-over arithmetic, the merge, and the exact
// text of the armed, VRAM and report lines; and the pure half of the head-pose miss (src/common/framegen_pose_report.h, H6): the miss arithmetic, the summaries, the probe's ring
// and the exact text of its line.
//
//   --dry-run    the same run (the rig never writes a file), for the gate's --dry-run convention
//   --self-test  every check
#include "framegen_pose_report.h"
#include "framegen_shadow_report.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace edvr;

namespace {

int g_fail = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("framegen_shadow_report_test: FAIL: %s\n", what);
        ++g_fail;
    }
}

void checkText(const std::string& got, const char* want, const char* what) {
    if (got != want) {
        std::printf("framegen_shadow_report_test: FAIL: %s\n  got:  %s\n  want: %s\n", what, got.c_str(), want);
        ++g_fail;
    }
}

void caseBins() {
    check(frameGenShadowBin(0.0f) == 0, "bin of 0 is 0");
    check(frameGenShadowBin(0.00097f) == 0, "bin just under 1/1024 is 0");
    check(frameGenShadowBin(1.0f / 1024.0f) == 1, "bin of exactly 1/1024 is 1");
    check(frameGenShadowBin(1.1f / 1024.0f) == 1, "bin 1 spans 1/1024 up to a quarter octave (2^(1/4) = 1.189)");
    check(frameGenShadowBin(1.2f / 1024.0f) == 2, "bin 2 begins past 2^(1/4)/1024");
    check(frameGenShadowBin(0.05f) == 23, "bin of 0.05 is 23");
    check(frameGenShadowBin(0.2f) == 31, "bin of 0.2 is 31");
    check(frameGenShadowBin(1.0f) == 41, "bin of 1.0 is 41");
    check(frameGenShadowBin(100.0f) == 47, "a large error clamps to bin 47");
    check(frameGenShadowBin(std::nanf("")) == 0, "NaN is bin 0");
    check(frameGenShadowBin(-0.5f) == 0, "a negative error is bin 0");
    check(frameGenShadowBinValue(0) == 0.0f, "bin 0 reads 0");
    bool roundTrip = true, rising = true;
    float prev = 0.0f;
    for (int b = 1; b < kFrameGenShadowBins; ++b) {
        const float v = frameGenShadowBinValue(b);
        if (frameGenShadowBin(v) != b) roundTrip = false;
        if (!(v > prev)) rising = false;
        prev = v;
    }
    check(roundTrip, "every bin's representative value bins back to it");
    check(rising, "representative values rise with the bin");
    check(std::fabs(frameGenShadowBinValue(23) - 0.0482f) < 0.0005f, "bin 23 reads about 0.0482");
}

void caseStats() {
    uint32_t empty[kFrameGenShadowBins] = {};
    check(frameGenShadowPercentile(empty, 0.5f) == 0.0f, "percentile of an empty histogram is 0");
    check(frameGenShadowShareOver(empty, 0.02f) == 0.0f, "share over of an empty histogram is 0");

    uint32_t h[kFrameGenShadowBins] = {};
    h[0] = 50;
    h[18] = 25;   // about 0.0203
    h[31] = 25;   // about 0.193
    check(frameGenShadowPercentile(h, 0.5f) == 0.0f, "p50 lands on the last of the 50 zeros");
    check(frameGenShadowPercentile(h, 0.51f) == frameGenShadowBinValue(18), "p51 steps into bin 18");
    check(frameGenShadowPercentile(h, 0.75f) == frameGenShadowBinValue(18), "p75 is the last of bin 18");
    check(frameGenShadowPercentile(h, 0.76f) == frameGenShadowBinValue(31), "p76 steps into bin 31");
    check(frameGenShadowPercentile(h, 1.0f) == frameGenShadowBinValue(31), "p100 is the top occupied bin");
    check(frameGenShadowPercentile(h, 0.0f) == 0.0f, "p0 is the first occupied bin");
    check(std::fabs(frameGenShadowShareOver(h, 0.02f) - 0.5f) < 1e-6f, "bin 18 (0.0203) and bin 31 are over 0.02");
    check(std::fabs(frameGenShadowShareOver(h, 0.05f) - 0.25f) < 1e-6f, "only bin 31 is over 0.05");
    check(frameGenShadowShareOver(h, 0.5f) == 0.0f, "nothing is over 0.5");
}

void caseMerge() {
    FrameGenShadowEyeResult a, b;
    a.cls[1].pixels = 10;
    a.cls[1].sumErr1024 = 100;
    a.cls[1].hist[5] = 4;
    a.cls[1].hist[9] = 6;
    a.cls[1].sumErrId1024 = 7;
    a.cls[1].histId[3] = 10;
    a.holes = 3;
    a.cells = 40;
    b.cls[1].pixels = 5;
    b.cls[1].sumErr1024 = 50;
    b.cls[1].hist[5] = 5;
    b.cls[1].sumErrId1024 = 1;
    b.cls[1].histId[3] = 5;
    b.cls[2].pixels = 2;
    b.holes = 1;
    b.cells = 20;
    FrameGenShadowWindow w;
    frameGenShadowMerge(w, a);
    frameGenShadowMerge(w, b);
    check(w.cls[1].pixels == 15 && w.cls[1].sumErr1024 == 150 && w.cls[1].sumErrId1024 == 8, "merge adds counts and sums");
    check(w.cls[1].hist[5] == 9 && w.cls[1].hist[9] == 6 && w.cls[1].histId[3] == 15, "merge adds histograms");
    check(w.cls[2].pixels == 2 && w.cls[0].pixels == 0, "merge keeps the classes apart");
    check(w.holes == 4 && w.cells == 60, "merge adds holes and cells");
    check(w.framesScored == 0, "merge leaves the frame count to the caller");
}

void caseLines() {
    checkText(formatFrameGenShadowArmed(612, 699),
              "frame generation (shadow): armed, quarter 612x699 per eye, classes far cockpit world (mover, reactive not scored)", "the armed line");

    FrameGenShadowWindow w;
    w.framesScored = 60;
    for (int i = 0; i < kFrameGenShadowSkipCount; ++i) w.skipped[i] = static_cast<uint32_t>(i + 1);
    w.cells = 1000;
    w.holes = 100;
    w.cls[0].pixels = 100;
    w.cls[0].hist[0] = 100;
    w.cls[1].pixels = 200;
    w.cls[1].sumErr1024 = 10240;   // mean 0.05
    w.cls[1].hist[23] = 200;
    w.cls[1].histId[0] = 200;
    w.cls[2].pixels = 300;
    w.cls[2].sumErr1024 = 30720;   // mean 0.1
    w.cls[2].hist[0] = 150;
    w.cls[2].hist[31] = 150;
    w.cls[2].sumErrId1024 = 30720;
    w.cls[2].histId[23] = 300;
    w.gpuMsMedian = 0.85f;
    w.gpuMsMax = 1.2f;
    w.gpuSamples = 3;
    w.rigDegMedian = 0.012f;
    w.rigDegMax = 0.034f;
    w.rigMmMedian = 0.4f;
    w.rigMmMax = 1.25f;
    // The jump set (far only, a mean of 0.05), then the CPU motion facts, then the second kernel's mv set (world only, a perfect landing).
    w.jump.frames = 5;
    w.jump.cells = 40;
    w.jump.cls[0].pixels = 20;
    w.jump.cls[0].sumErr1024 = 1024;   // mean 0.05
    w.jump.cls[0].hist[23] = 20;
    w.mv.frames = 60;
    w.mv.cells = 100;
    w.mv.cls[2].pixels = 50;
    w.mv.cls[2].hist[0] = 50;
    w.motion.motionN = 60; w.motion.motionMedian = 12.34f; w.motion.motionMax = 80.0f;
    w.motion.magN = 50; w.motion.magP5 = 0.5f; w.motion.magP50 = 1.0f; w.motion.magP95 = 2.0f;
    w.motion.dtRatioN = 55; w.motion.dtRatioP5 = 0.5f; w.motion.dtRatioP50 = 1.0f; w.motion.dtRatioP95 = 2.0f;
    w.motion.dtN = 56; w.motion.dtMedian = 11.1f; w.motion.dtMax = 22.3f;
    w.motion.rigDtN = 40; w.motion.rigDtMedian = 0.4f; w.motion.rigDtMax = 1.25f;
    const std::string line = formatFrameGenShadowReport(w);
    checkText(line,
              "frame generation (shadow): frames=60 skipped=1/2/3/4/5 far=10.0%,0,0,0,0.0%,0.0%|id:0,0 cockpit=20.0%,50,48,48,100.0%,0.0%|id:0,0 "
              "world=30.0%,100,0,193,50.0%,50.0%|id:100,48 holes=10.0% gpu_ms=0.85/1.20(n=3) rig_deg=0.012/0.034 rig_mm=0.40/1.25 "
              "jump=5 far=50.0%,50,48,48,100.0%,0.0%|id:0,0 cockpit=0.0%,0,0,0,0.0%,0.0%|id:0,0 world=0.0%,0,0,0,0.0%,0.0%|id:0,0 "
              "motion_mm=12.3/80.0 mag_ratio=0.50/1.00/2.00 dt_ratio=0.50/1.00/2.00 dt_ms=11.1/22.3 rig_dt_mm=0.40/1.25 "
              "mv=60 far=0.0%,0,0,0,0.0%,0.0%|id:0,0 cockpit=0.0%,0,0,0,0.0%,0.0%|id:0,0 world=50.0%,0,0,0,0.0%,0.0%|id:0,0",
              "the report line");
    check(line.find(" rig_dt_mm=0.40/1.25 mv=60 ") != std::string::npos, "the second kernel's fields follow rig_dt_mm, so the older heads still match");
    check(line.find('\n') == std::string::npos, "the report is one line");
    check(line.compare(0, 5, "frame") == 0 && line.find(" rig_mm=0.40/1.25 jump=5 ") != std::string::npos, "the jump set follows rig_mm, so the older head still matches");

    // An empty window still prints (no division by zero), every figure zero, the empty CPU facts "-".
    FrameGenShadowWindow z;
    checkText(formatFrameGenShadowReport(z),
              "frame generation (shadow): frames=0 skipped=0/0/0/0/0 far=0.0%,0,0,0,0.0%,0.0%|id:0,0 cockpit=0.0%,0,0,0,0.0%,0.0%|id:0,0 "
              "world=0.0%,0,0,0,0.0%,0.0%|id:0,0 holes=0.0% gpu_ms=-(n=0) rig_deg=0.000/0.000 rig_mm=0.00/0.00 "
              "jump=0 far=0.0%,0,0,0,0.0%,0.0%|id:0,0 cockpit=0.0%,0,0,0,0.0%,0.0%|id:0,0 world=0.0%,0,0,0,0.0%,0.0%|id:0,0 "
              "motion_mm=-/- mag_ratio=-/-/- dt_ratio=-/-/- dt_ms=-/- rig_dt_mm=-/- "
              "mv=0 far=0.0%,0,0,0,0.0%,0.0%|id:0,0 cockpit=0.0%,0,0,0,0.0%,0.0%|id:0,0 world=0.0%,0,0,0,0.0%,0.0%|id:0,0",
              "an empty window");

    // The longest line the format can plausibly print (every class full, every figure at its widest) stays inside Log::note's 1200-byte line: less the 15-byte stamp, the
    // NUL and the 16-byte truncation marker's reserve (log.cpp formatStamped) that leaves 1165 characters.
    FrameGenShadowWindow big = w;
    FrameGenShadowSetStats* const sets[2] = {&big.jump, &big.mv};
    for (int c = 0; c < kFrameGenShadowClasses; ++c) {
        FrameGenShadowClassStats* cs[3] = {&big.cls[c], &big.jump.cls[c], &big.mv.cls[c]};
        for (FrameGenShadowClassStats* k : cs) {
            *k = FrameGenShadowClassStats{};
            k->pixels = 1000;
            k->sumErr1024 = 1000ull * 1024ull;
            k->sumErrId1024 = 1000ull * 1024ull;
            k->hist[47] = 1000;
            k->histId[47] = 1000;
        }
    }
    big.cells = 1000;
    for (FrameGenShadowSetStats* st : sets) st->cells = 1000;
    big.holes = 1000;
    big.motion.motionMedian = 9999.9f; big.motion.motionMax = 9999.9f;
    big.rigDegMax = 179.999f; big.rigMmMax = 99999.99f;
    const std::string bigLine = formatFrameGenShadowReport(big);
    if (bigLine.size() >= 1165) std::printf("framegen_shadow_report_test: the widest report line is %zu bytes\n", bigLine.size());
    check(bigLine.size() < 1165, "the widest report line fits Log::note's 1200 bytes with the stamp and the truncation marker");
    check(bigLine.find(" mv=60 far=") != std::string::npos && bigLine.find("[truncated]") == std::string::npos, "the widest line still ends with the mv set's fields");

    // The sets merge apart: a jump merge counts its own frame and leaves the normal set alone.
    FrameGenShadowEyeResult e;
    e.cls[2].pixels = 7;
    e.cells = 9;
    FrameGenShadowSetStats s;
    frameGenShadowMergeSet(s, e);
    frameGenShadowMergeSet(s, e);
    check(s.frames == 2 && s.cls[2].pixels == 14 && s.cells == 18, "a set merge counts its frames and adds the stats");

    // The second kernel's merge: the mv set, one frame each, whatever the frame's own set.
    FrameGenShadowEyeResult p;
    p.mv.cls[2].pixels = 5; p.mv.cells = 8; p.mv.holes = 3;
    FrameGenShadowWindow pw;
    frameGenShadowMergeProbe(pw, p);
    frameGenShadowMergeProbe(pw, p);
    check(pw.mv.frames == 2 && pw.mv.cls[2].pixels == 10 && pw.mv.cells == 16 && pw.mv.holes == 6 && pw.framesScored == 0 && pw.cls[2].pixels == 0,
          "a probe merge counts a frame in the mv set, adds its stats and leaves the main set alone");

    // The VRAM line: the groups in order, the eye's total and twice it, bytes and MiB.
    FrameGenShadowVram v;
    v.captures = 6291456; v.keys = 3145728; v.stats = 11960;
    checkText(formatFrameGenShadowVram(1, v),
              "frame generation (shadow): eye 1 VRAM 9449144 B (9.0 MiB): captures=6291456 keys=3145728 stats=11960; two eyes 18898288 B (18.0 MiB)",
              "the VRAM line");
}

// ---- the head-pose miss (framegen_pose_report.h) ----

FgPose poseAt(int64_t ns, double vx, double yawRate) {   // a head moving along x at vx m/s and turning about y at yawRate rad/s, from zero at t = 0
    FgPose p;
    p.pos[0] = static_cast<float>(vx * static_cast<double>(ns) * 1e-9);
    const double a = yawRate * static_cast<double>(ns) * 1e-9;
    p.q[1] = static_cast<float>(std::sin(a * 0.5));
    p.q[3] = static_cast<float>(std::cos(a * 0.5));
    return p;
}

void casePoseMath() {
    const float a[3] = {1.0f, 2.0f, 3.0f}, b[3] = {4.0f, 6.0f, 3.0f};
    check(std::fabs(fgPositionMissMm(a, b) - 5000.0f) < 0.01f, "a 3-4-5 miss is 5000 mm");
    const FgPose p0 = poseAt(0, 0, 0), p90 = poseAt(1000000000, 0, 1.5707963267948966);
    check(std::fabs(fgRotationMissDeg(p90.q, p0.q) - 90.0f) < 0.001f && std::fabs(fgRotationMissDeg(p0.q, p90.q) - 90.0f) < 0.001f, "a quarter turn is 90 degrees either way");
    const FgPose tiny = poseAt(1000000, 0, 0.001745329252);   // 1 ms at 0.1 degree per millisecond
    check(std::fabs(fgRotationMissDeg(tiny.q, p0.q) - 0.0001f) < 0.00002f, "a ten-thousandth of a degree is measured as one");
    float out[4];
    const float omega[3] = {0, 2.0f, 0};
    fgRotateByVelocity(p0.q, omega, 0.25, out);
    check(std::fabs(fgRotationMissDeg(out, poseAt(250000000, 0, 2.0).q)) < 0.001f, "turning by the angular velocity matches the pose at that time");
    float w[3];
    fgVelocityBetween(p0.q, poseAt(5000000, 0, 2.0).q, 0.005, w);
    check(std::fabs(w[1] - 2.0f) < 0.001f && std::fabs(w[0]) < 0.001f && std::fabs(w[2]) < 0.001f, "the angular velocity between two poses is the one that turned them");
    std::vector<float> v;
    for (int i = 20; i >= 1; --i) v.push_back(static_cast<float>(i));
    float s[3];
    fgSummarise(v, s);
    check(s[0] == 11.0f && s[1] == 19.0f && s[2] == 20.0f, "median, p95 and max of 1..20 are 11, 19, 20");
    std::vector<float> none;
    fgSummarise(none, s);
    check(s[0] == 0.0f && s[1] == 0.0f && s[2] == 0.0f, "no values summarise to zero");
}

constexpr int64_t kPeriod = 11111111;   // 90 Hz
constexpr int64_t kBase = 100000000;
constexpr double kVx = 1.0, kYaw = 2.0;

// Six real frames at T_i = base + i P, each predicted 3 ms before T; the runtime answers a time past now + 10 ms with the pose at now + 10 ms.
FrameGenPoseWindow runProbe(bool angularValid, bool refuseTruth, bool refusePrediction, bool resolveEach) {
    FrameGenPoseProbe probe;
    for (int i = 0; i < 6; ++i) {
        FrameGenPoseProbe::Frame f;
        f.displayTime = kBase + i * kPeriod;
        f.period = kPeriod;
        f.now = f.displayTime - 3000000;
        f.angularValid = angularValid;
        f.omega[1] = static_cast<float>(kYaw);
        const int64_t now = f.now;
        const auto early = [&](int64_t t, FgPose& out) {
            if (refusePrediction && t > f.displayTime) return FgLocate::Refused;
            out = poseAt(t > now + 10000000 ? now + 10000000 : t, kVx, kYaw);   // the runtime's limit: it does not predict past 10 ms
            return FgLocate::Ok;
        };
        const auto late = [&](int64_t t, FgPose& out) {
            if (refuseTruth) return FgLocate::Refused;
            out = poseAt(t, kVx, kYaw);
            return FgLocate::Ok;
        };
        probe.onFrame(f, poseAt(f.displayTime, kVx, kYaw), early);
        if (resolveEach) probe.resolve(now, late);
    }
    FrameGenPoseWindow w;
    probe.takeWindow(w);
    return w;
}

void casePoseProbe() {
    const FrameGenPoseWindow w = runProbe(true, false, false, true);
    check(w.frames == 4 && w.expired == 0 && w.refused == 0 && w.unpredicted == 0, "six frames, 3 ms before T each: the first four are scored once the clock passes their T_S");
    checkText(formatFrameGenPoseReport(w),
              "frame generation (shadow) pose: frames=4 lead_ms=14.1 runtime_mm=4.11/4.11/4.11 runtime_deg=0.471/0.471/0.471 velocity_mm=0.00/0.00/0.00 "
              "velocity_deg=0.000/0.000/0.000 vel_src=3/0/1 expired=0 refused=0 unpredicted=0 slot=0 slot_mm=0.00/0.00/0.00 slot_deg=0.000/0.000/0.000 slot_lead_ms=0.0",
              "a runtime that stops predicting at 10 ms misses by the rest of the lead at the head's speed; the velocity fallback does not");
    const FrameGenPoseWindow pair = runProbe(false, false, false, true);
    check(pair.frames == 4 && pair.srcXr == 0 && pair.srcPair == 3 && pair.srcNone == 1 && pair.vDeg[2] < 0.001f && pair.vMm[2] < 0.01f,
          "without the runtime's angular velocity the two locates 5 ms apart carry the rotation: the same fallback, counted as pair");
    const FrameGenPoseWindow refused = runProbe(true, true, false, true);
    check(refused.frames == 0 && refused.refused == 4 && refused.rtMm[2] == 0.0f, "a runtime that refuses a past time scores nothing and counts the refusals");
    const FrameGenPoseWindow unpredicted = runProbe(true, false, true, true);
    check(unpredicted.unpredicted == 6 && unpredicted.frames == 4 && unpredicted.rtMm[2] == 0.0f && unpredicted.vMm[2] < 0.01f,
          "a runtime that refuses T_S at t0 is counted as unpredicted and the fallback is still scored");
    const FrameGenPoseWindow ring = runProbe(true, false, false, false);
    check(ring.frames == 0 && ring.expired == 0, "six unresolved frames fit the ring of eight");
    FrameGenPoseProbe probe;
    for (int i = 0; i < 10; ++i) {
        FrameGenPoseProbe::Frame f;
        f.displayTime = kBase + i * kPeriod;
        f.period = kPeriod;
        f.now = f.displayTime - 3000000;
        probe.onFrame(f, poseAt(f.displayTime, kVx, kYaw), [](int64_t t, FgPose& o) { o = poseAt(t, kVx, kYaw); return FgLocate::Ok; });
    }
    FrameGenPoseWindow full;
    probe.takeWindow(full);
    check(full.expired == 2, "ten frames never resolved: the two oldest fall out of the ring unscored");
    FrameGenPoseProbe stale;
    FrameGenPoseProbe::Frame f;
    f.displayTime = kBase;
    f.period = kPeriod;
    f.now = kBase - 3000000;
    stale.onFrame(f, poseAt(f.displayTime, kVx, kYaw), [](int64_t t, FgPose& o) { o = poseAt(t, kVx, kYaw); return FgLocate::Ok; });
    stale.resolve(kBase + kPeriod + 2000000000, [](int64_t t, FgPose& o) { o = poseAt(t, kVx, kYaw); return FgLocate::Ok; });
    FrameGenPoseWindow late;
    check(stale.takeWindow(late) && late.expired == 1 && late.frames == 0, "a frame whose T_S is more than a second behind the clock is dropped unscored");
    FrameGenPoseProbe quiet;
    FrameGenPoseWindow nothing;
    check(!quiet.takeWindow(nothing), "a window with nothing in it says nothing");

    // Phase 2.3, H6 at the slot: the slot pose located 20 ms ahead by a runtime that predicts only 10 ms misses by the other 10 ms at the head's speed; one exactly right does not.
    FrameGenPoseProbe slots;
    const auto truth = [](int64_t t, FgPose& o) { o = poseAt(t, kVx, kYaw); return FgLocate::Ok; };
    for (int i = 0; i < 3; ++i) {
        const int64_t now = kBase + i * kPeriod, target = now + 20000000;
        slots.onSlot(target, now, poseAt(i == 2 ? target : now + 10000000, kVx, kYaw));
    }
    slots.resolve(kBase + kPeriod + 15000000, truth);   // only the first slot's T_S has come
    FrameGenPoseWindow sw1;
    check(slots.takeWindow(sw1) && sw1.slotFrames == 1 && sw1.frames == 0 && std::fabs(sw1.slotMm[2] - 10.0f) < 0.05f && std::fabs(sw1.slotDeg[2] - 1.15f) < 0.02f &&
              std::fabs(sw1.slotLeadMsMedian - 20.0f) < 0.01f,
          "a slot pose 10 ms short of its 20 ms lead misses by 10 mm and 1.15 deg at 1 m/s and 2 rad/s, scored only once its T_S has come");
    slots.resolve(kBase + 2 * kPeriod + 25000000, truth);
    FrameGenPoseWindow sw2;
    check(slots.takeWindow(sw2) && sw2.slotFrames == 2 && std::fabs(sw2.slotMm[2] - 10.0f) < 0.05f, "...and the next two are scored at the next resolve");
    FrameGenPoseProbe exact;
    exact.onSlot(kBase + 20000000, kBase, poseAt(kBase + 20000000, kVx, kYaw));
    exact.resolve(kBase + 21000000, truth);
    FrameGenPoseWindow swe;
    check(exact.takeWindow(swe) && swe.slotFrames == 1 && swe.slotMm[2] < 0.01f && swe.slotDeg[2] < 0.001f, "a slot pose located exactly where the head goes misses by nothing");
    FrameGenPoseProbe slotRing;
    for (int i = 0; i < 10; ++i) slotRing.onSlot(kBase + i * kPeriod + 20000000, kBase + i * kPeriod, poseAt(kBase, kVx, kYaw));
    FrameGenPoseWindow sw3;
    check(slotRing.takeWindow(sw3) && sw3.expired == 2 && sw3.slotFrames == 0, "ten slot poses never resolved: the two oldest fall out of the ring unscored");
    FrameGenPoseProbe slotRefused;
    slotRefused.onSlot(kBase + 20000000, kBase, poseAt(kBase, kVx, kYaw));
    slotRefused.resolve(kBase + 30000000, [](int64_t, FgPose&) { return FgLocate::Refused; });
    FrameGenPoseWindow sw4;
    check(slotRefused.takeWindow(sw4) && sw4.refused == 1 && sw4.slotFrames == 0, "a slot whose truth the runtime refuses is counted refused");
}

void casePoseLine() {
    FrameGenPoseWindow w;
    w.frames = 60;
    w.leadMsMedian = 14.1f;
    w.rtMm[0] = 1.5f; w.rtMm[1] = 2.5f; w.rtMm[2] = 3.5f;
    w.rtDeg[0] = 0.25f; w.rtDeg[1] = 0.5f; w.rtDeg[2] = 0.75f;
    w.vMm[0] = 0.5f; w.vMm[1] = 1.0f; w.vMm[2] = 1.5f;
    w.vDeg[0] = 0.1f; w.vDeg[1] = 0.2f; w.vDeg[2] = 0.3f;
    w.srcXr = 50; w.srcPair = 5; w.srcNone = 5;
    w.expired = 1; w.refused = 2; w.unpredicted = 3;
    w.slotFrames = 45; w.slotMm[0] = 0.4f; w.slotMm[1] = 0.9f; w.slotMm[2] = 1.2f; w.slotDeg[0] = 0.05f; w.slotDeg[1] = 0.1f; w.slotDeg[2] = 0.15f; w.slotLeadMsMedian = 21.5f;
    const std::string line = formatFrameGenPoseReport(w);
    checkText(line,
              "frame generation (shadow) pose: frames=60 lead_ms=14.1 runtime_mm=1.50/2.50/3.50 runtime_deg=0.250/0.500/0.750 velocity_mm=0.50/1.00/1.50 "
              "velocity_deg=0.100/0.200/0.300 vel_src=50/5/5 expired=1 refused=2 unpredicted=3 slot=45 slot_mm=0.40/0.90/1.20 slot_deg=0.050/0.100/0.150 slot_lead_ms=21.5",
              "the pose line");
    check(line.find('\n') == std::string::npos && line.find("(shadow):") == std::string::npos, "the pose line is one line and does not carry the scorer's label");
}

}  // namespace

int main(int argc, char** argv) {
    bool run = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--self-test") || !std::strcmp(argv[i], "--dry-run")) run = true;
        else {
            std::fprintf(stderr, "usage: framegen_shadow_report_test --self-test | --dry-run\n");
            return 2;
        }
    }
    if (!run) {
        std::fprintf(stderr, "usage: framegen_shadow_report_test --self-test | --dry-run\n");
        return 2;
    }
    caseBins();
    caseStats();
    caseMerge();
    caseLines();
    casePoseMath();
    casePoseProbe();
    casePoseLine();
    if (g_fail) {
        std::printf("framegen_shadow_report_test: %d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("framegen_shadow_report_test: OK\n");
    return 0;
}

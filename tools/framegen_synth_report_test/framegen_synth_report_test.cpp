// framegen_synth_report_test: the pure half of the frame-generation synthesizer (src/common/framegen_synth_report.h and framegen_synth_math.h; fix.frame_generation_shadow,
// TEMPORARY, docs/design-frame-generation-2026-10-26.md Phase 2.1): the exact text of the armed and report lines, the window's merge, the head's step from B to S (the rotation
// raised to the power f, the translation), the constant block's layout, the stats decode, and the CPU reference of a pixel's landing. Needs no GPU.
//
//   --dry-run    the same run (the rig never writes a file), for the gate's --dry-run convention
//   --self-test  every check
#include "framegen_synth_math.h"
#include "framegen_slot_report.h"
#include "framegen_monitor_text.h"   // the Monitor page's FRAME GENERATION block
#include "framegen_gate.h"           // Phase 2.4: the performance gate (fix.frame_generation = auto)

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace edvr;

namespace {

int g_fail = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("framegen_synth_report_test: FAIL: %s\n", what);
        ++g_fail;
    }
}

void checkText(const std::string& got, const char* want, const char* what) {
    if (got != want) {
        std::printf("framegen_synth_report_test: FAIL: %s\n  got:  %s\n  want: %s\n", what, got.c_str(), want);
        ++g_fail;
    }
}

constexpr float kPi = 3.14159265358979f;

void rotAxis(int axis, float deg, float m[9]) {
    const float r = deg * kPi / 180.0f, c = std::cos(r), s = std::sin(r);
    const float y[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
    const float x[9] = {1, 0, 0, 0, c, -s, 0, s, c};
    const float z[9] = {c, -s, 0, s, c, 0, 0, 0, 1};
    std::memcpy(m, axis == 0 ? x : (axis == 1 ? y : z), sizeof(y));
}

void caseLines() {
    checkText(formatFrameGenSynthArmed(612, 699, 1224, 1398, 10),
              "frame generation (synth): armed, render 612x699 output 1224x1398 per eye, S format 10, f=0.5 built on the even frames and f=1 scored against the next real frame (nothing is shown)",
              "the armed line");

    FrameGenSynthWindow w;
    w.frames[0] = 40;
    w.frames[1] = 39;
    w.scored = 38;
    w.skipped = 2;
    w.gpuSamples[0] = 40;
    w.gpuMedian[0] = 0.31f;
    w.gpuMax[0] = 0.8f;   // the f = 1 frames were not timed: "-(n=0)"
    w.pixels = 1000000;
    w.filled = 2500;
    w.fillReproj = 2000;
    w.fillStretch = 375;
    w.left = 100;
    w.guard = 15000;
    w.cells = 1000;
    w.cls[0].pixels = 300;
    w.cls[0].hist[0] = 300;
    w.cls[1].pixels = 100;
    w.cls[1].hist[23] = 100;       // about 0.0482
    w.cls[1].sumErr1024 = 5120;    // mean 0.05
    w.cls[1].histId[28] = 100;     // about 0.1146
    w.cls[1].sumErrId1024 = 10240; // mean 0.1
    w.cls[2].pixels = 600;
    w.cls[2].hist[31] = 600;       // about 0.193
    w.cls[2].sumErr1024 = 600u * 200u;
    w.clamped = 500;
    w.orphan = 30;
    w.tcells = 960;                // 40 cells fell outside S when it was turned
    w.tout = 40;
    w.tcls[0].pixels = 300;
    w.tcls[0].hist[0] = 300;
    w.tcls[2].pixels = 660;
    w.tcls[2].hist[3] = 660;       // about 0.0037
    w.tcls[2].sumErr1024 = 660u * 4u;
    w.rotSamples = 38;
    w.rotMedian = 0.4f;
    w.rotMax = 1.2345f;
    w.poseRuntime = 18;
    w.poseFallback = 22;
    w.pathBits = 2;
    w.busy = 3;
    const std::string line = formatFrameGenSynthReport(w);
    checkText(line,
              "frame generation (synth): frames=40/39 scored=38 skipped=2 gpu_ms=0.31/0.80(n=40)/-(n=0) holes=0.25%/0.01% guard=1.50% clamp=0.050% orphan=0.003% fill_reproj=80.0% fill_stretch=15.0%"
              " far=30.0%,0,0,0,0.0%,0.0%|id:0,0 cockpit=10.0%,50,48,48,100.0%,0.0%|id:100,115 world=60.0%,195,193,193,100.0%,100.0%|id:0,0"
              " turned: far=31.2%,0,0,0,0.0%,0.0%|id:0,0 cockpit=0.0%,0,0,0,0.0%,0.0%|id:0,0 world=68.8%,4,2,2,0.0%,0.0%|id:0,0 rot_res_deg=0.400/1.235 out=4.00% pose=18/22 s=uav busy=3",
              "the report line: counts, the two gpu fields, the hole, guard, clamp and orphan shares, the fill's kinds, the scorer's three class fields, then the same turned with the residual rotation and the share outside S, then the slot's pose counts, where S was written and the busy builds (Phase 2.2)");
    check(line.size() < 700, "the report line is well under Log::note's 1165 characters");
    {   // the widest the line can be: every number at its widest. One line must still fit.
        FrameGenSynthWindow x;
        x.frames[0] = x.frames[1] = x.scored = x.skipped = 4000000000u;
        x.gpuSamples[0] = x.gpuSamples[1] = 4000000000u;
        x.gpuMedian[0] = x.gpuMedian[1] = x.gpuMax[0] = x.gpuMax[1] = 99999.99f;
        x.pixels = 1;
        x.filled = x.left = x.guard = x.clamped = x.orphan = 1000000000000ull;
        x.fillReproj = x.fillStretch = 1000000000000ull;   // 100.0% each, the widest shares
        x.cells = x.tcells = 1;
        for (int c = 0; c < kFrameGenShadowClasses; ++c) {
            x.cls[c].pixels = x.tcls[c].pixels = 1;
            x.cls[c].hist[47] = x.tcls[c].hist[47] = 1;
            x.cls[c].histId[47] = x.tcls[c].histId[47] = 1;
            x.cls[c].sumErr1024 = x.tcls[c].sumErr1024 = 4000000000u;
            x.cls[c].sumErrId1024 = x.tcls[c].sumErrId1024 = 4000000000u;
        }
        x.tout = 1000000000000ull;
        x.rotSamples = 1;
        x.rotMedian = x.rotMax = 179.999f;
        x.poseRuntime = x.poseFallback = x.busy = 4000000000u;
        x.pathBits = 7;
        x.finished = x.unfinished = 4000000000u;
        x.unfinishedWhy = kFrameGenFinishLayerOnly;
        x.gpuSamples[2] = 4000000000u;
        x.gpuMedian[2] = x.gpuMax[2] = 99999.99f;
        const std::string widest = formatFrameGenSynthReport(x);
        std::printf("framegen_synth_report_test: widest report line %zu characters\n", widest.size());
        check(widest.size() < 1100, "the widest report line, every number at its widest, is under Log::note's 1165 characters in one line");
    }

    {   // Phase 2.3b: the finish's tail, only in a window that finished or refused to finish an S.
        FrameGenSynthWindow fw = w;
        fw.finished = 88; fw.unfinished = 2; fw.unfinishedWhy = kFrameGenFinishScope;
        fw.gpuSamples[2] = 90; fw.gpuMedian[2] = 0.21f; fw.gpuMax[2] = 0.4f;
        const std::string fl = formatFrameGenSynthReport(fw);
        checkText(fl.substr(line.size()), " finish=88/2 finish_ms=0.21/0.40(n=90) why=scope",
                  "the finish's tail (Phase 2.3b): finished / unfinished, the finish's GPU time per eye, the last reason one was left unfinished");
        fw.unfinished = 0;
        check(formatFrameGenSynthReport(fw).substr(line.size()) == " finish=88/0 finish_ms=0.21/0.40(n=90) why=finished", "...and why=finished in a window that left none unfinished");
        // The homography the finish carries the layer by: the identity step is exactly the identity; a turn about y moves the centre by the tangent of the angle in pixels.
        float H[9], id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, t0[3] = {}, q[2];
        const float tan4[4] = {-1.0f, 1.0f, -0.75f, 0.75f};
        frameGenSynthFinishHomography(id, t0, tan4, 3756, 3264, kFrameGenFinishPanelMetres, H);
        check(std::memcmp(H, id, sizeof(H)) == 0, "the finish's homography for no head step is exactly the identity");
        const float a = 2.0f * 3.14159265f / 180.0f, ry[9] = {std::cos(a), 0, std::sin(a), 0, 1, 0, -std::sin(a), 0, std::cos(a)};
        frameGenSynthFinishHomography(ry, t0, tan4, 3756, 3264, kFrameGenFinishPanelMetres, H);
        // x_S = R x_B: S's centre (0, 0, -1) is R^T (0, 0, -1) = (sin a, 0, -cos a) in B's eye, B's pixel at tan(a).
        const bool landed = frameGenSynthFinishLanding(H, 1877.5f, 1631.5f, q);
        const float want = (std::tan(a) - tan4[0]) / (tan4[1] - tan4[0]) * 3756.0f - 0.5f;
        check(landed && std::fabs(q[0] - want) < 0.01f && std::fabs(q[1] - 1631.5f) < 0.01f, "the finish's homography: a 2 degree turn about y carries S's centre to B's pixel at tan(2 deg)");
    }

    FrameGenSynthWindow idle;
    idle.skipped = 7;
    checkText(formatFrameGenSynthReport(idle),
              "frame generation (synth): frames=0/0 scored=0 skipped=7 gpu_ms=-(n=0)/-(n=0) holes=0.00%/0.00% guard=0.00% clamp=0.000% orphan=0.000% fill_reproj=0.0% fill_stretch=0.0%"
              " far=0.0%,0,0,0,0.0%,0.0%|id:0,0 cockpit=0.0%,0,0,0,0.0%,0.0%|id:0,0 world=0.0%,0,0,0,0.0%,0.0%|id:0,0"
              " turned: far=0.0%,0,0,0,0.0%,0.0%|id:0,0 cockpit=0.0%,0,0,0,0.0%,0.0%|id:0,0 world=0.0%,0,0,0,0.0%,0.0%|id:0,0 rot_res_deg=-/- out=0.00% pose=0/0 s=none busy=0",
              "a window that called the module and built nothing still says so (frames=0/0, the skips), so a synth that never ran reads differently from one that did");

    // The VRAM line: the eye's groups and the shared scratch's in order, each with its total, then two eyes and the scratch, bytes and MiB.
    FrameGenSynthVram v;
    v.s = 49038336; v.luma = 862920; v.cls = 3451680; v.stats = 12060;
    v.key = 27584064; v.read = 27584064; v.holes = 3448008;
    checkText(formatFrameGenSynthVram(0, v),
              "frame generation (synth): eye 0 VRAM 53364996 B (50.9 MiB): s=49038336 luma=862920 class=3451680 stats=12060 shared=0; shared scratch 58616136 B (55.9 MiB): "
              "key=27584064 read=27584064 holes=3448008; two eyes and the scratch 165346128 B (157.7 MiB)",
              "the VRAM line");
    // Phase 2.2: on the uav path the producer texture replaces S; its bytes are the shared group.
    v.shared = v.s;
    v.s = 0;
    checkText(formatFrameGenSynthVram(1, v),
              "frame generation (synth): eye 1 VRAM 53364996 B (50.9 MiB): s=0 luma=862920 class=3451680 stats=12060 shared=49038336; shared scratch 58616136 B (55.9 MiB): "
              "key=27584064 read=27584064 holes=3448008; two eyes and the scratch 165346128 B (157.7 MiB)",
              "the VRAM line on the uav path: S is the producer texture (shared), the private S is gone");
    check(std::strcmp(frameGenSynthPathName(1), "private") == 0 && std::strcmp(frameGenSynthPathName(2), "uav") == 0 && std::strcmp(frameGenSynthPathName(4), "copy") == 0 &&
              std::strcmp(frameGenSynthPathName(5), "mixed") == 0 && std::strcmp(frameGenSynthPathName(0), "none") == 0,
          "the path names: private, uav, copy, mixed, none");
}

// Phase 2.2, the runtime's half (framegen_slot_report.h): T_S, and the transfer line.
void caseSlot() {
    int64_t ts = -1;
    // 45 fps game on a 90 Hz display: T - T_A = 22.222 ms, so T_S = T + 11.111 ms (the synthesizer's f = 0.5; in Lock 2:1 this is T + P).
    check(frameGenSlotTime(1000000000, 1000000000 - 22222222, true, ts) && ts == 1000000000 + 11111111, "T_S at 45/90: half an Elite step past T (11.111 ms)");
    check(frameGenSlotTime(1000000000, 1000000000 - 11111111, true, ts) && ts == 1000000000 + 5555555, "T_S at 90/90: half a period past T");
    check(!frameGenSlotTime(1000000000, 0, false, ts) && ts == 0, "no previous pair: no T_S");
    check(!frameGenSlotTime(1000000000, 1000000000 - 1999998, true, ts) && ts == 0, "a lead under 1 ms: none");
    check(frameGenSlotTime(1000000000, 1000000000 - 2000000, true, ts) && ts == 1000000000 + 1000000, "a lead of exactly 1 ms is taken");
    check(frameGenSlotTime(1000000000, 1000000000 - 100000000, true, ts) && ts == 1000000000 + 50000000, "a lead of exactly 50 ms is taken");
    check(!frameGenSlotTime(1000000000, 1000000000 - 100000002, true, ts) && ts == 0, "a lead over 50 ms (a missed pair or more): none");
    check(!frameGenSlotTime(1000000000, 1000000000, true, ts) && !frameGenSlotTime(1000000000, 1000000001, true, ts) && !frameGenSlotTime(0, -5, true, ts), "a T not after T_A, or no T: none");
    check(frameGenSlotFormatBytes(26) == 4 && frameGenSlotFormatBytes(10) == 8 && frameGenSlotFormatBytes(2) == 16 && frameGenSlotFormatBytes(28) == 4 && frameGenSlotFormatBytes(87) == 4 &&
              frameGenSlotFormatBytes(0) == 0,
          "texel bytes: R11G11B10 4, RGBA16F 8, RGBA32F 16, RGBA8 4, BGRA8 4, unknown 0");

    FrameGenSlotWindow w;
    w.produced = 45; w.consumed = 44; w.busy = 1; w.late = 2;
    w.readyMs = {-9.5f, -8.25f, -9.0f, 0.5f, 1.25f};
    w.consumeMs = {0.02f, 0.03f, 0.05f};
    w.poseRuntime = 44; w.poseNone = 1;
    w.pathUav = true;
    w.gameBytes = 2ull * 49038336ull;
    w.xrBytes = 2ull * 49038336ull;
    checkText(formatFrameGenSlotReport(w),
              "frame generation (synth) transfer: produced=45 consumed=44 s_busy=1 s_late=2 ready_ms=-8.25/1.25 consume_ms=0.03/0.05 pose=44/1 path=uav vram_game_mib=93.5 vram_xr_mib=93.5",
              "the transfer line: produced, consumed, busy, late, ready against T_S (median/max), the consume's time, the pose counts, the path, both devices' VRAM");
    FrameGenSlotWindow none;
    none.poseNone = 3;
    checkText(formatFrameGenSlotReport(none),
              "frame generation (synth) transfer: produced=0 consumed=0 s_busy=0 s_late=0 ready_ms=-/- consume_ms=-/- pose=0/3 path=none vram_game_mib=0.0 vram_xr_mib=0.0",
              "a window with slots handed and no S produced says so (path none, no times)");
    check(none.any() && !FrameGenSlotWindow{}.any(), "a window with only pose counts is something to say; an empty one is not");
    w.reset();
    check(w.produced == 0 && w.readyMs.empty() && w.gameBytes == 2ull * 49038336ull, "a window reset keeps the bytes as they stand (state), and clears the counts");
    w.pathCopy = true;
    check(std::strcmp(frameGenSlotPathName(true, true), "mixed") == 0 && std::strcmp(frameGenSlotPathName(false, true), "copy") == 0, "the path: mixed when both were seen, copy alone");
}

void caseMerge() {
    FrameGenSynthWindow w;
    FrameGenSynthEyeResult built;
    built.filled = 10;
    built.left = 2;
    built.guard = 7;
    built.clamped = 3;
    built.fillReproj = 6;
    built.fillStretch = 1;
    built.orphan = 4;
    frameGenSynthMergeFrame(w, 0, 100, built);
    frameGenSynthMergeFrame(w, 1, 100, built);
    frameGenSynthMergeFrame(w, 1, 100, built);
    check(w.frames[0] == 1 && w.frames[1] == 2 && w.pixels == 300 && w.filled == 30 && w.left == 6 && w.guard == 21 && w.clamped == 9 && w.fillReproj == 18 && w.fillStretch == 3 &&
              w.orphan == 12,
          "frames are counted by kind and the counters (the fill's kinds and the orphans too) and pixels add");
    FrameGenSynthEyeResult sc;
    sc.cells = 12;
    sc.cls[2].pixels = 12;
    sc.cls[2].hist[5] = 12;
    sc.cls[2].sumErr1024 = 24;
    sc.tcells = 10;
    sc.tout = 2;
    sc.tcls[2].pixels = 10;
    sc.tcls[2].hist[1] = 10;
    frameGenSynthMergeScore(w, sc);
    frameGenSynthMergeScore(w, sc);
    check(w.scored == 2 && w.cells == 24 && w.cls[2].pixels == 24 && w.cls[2].hist[5] == 24 && w.cls[2].sumErr1024 == 48, "a scored frame adds its classes and counts once");
    check(w.tcells == 20 && w.tout == 4 && w.tcls[2].pixels == 20 && w.tcls[2].hist[1] == 20 && w.tcls[0].pixels == 0, "and its turned classes, cells and cells outside S");
    check(w.cls[0].pixels == 0 && w.cls[1].pixels == 0, "the other classes stay empty");
}

void caseDecode() {
    uint32_t words[kFrameGenSynthStatsUints] = {};
    words[0] = 3;                                                   // far hist bin 0
    words[2 * kFrameGenShadowBins] = 3;                             // far pixels
    words[kFrameGenShadowStatsPerClass * 2 + 2 * kFrameGenShadowBins] = 9;   // world pixels
    words[kFrameGenShadowStatsCells] = 12;
    words[kFrameGenSynthStatsFilled] = 40;
    words[kFrameGenSynthStatsLeft] = 5;
    words[kFrameGenSynthStatsGuard] = 77;
    words[kFrameGenSynthStatsClamp] = 4;
    words[kFrameGenSynthStatsFillReproj] = 30;
    words[kFrameGenSynthStatsFillStretch] = 8;
    words[kFrameGenSynthStatsOrphan] = 11;
    words[kFrameGenSynthStatsTurned + 2 * kFrameGenShadowBins] = 6;                             // turned far pixels
    words[kFrameGenSynthStatsTurned + kFrameGenShadowStatsHoles] = 2;                           // turned: outside S
    words[kFrameGenSynthStatsTurned + kFrameGenShadowStatsCells] = 6;
    FrameGenSynthEyeResult r;
    frameGenSynthDecodeStats(words, r);
    check(r.cells == 12 && r.cls[0].pixels == 3 && r.cls[0].hist[0] == 3 && r.cls[2].pixels == 9 && r.filled == 40 && r.left == 5 && r.guard == 77,
          "the stats words decode: the score block as the scorer's, then filled, left and guard");
    check(r.clamped == 4 && r.tcls[0].pixels == 6 && r.tout == 2 && r.tcells == 6 && r.tcls[2].pixels == 0, "and the clamp count and the turned block (its holes word is the cells outside S)");
    check(r.fillReproj == 30 && r.fillStretch == 8, "and the fill's kinds (reprojected, stretched)");
    check(r.orphan == 11, "and the orphans (a key and no read offer, made holes)");
    check(kFrameGenSynthStatsUints == 606 && kFrameGenSynthStatsFilled == 299 && kFrameGenSynthStatsClamp == 302 && kFrameGenSynthStatsTurned == 303 && kFrameGenSynthStatsHoles == 602 &&
              kFrameGenSynthStatsFillReproj == 603 && kFrameGenSynthStatsFillStretch == 604 &&
              kFrameGenSynthStatsOrphan == 605,
          "the stats layout is 299 score words, four counters, the turned block of 299, the hole list's count, the fill's kinds, then the orphans");
    check(frameGenSynthHoleCapacity(2817, 2448) == 862002 && frameGenSynthHoleCapacity(8, 8) == 8, "the hole list holds an eighth of the render pixels");
}

void caseRotation() {
    float R[9], half[9], full[9], twice[9];
    rotAxis(1, 6.0f, R);
    frameGenSynthRotationPow(R, 1.0f, full);
    double worst = 0.0;
    for (int i = 0; i < 9; ++i) worst = std::fmax(worst, std::fabs(full[i] - R[i]));
    check(worst < 1e-6, "R^1 is R");
    frameGenSynthRotationPow(R, 0.0f, half);
    worst = 0.0;
    for (int i = 0; i < 9; ++i) worst = std::fmax(worst, std::fabs(half[i] - ((i % 4 == 0) ? 1.0f : 0.0f)));
    check(worst < 1e-6, "R^0 is the identity");
    frameGenSynthRotationPow(R, 0.5f, half);
    temporalMul3(half, half, twice);
    worst = 0.0;
    for (int i = 0; i < 9; ++i) worst = std::fmax(worst, std::fabs(twice[i] - R[i]));
    check(worst < 1e-6, "(R^0.5)^2 is R");
    check(std::fabs(temporalRotationAngleDeg(half) - 3.0f) < 1e-3f, "R^0.5 of a 6 degree turn is 3 degrees");
    float q[9];
    rotAxis(0, 90.0f, q);
    frameGenSynthRotationPow(q, 2.0f, half);   // beyond the short way: 180 degrees about x
    check(std::fabs(half[4] + 1.0f) < 1e-5f && std::fabs(half[8] + 1.0f) < 1e-5f && std::fabs(half[0] - 1.0f) < 1e-5f, "R^2 of a 90 degree turn is the half turn");
    rotAxis(2, -170.0f, q);
    frameGenSynthRotationPow(q, 0.5f, half);
    check(std::fabs(temporalRotationAngleDeg(half) - 85.0f) < 1e-3f, "a big negative turn takes its half the short way");
}

void caseHeadStep() {
    // B's backward head delta is a 3 degree turn about y and a 20 mm shift; the forward step A to B is its inverse.
    FrameGenDeltas d;
    rotAxis(1, 3.0f, d.Hd);
    d.th[0] = 0.02f;
    d.th[1] = -0.01f;
    d.th[2] = 0.004f;
    float rot[9], t[3];
    frameGenSynthHeadStep(d, 1.0f, rot, t);
    float back[3], p[3] = {0.3f, -0.2f, -1.1f}, a[3], b[3];
    temporalApply3(d.Hd, p, a);                    // a point of B's eye space in A's
    for (int i = 0; i < 3; ++i) a[i] += d.th[i];
    temporalApply3(rot, a, back);                  // carried forward A to B by the f = 1 step
    for (int i = 0; i < 3; ++i) b[i] = back[i] + t[i];
    check(std::fabs(b[0] - p[0]) < 1e-5f && std::fabs(b[1] - p[1]) < 1e-5f && std::fabs(b[2] - p[2]) < 1e-5f, "the f = 1 head step is the forward step A to B (the inverse of the backward delta)");
    float rotH[9], tH[3];
    frameGenSynthHeadStep(d, 0.5f, rotH, tH);
    check(std::fabs(temporalRotationAngleDeg(rotH) - 1.5f) < 1e-3f, "the f = 0.5 head step turns half as far");
    check(std::fabs(tH[0] - 0.5f * t[0]) < 1e-7f && std::fabs(tH[1] - 0.5f * t[1]) < 1e-7f && std::fabs(tH[2] - 0.5f * t[2]) < 1e-7f, "and its translation is half");
    check(t[0] < -0.015f && t[1] > 0.005f, "the forward translation is the backward one reversed (the head moved +x in A's frame, so B is at -x of it)");
}

void caseCb() {
    FrameGenSynthCb cb;
    FrameGenDeltas d;
    rotAxis(1, 2.0f, d.Hd);
    d.th[0] = 0.01f;
    const float tan[4] = {-1.0f, 1.0f, -0.75f, 0.75f};
    frameGenSynthFillCb(cb, 128, 96, 256, 192, tan, 0.0f, 0.025f, 10.0f, 0.5f, d);
    check(cb.sz[0] == 128 && cb.sz[1] == 96 && cb.sz[2] == 256 && cb.sz[3] == 192 && cb.qz[0] == 32 && cb.qz[1] == 24, "the sizes and the quarter grid");
    check(cb.dk[0] == 0.0f && cb.dk[1] == 0.025f && cb.dk[2] == 10.0f && cb.dk[3] == 0.5f, "the depth encoding, the split and f ride dk");
    check(std::fabs(cb.hp[0][0] - d.Hd[0]) < 1e-7f && std::fabs(cb.hp[0][2] - d.Hd[2]) < 1e-7f && std::fabs(cb.hp[2][0] - d.Hd[6]) < 1e-7f, "hp holds the backward head rotation, row by row");
    float rot[9], t[3];
    frameGenSynthHeadStep(d, 0.5f, rot, t);
    check(std::fabs(cb.rs[1][1] - rot[4]) < 1e-7f && std::fabs(cb.rs[0][2] - rot[2]) < 1e-7f && std::fabs(cb.rs[0][3] - t[0]) < 1e-9f, "rs holds the step, translation in w");
    check(sizeof(FrameGenSynthCb) == 224, "fourteen rows of 16 bytes");
    check(cb.rr[0][0] == 1.0f && cb.rr[1][1] == 1.0f && cb.rr[2][2] == 1.0f && cb.rr[0][1] == 0.0f && cb.tanK[1] == cb.tanB[1], "off a score the residual is the identity and the kept tangents are this frame's");
}

void caseTarget() {
    FrameGenSynthCb cb;
    FrameGenDeltas still;
    const float tan[4] = {-1.0f, 1.0f, -0.75f, 0.75f};
    frameGenSynthFillCb(cb, 128, 96, 128, 96, tan, 0.0f, 0.025f, 10.0f, 0.5f, still);
    float pp[2];
    int cls = -1;
    // The world with no head motion: the ship's screen motion m (the vector, B to A) carried back by f: p - f m.
    check(frameGenSynthTarget(cb, 40.0f, 30.0f, 50.0f, false, 6.0f, -2.0f, pp, &cls) && cls == 2 && std::fabs(pp[0] - 37.0f) < 1e-3f && std::fabs(pp[1] - 31.0f) < 1e-3f,
          "a world pixel with the head still moves by -f of its vector (50 m, vector (6, -2), f 0.5: (37, 31))");
    check(frameGenSynthTarget(cb, 40.0f, 30.0f, 0.0f, true, 0.0f, 0.0f, pp, &cls) && cls == 0 && std::fabs(pp[0] - 40.0f) < 1e-3f && std::fabs(pp[1] - 30.0f) < 1e-3f,
          "a far pixel with no vector stays");
    check(frameGenSynthTarget(cb, 40.0f, 30.0f, 0.5f, false, 9.0f, 9.0f, pp, &cls) && cls == 1 && std::fabs(pp[0] - 40.0f) < 1e-3f && std::fabs(pp[1] - 30.0f) < 1e-3f,
          "a cockpit pixel with the head still stays whatever its vector says (the class is the head's alone)");
    check(frameGenSynthTarget(cb, 40.0f, 30.0f, 10.0f, false, 0.0f, 0.0f, pp, &cls) && cls == 1, "exactly the split is the cockpit (z <= split)");
}

// The residual head rotation: S was built for the head step `kept` (B's eye space to S's), C's true head rotation is its backward delta Hd_C (C's eye space to B's).
void caseResidual() {
    float hdB[9], hdC[9], kept[9], res[9], keptT[9];
    rotAxis(1, 3.0f, hdB);
    temporalTranspose3(hdB, kept);   // the f = 1 step of a head whose backward delta was hdB
    rotAxis(1, 3.0f, hdC);
    frameGenSynthResidual(kept, hdC, res);
    check(std::fabs(temporalRotationAngleDeg(res)) < 1e-3f, "the head doing what S was built for leaves no residual");
    rotAxis(1, 5.0f, hdC);          // C's head turned 5 degrees (backward), the prediction 3
    frameGenSynthResidual(kept, hdC, res);
    check(std::fabs(temporalRotationAngleDeg(res) - 2.0f) < 1e-3f, "a head that turned 5 degrees where 3 were predicted leaves a 2 degree residual");
    // The direction: a C direction d turned by the residual lands where S shows what C shows. C's backward delta takes d to B (hdC d), the kept step takes B to S.
    const float d[3] = {0.3f, -0.2f, -1.0f};
    float dB[3], dS[3], dRes[3];
    temporalApply3(hdC, d, dB);
    temporalApply3(kept, dB, dS);
    temporalApply3(res, d, dRes);
    check(std::fabs(dS[0] - dRes[0]) < 1e-6f && std::fabs(dS[1] - dRes[1]) < 1e-6f && std::fabs(dS[2] - dRes[2]) < 1e-6f, "the residual is the kept step after C's backward delta: C's direction into S's eye");
    temporalTranspose3(kept, keptT);
    frameGenSynthResidual(keptT, hdC, res);   // the wrong way round is not the same turn
    temporalApply3(res, d, dRes);
    check(std::fabs(dS[0] - dRes[0]) > 1e-3f, "and the kept step the wrong way round is a different turn");
}

// The settings menu's Monitor page, FRAME GENERATION (framegen_monitor_text.h): the exact lines for each state, nothing with both keys off, and a runtime whose slot is older
// than size 3 reads as unknown.
void caseMonitor() {
    char l[kFrameGenMonitorMaxLines][kFrameGenMonitorLineChars];
    const auto lines = [&](const FrameGenMonitorInput& in) {
        std::memset(l, 0, sizeof(l));
        const int n = formatFrameGenMonitor(in, l, kFrameGenMonitorMaxLines);
        std::string s;
        for (int i = 0; i < n; ++i) { s += l[i]; s += '\n'; }
        return s;
    };
    FrameGenMonitorInput off;
    check(formatFrameGenMonitor(off, l, kFrameGenMonitorMaxLines) == 0, "monitor: both keys off draws no block");

    FrameGenMonitorSynth synth;
    synth.valid = true;
    synth.built = 90;
    synth.gpuMs = 1.234f;
    synth.gpuN = 90;
    synth.finishMs = 0.21f;
    synth.finishN = 90;
    synth.gameBytes = 182ull * 1024 * 1024;
    FrameGenMonitorRuntime rt;
    rt.known = kFrameGenMonitorSlot;
    rt.ageMs = 11;
    rt.reason = static_cast<uint32_t>(FrameGenLockReason::None);
    rt.lastSecond = true;
    rt.sSlots = 45;
    rt.fromS = 44;
    rt.lateS = 1;
    rt.lateReal = 0;
    rt.slotDegMedian = 0.42f;
    rt.xrBytes = 63ull * 1024 * 1024;

    FrameGenMonitorInput shadow;
    shadow.shadowKey = true;
    shadow.synth = synth;
    shadow.runtime = rt;
    checkText(lines(shadow),
              "State: shadow (S is synthesized, nothing is shown)\n"
              "Synth 1.23 ms/eye, finish 0.21 ms/eye, slot pose miss 0.42 deg\n"
              "VRAM: game 182.0 MiB, XR 63.0 MiB\n",
              "monitor: shadow only");

    FrameGenMonitorInput lock;
    lock.lockKey = true;
    lock.synth = synth;
    lock.runtime = rt;
    checkText(lines(lock),
              "State: Lock engaged\n"
              "Last second: S shown 44/45, B replayed 1, late real 0\n"
              "Synth 1.23 ms/eye, finish 0.21 ms/eye, slot pose miss 0.42 deg\n"
              "VRAM: game 182.0 MiB, XR 63.0 MiB\n",
              "monitor: Lock engaged");

    FrameGenMonitorInput waiting = lock;
    waiting.runtime.reason = static_cast<uint32_t>(FrameGenLockReason::NoS);
    waiting.runtime.lastSecond = false;
    waiting.runtime.slotDegMedian = -1.0f;
    waiting.synth.finishN = 0;
    checkText(lines(waiting),
              "State: Lock waiting (no_s)\n"
              "Last second: no window closed yet\n"
              "Synth 1.23 ms/eye, finish - ms/eye\n"
              "VRAM: game 182.0 MiB, XR 63.0 MiB\n",
              "monitor: Lock waiting, named by the gate's reason; no window, no miss, no finish timed");
    waiting.runtime.reason = static_cast<uint32_t>(FrameGenLockReason::DeferredPacing);
    check(lines(waiting).rfind("State: Lock waiting (deferred_pacing)\n", 0) == 0, "monitor: the reason is the lock's own name (deferred_pacing)");
    waiting.runtime.reason = 200;
    check(lines(waiting).rfind("State: Lock waiting (unknown)\n", 0) == 0, "monitor: a reason past the table reads unknown");

    // A runtime from 2.3 or 2.2 hands a size 2 or 1 slot: its Lock state is unknown here, and so is the XR side.
    FrameGenMonitorInput old = lock;
    old.runtime = FrameGenMonitorRuntime{};
    old.runtime.known = kFrameGenMonitorOldSlot;
    checkText(lines(old),
              "State: Lock unknown (the runtime is older than this d3d11.dll)\n"
              "Synth 1.23 ms/eye, finish 0.21 ms/eye\n"
              "VRAM: game 182.0 MiB, XR unknown\n",
              "monitor: a size 2 runtime reads unknown");
    FrameGenMonitorInput none = lock;
    none.runtime = FrameGenMonitorRuntime{};
    none.synth = FrameGenMonitorSynth{};
    checkText(lines(none),
              "State: Lock unknown (no slot from the runtime yet)\n"
              "Synth: no S built in the last second\n"
              "VRAM: game 0.0 MiB, XR unknown\n",
              "monitor: no slot yet, no synth window");
    FrameGenMonitorInput stale = lock;
    stale.runtime.ageMs = 5000;
    check(lines(stale).rfind("State: Lock unknown (no slot from the runtime for 5 s)\n", 0) == 0, "monitor: a stale slot is not the state now");
    // Every line fits MenuLine::left with room to spare (the page's notes run to about 80 characters).
    FrameGenMonitorInput wide = lock;
    wide.runtime.sSlots = wide.runtime.fromS = wide.runtime.lateS = wide.runtime.lateReal = 4000000000u;
    wide.runtime.reason = static_cast<uint32_t>(FrameGenLockReason::FinishPending);
    wide.synth.gpuMs = wide.synth.finishMs = 99999.0f;
    wide.runtime.slotDegMedian = 180.0f;
    const int n = formatFrameGenMonitor(wide, l, kFrameGenMonitorMaxLines);
    bool fits = n == 4;
    for (int i = 0; i < n; ++i) fits = fits && std::strlen(l[i]) < 90;
    check(fits, "monitor: the widest lines fit the page");

    // Phase 2.4, auto: the gate's line under the state, its reason named; on draws none.
    FrameGenMonitorInput autoIn = lock;
    autoIn.autoMode = true;
    autoIn.gateState = FrameGenGateState::Waiting;
    autoIn.gateReason = FrameGenGateReason::FullRate;
    autoIn.runtime.reason = static_cast<uint32_t>(FrameGenLockReason::Gate);
    checkText(lines(autoIn),
              "State: Lock waiting (auto_gate)\n"
              "Gate: waiting (full_rate)\n"
              "Last second: S shown 44/45, B replayed 1, late real 0\n"
              "Synth 1.23 ms/eye, finish 0.21 ms/eye, slot pose miss 0.42 deg\n"
              "VRAM: game 182.0 MiB, XR 63.0 MiB\n",
              "monitor: auto, the gate waiting (full_rate) and Lock waiting on it (auto_gate)");
    autoIn.gateState = FrameGenGateState::Engaged;
    autoIn.gateReason = FrameGenGateReason::None;
    autoIn.runtime.reason = static_cast<uint32_t>(FrameGenLockReason::None);
    check(lines(autoIn).rfind("State: Lock engaged\nGate: engaged (Lock allowed)\n", 0) == 0, "monitor: auto, the gate engaged");
    autoIn.gateState = FrameGenGateState::Waiting;
    autoIn.gateReason = FrameGenGateReason::NoRuntime;
    autoIn.runtime = FrameGenMonitorRuntime{};
    check(lines(autoIn).rfind("State: Lock unknown (no slot from the runtime yet)\nGate: waiting (no_runtime: openvr_api.dll older than auto?)\n", 0) == 0,
          "monitor: auto with no slot names the runtime");
    FrameGenMonitorInput wideAuto = wide;
    wideAuto.autoMode = true;
    wideAuto.gateState = FrameGenGateState::Waiting;
    wideAuto.gateReason = FrameGenGateReason::OverGpu;
    const int na = formatFrameGenMonitor(wideAuto, l, kFrameGenMonitorMaxLines);
    bool fitsAuto = na == 5;
    for (int i = 0; i < na; ++i) fitsAuto = fitsAuto && std::strlen(l[i]) < 90;
    check(fitsAuto, "monitor: auto, five lines, the widest fit the page");
}

// ---- Phase 2.4: the performance gate (framegen_gate.h), table-driven ----
// A 90 Hz display (11.11 ms), Elite at 60 fps with a GPU frame of 12 ms and a CPU frame of 9 ms, the synthesizer 1.2 ms a pair, in a ship: every engage term holds.
FrameGenGateInputs gateBase() {
    FrameGenGateInputs in;
    in.mode = FrameGenMode::Auto;
    in.runtime = true;
    in.displayMs = 11.111;
    in.predictedMs = 11.111;
    in.realFps = 60.0;
    in.gpuN = 120; in.gpuMedMs = 12.0; in.gpuMaxMs = 14.0;
    in.cpuN = 120; in.cpuMedMs = 9.0; in.cpuMaxMs = 10.5;
    in.synthKnown = true; in.synthMs = 1.2;
    in.context = FrameGenGateContext::Vehicle;
    return in;
}

void caseGateTerms() {
    struct Row { const char* what; void (*edit)(FrameGenGateInputs&); bool engaged; FrameGenGateReason want; };
    const Row rows[] = {
        {"all terms hold", [](FrameGenGateInputs&) {}, false, FrameGenGateReason::None},
        {"no runtime slot of 2.4 (fail-open)", [](FrameGenGateInputs& i) { i.runtime = false; }, false, FrameGenGateReason::NoRuntime},
        {"display period unknown (fail-open)", [](FrameGenGateInputs& i) { i.displayMs = 0; }, false, FrameGenGateReason::NoDisplay},
        {"no GPU time (fail-open)", [](FrameGenGateInputs& i) { i.gpuN = 0; }, false, FrameGenGateReason::NoGpu},
        {"no CPU time (fail-open)", [](FrameGenGateInputs& i) { i.cpuN = 0; }, false, FrameGenGateReason::NoCpu},
        {"no synthesizer cost (fail-open)", [](FrameGenGateInputs& i) { i.synthKnown = false; }, false, FrameGenGateReason::NoSynth},
        {"on foot", [](FrameGenGateInputs& i) { i.context = FrameGenGateContext::OnFoot; }, false, FrameGenGateReason::OnFoot},
        {"context unknown", [](FrameGenGateInputs& i) { i.context = FrameGenGateContext::Unknown; }, false, FrameGenGateReason::Context},
        {"full rate: 85 fps of 90 is not under 0.9", [](FrameGenGateInputs& i) { i.realFps = 85.0; }, false, FrameGenGateReason::FullRate},
        {"80 fps of 90 is under 0.9", [](FrameGenGateInputs& i) { i.realFps = 80.0; }, false, FrameGenGateReason::None},
        {"full rate, but the runtime halved its period", [](FrameGenGateInputs& i) { i.realFps = 90.0; i.predictedMs = 22.222; }, false, FrameGenGateReason::None},
        {"GPU + synth over 0.85 x 2P (18.89 ms)", [](FrameGenGateInputs& i) { i.gpuMedMs = 17.8; }, false, FrameGenGateReason::OverGpu},
        {"GPU alone under, synth puts it over", [](FrameGenGateInputs& i) { i.gpuMedMs = 17.7; i.synthMs = 1.3; }, false, FrameGenGateReason::OverGpu},
        {"GPU + synth just under", [](FrameGenGateInputs& i) { i.gpuMedMs = 17.6; }, false, FrameGenGateReason::None},
        // 2.4, the synthesizer inside the GPU figure (the treat lies in the application segment and the work span): counted once, by synth_ms.
        {"synth on every frame: GPU 18.8 holds it, 17.6 + 1.2 is under", [](FrameGenGateInputs& i) { i.gpuMedMs = 18.8; i.synthShare = 1.0; }, false, FrameGenGateReason::None},
        {"synth on 60% of frames: the median holds it", [](FrameGenGateInputs& i) { i.gpuMedMs = 18.8; i.synthShare = 0.6; }, false, FrameGenGateReason::None},
        {"synth on 60% of frames, GPU 19.0: 17.8 + 1.2 is over", [](FrameGenGateInputs& i) { i.gpuMedMs = 19.0; i.synthShare = 0.6; }, false, FrameGenGateReason::OverGpu},
        {"synth on half the frames: the median is not taken to hold it", [](FrameGenGateInputs& i) { i.gpuMedMs = 18.8; i.synthShare = 0.5; }, false, FrameGenGateReason::OverGpu},
        {"auto closed, one sample a second (share 0.02): nothing taken out", [](FrameGenGateInputs& i) { i.gpuMedMs = 17.8; i.synthShare = 0.02; }, false, FrameGenGateReason::OverGpu},
        {"CPU over 0.85 x 2P", [](FrameGenGateInputs& i) { i.cpuMedMs = 19.0; }, false, FrameGenGateReason::OverCpu},
        {"engaged: the rate term is not the engaged test", [](FrameGenGateInputs& i) { i.realFps = 45.0; i.gpuMedMs = 30.0; }, true, FrameGenGateReason::None},
        {"engaged: a missing measurement still blocks", [](FrameGenGateInputs& i) { i.cpuN = 0; }, true, FrameGenGateReason::NoCpu},
        {"engaged: on foot still blocks", [](FrameGenGateInputs& i) { i.context = FrameGenGateContext::OnFoot; }, true, FrameGenGateReason::OnFoot},
    };
    for (const Row& r : rows) {
        FrameGenGateInputs in = gateBase();
        r.edit(in);
        const FrameGenGateTerms t = frameGenGateTerms(in, r.engaged);
        char what[160];
        std::snprintf(what, sizeof(what), "gate terms: %s -> %s (got %s)", r.what, frameGenGateReasonName(r.want), frameGenGateReasonName(t.blocker));
        check(t.blocker == r.want, what);
    }
    const FrameGenGateTerms t = frameGenGateTerms(gateBase(), false);
    check(std::fabs(t.budgetMs - 18.8887) < 0.001 && std::fabs(t.workMs - 13.2) < 1e-9 && std::fabs(t.marginMs - (t.budgetMs - 13.2)) < 1e-9,
          "gate terms: budget 0.85 x 2P, work the larger of GPU + synth and CPU, margin their difference");
    FrameGenGateInputs fit = gateBase();
    fit.gpuMedMs = 8.0; fit.cpuMedMs = 8.2;
    check(frameGenGateTerms(fit, true).fitsFull, "gate terms: GPU 8.0 and CPU 8.2 are under 0.75 of 11.11 ms");
    fit.cpuMedMs = 8.4;
    check(!frameGenGateTerms(fit, true).fitsFull, "gate terms: CPU 8.4 is not under 0.75 of 11.11 ms");
    {
        FrameGenGateInputs in = gateBase();
        in.synthShare = 1.0;
        const FrameGenGateTerms s = frameGenGateTerms(in, true);
        check(std::fabs(frameGenGateEliteGpuMs(in) - 10.8) < 1e-9 && std::fabs(s.workMs - 12.0) < 1e-9,
              "gate terms: synth on every frame: Elite's GPU is 12.0 - 1.2, the work 10.8 + 1.2 (not 12.0 + 1.2)");
        in.gpuMedMs = 9.0; in.cpuMedMs = 8.0;
        check(frameGenGateTerms(in, true).fitsFull, "gate terms: fits_full_rate judges Elite's GPU without the synth (9.0 - 1.2 = 7.8 under 8.33)");
        in.synthShare = 0.0;
        check(!frameGenGateTerms(in, true).fitsFull, "gate terms: ...and with the synth not in the median, 9.0 is not under 8.33");
        in.synthShare = 1.0; in.synthKnown = false;
        check(std::fabs(frameGenGateEliteGpuMs(in) - 9.0) < 1e-9, "gate terms: an unmeasured synth is not taken out");
        in.synthKnown = true; in.gpuMedMs = 1.0; in.synthMs = 5.0;
        check(frameGenGateEliteGpuMs(in) == 0.0, "gate terms: Elite's GPU never goes under 0");
    }
    FrameGenGateInputs slow = gateBase();
    slow.pairs = 90; slow.lateReal = 5;
    check(frameGenGateTerms(slow, true).tooSlow, "gate terms: 5 late of 90 pairs is over 5%");
    slow.lateReal = 4;
    check(!frameGenGateTerms(slow, true).tooSlow, "gate terms: 4 late of 90 pairs is not over 5%");
    slow.pairs = 9; slow.lateReal = 9;
    check(!frameGenGateTerms(slow, true).tooSlow, "gate terms: under 10 pairs is not judged");
}

// Steps the gate every 100 ms from t0 to t1 (exclusive) with `in`; returns the last step.
FrameGenGateStep gateRun(FrameGenGate& g, const FrameGenGateInputs& in, uint64_t t0, uint64_t t1, unsigned* edges = nullptr) {
    FrameGenGateStep s;
    for (uint64_t t = t0; t < t1; t += 100) {
        s = g.step(in, t);
        if (edges && s.edge) ++*edges;
    }
    return s;
}

void caseGateDwell() {
    const FrameGenGateInputs in = gateBase();
    // Engage after 2 s, not before; a single edge.
    {
        FrameGenGate g;
        unsigned edges = 0;
        FrameGenGateStep s = gateRun(g, in, 1000, 2900, &edges);
        check(s.state == FrameGenGateState::Waiting && s.reason == FrameGenGateReason::Dwell && edges == 0, "gate: waiting (dwell) before 2 s");
        s = g.step(in, 3000);
        check(s.state == FrameGenGateState::Engaged && s.reason == FrameGenGateReason::None && s.edge, "gate: engaged at 2 s, an edge");
        s = g.step(in, 3100);
        check(s.state == FrameGenGateState::Engaged && !s.edge, "gate: stays engaged, no second edge");
    }
    // off is never engaged; on is engaged (forced) at once, whatever the performance terms say; both start the dwell over.
    {
        FrameGenGate g;
        FrameGenGateInputs off = in;
        off.mode = FrameGenMode::Off;
        const FrameGenGateStep s = gateRun(g, off, 0, 10000);
        check(s.state == FrameGenGateState::Off && s.reason == FrameGenGateReason::KeyOff && !g.engaged(), "gate: off is never engaged");
        FrameGenGateInputs on = in;
        on.mode = FrameGenMode::On;
        on.runtime = false; on.gpuN = 0; on.context = FrameGenGateContext::OnFoot; on.realFps = 90.0;
        const FrameGenGateStep o = g.step(on, 10000);
        check(o.state == FrameGenGateState::Engaged && o.reason == FrameGenGateReason::Forced && !o.edge, "gate: on bypasses every term (forced), at once");
        FrameGenGateStep a = g.step(in, 10100);
        check(a.state == FrameGenGateState::Waiting && a.reason == FrameGenGateReason::Dwell, "gate: on -> auto starts the dwell over");
        a = gateRun(g, in, 10200, 12200);
        check(a.state == FrameGenGateState::Engaged, "gate: ... and engages 2 s later");
    }
    // Fail-open: a measurement missing engages nothing, and lets an engaged gate go at once, named.
    {
        FrameGenGate g;
        FrameGenGateInputs miss = in;
        miss.gpuN = 0;
        FrameGenGateStep s = gateRun(g, miss, 0, 10000);
        check(s.state == FrameGenGateState::Waiting && s.reason == FrameGenGateReason::NoGpu, "gate: fail-open, no GPU time never engages (no_gpu_time)");
        gateRun(g, in, 10000, 12100);
        check(g.engaged(), "gate: engages once the measurement is back for 2 s");
        s = g.step(miss, 12100);
        check(s.edge && s.state == FrameGenGateState::Waiting && s.reason == FrameGenGateReason::NoGpu, "gate: a measurement lost while engaged lets go at once, named");
    }
    // A cut: lets go at once; then 2 s with none and 5 s since the disengage before it engages again; a cut while waiting resets the hold.
    {
        FrameGenGate g;
        gateRun(g, in, 0, 2100);
        check(g.engaged(), "gate: engaged before the cut");
        FrameGenGateInputs cut = in;
        cut.cut = FrameGenGateCut::Screen;
        FrameGenGateStep s = g.step(cut, 3000);
        check(s.edge && s.state == FrameGenGateState::Waiting && s.reason == FrameGenGateReason::Cut && s.cut == FrameGenGateCut::Screen, "gate: a cut lets go at once (cut=screen)");
        s = gateRun(g, in, 3100, 5000);
        check(s.reason == FrameGenGateReason::Cut, "gate: within 2 s of the cut the reason stays cut");
        s = gateRun(g, in, 5000, 7100);
        check(s.state == FrameGenGateState::Waiting && s.reason == FrameGenGateReason::Rearm, "gate: held 2 s after the cut, but within 5 s of the disengage: rearm");
        s = g.step(in, 8000);
        check(s.state == FrameGenGateState::Engaged && s.edge, "gate: engages 5 s after the disengage (held 3 s)");
        // While waiting: a cut resets the hold.
        FrameGenGate w;
        gateRun(w, in, 0, 1900);
        FrameGenGateInputs cam = in;
        cam.cut = FrameGenGateCut::Camera;
        s = w.step(cam, 1900);
        check(s.reason == FrameGenGateReason::Cut && s.cut == FrameGenGateCut::Camera, "gate: a cut while waiting is named");
        s = gateRun(w, in, 2000, 3900);
        check(s.state == FrameGenGateState::Waiting, "gate: the hold starts over after a cut (not engaged 2 s after the start)");
        s = gateRun(w, in, 3900, 6000);
        check(s.state == FrameGenGateState::Engaged, "gate: engages 2 s after the cut cleared, never having let go (no rearm)");
    }
    // fits_full_rate: Elite's work under 0.75 of one period for 3 s lets go; a break in it starts the 3 s over.
    {
        FrameGenGate g;
        gateRun(g, in, 0, 2100);
        FrameGenGateInputs fit = in;
        fit.gpuMedMs = 7.0; fit.cpuMedMs = 6.0; fit.realFps = 45.0;
        FrameGenGateStep s = gateRun(g, fit, 2100, 4000);
        FrameGenGateInputs busy = fit;
        busy.gpuMedMs = 9.0;
        s = g.step(busy, 4000);
        check(s.state == FrameGenGateState::Engaged, "gate: work not under 0.75 P breaks the fits run");
        unsigned edges = 0;
        s = gateRun(g, fit, 4100, 7100, &edges);
        check(s.state == FrameGenGateState::Engaged && edges == 0, "gate: 2.9 s of fitting after the break: still engaged");
        s = g.step(fit, 7100);
        check(s.edge && s.reason == FrameGenGateReason::FitsFullRate, "gate: 3 s of fitting lets go (fits_full_rate)");
    }
    // too_slow: Lock's late_real over 5% of its pairs lets go at once.
    {
        FrameGenGate g;
        gateRun(g, in, 0, 2100);
        FrameGenGateInputs slow = in;
        slow.pairs = 88; slow.lateReal = 6;
        const FrameGenGateStep s = g.step(slow, 2200);
        check(s.edge && s.reason == FrameGenGateReason::TooSlow, "gate: late_real 6 of 88 pairs lets go (too_slow)");
    }
    // on_foot while engaged lets go at once; it is never admitted.
    {
        FrameGenGate g;
        gateRun(g, in, 0, 2100);
        FrameGenGateInputs foot = in;
        foot.context = FrameGenGateContext::OnFoot;
        FrameGenGateStep s = g.step(foot, 2200);
        check(s.edge && s.reason == FrameGenGateReason::OnFoot, "gate: on foot lets go at once (on_foot)");
        s = gateRun(g, foot, 2300, 20000);
        check(s.state == FrameGenGateState::Waiting && s.reason == FrameGenGateReason::OnFoot, "gate: on foot is never admitted");
    }
    // full_rate while waiting holds it off; the rate is not judged once engaged (Lock halves it).
    {
        FrameGenGate g;
        FrameGenGateInputs full = in;
        full.realFps = 89.0;
        FrameGenGateStep s = gateRun(g, full, 0, 10000);
        check(s.state == FrameGenGateState::Waiting && s.reason == FrameGenGateReason::FullRate, "gate: Elite at full rate never engages (full_rate)");
        FrameGenGateInputs over = in;
        over.cpuMedMs = 20.0;
        s = gateRun(g, over, 10000, 20000);
        check(s.reason == FrameGenGateReason::OverCpu, "gate: over the CPU budget never engages (over_budget_cpu)");
    }
}

void caseGateWordAndToggle() {
    check(frameGenModeParse("off") == FrameGenMode::Off && frameGenModeParse("auto") == FrameGenMode::Auto && frameGenModeParse("on") == FrameGenMode::On &&
              frameGenModeParse("yes") == FrameGenMode::Off && frameGenModeParse(nullptr) == FrameGenMode::Off,
          "mode: off, auto, on; anything else off");
    check(frameGenLockWord(FrameGenMode::Off, true) == 0u && frameGenLockWord(FrameGenMode::On, false) == 1u && frameGenLockWord(FrameGenMode::Auto, false) == 2u &&
              frameGenLockWord(FrameGenMode::Auto, true) == 3u,
          "word 17: off 0, on 1, auto closed 2, auto open 3");
    // A 2.3 runtime reads only 1 as on: both auto words read off there. The 2.4 runtime: armed 1..3, admits 1 and 3.
    check(!frameGenWordArmed(0) && frameGenWordArmed(1) && frameGenWordArmed(2) && frameGenWordArmed(3) && !frameGenWordArmed(4), "word 17: armed 1..3");
    check(!frameGenWordAdmits(0) && frameGenWordAdmits(1) && !frameGenWordAdmits(2) && frameGenWordAdmits(3), "word 17: admits on and auto open only");
    check(frameGenWordMode(2) == FrameGenMode::Auto && frameGenWordMode(3) == FrameGenMode::Auto && frameGenWordMode(1) == FrameGenMode::On && frameGenWordMode(7) == FrameGenMode::Off,
          "word 17: the mode it says");
    FrameGenMode last = FrameGenMode::Off;
    check(frameGenToggle(FrameGenMode::Off, last) == FrameGenMode::Auto, "toggle: off goes to auto by default");
    check(frameGenToggle(FrameGenMode::On, last) == FrameGenMode::Off && last == FrameGenMode::On, "toggle: on goes off, remembered");
    check(frameGenToggle(FrameGenMode::Off, last) == FrameGenMode::On, "toggle: off goes back to on");
    check(frameGenToggle(FrameGenMode::Auto, last) == FrameGenMode::Off && frameGenToggle(FrameGenMode::Off, last) == FrameGenMode::Auto, "toggle: auto remembered");
}

void caseGateLines() {
    FrameGenGate g;
    FrameGenGateInputs in = gateBase();
    in.pairs = 0;
    const FrameGenGateStep w = g.step(in, 0);
    checkText(formatFrameGenGateReport(w, in, FrameGenMode::Auto, "auto_gate"),
              "frame generation gate: state=waiting reason=dwell mode=auto real_fps=60.00/90.00 period_ms=11.11/11.11 gpu_ms=12.00/14.00 cpu_ms=9.00/10.50 synth_ms=1.20 "
              "budget_ms=18.89 margin_ms=5.69 late_real=0/0 context=vehicle cut=none lock=auto_gate",
              "gate line: waiting, every field");
    FrameGenGateInputs none;
    none.mode = FrameGenMode::Auto;
    const FrameGenGateStep n = FrameGenGate{}.step(none, 0);
    checkText(formatFrameGenGateReport(n, none, FrameGenMode::Auto, nullptr),
              "frame generation gate: state=waiting reason=no_runtime mode=auto real_fps=-/- period_ms=-/- gpu_ms=-/- cpu_ms=-/- synth_ms=- budget_ms=- margin_ms=- "
              "late_real=0/0 context=unknown cut=none lock=unknown",
              "gate line: nothing measured reads '-' and names no_runtime");
    const FrameGenGateStep e = gateRun(g, in, 100, 2100);
    check(e.edge, "gate line: the run ends on the engage edge");
    checkText(formatFrameGenGateEdge(e), "frame generation gate: engage reason=held_2s margin_ms=5.69", "gate line: the engage edge");
    FrameGenGateInputs cut = in;
    cut.cut = FrameGenGateCut::Tunnel;
    checkText(formatFrameGenGateEdge(g.step(cut, 2200)), "frame generation gate: disengage reason=cut cut=tunnel", "gate line: the disengage edge names the cut");
    FrameGenGateInputs on = in;
    on.mode = FrameGenMode::On;
    const FrameGenGateStep o = FrameGenGate{}.step(on, 0);
    check(formatFrameGenGateReport(o, on, FrameGenMode::On, "none").rfind("frame generation gate: state=engaged reason=forced mode=on ", 0) == 0, "gate line: on reads engaged, forced");
    check(std::strlen(formatFrameGenGateReport(w, in, FrameGenMode::Auto, "finish_pending").c_str()) < 480, "gate line: fits its buffer");
}

}  // namespace

int main(int argc, char** argv) {
    bool run = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--self-test") || !std::strcmp(argv[i], "--dry-run")) run = true;
        else {
            std::fprintf(stderr, "usage: framegen_synth_report_test --self-test | --dry-run\n");
            return 2;
        }
    }
    if (!run) {
        std::fprintf(stderr, "usage: framegen_synth_report_test --self-test | --dry-run\n");
        return 2;
    }
    caseLines();
    caseSlot();
    caseMerge();
    caseDecode();
    caseResidual();
    caseRotation();
    caseHeadStep();
    caseCb();
    caseTarget();
    caseMonitor();
    caseGateTerms();
    caseGateDwell();
    caseGateWordAndToggle();
    caseGateLines();
    if (g_fail) {
        std::printf("framegen_synth_report_test: %d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("framegen_synth_report_test: all checks passed\n");
    return 0;
}

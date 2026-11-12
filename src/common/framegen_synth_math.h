#pragma once
// The frame-generation synthesizer's geometry (fix.frame_generation_shadow, TEMPORARY; docs/design-frame-generation-2026-10-26.md, Phase 2.1, "The synthesis pass" and
// "Motion model"). Pure: the constant block the shaders read, the head's motion from B to S, the stats words, and a CPU reference of one B pixel's landing in S that the
// WARP rig (tools\framegen_synth_test) holds the shaders to. Conventions are framegen_shadow_math.h's.
//
// Motion of a B pixel p (render pixels, centres at integers) to S, f the fraction of B's frame step that S is ahead (Elite's step A to B is two display periods in
// Lock: f = 0.5 for the S a later phase shows; f = 1 lands on the next real frame's time, which is what is scored):
//   the head B to S   Hd_B (the pass's head delta) maps B's eye space to A's, x_A = Hd x_B + th, so the head's own forward step A to B is Hd^T (translation -Hd^T th).
//                     Carried on at constant velocity: the rotation is the f-th power of Hd^T (axis-angle scaled by f), the translation f * (-Hd^T th). This is the fallback
//                     until the runtime's slot pose (Phase 2.2) replaces it.
//   cockpit           (the class by B's dilated depth and the pass's split): the rigid head step with B's depth, carried and projected with B's tangents both sides.
//   far and world     (ex2dh, the flight 5 winner): with the DLSS vector mvB at the pixel (B to A, jitter cancelled): pHeadPrev is the pixel's direction turned by the head's
//                     B-to-A rotation Hd and projected with B's tangents (A's head turn removed), m = (p + mvB) - pHeadPrev is the ship's screen motion over one Elite step,
//                     pShip = p - f m (B's side of the step reversed), and the head's step to S turns pShip's direction and projects it.
#include "framegen_shadow_math.h"
#include "framegen_synth_report.h"

#include <cmath>
#include <cstdint>

namespace edvr {

// The synthesizer's constant buffer (b0 of every entry), laid out for the HLSL cbuffer P in src\d3d11\framegen_synth_shader.h: 10 rows of 16 bytes.
struct FrameGenSynthCb {
    int32_t sz[4];     // render width, render height, output width, output height
    int32_t qz[4];     // quarter grid width, height, 0, 0
    float tanB[4];     // this frame's tangents (left, right, bottom, top), jitter excluded
    float dk[4];       // the depth encoding's A and B (depth = A + B / z), the ship's split in metres, f
    float rs[3][4];    // the head's step B to S: rotation rows (xyz), w the translation component
    float hp[3][4];    // the head's rotation B to A (the pass's Hd), xyz of each row
    float rr[3][4];    // the score's residual head rotation (frameGenSynthResidual), rows xyz: C's eye space to the eye space the kept f = 1 S was built for (identity off a score)
    float tanK[4];     // the tangents the kept S was built with (B's); this frame's on a frame that scores nothing
};
static_assert(sizeof(FrameGenSynthCb) == 224, "the synthesizer's cbuffer is 14 16-byte rows");

// The stats words: the score against C (frameGenDecodeBlock's layout: three classes, holes, cells = 299 words), then the render pixels the background filled, the holes left, and
// the reads the guard moved, the read offsets clamped, then the same score block again with S turned by the residual head rotation (its holes word counts the cells whose turned
// position fell outside S).
constexpr uint32_t kFrameGenSynthStatsFilled = kFrameGenShadowStatsBlockUints;
constexpr uint32_t kFrameGenSynthStatsLeft = kFrameGenSynthStatsFilled + 1;
constexpr uint32_t kFrameGenSynthStatsGuard = kFrameGenSynthStatsFilled + 2;
constexpr uint32_t kFrameGenSynthStatsClamp = kFrameGenSynthStatsFilled + 3;
constexpr uint32_t kFrameGenSynthStatsTurned = kFrameGenSynthStatsFilled + 4;   // 303
constexpr uint32_t kFrameGenSynthStatsHoles = kFrameGenSynthStatsTurned + kFrameGenShadowStatsBlockUints;   // 602: the holes listed for the fill pass (GPU-side only)
constexpr uint32_t kFrameGenSynthStatsFillReproj = kFrameGenSynthStatsHoles + 1;    // 603: the filled holes that read B where the background's motion carried them back
constexpr uint32_t kFrameGenSynthStatsFillStretch = kFrameGenSynthStatsHoles + 2;   // 604: the filled holes stretched along that motion past the occluder (the rest: the background pull)
constexpr uint32_t kFrameGenSynthStatsOrphan = kFrameGenSynthStatsHoles + 3;   // 605: the S pixels with a key and no read offer (fgSynSrc's landing disagreeing with fgSynSplat's), made holes
constexpr uint32_t kFrameGenSynthStatsUints = kFrameGenSynthStatsHoles + 4;   // 606

// The hole list the resolve pass writes and the fill pass reads (one render pixel index per hole): an eighth of the render pixels; holes past it stay unfilled (counted as left).
// The shader computes the same (fgSynHoleCapacity).
inline uint32_t frameGenSynthHoleCapacity(uint32_t rw, uint32_t rh) { return (rw * rh) / 8u; }

// The read offsets (the source B position minus the S pixel centre, render pixels) are stored as two half floats packed in a uint, rounded toward zero (framegen_synth_shader.h
// synHalfBits): exact to 1/8 px up to 256 px, to 1/4 px up to 512 px, to 1 px up to 2048 px. An offset past kFrameGenSynthMaxOffset on either axis is clamped to it (a wrong read,
// counted in the clamp stat); kFrameGenSynthNoRead on x marks a pixel with nothing to read.
constexpr float kFrameGenSynthMaxOffset = 2000.0f;
constexpr float kFrameGenSynthNoRead = -4096.0f;

// The rotation R raised to the power f (axis-angle scaled by f, through the quaternion, so a turn of any size is the short way round); R row-major.
inline void frameGenSynthRotationPow(const float R[9], float f, float out[9]) {
    double w, x, y, z;
    const double tr = static_cast<double>(R[0]) + R[4] + R[8];
    if (tr > 0.0) {
        const double s = std::sqrt(tr + 1.0) * 2.0;
        w = 0.25 * s; x = (R[7] - R[5]) / s; y = (R[2] - R[6]) / s; z = (R[3] - R[1]) / s;
    } else if (R[0] > R[4] && R[0] > R[8]) {
        const double s = std::sqrt(1.0 + R[0] - R[4] - R[8]) * 2.0;
        w = (R[7] - R[5]) / s; x = 0.25 * s; y = (R[1] + R[3]) / s; z = (R[2] + R[6]) / s;
    } else if (R[4] > R[8]) {
        const double s = std::sqrt(1.0 + R[4] - R[0] - R[8]) * 2.0;
        w = (R[2] - R[6]) / s; x = (R[1] + R[3]) / s; y = 0.25 * s; z = (R[5] + R[7]) / s;
    } else {
        const double s = std::sqrt(1.0 + R[8] - R[0] - R[4]) * 2.0;
        w = (R[3] - R[1]) / s; x = (R[2] + R[6]) / s; y = (R[5] + R[7]) / s; z = 0.25 * s;
    }
    if (w < 0.0) { w = -w; x = -x; y = -y; z = -z; }
    const double v = std::sqrt(x * x + y * y + z * z);
    if (v < 1e-12) {
        for (int i = 0; i < 9; ++i) out[i] = (i % 4 == 0) ? 1.0f : 0.0f;
        return;
    }
    const double half = std::atan2(v, w) * static_cast<double>(f);
    const double c = std::cos(half), sv = std::sin(half) / v;
    w = c; x *= sv; y *= sv; z *= sv;
    out[0] = static_cast<float>(1.0 - 2.0 * (y * y + z * z)); out[1] = static_cast<float>(2.0 * (x * y - z * w)); out[2] = static_cast<float>(2.0 * (x * z + y * w));
    out[3] = static_cast<float>(2.0 * (x * y + z * w)); out[4] = static_cast<float>(1.0 - 2.0 * (x * x + z * z)); out[5] = static_cast<float>(2.0 * (y * z - x * w));
    out[6] = static_cast<float>(2.0 * (x * z - y * w)); out[7] = static_cast<float>(2.0 * (y * z + x * w)); out[8] = static_cast<float>(1.0 - 2.0 * (x * x + y * y));
}

// The head's step from B to S at fraction f: x_S = rot x_B + t. `head` is B's head delta (backward, B to A: x_A = Hd x_B + th).
inline void frameGenSynthHeadStep(const FrameGenDeltas& head, float f, float rot[9], float t[3]) {
    float fwd[9], tf[3];
    temporalTranspose3(head.Hd, fwd);                  // the forward step A to B
    temporalApply3(fwd, head.th, tf);                  // its translation is -fwd * th
    frameGenSynthRotationPow(fwd, f, rot);
    for (int i = 0; i < 3; ++i) t[i] = -f * tf[i];
}

// Phase 2.2: the head's step from B to S from the runtime's pose for S's slot (EdvrNativeFrameGenSlot::head) instead of the extrapolation. headS and headB are rigid row-major 3x4
// head-to-space poses (the temporal frame's convention), eyeToHead the eye's (the same one the pass composes B's eye with). The eye's world transform is head * eye for each; the
// step is the temporal pass's eyeDelta with S as the earlier frame: x_S = R_S^T R_B x_B + R_S^T (t_B - t_S).
inline void frameGenSlotEyeWorld(const float head[12], const float eye[12], float r[9], float t[3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i * 3 + j] = head[i * 4 + 0] * eye[0 * 4 + j] + head[i * 4 + 1] * eye[1 * 4 + j] + head[i * 4 + 2] * eye[2 * 4 + j];
    for (int i = 0; i < 3; ++i) t[i] = head[i * 4 + 3] + head[i * 4 + 0] * eye[3] + head[i * 4 + 1] * eye[7] + head[i * 4 + 2] * eye[11];
}
inline void frameGenSlotStep(const float headS[12], const float headB[12], const float eyeToHead[12], float rot[9], float t[3]) {
    float rs[9], ts[3], rb[9], tb[3], rst[9];
    frameGenSlotEyeWorld(headS, eyeToHead, rs, ts);
    frameGenSlotEyeWorld(headB, eyeToHead, rb, tb);
    temporalTranspose3(rs, rst);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) rot[r * 3 + c] = rst[r * 3 + 0] * rb[0 * 3 + c] + rst[r * 3 + 1] * rb[1 * 3 + c] + rst[r * 3 + 2] * rb[2 * 3 + c];
    const float d[3] = {tb[0] - ts[0], tb[1] - ts[1], tb[2] - ts[2]};
    temporalApply3(rst, d, t);
}

// The residual head rotation of a scored frame, rotation only: the f = 1 S kept from B was built for the head step `kept` (B's eye space to S's, the cb's rs rotation); C's true head
// rotation is its backward delta `hdC` (C's eye space to B's: x_B = Hd_C x_C + th). A direction in C's eye space is d_B = Hd_C d_C in B's, d_S = kept d_B in the S eye: out =
// kept * Hd_C, the identity when the head did exactly what S was built for. The compositor turns a submitted frame by this much on display.
inline void frameGenSynthResidual(const float kept[9], const float hdC[9], float out[9]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[r * 3 + c] = kept[r * 3 + 0] * hdC[0 * 3 + c] + kept[r * 3 + 1] * hdC[1 * 3 + c] + kept[r * 3 + 2] * hdC[2 * 3 + c];
}

inline void frameGenSynthFillCb(FrameGenSynthCb& cb, uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH, const float tanB[4], float depthA, float depthB,
                                float splitMetres, float f, const FrameGenDeltas& head, const float* residual = nullptr, const float* tanKept = nullptr,
                                const float* slotRot = nullptr, const float* slotT = nullptr) {
    cb = FrameGenSynthCb{};
    cb.sz[0] = static_cast<int32_t>(renderW);
    cb.sz[1] = static_cast<int32_t>(renderH);
    cb.sz[2] = static_cast<int32_t>(outW);
    cb.sz[3] = static_cast<int32_t>(outH);
    cb.qz[0] = static_cast<int32_t>((renderW + 3u) / 4u);
    cb.qz[1] = static_cast<int32_t>((renderH + 3u) / 4u);
    for (int i = 0; i < 4; ++i) cb.tanB[i] = tanB[i];
    cb.dk[0] = depthA;
    cb.dk[1] = depthB;
    cb.dk[2] = splitMetres;
    cb.dk[3] = f;
    float rot[9], t[3];
    if (slotRot && slotT) {   // Phase 2.2: the runtime's pose for S's slot (frameGenSlotStep); f still carries the ship's share of the step
        for (int i = 0; i < 9; ++i) rot[i] = slotRot[i];
        for (int i = 0; i < 3; ++i) t[i] = slotT[i];
    } else {
        frameGenSynthHeadStep(head, f, rot, t);
    }
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) {
            cb.rs[r][k] = rot[r * 3 + k];
            cb.hp[r][k] = head.Hd[r * 3 + k];
        }
        cb.rs[r][3] = t[r];
        for (int k = 0; k < 3; ++k) cb.rr[r][k] = residual ? residual[r * 3 + k] : (r == k ? 1.0f : 0.0f);
    }
    for (int i = 0; i < 4; ++i) cb.tanK[i] = tanKept ? tanKept[i] : tanB[i];
}

// Decodes one eye's stats words. `scored` says the score block holds a pass (a frame that scored nothing leaves it zero, and the decode leaves cells 0).
inline void frameGenSynthDecodeStats(const uint32_t* s, FrameGenSynthEyeResult& out) {
    out = FrameGenSynthEyeResult{};
    uint64_t holes = 0;
    frameGenDecodeBlock(s, out.cls, holes, out.cells);
    out.filled = s[kFrameGenSynthStatsFilled];
    out.left = s[kFrameGenSynthStatsLeft];
    out.guard = s[kFrameGenSynthStatsGuard];
    out.clamped = s[kFrameGenSynthStatsClamp];
    out.fillReproj = s[kFrameGenSynthStatsFillReproj];
    out.fillStretch = s[kFrameGenSynthStatsFillStretch];
    out.orphan = s[kFrameGenSynthStatsOrphan];
    frameGenDecodeBlock(s + kFrameGenSynthStatsTurned, out.tcls, out.tout, out.tcells);
}

// The CPU reference of the shader's target for a B pixel (px, py) of depth zMetres (farPlane: no depth) and vector (mvx, mvy): where it lands in S, in render pixels, and its class
// (0 far plane, 1 cockpit, 2 world). False when the landing is behind the eye. Evaluates the same closed form as fgSynTarget in framegen_synth_shader.h.
inline bool frameGenSynthTarget(const FrameGenSynthCb& cb, float px, float py, float zMetres, bool farPlane, float mvx, float mvy, float pp[2], int* clsOut = nullptr) {
    const uint32_t w = static_cast<uint32_t>(cb.sz[0]), h = static_cast<uint32_t>(cb.sz[1]);
    const int cls = farPlane ? 0 : (zMetres > cb.dk[2] ? 2 : 1);
    if (clsOut) *clsOut = cls;
    float d[3];
    temporalPixelToDir(px, py, cb.tanB, w, h, d);
    float rot[9], hd[9];
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) {
            rot[r * 3 + k] = cb.rs[r][k];
            hd[r * 3 + k] = cb.hp[r][k];
        }
    if (cls == 1) {
        float dp[3];
        temporalApply3(rot, d, dp);
        for (int i = 0; i < 3; ++i) dp[i] = dp[i] * zMetres + cb.rs[i][3];
        return temporalDirToPixel(dp, cb.tanB, w, h, &pp[0], &pp[1]);
    }
    float dh[3], head[2];
    temporalApply3(hd, d, dh);
    if (!temporalDirToPixel(dh, cb.tanB, w, h, &head[0], &head[1])) return false;
    const float mx = (px + mvx) - head[0], my = (py + mvy) - head[1];
    float ds[3], dr[3];
    temporalPixelToDir(px - cb.dk[3] * mx, py - cb.dk[3] * my, cb.tanB, w, h, ds);
    temporalApply3(rot, ds, dr);
    return temporalDirToPixel(dr, cb.tanB, w, h, &pp[0], &pp[1]);
}

// ---- Phase 2.3b: S finished like B (fgSynFinish) ----
// B's UI layer (the HUD, the holo panels: drawn by the ship, fixed to it) moves in the eye from B to S by the head's step alone, x_S = rot x_B + t (B's eye space to S's, the step the
// f = 0.5 S was built with). The layer has no depth of its own, so it is taken as a plane at kFrameGenFinishPanelMetres in front of S's eye: an S output pixel (centre at
// integers, w x h, tangents tan: left, right, bottom, top) shows the point of that plane a B pixel showed, d_B ~ rot^T (d_S - t / D). The rotation is exact at any distance; the
// translation's part is exact at D only (a panel at d instead lands off by |t| (1/d - 1/D) radians).
constexpr float kFrameGenFinishPanelMetres = 1.5f;
// H (row-major 3x3): (x_B, y_B, 1) ~ H (x_S, y_S, 1). Built in double; entries within 1e-9 of 0 or 1 are snapped, so the identity step gives the identity exactly and S's composite
// reads the very texels B's did.
inline void frameGenSynthFinishHomography(const float rot[9], const float t[3], const float tan[4], uint32_t w, uint32_t h, float panelMetres, float H[9]) {
    const double L = tan[0], R = tan[1], Bt = tan[2], T = tan[3], W = static_cast<double>(w), Hh = static_cast<double>(h);
    const double sx = (R - L) / W, sy = (T - Bt) / Hh, u = W / (R - L), v = Hh / (T - Bt);
    const double kinv[9] = {sx, 0.0, L + 0.5 * sx, 0.0, -sy, T - 0.5 * sy, 0.0, 0.0, -1.0};   // S pixel -> direction (z = -1)
    const double k[9] = {u, 0.0, u * L + 0.5, 0.0, -v, 0.5 - v * T, 0.0, 0.0, -1.0};          // direction -> B pixel, homogeneous
    double m[9];                                                                                // rot^T (I + (t / D) e_z^T)
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            double s = 0.0;
            for (int i = 0; i < 3; ++i) s += static_cast<double>(rot[i * 3 + r]) * ((i == c ? 1.0 : 0.0) + (c == 2 ? static_cast<double>(t[i]) / panelMetres : 0.0));
            m[r * 3 + c] = s;
        }
    double km[9], out[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            double a = 0.0;
            for (int i = 0; i < 3; ++i) a += k[r * 3 + i] * m[i * 3 + c];
            km[r * 3 + c] = a;
        }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            double a = 0.0;
            for (int i = 0; i < 3; ++i) a += km[r * 3 + i] * kinv[i * 3 + c];
            out[r * 3 + c] = a;
        }
    for (int i = 0; i < 9; ++i) {
        double x = out[i];
        if (std::fabs(x) < 1e-9) x = 0.0;
        else if (std::fabs(x - 1.0) < 1e-9) x = 1.0;
        H[i] = static_cast<float>(x);
    }
}
// Where S's output pixel (sx, sy) reads B's (the layer's footprint starts there, as B's composite starts at its own pixel). False when it is not in front of B's eye.
inline bool frameGenSynthFinishLanding(const float H[9], float sx, float sy, float out[2]) {
    const float x = H[0] * sx + H[1] * sy + H[2], y = H[3] * sx + H[4] * sy + H[5], z = H[6] * sx + H[7] * sy + H[8];
    if (!(z > 1e-6f)) return false;
    out[0] = x / z;
    out[1] = y / z;
    return true;
}
// fgSynFinish's block, in P's rows (framegen_synth_shader.h says which field carries what): the output (the region's) size and the region's origin in the frame, whether a multiply
// drew this frame, the layer's size and its rectangle for the region (ui_layer's composite parameters as B's door used them), and H's rows.
inline void frameGenSynthFinishFillCb(FrameGenSynthCb& cb, uint32_t outW, uint32_t outH, uint32_t x0, uint32_t y0, const float H[9], const float layerUv[4], uint32_t layerW,
                                      uint32_t layerH, bool useMult) {
    cb = FrameGenSynthCb{};
    cb.sz[0] = static_cast<int32_t>(outW);
    cb.sz[1] = static_cast<int32_t>(outH);
    cb.sz[2] = static_cast<int32_t>(x0);
    cb.sz[3] = static_cast<int32_t>(y0);
    cb.qz[0] = useMult ? 1 : 0;
    cb.dk[0] = static_cast<float>(layerW);
    cb.dk[1] = static_cast<float>(layerH);
    for (int i = 0; i < 4; ++i) cb.tanK[i] = layerUv[i];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) cb.rs[r][c] = H[r * 3 + c];
}

}  // namespace edvr

#pragma once
// Frame generation, Phase 2.2 (fix.frame_generation_shadow, TEMPORARY; docs/design-frame-generation-2026-10-26.md): the synthetic slot's time and S's transfer, the runtime's half.
// Pure: no OpenXR, no D3D, no clock. The runtime's frame loop (src/openxr/native_runtime_host.h) computes T_S here, counts what crossed, and writes the one-second line from the
// format string below, which tools\edvr_log.py --health reads from this file.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace edvr {

// S is half an Elite step after B: T_S = T + (T - T_A) / 2, T the pair's predicted display time and T_A the previous real pair's (the synthesizer's f = 0.5; in Lock 2:1 it is
// T + P). Valid only from 1 ms to 50 ms past T: outside that, or with no previous pair, there is no slot pose (the synthesizer then keeps its own extrapolation and counts it).
constexpr int64_t kFrameGenSlotMinLeadNs = 1000000;
constexpr int64_t kFrameGenSlotMaxLeadNs = 50000000;
inline bool frameGenSlotTime(int64_t displayTime, int64_t previousDisplayTime, bool havePrevious, int64_t& slotTime) {
    slotTime = 0;
    if (!havePrevious || displayTime <= 0 || previousDisplayTime <= 0 || displayTime <= previousDisplayTime) return false;
    const int64_t lead = (displayTime - previousDisplayTime) / 2;
    if (lead < kFrameGenSlotMinLeadNs || lead > kFrameGenSlotMaxLeadNs) return false;
    slotTime = displayTime + lead;
    return true;
}

// The bytes a texel of S's formats takes (the formats the upscaler's output comes in, and the common 8-bit ones); 0 for one not known (its bytes are then not counted).
inline uint32_t frameGenSlotFormatBytes(uint32_t dxgiFormat) {
    switch (dxgiFormat) {
        case 1: case 2: case 3: case 4: return 16;                   // R32G32B32A32
        case 9: case 10: case 11: case 12: case 13: case 14: return 8;   // R16G16B16A16
        case 23: case 24: case 25: case 26:                   // R10G10B10A2, R11G11B10_FLOAT
        case 27: case 28: case 29: case 30: case 31: case 32: // R8G8B8A8
        case 87: case 88: case 90: case 91: case 92: case 93: // B8G8R8A8 / B8G8R8X8
            return 4;
        default: return 0;
    }
}

// Phase 2.3b: whether an eye's S was finished as B is (EdvrNativeFrameGenS::finish, common/native_temporal.h): B's door (src\d3d11\native_sharpen.cpp) runs B's sharpener on S
// and composites B's UI layer over it moved by the head's step alone. 0 is what a d3d11.dll from before 2.3b answers (never finished); 1 finished; anything else names why not, and
// that S is not handed over. The runtime shows only a finished S (otherwise Lock lets go with reason s_unfinished, and says the why once per change).
constexpr uint32_t kFrameGenFinishOld = 0;        // a d3d11.dll from before 2.3b: its S is not finished
constexpr uint32_t kFrameGenFinishDone = 1;       // finished like B
constexpr uint32_t kFrameGenFinishNoDoor = 2;     // B's door did not run for the eye after S was built (no native sharpen table, or the door refused the eye)
constexpr uint32_t kFrameGenFinishLayerOnly = 3;  // the eye is the UI layer alone (an on-foot map or menu screen, the VR world route): there is no scene to synthesize
constexpr uint32_t kFrameGenFinishScope = 4;      // the scope lens rewrites B's picture (a press of the sights): S has no lens
constexpr uint32_t kFrameGenFinishRipple = 5;     // the shield ripple rewrote B's picture this frame: S has no ripple
constexpr uint32_t kFrameGenFinishCropped = 6;    // B's door works on a region of the frame, not the whole of it (S is the whole)
constexpr uint32_t kFrameGenFinishSharpen = 7;    // B is sharpened and S could not be (or the reverse)
constexpr uint32_t kFrameGenFinishFault = 8;      // the finish itself failed (a texture, a view, the parameter block, the producer texture's key)
constexpr uint32_t kFrameGenFinishMenu = 9;       // EDVR's menu shows and could not be drawn on S as on B (no menu pose, another thread or device): S has no menu
inline const char* frameGenFinishName(uint32_t why) {
    switch (why) {
        case kFrameGenFinishOld: return "old_d3d11";
        case kFrameGenFinishDone: return "finished";
        case kFrameGenFinishNoDoor: return "no_door";
        case kFrameGenFinishLayerOnly: return "layer_only";
        case kFrameGenFinishScope: return "scope";
        case kFrameGenFinishRipple: return "ripple";
        case kFrameGenFinishCropped: return "cropped";
        case kFrameGenFinishSharpen: return "sharpen";
        case kFrameGenFinishFault: return "fault";
        case kFrameGenFinishMenu: return "menu";
        default: return "unknown";
    }
}

// The transfer path the window saw: uav (the resample pass writes S into the producer texture), copy (a private S copied into it), none (nothing produced), mixed (both).
inline const char* frameGenSlotPathName(bool uav, bool copy) { return uav && copy ? "mixed" : uav ? "uav" : copy ? "copy" : "none"; }

// The line, once a second while the key is on and the d3d11 half has the channel. produced/consumed: S handed over (key 1) and copied to the runtime's private texture; s_busy: S
// not handed over because the previous one was still unconsumed (the d3d11 half's AcquireSync(0) timed out); s_late: consumed after T_S. ready_ms: when the consume took S
// (the runtime's clock) less T_S, median/max (negative is before the slot); consume_ms: the consume's own CPU time, median/max. pose: the pairs handed a located slot pose /
// handed none (no previous pair, a lead outside 1-50 ms, or the locate failed). path: uav / copy / none / mixed. vram: the bytes the set holds on the game device (the producer
// textures, the private S on the copy path) and on the XR device (the private copies), both eyes, MiB.
constexpr const char* kFrameGenSlotReportFmt =
    "frame generation (synth) transfer: produced=%u consumed=%u s_busy=%u s_late=%u ready_ms=%s/%s consume_ms=%s/%s pose=%u/%u path=%s vram_game_mib=%.1f vram_xr_mib=%.1f";
// Said once when the key is on and the d3d11 half has no slot channel (an older d3d11.dll): nothing is handed and nothing crosses.
constexpr const char* kFrameGenSlotNoChannelFmt =
    "frame generation (synth) transfer: the d3d11 half has no slot channel (edvrFrameGenNoteSlot missing: an older d3d11.dll); no slot pose is handed and no S crosses";
constexpr const char* kFrameGenSlotMsFmt = "%.2f";
constexpr const char* kFrameGenSlotMsNoneFmt = "-";

struct FrameGenSlotWindow {
    uint32_t produced = 0, consumed = 0, busy = 0, late = 0;
    uint32_t poseRuntime = 0, poseNone = 0;
    bool pathUav = false, pathCopy = false;
    std::vector<float> readyMs, consumeMs;
    uint64_t gameBytes = 0, xrBytes = 0;   // as they stand at the line, both eyes
    bool any() const { return produced || consumed || busy || poseRuntime || poseNone; }
    void reset() {
        const uint64_t g = gameBytes, x = xrBytes;
        *this = FrameGenSlotWindow{};
        gameBytes = g;
        xrBytes = x;
    }
};

inline std::string frameGenSlotMs(const std::vector<float>& v, bool maximum) {
    char b[24];
    if (v.empty()) {
        std::snprintf(b, sizeof(b), "%s", kFrameGenSlotMsNoneFmt);
        return b;
    }
    std::vector<float> s = v;
    std::sort(s.begin(), s.end());
    std::snprintf(b, sizeof(b), kFrameGenSlotMsFmt, static_cast<double>(maximum ? s.back() : s[s.size() / 2]));
    return b;
}

inline std::string formatFrameGenSlotReport(const FrameGenSlotWindow& w) {
    char buf[512];
    const std::string r0 = frameGenSlotMs(w.readyMs, false), r1 = frameGenSlotMs(w.readyMs, true), c0 = frameGenSlotMs(w.consumeMs, false), c1 = frameGenSlotMs(w.consumeMs, true);
    std::snprintf(buf, sizeof(buf), kFrameGenSlotReportFmt, static_cast<unsigned>(w.produced), static_cast<unsigned>(w.consumed), static_cast<unsigned>(w.busy),
                  static_cast<unsigned>(w.late), r0.c_str(), r1.c_str(), c0.c_str(), c1.c_str(), static_cast<unsigned>(w.poseRuntime), static_cast<unsigned>(w.poseNone),
                  frameGenSlotPathName(w.pathUav, w.pathCopy), static_cast<double>(w.gameBytes) / 1048576.0, static_cast<double>(w.xrBytes) / 1048576.0);
    return buf;
}

}  // namespace edvr

#pragma once
// The frame-generation shadow scorer (fix.frame_generation_shadow = off | on, TEMPORARY, hidden, VR only; docs/design-frame-generation-2026-10-26.md,
// Phase 1). It measures how well the previous real frame, warped by the camera and head motion, predicts the next one; nothing in the headset changes.
// The pure halves are src\common\framegen_shadow_report.h (statistics and the log lines) and src\common\framegen_shadow_math.h (the prediction, the
// transforms and the constant block); the shaders are framegen_shadow_shader.h.
//
// temporal_pass.cpp calls frameGenShadowEyeFrame once per treated eye-frame beside the sky ring, with the numbers the pass already computed for that frame.
// Per eye the module keeps the previous frame's capture (a quarter-size luma and depth, ping-ponged) and deltas; a frame with both and no skip reason is
// scored on the GPU and the 598 stats words read back without waiting, a few frames later. Key off: nothing is created and nothing is dispatched.
// frameGenShadowFrameBoundary (once a frame, from temporalPassFrameBoundary) prints the one-second report.
#include <d3d11.h>

#include <cstdint>

#include "../common/framegen_shadow_math.h"

namespace edvr {

// The key. Live: a reload that changes it takes effect at once; turning it off re-arms the line and the next boundary releases the resources.
void frameGenShadowConfigure(bool on);
bool frameGenShadowOn();

// What the temporal pass knew about one eye-frame. Pointers are the pass's own arrays and are not kept.
struct FrameGenShadowFrame {
    uint32_t renderW = 0, renderH = 0, outW = 0, outH = 0;
    ID3D11ShaderResourceView* outSrv = nullptr;     // the upscaler's output, before the interface (dlOutSrv)
    ID3D11ShaderResourceView* mvSrv = nullptr;      // the DLSS motion vectors (dlMvSrv): R16G16_FLOAT, render size, pixels, this frame -> previous, jitter-free
    ID3D11ShaderResourceView* depthSrv = nullptr;   // this frame's depth copy (dlDepthSrv): Elite's reversed-Z, depth = A + B / metres
    const float* tanNow = nullptr;                  // this frame's tangents, jitter excluded (left, right, bottom, top)
    const float* tanPrev = nullptr;                 // the previous frame's (what p.tanPrev holds), null when the pass has none
    float depthA = 0.0f, depthB = 0.0f;             // p.knobs[0], p.knobs[2]
    float splitMetres = 0.0f;                       // p.split[0]
    FrameGenDeltas deltas;                          // the world delta and tvCam, the head delta and p.tvUsed, as the pass uses them
    bool depthWritten = false;                      // this frame wrote a depth copy (zcWritten)
    bool prevDepthValid = false;                    // the previous frame's depth copy is in hand (zPrevValid)
    bool worldValid = false;                        // the pass's world path ran on this frame's rows (worldOn)
    bool cameraJump = false;                        // the camera gate dropped or carried this frame's rows, or the origin jumped
};

// Once per treated eye-frame (usedDlaa, key on). `frame` is the pass's frame counter: the previous capture is used only when it was the frame before.
void frameGenShadowEyeFrame(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t eye, uint32_t frame, const FrameGenShadowFrame& f);

// Once a frame: the one-second report when the key is on, the release of the resources when it went off.
void frameGenShadowFrameBoundary();

// For the WARP rig (tools/framegen_shadow_test): whether any of the scorer's shaders, buffers or textures exist, and a copy of the open one-second window.
bool frameGenShadowResident();
// The settings menu's Monitor page: the game-device bytes the scorer holds (its VRAM line's groups, both eyes), as of the last one-second boundary; 0 while the key is off.
uint64_t frameGenShadowVramBytes();
FrameGenShadowWindow frameGenShadowOpenWindow();

}  // namespace edvr

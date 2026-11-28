#pragma once
// The frame-generation synthesizer (fix.frame_generation_shadow = on, TEMPORARY, hidden, VR only; docs/design-frame-generation-2026-10-26.md, Phase 2.1, "The synthesis pass").
// Per eye and real frame B it builds S, the frame one display period later, at output size, from B's upscaled output, depth and DLSS vectors and the head's own step; nothing
// is shown in the headset and the frame loop does not change. It runs under the shadow scorer's key (no key of its own) and is called beside it. The pure halves are
// src\common\framegen_synth_math.h (the geometry and the constant block) and framegen_synth_report.h (the statistics and the lines); the shaders are framegen_synth_shader.h.
//
// Every other real frame of an eye builds the f = 0.5 S (a later phase shows it: Elite's step is two display periods); the frames between build the f = 1 S, which lands on the
// next real frame C's time. That one is reduced to the scorer's quarter-grid luma, kept with B's luma and class, and scored against C on C's frame: the error per class of B, with
// the no-warp comparison (B against C), read back without waiting. Key off: nothing is created and nothing is dispatched.
// frameGenSynthFrameBoundary (once a frame, from temporalPassFrameBoundary) prints the one-second report.
#include <d3d11.h>

#include <cstdint>

#include "../common/framegen_monitor_text.h"
#include "../common/framegen_replay_math.h"
#include "../common/framegen_synth_math.h"
#include "../common/native_temporal.h"

namespace edvr {

// The key (the shadow scorer's, passed on by the caller). Live: turning it off re-arms the line and the next boundary releases the resources.
void frameGenSynthConfigure(bool on);
bool frameGenSynthOn();

// What the temporal pass knew about one eye-frame. Pointers are the pass's own and are not kept.
struct FrameGenSynthFrame {
    uint32_t renderW = 0, renderH = 0, outW = 0, outH = 0;
    ID3D11ShaderResourceView* outSrv = nullptr;     // the upscaler's output, before the interface (dlOutSrv)
    ID3D11ShaderResourceView* mvSrv = nullptr;      // the DLSS motion vectors (dlMvSrv): R16G16_FLOAT, render size, pixels, this frame -> previous, jitter-free
    ID3D11ShaderResourceView* depthSrv = nullptr;   // this frame's depth copy (dlDepthSrv): Elite's reversed-Z, depth = A + B / metres
    const float* tanNow = nullptr;                  // this frame's tangents, jitter excluded (left, right, bottom, top)
    float depthA = 0.0f, depthB = 0.0f;             // p.knobs[0], p.knobs[2]
    float splitMetres = 0.0f;                       // p.split[0]
    FrameGenDeltas deltas;                          // only the head delta (Hd, th) is read
    bool depthWritten = false;                      // this frame wrote a depth copy (zcWritten)
    bool cameraJump = false;                        // the camera gate dropped or carried this frame's rows, or the origin jumped: built, not scored
    bool wantShown = false;                         // the eye dump is open: build the f = 0.5 S on this frame whatever the alternation says
    // Replay capture (.agents/framegen-replay-format.md): the eye dump is open on the left eye and this is its frame replayIndex (0..5). The module copies B's output, depth and
    // vectors into staging textures and keeps what the constant block was built from; nothing is written until frameGenReplayFinish. Needs wantShown (the f = 0.5 variant).
    bool replay = false;
    uint32_t replayIndex = 0;
    const float* tanPrev = nullptr;                 // the previous frame's tangents when the pass has them (null otherwise); recorded only
    bool worldValid = false;                        // the pass's world path was on (deltas.W / deltas.tv are real); recorded only
    // Phase 2.2 (the runtime's half, edvrFrameGenNoteSlot): the head step B -> S from the runtime's pose for S's slot (frameGenSlotStep), taken by the f = 0.5 S instead of the
    // extrapolation (a replay-capture frame keeps the extrapolation, which the bench rebuilds); and whether the runtime consumes this frame's S, which puts S in the producer
    // texture it crosses in (edvrFrameGenTakeS).
    bool slotPose = false;
    float slotRot[9] = {};
    float slotT[3] = {};
    bool consumer = false;
    uint64_t slotSequence = 0;                      // the treat's sequence, which the answer to edvrFrameGenTakeS carries (0: no slot this frame)
    // Phase 2.3 (fix.frame_generation): build the f = 0.5 S on this frame whatever the alternation says, and no f = 1 S (Lock shows S in every other slot).
    bool everyHalf = false;
    // Called with the f = 0.5 S on a wantShown frame while the module still holds it (on the transfer's path it is the runtime's once handed over): the eye dump stages it here.
    // Not called for an S left to be finished (finish): the door stages the finished one.
    void (*shownSink)(void* context, ID3D11DeviceContext* ctx, ID3D11Texture2D* shown) = nullptr;
    void* shownSinkContext = nullptr;
    // Phase 2.3b: B's door (native_sharpen.cpp, the last d3d11 step of B's eye) finishes this frame's f = 0.5 S as it finishes B (frameGenSynthFinish): until then S is held, not
    // handed over, and its answer says it was not finished (no_door). finishSharpen: the door will sharpen S, so S is written where the sharpener reads it and the finish writes
    // over it (the producer texture on the uav path); otherwise S is written in the private texture and finished into the producer texture. dumpIndex: the eye dump's frame k,
    // whose finished S the door stages (-1: none).
    bool finish = false;
    bool finishSharpen = false;
    int dumpIndex = -1;
};

// Once per treated eye-frame (usedDlaa, key on). `frame` is the pass's frame counter: the f = 1 S is scored only against the frame right after it.
void frameGenSynthEyeFrame(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t eye, uint32_t frame, const FrameGenSynthFrame& f);

// Phase 2.4, auto with the performance gate closed: S is shown nowhere, so the caller builds it only to keep the cost measured that the gate reads (frameGenSynthMonitor):
// once per eye per one-second window. SampleDue: the eye has not been called in the open window (the first call after each report is the sample, so its timers and stats
// come back inside the same window). Idle, on every other eye-frame: nothing is dispatched and nothing is created or released; it only collects what earlier builds left
// (readbacks, GPU timers, a finish whose door never came). No answer is stored, so the runtime's take reads no S for that slot (S_FALSE), as with the key off.
bool frameGenSynthSampleDue(uint32_t eye);
void frameGenSynthIdle(ID3D11DeviceContext* ctx, uint32_t eye);

// The output-size S texture (the upscaler's format) the call for `frame` just built at f = 0.5 in the module's private texture, else null (also null when S went to the runtime
// through the producer texture: shownSink sees it there): valid until the next call for the eye.
ID3D11Texture2D* frameGenSynthShown(uint32_t eye, uint32_t frame);

// Phase 2.2, the runtime's read of one eye's S after its treat (edvrFrameGenTakeS is this, exported). Fills `out` (its size and version set by the caller) and clears the eye's
// answer: a second take before the next build answers sequence 0. The answer carries the slot sequence its build was handed (slotSequence).
HRESULT frameGenSynthTakeS(uint32_t eye, EdvrNativeFrameGenS* out);

// ---- Phase 2.3b: S finished as B is, at B's door ----
// The f = 0.5 S this eye's last build left for B's door, or null when none waits (S was not built, or was built without finish). Opens the finish's GPU timer when one is due.
// Same thread and context as the build; the texture stays the module's.
ID3D11Texture2D* frameGenSynthFinishSource(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t eye);
// What B's door did to B, for S. `why` other than kFrameGenFinishDone (framegen_slot_report.h) leaves S unfinished: not handed over, the runtime told why, the producer texture
// given back to the game device. Otherwise `frame` is S as B's door would composite over it (S sharpened by B's sharpener, region-sized = S's size), or null for S itself; and
// `layer` the UI layer B's composite read this sequence (null: B got no UI composite, so S gets none), with its multiply (null: none drew), its size and the rectangle the
// composite took for the region. dumpSink stages the finished S for the eye dump's frame k before it is handed over, with B as its door finished it (bFinished) beside it.
struct FrameGenSynthFinishInputs {
    uint32_t why = 0;
    ID3D11Texture2D* frame = nullptr;
    ID3D11ShaderResourceView* layer = nullptr;
    ID3D11ShaderResourceView* mult = nullptr;
    uint32_t layerW = 0, layerH = 0;
    float layerUv[4] = {};
    ID3D11Texture2D* bFinished = nullptr;
    void (*dumpSink)(ID3D11DeviceContext* ctx, int dumpIndex, ID3D11Texture2D* finishedS, ID3D11Texture2D* finishedB) = nullptr;
    // EDVR's menu on S (native_menu.cpp nativeMenuFrameGenDraw): called with the finished S, before the dump stages it and before it is handed over, to draw the menu B will get
    // into it in place, placed for S's eye (B's eye carried by the head's step rot, t: x_S = rot x_B + t). menuBox: B's bounds as the runtime's menu receives them (the whole
    // frame, B's flips). Returns kFrameGenFinishDone (drawn, or none shows on the eye) or why not (S is then left unfinished). Null: no menu is drawn and the answer's menu
    // field says so (0).
    uint32_t (*menuSink)(ID3D11DeviceContext* ctx, uint32_t eye, ID3D11Texture2D* finishedS, const float box[4], const float rot[9], const float t[3]) = nullptr;
    float menuBox[4] = {0, 0, 1, 1};
};
// Finishes the waiting S (fgSynFinish: the layer over S, moved by the head's step S was built with, framegen_synth_math.h frameGenSynthFinishHomography) into the texture the
// runtime takes it from, then hands it over (key 1) when the runtime consumes S; or leaves it unfinished. Its answer (edvrFrameGenTakeS) is stored either way. Returns the finished
// texture (valid until the next build for the eye; on the transfer's path the runtime's once handed over) or null.
ID3D11Texture2D* frameGenSynthFinish(ID3D11DeviceContext* ctx, uint32_t eye, const FrameGenSynthFinishInputs& in);

// Once a frame: the one-second report when the key is on, the release of the resources when it went off.
void frameGenSynthFrameBoundary();

// Replay capture, the writer. frameGenReplayFinish(eyesDir, stamp) is called when the eye dump writes its files: it makes <eyesDir>\fgreplay_<stamp>\ and hands the frames captured by
// the run to the pump, which writes them one file at a time on a worker thread. frameGenReplayPump (once a frame, from frameGenSynthFrameBoundary) maps a finished copy with
// DO_NOT_WAIT only (a copy the GPU has not finished is tried again next frame), starts the worker, and joins a finished one. Returns false when there was nothing to write.
bool frameGenReplayFinish(const wchar_t* eyesDir, const wchar_t* stamp);
void frameGenReplayPump();
bool frameGenReplayIdle();            // nothing captured and nothing being written
uint32_t frameGenReplayCaptured();    // frames held (captured, not yet written out)

// For the WARP rig (tools/framegen_synth_test): whether any shader, buffer or texture exists, and a copy of the open one-second window.
bool frameGenSynthResident();
FrameGenSynthWindow frameGenSynthOpenWindow();

// The settings menu's Monitor page (common/framegen_monitor_text.h): the last closed one-second window's build and finish medians and the game-device bytes the synthesizer
// holds, as its log lines say them. Any thread; empty while the key is off.
FrameGenMonitorSynth frameGenSynthMonitor();

}  // namespace edvr

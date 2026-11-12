#pragma once
#include <windows.h>
#include <d3d11.h>
#include <stddef.h>
#include <stdint.h>

#define EDVR_NATIVE_TEMPORAL_VERSION_1 1u

// Private temporal-only capability. Acquire/treat use the bound game producer;
// begin/projection/invalidate/close are CPU-only and can run on the XR owner.
// No method announces the legacy glitch/withhold consumer. The provider owns
// its context; the host retains the device and module until close completes.
struct EdvrNativeTemporalRequest {
  uint32_t size, version;
  ID3D11Device* gameDevice;
  uint64_t generation;
};
struct EdvrNativeTemporalFrame {
  uint32_t size, version;
  uint64_t generation, referenceGeneration, sequence;
  float head[12], eyeToHead[2][12]; // rigid row-major 3x4, exactly as given to Elite (the eye origins moved by the world scale, common/world_scale.h)
  float frusta[2][4];             // unjittered signed {left,right,down,up}
  uint32_t recommendedWidth, recommendedHeight;  // what this frame was rendered for
  // What the game is told now, max over eyes: during a FOV-trim
  // adoption the game re-creates its targets for this size before a frame of
  // it arrives, while the recommendation above is still the previous ask.
  // 0 when the host does not say (the recommendation is then the answer).
  uint32_t askedWidth, askedHeight;
  // Eye 0's TRUE (located, display) frustum's vertical tangents, magnitudes:
  // what the headset shows, before a FOV trim narrows what the game is
  // told. fix.ui_quality's engine-side panel sizing takes
  // its untrimmed k from it. 0 when the host does not say.
  float trueUp, trueDown;
  // B3 (the scope zoom): 1 when eye 0's frustum in `frusta` is the zoomed window this frame, and then eye 0's frustum without it, {left,right,down,up} as `frusta`. The announced
  // frusta (the panels' size, the terrain cull's cover, the menu, the visor) stay these; DLSS, the jitter and the history follow `frusta`. 0 and zeros: eye 0 is not zoomed.
  uint32_t zoomed;
  float preZoom[4];
};
// The size of the frame before B3 appended its fields: a runtime from before them sends it, and begin() takes it (with no zoom). The two DLLs are copied apart by hand.
#define EDVR_NATIVE_TEMPORAL_FRAME_SIZE_NO_ZOOM ((uint32_t)offsetof(EdvrNativeTemporalFrame, zoomed))
struct EdvrNativeTemporalProjection {
  uint32_t size, version;
  float tangentShift[2][2]; // per-eye {dx,dy}, added to both endpoints on each axis
};
typedef HRESULT(WINAPI *EdvrNativeTemporalBegin)(void*, const EdvrNativeTemporalFrame*, EdvrNativeTemporalProjection*);
// The exact successful matrix query's frame/eye and clip planes. CPU only.
typedef HRESULT(WINAPI *EdvrNativeTemporalNoteProjection)(void*, uint64_t, uint32_t, float, float);
// S_OK: AddRef'd unjittered output and UVUV full bounds retaining input flips.
// S_FALSE: unchanged input (AA off or a safe stand-down); the host must submit
// that image with this frame's jittered FOV. Failure: null/zero outputs.
// Only one successful treatment per eye/sequence. History follows successful
// treatments, never merely WaitGetPoses. Invalidate breaks both histories.
typedef HRESULT(WINAPI *EdvrNativeTemporalTreat)(void*, uint64_t, uint32_t, ID3D11Texture2D*, const float*, ID3D11Texture2D**, float*);
typedef HRESULT(WINAPI *EdvrNativeTemporalInvalidate)(void*);
typedef HRESULT(WINAPI *EdvrNativeTemporalClose)(void*);
// Consume an intentionally omitted eye without feeding bad pixels to history.
// Jump-only omissions wait for the transition-flash fix's verdict; other causes reset it.
typedef HRESULT(WINAPI *EdvrNativeTemporalSkip)(void*, uint64_t, uint32_t, uint32_t, uint32_t);
struct EdvrNativeTemporalTable {
  uint32_t size, version;
  void* context;
  EdvrNativeTemporalBegin beginFrame;
  EdvrNativeTemporalNoteProjection noteProjection;
  EdvrNativeTemporalTreat treatEye;
  EdvrNativeTemporalInvalidate invalidate;
  EdvrNativeTemporalClose close;
  EdvrNativeTemporalSkip skipEye;
};
extern "C" HRESULT WINAPI edvrAcquireNativeTemporal(const EdvrNativeTemporalRequest*, EdvrNativeTemporalTable*);

// Frame generation, Phase 2.2 (fix.frame_generation_shadow, TEMPORARY; docs/design-frame-generation-2026-10-26.md): the synthetic slot's channel. Two exports of their own
// beside the table above, so either DLL may be the older one: a runtime that finds no edvrFrameGenNoteSlot says so once and hands nothing; a d3d11.dll that is never handed a
// slot keeps S on the game device and its head on the synthesizer's own extrapolation, and its synth line says so. Each struct is versioned by its size and version, and only
// ever extended at the end: a reader takes any size at or above its own version's, and reads no field past the size it was given.
#define EDVR_NATIVE_FRAMEGEN_VERSION_1 1u
// Runtime -> d3d11, once per real pair on the XR owner, before the pair's first treat. CPU only.
struct EdvrNativeFrameGenSlot {
  uint32_t size, version;
  uint64_t sequence;            // the temporal frame (beginFrame's sequence) this slot follows
  int64_t displayTime;          // T, the pair's predicted display time (XrTime, ns)
  int64_t slotTime;             // T_S = T + (T - T_A) / 2, the slot S is made for; 0 when there is none
  uint32_t poseValid;           // 1: head is the head located at slotTime
  uint32_t consumer;            // 1: the runtime takes this pair's S through edvrFrameGenTakeS and consumes it
  float head[12];               // the head at T_S: EdvrNativeTemporalFrame::head's space and convention (rigid row-major 3x4)
  // Size 2 (Phase 2.3, fix.frame_generation): the fields below. A size 1 slot (a runtime from 2.2) has none, which reads as Lock not engaged; a d3d11.dll from 2.2 reads no
  // field past size 1. The version stays 1: the size alone says which fields are there.
  uint32_t lockEngaged;         // 1: Lock 2:1 is engaged as this pair begins (the d3d11 half builds the f = 0.5 S on every frame and no f = 1 S)
  uint32_t reserved;
  // Size 3 (the settings menu's Monitor page, FRAME GENERATION): what Lock is doing, for the page only (nothing in the frame loop reads it). A size 2 slot has none of these, and
  // the page says the runtime's state is unknown. The version stays 1.
  uint32_t lockReason;          // framegen_lock_report.h FrameGenLockReason as the gate stands now (0 none: engaged; key_off while fix.frame_generation is off)
  uint32_t lastSecond;          // 1: the four counts below are Lock's last completed one-second window (its log line); 0: no window has closed since the key went on
  uint32_t sSlots, fromS;       // S slots run / of them, the ones that showed S
  uint32_t lateS, lateReal;     // S slots that replayed B (late_s) / display periods Elite's frame missed while engaged (late_real)
  float slotDegMedian;          // the head pose's miss at S's slot, degrees, median over the pose probe's last window (slot_deg); negative: none
  uint32_t reserved3;
  uint64_t xrBytes;             // the XR device's bytes the S transfers hold now (both eyes)
  // Size 4 (Phase 2.4, fix.frame_generation = auto): the mode the runtime runs, read from word 17 (framegen_gate.h FrameGenMode: 0 off, 1 auto, 2 on). A smaller slot comes from a
  // runtime that reads auto as off: the d3d11 half's gate says so once and holds frame generation off. The version stays 1.
  uint32_t lockMode;
  uint32_t reserved4;
};
#define EDVR_NATIVE_FRAMEGEN_SLOT_SIZE_1 ((uint32_t)offsetof(EdvrNativeFrameGenSlot, lockEngaged))
#define EDVR_NATIVE_FRAMEGEN_SLOT_SIZE_2 ((uint32_t)offsetof(EdvrNativeFrameGenSlot, lockReason))
#define EDVR_NATIVE_FRAMEGEN_SLOT_SIZE_3 ((uint32_t)offsetof(EdvrNativeFrameGenSlot, lockMode))
#define EDVR_NATIVE_FRAMEGEN_SLOT_SIZE_4 ((uint32_t)sizeof(EdvrNativeFrameGenSlot))
// The state of one eye's S after its treat.
#define EDVR_NATIVE_FRAMEGEN_S_NONE 0u       // no S built for this sequence
#define EDVR_NATIVE_FRAMEGEN_S_PRODUCED 1u   // S written and published (the producer texture's key released to 1): the runtime consumes it
#define EDVR_NATIVE_FRAMEGEN_S_BUSY 2u       // the previous S was not consumed (key 1 still held): this S was not handed over
#define EDVR_NATIVE_FRAMEGEN_S_KEPT 3u       // built and not handed over (the f = 1 variant, a replay frame, or no consumer)
#define EDVR_NATIVE_FRAMEGEN_PATH_NONE 0u
#define EDVR_NATIVE_FRAMEGEN_PATH_UAV 1u     // the resample pass writes S straight into the producer texture
#define EDVR_NATIVE_FRAMEGEN_PATH_COPY 2u    // S is built in a private texture and copied into the producer texture
// d3d11 -> runtime, per eye after its treat (the owner, once the treat's callback has returned).
struct EdvrNativeFrameGenS {
  uint32_t size, version;
  uint64_t sequence;            // the sequence of the treat this answers; 0 when the eye had none since the last slot
  uint32_t eye, state;          // EDVR_NATIVE_FRAMEGEN_S_*
  uint32_t path;                // EDVR_NATIVE_FRAMEGEN_PATH_*
  uint32_t poseRuntime;         // 1: S's head step was the handed slot pose; 0: the synthesizer's extrapolation
  void* sharedHandle;           // the producer texture's NT handle (keyed mutex), owned by the d3d11 half: valid while textureSerial stands; never closed by the reader
  uint64_t textureSerial;       // changes whenever the producer texture is made again
  uint32_t width, height, format;
  uint32_t finish;              // Phase 2.3b (framegen_slot_report.h kFrameGenFinish*): 1 S was finished as B is (B's sharpening, B's UI layer by the head's step); 0 from a d3d11.dll before
                                // it (the field was reserved and zero); else why not, and S was not handed over. A runtime from before 2.3b does not read it.
  uint64_t gameBytes;           // the game-device bytes this eye's S set holds now (the producer texture, plus the private S on the copy path)
  // Size 2 (EDVR's menu on S): a size 1 answer (a d3d11.dll from before it) has none, which reads as the menu not drawn on S; the version stays 1.
  uint32_t menu;                // 1: S was finished with EDVR's menu drawn on it as the runtime draws it on B (or none showing on the eye); 0: the menu is not drawn on S
  uint32_t reserved2;
};
#define EDVR_NATIVE_FRAMEGEN_S_SIZE_1 ((uint32_t)offsetof(EdvrNativeFrameGenS, menu))
#define EDVR_NATIVE_FRAMEGEN_S_SIZE_2 ((uint32_t)sizeof(EdvrNativeFrameGenS))
typedef HRESULT(WINAPI *EdvrNativeFrameGenNoteSlot)(const EdvrNativeFrameGenSlot*);
typedef HRESULT(WINAPI *EdvrNativeFrameGenTakeS)(uint32_t, EdvrNativeFrameGenS*);
extern "C" HRESULT WINAPI edvrFrameGenNoteSlot(const EdvrNativeFrameGenSlot*);
extern "C" HRESULT WINAPI edvrFrameGenTakeS(uint32_t, EdvrNativeFrameGenS*);

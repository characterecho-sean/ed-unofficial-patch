// diag.hull_layer (TEMPORARY; docs\research-own-hull-layer-2026-11-08.md, "Rig probe" and "Rig seed"): the own ship's pool (t33) slots,
// known on the CPU from the engine's own rig data, compared with the hull layer's GPU own mask, and (hullRigProbeOwnSlots) the seed that
// mask is grown from.
//
// The chain, all on what engine motion and vehicle_rig already observe:
//   emit (FUN_144315A20, job threads): a rig record inside the own ship's or cockpit's record array (vehicle_rig.h g_vehicleRigOwn) claims the
//       0x150 items the call appended (engine_velocity_emit.h guardedLookup / collectItems), for the present frame;
//   merge (0x43513A0): claims move with their items, the way primaryCopy's plans move its claims (engine_velocity_primary_copy.h walkMerge);
//   pool copier (0x4C846A0): a claimed item copied to slot firstSlot+i of the mapped pool sets that slot's own bit, after its pose is checked
//       against its rig record's; any other copy clears the bit (the "content" set: own records the buffer holds now);
//   map (engine motion's watched 336-byte pools, render thread): a DISCARD map forgets the buffer's bits, a new frame's first map starts the
//       frame's written and claimed sets.
// The hull layer, at an eye's seed, hands the probe its mask buffer, its pool copy and the game's t33 buffer; every ~30 s and on an eye dump the
// probe copies the mask to staging, keeps that moment's CPU sets with it, reads it back without a stall a few frames later and logs one line
// (hull layer: rig_probe ...); a dump also writes eye_<stamp>_HullRigProbe.json.
//
// engine_velocity.cpp (also built into tools\engine_velocity_test) reaches the probe only through g_hullRigProbeTaps below, so it links to
// nothing; the probe sets it while diag.hull_layer is dump or separate and clears it when off.
#pragma once

#include <d3d11.h>

#include <atomic>
#include <cstdint>

namespace edvr {

struct HullRigProbeTaps {
    // FUN_144315A20's bracket, after the forward (job threads). lookup: engine motion's dictionary lookup (emit::LookupFn), null when unknown.
    void (*emit)(uintptr_t record, uintptr_t owner, int32_t before, int32_t after, void* lookup, uint32_t frame) noexcept;
    // The pool copier, once per copy-list entry (job threads); count UINT32_MAX: the task's metadata could not be read.
    void (*copy)(uintptr_t mapped, uint32_t stride, uintptr_t source, uint64_t slot, uint32_t count, uint32_t frame) noexcept;
    // The list merge, around the game's own (one job thread, begin then end).
    void (*mergeBegin)(uintptr_t destination, uintptr_t source) noexcept;
    void (*mergeEnd)(bool completed) noexcept;
    // A watched 336-byte pool mapped (render thread, under engine motion's lock).
    void (*map)(const void* buffer, void* data, uint32_t bytes, int mapType, uint32_t frame) noexcept;
};
inline std::atomic<const HullRigProbeTaps*> g_hullRigProbeTaps{nullptr};

// From hullLayerConfigure: on while diag.hull_layer is dump or separate.
void hullRigProbeConfigure(bool on);
// At an eye's seed, after the seed dispatch (render thread). mask: the eye's own mask (one uint a record, `records` of them); poolCopy: the
// layer's copy of the pool the seed read (records * 336 bytes from the view's first record); gamePool: the game's buffer at VS t33 (identity
// only); firstSlot: the view's first record in that buffer. dumpStamp: non-null on the eye dump's capture frame.
void hullRigProbeAtSeed(ID3D11DeviceContext* ctx, ID3D11Device* device, uint32_t hullFrame, int eye, ID3D11Buffer* mask, ID3D11Buffer* poolCopy,
                        const void* gamePool, uint32_t firstSlot, uint32_t records, const wchar_t* dumpStamp);
// The rig seed (docs "Rig seed", 2026-11-10): at an eye's seed, before the seed dispatch (render thread), the own slots the watched buffer
// `gamePool` holds now (the content set: own items the copier put there, cleared by any other copy and by a DISCARD map), as indices from
// firstSlot, at most `cap` of them; gen: the own rigs' generation (vehicle_rig's), also when 0 is returned. 0: the probe is off, no own rig is
// known (on foot), the buffer is not one engine motion watches, or it holds no own record. The set belongs to the frame being drawn: the
// copier filled the buffer between its map and this frame's draws.
uint32_t hullRigProbeOwnSlots(const void* gamePool, uint32_t firstSlot, uint32_t records, uint32_t* out, uint32_t cap, uint32_t* gen);
// At the hull layer's frame boundary (render thread): readbacks, the 30 s line, the target and emit-gap lines.
void hullRigProbeFrameEnd(ID3D11DeviceContext* ctx, uint32_t hullFrame);
// The layer's device goes away: staging released, pending readbacks dropped.
void hullRigProbeRelease();

}  // namespace edvr

#pragma once

#include <atomic>
#include <cstdint>
#include "../common/input_latency_protocol.h"
#include "../common/native_reflex_setting.h"

struct ID3D11Device;
struct ID3D11Texture2D;

namespace edvr::input_latency_probe {

// Off by default: input and render hooks pay only this relaxed load. The
// diagnostic owns no resources and modifies no eye image while disabled.
inline std::atomic<bool> live{false};
inline bool enabled() noexcept { return live.load(std::memory_order_relaxed); }

// Called on the owned game's Present thread after settings reload. Creates the
// per-process mapping and asynchronous GPU-fence observer only when requested.
// A mode/config generation change rejects an outstanding pulse.
void configure(ID3D11Device*, bool wanted, native_reflex::Mode, uint64_t generation) noexcept;
void stop() noexcept;

// Actual game-owned input calls report a successful DirectInput read or a
// removed message for the owned game window. A released receipt must precede
// a pressed edge. source=1 is a standard state packet, source=2 a non-peek
// buffered event, source=3 a PM_REMOVE WM_LBUTTONDOWN/UP message returned by
// PeekMessageA. This is an API receipt, not proof of gameplay processing.
// device identifies the DirectInput device or the owned window's HWND.
// Never changes game input or message results.
void mouseButton(void* device, bool pressed, unsigned source) noexcept;

// source4: the input gate supplies an unchanged, successful 256-byte state
// from its captured game keyboard, within a qualified InputManager invocation.
// Both UP/DOWN states update the optional per-device cache. A zero token supplies diagnostics
// only; it cannot certify release or consume a pulse. A qualified token is
// retained with the actual pressed receipt.
void keyboardState(void* device, const uint8_t* state256, uint64_t inputTickToken) noexcept;

// source5: one actual post-filtered UP/DOWN event from a successful standard,
// non-peek GetDeviceData read. An empty buffer supplies no events or release.
// Qualified reads cache real key cycles even while no UI request is active.
// apiReturnQpc is captured once immediately after GetDeviceData, before hook
// filtering/logging; all rows from that call share it and retain event order.
void keyboardBuffered(void* device, unsigned scanCode, bool pressed, uint64_t inputTickToken,
                      uint64_t apiReturnQpc) noexcept;
// Failed/overflowed/uninterpretable reads and replaced device identities erase
// cached knowledge; an affected active UI request explicitly rejects.
void keyboardReadLost(void* device) noexcept;

// After all eye treatments, immediately before transfer: owned-marker mode
// paints a diagnostic rectangle; opt-in GameUi mode only copies configured
// ROIs and verifies the real pixels. Both eyes must belong to the same native
// sequence and be accepted. Fence CPU notification is a completion upper bound;
// only the producer thread issues context commands or maps staging textures.
void eyeRendered(ID3D11Device*, ID3D11Texture2D*, uint64_t sequence, unsigned eye) noexcept;

// Only a successful paired native submission qualifies. This CPU endpoint is
// distinct from GPU completion and from display/optical presentation.
void eyeSubmitted(uint64_t sequence, unsigned eye, bool accepted) noexcept;

} // namespace edvr::input_latency_probe

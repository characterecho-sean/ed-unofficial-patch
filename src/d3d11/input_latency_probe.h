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

// Actual game-owned DirectInput reads call this after successful return. A
// released packet must be seen before a pressed edge. source=1 is a standard
// state packet, source=2 a non-peek buffered event. Never changes game input.
void mouseButton(void* device, bool pressed, unsigned source) noexcept;

// After all eye treatments, immediately before transfer, paint a small owned
// diagnostic response into the actual submitted eye texture. Both eyes must
// belong to the same native sequence. Signal a GPU fence after the second
// response; its CPU notification is an observed completion upper bound.
void eyeRendered(ID3D11Device*, ID3D11Texture2D*, uint64_t sequence, unsigned eye) noexcept;

// Only a successful paired native submission qualifies. This CPU endpoint is
// distinct from GPU completion and from display/optical presentation.
void eyeSubmitted(uint64_t sequence, unsigned eye, bool accepted) noexcept;

} // namespace edvr::input_latency_probe

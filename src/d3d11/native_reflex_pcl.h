#pragma once

#include <cstdint>

namespace edvr::native_reflex::pcl {

// Process-lifetime PCL telemetry, independent of vendor and low-latency mode.
// Initialization is attempted once. Normal shutdown joins the SDK worker and
// unregisters its provider. Neither operation belongs in DllMain.
bool initialize() noexcept;
bool available() noexcept;
void shutdown() noexcept;

// Mark actual engine work with its carried simulation ID. IDs 0..5 and 7 use
// NVIDIA's marker values. Deprecated INPUT_SAMPLE and direct ping emission are
// excluded. Overlapping simulation and rendering remain the caller's concern.
void mark(uint32_t marker, uint64_t actualFrameId) noexcept;

// The input owner calls this only for a removed message. Recognizing a ping
// records a pending ticket without guessing which frame will consume it.
// Returning true identifies the SDK ping and does not alter the game message.
bool messageRemoved(uint32_t messageId) noexcept;

// Call once before the actual input read of each proven simulation iteration.
// Pending pings are attributed here to that frame, including when mode is Off.
// Repeated or older frame IDs leave a newly arrived ping for the next frame.
void inputFrame(uint64_t actualFrameId) noexcept;

} // namespace edvr::native_reflex::pcl

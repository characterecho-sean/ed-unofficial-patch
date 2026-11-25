#pragma once

#include <cstdint>
#include <atomic>
#include <memory>
#include "../common/native_reflex_frame.h"

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace edvr::native_reflex {
class FrameDriver;
struct FrameSession;

// UI reads the last verified session capability without querying COM/NVAPI.
inline std::atomic<bool> fullFrameAvailable{false};
// A runtime transport fault is distinct from initial unavailability. The mode
// owner observes this latch at its next safe Present and withdraws its request.
inline std::atomic<bool> fullFrameFaulted{false};
inline bool frameSessionFaulted() noexcept { return fullFrameFaulted.load(std::memory_order_acquire); }
inline bool availableForUser() noexcept {
    return fullFrameAvailable.load(std::memory_order_acquire) && !frameSessionFaulted();
}

// A successful begin retains the exact session/device through the real
// Present. Ending a stale receipt cannot address a replacement session.
struct PresentFrameReceipt {
    std::shared_ptr<FrameSession> owner;
    uint64_t frameId = 0;
};

// Armed only in a qualified LateRender worker. The first owned immediate-
// context command begins that frame's render-submission interval.
void noteFrameCommand(ID3D11DeviceContext*) noexcept;
PresentFrameReceipt beginFramePresent(ID3D11Device*, const EdvrNativeReflexSource&) noexcept;
void endFramePresent(const PresentFrameReceipt&) noexcept;

// The input owner validates an actual removed game-window left-button message.
// Queue its press edge for the next qualified InputManager frame, independently
// of the optional diagnostic probe and the selected low-latency mode.
void mouseMessageRemoved(bool down) noexcept;

// Normal native-session lifecycle only. The returned transport is borrowed
// until stopFrameSession for this generation. Engine callbacks take their own
// strong snapshot before calling the driver without renderer locks held.
FrameDriver* startFrameSession(ID3D11Device*, uint64_t generation);
void stopFrameSession(uint64_t generation) noexcept;
void reportFrameSession(uint64_t generation) noexcept;

// Called from the game's removed WM_QUIT, outside DllMain/loader teardown.
void shutdownFrameTelemetry() noexcept;
} // namespace edvr::native_reflex

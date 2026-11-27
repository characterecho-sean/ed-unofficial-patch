#pragma once

struct ID3D11DeviceContext;

namespace edvr::native_reflex {
// Only the qualified LateRender worker arms this thread's first-command
// observer. It records entry to a tracked immediate-context command, including
// a draw the patch later suppresses. This timestamps the CPU submission region.
// Once disarmed, other commands pay only a thread-local pointer read.
using FrameCommandObserver = void (*)(ID3D11DeviceContext*) noexcept;
inline thread_local FrameCommandObserver frameCommandObserver = nullptr;

inline void observeFrameCommand(ID3D11DeviceContext* context) noexcept {
    if (const auto observer = frameCommandObserver) observer(context);
}
} // namespace edvr::native_reflex

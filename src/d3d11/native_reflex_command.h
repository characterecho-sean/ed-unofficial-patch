#pragma once

struct ID3D11DeviceContext;

namespace edvr::native_reflex {
// Only the qualified LateRender worker arms this thread's first-command
// observer. The observer disarms after the first admitted real command; all
// other commands pay a thread-local pointer read, without driver or COM calls.
using FrameCommandObserver = void (*)(ID3D11DeviceContext*) noexcept;
inline thread_local FrameCommandObserver frameCommandObserver = nullptr;

inline void observeFrameCommand(ID3D11DeviceContext* context) noexcept {
    if (const auto observer = frameCommandObserver) observer(context);
}
} // namespace edvr::native_reflex

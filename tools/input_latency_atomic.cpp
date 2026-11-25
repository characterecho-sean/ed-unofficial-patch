// Native atomic ABI for the Python diagnostic sender. The Win32 Interlocked
// names may be compiler intrinsics without DLL exports, so Python calls these
// owned exports instead. The helper has no process attach or global state.
#if !defined(_WIN64)
#error input_latency_atomic requires a Win64 build
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstdint>
#include <intrin.h>

#pragma intrinsic(_InterlockedCompareExchange64, _InterlockedExchange64)

// Valid calls return the previous word, including UINT64_MAX. Invalid pointer
// shape returns UINT64_MAX and ERROR_INVALID_PARAMETER without dereferencing it.
// The caller owns mapped-memory lifetime and guarantees the pointer is writable.
extern "C" __declspec(dllexport) uint64_t WINAPI edupInputLatencyCompareExchange64(
    volatile LONG64* word, LONG64 exchange, LONG64 expected) noexcept {
    if (!word || (reinterpret_cast<uintptr_t>(word) & 7u)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return UINT64_MAX;
    }
    return uint64_t(InterlockedCompareExchange64(word, exchange, expected));
}

extern "C" __declspec(dllexport) uint64_t WINAPI edupInputLatencyExchange64(
    volatile LONG64* word, LONG64 value) noexcept {
    if (!word || (reinterpret_cast<uintptr_t>(word) & 7u)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return UINT64_MAX;
    }
    return uint64_t(InterlockedExchange64(word, value));
}

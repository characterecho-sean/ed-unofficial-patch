#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr::input_latency_probe {

// Windows-only diagnostic IPC. Every field occupies one aligned 64-bit word;
// readers/writers publish through Interlocked operations on state. The sender
// owns request/sentQpc only between Idle -> Reserved -> Armed. The receiver owns
// results until Complete/Rejected. PID and process creation time name the map,
// so a stale test process cannot attach to a recycled game PID.
constexpr uint64_t kMagic = 0x45445550494c5031ull;
constexpr uint64_t kVersion = 1;
constexpr wchar_t kMappingPrefix[] = L"Local\\EDUP.InputLatency.";

enum class Phase : uint64_t {
    Idle, Reserved, Armed, Consumed, Rendering, Complete, Rejected, Stopped
};
enum class Refusal : uint64_t {
    None, Timeout, InvalidRequest, GenerationChanged, WrongThread,
    TextureUnsupported, MixedFrame, DuplicateEye, GraphicsFailure,
    SubmitFailed, DeviceRemoved, Stopped
};

struct alignas(8) Shared {
    uint64_t magic = kMagic, version = kVersion, size = sizeof(Shared);
    uint64_t pid = 0, creationTime = 0, frequency = 0;
    uint64_t state = uint64_t(Phase::Idle), ready = 0, generation = 0, mode = 0;
    uint64_t request = 0, sentQpc = 0, consumedQpc = 0, renderSequence = 0;
    uint64_t eyeMask = 0, submitMask = 0, gpuCompleteQpc = 0, submitCompleteQpc = 0;
    uint64_t reason = 0, source = 0;
    uint64_t reserved[12]{};
};
static_assert(sizeof(Shared) == 256 && alignof(Shared) == 8 &&
              offsetof(Shared, state) == 48 && offsetof(Shared, sentQpc) == 88 &&
              offsetof(Shared, gpuCompleteQpc) == 128,
              "input latency diagnostic IPC v1 word layout");

} // namespace edvr::input_latency_probe

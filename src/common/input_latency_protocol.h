#pragma once

#include <cstddef>
#include <cstdint>

namespace edvr::input_latency_probe {

// Windows-only diagnostic IPC. Every field occupies one aligned 64-bit word;
// readers/writers publish through Interlocked operations on state. The sender
// owns request parameters while Reserved. UI requests publish Preparing and
// await receiver-owned Prepared before stamping sentQpc and publishing Armed.
// The receiver owns results until Complete/Rejected. PID and creation time name the map,
// so a stale test process cannot attach to a recycled game PID.
constexpr uint64_t kMagic = 0x45445550494c5031ull;
constexpr uint64_t kVersion = 3;
constexpr wchar_t kMappingPrefix[] = L"Local\\EDUP.InputLatency.";

enum class Phase : uint64_t {
    Idle, Reserved, Armed, Consumed, Rendering, Complete, Rejected, Stopped,
    Preparing, Prepared
};
enum class Refusal : uint64_t {
    None, Timeout, InvalidRequest, GenerationChanged, WrongThread,
    TextureUnsupported, MixedFrame, DuplicateEye, GraphicsFailure,
    SubmitFailed, DeviceRemoved, Stopped, BaselineMismatch, CandidateOverflow, InputReadLost
};
enum class ResponseKind : uint64_t { OwnedMarker, GameUi };

struct alignas(8) Shared {
    uint64_t magic = kMagic, version = kVersion, size = sizeof(Shared);
    uint64_t pid = 0, creationTime = 0, frequency = 0;
    uint64_t state = uint64_t(Phase::Idle), ready = 0, generation = 0, mode = 0;
    uint64_t request = 0, sentQpc = 0, consumedQpc = 0, renderSequence = 0;
    uint64_t eyeMask = 0, submitMask = 0, gpuCompleteQpc = 0, submitCompleteQpc = 0;
    uint64_t reason = 0, source = 0;
    // Sender-owned words20..26 are immutable from Preparing through result.
    // ROI edges are normalized u16 left/top/right/bottom, least significant
    // edge first. Fractions use parts per million; low < high <=1,000,000.
    uint64_t responseKind = uint64_t(ResponseKind::OwnedMarker), inputScanCode = 0;
    uint64_t roiLeft = 0, roiRight = 0, orangeLowPpm = 0, orangeHighPpm = 0, targetOn = 0;
    // v3 Preparing.sentQpc is the run's untimed priming-begin fence. The
    // receiver proves fresh real key cycles and a later opposite-state stereo
    // baseline, then clears sentQpc before publishing Prepared. Armed.sentQpc
    // retains its ordinary measured-injection timestamp meaning.
    uint64_t baselineLeftPpm = 0, baselineRightPpm = 0, responseLeftPpm = 0, responseRightPpm = 0;
    uint64_t consumedTickToken = 0;
};
static_assert(sizeof(Shared) == 256 && alignof(Shared) == 8 &&
              offsetof(Shared, state) == 48 && offsetof(Shared, sentQpc) == 88 &&
              offsetof(Shared, gpuCompleteQpc) == 128 && offsetof(Shared, responseKind) == 160 &&
              offsetof(Shared, consumedTickToken) == 248,
              "input latency diagnostic IPC v3 preserves the 256-byte word layout");

} // namespace edvr::input_latency_probe

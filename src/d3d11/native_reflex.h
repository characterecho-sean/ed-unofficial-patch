#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include "../common/native_reflex_setting.h"

struct ID3D11Device;

namespace edvr::native_reflex {

enum class Marker : uint32_t {
    SimulationStart = 0,
    SimulationEnd = 1,
    RenderSubmitStart = 2,
    RenderSubmitEnd = 3,
    PresentStart = 4,
    PresentEnd = 5,
    TriggerFlash = 7,
    LatencyPing = 8,
};

// Native NVAPI timestamps in microseconds. Zero means the driver supplied no
// timestamp for that field. These records have no headset display timestamp.
struct alignas(8) LatencyFrame {
    uint64_t frameId;
    uint64_t inputSampleUs;
    uint64_t simStartUs;
    uint64_t simEndUs;
    uint64_t renderSubmitStartUs;
    uint64_t renderSubmitEndUs;
    uint64_t presentStartUs;
    uint64_t presentEndUs;
    uint64_t driverStartUs;
    uint64_t driverEndUs;
    uint64_t osRenderQueueStartUs;
    uint64_t osRenderQueueEndUs;
    uint64_t gpuRenderStartUs;
    uint64_t gpuRenderEndUs;
    uint32_t gpuActiveRenderUs;
    uint32_t gpuFrameUs;
    uint64_t cameraConstructedUs;
    uint32_t crossAdapterCopyUs;
    uint32_t aiFrameUs;
};

struct LatencyBatch {
    // The first count records are available, in increasing driver frame order.
    std::array<LatencyFrame, 64> frames;
    size_t count = 0;
};

class FrameDriver;

enum class ModeStatus : uint8_t {
    Unavailable, Pending, ConfirmedOff, ConfirmedOn, ConfirmedOnWithBoostRequested,
    RequestRefused, WithdrawalRefused, ReadbackUnavailable, DriverReportsOff, DriverReportsOn,
};

// Snapshot of the current session's driver evidence for the settings menu.
ModeStatus modeStatusForUser() noexcept;

inline constexpr const char* kModeUnavailableHint =
    "On and On + Boost need supported NVIDIA VR and an eye view under Mirror window > Image. Off remains available.";

// F8 shows driver evidence separately from the saved choice and eligibility.
constexpr const char* modeStatusHint(ModeStatus status, bool canEnable) noexcept {
    switch (status) {
        case ModeStatus::WithdrawalRefused:
            return "The Off request failed. Close the VR session to retry Off, or restart Elite.";
        case ModeStatus::DriverReportsOn:
            return "The driver still reports On. Check the graphics log or restart Elite.";
        case ModeStatus::RequestRefused:
            return "ED-UP couldn't apply the saved mode. Check the graphics log.";
        case ModeStatus::ReadbackUnavailable:
            return "ED-UP couldn't verify the driver mode. Check the graphics log.";
        case ModeStatus::DriverReportsOff:
            return "The driver still reports Off. Check the graphics log.";
        case ModeStatus::Pending:
            return "Waiting to apply the saved mode.";
        case ModeStatus::ConfirmedOff:
            if (canEnable) return "The driver reports Off.";
            return "The driver reports Off. On and On + Boost need supported NVIDIA VR "
                   "and an eye view under Mirror window > Image.";
        case ModeStatus::ConfirmedOn:
            return canEnable ? "The driver reports On." : kModeUnavailableHint;
        case ModeStatus::ConfirmedOnWithBoostRequested:
            return canEnable ? "The driver reports On. Boost was requested." : kModeUnavailableHint;
        case ModeStatus::Unavailable:
            return kModeUnavailableHint;
    }
    return kModeUnavailableHint;
}

// Per native frame session. The provider serializes config updates, Presents
// and close. Cleanup ownership begins when an enable request is accepted.
class ModeHistory {
public:
    // Lets the provider retire active measurements before issuing a write.
    bool requestDue(Mode mode, bool frameDriver) const noexcept {
        return !noted_ || mode != requested_ || frameDriver != frameDriver_ || (retryNeeded_ && !retryDelay_);
    }

    bool change(Mode mode, bool frameDriver = false) noexcept {
        if (!requestDue(mode, frameDriver)) return false;
        if (mode != requested_ || frameDriver != frameDriver_)
            withdrawalRetries_ = 0;
        requested_ = mode;
        frameDriver_ = frameDriver;
        noted_ = true;
        observing_ = false;
        retryNeeded_ = false;
        retryDelay_ = 0;
        status_ = ModeStatus::Unavailable;
        return true;
    }

    Mode requested() const noexcept { return requested_; }
    ModeStatus status() const noexcept { return status_; }

    // Only label a sample when the driver accepted the request and its latest
    // latency bit agrees. NVAPI has no independent Boost readback bit.
    std::optional<Mode> confirmedMode() const noexcept {
        switch (status_) {
            case ModeStatus::ConfirmedOff: return Mode::Off;
            case ModeStatus::ConfirmedOn: return Mode::On;
            case ModeStatus::ConfirmedOnWithBoostRequested: return Mode::Boost;
            default: return std::nullopt;
        }
    }

    bool requiresDriver() const noexcept { return requested_ != Mode::Off || needsReset_ || frameDriver_; }
    bool needsReset() const noexcept { return needsReset_; }
    bool observing() const noexcept { return observing_; }
    bool retryNeeded() const noexcept { return retryNeeded_; }
    void deferRequest() noexcept {
        status_ = ModeStatus::Pending;
        retryNeeded_ = true;
    }

    void acceptRequest() noexcept {
        if (requested_ != Mode::Off) needsReset_ = true;
        presents_ = 0;
        observing_ = true;
    }

    void refuseRequest() noexcept {
        status_ = requested_ == Mode::Off && needsReset_ ? ModeStatus::WithdrawalRefused : ModeStatus::RequestRefused;
        scheduleWithdrawalRetry();
    }

    void recordReadback(bool readable, bool enabled) noexcept {
        if (!readable) status_ = ModeStatus::ReadbackUnavailable;
        else if (requested_ == Mode::Off)
            status_ = enabled ? ModeStatus::DriverReportsOn : ModeStatus::ConfirmedOff;
        else if (!enabled) status_ = ModeStatus::DriverReportsOff;
        else status_ = requested_ == Mode::Boost ? ModeStatus::ConfirmedOnWithBoostRequested : ModeStatus::ConfirmedOn;
    }

    static constexpr unsigned kLastObservation = 64;

    // Count only successful Presents from the acquired device and producer.
    // Advance any withdrawal backoff and return a readback number when due.
    unsigned notePresent() noexcept {
        if (retryDelay_) --retryDelay_;
        if (!observing_) return 0;
        ++presents_;
        return presents_ == 4 || presents_ == 16 || presents_ == kLastObservation ? presents_ : 0;
    }

    // An enabled or unreadable final Off sample keeps cleanup armed and can
    // use the same bounded retry schedule as a refused setter.
    void finishObservation() noexcept {
        observing_ = false;
        if (requested_ != Mode::Off) return;
        if (status_ == ModeStatus::ConfirmedOff) needsReset_ = false;
        else scheduleWithdrawalRetry();
    }

private:
    // Start after the failed attempt or its last readback. Config edits that
    // leave the requested mode unchanged cannot restart the retry budget.
    void scheduleWithdrawalRetry() noexcept {
        constexpr std::array<unsigned, 3> delays{4, 16, 64};
        if (requested_ != Mode::Off || !needsReset_ || withdrawalRetries_ == delays.size()) return;
        retryDelay_ = delays[withdrawalRetries_++];
        retryNeeded_ = true;
    }

    Mode requested_ = Mode::Off;
    ModeStatus status_ = ModeStatus::Unavailable;
    unsigned presents_ = 0;
    unsigned withdrawalRetries_ = 0;
    unsigned retryDelay_ = 0;
    bool noted_ = false;
    bool frameDriver_ = false;
    bool needsReset_ = false;
    bool observing_ = false;
    bool retryNeeded_ = false;
};

namespace detail {

// Layouts for NVIDIA's public D3D NVAPI v1 ABI. NvBool occupies one byte.
// https://docs.nvidia.com/nvapi/group__dx.html
struct SetSleepMode {
    uint32_t version;
    uint8_t lowLatencyMode;
    uint8_t lowLatencyBoost;
    uint32_t minimumIntervalUs;
    uint8_t useMarkersToOptimize;
    uint8_t useMinQueueTime;
    uint8_t reserved[30];
};

struct GetSleepStatus {
    uint32_t version;
    uint8_t lowLatencyMode;
    uint8_t fullscreenVrr;
    uint8_t controlPanelVsyncOn;
    uint32_t sleepIntervalUs;
    uint8_t useGameSleep;
    uint8_t fullscreenIFlip;
    uint8_t otherStatus[6];  // Unread NVAPI output at bytes 14–19.
    uint8_t reserved[114];
};

static_assert(sizeof(SetSleepMode) == 44 && offsetof(SetSleepMode, minimumIntervalUs) == 8 &&
              offsetof(SetSleepMode, reserved) == 14, "NV_SET_SLEEP_MODE_PARAMS_VER1 layout");
static_assert(sizeof(GetSleepStatus) == 136 && offsetof(GetSleepStatus, lowLatencyMode) == 4 &&
              offsetof(GetSleepStatus, sleepIntervalUs) == 8 &&
              offsetof(GetSleepStatus, useGameSleep) == 12 && offsetof(GetSleepStatus, reserved) == 20,
              "NV_GET_SLEEP_STATUS_PARAMS_VER1 layout");

constexpr uint32_t version1(size_t size) noexcept {
    return static_cast<uint32_t>(size) | (1u << 16);
}

constexpr SetSleepMode modeParameters(Mode mode, bool optimizeMarkers = false) noexcept {
    SetSleepMode params{};
    params.version = version1(sizeof(params));
    params.lowLatencyMode = mode != Mode::Off;
    params.lowLatencyBoost = mode == Mode::Boost;
    params.useMarkersToOptimize = mode != Mode::Off && optimizeMarkers;
    return params;
}

// NV_LATENCY_MARKER_PARAMS_VER1 and NV_LATENCY_RESULT_PARAMS_VER1 from
// https://github.com/NVIDIA/nvapi/blob/main/nvapi.h
struct alignas(8) MarkerParams {
    uint32_t version;
    uint64_t frameId;
    uint32_t markerType;
    uint64_t reserved0;
    uint8_t reserved[56];
};

struct LatencyResult {
    struct FrameReport {
        LatencyFrame timing;
        uint8_t reserved[104];
    };
    uint32_t version;
    FrameReport frames[64];
    uint8_t reserved[32];
};

static_assert(sizeof(MarkerParams) == 88 && offsetof(MarkerParams, frameId) == 8 &&
              offsetof(MarkerParams, markerType) == 16 && offsetof(MarkerParams, reserved0) == 24 &&
              offsetof(MarkerParams, reserved) == 32, "NV_LATENCY_MARKER_PARAMS_VER1 layout");
static_assert(sizeof(LatencyFrame) == 136 && offsetof(LatencyFrame, gpuRenderEndUs) == 104 &&
              offsetof(LatencyFrame, gpuActiveRenderUs) == 112 &&
              offsetof(LatencyFrame, cameraConstructedUs) == 120, "NVAPI latency timing fields");
static_assert(sizeof(LatencyResult::FrameReport) == 240 && offsetof(LatencyResult, frames) == 8 &&
              offsetof(LatencyResult, reserved) == 15368 && sizeof(LatencyResult) == 15400,
              "NV_LATENCY_RESULT_PARAMS_VER1 layout");

constexpr MarkerParams markerParameters(uint64_t sourceFrameId, Marker marker) noexcept {
    MarkerParams params{};
    params.version = version1(sizeof(params));
    params.frameId = sourceFrameId;
    params.markerType = static_cast<uint32_t>(marker);
    return params;
}

constexpr bool validSpan(uint64_t start, uint64_t end) noexcept { return start && end >= start; }

// The driver's 64 slots run oldest to newest. Require all three recorded
// application phases and GPU completion without requiring these spans to be
// disjoint. The caller can advance its watermark after each successful batch.
inline bool collectLatency(const LatencyResult& raw, uint64_t firstFrame, uint64_t newestFrame,
                           uint64_t afterFrameId, LatencyBatch& output) noexcept {
    output.count = 0;
    if (!firstFrame) return false;
    for (const auto& report : raw.frames) {
        const auto& frame = report.timing;
        if (frame.frameId <= afterFrameId || frame.frameId < firstFrame || frame.frameId > newestFrame ||
            !validSpan(frame.simStartUs, frame.simEndUs) ||
            !validSpan(frame.renderSubmitStartUs, frame.renderSubmitEndUs) ||
            !validSpan(frame.presentStartUs, frame.presentEndUs) ||
            !validSpan(frame.gpuRenderStartUs, frame.gpuRenderEndUs) ||
            frame.gpuRenderEndUs <= frame.simStartUs) continue;
        output.frames[output.count++] = frame;
        afterFrameId = frame.frameId;
    }
    return output.count != 0;
}

// Accept source IDs on the simulation producer. Concurrent render and
// Present hooks can still address older sources, without imposing an order
// on render or Present events across different frames.
class FrameSleepHistory {
public:
    FrameSleepHistory() noexcept {
        for (auto& frame : slept_) frame.store(0, std::memory_order_relaxed);
    }

    template<class Sleep>
    bool sleep(uint64_t frame, Sleep invoke) noexcept {
        if (!frame || retired()) return false;
        auto last = attempted_.load(std::memory_order_relaxed);
        do {
            if (frame <= last) return false;
        } while (!attempted_.compare_exchange_weak(last, frame, std::memory_order_relaxed));
        // The reservation is permanent even if NVAPI fails. No transport
        // lock is held while the driver waits on frame pacing.
        if (retired() || !invoke() || retired()) return false;
        auto& slot = slept_[frame % slept_.size()];
        uint64_t previous = slot.load(std::memory_order_relaxed);
        while (frame > previous &&
               !slot.compare_exchange_weak(previous, frame, std::memory_order_release, std::memory_order_relaxed)) {}
        uint64_t first = first_.load(std::memory_order_relaxed);
        while ((!first || frame < first) &&
               !first_.compare_exchange_weak(first, frame, std::memory_order_release, std::memory_order_relaxed)) {}
        return true;
    }

    bool contains(uint64_t frame) const noexcept {
        if (!frame || retired() ||
            slept_[frame % slept_.size()].load(std::memory_order_acquire) != frame) return false;
        const auto newest = lastAttemptedFrame();
        return newest - frame < slept_.size();
    }
    bool retired() const noexcept { return retired_.load(std::memory_order_acquire); }
    void retire() noexcept { retired_.store(true, std::memory_order_release); }
    uint64_t firstFrame() const noexcept { return first_.load(std::memory_order_acquire); }
    uint64_t lastAttemptedFrame() const noexcept { return attempted_.load(std::memory_order_relaxed); }

private:
    std::array<std::atomic<uint64_t>, 128> slept_{};
    std::atomic<uint64_t> attempted_{0};
    std::atomic<uint64_t> first_{0};
    std::atomic<bool> retired_{false};
};

constexpr int kUnavailable = 1;
constexpr int kDriverFault = 2;

struct Result {
    const char* reason;
    bool apiResolved = false;
    int setStatus = kUnavailable;
    int getStatus = kUnavailable;
    GetSleepStatus readback{};
    bool retry = false;
};

// Until our first accepted Sleep, a mode baseline and the vendor Sleep call
// must not overlap. Either can decline immediately without waiting on the
// other. Once Sleep ownership is proven, changes use the normal driver path.
class FirstSleepGate {
public:
    enum class Admission : uint8_t { Unproven, Busy, Owned };

    explicit FirstSleepGate(bool owned) noexcept : activity_(owned ? Activity::Owned : Activity::Idle) {}

    Admission beginSleep() noexcept { return begin(Activity::Sleeping); }
    Admission beginModeUpdate() noexcept { return begin(Activity::CheckingMode); }

    // Only a caller admitted as Unproven may finish the reserved operation.
    void finish(bool sleepAccepted) noexcept {
        activity_.store(sleepAccepted ? Activity::Owned : Activity::Idle, std::memory_order_release);
    }

private:
    enum class Activity : uint8_t { Idle, Sleeping, CheckingMode, Owned };

    Admission begin(Activity next) noexcept {
        auto current = activity_.load(std::memory_order_acquire);
        if (current == Activity::Idle &&
            activity_.compare_exchange_strong(current, next, std::memory_order_acq_rel))
            return Admission::Unproven;
        return current == Activity::Owned ? Admission::Owned : Admission::Busy;
    }

    std::atomic<Activity> activity_;
};

// Only the game-sleep bit can be explained by an accepted ED-UP Sleep on
// this device. An enabled mode or a nonzero interval remains external policy.
constexpr bool hasExternalDriverPolicy(const GetSleepStatus& status, bool ownGameSleep) noexcept {
    return status.lowLatencyMode || status.sleepIntervalUs ||
           (status.useGameSleep && !ownGameSleep);
}

constexpr bool canWithdrawRetiredMode(Mode mode, bool alreadyOwned) noexcept {
    return mode == Mode::Off && alreadyOwned;
}

template<class Read>
void readMode(Result& result, Mode mode, Read read) {
    result.readback = {};
    result.readback.version = version1(sizeof(result.readback));
    result.getStatus = read(result.readback);
    if (result.getStatus != 0) {
        result.reason = "readback_failed";
    } else {
        result.reason = bool(result.readback.lowLatencyMode) == (mode != Mode::Off)
            ? "driver_mode_matches" : "driver_mode_differs";
    }
}

// Before taking ownership, leave existing game or driver policy alone.
// GetSleepStatus cannot recover the previous Boost or optimization parameters.
template<class Read, class Set>
Result requestMode(Mode mode, bool alreadyOwned, Read read, Set set,
                   bool ownGameSleep = false, bool optimizeMarkers = false) {
    Result result{"set_failed", true};
    if (!alreadyOwned) {
        result.readback.version = version1(sizeof(result.readback));
        result.getStatus = read(result.readback);
        if (result.getStatus != 0) {
            result.reason = "baseline_unknown";
            return result;
        }
        if (hasExternalDriverPolicy(result.readback, ownGameSleep)) {
            result.reason = "existing_driver_policy";
            return result;
        }
    }
    auto params = modeParameters(mode, optimizeMarkers);
    result.setStatus = set(params);
    // Baseline data describes the state before the request.
    result.readback = {};
    result.getStatus = kUnavailable;
    if (result.setStatus == 0) readMode(result, mode, read);
    return result;
}

// Driver adapters. The Windows native-frame rig supplies controlled substitutes.
Result apply(ID3D11Device* device, Mode mode, bool alreadyOwned);
Result applyFrameMode(FrameDriver& frameDriver, ID3D11Device* device, Mode mode, bool alreadyOwned);
Result observe(ID3D11Device* device, Mode mode);
void logMode(Mode mode, const char* event, uint64_t session, const Result& result, unsigned presents = 0);

}  // namespace detail

// Holds one device reference until all game-thread callers release their
// shared handles. Call Sleep on the simulation producer and mark the actual
// game phase on its owning thread, with the ID captured from simulation.
class FrameDriver {
public:
    ~FrameDriver();
    FrameDriver(const FrameDriver&) = delete;
    FrameDriver& operator=(const FrameDriver&) = delete;

    bool ready() const noexcept {
        return configured_.load(std::memory_order_acquire) && !frames_.retired();
    }

    // Attempt Sleep once per strictly increasing source ID. A true result
    // permits that ID's markers from other threads. Skips leave the transport
    // healthy, including while the first mode update is checking its baseline.
    enum class SleepResult : uint8_t { Skipped, Accepted, Failed };
    SleepResult sleepFrame(uint64_t sourceFrameId) noexcept;
    bool sleep(uint64_t sourceFrameId) noexcept { return sleepFrame(sourceFrameId) == SleepResult::Accepted; }
    // Insert an actual game phase for a recent source with accepted Sleep.
    // The caller owns phase pairing and ordering at the hook sites.
    bool mark(uint64_t sourceFrameId, Marker marker) noexcept;
    // Read the driver's completed timings. Poll after warmup and advance
    // afterFrameId to the newest frame processed by the caller.
    bool getLatency(LatencyBatch& output, uint64_t afterFrameId = 0) noexcept;

    // Retire on close, before revoking device mode ownership. Callers already
    // admitted may finish while their shared handles retain the device.
    void retire() noexcept { frames_.retire(); }

private:
    FrameDriver(ID3D11Device* device, bool ownGameSleep);
    void reportFailure(std::atomic<bool>& logged, const char* event, int code) noexcept;
    friend std::shared_ptr<FrameDriver> openFrameDriver(ID3D11Device*);
    friend detail::Result detail::applyFrameMode(FrameDriver&, ID3D11Device*, Mode, bool);

    ID3D11Device* const device_;
    detail::FrameSleepHistory frames_;
    detail::FirstSleepGate firstSleep_;
    std::atomic<bool> configured_{false};
    std::atomic<bool> loggedSleepFailure_{false};
    std::atomic<bool> loggedMarkerFailure_{false};
    std::atomic<bool> loggedReportFailure_{false};
};

// Call at enrollment before the first ED-UP mode write. Validate API and
// adapter once and refuse existing driver policy. A stale game-sleep bit can
// be carried over only from an accepted ED-UP Sleep on this same device.
std::shared_ptr<FrameDriver> openFrameDriver(ID3D11Device* device);

// A verified non-NVIDIA device can measure response with Reflex Off, even
// without a frame driver. Unknown adapters and NVIDIA policy need readback.
bool isNonNvidiaAdapter(ID3D11Device* device);

// Return true when a request first arrives, changes or retries. Accepted
// requests begin a new observation window on the same device.
inline bool updateMode(ModeHistory& history, ID3D11Device* device, Mode mode, uint64_t session,
                       FrameDriver* frameDriver = nullptr) {
    const bool isRetry = history.retryNeeded() && history.requested() == mode;
    if (!history.change(mode, frameDriver != nullptr)) return false;
    detail::Result result{"off_unmodified"};
    if (frameDriver)
        result = detail::applyFrameMode(*frameDriver, device, mode, history.needsReset());
    else if (history.requiresDriver())
        result = detail::apply(device, mode, history.needsReset());
    if (result.retry) {
        history.deferRequest();
        if (isRetry) return true;
    } else if (result.setStatus == 0) {
        history.acceptRequest();
        history.recordReadback(result.getStatus == 0, result.readback.lowLatencyMode != 0);
    } else if (history.requiresDriver()) {
        history.refuseRequest();
    }
    detail::logMode(mode, "apply", session, result);
    return true;
}

inline void notePresent(ModeHistory& history, ID3D11Device* device, uint64_t session) {
    const unsigned presents = history.notePresent();
    if (!presents) return;
    const auto result = detail::observe(device, history.requested());
    history.recordReadback(result.getStatus == 0, result.readback.lowLatencyMode != 0);
    detail::logMode(history.requested(), "observe", session, result, presents);
    if (presents == ModeHistory::kLastObservation)
        history.finishObservation();
}

// Try Off on normal close when a prior enable still belongs to this session.
inline void closeMode(const ModeHistory& history, ID3D11Device* device, uint64_t session) {
    if (!history.needsReset()) return;
    detail::logMode(Mode::Off, "close", session, detail::apply(device, Mode::Off, true));
}

// Called after a successful, non-test game Present and its profile/config tick.
void updateOnGamePresent(ID3D11Device* device);

}  // namespace edvr::native_reflex

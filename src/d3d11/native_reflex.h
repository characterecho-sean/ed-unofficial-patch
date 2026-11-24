#pragma once

#include <cstddef>
#include <cstdint>
#include "../common/native_reflex_setting.h"

struct ID3D11Device;

namespace edvr::native_reflex {

// Per native frame session. The provider serializes config updates, Presents
// and close. Cleanup ownership begins when an enable request is accepted.
class ModeHistory {
public:
    bool change(Mode mode) noexcept {
        if (noted_ && requested_ == mode) return false;
        requested_ = mode;
        noted_ = true;
        observing_ = false;
        return true;
    }

    Mode requested() const noexcept { return requested_; }
    bool requiresDriver() const noexcept { return requested_ != Mode::Off || needsReset_; }
    bool needsReset() const noexcept { return needsReset_; }
    bool observing() const noexcept { return observing_; }

    void acceptRequest() noexcept {
        if (requested_ != Mode::Off) needsReset_ = true;
        presents_ = 0;
        observing_ = true;
    }

    static constexpr unsigned kLastObservation = 64;

    // Only count successful Presents from the acquired device and producer.
    // Return the sample number when a readback is due.
    unsigned notePresent() noexcept {
        if (!observing_) return 0;
        ++presents_;
        return presents_ == 4 || presents_ == 16 || presents_ == kLastObservation ? presents_ : 0;
    }

    // An enabled or unreadable final Off sample keeps close-time cleanup armed.
    void finishObservation(bool driverDisabled) noexcept {
        if (requested_ == Mode::Off && driverDisabled) needsReset_ = false;
        observing_ = false;
    }

private:
    Mode requested_ = Mode::Off;
    unsigned presents_ = 0;
    bool noted_ = false;
    bool needsReset_ = false;
    bool observing_ = false;
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

constexpr SetSleepMode modeParameters(Mode mode) noexcept {
    SetSleepMode params{};
    params.version = version1(sizeof(params));
    params.lowLatencyMode = mode != Mode::Off;
    params.lowLatencyBoost = mode == Mode::Boost;
    return params;
}

constexpr int kUnavailable = 1;
constexpr int kDriverFault = 2;

struct Result {
    const char* reason;
    bool apiResolved = false;
    int setStatus = kUnavailable;
    int getStatus = kUnavailable;
    GetSleepStatus readback{};
};

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
Result requestMode(Mode mode, bool alreadyOwned, Read read, Set set) {
    Result result{"set_failed", true};
    if (!alreadyOwned) {
        result.readback.version = version1(sizeof(result.readback));
        result.getStatus = read(result.readback);
        if (result.getStatus != 0) {
            result.reason = "baseline_unknown";
            return result;
        }
        if (result.readback.lowLatencyMode || result.readback.useGameSleep || result.readback.sleepIntervalUs) {
            result.reason = "existing_driver_policy";
            return result;
        }
    }
    auto params = modeParameters(mode);
    result.setStatus = set(params);
    // Baseline data describes the state before the request.
    result.readback = {};
    result.getStatus = kUnavailable;
    if (result.setStatus == 0) readMode(result, mode, read);
    return result;
}

// Driver adapters. The Windows native-frame rig supplies controlled substitutes.
Result apply(ID3D11Device* device, Mode mode, bool alreadyOwned);
Result observe(ID3D11Device* device, Mode mode);
void logMode(Mode mode, const char* event, uint64_t session, const Result& result, unsigned presents = 0);

}  // namespace detail

// Return true when an effective setting first arrives or changes. Accepted
// requests start a new observation window on the same device.
inline bool updateMode(ModeHistory& history, ID3D11Device* device, Mode mode, uint64_t session) {
    if (!history.change(mode)) return false;
    const auto result = history.requiresDriver()
        ? detail::apply(device, mode, history.needsReset()) : detail::Result{"off_unmodified"};
    if (result.setStatus == 0) history.acceptRequest();
    detail::logMode(mode, "apply", session, result);
    return true;
}

inline void notePresent(ModeHistory& history, ID3D11Device* device, uint64_t session) {
    const unsigned presents = history.notePresent();
    if (!presents) return;
    const auto result = detail::observe(device, history.requested());
    detail::logMode(history.requested(), "observe", session, result, presents);
    if (presents == ModeHistory::kLastObservation)
        history.finishObservation(result.getStatus == 0 && !result.readback.lowLatencyMode);
}

// Try Off on normal close when a prior enable still belongs to this session.
inline void closeMode(const ModeHistory& history, ID3D11Device* device, uint64_t session) {
    if (!history.needsReset()) return;
    detail::logMode(Mode::Off, "close", session, detail::apply(device, Mode::Off, true));
}

// Called after a successful, non-test game Present and its profile/config tick.
void updateOnGamePresent(ID3D11Device* device);

}  // namespace edvr::native_reflex

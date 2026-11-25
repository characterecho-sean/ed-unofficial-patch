#pragma once

#include "../common/input_latency_protocol.h"
#include <array>
#include <cstdint>
#include <limits>
#include <cstddef>

namespace edvr::input_latency_probe {

// An input owner may report the same edge through repeated reads or messages.
// Each device or game window must report release before its next press edge.
class ButtonEdges final {
public:
    bool press(uintptr_t device, bool pressed) noexcept {
        if (!device) return false;
        Entry* slot = nullptr;
        for (auto& entry : entries_) {
            if (entry.device == device) { slot = &entry; break; }
            if (!entry.device && !slot) slot = &entry;
        }
        if (!slot) return false;
        if (!slot->device) slot->device = device;
        if (!pressed) { slot->released = true; return false; }
        const bool edge = slot->released;
        slot->released = false;
        return edge;
    }
    bool released(uintptr_t device) const noexcept {
        for (const auto& entry : entries_) if (entry.device == device && device) return entry.released;
        return false;
    }
    void reset() noexcept { entries_ = {}; }
private:
    struct Entry { uintptr_t device = 0; bool released = false; };
    std::array<Entry, 16> entries_{};
};

// Input may arrive between two eye callbacks. Remember every observed pair,
// even while idle, and skip that whole sequence when a pulse is consumed.
class PairBoundary final {
public:
    bool observe(uint64_t sequence, unsigned eye) noexcept {
        if (!sequence || eye > 1) return false;
        if (sequence_ != sequence) { sequence_ = sequence; eyeMask_ = 0; }
        eyeMask_ |= uint64_t(1) << eye;
        return true;
    }
    void consumed() noexcept { skippedSequence_ = sequence_; }
    bool allowsResponse(uint64_t sequence) const noexcept {
        return sequence && sequence != skippedSequence_;
    }
    void reset() noexcept { sequence_ = eyeMask_ = skippedSequence_ = 0; }
private:
    uint64_t sequence_ = 0, eyeMask_ = 0, skippedSequence_ = 0;
};

struct PulseResult {
    uint64_t request = 0, sentQpc = 0, consumedQpc = 0, sequence = 0;
    uint64_t eyeMask = 0, submitMask = 0, gpuCompleteQpc = 0, submitCompleteQpc = 0;
    unsigned source = 0;
    uint64_t consumedTickToken = 0;
    Refusal reason = Refusal::None;
};

// Portable policy for one externally armed pulse. Callers serialize access;
// timestamps use one QPC frequency. Completion requires the response in both
// eyes, both accepted submissions, and notification of the signaled GPU fence.
class Pulse final {
public:
    Phase phase() const noexcept { return phase_; }
    const PulseResult& result() const noexcept { return result_; }
    bool active() const noexcept {
        return phase_ == Phase::Armed || phase_ == Phase::Consumed || phase_ == Phase::Rendering;
    }
    bool terminal() const noexcept { return phase_ == Phase::Complete || phase_ == Phase::Rejected; }

    bool arm(uint64_t request, uint64_t sentQpc, uint64_t now, uint64_t frequency) noexcept {
        if (active()) { reject(Refusal::InvalidRequest); return false; }
        result_ = {};
        result_.request = request; result_.sentQpc = sentQpc;
        phase_ = Phase::Armed;
        frequency_ = frequency;
        if (!request || request <= newestRequest_ || !sentQpc || sentQpc > now ||
            !frequency || frequency > (std::numeric_limits<uint64_t>::max)() / 2) {
            reject(Refusal::InvalidRequest); return false;
        }
        newestRequest_ = request;
        return !expire(now);
    }
    bool consume(uint64_t now, unsigned source, uint64_t inputTickToken = 0) noexcept {
        if (phase_ != Phase::Armed) return false;
        if (expire(now)) return false;
        if ((source != 1 && source != 2 && source != 3 && source != 4) ||
            (source == 4 && !inputTickToken)) { reject(Refusal::InvalidRequest); return false; }
        result_.consumedQpc = now; result_.source = source;
        result_.consumedTickToken = inputTickToken;
        phase_ = Phase::Consumed;
        return true;
    }
    bool render(uint64_t sequence, unsigned eye, uint64_t now) noexcept {
        if (phase_ != Phase::Consumed && phase_ != Phase::Rendering) return false;
        if (expire(now)) return false;
        if (!sequence || eye > 1) { reject(Refusal::InvalidRequest); return false; }
        if (result_.sequence && result_.sequence != sequence) { reject(Refusal::MixedFrame); return false; }
        const uint64_t bit = uint64_t(1) << eye;
        if (result_.eyeMask & bit) { reject(Refusal::DuplicateEye); return false; }
        result_.sequence = sequence; result_.eyeMask |= bit;
        phase_ = Phase::Rendering;
        return true;
    }
    bool submit(uint64_t sequence, unsigned eye, bool accepted, uint64_t now) noexcept {
        if (phase_ != Phase::Rendering) return false;
        if (expire(now)) return false;
        if (!sequence || sequence != result_.sequence) { reject(Refusal::MixedFrame); return false; }
        if (eye > 1) { reject(Refusal::InvalidRequest); return false; }
        const uint64_t bit = uint64_t(1) << eye;
        if (result_.submitMask & bit) { reject(Refusal::DuplicateEye); return false; }
        if (!accepted || !(result_.eyeMask & bit)) { reject(Refusal::SubmitFailed); return false; }
        result_.submitMask |= bit;
        if (result_.submitMask == 3) result_.submitCompleteQpc = now;
        complete();
        return true;
    }
    bool gpuCompleted(uint64_t now) noexcept {
        if (phase_ != Phase::Rendering) return false;
        if (expire(now)) return false;
        if (result_.eyeMask != 3 || result_.gpuCompleteQpc) { reject(Refusal::GraphicsFailure); return false; }
        result_.gpuCompleteQpc = now;
        complete();
        return true;
    }
    // UI evidence is qualified outside Pulse: one FIFO candidate has both
    // accepted submissions and pixel proof in both immutable staging copies.
    // Rejected/unchanged candidates never rewrite the original input clock.
    bool uiResponse(uint64_t sequence, uint64_t gpuQpc, uint64_t submitQpc, uint64_t now) noexcept {
        if (phase_ != Phase::Consumed || result_.source != 4 || !result_.consumedTickToken) return false;
        if (expire(now)) return false;
        if (!sequence || gpuQpc < result_.consumedQpc || submitQpc < result_.consumedQpc ||
            gpuQpc > now || submitQpc > now) { reject(Refusal::InvalidRequest); return false; }
        result_.sequence = sequence; result_.eyeMask = result_.submitMask = 3;
        result_.gpuCompleteQpc = gpuQpc; result_.submitCompleteQpc = submitQpc;
        complete();
        return true;
    }
    bool expire(uint64_t now) noexcept {
        if (!active()) return false;
        if (!now || now < result_.sentQpc || (result_.consumedQpc && now < result_.consumedQpc)) {
            reject(Refusal::InvalidRequest); return true;
        }
        if (now - result_.sentQpc > frequency_ * 2) { reject(Refusal::Timeout); return true; }
        return false;
    }
    void reject(Refusal reason) noexcept {
        if (!active()) return;
        result_.reason = reason;
        phase_ = Phase::Rejected;
    }
private:
    void complete() noexcept {
        if (result_.eyeMask == 3 && result_.submitMask == 3 &&
            result_.gpuCompleteQpc && result_.submitCompleteQpc) phase_ = Phase::Complete;
    }
    Phase phase_ = Phase::Idle;
    PulseResult result_{};
    uint64_t newestRequest_ = 0, frequency_ = 0;
};

// Request rectangles use four u16 normalized edges, independently for each eye.
// Integer scaling rounds outward, then clamps only the upper edge. Reject empty
// rectangles and large readbacks before issuing any context command.
struct PixelRoi { uint32_t left = 0, top = 0, right = 0, bottom = 0; };
inline bool decodeRoi(uint64_t packed, uint32_t width, uint32_t height, PixelRoi& out) noexcept {
    const uint32_t left = uint16_t(packed), top = uint16_t(packed >> 16);
    const uint32_t right = uint16_t(packed >> 32), bottom = uint16_t(packed >> 48);
    if (!width || !height || left >= right || top >= bottom) return false;
    out = {uint32_t(uint64_t(left) * width / 65535), uint32_t(uint64_t(top) * height / 65535),
           uint32_t((uint64_t(right) * width + 65534) / 65535),
           uint32_t((uint64_t(bottom) * height + 65534) / 65535)};
    const uint64_t pixels = uint64_t(out.right - out.left) * (out.bottom - out.top);
    return out.right <= width && out.bottom <= height && pixels && pixels <= 65536;
}

// Color classification works on the copied eight-bit code values. Matching
// thresholds are request data; no game-specific coordinates enter this code.
inline bool orangeFraction(const uint8_t* bytes, size_t rowPitch, uint32_t width,
                           uint32_t height, bool bgra, uint32_t& ppm) noexcept {
    if (!bytes || !width || !height || rowPitch < uint64_t(width) * 4 ||
        uint64_t(width) * height > 65536) return false;
    uint64_t orange = 0;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = bytes + size_t(y) * rowPitch;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* pixel = row + size_t(x) * 4;
            const unsigned r = pixel[bgra ? 2 : 0], g = pixel[1], b = pixel[bgra ? 0 : 2];
            orange += r >= 96 && g >= 48 && r >= g + 16 && g >= b + 24 && b <= 96;
        }
    }
    ppm = uint32_t(orange * 1000000 / (uint64_t(width) * height));
    return true;
}

enum class OrangeState { Off, On, Ambiguous };
inline OrangeState orangeState(uint32_t ppm, uint32_t low, uint32_t high) noexcept {
    if (low >= high || high > 1000000 || ppm > 1000000) return OrangeState::Ambiguous;
    if (ppm <= low) return OrangeState::Off;
    if (ppm >= high) return OrangeState::On;
    return OrangeState::Ambiguous;
}

// Portable receipt state for one copied stereo candidate. The GPU worker may
// only add its fence timestamp; producer-thread classification supplies proof.
struct UiCandidateReceipt {
    uint64_t sequence = 0, eyeMask = 0, submitMask = 0, gpuQpc = 0, submitQpc = 0;
    bool copyEye(unsigned eye) noexcept {
        if (eye > 1 || (eyeMask & (uint64_t(1) << eye))) return false;
        eyeMask |= uint64_t(1) << eye; return true;
    }
    bool submit(unsigned eye, bool accepted, uint64_t now) noexcept {
        if (eye > 1 || !accepted || !(eyeMask & (uint64_t(1) << eye)) ||
            (submitMask & (uint64_t(1) << eye))) return false;
        submitMask |= uint64_t(1) << eye;
        if (submitMask == 3) submitQpc = now;
        return true;
    }
    bool ready() const noexcept { return sequence && eyeMask == 3 && submitMask == 3 && gpuQpc && submitQpc; }
};

inline bool uiTargetMatches(const UiCandidateReceipt& frame, uint64_t baselineSequence,
                            uint32_t leftPpm, uint32_t rightPpm, uint32_t low, uint32_t high,
                            bool targetOn) noexcept {
    const auto target = targetOn ? OrangeState::On : OrangeState::Off;
    return frame.ready() && frame.sequence > baselineSequence &&
        orangeState(leftPpm, low, high) == target && orangeState(rightPpm, low, high) == target;
}

} // namespace edvr::input_latency_probe

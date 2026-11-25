#pragma once

#include "../common/input_latency_protocol.h"
#include <array>
#include <cstdint>
#include <limits>

namespace edvr::input_latency_probe {

// One physical mouse may report the same edge through state and buffered
// reads. A device must report release before its next press can be an edge.
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
    bool consume(uint64_t now, unsigned source) noexcept {
        if (phase_ != Phase::Armed) return false;
        if (expire(now)) return false;
        if (source != 1 && source != 2) { reject(Refusal::InvalidRequest); return false; }
        result_.consumedQpc = now; result_.source = source;
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

} // namespace edvr::input_latency_probe

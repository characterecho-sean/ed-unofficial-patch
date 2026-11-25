#pragma once

#include <cstdint>
#include <array>
#include <cstddef>

namespace edvr::native_reflex {

// These tokens identify qualified app-tick invocations, not simulations, scene
// publications, rendered images, or XR frames. Callback tables and context must
// outlive the process-lifetime hooks and every in-flight callback. The same
// borrowed table brackets one tick, even if another table is published meanwhile.
// Callbacks run on their original game/worker thread, with no hook/lifecycle lock held.
struct FrameBoundaryCallbacks {
    void* context = nullptr;
    void (*beforeTick)(void*, uint64_t token) noexcept = nullptr;
    void (*afterTick)(void*, uint64_t token) noexcept = nullptr;
    void (*inputBegin)(void*, uint64_t token) noexcept = nullptr;
    void (*inputEnd)(void*, uint64_t token) noexcept = nullptr;
    // C8 includes its original tail call. Returned is deliberately not a
    // simulation-completion claim. Late callbacks carry the allocator's exact
    // tick ticket, even when a different tick is now current on another thread.
    void (*normalPhaseBegin)(void*, uint64_t token) noexcept = nullptr;
    void (*normalPhaseReturned)(void*, uint64_t token) noexcept = nullptr;
    void (*lateWorkerBegin)(void*, uint64_t token) noexcept = nullptr;
    void (*lateWorkerEnd)(void*, uint64_t token) noexcept = nullptr;
};

// One attempt, supported build 334925/callsite/receiver guarded. Scheduler,
// pose and app layouts have been verified only for that exact PE identity.
// Other builds stand down before any address resolution or hook installation.
// Forwarding hooks remain mapped
// until process exit; nullptr callbacks disable future observation, without
// joining threads or touching graphics. Installation issues no NVAPI calls.
bool installFrameBoundaryHooks() noexcept;
void setFrameBoundaryCallbacks(const FrameBoundaryCallbacks*) noexcept;
const char* frameBoundaryHookStatus() noexcept;
// Current thread only, and only while the qualified original InputManager is
// executing. Before/after callbacks, foreign calls and nested calls return 0.
// This token identifies input consumption scope, not gameplay/world output.
uint64_t currentInputFrameToken() noexcept;
// Current thread's qualified LateRender original only. This is the issuing
// tick's retained scheduler ticket, not proof of the image's input dependency.
uint64_t currentLateRenderFrameToken() noexcept;

enum FrameBoundaryFlags : uint32_t {
    TickCallQualified = 1u, InputCallQualified = 2u,
    InputCallDuplicate = 4u, InputCallReentrant = 8u, BoundaryClockInvalid = 16u,
    NormalCallQualified = 32u, NormalCallDuplicate = 64u, NormalCallReentrant = 128u
};
struct FrameBoundaryRecord {
    uint64_t token = 0;
    uint64_t beginQpc = 0, forwardQpc = 0, returnQpc = 0, endQpc = 0;
    uint64_t inputBeginQpc = 0, inputEndQpc = 0;
    uint64_t normalBeginQpc = 0, normalReturnQpc = 0;
    uint32_t thread = 0, callerRva = 0, inputThread = 0, inputCallerRva = 0, inputCalls = 0, flags = 0;
};
struct FrameBoundaryStats {
    uint64_t ticks = 0, foreignTickCalls = 0, wrongTickReceivers = 0, reentrantTicks = 0;
    uint64_t unqualifiedInputCalls = 0;
    uint64_t normalPhases = 0, lateTickets = 0, qualifiedLateWorkers = 0;
    uint64_t unqualifiedLateWorkers = 0, refusedLateTickets = 0, abandonedLateTickets = 0;
    uint64_t qpcFrequency = 0;
    bool installed = false;
};
FrameBoundaryStats frameBoundaryStats() noexcept;
// Non-destructive 256-record completion history. Independent readers own their
// cursors. Dropped counts overwritten records, never frames assigned by guess.
unsigned readFrameBoundaryRecords(uint64_t& cursor, FrameBoundaryRecord* output,
                                  unsigned capacity, uint64_t& dropped) noexcept;

namespace engine_detail {
constexpr bool supportedBuildIdentity(bool known, uint32_t stamp, uint32_t imageSize,
                                      uint32_t supportedStamp, uint32_t supportedImageSize) noexcept {
    return known && stamp == supportedStamp && imageSize == supportedImageSize;
}
constexpr uint64_t inputFrameToken(bool insideOriginal, bool suppressed, bool qualified, uint64_t token) noexcept {
    return insideOriginal && !suppressed && qualified ? token : 0;
}
// The caller provides synchronization. Numeric payload identity is copied
// before enqueue; payload bytes are never read after the scheduler recycles it.
struct JobKey {
    uintptr_t group = 0, executor = 0, payload = 0;
    uint32_t generation = 0;
};
constexpr bool sameJob(const JobKey& a, const JobKey& b) noexcept {
    return a.group == b.group && a.generation == b.generation &&
        a.executor == b.executor && a.payload == b.payload;
}
struct JobTicket {
    JobKey key{};
    uint64_t token = 0;
    uintptr_t app = 0;
    const FrameBoundaryCallbacks* observer = nullptr;
};
constexpr uint64_t ownedJobFrameToken(bool insideOriginal, bool activeHeadMatched,
                                     const JobTicket& ticket, const JobKey& current) noexcept {
    return insideOriginal && activeHeadMatched && sameJob(ticket.key, current) ? ticket.token : 0;
}
template<size_t Capacity> class TicketRegistry {
    enum class State { Free, Pending, Running, Complete };
    struct Slot { JobTicket ticket{}; State state = State::Free; bool retired = false; };
    std::array<Slot, Capacity> slots_{};
public:
    bool insert(const JobTicket& ticket) noexcept {
        if (!ticket.token || !ticket.app || !ticket.observer || !ticket.key.group ||
            !ticket.key.executor || !ticket.key.payload) return false;
        Slot* empty = nullptr;
        for (auto& slot : slots_) {
            if (slot.state == State::Free) { if (!empty) empty = &slot; continue; }
            // Live or completed reservations cannot be replaced on address
            // reuse, including a different generation on the same group.
            if (slot.ticket.token == ticket.token || slot.ticket.key.group == ticket.key.group ||
                slot.ticket.key.payload == ticket.key.payload) return false;
        }
        if (!empty) return false;
        empty->ticket = ticket; empty->state = State::Pending; empty->retired = false; return true;
    }
    bool claim(const JobKey& key, uintptr_t app, JobTicket& output) noexcept {
        output = {};
        for (auto& slot : slots_) if (slot.state == State::Pending && !slot.retired &&
            slot.ticket.app == app && sameJob(slot.ticket.key, key)) {
            slot.state = State::Running; output = slot.ticket; return true;
        }
        return false;
    }
    bool finish(const JobTicket& ticket) noexcept {
        for (auto& slot : slots_) if (slot.state == State::Running && slot.ticket.token == ticket.token &&
            sameJob(slot.ticket.key, ticket.key)) {
            if (slot.retired) slot = {};
            else slot.state = State::Complete;
            return true;
        }
        return false;
    }
    unsigned retire(uint64_t token) noexcept {
        unsigned abandoned = 0;
        for (auto& slot : slots_) if (slot.state != State::Free && slot.ticket.token == token) {
            if (slot.state == State::Running) { slot.retired = true; ++abandoned; }
            else { if (slot.state == State::Pending) ++abandoned; slot = {}; }
        }
        return abandoned;
    }
};
// Production forwarding gate, also exercised with fake callbacks in the
// portable rig. Rejected/nested calls always execute original exactly once.
template<class Before, class Original, class After>
bool forwardBoundary(bool callerQualified, bool receiverQualified, bool& entered,
                     Before before, Original original, After after) {
    if (!callerQualified || !receiverQualified || entered) { original(); return false; }
    struct Restore {
        bool& entered;
        ~Restore() { entered = false; }
    } restore{entered};
    entered = true;
    before();
    original();
    after();
    return true;
}
} // namespace engine_detail
} // namespace edvr::native_reflex

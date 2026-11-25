// Resolved app-tick/input boundaries and retained LateRender tickets. A tick
// token is a Sleep iteration identity; scheduler ownership alone does not
// establish the scene or rendered image's input dependency.
#include "native_reflex_engine_hook.h"
#include "../common/code_hook.h"
#include "../common/game_build.h"
#include "../common/log.h"

#include <windows.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>

namespace edvr::native_reflex {
namespace {
constexpr uint32_t kSupportedStamp = 0x6ADA8048, kSupportedImageSize = 0x640C000;
constexpr uint32_t kTickRva = 0x7F33D0, kInputRva = 0x8F0BA0;
constexpr uint32_t kAppVtable = 0x4DDF490, kInputVtable = 0x512FD90;
constexpr uint32_t kTickReturnA = 0x7F95D8, kTickReturnB = 0x7F96DF, kInputReturn = 0x1EB2FC;
constexpr uint32_t kNormalRva = 0x1EB050, kAllocateRva = 0x5D3EA0, kLateRva = 0x7FDCB0;
constexpr uint32_t kNormalReturn = 0x7F3962, kAllocateReturn = 0x7F3882, kWorkerReturn = 0x5D6B81;
constexpr uint32_t kGameTlsIndex = 0x60484E8;
constexpr unsigned kCapacity = 256;
using PhaseFunction = void(*)(uintptr_t, float);
using AllocateFunction = void*(*)(uintptr_t, uintptr_t, size_t, uint32_t);
using WorkerFunction = void(*)(uintptr_t);
std::atomic<void*> tickOriginal{nullptr}, inputOriginal{nullptr};
std::atomic<void*> normalOriginal{nullptr}, allocateOriginal{nullptr}, lateOriginal{nullptr};
std::atomic<const FrameBoundaryCallbacks*> callbacks{nullptr};
std::atomic<uintptr_t> imageBase{0};
uint32_t imageSpan = 0;
struct Addresses {
    uintptr_t tick = 0, input = 0, tickReturnA = 0, tickReturnB = 0, inputReturn = 0;
    uintptr_t appVtable = 0, inputVtable = 0;
    uintptr_t normal = 0, allocation = 0, late = 0, normalReturn = 0, allocateReturn = 0, workerReturn = 0, tlsIndex = 0;
} addresses; // resolved before installed's release store; immutable thereafter
std::atomic<bool> installed{false};
std::atomic<const char*> status{"not_attempted"};
std::atomic<uint64_t> nextToken{0}, tickCount{0}, foreignTicks{0}, wrongReceivers{0}, reentrantTicks{0}, foreignInputs{0};
std::atomic<uint64_t> normalCount{0}, ticketCount{0}, lateCount{0}, foreignLate{0}, refusedTickets{0}, abandonedTickets{0};
uint64_t frequency = 0; // published by installed's release store
SRWLOCK installLock = SRWLOCK_INIT, historyLock = SRWLOCK_INIT, ticketLock = SRWLOCK_INIT;
engine_detail::TicketRegistry<64> tickets;
bool attempted = false; // installation lock only; never held on game callbacks
struct Completion { uint64_t ordinal = 0; FrameBoundaryRecord record{}; };
std::array<Completion, kCapacity> history{};
uint64_t nextCompletion = 1;
struct TickScope {
    FrameBoundaryRecord record{};
    const FrameBoundaryCallbacks* observer = nullptr;
    uintptr_t app = 0;
    bool inputEntered = false, normalEntered = false, normalObserved = false, allocationObserved = false;
};
thread_local TickScope scope;
thread_local bool tickEntered = false;
thread_local unsigned inputSuppression = 0;
thread_local bool inputOriginalActive = false;
thread_local bool lateEntered = false, lateOriginalActive = false;
thread_local unsigned lateSuppression = 0;
thread_local engine_detail::JobTicket lateScope;

void forwardUnqualifiedPhase(PhaseFunction original, uintptr_t object, float delta) {
    struct Suppress {
        Suppress() { ++inputSuppression; }
        ~Suppress() { --inputSuppression; }
    } suppress;
    original(object, delta);
}

uint64_t qpcNow() noexcept {
    LARGE_INTEGER now{};
    return QueryPerformanceCounter(&now) && now.QuadPart > 0 ? uint64_t(now.QuadPart) : 0;
}
uint32_t origin(const void* caller) noexcept {
    const uintptr_t base = imageBase.load(std::memory_order_acquire), address = reinterpret_cast<uintptr_t>(caller);
    return base && address >= base && address - base < imageSpan ? uint32_t(address - base) : 0;
}
// Only guarded reads of the original game's receiver and pinned image bytes.
bool readBytes(uintptr_t address, void* output, size_t size) noexcept {
    __try { if (!address) return false; std::memcpy(output, reinterpret_cast<const void*>(address), size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool receiver(uintptr_t object, uintptr_t expected) noexcept {
    uintptr_t actual = 0;
    return expected && readBytes(object, &actual, sizeof(actual)) && actual == expected;
}
template<class T> bool readField(uintptr_t address, T& output) noexcept {
    return readBytes(address, &output, sizeof(output));
}
// This thread's list is scheduler-owned and the group is retained until its
// callback returns. Verify every link, bound traversal and refuse ambiguity.
bool activeLateJob(uintptr_t app, engine_detail::JobKey& output) noexcept {
    uint32_t index = 0;
    uint8_t initialized = 0;
    uintptr_t block = 0, thread = 0, node = 0;
    const uintptr_t vector = uintptr_t(__readgsqword(0x58));
    if (!readField(addresses.tlsIndex, index) || !vector || index > 65535 ||
        !readField(vector + sizeof(void*) * index, block) || !block ||
        !readField(block + 0x44BC, initialized) || !initialized ||
        !readField(block + 0x4480, thread) || !thread || !readField(thread + 0x90, node)) return false;
    uintptr_t previous = thread + 0x90;
    unsigned matches = 0;
    for (unsigned depth = 0; node != thread + 0xA8; ++depth) {
        uintptr_t next = 0, back = 0;
        uint16_t kind = 0;
        uint64_t bytes = 0;
        engine_detail::JobKey key{};
        if (depth == 8 || !node || !readField(node, next) || !readField(node + 8, back) || back != previous ||
            !readField(node + 0x18, kind) || !readField(node + 0x20, key.executor) ||
            !readField(node + 0x28, bytes) || !readField(node + 0x30, key.group) || !key.group ||
            !readField(key.group + 0x30, key.generation) || !readField(node + 0x40, key.payload)) return false;
        if (!kind && bytes == 16 && key.executor == app + 0x58) {
            // Nested stolen work belongs to the head job, even while the
            // qualified LateRender original remains further up the stack.
            if (depth != 0) return false;
            output = key; ++matches;
        }
        previous = node; node = next;
    }
    uintptr_t back = 0;
    return readField(node + 8, back) && back == previous && matches == 1;
}
uint64_t allocateToken() noexcept {
    uint64_t prior = nextToken.load(std::memory_order_relaxed);
    while (prior != (std::numeric_limits<uint64_t>::max)()) {
        if (nextToken.compare_exchange_weak(prior, prior + 1, std::memory_order_relaxed)) return prior + 1;
    }
    return 0; // never wrap an invocation identity back to an earlier token
}
void publish(FrameBoundaryRecord record) noexcept {
    if (!record.beginQpc || record.forwardQpc < record.beginQpc || record.returnQpc < record.forwardQpc ||
        record.endQpc < record.returnQpc || ((record.flags & InputCallQualified) &&
        (!record.inputBeginQpc || record.inputBeginQpc < record.forwardQpc || record.inputEndQpc < record.inputBeginQpc ||
         record.inputEndQpc > record.returnQpc || record.inputThread != record.thread)) ||
        ((record.flags & NormalCallQualified) && (!record.normalBeginQpc || record.normalBeginQpc < record.forwardQpc ||
          record.normalReturnQpc < record.normalBeginQpc || record.normalReturnQpc > record.returnQpc)))
        record.flags |= BoundaryClockInvalid;
    AcquireSRWLockExclusive(&historyLock);
    const uint64_t ordinal = nextCompletion++;
    history[ordinal % kCapacity] = {ordinal, record};
    ReleaseSRWLockExclusive(&historyLock);
}

void hookedTick(uintptr_t app, float delta) {
    const auto original = reinterpret_cast<PhaseFunction>(tickOriginal.load(std::memory_order_acquire));
    const auto* observer = callbacks.load(std::memory_order_acquire);
    if (!installed.load(std::memory_order_acquire) || !observer) { forwardUnqualifiedPhase(original, app, delta); return; }
    const uint32_t caller = origin(_ReturnAddress());
    const bool callerOk = caller == addresses.tickReturnA || caller == addresses.tickReturnB;
    const bool receiverOk = callerOk && receiver(app, addresses.appVtable);
    const bool qualifies = callerOk && receiverOk && !tickEntered;
    if (!callerOk) foreignTicks.fetch_add(1, std::memory_order_relaxed);
    else if (!receiverOk) wrongReceivers.fetch_add(1, std::memory_order_relaxed);
    else if (tickEntered) reentrantTicks.fetch_add(1, std::memory_order_relaxed);
    engine_detail::forwardBoundary(callerOk, receiverOk, tickEntered,
        [&] {
            scope = {};
            scope.observer = observer;
            scope.app = app;
            auto& record = scope.record;
            record.token = allocateToken(); record.thread = GetCurrentThreadId(); record.callerRva = caller;
            record.flags = TickCallQualified; record.beginQpc = qpcNow();
            tickCount.fetch_add(1, std::memory_order_relaxed);
            // Before the original app entry: before its epoch increment,
            // early-worker launch, and later InputManager poll. No lock held.
            if (record.token && observer->beforeTick) observer->beforeTick(observer->context, record.token);
            record.forwardQpc = qpcNow();
        },
        [&] {
            struct Suppress {
                bool active;
                explicit Suppress(bool value) : active(value) { if (active) ++inputSuppression; }
                ~Suppress() { if (active) --inputSuppression; }
            } suppress(!qualifies);
            original(app, delta);
        },
        [&] {
            scope.record.returnQpc = qpcNow();
            if (scope.record.token && observer->afterTick) observer->afterTick(observer->context, scope.record.token);
            scope.record.endQpc = qpcNow();
            AcquireSRWLockExclusive(&ticketLock);
            const unsigned abandoned = tickets.retire(scope.record.token);
            ReleaseSRWLockExclusive(&ticketLock);
            abandonedTickets.fetch_add(abandoned, std::memory_order_relaxed);
            publish(scope.record);
            scope = {};
        });
}

void hookedNormal(uintptr_t app, float delta) {
    const auto original = reinterpret_cast<PhaseFunction>(normalOriginal.load(std::memory_order_acquire));
    const bool qualified = !inputSuppression && tickEntered && scope.observer && scope.record.token &&
        scope.app == app && origin(_ReturnAddress()) == addresses.normalReturn &&
        receiver(app, addresses.appVtable) && GetCurrentThreadId() == scope.record.thread;
    if (!qualified) { forwardUnqualifiedPhase(original, app, delta); return; }
    if (scope.normalEntered) { scope.record.flags |= NormalCallReentrant; forwardUnqualifiedPhase(original, app, delta); return; }
    if (scope.normalObserved) { scope.record.flags |= NormalCallDuplicate; forwardUnqualifiedPhase(original, app, delta); return; }
    scope.normalObserved = true;
    const auto* observer = scope.observer;
    engine_detail::forwardBoundary(true, true, scope.normalEntered,
        [&] {
            scope.record.flags |= NormalCallQualified;
            scope.record.normalBeginQpc = qpcNow(); normalCount.fetch_add(1, std::memory_order_relaxed);
            if (observer->normalPhaseBegin) observer->normalPhaseBegin(observer->context, scope.record.token);
        }, [&] { original(app, delta); }, [&] {
            scope.record.normalReturnQpc = qpcNow();
            if (observer->normalPhaseReturned) observer->normalPhaseReturned(observer->context, scope.record.token);
        });
}

void* hookedAllocate(uintptr_t scheduler, uintptr_t executor, size_t size, uint32_t flags) {
    const auto original = reinterpret_cast<AllocateFunction>(allocateOriginal.load(std::memory_order_acquire));
    void* const payload = original(scheduler, executor, size, flags);
    // This hook returns to the allocation call before payload initialization,
    // close and enqueue. Registration therefore precedes worker visibility.
    if (!installed.load(std::memory_order_acquire) || inputSuppression || !tickEntered || !scope.observer ||
        !scope.record.token || !payload || origin(_ReturnAddress()) != addresses.allocateReturn ||
        executor != scope.app + 0x58 || size != 16 || flags || GetCurrentThreadId() != scope.record.thread) return payload;
    if (scope.allocationObserved) { refusedTickets.fetch_add(1, std::memory_order_relaxed); return payload; }
    scope.allocationObserved = true;
    engine_detail::JobTicket ticket{};
    ticket.token = scope.record.token; ticket.app = scope.app; ticket.observer = scope.observer;
    ticket.key.executor = executor; ticket.key.payload = reinterpret_cast<uintptr_t>(payload);
    if (ticket.key.payload < sizeof(void*) ||
        !readField(ticket.key.payload - sizeof(void*), ticket.key.group) || !ticket.key.group ||
        !readField(ticket.key.group + 0x30, ticket.key.generation)) {
        refusedTickets.fetch_add(1, std::memory_order_relaxed); return payload;
    }
    AcquireSRWLockExclusive(&ticketLock);
    const bool accepted = tickets.insert(ticket);
    ReleaseSRWLockExclusive(&ticketLock);
    (accepted ? ticketCount : refusedTickets).fetch_add(1, std::memory_order_relaxed);
    return payload;
}

void forwardLate(WorkerFunction original, uintptr_t app, bool qualified) {
    struct Restore {
        bool previous, suppressed;
        explicit Restore(bool accepted) : previous(lateOriginalActive), suppressed(!accepted) {
            lateOriginalActive = accepted;
            if (suppressed) ++lateSuppression;
        }
        ~Restore() { if (suppressed) --lateSuppression; lateOriginalActive = previous; }
    } restore(qualified);
    original(app);
}
void hookedLate(uintptr_t app) {
    const auto original = reinterpret_cast<WorkerFunction>(lateOriginal.load(std::memory_order_acquire));
    engine_detail::JobKey key{};
    engine_detail::JobTicket ticket{};
    bool qualified = installed.load(std::memory_order_acquire) && !lateEntered && !lateSuppression &&
        origin(_ReturnAddress()) == addresses.workerReturn && receiver(app, addresses.appVtable) && activeLateJob(app, key);
    if (qualified) {
        AcquireSRWLockExclusive(&ticketLock);
        qualified = tickets.claim(key, app, ticket);
        ReleaseSRWLockExclusive(&ticketLock);
    }
    if (!qualified) {
        foreignLate.fetch_add(1, std::memory_order_relaxed); forwardLate(original, app, false); return;
    }
    lateCount.fetch_add(1, std::memory_order_relaxed);
    lateScope = ticket;
    struct Finish {
        engine_detail::JobTicket ticket;
        ~Finish() {
            AcquireSRWLockExclusive(&ticketLock);
            tickets.finish(ticket);
            ReleaseSRWLockExclusive(&ticketLock);
            lateScope = {};
        }
    } finish{ticket};
    const auto* observer = ticket.observer;
    engine_detail::forwardBoundary(true, true, lateEntered,
        [&] { if (observer->lateWorkerBegin) observer->lateWorkerBegin(observer->context, ticket.token); },
        [&] { forwardLate(original, app, true); },
        [&] { if (observer->lateWorkerEnd) observer->lateWorkerEnd(observer->context, ticket.token); });
}

void forwardInput(PhaseFunction original, uintptr_t manager, float delta, bool qualified) {
    struct Restore {
        bool previous;
        bool suppressed;
        explicit Restore(bool accepted) : previous(inputOriginalActive), suppressed(!accepted) {
            inputOriginalActive = accepted;
            if (suppressed) ++inputSuppression;
        }
        ~Restore() {
            if (suppressed) --inputSuppression;
            inputOriginalActive = previous;
        }
    } restore(qualified);
    original(manager, delta);
}
void hookedInput(uintptr_t manager, float delta) {
    const auto original = reinterpret_cast<PhaseFunction>(inputOriginal.load(std::memory_order_acquire));
    const bool qualified = !inputSuppression && tickEntered && scope.observer && scope.record.token &&
        origin(_ReturnAddress()) == addresses.inputReturn && receiver(manager, addresses.inputVtable) &&
        GetCurrentThreadId() == scope.record.thread;
    if (!qualified) { foreignInputs.fetch_add(1, std::memory_order_relaxed); forwardInput(original, manager, delta, false); return; }
    if (scope.inputEntered) {
        scope.record.flags |= InputCallReentrant;
        forwardInput(original, manager, delta, false); return;
    }
    if (++scope.record.inputCalls != 1) {
        scope.record.flags |= InputCallDuplicate;
        forwardInput(original, manager, delta, false); return;
    }
    const auto* observer = scope.observer;
    engine_detail::forwardBoundary(true, true, scope.inputEntered,
        [&] {
            scope.record.flags |= InputCallQualified;
            scope.record.inputThread = GetCurrentThreadId(); scope.record.inputCallerRva = uint32_t(addresses.inputReturn);
            scope.record.inputBeginQpc = qpcNow();
            if (observer->inputBegin) observer->inputBegin(observer->context, scope.record.token);
        },
        [&] { forwardInput(original, manager, delta, true); },
        [&] {
            scope.record.inputEndQpc = qpcNow();
            if (observer->inputEnd) observer->inputEnd(observer->context, scope.record.token);
        });
}

bool prepareOriginal(void* trampoline, void* context) noexcept {
    static_cast<std::atomic<void*>*>(context)->store(trampoline, std::memory_order_release);
    return true;
}
// Same VirtualQuery/free-region search as the existing net_keepalive and
// object-record writer relays. CodeHook owns prologue relocation; this page
// only bridges the rel32 entry patch to a possibly distant DLL callback.
uint8_t* reserveEntryRelay(uintptr_t target) noexcept {
    SYSTEM_INFO info{};GetSystemInfo(&info);
    const uintptr_t granularity=info.dwAllocationGranularity;
    const uintptr_t floor=reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t ceiling=reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    if (!granularity || (granularity&(granularity-1)) || floor>=ceiling || target<floor || target>ceiling) return nullptr;
    const uintptr_t distance=uintptr_t(INT32_MAX)-0x10000u;
    uintptr_t at=target-floor>distance?target-distance:floor;
    const uintptr_t limit=ceiling-target>distance?target+distance:ceiling;
    while (at<limit) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(at),&region,sizeof(region))) break;
        const uintptr_t start=reinterpret_cast<uintptr_t>(region.BaseAddress);
        if (region.RegionSize>UINTPTR_MAX-start) break;
        const uintptr_t end=start+region.RegionSize;
        if (region.State==MEM_FREE) {
            uintptr_t candidate=at>start?at:start;
            if (candidate>UINTPTR_MAX-(granularity-1)) break;
            candidate=(candidate+granularity-1)&~(granularity-1);
            if (candidate<limit && candidate<end && end-candidate>=4096) {
                auto* page=static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate),4096,
                    MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
                if (page) return page;
            }
        }
        if (end<=at) break;
        at=end;
    }
    return nullptr;
}
bool verifyPins(uintptr_t base) noexcept {
    struct Pin { uint32_t rva; const char* hex; };
    // Known-build pins compare exactly; gamePinMatches masks only fields
    // already proven by whole-function resolution on another build. These verify identity and
    // origins; CodeHook alone determines safe instruction relocation lengths.
    constexpr Pin pins[] = {
        {kTickRva, "488bc45553565741564157488d68a148"},
        {kInputRva, "4c8bdc554154498dab28feffff4881ec"},
        {kNormalRva, "48895c2410488974241848897c242055"},
        {kAllocateRva, "48895c240848896c2410488974241848"},
        {kLateRva, "40534883ec30f30f1089100400000f57"},
        {0x7F95D3, "e8f89dffff"}, {0x7F96DA, "e8f19cffff"},
        {0x7F340A, "e8512fdeff"}, {0x7F395C, "ff90c8000000"}, {0x1EB2F9, "ff5018"},
        {0x7F386D, "488d053ca40000"}, {0x7F387D, "e81e06deff"},
        {0x5D41EA, "65488b042558000000"}, {0x5D3D96, "895930"},
        {0x5D6BBD, "488b4d188b4510418987"}, {0x5D6B7E, "ff5008"},
        {0x5D570F, "488d5308"}, {0x5D3E7F, "48894218"}, {0x5D3E86, "48897a08"},
        {0x5D6B8D, "488b4d48e81ae4ffff"}
    };
    for (const auto& pin : pins) {
        uint8_t bytes[16]{}, actual[16]{}; size_t length = 0;
        for (const char* hex = pin.hex; *hex; hex += 2) {
            const auto digit = [](char c) { return c <= '9' ? unsigned(c - '0') : unsigned(c - 'a' + 10); };
            if (length == sizeof(bytes)) return false;
            bytes[length++] = uint8_t((digit(hex[0]) << 4) | digit(hex[1]));
        }
        const uintptr_t resolved = gameRva(pin.rva);
        if (!resolved || !readBytes(base + resolved, actual, length) ||
            !gamePinMatches(pin.rva, actual, bytes, length)) return false;
    }
    return true;
}
bool installOnce() noexcept {
    uint32_t stamp = 0;
    if (!gameBuildKnown() || !gameBuildIdentity(&stamp, &imageSpan) ||
        !engine_detail::supportedBuildIdentity(true, stamp, imageSpan, kSupportedStamp, kSupportedImageSize)) {
        status.store("unsupported_build_requires_334925", std::memory_order_release); return false;
    }
    constexpr uint32_t required[] = {kTickRva, kInputRva, kAppVtable, kInputVtable,
        kTickReturnA, kTickReturnB, kInputReturn,
        0x7F95D3, 0x7F96DA, 0x7F340A, 0x7F395C, 0x1EB2F9,
        kNormalRva, kAllocateRva, kLateRva, kNormalReturn, kAllocateReturn, kWorkerReturn, kGameTlsIndex,
        0x7F386D, 0x7F387D, 0x5D41EA, 0x5D3D96, 0x5D6BBD, 0x5D6B7E,
        0x5D570F, 0x5D3E7F, 0x5D3E86, 0x5D6B8D};
    if (!gameRvasFound(required, sizeof(required) / sizeof(*required), nullptr)) {
        status.store("required_game_address_unresolved", std::memory_order_release); return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base || !verifyPins(base)) { status.store("identity_pin_mismatch", std::memory_order_release); return false; }
    addresses.tick = gameRva(kTickRva); addresses.input = gameRva(kInputRva);
    addresses.tickReturnA = gameRva(kTickReturnA); addresses.tickReturnB = gameRva(kTickReturnB);
    addresses.inputReturn = gameRva(kInputReturn);
    addresses.normal = gameRva(kNormalRva); addresses.allocation = gameRva(kAllocateRva); addresses.late = gameRva(kLateRva);
    addresses.normalReturn = gameRva(kNormalReturn); addresses.allocateReturn = gameRva(kAllocateReturn);
    addresses.workerReturn = gameRva(kWorkerReturn);
    const uintptr_t tlsIndex = gameRva(kGameTlsIndex);
    const uintptr_t appTable = gameRva(kAppVtable), inputTable = gameRva(kInputVtable);
    if (!addresses.tick || !addresses.input || !addresses.tickReturnA || !addresses.tickReturnB ||
        !addresses.inputReturn || !appTable || !inputTable || !addresses.normal || !addresses.allocation ||
        !addresses.late || !addresses.normalReturn || !addresses.allocateReturn || !addresses.workerReturn || !tlsIndex) {
        status.store("required_game_address_unresolved", std::memory_order_release); return false;
    }
    addresses.appVtable = base + appTable; addresses.inputVtable = base + inputTable;
    addresses.tlsIndex = base + tlsIndex;
    LARGE_INTEGER clock{};
    if (!QueryPerformanceFrequency(&clock) || clock.QuadPart <= 0) {
        status.store("qpc_unavailable", std::memory_order_release); return false;
    }
    frequency = uint64_t(clock.QuadPart);
    imageBase.store(base, std::memory_order_release);
    HMODULE retained = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&hookedTick), &retained)) {
        status.store("module_pin_failed", std::memory_order_release); return false;
    }
    struct Target { uintptr_t address; void* replacement; std::atomic<void*>* original; const char* name; };
    const Target targets[] = {
        {base + addresses.tick, reinterpret_cast<void*>(&hookedTick), &tickOriginal, "Reflex app tick boundary"},
        {base + addresses.input, reinterpret_cast<void*>(&hookedInput), &inputOriginal, "Reflex InputManager boundary"},
        {base + addresses.normal, reinterpret_cast<void*>(&hookedNormal), &normalOriginal, "Reflex normal C8 boundary"},
        {base + addresses.allocation, reinterpret_cast<void*>(&hookedAllocate), &allocateOriginal, "Reflex LateRender reservation"},
        {base + addresses.late, reinterpret_cast<void*>(&hookedLate), &lateOriginal, "Reflex retained LateRender boundary"}
    };
    // Installed forwarding hooks intentionally have process lifetime, including
    // a partial failure. Never destruct an entry trampoline another thread uses.
    for (const auto& target : targets) {
        auto* hook = new (std::nothrow) CodeHook;
        if (!hook) { status.store("allocation_failed", std::memory_order_release); return false; }
        void* unused = nullptr;
        const auto result=engine_detail::installEntryRelay(target.address,reinterpret_cast<uintptr_t>(target.replacement),
            reserveEntryRelay,
            [](uint8_t* page,size_t bytes) {
                DWORD oldProtect=0;
                return VirtualProtect(page,4096,PAGE_EXECUTE_READ,&oldProtect) &&
                    FlushInstructionCache(GetCurrentProcess(),page,bytes);
            },
            [&](void* relay) { return hook->install(reinterpret_cast<void*>(target.address),relay,&unused,
                target.name,prepareOriginal,target.original); },
            [](uint8_t* page) { VirtualFree(page,0,MEM_RELEASE); });
        if (result!=engine_detail::EntryInstallResult::Installed) {
            delete hook;
            const char* reason=result==engine_detail::EntryInstallResult::RelayAllocationFailed?"entry_relay_allocation_failed":
                result==engine_detail::EntryInstallResult::RelayOutOfRange?"entry_relay_out_of_range":
                result==engine_detail::EntryInstallResult::RelayProtectionFailed?"entry_relay_protection_failed":"entry_hook_refused";
            status.store(reason,std::memory_order_release);return false;
        }
    }
    installed.store(true, std::memory_order_release);
    status.store("installed_build_334925", std::memory_order_release);
    return true;
}
} // namespace

bool installFrameBoundaryHooks() noexcept {
    AcquireSRWLockExclusive(&installLock);
    if (!attempted) {
        attempted = true;
        const bool ok = installOnce();
        Log::get().note("Reflex engine boundary: %s; build 334925 only; app tick and retained LateRender job identities.",
            ok ? "installed" : status.load(std::memory_order_acquire));
    }
    const bool ok = installed.load(std::memory_order_acquire);
    ReleaseSRWLockExclusive(&installLock);
    return ok;
}
void setFrameBoundaryCallbacks(const FrameBoundaryCallbacks* table) noexcept { callbacks.store(table, std::memory_order_release); }
const char* frameBoundaryHookStatus() noexcept { return status.load(std::memory_order_acquire); }
uint64_t currentInputFrameToken() noexcept {
    const bool qualified = tickEntered && scope.inputEntered && scope.observer &&
        (scope.record.flags & InputCallQualified) &&
        !(scope.record.flags & (InputCallDuplicate | InputCallReentrant));
    return engine_detail::inputFrameToken(inputOriginalActive, inputSuppression != 0, qualified, scope.record.token);
}
uint64_t currentLateRenderFrameToken() noexcept {
    const uint64_t token = engine_detail::inputFrameToken(lateOriginalActive, lateSuppression != 0,
        lateEntered && lateScope.observer, lateScope.token);
    if (!token) return 0;
    engine_detail::JobKey current{};
    const bool matched = activeLateJob(lateScope.app, current);
    return engine_detail::ownedJobFrameToken(true, matched, lateScope, current);
}
FrameBoundaryStats frameBoundaryStats() noexcept {
    FrameBoundaryStats result{};
    result.installed = installed.load(std::memory_order_acquire);
    result.ticks = tickCount.load(std::memory_order_relaxed);
    result.foreignTickCalls = foreignTicks.load(std::memory_order_relaxed);
    result.wrongTickReceivers = wrongReceivers.load(std::memory_order_relaxed);
    result.reentrantTicks = reentrantTicks.load(std::memory_order_relaxed);
    result.unqualifiedInputCalls = foreignInputs.load(std::memory_order_relaxed);
    result.normalPhases = normalCount.load(std::memory_order_relaxed);
    result.lateTickets = ticketCount.load(std::memory_order_relaxed);
    result.qualifiedLateWorkers = lateCount.load(std::memory_order_relaxed);
    result.unqualifiedLateWorkers = foreignLate.load(std::memory_order_relaxed);
    result.refusedLateTickets = refusedTickets.load(std::memory_order_relaxed);
    result.abandonedLateTickets = abandonedTickets.load(std::memory_order_relaxed);
    result.qpcFrequency = result.installed ? frequency : 0;
    return result;
}
unsigned readFrameBoundaryRecords(uint64_t& cursor, FrameBoundaryRecord* output, unsigned capacity, uint64_t& dropped) noexcept {
    if (!output || !capacity) return 0;
    AcquireSRWLockShared(&historyLock);
    const uint64_t latest = nextCompletion - 1;
    const uint64_t oldest = latest >= kCapacity ? latest - kCapacity + 1 : 1;
    if (cursor < oldest - 1) { dropped += oldest - 1 - cursor; cursor = oldest - 1; }
    unsigned count = 0;
    while (count < capacity && cursor < latest) {
        const auto& item = history[(cursor + 1) % kCapacity];
        ++cursor;
        if (item.ordinal != cursor) { ++dropped; continue; }
        output[count++] = item.record;
    }
    ReleaseSRWLockShared(&historyLock);
    return count;
}
} // namespace edvr::native_reflex

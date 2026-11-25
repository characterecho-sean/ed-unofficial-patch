#include "native_reflex_session.h"
#include "native_reflex.h"
#include "native_reflex_command.h"
#include "native_reflex_engine_hook.h"
#include "native_reflex_pcl.h"
#include "input_gate.h"
#include "../common/log.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>

namespace edvr::native_reflex {
namespace {
constexpr size_t kFrameHistory = 128, kProviderContexts = 64;
constexpr unsigned kAllFrameMarkers = 0x3f;
struct FrameRecord {
    uint64_t token = 0, poseSequence = 0;
    uint64_t poseConsumedQpc = 0, firstCommandQpc = 0, lateEndQpc = 0;
    uint64_t mirrorCompleteQpc = 0, presentBeginQpc = 0, presentEndQpc = 0;
    DWORD simulationThread = 0, poseThread = 0, commandThread = 0, presentThread = 0;
    unsigned reservedMarkers = 0, completedMarkers = 0, eyes = 0, acceptedEyes = 0;
    unsigned provider = 0;
    bool invalid = false, driverFailed = false, lateBegun = false;
    bool lateEnded = false, mirrorDrawComplete = false, completed = false;
    bool softwareReported = false, driverReported = false;
};
}

struct FrameSession {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    uint64_t generation = 0;
    DWORD producerThread = 0;
    std::atomic<DWORD> simulationOwner{0};
    std::shared_ptr<FrameDriver> driver;
    std::atomic<bool> active{true};
    std::atomic<bool> providerAcquired{false};
    std::atomic<bool> leftButtonDown{false}, pendingFlash{false};
    std::mutex recordsMutex;
    std::array<FrameRecord, kFrameHistory> frames{};
    uint64_t newestToken = 0, lastPresentedToken = 0;
    std::atomic<uint64_t> sleepCalls{0}, sleepAccepted{0}, sleepTicks{0};
    std::atomic<uint64_t> inputCalls{0}, completedTicks{0}, completedFrames{0};
    std::atomic<uint64_t> lastCompletedMs{0};
    std::atomic<uint64_t> missingBoundaries{0}, overwrittenFrames{0}, refusedSources{0};
    std::atomic<uint64_t> duplicatePresents{0}, markerFailures{0};
    std::atomic<uint64_t> foreignSimulationTicks{0};
    std::atomic<uint64_t> unconfiguredTicks{0}, softwareDropped{0}, driverFrameGaps{0};
    std::atomic<uint64_t> lastSoftwareReported{0};
    uint64_t lastPollMs = 0, lastSummaryMs = 0, lastDriverReported = 0;
};

namespace {
// Normal lifecycle owns this handle. No process-exit destructor releases a
// device or waits for a worker under the loader lock.
std::shared_ptr<FrameSession>& published() {
    static auto* value = new std::shared_ptr<FrameSession>;
    return *value;
}
std::shared_ptr<FrameSession> activeSession() noexcept {
    auto session = std::atomic_load_explicit(&published(), std::memory_order_acquire);
    return session && session->active.load(std::memory_order_acquire) ? session : nullptr;
}
std::mutex capabilityMutex;
void faultSession(const std::shared_ptr<FrameSession>& session) noexcept {
    std::lock_guard<std::mutex> lock(capabilityMutex);
    if (!session->active.load(std::memory_order_acquire) ||
        std::atomic_load_explicit(&published(), std::memory_order_acquire) != session) return;
    fullFrameAvailable.store(false, std::memory_order_release);
    fullFrameFaulted.store(true, std::memory_order_release);
}
struct TickState { uint64_t token = 0, generation = 0; };
struct LateState { uint64_t token = 0, generation = 0; };
// POD-only thread-local ownership. Strong references live on callback stacks,
// so thread/process teardown never destroys an owning COM handle through TLS.
thread_local TickState tick;
thread_local LateState late;

uint64_t qpc() noexcept {
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value) && value.QuadPart > 0 ? uint64_t(value.QuadPart) : 0;
}
FrameRecord* findFrame(FrameSession& session, uint64_t token) noexcept {
    if (!token) return nullptr;
    auto& frame = session.frames[token % session.frames.size()];
    return frame.token == token ? &frame : nullptr;
}
bool reserveMarker(FrameRecord& frame, Marker marker) noexcept {
    const unsigned bit = 1u << unsigned(marker);
    if (frame.invalid || (frame.reservedMarkers & bit)) return false;
    frame.reservedMarkers |= bit;
    return true;
}
void completeRecord(FrameSession& session, FrameRecord& frame) noexcept {
    if (!frame.completed && !frame.invalid && !frame.driverFailed &&
        (frame.completedMarkers & kAllFrameMarkers) == kAllFrameMarkers &&
        frame.eyes == 3 && frame.acceptedEyes == 3 && frame.poseSequence) {
        frame.completed = true;
        session.completedFrames.fetch_add(1, std::memory_order_relaxed);
        session.lastCompletedMs.store(GetTickCount64(), std::memory_order_release);
    }
}
// Marker calls are made after releasing record locks. Independent simulation,
// render and Present owners may overlap without serializing a driver's wait.
void emit(const std::shared_ptr<FrameSession>& session, uint64_t token, Marker marker) noexcept {
    if (!session || !session->active.load(std::memory_order_acquire)) return;
    const bool accepted = !session->driver || session->driver->mark(token, marker);
    pcl::mark(uint32_t(marker), token);
    if (!accepted) session->markerFailures.fetch_add(1, std::memory_order_relaxed);
    if (!accepted) {
        faultSession(session);
    }
    std::lock_guard<std::mutex> lock(session->recordsMutex);
    if (auto* frame = findFrame(*session, token)) {
        frame->completedMarkers |= 1u << unsigned(marker);
        frame->driverFailed = frame->driverFailed || !accepted;
        completeRecord(*session, *frame);
    }
}
bool reserveRenderEnd(FrameRecord& frame) noexcept {
    return frame.lateEnded && frame.mirrorDrawComplete &&
        (frame.completedMarkers & (1u << unsigned(Marker::RenderSubmitStart))) &&
        reserveMarker(frame, Marker::RenderSubmitEnd);
}

void beforeTick(void*, uint64_t token) noexcept {
    tick = {};
    auto session = activeSession();
    if (!session || !token) return;
    DWORD expectedOwner = 0;
    const DWORD caller = GetCurrentThreadId();
    session->simulationOwner.compare_exchange_strong(expectedOwner, caller, std::memory_order_acq_rel);
    if (session->simulationOwner.load(std::memory_order_acquire) != caller) {
        ++session->foreignSimulationTicks;
        faultSession(session);
        return;
    }
    if (session->driver && !session->driver->ready()) {
        ++session->unconfiguredTicks;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(session->recordsMutex);
        if (token <= session->newestToken) { ++session->missingBoundaries; return; }
        auto& frame = session->frames[token % session->frames.size()];
        if (frame.token && !frame.completed) ++session->overwrittenFrames;
        if (frame.completed && !frame.softwareReported) ++session->softwareDropped;
        if (frame.completed && frame.token > session->lastSoftwareReported.load(std::memory_order_acquire))
            ++session->softwareDropped;
        frame = {};
        frame.token = token;
        frame.simulationThread = GetCurrentThreadId();
        session->newestToken = token;
    }
    tick = {token, session->generation};
    bool accepted = true;
    if (session->driver) {
        const uint64_t began = qpc();
        const auto result = session->driver->sleepFrame(token);
        const uint64_t ended = qpc();
        if (result == FrameDriver::SleepResult::Skipped) {
            ++session->unconfiguredTicks;
            std::lock_guard<std::mutex> lock(session->recordsMutex);
            if (auto* frame = findFrame(*session, token)) frame->invalid = true;
            tick = {};
            return;
        }
        ++session->sleepCalls;
        accepted = result == FrameDriver::SleepResult::Accepted;
        if (accepted) ++session->sleepAccepted;
        else {
            faultSession(session);
        }
        if (began && ended >= began) session->sleepTicks.fetch_add(ended - began, std::memory_order_relaxed);
    }
    if (!session->active.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lock(session->recordsMutex);
        auto* frame = findFrame(*session, token);
        if (!frame) return;
        frame->driverFailed = !accepted;
        if (!reserveMarker(*frame, Marker::SimulationStart)) return;
    }
    emit(session, token, Marker::SimulationStart);
}
void inputBegin(void*, uint64_t token) noexcept {
    auto session = activeSession();
    if (!session || tick.token != token || tick.generation != session->generation) return;
    ++session->inputCalls;
    pcl::inputFrame(token);
    if (!session->pendingFlash.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lock(session->recordsMutex);
        auto* frame = findFrame(*session, token);
        if (!frame || !(frame->completedMarkers & (1u << unsigned(Marker::SimulationStart))) ||
            (frame->reservedMarkers & (1u << unsigned(Marker::SimulationEnd))) ||
            (frame->reservedMarkers & (1u << unsigned(Marker::TriggerFlash))) ||
            !session->pendingFlash.exchange(false, std::memory_order_acq_rel) ||
            !reserveMarker(*frame, Marker::TriggerFlash)) return;
    }
    emit(session, token, Marker::TriggerFlash);
}
void normalReturned(void*, uint64_t token) noexcept {
    auto session = activeSession();
    if (!session || tick.token != token || tick.generation != session->generation) return;
    {
        std::lock_guard<std::mutex> lock(session->recordsMutex);
        auto* frame = findFrame(*session, token);
        if (!frame || !reserveMarker(*frame, Marker::SimulationEnd)) { ++session->missingBoundaries; return; }
    }
    emit(session, token, Marker::SimulationEnd);
}
void afterTick(void*, uint64_t token) noexcept {
    auto session = activeSession();
    if (session && tick.token == token && tick.generation == session->generation) ++session->completedTicks;
    tick = {};
}
void lateBegin(void*, uint64_t token) noexcept {
    auto session = activeSession();
    if (!session || !token || late.token) return;
    {
        std::lock_guard<std::mutex> lock(session->recordsMutex);
        auto* frame = findFrame(*session, token);
        if (!frame || frame->invalid || frame->lateBegun) { ++session->missingBoundaries; return; }
        frame->lateBegun = true;
    }
    late = {token, session->generation};
    frameCommandObserver = noteFrameCommand;
}
void lateEnd(void*, uint64_t token) noexcept {
    if (late.token != token) return;
    frameCommandObserver = nullptr;
    auto session = activeSession();
    const uint64_t generation = late.generation;
    late = {};
    if (!session || generation != session->generation) return;
    bool endRender = false;
    {
        std::lock_guard<std::mutex> lock(session->recordsMutex);
        auto* frame = findFrame(*session, token);
        if (!frame || frame->invalid || frame->lateEnded) { ++session->missingBoundaries; return; }
        frame->lateEnded = true;
        frame->lateEndQpc = qpc();
        endRender = reserveRenderEnd(*frame);
    }
    if (endRender) emit(session, token, Marker::RenderSubmitEnd);
}
const FrameBoundaryCallbacks callbacks = [] {
    FrameBoundaryCallbacks result{};
    result.beforeTick = beforeTick;
    result.afterTick = afterTick;
    result.inputBegin = inputBegin;
    result.normalPhaseReturned = normalReturned;
    result.lateWorkerBegin = lateBegin;
    result.lateWorkerEnd = lateEnd;
    return result;
}();

struct ProviderContext {
    std::weak_ptr<FrameSession> owner;
    std::atomic<bool> active{false};
    unsigned ordinal = 0;
};
std::array<ProviderContext, kProviderContexts> providerContexts;
std::mutex providerMutex;
unsigned usedContexts = 0;
ProviderContext* identify(void* context) noexcept {
    for (auto& entry : providerContexts) if (&entry == context) return &entry;
    return nullptr;
}
std::shared_ptr<FrameSession> providerSession(void* context, ProviderContext*& entry) noexcept {
    entry = identify(context);
    if (!entry || !entry->active.load(std::memory_order_acquire)) return {};
    auto session = entry->owner.lock();
    return session && session->active.load(std::memory_order_acquire) ? session : nullptr;
}
uint64_t WINAPI poseConsumed(void* context, uint64_t sequence) {
    const uint64_t observed = qpc(), token = currentLateRenderFrameToken();
    ProviderContext* entry = nullptr;
    auto session = providerSession(context, entry);
    if (!session || !sequence || !observed || token != late.token || late.generation != session->generation) return 0;
    std::lock_guard<std::mutex> lock(session->recordsMutex);
    auto* frame = findFrame(*session, token);
    if (!frame || frame->invalid || !frame->lateBegun || frame->poseSequence) { ++session->refusedSources; return 0; }
    frame->poseSequence = sequence;
    frame->poseConsumedQpc = observed;
    frame->poseThread = GetCurrentThreadId();
    frame->provider = entry->ordinal;
    return token;
}
HRESULT WINAPI eyeSource(void* context, ID3D11Texture2D* texture, uint32_t eye,
                         uint64_t sequence, EdvrNativeReflexSource* output) {
    if (output) *output = {};
    clearSource(texture);
    if (!texture || !output || eye > 1 || !sequence) return E_INVALIDARG;
    ProviderContext* entry = nullptr;
    auto session = providerSession(context, entry);
    const uint64_t token = currentLateRenderFrameToken();
    if (!session || token != late.token || late.generation != session->generation) return E_PENDING;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    texture->GetDevice(&device);
    if (device.Get() != session->device.Get()) return E_INVALIDARG;
    {
        std::lock_guard<std::mutex> lock(session->recordsMutex);
        auto* frame = findFrame(*session, token);
        const unsigned bit = 1u << eye;
        if (!frame || frame->invalid || frame->provider != entry->ordinal || frame->poseSequence != sequence ||
            !(frame->completedMarkers & (1u << unsigned(Marker::RenderSubmitStart))) || (frame->eyes & bit)) {
            ++session->refusedSources; return E_PENDING;
        }
        frame->eyes |= bit;
    }
    const EdvrNativeReflexSource source{sizeof(source), EDVR_NATIVE_REFLEX_FRAME_VERSION_1,
                                      session->generation, token, sequence};
    if (!writeSource(texture, source) || !session->active.load(std::memory_order_acquire)) {
        clearSource(texture);
        std::lock_guard<std::mutex> lock(session->recordsMutex);
        if (auto* frame = findFrame(*session, token)) frame->invalid = true;
        ++session->refusedSources;
        return E_FAIL;
    }
    *output = source;
    return S_OK;
}
void WINAPI eyeSubmitted(void* context, uint32_t eye, uint64_t sequence, uint32_t accepted) {
    ProviderContext* entry = nullptr;
    auto session = providerSession(context, entry);
    const uint64_t token = currentLateRenderFrameToken();
    if (!session || eye > 1 || accepted > 1 || token != late.token || late.generation != session->generation) return;
    std::lock_guard<std::mutex> lock(session->recordsMutex);
    auto* frame = findFrame(*session, token);
    const unsigned bit = 1u << eye;
    if (!frame || frame->invalid || frame->provider != entry->ordinal || frame->poseSequence != sequence ||
        !(frame->eyes & bit) || (frame->acceptedEyes & bit) || !accepted) {
        if (frame) frame->invalid = true;
        ++session->refusedSources;
        return;
    }
    frame->acceptedEyes |= bit;
}
HRESULT WINAPI closeProvider(void* context) {
    std::lock_guard<std::mutex> providerLock(providerMutex);
    auto* entry = identify(context);
    if (!entry) return E_INVALIDARG;
    if (!entry->active.exchange(false, std::memory_order_acq_rel)) return S_FALSE;
    auto session = entry->owner.lock();
    if (session) {
        session->providerAcquired.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> capabilityLock(capabilityMutex);
            if (std::atomic_load_explicit(&published(), std::memory_order_acquire) == session)
                fullFrameAvailable.store(false, std::memory_order_release);
        }
        std::lock_guard<std::mutex> lock(session->recordsMutex);
        for (auto& frame : session->frames)
            if (frame.provider == entry->ordinal && !frame.completed) frame.invalid = true;
    }
    return S_OK;
}
} // namespace

void mouseMessageRemoved(bool down) noexcept {
    auto session = activeSession();
    if (!session) return;
    const bool previous = session->leftButtonDown.exchange(down, std::memory_order_acq_rel);
    if (down && !previous) session->pendingFlash.store(true, std::memory_order_release);
}

void noteFrameCommand(ID3D11DeviceContext* context) noexcept {
    const uint64_t token = currentLateRenderFrameToken();
    if (!context || !token || token != late.token || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    auto session = activeSession();
    if (!session || late.generation != session->generation) return;
    {
        std::lock_guard<std::mutex> lock(session->recordsMutex);
        auto* frame = findFrame(*session, token);
        if (!frame || frame->invalid || !frame->lateBegun || !frame->poseSequence) return;
    }
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (device.Get() != session->device.Get()) return;
    {
        std::lock_guard<std::mutex> lock(session->recordsMutex);
        auto* frame = findFrame(*session, token);
        if (!frame || !reserveMarker(*frame, Marker::RenderSubmitStart)) return;
        frame->firstCommandQpc = qpc();
        frame->commandThread = GetCurrentThreadId();
    }
    frameCommandObserver = nullptr;
    emit(session, token, Marker::RenderSubmitStart);
}

PresentFrameReceipt beginFramePresent(ID3D11Device* device, const EdvrNativeReflexSource& source) noexcept {
    const uint64_t observed = qpc();
    auto session = activeSession();
    if (!session || !observed || device != session->device.Get() || !validSource(source) || source.generation != session->generation)
        return {};
    bool endRender = false;
    {
        std::lock_guard<std::mutex> lock(session->recordsMutex);
        auto* frame = findFrame(*session, source.frameToken);
        if (!frame || frame->invalid || frame->poseSequence != source.poseSequence || frame->eyes != 3 || frame->acceptedEyes != 3 ||
            !(frame->completedMarkers & (1u << unsigned(Marker::RenderSubmitStart)))) {
            ++session->refusedSources; return {};
        }
        if (source.frameToken <= session->lastPresentedToken || !reserveMarker(*frame, Marker::PresentStart)) {
            ++session->duplicatePresents; return {};
        }
        session->lastPresentedToken = source.frameToken;
        frame->mirrorDrawComplete = true;
        frame->mirrorCompleteQpc = observed;
        frame->presentBeginQpc = observed;
        frame->presentThread = GetCurrentThreadId();
        endRender = reserveRenderEnd(*frame);
    }
    if (endRender) emit(session, source.frameToken, Marker::RenderSubmitEnd);
    emit(session, source.frameToken, Marker::PresentStart);
    return {session, source.frameToken};
}
void endFramePresent(const PresentFrameReceipt& receipt) noexcept {
    const uint64_t observed = qpc();
    const auto& session = receipt.owner;
    if (!session || !receipt.frameId || !observed || !session->active.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lock(session->recordsMutex);
        auto* frame = findFrame(*session, receipt.frameId);
        if (!frame || frame->invalid || frame->presentThread != GetCurrentThreadId() ||
            !(frame->completedMarkers & (1u << unsigned(Marker::PresentStart))) ||
            !reserveMarker(*frame, Marker::PresentEnd)) return;
        frame->presentEndQpc = observed;
    }
    emit(session, receipt.frameId, Marker::PresentEnd);
}

FrameDriver* startFrameSession(ID3D11Device* device, uint64_t generation) {
    if (!device || !generation || std::atomic_load_explicit(&published(), std::memory_order_acquire)) return nullptr;
    pcl::initialize();
    inputGateInstall();
    if (!installFrameBoundaryHooks()) {
        Log::get().note("native_reflex_frames,state=unavailable,reason=%s,full_boundary=0", frameBoundaryHookStatus());
        return nullptr;
    }
    auto session = std::make_shared<FrameSession>();
    session->device = device;
    session->generation = generation;
    session->producerThread = GetCurrentThreadId();
    session->driver = openFrameDriver(device);
    auto* borrowed = session->driver.get();
    {
        std::lock_guard<std::mutex> lock(capabilityMutex);
        fullFrameAvailable.store(false, std::memory_order_release);
        fullFrameFaulted.store(false, std::memory_order_release);
        std::atomic_store_explicit(&published(), std::move(session), std::memory_order_release);
    }
    setFrameBoundaryCallbacks(&callbacks);
    Log::get().note("native_reflex_frames,state=enrolled,session=%llu,sleep_boundary=app_tick_before_input,"
                   "full_boundary=0,reason=awaiting_complete_engine_source", generation);
    return borrowed;
}
void stopFrameSession(uint64_t generation) noexcept {
    auto session = std::atomic_load_explicit(&published(), std::memory_order_acquire);
    if (!session || session->generation != generation) return;
    {
        std::lock_guard<std::mutex> lock(capabilityMutex);
        fullFrameAvailable.store(false, std::memory_order_release);
        fullFrameFaulted.store(false, std::memory_order_release);
        session->active.store(false, std::memory_order_release);
        std::atomic_store_explicit(&published(), std::shared_ptr<FrameSession>{}, std::memory_order_release);
    }
    if (session->driver) session->driver->retire();
    setFrameBoundaryCallbacks(nullptr);
}

void reportFrameSession(uint64_t generation) noexcept {
    auto session = activeSession();
    if (!session || session->producerThread != GetCurrentThreadId() || (generation && session->generation != generation)) return;
    const uint64_t now = GetTickCount64();
    if (session->lastPollMs && now - session->lastPollMs < 500) return;
    session->lastPollMs = now;
    const bool summaryDue = !session->lastSummaryMs || now - session->lastSummaryMs >= 5000;
    if (summaryDue) session->lastSummaryMs = now;
    const auto boundary = frameBoundaryStats();
    const uint64_t lastComplete = session->lastCompletedMs.load(std::memory_order_acquire);
    const uint64_t capabilityNow = GetTickCount64();
    const bool capable = session->providerAcquired.load(std::memory_order_acquire) &&
        session->driver && session->driver->ready() && pcl::available() && boundary.installed &&
        lastComplete && capabilityNow >= lastComplete && capabilityNow - lastComplete <= 2000 &&
        !session->foreignSimulationTicks.load(std::memory_order_relaxed) && !session->markerFailures.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(capabilityMutex);
        if (session->active.load(std::memory_order_acquire) &&
            std::atomic_load_explicit(&published(), std::memory_order_acquire) == session)
            fullFrameAvailable.store(!frameSessionFaulted() && capable &&
                session->providerAcquired.load(std::memory_order_acquire), std::memory_order_release);
    }
    std::array<FrameRecord, kFrameHistory> frames;
    {
        std::lock_guard<std::mutex> lock(session->recordsMutex);
        frames = session->frames;
    }
    std::sort(frames.begin(), frames.end(), [](const FrameRecord& a, const FrameRecord& b) { return a.token < b.token; });
    // Drain at most 64 complete records in source order. A later completion
    // cannot consume an earlier source's reporting ticket.
    unsigned softwareRecords = 0;
    const uint64_t previousSoftware = session->lastSoftwareReported.load(std::memory_order_acquire);
    for (const auto& frame : frames) {
        if (!frame.completed || frame.invalid || frame.softwareReported) continue;
        if (softwareRecords == 64) break;
        ++softwareRecords;
        session->lastSoftwareReported.store((std::max)(previousSoftware, frame.token), std::memory_order_release);
        const bool clock = boundary.qpcFrequency && frame.poseConsumedQpc && frame.presentEndQpc >= frame.poseConsumedQpc;
        if (!clock) continue;
        const double milliseconds = double(frame.presentEndQpc - frame.poseConsumedQpc) * 1000.0 / double(boundary.qpcFrequency);
        Log::get().note("native_reflex_frame,session=%llu,frame=%llu,pose=%llu,pose_qpc=%llu,command_qpc=%llu,"
            "late_end_qpc=%llu,mirror_qpc=%llu,present_begin_qpc=%llu,present_end_qpc=%llu,qpc_frequency=%llu,"
            "simulation_thread=%lu,pose_thread=%lu,command_thread=%lu,present_thread=%lu,"
            "pose_to_present_cpu_ms=%.4f,driver_markers=%u,photons=unmeasured",
            session->generation, frame.token, frame.poseSequence, frame.poseConsumedQpc, frame.firstCommandQpc,
            frame.lateEndQpc, frame.mirrorCompleteQpc, frame.presentBeginQpc, frame.presentEndQpc, boundary.qpcFrequency,
            frame.simulationThread, frame.poseThread, frame.commandThread, frame.presentThread,
            milliseconds, unsigned(session->driver && !frame.driverFailed));
        {
            std::lock_guard<std::mutex> lock(session->recordsMutex);
            if (auto* current = findFrame(*session, frame.token)) current->softwareReported = true;
        }
    }
    unsigned driverRecords = 0;
    if (session->driver) {
        LatencyBatch batch;
        // Driver completion can precede the final CPU callback. Revisit its
        // finite history and deduplicate per source, so an overlapping frame
        // is not permanently discarded by a newer frame's watermark.
        if (session->driver->getLatency(batch)) {
            for (size_t i = 0; i < batch.count; ++i) {
                const auto& value = batch.frames[i];
                const auto found = std::find_if(frames.begin(), frames.end(), [&](const FrameRecord& frame) {
                    return frame.token == value.frameId && frame.completed && !frame.invalid && !frame.driverFailed;
                });
                if (found == frames.end() || found->driverReported) continue;
                if (session->lastDriverReported && value.frameId > session->lastDriverReported + 1)
                    session->driverFrameGaps.fetch_add(value.frameId - session->lastDriverReported - 1, std::memory_order_relaxed);
                session->lastDriverReported = (std::max)(session->lastDriverReported, value.frameId);
                ++driverRecords;
                Log::get().note("native_reflex_latency,session=%llu,frame=%llu,clock=nvapi_us,sim_start_us=%llu,"
                    "sim_end_us=%llu,render_start_us=%llu,render_end_us=%llu,present_start_us=%llu,present_end_us=%llu,"
                    "gpu_start_us=%llu,gpu_end_us=%llu,sim_to_gpu_us=%llu,photons=unmeasured",
                    session->generation, value.frameId, value.simStartUs, value.simEndUs,
                    value.renderSubmitStartUs, value.renderSubmitEndUs, value.presentStartUs, value.presentEndUs,
                    value.gpuRenderStartUs, value.gpuRenderEndUs, value.gpuRenderEndUs - value.simStartUs);
                {
                    std::lock_guard<std::mutex> lock(session->recordsMutex);
                    if (auto* current = findFrame(*session, value.frameId)) current->driverReported = true;
                }
            }
        }
    }
    unsigned recentPoses = 0, recentRenders = 0, recentStereo = 0;
    if (summaryDue) for (const auto& frame : frames) {
        recentPoses += frame.poseSequence != 0;
        recentRenders += (frame.completedMarkers & (1u << unsigned(Marker::RenderSubmitStart))) != 0;
        recentStereo += frame.acceptedEyes == 3;
    }
    if (summaryDue) Log::get().note("native_reflex_frames,state=observed,session=%llu,sleep_calls=%llu,sleep_accepted=%llu,"
        "sleep_qpc_ticks=%llu,qpc_frequency=%llu,input_calls=%llu,completed_ticks=%llu,complete_sources=%llu,"
        "overwritten_sources=%llu,missing_boundaries=%llu,refused_sources=%llu,duplicate_presents=%llu,"
        "marker_failures=%llu,foreign_simulation_ticks=%llu,unconfigured_ticks=%llu,software_records_dropped=%llu,"
        "driver_frame_id_gaps=%llu,driver_records=%u,normal_phases=%llu,late_tickets=%llu,late_workers=%llu,"
        "late_unqualified=%llu,late_refused=%llu,recent_pose_sources=%u,recent_render_sources=%u,"
        "recent_stereo_sources=%u,pcl=%u,full_boundary=%u",
        session->generation, session->sleepCalls.load(), session->sleepAccepted.load(), session->sleepTicks.load(),
        boundary.qpcFrequency, session->inputCalls.load(), session->completedTicks.load(), session->completedFrames.load(),
        session->overwrittenFrames.load(), session->missingBoundaries.load(), session->refusedSources.load(),
        session->duplicatePresents.load(), session->markerFailures.load(), session->foreignSimulationTicks.load(),
        session->unconfiguredTicks.load(), session->softwareDropped.load(), session->driverFrameGaps.load(),
        driverRecords, boundary.normalPhases, boundary.lateTickets, boundary.qualifiedLateWorkers,
        boundary.unqualifiedLateWorkers, boundary.refusedLateTickets, recentPoses, recentRenders, recentStereo,
        unsigned(pcl::available()),
        unsigned(availableForUser() && session->completedFrames.load() != 0));
}
void shutdownFrameTelemetry() noexcept { pcl::shutdown(); }

HRESULT acquireFrameProvider(const EdvrNativeReflexFrameRequest* request, EdvrNativeReflexFrameTable* table) {
    if (!request || !table || request->size < sizeof(*request) || request->version != EDVR_NATIVE_REFLEX_FRAME_VERSION_1 ||
        table->size < sizeof(*table) || table->version != EDVR_NATIVE_REFLEX_FRAME_VERSION_1 || !request->device || !request->generation)
        return E_INVALIDARG;
    auto session = activeSession();
    if (!session || session->generation != request->generation || session->device.Get() != request->device) return E_PENDING;
    std::lock_guard<std::mutex> lock(providerMutex);
    if (usedContexts == providerContexts.size()) return E_OUTOFMEMORY;
    for (auto& entry : providerContexts)
        if (entry.active.load(std::memory_order_acquire) && entry.owner.lock() == session) return E_PENDING;
    auto& entry = providerContexts[usedContexts++];
    entry.owner = session;
    entry.ordinal = usedContexts;
    entry.active.store(true, std::memory_order_release);
    session->providerAcquired.store(true, std::memory_order_release);
    *table = {sizeof(*table), EDVR_NATIVE_REFLEX_FRAME_VERSION_1, &entry, poseConsumed, eyeSource, eyeSubmitted, closeProvider};
    return S_OK;
}
} // namespace edvr::native_reflex

extern "C" HRESULT WINAPI edvrAcquireNativeReflexFrame(const EdvrNativeReflexFrameRequest* request,
                                                        EdvrNativeReflexFrameTable* table) {
    return edvr::native_reflex::acquireFrameProvider(request, table);
}

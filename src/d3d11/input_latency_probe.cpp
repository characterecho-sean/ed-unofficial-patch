#include "input_latency_probe.h"
#include "input_latency_state.h"
#include "../common/log.h"

#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <cwchar>
#include <memory>
#include <mutex>
#include <new>

namespace edvr::input_latency_probe {
namespace {
using Microsoft::WRL::ComPtr;

uint64_t readWord(uint64_t& word) noexcept {
    return uint64_t(InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&word), 0, 0));
}
void writeWord(uint64_t& word, uint64_t value) noexcept {
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&word), LONG64(value));
}
uint64_t nowQpc() noexcept {
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value) && value.QuadPart > 0 ? uint64_t(value.QuadPart) : 0;
}
const char* reasonWord(Refusal reason) noexcept {
    switch (reason) {
    case Refusal::None: return "none";
    case Refusal::Timeout: return "timeout";
    case Refusal::InvalidRequest: return "invalid_request";
    case Refusal::GenerationChanged: return "generation_changed";
    case Refusal::WrongThread: return "wrong_thread";
    case Refusal::TextureUnsupported: return "texture_unsupported";
    case Refusal::MixedFrame: return "mixed_frame";
    case Refusal::DuplicateEye: return "duplicate_eye";
    case Refusal::GraphicsFailure: return "graphics_failure";
    case Refusal::SubmitFailed: return "submit_failed";
    case Refusal::DeviceRemoved: return "device_removed";
    case Refusal::Stopped: return "stopped";
    case Refusal::BaselineMismatch: return "baseline_mismatch";
    case Refusal::CandidateOverflow: return "candidate_overflow";
    }
    return "invalid_reason";
}

constexpr size_t kUiCandidates = 4;
struct UiFrame {
    UiCandidateReceipt receipt;
    ComPtr<ID3D11Texture2D> source[2], staging[2];
    uint32_t width[2]{}, height[2]{};
    DXGI_FORMAT format[2]{};
    bool bgra[2]{};
    uint64_t fenceValue = 0;
};
struct UiRequest {
    bool active = false, baseline = false, released = false, prepared = false;
    uint64_t request = 0, beganQpc = 0, baselineSequence = 0;
    uint64_t scanCode = 0, roi[2]{}, lowPpm = 0, highPpm = 0, targetOn = 0;
    uint32_t sourceWidth[2]{}, sourceHeight[2]{};
    DXGI_FORMAT sourceFormat[2]{};
    std::array<UiFrame, kUiCandidates> frames;
    size_t head = 0, count = 0;
};

struct Probe {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> fence;
    ComPtr<ID3D11Texture2D> responseTextures[2];
    HANDLE mapping = nullptr, stopEvent = nullptr, fenceEvent = nullptr, worker = nullptr;
    Shared* shared = nullptr;
    DWORD ownerThread = 0;
    uint64_t frequency = 0, generation = 0, fenceValue = 0;
    uint64_t pendingFence = 0, pendingRequest = 0, reservationAt = 0;
    native_reflex::Mode mode = native_reflex::Mode::Off;
    Pulse pulse;
    ButtonEdges buttons;
    PairBoundary pairs;
    UiRequest ui;
    unsigned textureNotes = 0;
    bool reported = false;
    unsigned reports = 0;

    // The lifecycle owner joins worker before releasing this storage. These
    // destructors release COM references only; no context commands run here.
    ~Probe() {
        if (worker) CloseHandle(worker);
        if (shared) UnmapViewOfFile(shared);
        if (mapping) CloseHandle(mapping);
        if (fenceEvent) CloseHandle(fenceEvent);
        if (stopEvent) CloseHandle(stopEvent);
    }
};

// Lifecycle serialization is separate from the short callback lock: stop may
// wait for worker only after dropping callbacks, which worker also takes.
std::mutex lifecycle, callbacks;
// Explicit normal-close ownership. A process-exit global destructor must not
// release D3D objects or try to join a worker under the loader lock.
Probe* current = nullptr;
ID3D11Device* attemptedDevice = nullptr;
uint64_t attemptedGeneration = 0;
native_reflex::Mode attemptedMode = native_reflex::Mode::Off;
bool attempted = false;

void clearUiFrames(UiRequest& ui) noexcept {
    for (auto& frame : ui.frames) {
        frame.receipt = {}; frame.fenceValue = 0;
        frame.source[0].Reset(); frame.source[1].Reset();
    }
    ui.head = ui.count = 0;
}
void clearUiRequest(Probe& probe) noexcept {
    clearUiFrames(probe.ui);
    probe.ui.active = probe.ui.baseline = probe.ui.released = probe.ui.prepared = false;
}
bool uiSettingsMatch(const Probe& probe) noexcept {
    const auto& s = *probe.shared;
    const auto& ui = probe.ui;
    return s.responseKind == uint64_t(ResponseKind::GameUi) && s.request == ui.request &&
        s.inputScanCode == ui.scanCode && s.roiLeft == ui.roi[0] && s.roiRight == ui.roi[1] &&
        s.orangeLowPpm == ui.lowPpm && s.orangeHighPpm == ui.highPpm && s.targetOn == ui.targetOn;
}

void reportLocked(Probe& probe) noexcept {
    if (!probe.pulse.terminal() || probe.reported) return;
    probe.reported = true;
    if (probe.reports++ >= 256) {
        if (probe.reports == 257) Log::get().note("input_latency_probe,logging=limited,pulse_limit=256");
        return;
    }
    const auto& result = probe.pulse.result();
    if (probe.pulse.phase() == Phase::Complete) {
        const double scale = 1000.0 / double(probe.frequency);
        Log::get().note("input_latency_probe,request=%llu,state=complete,mode=%u,generation=%llu,sequence=%llu,source=%u,"
            "sent_to_consumed_ms=%.4f,consumed_to_gpu_notification_upper_bound_ms=%.4f,consumed_to_submit_ms=%.4f,"
            "input_tick=%llu,response_kind=%llu,left_ppm=%llu,right_ppm=%llu,photons=unmeasured",
            result.request, unsigned(probe.mode), probe.generation, result.sequence, result.source,
            double(result.consumedQpc - result.sentQpc) * scale,
            double(result.gpuCompleteQpc - result.consumedQpc) * scale,
            double(result.submitCompleteQpc - result.consumedQpc) * scale, result.consumedTickToken,
            probe.shared->responseKind, probe.shared->responseLeftPpm, probe.shared->responseRightPpm);
    } else {
        Log::get().note("input_latency_probe,request=%llu,state=rejected,mode=%u,generation=%llu,reason=%s,sequence=%llu,eyes=%llu,submits=%llu",
            result.request, unsigned(probe.mode), probe.generation, reasonWord(result.reason),
            result.sequence, result.eyeMask, result.submitMask);
    }
}

// Publish every result word before state. The external sender acquires state
// before reading results and does not write request data while receiver owns it.
void publishLocked(Probe& probe) noexcept {
    // Once terminal state is visible the sender may acknowledge it and reserve
    // its next request immediately. Never republish an old terminal result.
    if (probe.pulse.terminal() && probe.reported) return;
    const auto& result = probe.pulse.result();
    auto& shared = *probe.shared;
    shared.consumedQpc = result.consumedQpc; shared.renderSequence = result.sequence;
    shared.eyeMask = result.eyeMask; shared.submitMask = result.submitMask;
    shared.gpuCompleteQpc = result.gpuCompleteQpc; shared.submitCompleteQpc = result.submitCompleteQpc;
    shared.reason = uint64_t(result.reason); shared.source = result.source;
    shared.consumedTickToken = result.consumedTickToken;
    if (probe.pulse.phase() == Phase::Rejected) probe.pendingFence = probe.pendingRequest = 0;
    if (probe.pulse.terminal()) {
        probe.responseTextures[0].Reset(); probe.responseTextures[1].Reset();
        clearUiRequest(probe);
    }
    writeWord(shared.state, uint64_t(probe.pulse.phase()));
    reportLocked(probe);
}

void rejectLocked(Probe& probe, Refusal reason) noexcept {
    if (probe.pulse.active()) {
        probe.pulse.reject(reason);
        publishLocked(probe);
    } else if (readWord(probe.shared->state) == uint64_t(Phase::Reserved) || probe.ui.active ||
               readWord(probe.shared->state) == uint64_t(Phase::Preparing) ||
               readWord(probe.shared->state) == uint64_t(Phase::Prepared) ||
               readWord(probe.shared->state) == uint64_t(Phase::Armed)) {
        // Reserved request words still belong to sender. Do not read or log
        // partially written payload; its Reserved -> Armed CAS must now fail.
        probe.shared->consumedQpc = probe.shared->renderSequence = 0;
        probe.shared->eyeMask = probe.shared->submitMask = 0;
        probe.shared->gpuCompleteQpc = probe.shared->submitCompleteQpc = 0;
        probe.shared->source = 0; probe.shared->reason = uint64_t(reason);
        probe.shared->consumedTickToken = 0;
        clearUiRequest(probe);
        writeWord(probe.shared->ready, 0);
        writeWord(probe.shared->state, uint64_t(Phase::Rejected));
        if (probe.reports++ < 256)
            Log::get().note("input_latency_probe,state=rejected,request=unpublished,reason=%s", reasonWord(reason));
    }
}

void prepareUiLocked(Probe& probe, uint64_t now) noexcept {
    auto& ui = probe.ui;
    const auto& s = *probe.shared;
    const auto roiValid = [](uint64_t roi) {
        return uint16_t(roi) < uint16_t(roi >> 32) && uint16_t(roi >> 16) < uint16_t(roi >> 48);
    };
    if (!s.request || s.sentQpc || s.responseKind != uint64_t(ResponseKind::GameUi) ||
        (s.inputScanCode != 0xC8 && s.inputScanCode != 0xD0) || s.targetOn > 1 ||
        s.orangeLowPpm >= s.orangeHighPpm || s.orangeHighPpm > 1000000 ||
        !roiValid(s.roiLeft) || !roiValid(s.roiRight)) {
        rejectLocked(probe, Refusal::InvalidRequest); return;
    }
    clearUiRequest(probe);
    ui.active = true; ui.request = s.request; ui.beganQpc = now;
    ui.scanCode = s.inputScanCode; ui.roi[0] = s.roiLeft; ui.roi[1] = s.roiRight;
    ui.lowPpm = s.orangeLowPpm; ui.highPpm = s.orangeHighPpm; ui.targetOn = s.targetOn;
    for (unsigned eye = 0; eye < 2; ++eye) {
        ui.sourceWidth[eye] = ui.sourceHeight[eye] = 0; ui.sourceFormat[eye] = DXGI_FORMAT_UNKNOWN;
    }
    probe.buttons.reset(); probe.pairs.reset();
    auto& shared = *probe.shared;
    shared.consumedQpc = shared.renderSequence = shared.eyeMask = shared.submitMask = 0;
    shared.gpuCompleteQpc = shared.submitCompleteQpc = shared.reason = shared.source = 0;
    shared.baselineLeftPpm = shared.baselineRightPpm = shared.responseLeftPpm = shared.responseRightPpm = 0;
    shared.consumedTickToken = 0;
    writeWord(shared.ready, 0);
}

void publishPreparedLocked(Probe& probe) noexcept {
    auto& ui = probe.ui;
    if (!ui.active || !ui.baseline || !ui.released || ui.prepared) return;
    ui.prepared = true;
    probe.shared->renderSequence = ui.baselineSequence;
    probe.shared->eyeMask = probe.shared->submitMask = 3;
    writeWord(probe.shared->ready, 1);
    writeWord(probe.shared->state, uint64_t(Phase::Prepared));
}

// Import one release-published external request and bound its entire lifetime
// from SendInput's QPC stamp. No heartbeat or mouse packet is required for expiry.
void synchronizeLocked(Probe& probe, uint64_t now) noexcept {
    const auto phase = Phase(readWord(probe.shared->state));
    if (phase == Phase::Idle && probe.ui.active) {
        if (probe.pulse.active()) { rejectLocked(probe, Refusal::InvalidRequest); return; }
        clearUiRequest(probe); writeWord(probe.shared->ready, 0); return;
    }
    if (phase == Phase::Reserved) {
        if (probe.pulse.active()) { rejectLocked(probe, Refusal::InvalidRequest); return; }
        // Prepared may be cancelled to Idle and re-reserved between callbacks.
        // Preparation itself remains receiver-owned and cannot be cancelled.
        if (probe.ui.active) {
            if (!probe.ui.prepared) { rejectLocked(probe, Refusal::InvalidRequest); return; }
            clearUiRequest(probe); writeWord(probe.shared->ready, 0);
        }
        if (!probe.reservationAt) probe.reservationAt = now;
        if (!now || now < probe.reservationAt || now - probe.reservationAt > probe.frequency * 2)
            rejectLocked(probe, Refusal::Timeout);
        return;
    }
    probe.reservationAt = 0;
    if (phase == Phase::Preparing || phase == Phase::Prepared) {
        if (probe.pulse.active()) { rejectLocked(probe, Refusal::InvalidRequest); return; }
        if (!probe.ui.active) {
            if (phase != Phase::Preparing) { rejectLocked(probe, Refusal::InvalidRequest); return; }
            prepareUiLocked(probe, now);
        }
        if (!probe.ui.active) return;
        if (!uiSettingsMatch(probe) || (phase == Phase::Preparing && probe.shared->sentQpc) ||
            (phase == Phase::Prepared && !probe.ui.prepared)) {
            rejectLocked(probe, Refusal::InvalidRequest); return;
        }
        if (!now || now < probe.ui.beganQpc || now - probe.ui.beganQpc > probe.frequency * 2)
            rejectLocked(probe, Refusal::Timeout);
        return;
    }
    if (probe.ui.active && !uiSettingsMatch(probe)) { rejectLocked(probe, Refusal::InvalidRequest); return; }
    if (phase == Phase::Armed && !probe.pulse.active()) {
        if (probe.shared->responseKind == uint64_t(ResponseKind::GameUi) &&
            (!probe.ui.active || !probe.ui.prepared || !probe.ui.released)) {
            rejectLocked(probe, Refusal::InvalidRequest); return;
        }
        if (probe.shared->responseKind > uint64_t(ResponseKind::GameUi)) {
            rejectLocked(probe, Refusal::InvalidRequest); return;
        }
        probe.reported = false;
        probe.responseTextures[0].Reset(); probe.responseTextures[1].Reset();
        clearUiFrames(probe.ui);
        probe.pulse.arm(probe.shared->request, probe.shared->sentQpc, now, probe.frequency);
        publishLocked(probe);
    } else if (probe.pulse.active() &&
               (phase != probe.pulse.phase() || probe.shared->request != probe.pulse.result().request ||
                probe.shared->sentQpc != probe.pulse.result().sentQpc)) {
        rejectLocked(probe, Refusal::InvalidRequest);
        return;
    }
    if (probe.pulse.expire(now)) publishLocked(probe);
}

DWORD WINAPI observeFence(void* argument) noexcept {
    auto& probe = *static_cast<Probe*>(argument);
    const HANDLE events[]{probe.stopEvent, probe.fenceEvent};
    for (;;) {
        const DWORD waited = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (waited == WAIT_OBJECT_0) return 0;
        std::lock_guard<std::mutex> lock(callbacks);
        if (current != &probe) return 0;
        if (waited != WAIT_OBJECT_0 + 1) {
            rejectLocked(probe, Refusal::GraphicsFailure);
            writeWord(probe.shared->ready, 0);
            live.store(false, std::memory_order_release);
            Log::get().note("input_latency_probe,state=unavailable,reason=worker_wait_failed");
            return 0;
        }
        // Fence notification may precede submit acceptance. Preserve its own
        // QPC endpoint, then await the second accepted submit independently.
        const uint64_t completed = probe.fence->GetCompletedValue();
        if (completed == UINT64_MAX) { rejectLocked(probe, Refusal::DeviceRemoved); continue; }
        const uint64_t observed = nowQpc();
        // UI candidates retain immutable ROI copies. The worker records only
        // the completed-fence upper bound; their context and pixels stay on
        // the producer thread, including all Map/Unmap calls.
        for (size_t i = 0; i < probe.ui.count; ++i) {
            auto& frame = probe.ui.frames[(probe.ui.head + i) % kUiCandidates];
            if (frame.fenceValue && frame.fenceValue <= completed && !frame.receipt.gpuQpc)
                frame.receipt.gpuQpc = observed;
        }
        if (!probe.pendingFence) continue;
        if (completed < probe.pendingFence) continue; // an older rejected pulse's event
        // Read QPC only after verifying this pulse's completed fence value.
        // A stale event cannot put the upper bound before current completion.
        const uint64_t request = probe.pendingRequest;
        probe.pendingFence = probe.pendingRequest = 0;
        synchronizeLocked(probe, observed);
        if (probe.pulse.active() && probe.pulse.result().request == request &&
            probe.pulse.gpuCompleted(observed)) publishLocked(probe);
        else if (probe.pulse.terminal()) publishLocked(probe);
    }
}

std::unique_ptr<Probe> createProbe(ID3D11Device* device, native_reflex::Mode mode,
                                 uint64_t generation, const char*& failure) noexcept {
    auto probe = std::unique_ptr<Probe>(new (std::nothrow) Probe);
    if (!probe) { failure = "allocation"; return {}; }
    probe->device = device; probe->mode = mode; probe->generation = generation;
    probe->ownerThread = GetCurrentThreadId();
    LARGE_INTEGER frequency{};
    FILETIME creation{}, exited{}, kernel{}, user{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !GetProcessTimes(GetCurrentProcess(), &creation, &exited, &kernel, &user)) {
        failure = "process_clock"; return {};
    }
    probe->frequency = uint64_t(frequency.QuadPart);
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5))) || !context ||
        FAILED(context.As(&probe->context1)) || FAILED(context.As(&probe->context4)) ||
        FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&probe->fence)))) {
        failure = "gpu_fence_unavailable"; return {};
    }
    probe->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    probe->fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!probe->stopEvent || !probe->fenceEvent) { failure = "event_creation"; return {}; }
    const uint64_t creationTime = (uint64_t(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
    wchar_t name[128]{};
    const int count = std::swprintf(name, sizeof(name) / sizeof(*name), L"%ls%lu.%llu",
        kMappingPrefix, GetCurrentProcessId(), creationTime);
    if (count <= 0 || size_t(count) >= sizeof(name) / sizeof(*name)) { failure = "mapping_name"; return {}; }
    probe->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name);
    const DWORD mappingError = GetLastError();
    if (!probe->mapping || mappingError == ERROR_ALREADY_EXISTS) { failure = "mapping_collision_or_failure"; return {}; }
    probe->shared = static_cast<Shared*>(MapViewOfFile(probe->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!probe->shared) { failure = "mapping_view"; return {}; }
    *probe->shared = Shared{};
    probe->shared->pid = GetCurrentProcessId(); probe->shared->creationTime = creationTime;
    probe->shared->frequency = probe->frequency; probe->shared->generation = generation;
    probe->shared->mode = uint64_t(mode);
    probe->worker = CreateThread(nullptr, 0, observeFence, probe.get(), 0, nullptr);
    if (!probe->worker) { failure = "worker_creation"; return {}; }
    return probe;
}

// lifecycle must be held. Detach under callbacks, then join without it so a
// notification already waiting for that lock can finish and release worker.
void retireProbe() noexcept {
    std::unique_ptr<Probe> retired;
    {
        std::lock_guard<std::mutex> lock(callbacks);
        live.store(false, std::memory_order_release);
        if (!current) return;
        synchronizeLocked(*current, nowQpc());
        rejectLocked(*current, Refusal::Stopped);
        writeWord(current->shared->ready, 0);
        if (!current->pulse.terminal()) writeWord(current->shared->state, uint64_t(Phase::Stopped));
        SetEvent(current->stopEvent);
        retired.reset(current);
        current = nullptr;
    }
    WaitForSingleObject(retired->worker, INFINITE);
}

DXGI_FORMAT responseFormat(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT: return format;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT uiCopyFormat(DXGI_FORMAT format, bool& bgra) noexcept {
    bgra = false;
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return format;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: bgra = true; return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: bgra = true; return format;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

UiFrame* findUiFrame(UiRequest& ui, uint64_t sequence) noexcept {
    for (size_t i = 0; i < ui.count; ++i) {
        auto& frame = ui.frames[(ui.head + i) % kUiCandidates];
        if (frame.receipt.sequence == sequence) return &frame;
    }
    return nullptr;
}

// Called only by the producer. Fence notification proves each immutable copy
// is ready; DO_NOT_WAIT still prevents a driver-dependent Map stall. Candidates
// are evaluated FIFO, so an unchanged frame cannot hide an earlier response.
void pollUiLocked(Probe& probe, uint64_t now) noexcept {
    auto& ui = probe.ui;
    while (ui.active && ui.count) {
        auto& frame = ui.frames[ui.head];
        if (!frame.receipt.ready()) return;
        uint32_t ppm[2]{};
        for (unsigned eye = 0; eye < 2; ++eye) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT hr = probe.context1->Map(frame.staging[eye].Get(), 0, D3D11_MAP_READ,
                                                   D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
            if (FAILED(hr)) { rejectLocked(probe, Refusal::GraphicsFailure); return; }
            const bool decoded = orangeFraction(static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch,
                                                 frame.width[eye], frame.height[eye], frame.bgra[eye], ppm[eye]);
            probe.context1->Unmap(frame.staging[eye].Get(), 0);
            if (!decoded) { rejectLocked(probe, Refusal::TextureUnsupported); return; }
        }
        const auto opposite = ui.targetOn ? OrangeState::Off : OrangeState::On;
        const auto left = orangeState(ppm[0], uint32_t(ui.lowPpm), uint32_t(ui.highPpm));
        const auto right = orangeState(ppm[1], uint32_t(ui.lowPpm), uint32_t(ui.highPpm));
        if (!ui.prepared) {
            probe.shared->baselineLeftPpm = ppm[0]; probe.shared->baselineRightPpm = ppm[1];
            probe.shared->renderSequence = frame.receipt.sequence;
            if (left != opposite || right != opposite) { rejectLocked(probe, Refusal::BaselineMismatch); return; }
            ui.baseline = true; ui.baselineSequence = frame.receipt.sequence;
            clearUiFrames(ui);
            publishPreparedLocked(probe);
            return;
        }
        probe.shared->responseLeftPpm = ppm[0]; probe.shared->responseRightPpm = ppm[1];
        if (uiTargetMatches(frame.receipt, ui.baselineSequence, ppm[0], ppm[1], uint32_t(ui.lowPpm),
                            uint32_t(ui.highPpm), ui.targetOn != 0)) {
            if (probe.pulse.uiResponse(frame.receipt.sequence, frame.receipt.gpuQpc, frame.receipt.submitQpc, now))
                publishLocked(probe);
            else if (probe.pulse.terminal()) publishLocked(probe);
            return;
        }
        // No synthetic response and no zero-valued success: retain the input
        // receipt, discard only this completed pre-change candidate and retry.
        frame.receipt = {}; frame.fenceValue = 0;
        frame.source[0].Reset(); frame.source[1].Reset();
        ui.head = (ui.head + 1) % kUiCandidates; --ui.count;
    }
}

void copyUiEyeLocked(Probe& probe, ID3D11Texture2D* texture, const D3D11_TEXTURE2D_DESC& desc,
                     uint64_t sequence, unsigned eye) noexcept {
    auto& ui = probe.ui;
    bool bgra = false;
    const DXGI_FORMAT format = uiCopyFormat(desc.Format, bgra);
    PixelRoi roi{};
    if (format == DXGI_FORMAT_UNKNOWN || !decodeRoi(ui.roi[eye], desc.Width, desc.Height, roi)) {
        rejectLocked(probe, Refusal::TextureUnsupported); return;
    }
    if (ui.sourceWidth[eye] && (ui.sourceWidth[eye] != desc.Width || ui.sourceHeight[eye] != desc.Height ||
                                ui.sourceFormat[eye] != desc.Format)) {
        rejectLocked(probe, Refusal::TextureUnsupported); return;
    }
    ui.sourceWidth[eye] = desc.Width; ui.sourceHeight[eye] = desc.Height; ui.sourceFormat[eye] = desc.Format;
    UiFrame* frame = findUiFrame(ui, sequence);
    if (!frame) {
        if (ui.count == kUiCandidates) { rejectLocked(probe, Refusal::CandidateOverflow); return; }
        if (ui.count) {
            const auto& previous = ui.frames[(ui.head + ui.count - 1) % kUiCandidates].receipt;
            if (previous.eyeMask != 3 || sequence <= previous.sequence) { rejectLocked(probe, Refusal::MixedFrame); return; }
        }
        frame = &ui.frames[(ui.head + ui.count++) % kUiCandidates];
        frame->receipt = {}; frame->fenceValue = 0; frame->receipt.sequence = sequence;
    }
    if (frame->source[1u - eye].Get() == texture) { rejectLocked(probe, Refusal::TextureUnsupported); return; }
    if (!frame->receipt.copyEye(eye)) { rejectLocked(probe, Refusal::DuplicateEye); return; }
    const uint32_t width = roi.right - roi.left, height = roi.bottom - roi.top;
    if (!frame->staging[eye] || frame->width[eye] != width || frame->height[eye] != height || frame->format[eye] != format) {
        D3D11_TEXTURE2D_DESC staging{};
        staging.Width = width; staging.Height = height;
        staging.MipLevels = staging.ArraySize = 1; staging.Format = format;
        staging.SampleDesc.Count = 1; staging.Usage = D3D11_USAGE_STAGING; staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        frame->staging[eye].Reset();
        if (FAILED(probe.device->CreateTexture2D(&staging, nullptr, &frame->staging[eye]))) {
            rejectLocked(probe, Refusal::GraphicsFailure); return;
        }
        frame->width[eye] = width; frame->height[eye] = height; frame->format[eye] = format;
    }
    frame->bgra[eye] = bgra; frame->source[eye] = texture;
    const D3D11_BOX box{roi.left, roi.top, 0, roi.right, roi.bottom, 1};
    probe.context1->CopySubresourceRegion(frame->staging[eye].Get(), 0, 0, 0, 0, texture, 0, &box);
    if (frame->receipt.eyeMask == 3) {
        if (probe.fenceValue == UINT64_MAX - 1 ||
            FAILED(probe.context4->Signal(probe.fence.Get(), ++probe.fenceValue))) {
            rejectLocked(probe, Refusal::GraphicsFailure); return;
        }
        frame->fenceValue = probe.fenceValue;
        if (FAILED(probe.fence->SetEventOnCompletion(frame->fenceValue, probe.fenceEvent)))
            rejectLocked(probe, Refusal::GraphicsFailure);
    }
}
} // namespace

void configure(ID3D11Device* device, bool wanted, native_reflex::Mode mode, uint64_t generation) noexcept {
    std::lock_guard<std::mutex> control(lifecycle);
    if (!wanted || !device || !generation || unsigned(mode) > unsigned(native_reflex::Mode::Boost)) {
        retireProbe(); attempted = false; return;
    }
    bool replace = false;
    {
        std::lock_guard<std::mutex> lock(callbacks);
        if (current) {
            synchronizeLocked(*current, nowQpc());
            if (current->ownerThread != GetCurrentThreadId()) {
                rejectLocked(*current, Refusal::WrongThread); return;
            }
            replace = current->device.Get() != device;
            if (replace || current->mode != mode || current->generation != generation) {
                rejectLocked(*current, Refusal::GenerationChanged);
                current->buttons.reset(); current->pairs.reset();
                writeWord(current->shared->ready, 0);
                current->mode = mode; current->generation = generation;
                writeWord(current->shared->mode, uint64_t(mode));
                writeWord(current->shared->generation, generation);
            }
            if (!replace) {
                if (FAILED(device->GetDeviceRemovedReason())) rejectLocked(*current, Refusal::DeviceRemoved);
                return;
            }
        }
    }
    if (replace) retireProbe();
    if (attempted && attemptedDevice == device && attemptedMode == mode && attemptedGeneration == generation) return;
    attempted = true; attemptedDevice = device; attemptedMode = mode; attemptedGeneration = generation;
    const char* failure = "unknown";
    std::lock_guard<std::mutex> lock(callbacks);
    auto probe = createProbe(device, mode, generation, failure);
    if (!probe) { Log::get().note("input_latency_probe,state=unavailable,reason=%s", failure); return; }
    current = probe.release();
    writeWord(current->shared->ready, 0);
    live.store(true, std::memory_order_release);
    Log::get().note("input_latency_probe,state=waiting_for_release,mode=%u,generation=%llu,response=16x16,gpu_endpoint=cpu_fence_notification_upper_bound,photons=unmeasured",
        unsigned(mode), generation);
}

void stop() noexcept {
    std::lock_guard<std::mutex> control(lifecycle);
    retireProbe(); attempted = false;
}

void mouseButton(void* device, bool pressed, unsigned source) noexcept {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(callbacks);
    if (!current || !device || (source != 1 && source != 2 && source != 3)) return;
    auto& probe = *current;
    const uint64_t now = nowQpc();
    synchronizeLocked(probe, now);
    if (probe.shared->responseKind == uint64_t(ResponseKind::GameUi)) return;
    const uintptr_t identity = reinterpret_cast<uintptr_t>(device);
    const bool edge = probe.buttons.press(identity, pressed);
    writeWord(probe.shared->ready, !pressed && probe.buttons.released(identity) ? 1 : 0);
    if (edge && probe.pulse.consume(now, source)) {
        probe.pairs.consumed();
        publishLocked(probe);
    }
}

void keyboardState(void* device, const uint8_t* state256, uint64_t inputTickToken) noexcept {
    if (!enabled() || !device || !state256 || !inputTickToken) return;
    // Timestamp the returned API receipt before possible classifier-lock
    // contention. No GPU context command runs on this input thread.
    const uint64_t received = nowQpc();
    std::lock_guard<std::mutex> lock(callbacks);
    if (!current) return;
    auto& probe = *current;
    const uint64_t now = nowQpc();
    synchronizeLocked(probe, now);
    auto& ui = probe.ui;
    if (!ui.active) return;
    const bool pressed = (state256[ui.scanCode] & 0x80) != 0;
    const uintptr_t identity = reinterpret_cast<uintptr_t>(device);
    const bool edge = probe.buttons.press(identity, pressed);
    ui.released = !pressed && probe.buttons.released(identity);
    writeWord(probe.shared->ready, ui.released && ui.prepared ? 1 : 0);
    publishPreparedLocked(probe);
    if (edge && probe.pulse.consume(received, 4, inputTickToken)) {
        probe.pairs.consumed();
        publishLocked(probe);
    }
}

void eyeRendered(ID3D11Device* device, ID3D11Texture2D* texture, uint64_t sequence, unsigned eye) noexcept {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(callbacks);
    if (!current) return;
    auto& probe = *current;
    const uint64_t now = nowQpc();
    synchronizeLocked(probe, now);
    if (GetCurrentThreadId() != probe.ownerThread) { rejectLocked(probe, Refusal::WrongThread); return; }
    if (!device || device != probe.device.Get() || !texture) { rejectLocked(probe, Refusal::TextureUnsupported); return; }
    if (!probe.pairs.observe(sequence, eye)) { rejectLocked(probe, Refusal::InvalidRequest); return; }
    // Even malformed or cancelled UI requests must never fall through to the
    // separately requested owned-marker path.
    if (probe.shared->responseKind == uint64_t(ResponseKind::GameUi) && !probe.ui.active) return;
    const bool uiMode = probe.ui.active;
    if (uiMode) {
        pollUiLocked(probe, now);
        if (!probe.ui.active || (probe.ui.baseline && !probe.ui.prepared)) return;
        if (probe.ui.prepared && probe.pulse.phase() != Phase::Consumed) return;
        if (probe.ui.prepared && !probe.pairs.allowsResponse(sequence)) return;
    } else {
        if (probe.pulse.phase() != Phase::Consumed && probe.pulse.phase() != Phase::Rendering) return;
        if (probe.pulse.phase() == Phase::Consumed && !probe.pairs.allowsResponse(sequence)) return;
    }
    if (FAILED(device->GetDeviceRemovedReason())) { rejectLocked(probe, Refusal::DeviceRemoved); return; }
    ComPtr<ID3D11Device> textureDevice;
    texture->GetDevice(&textureDevice);
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (uiMode && !(probe.textureNotes & (1u << eye))) {
        probe.textureNotes |= 1u << eye;
        Log::get().note("input_latency_ui_texture,eye=%u,format=%u,width=%u,height=%u,mips=%u,array=%u,samples=%u,bind=0x%X",
            eye, unsigned(description.Format), description.Width, description.Height, description.MipLevels,
            description.ArraySize, description.SampleDesc.Count, description.BindFlags);
    }
    const DXGI_FORMAT format = responseFormat(description.Format);
    if (textureDevice.Get() != device || format == DXGI_FORMAT_UNKNOWN || description.Width < 24 || description.Height < 24 ||
        description.MipLevels != 1 || description.ArraySize != 1 || description.SampleDesc.Count != 1 ||
        !(description.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS))) {
        rejectLocked(probe, Refusal::TextureUnsupported); return;
    }
    if (uiMode) {
        copyUiEyeLocked(probe, texture, description, sequence, eye);
        return;
    }
    if (eye < 2 && probe.responseTextures[1u - eye].Get() == texture) {
        // Bounds are intentionally absent from this capability. A shared eye
        // atlas cannot identify a safe per-eye response rectangle here.
        rejectLocked(probe, Refusal::TextureUnsupported); return;
    }
    if (!probe.pulse.render(sequence, eye, now)) { publishLocked(probe); return; }
    probe.responseTextures[eye] = texture;
    D3D11_RENDER_TARGET_VIEW_DESC viewDescription{};
    viewDescription.Format = format; viewDescription.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> renderTarget;
    ComPtr<ID3D11UnorderedAccessView> unordered;
    ID3D11View* view = nullptr;
    if ((description.BindFlags & D3D11_BIND_RENDER_TARGET) &&
        SUCCEEDED(device->CreateRenderTargetView(texture, &viewDescription, &renderTarget))) view = renderTarget.Get();
    if (!view && (description.BindFlags & D3D11_BIND_UNORDERED_ACCESS)) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC unorderedDescription{};
        unorderedDescription.Format = format; unorderedDescription.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (SUCCEEDED(device->CreateUnorderedAccessView(texture, &unorderedDescription, &unordered))) view = unordered.Get();
    }
    if (!view) { rejectLocked(probe, Refusal::GraphicsFailure); return; }
    const FLOAT white[]{1.0f, 1.0f, 1.0f, 1.0f};
    const D3D11_RECT rectangle{8, 8, 24, 24};
    // ClearView accepts RTVs and UAVs and limits writes to the supplied rect:
    // https://learn.microsoft.com/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-clearview
    // It acts on the view directly without binding pipeline state.
    probe.context1->ClearView(view, white, &rectangle, 1);
    if (probe.pulse.result().eyeMask == 3) {
        if (probe.fenceValue == UINT64_MAX - 1 ||
            FAILED(probe.context4->Signal(probe.fence.Get(), ++probe.fenceValue))) {
            rejectLocked(probe, Refusal::GraphicsFailure); return;
        }
        probe.pendingFence = probe.fenceValue; probe.pendingRequest = probe.pulse.result().request;
        if (FAILED(probe.fence->SetEventOnCompletion(probe.pendingFence, probe.fenceEvent))) {
            rejectLocked(probe, Refusal::GraphicsFailure); return;
        }
    }
    publishLocked(probe);
}

void eyeSubmitted(uint64_t sequence, unsigned eye, bool accepted) noexcept {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lock(callbacks);
    if (!current) return;
    auto& probe = *current;
    const uint64_t now = nowQpc();
    synchronizeLocked(probe, now);
    if (probe.ui.active) {
        auto* frame = findUiFrame(probe.ui, sequence);
        if (!frame) return; // a pre-consumption or partial-pair boundary
        if (eye > 1) { rejectLocked(probe, Refusal::InvalidRequest); return; }
        const bool duplicate = (frame->receipt.submitMask & (uint64_t(1) << eye)) != 0;
        if (!frame->receipt.submit(eye, accepted, now))
            rejectLocked(probe, duplicate ? Refusal::DuplicateEye : Refusal::SubmitFailed);
        return;
    }
    if (probe.pulse.submit(sequence, eye, accepted, now) || probe.pulse.terminal()) publishLocked(probe);
}

} // namespace edvr::input_latency_probe

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
    }
    return "invalid_reason";
}

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
            "sent_to_consumed_ms=%.4f,consumed_to_gpu_notification_upper_bound_ms=%.4f,consumed_to_submit_ms=%.4f,photons=unmeasured",
            result.request, unsigned(probe.mode), probe.generation, result.sequence, result.source,
            double(result.consumedQpc - result.sentQpc) * scale,
            double(result.gpuCompleteQpc - result.consumedQpc) * scale,
            double(result.submitCompleteQpc - result.consumedQpc) * scale);
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
    if (probe.pulse.phase() == Phase::Rejected) probe.pendingFence = probe.pendingRequest = 0;
    if (probe.pulse.terminal()) {
        probe.responseTextures[0].Reset(); probe.responseTextures[1].Reset();
    }
    writeWord(shared.state, uint64_t(probe.pulse.phase()));
    reportLocked(probe);
}

void rejectLocked(Probe& probe, Refusal reason) noexcept {
    if (probe.pulse.active()) {
        probe.pulse.reject(reason);
        publishLocked(probe);
    } else if (readWord(probe.shared->state) == uint64_t(Phase::Reserved)) {
        // Reserved request words still belong to sender. Do not read or log
        // partially written payload; its Reserved -> Armed CAS must now fail.
        probe.shared->consumedQpc = probe.shared->renderSequence = 0;
        probe.shared->eyeMask = probe.shared->submitMask = 0;
        probe.shared->gpuCompleteQpc = probe.shared->submitCompleteQpc = 0;
        probe.shared->source = 0; probe.shared->reason = uint64_t(reason);
        writeWord(probe.shared->state, uint64_t(Phase::Rejected));
        if (probe.reports++ < 256)
            Log::get().note("input_latency_probe,state=rejected,request=unpublished,reason=%s", reasonWord(reason));
    }
}

// Import one release-published external request and bound its entire lifetime
// from SendInput's QPC stamp. No heartbeat or mouse packet is required for expiry.
void synchronizeLocked(Probe& probe, uint64_t now) noexcept {
    const auto phase = Phase(readWord(probe.shared->state));
    if (phase == Phase::Reserved) {
        if (probe.pulse.active()) { rejectLocked(probe, Refusal::InvalidRequest); return; }
        if (!probe.reservationAt) probe.reservationAt = now;
        if (!now || now < probe.reservationAt || now - probe.reservationAt > probe.frequency * 2)
            rejectLocked(probe, Refusal::Timeout);
        return;
    }
    probe.reservationAt = 0;
    if (phase == Phase::Armed && !probe.pulse.active()) {
        probe.reported = false;
        probe.responseTextures[0].Reset(); probe.responseTextures[1].Reset();
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
        if (!probe.pendingFence) continue;
        // Fence notification may precede submit acceptance. Preserve its own
        // QPC endpoint, then await the second accepted submit independently.
        const uint64_t completed = probe.fence->GetCompletedValue();
        if (completed == UINT64_MAX) { rejectLocked(probe, Refusal::DeviceRemoved); continue; }
        if (completed < probe.pendingFence) continue; // an older rejected pulse's event
        // Read QPC only after verifying this pulse's completed fence value.
        // A stale event cannot put the upper bound before current completion.
        const uint64_t observed = nowQpc();
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
    if (!current || !device || (source != 1 && source != 2)) return;
    auto& probe = *current;
    const uint64_t now = nowQpc();
    synchronizeLocked(probe, now);
    const uintptr_t identity = reinterpret_cast<uintptr_t>(device);
    const bool edge = probe.buttons.press(identity, pressed);
    writeWord(probe.shared->ready, !pressed && probe.buttons.released(identity) ? 1 : 0);
    if (edge && probe.pulse.consume(now, source)) {
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
    if (probe.pulse.phase() != Phase::Consumed && probe.pulse.phase() != Phase::Rendering) return;
    if (probe.pulse.phase() == Phase::Consumed && !probe.pairs.allowsResponse(sequence)) return;
    if (FAILED(device->GetDeviceRemovedReason())) { rejectLocked(probe, Refusal::DeviceRemoved); return; }
    ComPtr<ID3D11Device> textureDevice;
    texture->GetDevice(&textureDevice);
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    const DXGI_FORMAT format = responseFormat(description.Format);
    if (textureDevice.Get() != device || format == DXGI_FORMAT_UNKNOWN || description.Width < 24 || description.Height < 24 ||
        description.MipLevels != 1 || description.ArraySize != 1 || description.SampleDesc.Count != 1 ||
        !(description.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS))) {
        rejectLocked(probe, Refusal::TextureUnsupported); return;
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
    if (probe.pulse.submit(sequence, eye, accepted, now) || probe.pulse.terminal()) publishLocked(probe);
}

} // namespace edvr::input_latency_probe

// Production session/provider integration on private WARP devices. No game,
// swap chain, hardware driver policy, SDK worker or filesystem logging.
#include "../../src/common/system_d3d11.h"
#include "../../src/common/log.h"
#include "../../src/common/config.h"
#include "../../src/d3d11/native_reflex_engine_hook.h"
#include "../../src/d3d11/native_reflex_pcl.h"

#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace rig {
struct Mark { uint32_t marker; uint64_t token; DWORD thread; };
std::mutex marksMutex;
std::vector<Mark> marks;
std::vector<uint64_t> inputFrames;
std::atomic<const edvr::native_reflex::FrameBoundaryCallbacks*> enrolled{nullptr};
thread_local uint64_t lateToken = 0;
unsigned initialized = 0, shutDown = 0, inputInstalls = 0, detailRows = 0;
bool collectDetail = false, messageObserverReady = true;
}

namespace edvr {
Log& Log::get() { static Log sink; return sink; }
Log::~Log() = default;
void Log::note(const char* format, ...) {
    if (std::strncmp(format, "native_reflex_frame,session=", 28) == 0) ++rig::detailRows;
}
Config& Config::get() { static Config settings; return settings; }
bool Config::getBool(const char* key, bool fallback) const {
    return std::strcmp(key, "advanced.app_gpu_timing") == 0 ? rig::collectDetail : fallback;
}
void breadcrumb(const char*) {}
bool inputGateInstallMessageObserver() { return rig::messageObserverReady; }
void inputGateInstall() { ++rig::inputInstalls; }
}
namespace edvr::native_reflex {
bool installFrameBoundaryHooks() noexcept { return true; }
void setFrameBoundaryCallbacks(const FrameBoundaryCallbacks* value) noexcept { rig::enrolled.store(value); }
const char* frameBoundaryHookStatus() noexcept { return "session_test"; }
uint64_t currentLateRenderFrameToken() noexcept { return rig::lateToken; }
FrameBoundaryStats frameBoundaryStats() noexcept {
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    FrameBoundaryStats result{}; result.installed = true; result.qpcFrequency = uint64_t(frequency.QuadPart);
    return result;
}
namespace pcl {
bool initialize() noexcept { ++rig::initialized; return true; }
bool available() noexcept { return true; }
void shutdown() noexcept { ++rig::shutDown; }
void mark(uint32_t marker, uint64_t token) noexcept {
    std::lock_guard<std::mutex> lock(rig::marksMutex);
    rig::marks.push_back({marker, token, GetCurrentThreadId()});
}
void inputFrame(uint64_t token) noexcept {
    std::lock_guard<std::mutex> lock(rig::marksMutex); rig::inputFrames.push_back(token);
}
}
}

// Include the production owner so the rig can read locked record snapshots.
// Frame observations use the public lifecycle/provider APIs and enrolled
// callbacks. Capture tests reset the polling timer between simulated polls.
#include "../../src/d3d11/native_reflex_session.cpp"

namespace {
using Microsoft::WRL::ComPtr;
using namespace edvr::native_reflex;
unsigned checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", name); }
}
struct Graphics {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context, deferred;
    ComPtr<ID3D11Texture2D> eye[2];
    ComPtr<ID3D11RenderTargetView> target[2];
};
bool createGraphics(Graphics& graphics) {
    const auto create = edvr::systemD3D11CreateDevice();
    const HRESULT hr = create ? create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &graphics.device, nullptr, &graphics.context) : E_FAIL;
    check(SUCCEEDED(hr) && graphics.device && graphics.context, "a private system WARP device is available");
    if (FAILED(hr) || !graphics.device || !graphics.context) return false;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 16; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (unsigned eye = 0; eye < 2; ++eye) {
        if (FAILED(graphics.device->CreateTexture2D(&desc, nullptr, &graphics.eye[eye])) ||
            FAILED(graphics.device->CreateRenderTargetView(graphics.eye[eye].Get(), nullptr, &graphics.target[eye]))) {
            check(false, "WARP owns real independent eye textures and targets"); return false;
        }
    }
    const bool deferred = SUCCEEDED(graphics.device->CreateDeferredContext(0, &graphics.deferred));
    check(deferred, "the same WARP device exposes a deferred context for rejection checks");
    return deferred;
}
EdvrNativeReflexFrameTable emptyTable() {
    EdvrNativeReflexFrameTable table{};
    table.size = sizeof(table); table.version = EDVR_NATIVE_REFLEX_FRAME_VERSION_1;
    return table;
}
HRESULT acquire(ID3D11Device* device, uint64_t generation, EdvrNativeReflexFrameTable& table) {
    const EdvrNativeReflexFrameRequest request{sizeof(request), EDVR_NATIVE_REFLEX_FRAME_VERSION_1, device, generation};
    return edvrAcquireNativeReflexFrame(&request, &table);
}
FrameRecord snapshot(uint64_t token) {
    auto session = activeSession();
    if (!session) return {};
    std::lock_guard<std::mutex> lock(session->recordsMutex);
    const auto* frame = findFrame(*session, token);
    return frame ? *frame : FrameRecord{};
}
std::vector<uint32_t> markerOrder(uint64_t token) {
    std::lock_guard<std::mutex> lock(rig::marksMutex);
    std::vector<uint32_t> result;
    for (const auto& value : rig::marks) if (value.token == token) result.push_back(value.marker);
    return result;
}
void issueCommand(Graphics& graphics, ID3D11DeviceContext* context = nullptr) {
    if (!context) context = graphics.context.Get();
    const FLOAT color[]{0.1f, 0.2f, 0.3f, 1};
    observeFrameCommand(context);
    context->ClearRenderTargetView(graphics.target[0].Get(), color);
}
struct LateScope {
    const FrameBoundaryCallbacks* callbacks;
    uint64_t token;
    LateScope(const FrameBoundaryCallbacks* value, uint64_t id) : callbacks(value), token(id) {
        rig::lateToken = token; callbacks->lateWorkerBegin(callbacks->context, token);
    }
    void end() {
        if (!token) return;
        callbacks->lateWorkerEnd(callbacks->context, token); rig::lateToken = 0; token = 0;
    }
    ~LateScope() { end(); }
};
struct Sources { EdvrNativeReflexSource eye[2]{}; bool stamped = false; };
Sources submitPair(const EdvrNativeReflexFrameTable& table, Graphics& graphics, uint64_t pose) {
    Sources result;
    const HRESULT left = table.eyeSource(table.context, graphics.eye[0].Get(), 0, pose, &result.eye[0]);
    const HRESULT right = table.eyeSource(table.context, graphics.eye[1].Get(), 1, pose, &result.eye[1]);
    result.stamped = left == S_OK && right == S_OK;
    if (left == S_OK) table.eyeSubmitted(table.context, 0, pose, 1);
    if (right == S_OK) table.eyeSubmitted(table.context, 1, pose, 1);
    return result;
}
const FrameBoundaryCallbacks* beginTick(uint64_t token, bool finishSimulation = true) {
    const auto* callbacks = rig::enrolled.load();
    if (!callbacks) { check(false, "session enrolls its engine callback owner"); return nullptr; }
    callbacks->beforeTick(callbacks->context, token);
    callbacks->inputBegin(callbacks->context, token);
    if (finishSimulation) callbacks->normalPhaseReturned(callbacks->context, token);
    return callbacks;
}

void acquisition(Graphics& graphics, Graphics& foreign, EdvrNativeReflexFrameTable& table) {
    mouseMessageRemoved(true); mouseMessageRemoved(false);
    check(!activeSession() && markerOrder(90).empty(), "mouse messages without a session cannot create telemetry or a pending frame");
    auto missing = emptyTable();
    check(acquire(graphics.device.Get(), 41, missing) == E_PENDING, "provider refuses enrollment without an active native session");
    const auto* driver = startFrameSession(graphics.device.Get(), 41);
    check(!driver && activeSession() && !activeSession()->driver && rig::enrolled.load() && rig::initialized == 1 &&
          rig::inputInstalls == 1, "WARP retains a PCL-only production session without a hardware Reflex driver");
    auto bad = emptyTable();
    EdvrNativeReflexFrameRequest request{sizeof(request), 1, graphics.device.Get(), 41};
    check(edvrAcquireNativeReflexFrame(nullptr, &bad) == E_INVALIDARG &&
          edvrAcquireNativeReflexFrame(&request, nullptr) == E_INVALIDARG, "provider rejects null request and table pointers");
    request.version = 2;
    check(edvrAcquireNativeReflexFrame(&request, &bad) == E_INVALIDARG, "provider rejects an unknown request version");
    request.version = 1; request.size = sizeof(request) - 1;
    check(edvrAcquireNativeReflexFrame(&request, &bad) == E_INVALIDARG, "provider rejects a truncated request");
    bad.version = 2;
    check(acquire(graphics.device.Get(), 41, bad) == E_INVALIDARG, "provider rejects an unknown output table version");
    bad = emptyTable(); bad.size = sizeof(bad) - 1;
    check(acquire(graphics.device.Get(), 41, bad) == E_INVALIDARG, "provider rejects a truncated output table");
    bad = emptyTable();
    check(acquire(foreign.device.Get(), 41, bad) == E_PENDING && acquire(graphics.device.Get(), 40, bad) == E_PENDING,
          "provider requires exact active device identity and native generation");
    check(acquire(graphics.device.Get(), 41, table) == S_OK && table.context && table.poseConsumed && table.eyeSource &&
          table.eyeSubmitted && table.close, "matching enrollment returns the complete fixed provider table");
    auto duplicate = emptyTable();
    check(acquire(graphics.device.Get(), 41, duplicate) == E_PENDING, "a native session admits one active source provider");
    void* invalid = reinterpret_cast<void*>(uintptr_t(1));
    EdvrNativeReflexSource source{};
    check(table.poseConsumed(invalid, 1) == 0 && table.close(invalid) == E_INVALIDARG &&
          table.eyeSource(invalid, graphics.eye[0].Get(), 0, 1, &source) == E_PENDING && !validSource(source),
          "unissued contexts cannot address a provider session or manufacture a source");
}

void clickAttribution() {
    mouseMessageRemoved(false);
    mouseMessageRemoved(true); mouseMessageRemoved(true);
    check(activeSession()->pendingFlash.load() && markerOrder(90).empty(),
          "an actual press only queues a per-session ticket and repeated down does not emit a marker");
    const auto* callbacks = beginTick(90, false);
    if (!callbacks) return;
    check(markerOrder(90) == std::vector<uint32_t>({0, 7}) && !activeSession()->pendingFlash.load(),
          "the qualified InputManager boundary emits the pending flash after SimulationStart");
    mouseMessageRemoved(true);
    callbacks->inputBegin(callbacks->context, 90);
    check(markerOrder(90) == std::vector<uint32_t>({0, 7}) && !activeSession()->pendingFlash.load(),
          "a repeated down while held cannot queue a second flash");
    mouseMessageRemoved(false); mouseMessageRemoved(true);
    callbacks->inputBegin(callbacks->context, 90);
    check(activeSession()->pendingFlash.load() && markerOrder(90) == std::vector<uint32_t>({0, 7}),
          "another real edge in an already marked frame waits for the next input iteration");
    callbacks->normalPhaseReturned(callbacks->context, 90);
    callbacks->afterTick(callbacks->context, 90);
    callbacks = beginTick(91);
    if (!callbacks) return;
    check(markerOrder(91) == std::vector<uint32_t>({0, 7, 1}) && !activeSession()->pendingFlash.load(),
          "the next frame owns the next click between its own simulation markers");
    callbacks->afterTick(callbacks->context, 91);
    mouseMessageRemoved(false);
    callbacks = beginTick(92);
    if (!callbacks) return;
    check(markerOrder(92) == std::vector<uint32_t>({0, 1}), "a release alone never triggers a flash");
    mouseMessageRemoved(true);
    callbacks->inputBegin(callbacks->context, 999);
    callbacks->inputBegin(callbacks->context, 92);
    check(activeSession()->pendingFlash.load() && markerOrder(92) == std::vector<uint32_t>({0, 1}),
          "foreign or already ended simulation boundaries cannot consume the next click");
    callbacks->afterTick(callbacks->context, 92);
    callbacks = beginTick(93);
    if (!callbacks) return;
    check(markerOrder(93) == std::vector<uint32_t>({0, 7, 1}), "a click after simulation end remains pending for the next actual input");
    callbacks->afterTick(callbacks->context, 93);
    mouseMessageRemoved(false);
}

// Drive the public source callbacks on the qualified thread, then return
// the same retained stamps a successful two-eye desktop draw would select.
EdvrNativeReflexSource prepareMirrorSource(Graphics& graphics, const EdvrNativeReflexFrameTable& table,
                                          uint64_t token) {
    const uint64_t pose = token + 1000;
    const auto* callbacks = beginTick(token);
    if (!callbacks) return {};
    Sources pair;
    {
        LateScope scope(callbacks, token);
        check(table.poseConsumed(table.context, pose) == token, "the mirror fixture retains its exact pose owner");
        issueCommand(graphics);
        pair = submitPair(table, graphics, pose);
    }
    callbacks->afterTick(callbacks->context, token);
    return selectMirrorSource(pair.eye, 3, 3, {});
}

void failedPresentAndRetry(Graphics& graphics, const EdvrNativeReflexFrameTable& table) {
    constexpr uint64_t token = 94;
    auto session = activeSession();
    const auto source = prepareMirrorSource(graphics, table, token);
    const auto first = beginFramePresent(graphics.device.Get(), source);
    check(first.owner && !beginFramePresent(graphics.device.Get(), source).owner &&
          !endFramePresent(first, DXGI_ERROR_WAS_STILL_DRAWING),
          "a pending or failed nonblocking Present cannot consume the source");
    check(!snapshot(token).completed && !snapshot(token).presentEndQpc &&
          !session->completedFrames.load() && !session->lastCompletedMs.load(),
          "a failed real call leaves completion, successful timestamps and capability freshness untouched");
    const auto second = beginFramePresent(graphics.device.Get(), source);
    check(second.owner && second.attempt != first.attempt && !endFramePresent(first, S_OK) &&
          !endFramePresent(second, DXGI_STATUS_OCCLUDED) && !snapshot(token).completed &&
          !session->lastCompletedMs.load(),
          "stale receipts and positive occlusion status cannot complete a pending retry");
    const auto third = beginFramePresent(graphics.device.Get(), source);
    check(endFramePresent(third, S_OK) && snapshot(token).completed && snapshot(token).presentEndQpc &&
          session->completedFrames.load() == 1 && !session->lastCompletedMs.load(),
          "a successful retry supplies CPU evidence without promoting the first attempt's driver timestamps");
    check(markerOrder(token) == std::vector<uint32_t>({0,1,2,3,4,5}) &&
          !endFramePresent(third, S_OK) && !beginFramePresent(graphics.device.Get(), source).owner,
          "exactly one SDK Present bracket survives retries and a successful source cannot be reused");
}

void abortedWorkers(Graphics& graphics, const EdvrNativeReflexFrameTable& table) {
    constexpr uint64_t token = 95;
    const auto* callbacks = beginTick(token, false);
    if (!callbacks) return;
    const auto starts = activeSession()->completedTicks.load();
    rig::lateToken = token;
    callbacks->lateWorkerBegin(callbacks->context, token);
    check(frameCommandObserver && table.poseConsumed(table.context, 1095) == token,
          "an aborted worker starts with an armed post-pose command observer");
    callbacks->lateWorkerAborted(callbacks->context, token);
    rig::lateToken = 0;
    callbacks->tickAborted(callbacks->context, token);
    check(!frameCommandObserver && !late.token && !tick.token && snapshot(token).invalid && !snapshot(token).lateEnded &&
          markerOrder(token) == std::vector<uint32_t>({0}) && activeSession()->completedTicks.load() == starts,
          "unwind clears worker and tick ownership without emitting successful return markers");
    const auto nextSource = prepareMirrorSource(graphics, table, token + 1);
    check(validSource(nextSource) && snapshot(token + 1).lateEnded && snapshot(token + 1).firstCommandQpc,
          "the same worker accepts the following source and records its first command");
}

void detailedCapture(Graphics& graphics, const EdvrNativeReflexFrameTable& table) {
    auto session = activeSession();
    const auto poll = [&] { session->lastPollMs = 0; reportFrameSession(session->generation); };
    poll();
    check(!rig::detailRows, "default settings keep completed per-frame CPU rows out of the log");
    rig::collectDetail = true;
    poll();
    check(!rig::detailRows, "capture begins at enrollment without replaying frames from the prior disabled interval");
    auto source = prepareMirrorSource(graphics, table, 105);
    check(endFramePresent(beginFramePresent(graphics.device.Get(), source), S_OK), "detailed capture receives a complete source");
    poll(); poll();
    check(rig::detailRows == 1, "an opted-in frame produces exactly one CPU record across repeated reports");
    source = prepareMirrorSource(graphics, table, 106);
    check(endFramePresent(beginFramePresent(graphics.device.Get(), source), S_OK), "the unreported source is eligible for rotation");
    const auto dropped = session->softwareDropped.load();
    const uint64_t rotated = 106 + kFrameHistory;
    const auto* callbacks = beginTick(rotated);
    if (callbacks) callbacks->afterTick(callbacks->context, rotated);
    check(session->softwareDropped.load() == dropped + 1, "rotation counts each eligible unreported source once");
    poll();
    check(rig::detailRows == 1, "an overwritten source cannot appear in the software log as well as the drop count");
    rig::collectDetail = false;
    poll();
    source = prepareMirrorSource(graphics, table, rotated + 1);
    check(endFramePresent(beginFramePresent(graphics.device.Get(), source), S_OK), "frames still complete with detailed capture disabled");
    poll();
    check(rig::detailRows == 1, "disabling collection stops new rows while source completion stays active");
}

void completeAndDeduplicate(Graphics& graphics, Graphics& foreign, const EdvrNativeReflexFrameTable& table) {
    constexpr uint64_t token = 101, pose = 1001;
    const auto completedBefore = activeSession()->completedFrames.load();
    mouseMessageRemoved(true);
    const auto* callbacks = beginTick(token);
    if (!callbacks) return;
    callbacks->normalPhaseReturned(callbacks->context, token);
    check(table.poseConsumed(table.context, pose) == 0, "pose receipt requires a qualified active LateRender original");
    LateScope lateScope(callbacks, token);
    rig::lateToken = token + 1;
    check(table.poseConsumed(table.context, pose) == 0, "a different scheduler ticket cannot borrow the LateRender scope");
    rig::lateToken = token;
    check(table.poseConsumed(table.context, pose) == token, "the active job accepts its actual returned render pose");
    EdvrNativeReflexSource premature{};
    check(table.eyeSource(table.context, graphics.eye[0].Get(), 0, pose, &premature) == E_PENDING,
          "pose receipt without the first real owned graphics command cannot stamp an eye");
    issueCommand(foreign); issueCommand(graphics, graphics.deferred.Get());
    check(!(snapshot(token).completedMarkers & (1u << unsigned(Marker::RenderSubmitStart))),
          "foreign-device and deferred-context commands cannot start this render interval");
    issueCommand(graphics); issueCommand(graphics);
    check(snapshot(token).firstCommandQpc != 0, "the first owned immediate-context command starts rendering");
    EdvrNativeReflexSource foreignSource{};
    check(table.eyeSource(table.context, foreign.eye[0].Get(), 0, pose, &foreignSource) == E_INVALIDARG &&
          !validSource(foreignSource), "a real foreign WARP texture cannot receive this session's source");
    Sources sources;
    check(table.eyeSource(table.context, graphics.eye[0].Get(), 0, pose, &sources.eye[0]) == S_OK,
          "the left source is stamped only after its owned render command");
    EdvrNativeReflexSource stored{};
    UINT bytes = sizeof(stored);
    check(graphics.eye[0]->GetPrivateData(kSourceGuid, &bytes, &stored) == S_OK && bytes == sizeof(stored) &&
          sameSource(stored, sources.eye[0]) && stored.generation == 41 && stored.frameToken == token && stored.poseSequence == pose,
          "real WARP GUID private data retains the exact generation, tick and pose");
    table.eyeSubmitted(table.context, 0, pose, 1);
    check(!beginFramePresent(graphics.device.Get(), sources.eye[0]).owner, "one accepted eye cannot own a Present");
    check(table.eyeSource(table.context, graphics.eye[1].Get(), 1, pose, &sources.eye[1]) == S_OK &&
          !beginFramePresent(graphics.device.Get(), sources.eye[0]).owner, "stamping the second eye alone does not establish acceptance");
    table.eyeSubmitted(table.context, 1, pose, 1);
    lateScope.end();
    check(!(snapshot(token).completedMarkers & (1u << unsigned(Marker::RenderSubmitEnd))),
          "LateEnd alone cannot close render submission before the actual mirror draw completes");
    const auto selected = selectMirrorSource(sources.eye, 3, 3, {});
    check(validSource(selected) && readSource(graphics.eye[1].Get(), stored) && sameSource(stored, selected),
          "paired source selection agrees with the second real texture's GUID readback");
    check(!beginFramePresent(foreign.device.Get(), selected).owner, "Present ownership rejects a foreign D3D device");
    auto receipt = beginFramePresent(graphics.device.Get(), selected);
    check(receipt.owner && receipt.frameId == token && !snapshot(token).completed,
          "the actual paired source opens Present but cannot finish before the original returns");
    endFramePresent(receipt, S_OK); endFramePresent(receipt, S_OK);
    check(snapshot(token).completed && activeSession()->completedFrames.load() == completedBefore + 1,
          "all six real boundaries and accepted eyes produce exactly one complete frame");
    check(!beginFramePresent(graphics.device.Get(), selected).owner &&
          markerOrder(token) == std::vector<uint32_t>({0, 7, 1, 2, 3, 4, 5}),
          "duplicate phase, command, Present and return receipts never duplicate ordered PCL markers");
    {
        std::lock_guard<std::mutex> lock(rig::marksMutex);
        check(std::count(rig::inputFrames.begin(), rig::inputFrames.end(), token) == 1,
              "the qualified input callback attributes its pending PCL input ticket exactly once");
    }
    callbacks->afterTick(callbacks->context, token);
    mouseMessageRemoved(false);
}

void refusedSources(Graphics& graphics, const EdvrNativeReflexFrameTable& table) {
    auto wrongCallbacks = beginTick(102);
    if (!wrongCallbacks) return;
    {
        LateScope scope(wrongCallbacks, 102);
        table.poseConsumed(table.context, 1002); issueCommand(graphics);
        EdvrNativeReflexSource wrong{};
        EdvrNativeReflexSource stale{};
        check(table.eyeSource(table.context, graphics.eye[0].Get(), 0, 9999, &wrong) == E_PENDING && !validSource(wrong),
              "a different pose sequence cannot stamp the current rendered eye");
        check(!readSource(graphics.eye[0].Get(), stale) && !validSource(stale),
              "a failed source attempt clears prior-frame GUID metadata on the reused real texture");
        table.eyeSubmitted(table.context, 0, 9999, 1);
    }
    wrongCallbacks->afterTick(wrongCallbacks->context, 102);
    check(!snapshot(102).completed, "a wrong-pose submission never becomes a complete frame");

    const auto* callbacks = beginTick(103);
    if (!callbacks) return;
    EdvrNativeReflexSource refused[2]{};
    {
        LateScope scope(callbacks, 103);
        table.poseConsumed(table.context, 1003); issueCommand(graphics);
        const HRESULT a = table.eyeSource(table.context, graphics.eye[0].Get(), 0, 1003, &refused[0]);
        const HRESULT b = table.eyeSource(table.context, graphics.eye[1].Get(), 1, 1003, &refused[1]);
        check(a == S_OK && b == S_OK, "refused-submit case first obtains a real same-pose stereo source");
        table.eyeSubmitted(table.context, 0, 1003, 1); table.eyeSubmitted(table.context, 1, 1003, 0);
    }
    check(!beginFramePresent(graphics.device.Get(), refused[0]).owner && !snapshot(103).completed,
          "a runtime-refused eye invalidates the pair even when both source stamps exist");
    callbacks->afterTick(callbacks->context, 103);
    EdvrNativeReflexSource mixed[2]{refused[0], refused[1]}; ++mixed[1].frameToken;
    const auto selected = selectMirrorSource(mixed, 3, 3, {});
    check(!validSource(selected) && !beginFramePresent(graphics.device.Get(), selected).owner,
          "mixed tick sources cannot assign a Present to a guessed producer");
}

void overlappingOwners(Graphics& graphics, const EdvrNativeReflexFrameTable& table) {
    constexpr uint64_t token = 104, pose = 1004;
    const auto* callbacks = beginTick(token, false);
    if (!callbacks) return;
    std::promise<Sources> ready;
    auto readyFuture = ready.get_future();
    std::promise<void> release;
    auto releaseFuture = release.get_future();
    std::thread worker([&] {
        LateScope scope(callbacks, token);
        const bool poseAccepted = table.poseConsumed(table.context, pose) == token;
        issueCommand(graphics);
        auto pair = submitPair(table, graphics, pose);
        pair.stamped = pair.stamped && poseAccepted;
        ready.set_value(pair);
        releaseFuture.wait_for(std::chrono::seconds(10));
    });
    const bool workerReady = readyFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    check(workerReady, "the real LateRender worker reaches its independently synchronized source boundary");
    if (workerReady) {
        const auto pair = readyFuture.get();
        const auto rendering = snapshot(token);
        check(pair.stamped && rendering.commandThread != rendering.simulationThread &&
              (rendering.completedMarkers & (1u << unsigned(Marker::RenderSubmitStart))) &&
              !(rendering.completedMarkers & (1u << unsigned(Marker::SimulationEnd))),
              "real worker rendering overlaps an unfinished simulation on a different thread");
        callbacks->normalPhaseReturned(callbacks->context, token);
        const auto selected = selectMirrorSource(pair.eye, 3, 3, {});
        auto receipt = beginFramePresent(graphics.device.Get(), selected);
        check(receipt.owner && !snapshot(token).lateEnded, "Present may begin before the qualified LateRender worker returns");
        endFramePresent(receipt, S_OK);
        check(!snapshot(token).completed && !(snapshot(token).completedMarkers & (1u << unsigned(Marker::RenderSubmitEnd))),
              "Present return still awaits the independent late/render completion boundary");
    }
    release.set_value(); worker.join();
    check(snapshot(token).completed && markerOrder(token) == std::vector<uint32_t>({0, 2, 1, 4, 5, 3}),
          "LateEnd completes genuine overlap without forcing serial Simulation/Render/Present marker order");
    callbacks->afterTick(callbacks->context, token);
}

void staleOwners(Graphics& graphics, EdvrNativeReflexFrameTable oldTable) {
    constexpr uint64_t token = 500;
    const auto* callbacks = beginTick(token);
    if (!callbacks) return;
    Sources oldPair;
    {
        LateScope scope(callbacks, token);
        oldTable.poseConsumed(oldTable.context, 1500); issueCommand(graphics);
        oldPair = submitPair(oldTable, graphics, 1500);
    }
    auto oldReceipt = beginFramePresent(graphics.device.Get(), selectMirrorSource(oldPair.eye, 3, 3, {}));
    check(oldReceipt.owner != nullptr, "old session retains a real in-flight Present receipt");
    callbacks->afterTick(callbacks->context, token);
    mouseMessageRemoved(false); mouseMessageRemoved(true);
    check(activeSession()->pendingFlash.load(), "the old session can still have a click pending when it closes");
    stopFrameSession(41);
    check(!rig::enrolled.load() && !activeSession(), "normal session close detaches the callback owner");
    startFrameSession(graphics.device.Get(), 42);
    EdvrNativeReflexFrameTable table = emptyTable();
    check(acquire(graphics.device.Get(), 42, table) == S_OK && table.context != oldTable.context,
          "new generation receives a distinct retained provider context");
    stopFrameSession(41);
    check(activeSession() && activeSession()->generation == 42 && oldTable.close(oldTable.context) == S_OK &&
          oldTable.close(oldTable.context) == S_FALSE && activeSession()->generation == 42,
          "stale generation close and old provider close cannot address the replacement session");
    callbacks = beginTick(token);
    if (!callbacks) return;
    check(!activeSession()->pendingFlash.load() && !activeSession()->leftButtonDown.load() &&
          !(snapshot(token).completedMarkers & (1u << unsigned(Marker::TriggerFlash))),
          "a replacement session cannot inherit a pending click or held edge from its predecessor");
    Sources pair;
    {
        LateScope scope(callbacks, token);
        check(oldTable.poseConsumed(oldTable.context, 2500) == 0 && table.poseConsumed(table.context, 2500) == token,
              "only the replacement provider can claim its same-numbered frame");
        issueCommand(graphics); pair = submitPair(table, graphics, 2500);
    }
    endFramePresent(oldReceipt, S_OK);
    check(!snapshot(token).presentEndQpc && !snapshot(token).completed,
          "ending a retained stale receipt cannot finish the replacement's same-numbered frame");
    auto receipt = beginFramePresent(graphics.device.Get(), selectMirrorSource(pair.eye, 3, 3, {}));
    endFramePresent(receipt, S_OK);
    check(snapshot(token).completed && activeSession()->completedFrames.load() == 1,
          "the replacement completes only through its own current source and Present receipt");
    callbacks->afterTick(callbacks->context, token);
    check(table.close(table.context) == S_OK, "normal provider close deactivates its retained context");
    callbacks = beginTick(token + 1);
    if (callbacks) {
        {
            LateScope scope(callbacks, token + 1);
            check(table.poseConsumed(table.context, 2501) == 0 && !snapshot(token + 1).poseSequence,
                  "a closed provider cannot claim a new otherwise-qualified LateRender invocation");
        }
        callbacks->afterTick(callbacks->context, token + 1);
    }
    stopFrameSession(42);
    rig::messageObserverReady = false;
    const auto initializations = rig::initialized;
    startFrameSession(graphics.device.Get(), 43);
    check(activeSession() && !activeSession()->pclReady && rig::initialized == initializations,
          "without passive ping and shutdown observation, a new session starts no SDK worker and keeps PCL unavailable");
    callbacks = beginTick(token + 2);
    if (callbacks) callbacks->afterTick(callbacks->context, token + 2);
    check(markerOrder(token + 2).empty(), "a refused observer cannot reuse a prior process-wide PCL marker sink");
    stopFrameSession(43);
    shutdownFrameTelemetry();
    check(rig::shutDown == 1, "normal process telemetry shutdown reaches the PCL owner");
}
}

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "--dry-run")) {
        std::puts("native_reflex_session_test: dry-run (no D3D device, provider or worker)"); return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--self-test")) return 2;
    Graphics graphics, foreign;
    if (createGraphics(graphics) && createGraphics(foreign)) {
        check(graphics.device.Get() != foreign.device.Get(), "WARP devices have distinct COM identities");
        auto table = emptyTable();
        acquisition(graphics, foreign, table);
        if (table.context && rig::enrolled.load()) {
            clickAttribution();
            failedPresentAndRetry(graphics, table);
            abortedWorkers(graphics, table);
            completeAndDeduplicate(graphics, foreign, table);
            refusedSources(graphics, table);
            overlappingOwners(graphics, table);
            detailedCapture(graphics, table);
            staleOwners(graphics, table);
        }
    }
    stopFrameSession(41); stopFrameSession(42);
    std::printf("native_reflex_session_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

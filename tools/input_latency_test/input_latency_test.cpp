#include "../../src/d3d11/input_latency_state.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <cwchar>
#include "../../src/common/system_d3d11.h"
#include "../../src/common/log.h"
namespace edvr {
Log& Log::get() { static Log sink; return sink; }
Log::~Log() = default;
void Log::note(const char* format, ...) {
    va_list arguments; va_start(arguments, format);
    std::vprintf(format, arguments); std::putchar('\n'); va_end(arguments);
}
}
// Exercise the real owner, worker, format conversion and staging readback.
// The native rig compiles this TU alone; do not also link the probe separately.
#include "../../src/d3d11/input_latency_probe.cpp"
#endif

using namespace edvr::input_latency_probe;

namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", name); }
}

Pulse consumed(uint64_t request = 1) {
    Pulse pulse;
    check(pulse.arm(request, 100, 101, 1000), "a unique published request arms");
    check(pulse.consume(110, 1), "actual state receipt consumes armed request");
    return pulse;
}

void edges() {
    ButtonEdges buttons;
    check(!buttons.released(1), "new probe is not ready before actual released packet");
    check(!buttons.press(1, true) && !buttons.press(1, true), "held button cannot manufacture initial edge");
    check(!buttons.press(1, false), "release is observation only");
    check(buttons.released(1), "actual released packet makes that mouse ready");
    check(!buttons.press(2, true), "another mouse cannot borrow observed release");
    check(buttons.press(1, true), "release followed by same mouse press is one edge");
    check(!buttons.released(1), "pressed receipt clears release readiness");
    check(!buttons.press(1, true), "buffered and state reports of same press deduplicate");
    check(!buttons.press(1, false) && !buttons.press(1, false) && buttons.press(1, true),
          "repeated release reports enable only one subsequent edge");
    buttons.reset();
    check(!buttons.released(1), "configuration reset clears release readiness");
    buttons.press(1, false); buttons.press(2, false); buttons.invalidate(1);
    check(!buttons.released(1) && buttons.released(2) && buttons.anyReleased(), "read loss invalidates only the affected device");
    buttons.invalidate(2);
    check(!buttons.anyReleased(), "last device's read loss clears readiness");
    check(!buttons.press(1, true) && !buttons.press(0, false) && !buttons.press(0, true),
          "generation reset and missing device both require new real release");
    for (uintptr_t device = 1; device <= 16; ++device) buttons.press(device, false);
    check(!buttons.press(17, false) && !buttons.press(17, true), "bounded device table fails closed when full");
}

void keyboardCache() {
    KeyboardReleaseCache cache;
    cache.observe(1, 0xC8, false, 110, 1); cache.observe(1, 0xD0, false, 110, 1);
    check(!cache.acknowledge(100, 0xD0), "released states alone cannot acknowledge a fresh priming cycle");
    check(cache.observe(1, 0xD0, true, 120, 0) == KeyboardPress::None && !cache.acknowledge(100, 0xD0),
          "unqualified input cannot update keyboard release or cycle knowledge");
    cache.observe(1, 0xD0, true, 120, 2); cache.observe(1, 0xD0, true, 121, 2);
    cache.observe(1, 0xD0, false, 130, 3); cache.observe(1, 0xC8, true, 140, 4); cache.observe(1, 0xC8, false, 150, 5);
    check(cache.acknowledge(100, 0xD0) == 1 && cache.released(1, 100) && cache.releaseTick(1) == 5,
          "actual ordered DOWN then UP cycles acknowledge one device and retain the final release tick");
    cache.observe(1, 0xD0, true, 160, 6); cache.observe(1, 0xD0, false, 170, 7);
    check(cache.acknowledge(100, 0xC8) == 1 && cache.releaseTick(1) == 7,
          "measured alternating requests reuse the acknowledged run fence while keeping later release freshness");
    check(!cache.acknowledge(200, 0xD0), "an old release certificate cannot acknowledge a new program's priming fence");
    check(cache.invalidate(1) && !cache.released(1, 100) && !cache.acknowledge(100, 0xD0),
          "device or read loss erases its priming certificate and release knowledge");
    KeyboardReleaseCache other;
    check(other.observe(2, 0xC8, true, 120, 1) == KeyboardPress::UnverifiedDown &&
          other.observe(2, 0xC8, true, 121, 1) == KeyboardPress::None &&
          other.observe(2, 0xC8, false, 122, 1) == KeyboardPress::None &&
          other.observe(2, 0xC8, true, 123, 1) == KeyboardPress::FromRelease,
          "first down may interfere, repeats and release do not, and a subsequent press has release proof");
    KeyboardReleaseCache batch;
    for (auto event : {std::pair<unsigned,bool>{0xD0,true}, {0xD0,false}, {0xC8,true}, {0xC8,false}})
        batch.observe(2, event.first, event.second, 500, 99);
    check(batch.acknowledge(499, 0xD0) == 2 && batch.releaseTick(2) == 99,
          "one coalesced buffered read with equal QPC and token retains actual row order for fresh acknowledgement");
    KeyboardReleaseCache reverse;
    for (auto event : {std::pair<unsigned,bool>{0xC8,true}, {0xC8,false}, {0xD0,true}, {0xD0,false}})
        reverse.observe(2, event.first, event.second, 500, 99);
    check(!reverse.acknowledge(499, 0xD0) && reverse.acknowledge(499, 0xC8) == 2,
          "equal timestamps cannot hide reversed priming order and UP-first remains supported");
    KeyboardReleaseCache split;
    split.observe(1, 0xD0, true, 500, 99); split.observe(1, 0xD0, false, 500, 99);
    split.observe(2, 0xC8, true, 500, 99); split.observe(2, 0xC8, false, 500, 99);
    check(!split.acknowledge(499, 0xD0), "different devices cannot combine their released keys into a priming certificate");
    split.observe(1, 0xC8, true, 510, 100); split.invalidate(1); split.observe(1, 0xC8, false, 520, 101);
    check(!split.acknowledge(499, 0xD0), "a release after read loss cannot borrow a discarded press");
}

void pairBoundary() {
    PairBoundary pairs;
    pairs.observe(70, 0);
    auto pulse = consumed();
    pairs.consumed();
    pairs.observe(70, 1);
    check(!pairs.allowsResponse(70) && pulse.phase() == Phase::Consumed && !pulse.result().eyeMask,
          "input after left eye defers remaining right eye without painting partial pair");
    check(!pulse.submit(70, 1, true, 115) && pulse.phase() == Phase::Consumed,
          "skipped partial pair submission cannot qualify pending response");
    pairs.observe(71, 0);
    check(pairs.allowsResponse(71) && pulse.render(71, 0, 120), "next whole pair may start response");
    pulse.submit(71, 0, true, 130); pairs.observe(71, 1);
    check(pairs.allowsResponse(71) && pulse.render(71, 1, 140) && pulse.submit(71, 1, true, 150) &&
          pulse.gpuCompleted(160) && pulse.phase() == Phase::Complete && pulse.result().sequence == 71,
          "deferred pulse completes only in next same-sequence full stereo pair");
    pairs.consumed();
    check(!pairs.allowsResponse(71) && pairs.allowsResponse(72), "already completed pair is also excluded at later consumption");
    pairs.reset(); pairs.consumed();
    check(pairs.allowsResponse(1) && !pairs.observe(0, 0) && !pairs.observe(1, 2),
          "empty boundary admits first valid pair and rejects malformed observations");
}

void completeOrders() {
    for (bool gpuFirst : {false, true}) {
        auto pulse = consumed();
        check(pulse.render(77, 1, 120) && pulse.submit(77, 1, true, 130),
              "right eye may render and submit before left eye");
        check(pulse.render(77, 0, 140), "second response retains same exact sequence");
        if (gpuFirst) {
            check(pulse.gpuCompleted(150) && pulse.phase() == Phase::Rendering,
                  "GPU notification alone does not qualify paired submit");
            check(pulse.submit(77, 0, true, 160), "last accepted eye completes after GPU notification");
        } else {
            check(pulse.submit(77, 0, true, 150) && pulse.phase() == Phase::Rendering,
                  "paired submit alone does not qualify GPU completion");
            check(pulse.gpuCompleted(160), "GPU notification completes after paired submit");
        }
        const auto result = pulse.result();
        check(pulse.phase() == Phase::Complete && result.reason == Refusal::None && result.request == 1 &&
              result.sentQpc == 100 && result.consumedQpc == 110 && result.sequence == 77 &&
              result.eyeMask == 3 && result.submitMask == 3 && result.gpuCompleteQpc >= 150 &&
              result.submitCompleteQpc >= 150 && result.source == 1, "complete retains every causal receipt and independent endpoint");
        check(!pulse.consume(170, 2) && !pulse.gpuCompleted(170) && !pulse.submit(77, 0, true, 170),
              "terminal pulse never completes twice");
        check(!pulse.arm(1, 180, 180, 1000) && pulse.result().reason == Refusal::InvalidRequest,
              "old unique request cannot be replayed after complete");
        check(pulse.arm(2, 180, 180, 1000) && pulse.consume(185, 2) && pulse.result().source == 2,
              "new request permits a distinct buffered input receipt");
    }
}

void refusals() {
    auto pulse = consumed();
    check(!pulse.arm(2, 115, 115, 1000) && pulse.result().request == 1 &&
          pulse.result().reason == Refusal::InvalidRequest, "rearm cannot replace active pulse identity");
    pulse = consumed();
    check(pulse.render(7, 0, 120) && !pulse.render(8, 1, 130) && pulse.result().reason == Refusal::MixedFrame,
          "two different frames never form stereo response");
    pulse = consumed();
    check(pulse.render(7, 0, 120) && !pulse.render(7, 0, 130) && pulse.result().reason == Refusal::DuplicateEye,
          "duplicate rendered eye rejects");
    pulse = consumed(); pulse.render(7, 0, 120);
    check(!pulse.submit(7, 1, true, 130) && pulse.result().reason == Refusal::SubmitFailed,
          "unpainted eye cannot qualify by accepted submit");
    pulse = consumed(); pulse.render(7, 0, 120);
    check(!pulse.submit(7, 0, false, 130) && pulse.result().reason == Refusal::SubmitFailed,
          "runtime rejection never becomes zero latency success");
    pulse = consumed(); pulse.render(7, 0, 120); pulse.submit(7, 0, true, 130);
    check(!pulse.submit(7, 0, true, 140) && pulse.result().reason == Refusal::DuplicateEye,
          "duplicate submitted eye rejects");
    pulse = consumed(); pulse.render(7, 0, 120);
    check(!pulse.submit(8, 0, true, 130) && pulse.result().reason == Refusal::MixedFrame,
          "submitted frame must equal response frame");
    pulse = consumed(); pulse.render(7, 0, 120);
    check(!pulse.gpuCompleted(130) && !pulse.result().gpuCompleteQpc && pulse.result().reason == Refusal::GraphicsFailure,
          "premature fence notification cannot qualify missing response");
    for (auto refusal : {Refusal::GenerationChanged, Refusal::WrongThread, Refusal::TextureUnsupported,
                         Refusal::GraphicsFailure, Refusal::DeviceRemoved, Refusal::Stopped}) {
        pulse = consumed(); pulse.reject(refusal);
        check(pulse.phase() == Phase::Rejected && pulse.result().reason == refusal &&
              pulse.result().consumedQpc == 110 && !pulse.result().gpuCompleteQpc,
              "external faults retain observed input and explicitly reject unavailable endpoint");
    }
}

void clocksAndRequests() {
    Pulse pulse;
    check(!pulse.arm(0, 100, 100, 1000), "zero request rejected");
    check(!pulse.arm(1, 0, 100, 1000), "missing sender clock rejected");
    check(!pulse.arm(1, 101, 100, 1000), "future sender clock rejected");
    check(!pulse.arm(1, 100, 100, 0), "missing frequency rejected");
    check(!pulse.arm(1, 100, 100, (std::numeric_limits<uint64_t>::max)()), "overflowing timeout scale rejected");
    for (unsigned stage = 0; stage < 3; ++stage) {
        pulse = Pulse{};
        check(pulse.arm(1, 100, 100, 1000), "timeout fixture arms");
        if (stage) pulse.consume(110, 1);
        if (stage == 2) pulse.render(7, 0, 120);
        check(!pulse.expire(2100) && pulse.expire(2101) && pulse.result().reason == Refusal::Timeout,
              "two second timeout covers armed, consumed, and partly rendered pulse");
    }
    pulse = consumed();
    check(!pulse.render(7, 0, 109) && pulse.result().reason == Refusal::InvalidRequest,
          "endpoint clock before consumption rejected");
    pulse = Pulse{}; pulse.arm(1, 100, 100, 1000);
    check(pulse.consume(110, 3) && pulse.phase() == Phase::Consumed && pulse.result().source == 3 &&
          pulse.result().consumedQpc == 110, "removed owned-window mouse message qualifies as API receipt source three");
    pulse = Pulse{}; pulse.arm(1, 100, 100, 1000);
    check(!pulse.consume(110, 4) && pulse.result().reason == Refusal::InvalidRequest,
          "keyboard source without a qualified InputManager token is rejected");
    pulse = Pulse{}; pulse.arm(1, 100, 100, 1000);
    check(!pulse.consume(110, 6) && pulse.result().reason == Refusal::InvalidRequest,
          "unrecognized input source is rejected");
    pulse = consumed();
    check(!pulse.render(0, 0, 120) && pulse.result().reason == Refusal::InvalidRequest,
          "missing sequence rejected");
    pulse = consumed();
    check(!pulse.render(7, 2, 120) && pulse.result().reason == Refusal::InvalidRequest,
          "non-eye index rejected before shifting mask");
}

void uiResponseEvidence() {
    PixelRoi roi{};
    const auto pack = [](uint64_t l, uint64_t t, uint64_t r, uint64_t b) {
        return l | (t << 16) | (r << 32) | (b << 48);
    };
    check(decodeRoi(pack(0, 0, 65535, 65535), 8, 8, roi) && roi.left == 0 && roi.top == 0 &&
          roi.right == 8 && roi.bottom == 8, "normalized ROI includes edge pixels without exceeding the texture");
    check(decodeRoi(pack(100, 200, 300, 400), 2560, 1440, roi) && roi.right > roi.left && roi.bottom > roi.top,
          "small nonempty normalized ROI rounds outward to a bounded pixel rectangle");
    check(!decodeRoi(pack(5, 0, 5, 65535), 8, 8, roi) &&
          !decodeRoi(pack(6, 0, 5, 65535), 8, 8, roi) &&
          !decodeRoi(pack(0, 0, 65535, 65535), 0, 8, roi) &&
          !decodeRoi(pack(0, 0, 65535, 65535), 257, 256, roi),
          "empty, reversed, missing-size and oversized ROI requests fail before GPU access");
    const uint8_t rgba[]{255,128,0,255, 128,128,128,255, 255,128,0,255,
                         128,128,128,255, 255,128,0,255, 255,128,0,255};
    uint32_t ppm = 0;
    check(orangeFraction(rgba, 12, 2, 2, false, ppm) && ppm == 500000,
          "RGBA classifier honors padded RowPitch and excludes orange padding");
    const uint8_t bgra[]{0,128,255,255, 128,128,128,255};
    check(orangeFraction(bgra, 8, 2, 1, true, ppm) && ppm == 500000,
          "BGRA classifier decodes channel order independently of alpha");
    check(!orangeFraction(nullptr, 8, 2, 1, false, ppm) &&
          !orangeFraction(rgba, 7, 2, 1, false, ppm) &&
          !orangeFraction(rgba, 8, 0, 1, false, ppm), "invalid maps and strides cannot manufacture pixel proof");
    check(orangeState(100000, 200000, 600000) == OrangeState::Off &&
          orangeState(800000, 200000, 600000) == OrangeState::On &&
          orangeState(400000, 200000, 600000) == OrangeState::Ambiguous &&
          orangeState(0, 600000, 200000) == OrangeState::Ambiguous,
          "orange-state thresholds preserve an ambiguous band and reject invalid limits");
    Pulse pulse;
    pulse.arm(1, 100, 101, 1000);
    check(pulse.consume(110, 4, 8765) && pulse.result().consumedTickToken == 8765,
          "actual keyboard receipt retains its qualified engine tick token");
    UiCandidateReceipt frame; frame.sequence = 77;
    frame.sourceGeneration = 8; frame.renderedTick = 8765; frame.renderedPose = 990;
    check(frame.copyEye(0) && !frame.copyEye(0) && !frame.copyEye(2) &&
          !frame.submit(1, true, 120) && !frame.ready(), "duplicate eyes and an uncopied submit cannot qualify a candidate");
    check(frame.submit(0, true, 120) && frame.copyEye(1) && !frame.submit(1, false, 125) &&
          frame.submit(1, true, 130) && !frame.ready(), "accepted stereo alone is insufficient without a fence receipt");
    check(!uiTargetMatches(frame, 76, 900000, 900000, 200000, 600000, true, 8765),
          "matching pixels without GPU completion cannot complete UI latency");
    frame.gpuQpc = 140;
    check(frame.ready() && !uiTargetMatches(frame, 76, 100000, 100000, 200000, 600000, true, 8765) &&
          pulse.phase() == Phase::Consumed && pulse.result().sentQpc == 100 && pulse.result().consumedQpc == 110,
          "an unchanged completed candidate leaves the original input timestamps pending for a retry");
    check(!uiTargetMatches(frame, 76, 900000, 100000, 200000, 600000, true, 8765) &&
          !uiTargetMatches(frame, 77, 900000, 900000, 200000, 600000, true, 8765),
          "mixed stereo states and the baseline frame cannot become a response");
    frame.sequence = 78; frame.gpuQpc = 150; frame.submitQpc = 145;
    frame.renderedTick = 8764;
    check(!uiTargetMatches(frame, 76, 900000, 900000, 200000, 600000, true, 8765) &&
          pulse.phase() == Phase::Consumed && pulse.result().consumedQpc == 110,
          "older rendered ticks retry without dropping or shortening the actual UI latency");
    frame.renderedTick = 8766;
    check(uiTargetMatches(frame, 76, 900000, 900000, 200000, 600000, true, 8765) &&
          pulse.uiResponse(frame.sequence, frame.gpuQpc, frame.submitQpc, 160, frame.renderedTick, frame.renderedPose) && pulse.phase() == Phase::Complete &&
          pulse.result().sequence == 78 && pulse.result().gpuCompleteQpc == 150 && pulse.result().consumedQpc == 110,
          "pixel-proven retry uses that candidate's GPU endpoint and preserves actual input receipt");
    check(uiTargetMatches(frame, 76, 100000, 100000, 200000, 600000, false, 8765),
          "the same evidence path verifies a real deselection response");
    pulse = Pulse{}; pulse.arm(1, 100, 101, 1000); pulse.consume(110, 4, 1);
    check(!pulse.uiResponse(78, 150, 145, 2101, 2, 990) && pulse.result().reason == Refusal::Timeout,
          "retry keeps the original absolute timeout instead of extending unavailable latency");
    pulse = consumed();
    check(!pulse.uiResponse(78, 150, 145, 160, 2, 990), "mouse marker receipt cannot masquerade as game UI keyboard evidence");
    pulse = Pulse{}; pulse.arm(1, 100, 101, 1000);
    check(pulse.consume(110, 5, 44) && pulse.result().source == 5 && pulse.result().consumedTickToken == 44 &&
          pulse.uiResponse(79, 150, 145, 160, 45, 991) && pulse.result().renderedTickToken == 45,
          "qualified buffered source5 accepts a real UI response in the following render tick");
}

#ifdef _WIN32
LONG64 exchange(uint64_t& word, LONG64 desired, LONG64 expected) {
    return InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&word), desired, expected);
}
void mappingPublication() {
    LARGE_INTEGER stamp{}; QueryPerformanceCounter(&stamp);
    wchar_t name[128]{};
    std::swprintf(name, sizeof(name) / sizeof(*name), L"Local\\EDUP.InputLatency.Test.%lu.%llu",
                  GetCurrentProcessId(), uint64_t(stamp.QuadPart));
    HANDLE receiver = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name);
    check(receiver && GetLastError() != ERROR_ALREADY_EXISTS, "IPC fixture exclusively creates its own mapping");
    if (!receiver) return;
    auto* receive = static_cast<Shared*>(MapViewOfFile(receiver, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    HANDLE sender = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    auto* send = sender ? static_cast<Shared*>(MapViewOfFile(sender, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared))) : nullptr;
    check(receive && send, "independent sender and receiver views attach");
    if (receive && send) {
        *receive = Shared{};
        check(exchange(send->state, LONG64(Phase::Reserved), LONG64(Phase::Idle)) == LONG64(Phase::Idle),
              "sender atomically reserves idle receiver");
        send->request = 99; send->sentQpc = 1234;
        check(exchange(send->state, LONG64(Phase::Armed), LONG64(Phase::Reserved)) == LONG64(Phase::Reserved) &&
              exchange(receive->state, 0, 0) == LONG64(Phase::Armed) && receive->request == 99 && receive->sentQpc == 1234,
              "Armed publication exposes complete sender payload across independent views");
        exchange(receive->state, LONG64(Phase::Reserved), LONG64(Phase::Armed));
        receive->reason = uint64_t(Refusal::GenerationChanged);
        exchange(receive->state, LONG64(Phase::Rejected), LONG64(Phase::Reserved));
        check(exchange(send->state, LONG64(Phase::Armed), LONG64(Phase::Reserved)) == LONG64(Phase::Rejected) &&
              send->reason == uint64_t(Refusal::GenerationChanged), "receiver rejection defeats stale reservation publication");
        HANDLE collision = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name);
        check(collision && GetLastError() == ERROR_ALREADY_EXISTS && receive->request == 99,
              "existing map is detectable without overwriting a live request");
        if (collision) CloseHandle(collision);
    }
    if (send) UnmapViewOfFile(send);
    if (sender) CloseHandle(sender);
    if (receive) UnmapViewOfFile(receive);
    CloseHandle(receiver);
}

struct UiGraphics {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> eyes[2];
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> unordered[2];
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> targets[2];
};
bool makeUiGraphics(UiGraphics& graphics, DXGI_FORMAT sourceFormat) {
    const auto create = edvr::systemD3D11CreateDevice();
    const HRESULT result = create ? create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &graphics.device, nullptr, &graphics.context) : E_FAIL;
    check(SUCCEEDED(result) && graphics.device && graphics.context, "UI readback rig creates a private system WARP device");
    if (FAILED(result) || !graphics.device || !graphics.context) return false;
    const bool rgbaTypeless = sourceFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    bool bgra = false;
    DXGI_FORMAT viewFormat = uiCopyFormat(sourceFormat, bgra);
    if (viewFormat == DXGI_FORMAT_UNKNOWN) viewFormat = sourceFormat;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 64; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = sourceFormat; desc.SampleDesc.Count = 1;
    // Match the live RGBA8 typeless source's UAV+SRV-only bind0x88 exactly.
    // BGRA8 and the explicit unsupported case use their valid typed RTVs.
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rgbaTypeless ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_RENDER_TARGET);
    for (unsigned eye = 0; eye < 2; ++eye) {
        HRESULT hr = graphics.device->CreateTexture2D(&desc, nullptr, &graphics.eyes[eye]);
        if (SUCCEEDED(hr) && rgbaTypeless) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format = viewFormat; view.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            hr = graphics.device->CreateUnorderedAccessView(graphics.eyes[eye].Get(), &view, &graphics.unordered[eye]);
        } else if (SUCCEEDED(hr)) {
            D3D11_RENDER_TARGET_VIEW_DESC view{};
            view.Format = viewFormat; view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            hr = graphics.device->CreateRenderTargetView(graphics.eyes[eye].Get(), &view, &graphics.targets[eye]);
        }
        check(SUCCEEDED(hr), "real source texture exposes its allowed typed byte-family view");
        if (FAILED(hr)) return false;
    }
    return true;
}
void fillUi(UiGraphics& graphics, bool orange) {
    const FLOAT selected[]{0.9f, 0.55f, 0.0f, 1.0f}, unselected[]{0.15f, 0.15f, 0.15f, 1.0f};
    for (unsigned eye = 0; eye < 2; ++eye) {
        if (graphics.unordered[eye]) graphics.context->ClearUnorderedAccessViewFloat(graphics.unordered[eye].Get(), orange ? selected : unselected);
        else graphics.context->ClearRenderTargetView(graphics.targets[eye].Get(), orange ? selected : unselected);
    }
}
void primeBufferedFixture(UiGraphics& graphics, uint64_t token = 4310) {
    const uint64_t receipt = nowQpc();
    for (auto event : {std::pair<unsigned,bool>{0xD0,true}, {0xD0,false}, {0xC8,true}, {0xC8,false}})
        keyboardBuffered(graphics.device.Get(), event.first, event.second, token, receipt);
}
Shared* prepareUiFixture(UiGraphics& graphics, bool buffered = false, bool prime = true) {
    configure(graphics.device.Get(), true, edvr::native_reflex::Mode::Off, 17);
    check(enabled() && current, "production probe creates its real GPU-fence observer and owned IPC");
    if (!enabled() || !current) return nullptr;
    Shared* shared = current->shared;
    const bool reserved = exchange(shared->state, LONG64(Phase::Reserved), LONG64(Phase::Idle)) == LONG64(Phase::Idle);
    check(reserved, "UI fixture reserves request words through the published IPC contract");
    if (!reserved) return nullptr;
    const uint64_t fence = nowQpc();
    if (prime) {
        if (buffered) primeBufferedFixture(graphics);
        else {
            uint8_t keyboard[256]{};
            keyboard[0xD0] = 0x80; keyboardState(graphics.device.Get(), keyboard, 4310);
            keyboard[0xD0] = 0; keyboardState(graphics.device.Get(), keyboard, 4310);
            keyboard[0xC8] = 0x80; keyboardState(graphics.device.Get(), keyboard, 4310);
            keyboard[0xC8] = 0; keyboardState(graphics.device.Get(), keyboard, 4310);
        }
    }
    shared->request = 1; shared->sentQpc = fence;
    shared->responseKind = uint64_t(ResponseKind::GameUi); shared->inputScanCode = 0xD0;
    shared->roiLeft = shared->roiRight = uint64_t(0xFFFFFFFF) << 32;
    shared->orangeLowPpm = 100000; shared->orangeHighPpm = 900000; shared->targetOn = 1;
    check(exchange(shared->state, LONG64(Phase::Preparing), LONG64(Phase::Reserved)) == LONG64(Phase::Reserved),
          "UI fixture publishes complete baseline parameters before receiver ownership");
    return shared;
}
void stampUiPair(UiGraphics& graphics, uint64_t sequence, uint64_t sourceTick) {
    const EdvrNativeReflexSource source{sizeof(source), EDVR_NATIVE_REFLEX_FRAME_VERSION_1, 81, sourceTick, sequence + 9000};
    for (auto& eye : graphics.eyes) check(edvr::native_reflex::writeSource(eye.Get(), source), "private WARP eye retains its actual paired source GUID");
}
void copyUiPair(UiGraphics& graphics, uint64_t sequence, bool secondAccepted = true, uint64_t sourceTick = 4311) {
    stampUiPair(graphics, sequence, sourceTick);
    for (unsigned eye = 0; eye < 2; ++eye) {
        eyeRendered(graphics.device.Get(), graphics.eyes[eye].Get(), sequence, eye, true);
        if (eye == 0 || secondAccepted) eyeSubmitted(sequence, eye, true);
    }
    // Test-only submission: there is deliberately no swap chain or real game
    // Present here to flush queued WARP commands. Production never calls Flush.
    graphics.context->Flush();
}
bool waitUiFence() {
    const ULONGLONG until = GetTickCount64() + 1000;
    do {
        // Read-only snapshot of the worker's actual notification receipt. No
        // synthetic fence timestamp or producer-side GetData polling is used.
        {
            std::lock_guard<std::mutex> lock(callbacks);
            if (current && current->ui.count && current->ui.frames[current->ui.head].receipt.gpuQpc) return true;
        }
        Sleep(1);
    } while (GetTickCount64() < until);
    return false;
}
void preparingPairBoundary() {
    for (bool releaseBetweenEyes : {false, true}) {
        UiGraphics graphics;
        if (!makeUiGraphics(graphics, DXGI_FORMAT_R8G8B8A8_TYPELESS)) continue;
        Shared* shared = prepareUiFixture(graphics, true, releaseBetweenEyes);
        if (!shared) { stop(); continue; }
        fillUi(graphics, false); stampUiPair(graphics, 350, 4320);
        eyeRendered(graphics.device.Get(), graphics.eyes[0].Get(), 350, 0, true);
        if (releaseBetweenEyes) {
            // A newer real release clears an unfinished baseline and marks
            // its entire stereo sequence ineligible, just like priming ack.
            keyboardBuffered(graphics.device.Get(), 0xD0, true, 4312, nowQpc());
            keyboardBuffered(graphics.device.Get(), 0xD0, false, 4312, nowQpc());
        } else {
            // First eye arrived while fresh priming receipts were pending.
            // The acknowledgment lands before the matching second eye.
            primeBufferedFixture(graphics);
        }
        eyeRendered(graphics.device.Get(), graphics.eyes[1].Get(), 350, 1, true);
        eyeSubmitted(350, 1, true);
        bool skippedWholePair = false;
        {
            std::lock_guard<std::mutex> lock(callbacks);
            skippedWholePair = current && !current->ui.count;
        }
        check(skippedWholePair && Phase(exchange(shared->state, 0, 0)) == Phase::Preparing && !shared->ready,
              releaseBetweenEyes ? "a release between eyes skips the whole Preparing pair instead of retaining its second eye" :
                                   "priming acknowledgment between eyes skips the whole Preparing pair instead of retaining its second eye");
        copyUiPair(graphics, 351, true, 4321);
        if (Phase(exchange(shared->state, 0, 0)) != Phase::Rejected) {
            check(waitUiFence(), "the next complete Preparing pair finishes a real WARP staging copy");
            eyeRendered(graphics.device.Get(), graphics.eyes[0].Get(), 352, 0, true);
        }
        check(Phase(exchange(shared->state, 0, 0)) == Phase::Prepared && shared->ready == 1 &&
              shared->renderSequence == 351 && shared->baselineLeftPpm == 0 && shared->baselineRightPpm == 0,
              releaseBetweenEyes ? "the first whole pair after a release qualifies without a false MixedFrame rejection" :
                                   "the first whole pair after priming acknowledgment qualifies without a false MixedFrame rejection");
        const unsigned intervening = releaseBetweenEyes ? 0xD0 : 0xC8;
        const uint64_t readAt = nowQpc();
        void* other = releaseBetweenEyes ? static_cast<void*>(graphics.device.Get()) : static_cast<void*>(graphics.context.Get());
        keyboardBuffered(other, intervening, true, 4323, readAt);
        keyboardBuffered(other, intervening, false, 4323, readAt);
        check(shared->reason == uint64_t(Refusal::BaselineMismatch) &&
              exchange(shared->state, LONG64(Phase::Armed), LONG64(Phase::Prepared)) == LONG64(Phase::Rejected),
              "extra real arrow cycle between Prepared and Armed cannot restore the old baseline");
        stop();
    }
}
void competingUiInputAfterConsumption() {
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        UiGraphics graphics;
        if (!makeUiGraphics(graphics, DXGI_FORMAT_R8G8B8A8_TYPELESS)) continue;
        Shared* shared = prepareUiFixture(graphics, true);
        if (!shared) { stop(); continue; }
        fillUi(graphics, false); copyUiPair(graphics, 400);
        check(waitUiFence(), "competition fixture gets opposite-state stereo baseline");
        eyeRendered(graphics.device.Get(), graphics.eyes[0].Get(), 401, 0, true);
        shared->sentQpc = nowQpc();
        check(exchange(shared->state, LONG64(Phase::Armed), LONG64(Phase::Prepared)) == LONG64(Phase::Prepared),
              "competition fixture arms the baseline");
        keyboardBuffered(graphics.device.Get(), 0xD0, true, 4322, nowQpc());
        uint8_t held[256]{};
        held[0xD0] = 0x80;
        keyboardState(graphics.device.Get(), held, 4322);
        keyboardBuffered(graphics.device.Get(), 0xD0, false, 4322, nowQpc());
        check(Phase(readWord(shared->state)) == Phase::Consumed, "duplicate held read and measured release preserve the receipt");
        void* keyboard = scenario == 2 ? static_cast<void*>(graphics.context.Get()) : static_cast<void*>(graphics.device.Get());
        keyboardBuffered(keyboard, scenario == 0 ? 0xC8u : 0xD0u, true, 4323, nowQpc());
        fillUi(graphics, true); copyUiPair(graphics, 402, true, 4323);
        check(Phase(readWord(shared->state)) == Phase::Rejected && shared->reason == uint64_t(Refusal::BaselineMismatch),
              "opposite arrow, repeated press after release, and another keyboard all invalidate later target pixels");
        stop();
    }
}

void realTypelessUiResponse() {
    for (DXGI_FORMAT format : {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_TYPELESS}) {
        const bool buffered = format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
        UiGraphics graphics;
        if (!makeUiGraphics(graphics, format)) continue;
        Shared* shared = prepareUiFixture(graphics, buffered);
        if (!shared) { stop(); continue; }
        fillUi(graphics, false); copyUiPair(graphics, 100);
        check(waitUiFence(), "actual typeless baseline copies complete through the production fence worker");
        eyeRendered(graphics.device.Get(), graphics.eyes[0].Get(), 101, 0, true);
        check(Phase(exchange(shared->state, 0, 0)) == Phase::Prepared && shared->ready == 1 && !shared->sentQpc &&
              shared->renderSequence == 100 && shared->baselineLeftPpm == 0 && shared->baselineRightPpm == 0,
              "fresh real key cycles plus a later opposite stereo baseline qualify and clear the priming fence");
        shared->sentQpc = nowQpc();
        check(exchange(shared->state, LONG64(Phase::Armed), LONG64(Phase::Prepared)) == LONG64(Phase::Prepared),
              "only the acquired Prepared state admits the arm timestamp");
        uint8_t keyboard[256]{}; keyboard[0xD0] = 0x80;
        if (buffered) keyboardBuffered(graphics.device.Get(), 0xD0, true, 0, nowQpc());
        else keyboardState(graphics.device.Get(), keyboard, 0);
        check(Phase(exchange(shared->state, 0, 0)) == Phase::Armed && !shared->consumedQpc,
              "zero-token pressed receipts remain diagnostic-only after priming");
        if (buffered) keyboardBuffered(graphics.device.Get(), 0xD0, true, 4322, nowQpc());
        else keyboardState(graphics.device.Get(), keyboard, 4322);
        const uint64_t consumedAt = shared->consumedQpc;
        check(Phase(exchange(shared->state, 0, 0)) == Phase::Consumed && consumedAt &&
              shared->source == (buffered ? 5u : 4u) && shared->consumedTickToken == 4322,
              "state4 and buffered5 retain distinct qualified API receipts and exact consuming ticks");
        copyUiPair(graphics, 102, true, 4322);
        check(waitUiFence(), "unchanged pixels have their own actual completed GPU candidate");
        fillUi(graphics, true); stampUiPair(graphics, 103, 4321);
        eyeRendered(graphics.device.Get(), graphics.eyes[0].Get(), 103, 0, true);
        check(Phase(exchange(shared->state, 0, 0)) == Phase::Consumed && shared->consumedQpc == consumedAt,
              "unchanged candidates do not shorten the measured input latency");
        eyeSubmitted(103, 0, true);
        eyeRendered(graphics.device.Get(), graphics.eyes[1].Get(), 103, 1, true); eyeSubmitted(103, 1, true);
        graphics.context->Flush(); check(waitUiFence(), "an older source's real changed pixels complete their staging copy");
        stampUiPair(graphics, 104, 4323);
        eyeRendered(graphics.device.Get(), graphics.eyes[0].Get(), 104, 0, true);
        check(Phase(exchange(shared->state, 0, 0)) == Phase::Consumed && shared->consumedQpc == consumedAt && !shared->gpuCompleteQpc,
              "older source ticks retry even with target pixels instead of manufacturing a response frame");
        eyeSubmitted(104, 0, true);
        eyeRendered(graphics.device.Get(), graphics.eyes[1].Get(), 104, 1, true); eyeSubmitted(104, 1, true);
        graphics.context->Flush(); check(waitUiFence(), "the genuine following-tick response completes its own GPU copy");
        eyeRendered(graphics.device.Get(), graphics.eyes[0].Get(), 105, 0, true);
        check(Phase(exchange(shared->state, 0, 0)) == Phase::Complete && shared->renderSequence == 104 &&
              shared->eyeMask == 3 && shared->submitMask == 3 && shared->consumedTickToken == 4322 &&
              shared->responseLeftPpm == 1000000 && shared->responseRightPpm == 1000000 && shared->gpuCompleteQpc >= consumedAt,
              "real paired typeless pixels from render tick N+1 complete the actual input response without same-world-frame claims");
        // A collector can map before stop and use its view after the game closes
        // its own handle. Exercise both orders of the terminal acknowledgement.
        HANDLE mapping = nullptr;
        const bool duplicate = DuplicateHandle(GetCurrentProcess(), current->mapping, GetCurrentProcess(),
                                               &mapping, 0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE;
        auto* retained = duplicate ? static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared))) : nullptr;
        check(retained != nullptr, "sender retains its mapping across receiver close");
        if (retained && buffered) exchange(retained->state, LONG64(Phase::Idle), LONG64(Phase::Complete));
        stop();
        if (retained) {
            check(retained->ready == kReceiverStopped &&
                  Phase(readWord(retained->state)) == (buffered ? Phase::Stopped : Phase::Complete),
                  "retirement keeps unread results and permanently closes an acknowledged idle mapping");
            if (!buffered) {
                check(retained->renderSequence == 104 && retained->gpuCompleteQpc &&
                      exchange(retained->state, LONG64(Phase::Idle), LONG64(Phase::Complete)) == LONG64(Phase::Complete) &&
                      retained->ready == kReceiverStopped, "acknowledging an unread result cannot revive a retired mapping");
            }
            configure(graphics.device.Get(), true, edvr::native_reflex::Mode::Off, 17);
            check(!enabled() && retryMappingAfter != 0, "retained sender view schedules a bounded creation retry");
            UnmapViewOfFile(retained);
            CloseHandle(mapping); mapping = nullptr;
            Sleep(510);
            configure(graphics.device.Get(), true, edvr::native_reflex::Mode::Off, 17);
            check(enabled(), "a confirmed receiver recovers after the prior sender releases its retired mapping");
            stop();
        }
        if (mapping) CloseHandle(mapping);
    }
    {
        UiGraphics graphics;
        if (makeUiGraphics(graphics, DXGI_FORMAT_R8G8B8A8_TYPELESS)) {
            Shared* shared = prepareUiFixture(graphics, false, false);
            if (shared) {
                uint8_t keyboard[256]{};
                keyboardState(graphics.device.Get(), keyboard, 0);
                keyboardState(graphics.device.Get(), keyboard, 4310);
                fillUi(graphics, false); copyUiPair(graphics, 150);
                check(Phase(exchange(shared->state, 0, 0)) == Phase::Preparing && !shared->ready && !shared->renderSequence,
                      "released state and old OFF pixels cannot acknowledge queued or absent priming cycles");
                primeBufferedFixture(graphics);
                copyUiPair(graphics, 152); check(waitUiFence(), "late observed buffered priming permits a fresh GPU baseline");
                eyeRendered(graphics.device.Get(), graphics.eyes[0].Get(), 153, 0, true);
                check(Phase(exchange(shared->state, 0, 0)) == Phase::Prepared && shared->renderSequence == 152 && !shared->sentQpc,
                      "fresh ordered buffered cycles arriving during Preparing still require a newly copied opposite baseline");
                shared->sentQpc = nowQpc();
                exchange(shared->state, LONG64(Phase::Armed), LONG64(Phase::Prepared));
                keyboardBuffered(graphics.device.Get(), 0xD0, true, 4322, nowQpc());
                check(Phase(exchange(shared->state, 0, 0)) == Phase::Consumed && shared->source == 5,
                      "a real buffered measured press consumes only after fresh priming and pixels");
                keyboardReadLost(graphics.device.Get());
                check(Phase(exchange(shared->state, 0, 0)) == Phase::Rejected && shared->reason == uint64_t(Refusal::InputReadLost) &&
                      shared->consumedTickToken == 4322 && !shared->gpuCompleteQpc,
                      "read loss invalidates cached release proof and an active consumed pulse without inventing completion");
            }
            stop();
        }
    }
    {
        UiGraphics graphics;
        if (makeUiGraphics(graphics, DXGI_FORMAT_R8G8B8A8_TYPELESS)) {
            Shared* shared = prepareUiFixture(graphics, true);
            if (shared) {
                fillUi(graphics, false); copyUiPair(graphics, 160, false); check(waitUiFence(), "both ROI copies complete despite a missing accepted submit");
                eyeRendered(graphics.device.Get(), graphics.eyes[0].Get(), 161, 0, true);
                check(Phase(exchange(shared->state, 0, 0)) == Phase::Preparing && !shared->ready,
                      "fresh priming and pixels cannot qualify a baseline without both accepted eyes");
            }
            stop();
        }
    }
    {
        UiGraphics graphics;
        if (makeUiGraphics(graphics, DXGI_FORMAT_R8G8B8A8_TYPELESS)) {
            Shared* shared = prepareUiFixture(graphics, true);
            if (shared) {
                fillUi(graphics, true); copyUiPair(graphics, 200); check(waitUiFence(), "transient priming UI state uses real staging bytes");
                fillUi(graphics, false); stampUiPair(graphics, 201, 4312);
                eyeRendered(graphics.device.Get(), graphics.eyes[0].Get(), 201, 0, true);
                check(Phase(exchange(shared->state, 0, 0)) == Phase::Preparing && !shared->ready,
                      "transient selected pixels after priming retry instead of arming against an old OFF baseline");
                eyeSubmitted(201, 0, true); eyeRendered(graphics.device.Get(), graphics.eyes[1].Get(), 201, 1, true); eyeSubmitted(201, 1, true);
                graphics.context->Flush(); check(waitUiFence(), "restored opposite UI pixels complete a later actual copy");
                eyeRendered(graphics.device.Get(), graphics.eyes[0].Get(), 202, 0, true);
                check(Phase(exchange(shared->state, 0, 0)) == Phase::Prepared && shared->renderSequence == 201,
                      "only the fresh restored opposite-state pair enables measured input");
            }
            stop();
        }
    }
    for (bool generationChanged : {false, true}) {
        UiGraphics graphics;
        if (!makeUiGraphics(graphics, DXGI_FORMAT_R8G8B8A8_TYPELESS)) continue;
        Shared* shared = prepareUiFixture(graphics, true);
        if (shared) {
            stampUiPair(graphics, 250, 4311);
            eyeRendered(graphics.device.Get(), graphics.eyes[0].Get(), 250, 0, true); eyeSubmitted(250, 0, true);
            EdvrNativeReflexSource wrong{sizeof(wrong), EDVR_NATIVE_REFLEX_FRAME_VERSION_1,
                generationChanged ? 82u : 81u, 4311, generationChanged ? 9250u : 9251u};
            edvr::native_reflex::writeSource(graphics.eyes[1].Get(), wrong);
            eyeRendered(graphics.device.Get(), graphics.eyes[1].Get(), 250, 1, true);
            check(Phase(exchange(shared->state, 0, 0)) == Phase::Rejected && shared->reason == uint64_t(
                    generationChanged ? Refusal::GenerationChanged : Refusal::MixedFrame),
                  "changed source generation or mixed stereo pose rejects instead of attributing pixels to a guessed frame");
        }
        stop();
    }
    {
        UiGraphics graphics;
        if (makeUiGraphics(graphics, DXGI_FORMAT_R16G16B16A16_FLOAT)) {
            Shared* shared = prepareUiFixture(graphics, true);
            if (shared) {
                stampUiPair(graphics, 300, 4311); eyeRendered(graphics.device.Get(), graphics.eyes[0].Get(), 300, 0, true);
                check(Phase(exchange(shared->state, 0, 0)) == Phase::Rejected && shared->reason == uint64_t(Refusal::TextureUnsupported),
                      "an unimplemented byte family explicitly refuses instead of guessing pixel values");
            }
            stop();
        }
    }
}
#endif
} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "--dry-run")) {
        std::puts("input_latency_test: dry-run (no device, mapping, or worker)"); return 0;
    }
    edges(); keyboardCache(); pairBoundary(); completeOrders(); refusals(); clocksAndRequests(); uiResponseEvidence();
#ifdef _WIN32
    mappingPublication();
    for (bool croppedLeft : {false, true}) {
        UiGraphics graphics;
        if (!makeUiGraphics(graphics, DXGI_FORMAT_R8G8B8A8_TYPELESS)) continue;
        auto* shared = prepareUiFixture(graphics, true);
        if (shared) {
            fillUi(graphics, false); stampUiPair(graphics, 88, 4320);
            for (unsigned eye = 0; eye < 2; ++eye) {
                eyeRendered(graphics.device.Get(), graphics.eyes[eye].Get(), 88, eye, eye != (croppedLeft ? 0u : 1u));
                eyeSubmitted(88, eye, true);
            }
            check(Phase(readWord(shared->state)) == Phase::Rejected &&
                  shared->reason == uint64_t(Refusal::TextureUnsupported) && !shared->gpuCompleteQpc,
                  "accepting both eye submissions cannot qualify an off-crop baseline");
        }
        stop();
    }
    preparingPairBoundary();
    competingUiInputAfterConsumption();
    realTypelessUiResponse();
#endif
    std::printf("input_latency_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

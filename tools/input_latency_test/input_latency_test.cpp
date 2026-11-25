#include "../../src/d3d11/input_latency_state.h"

#include <cstdio>
#include <cstring>
#include <limits>

#ifdef _WIN32
#include <windows.h>
#include <cwchar>
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
    check(!buttons.press(1, true) && !buttons.press(0, false) && !buttons.press(0, true),
          "generation reset and missing device both require new real release");
    for (uintptr_t device = 1; device <= 16; ++device) buttons.press(device, false);
    check(!buttons.press(17, false) && !buttons.press(17, true), "bounded device table fails closed when full");
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
    check(!pulse.consume(110, 3) && pulse.result().reason == Refusal::InvalidRequest,
          "unrecognized input source rejected");
    pulse = consumed();
    check(!pulse.render(0, 0, 120) && pulse.result().reason == Refusal::InvalidRequest,
          "missing sequence rejected");
    pulse = consumed();
    check(!pulse.render(7, 2, 120) && pulse.result().reason == Refusal::InvalidRequest,
          "non-eye index rejected before shifting mask");
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
#endif
} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "--dry-run")) {
        std::puts("input_latency_test: dry-run (no device, mapping, or worker)"); return 0;
    }
    edges(); pairBoundary(); completeOrders(); refusals(); clocksAndRequests();
#ifdef _WIN32
    mappingPublication();
#endif
    std::printf("input_latency_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

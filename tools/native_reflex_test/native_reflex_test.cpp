#include "../../src/d3d11/native_reflex.h"
#include "../../src/d3d11/nvapi_module_path.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace edvr::native_reflex;

namespace {

void check(bool condition, const char* name);

void frameTransport() {
    detail::FrameSleepHistory history;
    unsigned calls = 0;
    const auto accept = [&] { ++calls; return true; };
    const auto refuse = [&] { ++calls; return false; };
    check(!history.sleep(0, accept) && history.sleep(4, accept) && !history.sleep(4, accept) &&
          !history.sleep(3, accept) && !history.sleep(5, refuse) && !history.sleep(5, accept) &&
          history.sleep(6, accept) && calls == 3, "Sleep attempts each increasing source ID at most once");
    check(history.contains(4) && !history.contains(5) && history.contains(6),
          "overlapping render frames retain only their own accepted sleep IDs");
    check(!history.sleep(134, refuse) && !history.contains(6),
          "an old source loses admission even when a new sleep attempt is refused");
    history.retire();
    check(!history.sleep(135, accept) && !history.contains(6) && calls == 4,
          "a retired frame transport refuses new sleeps and markers");

    constexpr auto marker = detail::markerParameters(0x100000004ull, Marker::PresentStart);
    check(marker.version == 0x10058 && marker.frameId == 0x100000004ull &&
          marker.markerType == 4 && !marker.reserved0 && !marker.reserved[55],
          "marker ABI encodes the same 64-bit source ID and zeroes reserved fields");

    detail::LatencyResult report{};
    LatencyBatch batch;
    check(detail::version1(sizeof(report)) == 0x13c28 &&
          !detail::collectLatency(report, 1, 100, 0, batch) && batch.count == 0,
          "an all-zero driver batch provides no latency evidence");
    auto& completed = report.frames[63].timing;
    completed.frameId = 90;
    completed.simStartUs = 700000;
    completed.simEndUs = 703000;
    completed.renderSubmitStartUs = 702000;
    completed.renderSubmitEndUs = 719000;
    completed.presentStartUs = 714000;
    completed.presentEndUs = 716000;
    completed.gpuRenderStartUs = 712000;
    completed.gpuRenderEndUs = 723000;
    report.frames[59].timing = completed;
    report.frames[59].timing.frameId = 86;
    report.frames[59].timing.gpuRenderEndUs = 0;
    report.frames[60].timing = completed;
    report.frames[60].timing.frameId = 87;
    report.frames[60].timing.presentStartUs = 0;
    report.frames[61].timing = completed;
    report.frames[61].timing.frameId = 89;
    report.frames[62].timing = completed;
    check(detail::collectLatency(report, 1, 100, 85, batch) && batch.count == 2 &&
          batch.frames[0].frameId == 89 && batch.frames[1].frameId == 90 &&
          batch.frames[1].simStartUs == 700000 && batch.frames[1].gpuRenderEndUs == 723000,
          "driver timing batches omit missing, stale and duplicate records and keep exact timestamps");
    check(detail::collectLatency(report, 1, 100, 89, batch) && batch.count == 1 &&
          batch.frames[0].frameId == 90, "a caller watermark skips driver results already sampled");
    check(!detail::collectLatency(report, 91, 100, 0, batch) && !batch.count,
          "driver reports from an earlier session cannot leak into this session");

    ModeHistory mode;
    check(mode.change(Mode::Off, true) && mode.requiresDriver(),
          "full frame transport applies the initial Off driver mode");
    mode.acceptRequest();
    check(mode.observing() && !mode.needsReset(), "an accepted initial Off creates no enable cleanup ownership");
    check(detail::modeParameters(Mode::On, true).useMarkersToOptimize &&
          !detail::modeParameters(Mode::Off, true).useMarkersToOptimize &&
          !detail::modeParameters(Mode::On).useMarkersToOptimize,
          "only an enabled mode with frame transport opts into marker optimization");
}

unsigned checks = 0;
unsigned failures = 0;

void check(bool condition, const char* name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", name);
    }
}

struct FakeDriver {
    detail::GetSleepStatus status{};
    int readCode = 0;
    int writeCode = 0;
    bool applyWrites = true;
    unsigned reads = 0;
    std::vector<Mode> writes;
    std::vector<unsigned> samplePoints;
    detail::Result last{"no_result"};

    int read(detail::GetSleepStatus& output) {
        ++reads;
        check(output.version == 0x00010088, "GetSleepStatus receives the v1 size and version");
        output = status;
        return readCode;
    }

    int set(detail::SetSleepMode& input) {
        writes.push_back(input.lowLatencyBoost ? Mode::Boost : input.lowLatencyMode ? Mode::On : Mode::Off);
        if (writeCode == 0 && applyWrites) status.lowLatencyMode = input.lowLatencyMode;
        return writeCode;
    }
} driver;

void presents(ModeHistory& history, unsigned count) {
    for (unsigned i = 0; i < count; ++i) notePresent(history, nullptr, 7);
}

void modeSelection() {
    check(parseMode("oN") == Mode::On && parseMode("bOOsT") == Mode::Boost &&
          parseMode("Off") == Mode::Off, "the three driver modes accept existing case-insensitive values");
    check(parseMode("AuTo") == Mode::Off, "retired Auto profiles use Off");
    check(parseMode("invalid") == Mode::Off && parseMode("") == Mode::Off,
          "unknown and empty settings make no driver request");
    for (Mode mode : {Mode::Off, Mode::On, Mode::Boost})
        check(parseMode(modeWord(mode)) == mode, "each displayed setting round-trips to its driver mode");
}

void modeHint() {
    const char* hint = modeStatusHint(ModeStatus::ConfirmedOff, false);
    check(std::strstr(hint, "driver reports Off") && std::strstr(hint, "Mirror window > Image"),
          "F8 keeps confirmed Off visible alongside the enablement guidance");
    check(!std::strstr(modeStatusHint(ModeStatus::Unavailable, false), "driver reports Off"),
          "F8 does not claim Off without driver evidence");
}

void modeParameters() {
    constexpr auto off = detail::modeParameters(Mode::Off);
    constexpr auto on = detail::modeParameters(Mode::On);
    constexpr auto boost = detail::modeParameters(Mode::Boost);
    check(off.version == 0x0001002c && !off.lowLatencyMode && !off.lowLatencyBoost &&
          on.lowLatencyMode && !on.lowLatencyBoost && boost.lowLatencyMode && boost.lowLatencyBoost,
          "mode parameters set only the requested v1 mode and Boost bits");
    check(!boost.minimumIntervalUs && !boost.useMarkersToOptimize && !boost.useMinQueueTime,
          "the driver request does not enable a limiter or marker or queue-time optimization");
    const std::array<uint8_t, sizeof(boost.reserved)> zeros{};
    check(std::memcmp(boost.reserved, zeros.data(), zeros.size()) == 0, "reserved mode bytes are zero");
}

void preserveExternalPolicy() {
    for (unsigned i = 0; i < 4; ++i) {
        driver = {};
        ModeHistory history;
        if (i == 0) driver.status.lowLatencyMode = 1;
        if (i == 1) driver.status.sleepIntervalUs = 16000;
        if (i == 2) driver.status.useGameSleep = 1;
        if (i == 3) driver.readCode = -1;
        updateMode(history, nullptr, Mode::Boost, 7);
        presents(history, 65);
        closeMode(history, nullptr, 7);
        check(driver.reads == 1 && driver.writes.empty() && !history.needsReset() &&
              !history.retryNeeded() && !history.confirmedMode() &&
              std::strcmp(driver.last.reason, i == 3 ? "baseline_unknown" : "existing_driver_policy") == 0,
              "existing or unreadable policy prevents a write, delayed read, and close cleanup");
    }
    driver = {};
    driver.status.useGameSleep = 1;
    const auto read = [](detail::GetSleepStatus& status) { return driver.read(status); };
    const auto set = [](detail::SetSleepMode& params) { return driver.set(params); };
    auto result = detail::requestMode(Mode::On, false, read, set, true, true);
    check(result.setStatus == 0 && driver.writes == std::vector<Mode>{Mode::On},
          "an Off session's own accepted Sleep does not block live enable");
    driver.status.lowLatencyMode = 0;
    driver.status.sleepIntervalUs = 16000;
    result = detail::requestMode(Mode::On, false, read, set, true, true);
    check(std::strcmp(result.reason, "existing_driver_policy") == 0 && driver.writes.size() == 1,
          "own Sleep does not override an observed existing interval");
}

void reenrollOwnedSleepDevice() {
    struct Device {
        detail::GetSleepStatus baseline{};
        bool ownSleepProof = false;
    } first, unrelated;
    detail::FrameSleepHistory previousSession;
    const bool slept = previousSession.sleep(20, [] { return true; });
    first.ownSleepProof = slept;
    previousSession.retire();
    first.baseline.useGameSleep = 1;  // Remains set after the first session's Off cleanup.
    unrelated.baseline = first.baseline;
    check(slept && !detail::hasExternalDriverPolicy(first.baseline, first.ownSleepProof) &&
          detail::hasExternalDriverPolicy(unrelated.baseline, unrelated.ownSleepProof),
          "stale ED-UP Sleep permits reenrollment only on its own device");

    unsigned acceptedWrites = 0;
    const auto read = [&](detail::GetSleepStatus& out) { out = first.baseline; return 0; };
    const auto set = [&](detail::SetSleepMode&) { ++acceptedWrites; return 0; };
    auto result = detail::requestMode(Mode::Off, false, read, set, first.ownSleepProof, true);
    check(result.setStatus == 0 && acceptedWrites == 1,
          "the reopened session can issue its initial Off with the retained Sleep proof");

    first.baseline.lowLatencyMode = 1;
    check(detail::hasExternalDriverPolicy(first.baseline, first.ownSleepProof),
          "reenrollment never ignores an observed enabled mode");
    first.baseline.lowLatencyMode = 0;
    first.baseline.sleepIntervalUs = 16000;
    check(detail::hasExternalDriverPolicy(first.baseline, first.ownSleepProof),
          "reenrollment never ignores an observed interval");
}

void firstSleepModeRace() {
    using detail::FirstSleepGate;
    using Admission = FirstSleepGate::Admission;
    FirstSleepGate gate(false);
    ModeHistory mode;
    check(mode.change(Mode::Off, true), "initial Off is applied before the first Sleep");
    mode.acceptRequest();
    check(gate.beginSleep() == Admission::Unproven && gate.beginModeUpdate() == Admission::Busy,
          "first Sleep defers a concurrent mode change");
    check(mode.change(Mode::On, true), "the live On request is noticed");
    mode.deferRequest();
    check(mode.retryNeeded() && !mode.needsReset(),
          "defer keeps the mode pending without creating cleanup ownership");
    gate.finish(true);
    check(gate.beginModeUpdate() == Admission::Owned && mode.change(Mode::On, true),
          "the same request retries with ownership proof only after accepted Sleep");
    mode.acceptRequest();
    check(!mode.retryNeeded() && mode.needsReset() && !mode.change(Mode::On, true),
          "the accepted retry completes normally and is not repeated");

    FirstSleepGate rejected(false);
    check(rejected.beginSleep() == Admission::Unproven, "a new device begins without Sleep ownership");
    rejected.finish(false);
    check(rejected.beginModeUpdate() == Admission::Unproven && rejected.beginSleep() == Admission::Busy,
          "after refused Sleep, the next mode reads a clean baseline and excludes first Sleep during that read");
    rejected.finish(false);
    check(rejected.beginSleep() == Admission::Unproven,
          "a completed mode check reopens first Sleep admission");
    rejected.finish(false);

    check(detail::canWithdrawRetiredMode(Mode::Off, true) &&
          !detail::canWithdrawRetiredMode(Mode::Off, false) &&
          !detail::canWithdrawRetiredMode(Mode::On, true) &&
          !detail::canWithdrawRetiredMode(Mode::Boost, true),
          "retired transport permits only an Off withdrawal of an owned enable");
}

void acceptedAndRefusedRequests() {
    driver = {};
    ModeHistory history;
    check(updateMode(history, nullptr, Mode::Off, 7) && !updateMode(history, nullptr, Mode::Off, 7) &&
          driver.reads == 0 && driver.writes.empty(), "initial and repeated Off leave NVAPI untouched");
    driver.writeCode = -104;
    updateMode(history, nullptr, Mode::On, 7);
    check(!history.needsReset() && !history.observing() && driver.last.apiResolved &&
          driver.last.setStatus == -104 && driver.last.getStatus == detail::kUnavailable &&
          !history.confirmedMode() && history.status() == ModeStatus::RequestRefused,
          "a refused setter is distinct from API availability and does not schedule readback");
    closeMode(history, nullptr, 7);
    check(driver.writes.size() == 1, "a refused enable is not cleaned up");
    driver.writeCode = 0;
    driver.applyWrites = false;
    updateMode(history, nullptr, Mode::Boost, 7);
    check(history.needsReset() && driver.last.getStatus == 0 &&
          std::strcmp(driver.last.reason, "driver_mode_differs") == 0 &&
          !history.confirmedMode() && history.status() == ModeStatus::DriverReportsOff,
          "accepted Boost keeps cleanup ownership when the immediate readback disagrees");
    closeMode(history, nullptr, 7);
    check(driver.writes == std::vector<Mode>{Mode::On, Mode::Boost, Mode::Off},
          "close withdraws the accepted Boost request");
}

void delayedCleanup() {
    for (unsigned i = 0; i < 3; ++i) {
        driver = {};
        ModeHistory history;
        updateMode(history, nullptr, Mode::On, 7);
        presents(history, 4);
        check(driver.samplePoints == std::vector<unsigned>{4}, "the first request starts its own readback window");
        driver.samplePoints.clear();
        if (i == 1) driver.applyWrites = false;
        updateMode(history, nullptr, Mode::Off, 7);
        if (i == 2) driver.readCode = -1;
        presents(history, 63);
        check(driver.samplePoints == std::vector<unsigned>{4, 16} && history.needsReset(),
              "early Off readings keep cleanup ownership");
        presents(history, 4);
        check(driver.samplePoints == std::vector<unsigned>{4, 16, 64} &&
              !history.observing() && history.needsReset() == (i != 0),
              "only a successful final disabled reading releases cleanup ownership");
        const auto expected = i == 0 ? ModeStatus::ConfirmedOff : i == 1 ? ModeStatus::DriverReportsOn
                                                                       : ModeStatus::ReadbackUnavailable;
        check(history.status() == expected && history.confirmedMode().has_value() == (i == 0) &&
              history.retryNeeded() == (i != 0), "live evidence distinguishes confirmed, mismatched and unreadable Off");
        closeMode(history, nullptr, 7);
        check(driver.writes.size() == (i == 0 ? 2u : 3u) && driver.writes.back() == Mode::Off,
              "close retries Off only when the final sample is enabled or unavailable");
    }

    driver = {};
    ModeHistory history;
    updateMode(history, nullptr, Mode::On, 7);
    driver.writeCode = -1;
    updateMode(history, nullptr, Mode::Off, 7);
    presents(history, 64);
    check(driver.samplePoints.empty() && history.needsReset(),
          "a refused Off leaves cleanup armed without scheduling a readback");
    closeMode(history, nullptr, 7);
    check(driver.writes == std::vector<Mode>{Mode::On, Mode::Off, Mode::Off}, "close retries a refused Off");
}

void refusedWithdrawal() {
    driver = {};
    ModeHistory history;
    updateMode(history, nullptr, Mode::On, 7);
    driver.writeCode = -1;
    updateMode(history, nullptr, Mode::Off, 7);
    check(driver.status.lowLatencyMode && !history.confirmedMode() && history.status() == ModeStatus::WithdrawalRefused,
          "the requested Off is unconfirmed after a rejected driver write");
    driver.writeCode = 0;
    check(!updateMode(history, nullptr, Mode::Off, 7) && driver.status.lowLatencyMode,
          "re-reading the same setting does not bypass withdrawal backoff");
    presents(history, 4);
    check(updateMode(history, nullptr, Mode::Off, 7) && !driver.status.lowLatencyMode &&
              driver.writes == std::vector<Mode>{Mode::On, Mode::Off, Mode::Off} && history.confirmedMode() == Mode::Off,
          "owned Off withdrawal retries after four successful Presents and recovers");
    presents(history, 64);
    closeMode(history, nullptr, 7);
    check(!history.retryNeeded() && driver.writes.size() == 3, "verified withdrawal disarms the live and close retries");

    driver = {};
    history = {};
    updateMode(history, nullptr, Mode::On, 7);
    driver.writeCode = -1;
    updateMode(history, nullptr, Mode::Off, 7);
    for (const unsigned delay : {4u, 16u, 64u}) {
        const size_t before = driver.writes.size();
        presents(history, delay - 1);
        check(!updateMode(history, nullptr, Mode::Off, 7) && driver.writes.size() == before,
              "repeated withdrawal failures wait for the increasing Present backoff");
        presents(history, 1);
        check(updateMode(history, nullptr, Mode::Off, 7) && driver.writes.size() == before + 1,
              "the next retry attempts only the owned Off withdrawal");
    }
    presents(history, 256);
    check(!history.retryNeeded() && !updateMode(history, nullptr, Mode::Off, 7) && driver.writes.size() == 5 &&
          !history.confirmedMode(), "exhausted live retries cannot be rearmed by an unchanged setting");
    driver.writeCode = 0;
    closeMode(history, nullptr, 7);
    check(driver.writes.size() == 6 && !driver.status.lowLatencyMode, "normal close retains one final withdrawal attempt");
}

void modulePaths() {
    using namespace edvr::nvapi;
    constexpr auto system = LR"(C:\Windows\System32)";
    constexpr auto stub = LR"(c:\WINDOWS\System32\NVAPI64.DLL)";
    constexpr auto implementation = LR"(C:\Windows\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_0123456789abcDEF\nvapi64_impl.dll)";
    check(nvapiSystemStubPath(system, stub) && nvapiOwnerPathKind(system, stub, stub, stub) == 1,
          "the System32 stub matches independent of ASCII case");
    check(nvapiOwnerPathKind(system, stub, implementation, implementation) == 2 &&
          !nvapiOwnerPathKind(system, stub, stub, implementation), "the recognized DriverStore package matches a single loaded owner");
    check(!nvapiSystemStubPath(system, LR"(C:\Game\nvapi64.dll)") &&
          !nvapiSystemStubPath(system, LR"(C:\Windows\System32.\nvapi64.dll)"),
          "game-local and ambiguous stub paths are refused");
    constexpr auto foreign = LR"(C:\Windows\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_0123456789abcDEg\nvapi64_impl.dll)";
    check(!nvapiOwnerPathKind(system, stub, foreign, foreign), "an unrecognized DriverStore package is refused");
}

}  // namespace

namespace edvr::native_reflex::detail {

Result apply(ID3D11Device*, Mode mode, bool alreadyOwned) {
    return requestMode(mode, alreadyOwned, [](GetSleepStatus& status) { return driver.read(status); },
        [](SetSleepMode& params) { return driver.set(params); });
}

Result applyFrameMode(FrameDriver&, ID3D11Device* device, Mode mode, bool alreadyOwned) {
    return apply(device, mode, alreadyOwned);
}

Result observe(ID3D11Device*, Mode mode) {
    Result result{"unavailable", true};
    readMode(result, mode, [](GetSleepStatus& status) { return driver.read(status); });
    return result;
}

void logMode(Mode, const char*, uint64_t, const Result& result, unsigned count) {
    driver.last = result;
    if (count) driver.samplePoints.push_back(count);
}

}  // namespace edvr::native_reflex::detail

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (std::strcmp(argv[1], "--dry-run") == 0) return 0;
    if (std::strcmp(argv[1], "--self-test") != 0) return 2;
    modeSelection();
    modeHint();
    modeParameters();
    preserveExternalPolicy();
    reenrollOwnedSleepDevice();
    firstSleepModeRace();
    acceptedAndRefusedRequests();
    delayedCleanup();
    refusedWithdrawal();
    modulePaths();
    frameTransport();
    std::printf("native_reflex_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

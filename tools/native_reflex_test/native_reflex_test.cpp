#include "../../src/d3d11/native_reflex.h"
#include "../../src/d3d11/nvapi_module_path.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace edvr::native_reflex;

namespace {

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
    check(parseMode("AuTo") == Mode::On, "retired Auto profiles use On");
    check(parseMode("invalid") == Mode::Off && parseMode("") == Mode::Off,
          "unknown and empty settings make no driver request");
    for (Mode mode : {Mode::Off, Mode::On, Mode::Boost})
        check(parseMode(modeWord(mode)) == mode, "each displayed setting round-trips to its driver mode");
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
              std::strcmp(driver.last.reason, i == 3 ? "baseline_unknown" : "existing_driver_policy") == 0,
              "existing or unreadable policy prevents a write, delayed read, and close cleanup");
    }
}

void acceptedAndRefusedRequests() {
    driver = {};
    ModeHistory history;
    check(updateMode(history, nullptr, Mode::Off, 7) && !updateMode(history, nullptr, Mode::Off, 7) &&
          driver.reads == 0 && driver.writes.empty(), "initial and repeated Off leave NVAPI untouched");
    driver.writeCode = -104;
    updateMode(history, nullptr, Mode::On, 7);
    check(!history.needsReset() && !history.observing() && driver.last.apiResolved &&
          driver.last.setStatus == -104 && driver.last.getStatus == detail::kUnavailable,
          "a refused setter is distinct from API availability and does not schedule readback");
    closeMode(history, nullptr, 7);
    check(driver.writes.size() == 1, "a refused enable is not cleaned up");
    driver.writeCode = 0;
    driver.applyWrites = false;
    updateMode(history, nullptr, Mode::Boost, 7);
    check(history.needsReset() && driver.last.getStatus == 0 &&
          std::strcmp(driver.last.reason, "driver_mode_differs") == 0,
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
    modeParameters();
    preserveExternalPolicy();
    acceptedAndRefusedRequests();
    delayedCleanup();
    modulePaths();
    std::printf("native_reflex_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

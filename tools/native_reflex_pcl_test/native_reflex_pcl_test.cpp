// Substituting the transport keeps this lifecycle and ping-attribution test
// portable. Native builds compile the production transport separately.
#define EDVR_REFLEX_PCL_TEST
#include "../../src/d3d11/native_reflex_pcl.cpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

namespace {
unsigned checks = 0, failures = 0, starts = 0, stops = 0;
bool failStartup = false, joinedOutsideLocks = false;
std::vector<std::pair<uint32_t, uint64_t>> events;

void check(bool condition, const char* name) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL: %s\n", name); }
}
} // namespace

namespace edvr::native_reflex::pcl::detail {
bool beginVendor(uint32_t& messageId) noexcept {
    ++starts; messageId = 0xc123; return !failStartup;
}
void endVendor() noexcept {
    ++stops;
    // A joined worker may complete callbacks while shutdown is in progress.
    // Its query must return without waiting for the lifecycle owner.
    std::thread worker([] {
        joinedOutsideLocks = !available() && !initialize();
        mark(0, 900);
    });
    worker.join();
}
void vendorMarker(uint32_t marker, uint64_t frame) noexcept { events.emplace_back(marker, frame); }
} // namespace edvr::native_reflex::pcl::detail

namespace {
void pendingPolicy() {
    using edvr::native_reflex::pcl::detail::PendingPing;
    PendingPing ping;
    check(!ping.claim(10), "an input frame with no pending ping emits nothing");
    ping.received();
    check(!ping.claim(0) && !ping.claim(9) && !ping.claim(10), "invalid and repeated boundaries retain the pending ticket");
    check(ping.claim(11) && !ping.claim(12), "the next actual input frame consumes the pending ticket once");
    ping.received(); ping.received();
    check(ping.claim(13) && !ping.claim(14), "multiple messages may coalesce at one actual input boundary");
    ping.received(); ping.reset();
    check(!ping.claim(15), "shutdown discards an unconsumed ticket");
}

void publicLifecycle() {
    using namespace edvr::native_reflex::pcl;
    mark(0, 1); inputFrame(1);
    check(!available() && !messageRemoved(0xc123) && events.empty(), "an inactive provider does not claim messages or emit markers");
    std::array<std::thread, 4> starters;
    std::array<bool, 4> started{};
    for (size_t i = 0; i < starters.size(); ++i) starters[i] = std::thread([&, i] { started[i] = initialize(); });
    for (auto& thread : starters) thread.join();
    check(starts == 1 && available() && started == std::array<bool, 4>{true, true, true, true},
          "concurrent initialization creates one process-lifetime provider");
    check(!messageRemoved(0) && !messageRemoved(0xc124), "ordinary messages are left unclaimed");
    inputFrame(100);
    check(messageRemoved(0xc123) && events.empty(), "a removed ping only records a ticket");
    inputFrame(100); inputFrame(99); inputFrame(0);
    check(events.empty(), "a late ping is not attributed to an already sampled frame");
    inputFrame(101);
    check(events == std::vector<std::pair<uint32_t, uint64_t>>{{8, 101}}, "the next real input frame owns the ping marker");
    mark(0, 102); mark(2, 102); mark(1, 102); mark(4, 102); mark(5, 102); mark(3, 102); mark(7, 102);
    check(events.size() == 8 && events.back() == std::pair<uint32_t, uint64_t>{7, 102},
          "the wrapper preserves legitimate overlapping simulation, rendering and Present order");
    mark(6, 102); mark(8, 102); mark(0, 0); mark(99, 102);
    check(events.size() == 8, "deprecated, unsupported, guessed-ping and zero-frame markers are excluded");
    check(messageRemoved(0xc123), "a final ping can be pending before normal shutdown");
    shutdown(); shutdown();
    check(stops == 1 && joinedOutsideLocks && !available() && !initialize(),
          "normal shutdown joins outside locks, unregisters once and refuses restart");
    inputFrame(103); mark(0, 103);
    check(!messageRemoved(0xc123) && events.size() == 8, "a stopped provider cannot retain or emit another marker");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (std::strcmp(argv[1], "--dry-run") == 0) return 0;
    if (std::strcmp(argv[1], "--self-test-failed-init") == 0) {
        failStartup = true;
        check(!edvr::native_reflex::pcl::initialize() && starts == 1 && stops == 1 && joinedOutsideLocks,
              "partial initialization is retired outside the lifecycle lock");
        check(!edvr::native_reflex::pcl::initialize() && starts == 1,
              "an unavailable provider is not retried on every frame");
    } else if (std::strcmp(argv[1], "--self-test") == 0) {
        pendingPolicy(); publicLifecycle();
    } else return 2;
    std::printf("native_reflex_pcl_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

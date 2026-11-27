#include "native_reflex_pcl.h"

#include <atomic>
#include <condition_variable>
#include <mutex>

#if defined(EDVR_HAVE_REFLEX_PCL) && EDVR_HAVE_REFLEX_PCL && !defined(EDVR_REFLEX_PCL_TEST)
#include "../common/log.h"
#include <pclstats.h> // Externally fetched NVIDIA Reflex SDK, with its license.
PCLSTATS_DEFINE();
#endif

namespace edvr::native_reflex::pcl {
namespace detail {

// Like the SDK guide's deferred flag, several queued pings may be consumed by
// one simulation. Advancing the frame boundary without a ping is significant:
// a ping arriving later must wait for the next input frame.
class PendingPing final {
public:
    void received() noexcept { pending_.store(true, std::memory_order_release); }
    bool claim(uint64_t frame) noexcept {
        if (!frame || frame <= lastFrame_) return false;
        lastFrame_ = frame;
        return pending_.exchange(false, std::memory_order_acq_rel);
    }
    void reset() noexcept { pending_.store(false, std::memory_order_release); }
private:
    std::atomic<bool> pending_{false};
    uint64_t lastFrame_ = 0; // Only read or written under the emission lock.
};

#if defined(EDVR_REFLEX_PCL_TEST)
// The policy rig substitutes only the vendor transport, retaining the actual
// public initialization, deferred-message, emission and shutdown paths.
bool beginVendor(uint32_t& messageId) noexcept;
void endVendor() noexcept;
void vendorMarker(uint32_t marker, uint64_t frame) noexcept;
#elif defined(EDVR_HAVE_REFLEX_PCL) && EDVR_HAVE_REFLEX_PCL
bool beginVendor(uint32_t& messageId) noexcept {
    // Zero selects the SDK's registered custom message. Never request its
    // optional F13/F14/F15 keyboard-input route.
    PCLSTATS_INIT(0);
    messageId = g_PCLStatsWindowMessage;
    const bool ready = messageId && g_PCLStatsQuitEvent && g_PCLStatsPingThread;
    if (ready)
        Log::get().note("native_reflex_pcl,sdk=EDUP_REFLEX_PCL_SDK_v1,state=ready,ping=custom_window_message");
    return ready;
}

void endVendor() noexcept {
    // The vendor macro has a one-second join. Ensure the worker has actually
    // exited before it closes handles, or this DLL can later be unloaded.
    if (g_PCLStatsPingThread) {
        if (g_PCLStatsQuitEvent) SetEvent(g_PCLStatsQuitEvent);
        WaitForSingleObject(g_PCLStatsPingThread, INFINITE);
    }
    PCLSTATS_SHUTDOWN();
}

void vendorMarker(uint32_t marker, uint64_t frame) noexcept {
    PCLSTATS_MARKER(marker, frame);
}
#else
bool beginVendor(uint32_t&) noexcept { return false; }
void endVendor() noexcept {}
void vendorMarker(uint32_t, uint64_t) noexcept {}
#endif

} // namespace detail
namespace {
enum class State { Uninitialized, Running, Stopping, Stopped };
std::mutex lifecycle, emission;
std::condition_variable finished;
State state = State::Uninitialized;
std::atomic<bool> running{false};
std::atomic<uint32_t> pingMessage{0};
detail::PendingPing pending;

void retireVendor() noexcept {
    // Stop admission, drain any marker already in flight, then join the SDK
    // worker with neither wrapper lock held. The worker never uses our locks.
    {
        std::lock_guard<std::mutex> lock(emission);
        pending.reset();
    }
    detail::endVendor();
    {
        std::lock_guard<std::mutex> lock(lifecycle);
        state = State::Stopped;
    }
    finished.notify_all();
}
} // namespace

bool initialize() noexcept {
    {
        std::lock_guard<std::mutex> lock(lifecycle);
        if (state != State::Uninitialized) return state == State::Running;
        uint32_t messageId = 0;
        if (detail::beginVendor(messageId) && messageId) {
            pingMessage.store(messageId, std::memory_order_release);
            state = State::Running;
            running.store(true, std::memory_order_release);
            return true;
        }
        state = State::Stopping;
    }
    retireVendor();
    return false;
}

bool available() noexcept { return running.load(std::memory_order_acquire); }

void shutdown() noexcept {
    {
        std::unique_lock<std::mutex> lock(lifecycle);
        if (state == State::Stopping) {
            finished.wait(lock, [] { return state == State::Stopped; });
            return;
        }
        if (state == State::Stopped) return;
        if (state == State::Uninitialized) { state = State::Stopped; return; }
        state = State::Stopping;
        running.store(false, std::memory_order_release);
        pingMessage.store(0, std::memory_order_release);
    }
    retireVendor();
}

void mark(uint32_t marker, uint64_t actualFrameId) noexcept {
    if (!actualFrameId || (marker > 5 && marker != 7) || !available()) return;
    std::lock_guard<std::mutex> lock(emission);
    if (available()) detail::vendorMarker(marker, actualFrameId);
}

bool messageRemoved(uint32_t messageId) noexcept {
    if (!messageId || !available() || messageId != pingMessage.load(std::memory_order_acquire)) return false;
    pending.received();
    return true;
}

void inputFrame(uint64_t actualFrameId) noexcept {
    if (!actualFrameId || !available()) return;
    std::lock_guard<std::mutex> lock(emission);
    if (available() && pending.claim(actualFrameId)) detail::vendorMarker(8, actualFrameId);
}

} // namespace edvr::native_reflex::pcl

// diag.hull_layer (TEMPORARY): the rig probe. hull_rig_probe.h says what it asks and how; docs\research-own-hull-layer-2026-11-08.md
// ("Rig probe", 2026-11-09) says which line answers which question. It writes nothing into the game. Since 2026-11-10 ("Rig seed") its own
// slots also seed the layer's own mask (hullRigProbeOwnSlots), so it is load-bearing while the layer is on, not only a diagnostic.
//
// LOG LINES (all "hull layer: rig_probe ..."):
//   rig_probe armed / rig_probe off                 the probe's state follows diag.hull_layer
//   rig_probe target ship=".." ship_recs=N ...      the own rigs changed (vehicle_rig's refresh); capped
//   rig_probe own emits stopped / resumed           no emit for an own record for 2 s with a target known, and the first after; capped
//   rig_probe frame=F ... state=S                   one a comparison (~30 s, and the eye dump's frame), or one a 30 s window with no seed
//
// Threads: the emit, copier and merge taps run on game job threads, the map tap and everything else on the render thread. The claims and the
// pools are under g_mutex, which nothing here holds across a call into the game (the item and record reads are SEH-guarded memory reads) or
// into another EDVR lock; the map tap runs under engine motion's lock, which this file never takes.
#include "hull_rig_probe.h"

#include <windows.h>
#include <d3d11.h>
#include <intrin.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/log.h"
#include "engine_velocity.h"                // engineVelocityPresentClock
#include "engine_velocity_emit.h"           // guardedLookup, collectItems, the item and record pose reads
#include "vehicle_rig.h"                    // g_vehicleRigOwn, vehicleRigOwnDescribe
#include "vscreen.h"                        // the raw copy: the binding shadow keeps describing the game's work

namespace edvr {
namespace {

namespace emit = engine_velocity_emit;

constexpr uint64_t kRigRecordStride = 0x2F0;   // vehicle_rig_core.h kRecordStride
constexpr uint32_t kItem = 336;                // a pool record and an emitted item (emit::kItemBytes)
constexpr size_t kMaxClaims = 8192;            // a frame's own items; past it the frame's claims are dropped and counted
constexpr uint32_t kMaxPools = 16;             // watched 336-byte pools tracked; the least recently mapped makes room
constexpr uint32_t kMaxPoolSlots = 1u << 20;
constexpr uint64_t kWindowMs = 30000;          // the hull layer's report cadence (kReportMs), the gpu_ms line's
constexpr uint32_t kLagFrames = 2;             // a readback is tried this many layer frames after its copy, never waited on
constexpr uint32_t kLateFrames = 30;           // and given up after this many: state=readback_late
constexpr uint64_t kGapMs = 2000;              // no own emit this long, with a target known: the "stopped" line
constexpr uint32_t kLineCap = 64;              // target and emit-gap lines a session
constexpr uint32_t kMaxPending = 3;
constexpr uint32_t kDumpListCap = 4096;        // slots a list in the JSON

std::atomic<bool> g_on{false};

// ---- the window's counters (any thread; exchanged to zero when a comparison takes them) ---------------------------------------------------------------
enum Counter : uint32_t {
    kEmitCalls,              // every emit tap call (own or not): 0 says the tap never ran
    kOwnEmitsShip, kOwnEmitsCockpit,           // calls for an own record
    kAppendingShip, kAppendingCockpit,         // ...that appended items
    kClaimedShip, kClaimedCockpit,             // items claimed
    kLocateFailedShip, kLocateFailedCockpit,   // own calls whose items the dictionary did not give
    kTooMany,                // own calls past kMaxAppended
    kClaimOverflow,          // claims dropped at kMaxClaims
    kCopierCalls,            // every copier span
    kCopierRefused,          // spans whose metadata did not read (the pool's bits forgotten)
    kCopierNoPool,           // spans into a base no watched pool was mapped at
    kHitsShip, kHitsCockpit, // claimed items the copier copied, pose checked: the slot's own bit set
    kHitsNoPool,             // claimed items copied into an unwatched base
    kPoseMismatch,           // claimed items whose pose at the copy was not their rig record's (a recycled address): not set
    kStale,                  // claimed items copied in a later frame than their emit: not set
    kUnconsumed,             // claims a new frame found never copied
    kMergeCalls,             // merges seen with claims live
    kMergeMoved,             // claims a merge moved to another address
    kMergeSpliced,           // claims in nodes a merge took whole (address kept)
    kMergeFailed,            // merges whose list did not walk (claims left as they were)
    kMergeUnwound,           // merges the game unwound (the moved claims dropped)
    kMapsDiscard, kMapsNoOverwrite, kMapsOther,
    kCounters
};
std::atomic<uint64_t> g_n[kCounters];
std::atomic<uint64_t> g_ownEmitTotal{0};   // never reset: the gap detector's clock
inline void bump(Counter c, uint64_t v = 1) noexcept { g_n[c].fetch_add(v, std::memory_order_relaxed); }

// ---- claims and pools (g_mutex) ------------------------------------------------------------------------------------------------------------------------
struct Claim {
    uintptr_t record = 0;   // the rig record that emitted the item
    uint32_t frame = 0;     // the present frame of the emit
    uint8_t role = 0;       // 0 ship, 1 cockpit
};
struct Pool {
    const void* buffer = nullptr;   // the game's ID3D11Buffer (identity only, no reference)
    uintptr_t base = 0;             // its last map's pointer
    uint32_t slots = 0, frame = 0;
    uint64_t lastMapSeq = 0;
    bool known = false;             // a DISCARD map was seen since the bits started: the own bits describe the whole content
    std::vector<uint64_t> own;      // slots holding an own record now (set by a claimed copy, cleared by any other copy and by a discard)
    std::vector<uint64_t> written;  // slots copied in this frame's maps
    std::vector<uint64_t> fresh;    // slots an own claim was copied to in this frame's maps
};
std::mutex g_mutex;
std::unordered_map<uintptr_t, Claim> g_claims;
uint32_t g_claimFrame = ~0u;
std::atomic<size_t> g_claimCount{0};
std::atomic<uintptr_t> g_claimLo{UINTPTR_MAX}, g_claimHi{0};   // a conservative bound on the claimed addresses (widened only, reset with the claims)
Pool g_pools[kMaxPools];
uint64_t g_mapSeq = 0;

inline bool testBit(const std::vector<uint64_t>& v, uint32_t i) { return (v[i >> 6] >> (i & 63)) & 1u; }
inline void setBit(std::vector<uint64_t>& v, uint32_t i) { v[i >> 6] |= 1ull << (i & 63); }
inline void clearBit(std::vector<uint64_t>& v, uint32_t i) { v[i >> 6] &= ~(1ull << (i & 63)); }
inline void zero(std::vector<uint64_t>& v) { std::fill(v.begin(), v.end(), 0ull); }

void dropClaimsLocked() {
    g_claims.clear();
    g_claimCount.store(0, std::memory_order_release);
    g_claimLo.store(UINTPTR_MAX, std::memory_order_relaxed);
    g_claimHi.store(0, std::memory_order_relaxed);
}
void widenLocked(uintptr_t a) {
    if (a < g_claimLo.load(std::memory_order_relaxed)) g_claimLo.store(a, std::memory_order_relaxed);
    if (a > g_claimHi.load(std::memory_order_relaxed)) g_claimHi.store(a, std::memory_order_relaxed);
}
void forgetPool(Pool& p) {
    zero(p.own);
    zero(p.fresh);
    p.known = false;
}
Pool* poolAtLocked(uintptr_t base) {
    Pool* best = nullptr;
    for (Pool& p : g_pools)
        if (p.buffer && p.base == base && (!best || p.lastMapSeq > best->lastMapSeq)) best = &p;
    return best;
}

// The own role of a rig record, or -1: inside the ship's or the cockpit's record array, on a record boundary.
int ownRole(uintptr_t record) noexcept {
    for (int r = 0; r < 2; ++r) {
        const uint64_t hi = g_vehicleRigOwn.hi[r].load(std::memory_order_acquire);
        if (!hi) continue;
        const uint64_t lo = g_vehicleRigOwn.lo[r].load(std::memory_order_acquire);
        if (record >= lo && record < hi && (record - lo) % kRigRecordStride == 0) return r;
    }
    return -1;
}

// ---- the taps ------------------------------------------------------------------------------------------------------------------------------------------
void tapEmit(uintptr_t record, uintptr_t owner, int32_t before, int32_t after, void* lookup, uint32_t frame) noexcept {
    bump(kEmitCalls);
    const int role = ownRole(record);
    if (role < 0) return;
    g_ownEmitTotal.fetch_add(1, std::memory_order_relaxed);
    bump(role ? kOwnEmitsCockpit : kOwnEmitsShip);
    if (after <= before) return;   // the game's gate appended nothing for it this call (the "unchanged records" question)
    bump(role ? kAppendingCockpit : kAppendingShip);
    const int32_t k = after - before;
    if (k > emit::kMaxAppended) {
        bump(kTooMany);
        return;
    }
    const uintptr_t entry = lookup ? emit::guardedLookup(reinterpret_cast<emit::LookupFn>(lookup), owner + emit::kOwnerDictionary, record + emit::kRecordKey) : 0;
    uintptr_t items[emit::kMaxAppended] = {};
    if (!entry || !emit::collectItems(entry, k, items)) {
        bump(role ? kLocateFailedCockpit : kLocateFailedShip);
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_claimFrame != frame) {
        if (!g_claims.empty()) bump(kUnconsumed, g_claims.size());
        dropClaimsLocked();
        g_claimFrame = frame;
    }
    if (g_claims.size() + static_cast<size_t>(k) > kMaxClaims) {
        bump(kClaimOverflow, static_cast<uint64_t>(k));
        return;
    }
    try {
        for (int32_t j = 0; j < k; ++j) {
            g_claims[items[j]] = Claim{record, frame, static_cast<uint8_t>(role)};
            widenLocked(items[j]);
        }
    } catch (...) {
        dropClaimsLocked();
        return;
    }
    bump(role ? kClaimedCockpit : kClaimedShip, static_cast<uint64_t>(k));
    g_claimCount.store(g_claims.size(), std::memory_order_release);
}

// A claimed item at the copy: its pose must still be its rig record's, bit for bit (the emit's own gate), or the address was recycled.
bool samePose(uintptr_t item, uintptr_t record) noexcept {
    emit::Pose a, b;
    uint64_t node = 0;
    return emit::readItemPose(item, a) && emit::readRecordPose(record, b, node) && a == b;
}

void tapCopy(uintptr_t mapped, uint32_t stride, uintptr_t source, uint64_t slot, uint32_t count, uint32_t frame) noexcept {
    bump(kCopierCalls);
    std::lock_guard<std::mutex> lock(g_mutex);
    Pool* p = mapped ? poolAtLocked(mapped) : nullptr;
    if (count == UINT32_MAX || stride != kItem || slot > UINT32_MAX) {
        bump(kCopierRefused);
        if (p) forgetPool(*p);
        else if (!mapped)
            for (Pool& q : g_pools) if (q.buffer) forgetPool(q);
        return;
    }
    if (!count) return;
    const uint32_t first = static_cast<uint32_t>(slot);
    if (!p) bump(kCopierNoPool);
    if (p) {
        if (first >= p->slots || count > p->slots - first) {
            forgetPool(*p);   // a span past the buffer: its content is no longer known
            p = nullptr;
        } else {
            for (uint32_t i = 0; i < count; ++i) {
                setBit(p->written, first + i);
                clearBit(p->own, first + i);
                clearBit(p->fresh, first + i);
            }
        }
    }
    if (!g_claimCount.load(std::memory_order_acquire) || !source || source > UINTPTR_MAX - uint64_t(count) * kItem) return;
    const uintptr_t end = source + uintptr_t(count) * kItem;
    if (end <= g_claimLo.load(std::memory_order_relaxed) || source > g_claimHi.load(std::memory_order_relaxed)) return;
    // The claimed items in [source, end): looked up item by item, or the claims walked, whichever is fewer. Each is judged as it is taken
    // (its pose check is two guarded reads, no lock and no call into the game).
    const auto hit = [&](uint32_t i, const Claim& c) {
        if (c.frame != frame) {
            bump(kStale);
            return;
        }
        if (!samePose(source + uintptr_t(i) * kItem, c.record)) {
            bump(kPoseMismatch);
            return;
        }
        if (!p) {
            bump(kHitsNoPool);
            return;
        }
        setBit(p->own, first + i);
        setBit(p->fresh, first + i);
        bump(c.role ? kHitsCockpit : kHitsShip);
    };
    if (count <= g_claims.size()) {
        for (uint32_t i = 0; i < count; ++i) {
            auto it = g_claims.find(source + uintptr_t(i) * kItem);
            if (it == g_claims.end()) continue;
            const Claim c = it->second;
            g_claims.erase(it);
            hit(i, c);
        }
    } else {
        for (auto it = g_claims.begin(); it != g_claims.end();) {
            const uintptr_t a = it->first;
            if (a >= source && a < end && (a - source) % kItem == 0) {
                const Claim c = it->second;
                it = g_claims.erase(it);
                hit(static_cast<uint32_t>((a - source) / kItem), c);
            } else {
                ++it;
            }
        }
    }
    g_claimCount.store(g_claims.size(), std::memory_order_release);
}

// The merge's walk of the source list, the rule engine_velocity_primary_copy.h walkMerge reads (that header is not taken here: it brings the
// temporal shaders' bytecode): full eight-item nodes splice at the destination's head and keep their addresses; a partial node first fills the
// destination's tail node, and its remainder shifts down within its own node. visit(node, count, fill, tail, tailCount) for each well-formed
// node; false from visit stops it. True only for a whole, well-formed list (bounded at 4096 nodes; a repeated node ends it as malformed).
template <class Visit>
bool walkMergeList(uintptr_t destination, uintptr_t source, Visit&& visit) {
    uintptr_t dstLinks[2]{}, srcLinks[2]{};
    if (!destination || !source || destination == source || !emit::read(destination, dstLinks, sizeof dstLinks) || !emit::read(source, srcLinks, sizeof srcLinks))
        return false;
    uintptr_t tail = dstLinks[1], node = srcLinks[0], previous = source;
    uint64_t tailCount = 0;
    if (dstLinks[0] == destination) {
        if (tail != destination) return false;
    } else {
        uintptr_t next = 0;
        if (!tail || !emit::read(tail, &next, sizeof next) || next != destination || !emit::read(tail + emit::kNodeCount, &tailCount, 8) || tailCount > 8) return false;
    }
    for (uint32_t walked = 0; node != source; ++walked) {
        uint64_t header[4]{};
        if (walked >= 4096 || !node || node == destination || node == tail || node == previous || !emit::read(node, header, sizeof header) ||
            header[1] != previous || header[3] == 0 || header[3] > 8)
            return false;
        const uint32_t count = static_cast<uint32_t>(header[3]);
        uint32_t fill = 0;
        if (count != 8 && tail != destination && tailCount != 8) fill = std::min(count, static_cast<uint32_t>(8 - tailCount));
        if (!visit(node, count, fill, tail, tailCount)) return false;
        tailCount += fill;
        if (count == 8) {
            if (tail == destination) {
                tail = node;
                tailCount = 8;
            }
        } else if (count > fill) {
            tail = node;
            tailCount = count - fill;
        }
        previous = node;
        node = static_cast<uintptr_t>(header[0]);
    }
    return srcLinks[1] == previous;
}

// The merge's plan: the claims it moves, staged at the begin (their old addresses erased) and registered at the end. One merge a thread at a
// time (begin, the game's merge, end); a heap block a thread, kept.
struct Move {
    uintptr_t target;
    Claim claim;
};
struct Plan {
    std::vector<Move> moves;
    bool active = false;
};
thread_local Plan* t_plan = nullptr;

void tapMergeBegin(uintptr_t destination, uintptr_t source) noexcept {
    if (t_plan) {
        t_plan->active = false;
        t_plan->moves.clear();
    }
    if (!g_claimCount.load(std::memory_order_acquire)) return;
    bump(kMergeCalls);
    try {
        if (!t_plan) t_plan = new Plan;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_claims.empty()) return;
        std::vector<Move>& moves = t_plan->moves;
        std::vector<uintptr_t> olds;
        uint64_t spliced = 0;
        const bool ok = walkMergeList(destination, source, [&](uintptr_t node, uint32_t count, uint32_t fill, uintptr_t tail, uint64_t tailCount) {
            for (uint32_t i = 0; i < count; ++i) {
                const uintptr_t old = node + emit::kNodeRecords + uintptr_t(i) * kItem;
                const auto claim = g_claims.find(old);
                if (claim == g_claims.end()) continue;
                const uintptr_t target = i < fill ? tail + emit::kNodeRecords + uintptr_t(tailCount + i) * kItem : node + emit::kNodeRecords + uintptr_t(i - fill) * kItem;
                if (target == old) {
                    ++spliced;
                    continue;
                }
                moves.push_back(Move{target, claim->second});
                olds.push_back(old);
            }
            return true;
        });
        if (!ok) {
            moves.clear();
            bump(kMergeFailed);
            return;
        }
        for (uintptr_t a : olds) g_claims.erase(a);
        g_claimCount.store(g_claims.size(), std::memory_order_release);
        bump(kMergeSpliced, spliced);
        t_plan->active = !moves.empty();
    } catch (...) {
        if (t_plan) t_plan->moves.clear();
        bump(kMergeFailed);
    }
}

void tapMergeEnd(bool completed) noexcept {
    if (!t_plan || !t_plan->active) return;
    t_plan->active = false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!completed) {
        bump(kMergeUnwound);
        t_plan->moves.clear();
        return;
    }
    try {
        for (const Move& m : t_plan->moves) {
            if (m.claim.frame != g_claimFrame) continue;   // a new frame dropped the claims meanwhile
            g_claims[m.target] = m.claim;
            widenLocked(m.target);
            bump(kMergeMoved);
        }
    } catch (...) {
        dropClaimsLocked();
    }
    t_plan->moves.clear();
    g_claimCount.store(g_claims.size(), std::memory_order_release);
}

void tapMap(const void* buffer, void* data, uint32_t bytes, int mapType, uint32_t frame) noexcept {
    bump(mapType == D3D11_MAP_WRITE_DISCARD ? kMapsDiscard : mapType == D3D11_MAP_WRITE_NO_OVERWRITE ? kMapsNoOverwrite : kMapsOther);
    std::lock_guard<std::mutex> lock(g_mutex);
    Pool* p = nullptr;
    for (Pool& q : g_pools)
        if (q.buffer == buffer) p = &q;
    if (!p) {
        p = &g_pools[0];
        for (Pool& q : g_pools)
            if (!q.buffer || q.lastMapSeq < p->lastMapSeq) {
                p = &q;
                if (!q.buffer) break;
            }
        *p = Pool{};
        p->buffer = buffer;
    }
    const uint32_t slots = std::min<uint32_t>(bytes / kItem, kMaxPoolSlots);
    try {
        if (p->slots != slots) {
            p->slots = slots;
            const size_t words = (static_cast<size_t>(slots) + 63) / 64;
            p->own.assign(words, 0ull);
            p->written.assign(words, 0ull);
            p->fresh.assign(words, 0ull);
            p->known = false;
            p->frame = frame;
        }
    } catch (...) {
        *p = Pool{};
        return;
    }
    if (mapType == D3D11_MAP_WRITE_DISCARD) {
        // The whole content is new: nothing is held, and this frame's earlier writes are gone with it.
        zero(p->own);
        zero(p->written);
        zero(p->fresh);
        p->known = true;
    } else if (mapType != D3D11_MAP_WRITE_NO_OVERWRITE) {
        forgetPool(*p);
    }
    if (p->frame != frame) {
        zero(p->written);
        zero(p->fresh);
        p->frame = frame;
    }
    p->base = reinterpret_cast<uintptr_t>(data);
    p->lastMapSeq = ++g_mapSeq;
}

const HullRigProbeTaps kTaps = {&tapEmit, &tapCopy, &tapMergeBegin, &tapMergeEnd, &tapMap};

// ---- the comparison (render thread) --------------------------------------------------------------------------------------------------------------------
struct Window {
    uint64_t n[kCounters] = {};
};
Window takeWindow() {
    Window w;
    for (uint32_t i = 0; i < kCounters; ++i) w.n[i] = g_n[i].exchange(0, std::memory_order_relaxed);
    return w;
}

struct Target {
    char names[2][48] = {};
    uint64_t counts[2] = {};
    uint32_t gen = 0;
    bool known() const { return counts[0] || counts[1]; }
};
Target readTarget() {
    Target t;
    vehicleRigOwnDescribe(t.names, t.counts, &t.gen);
    return t;
}

struct Pending {
    ID3D11Buffer* maskStage = nullptr;
    ID3D11Buffer* poolStage = nullptr;   // the dump's: the layer's pool copy, for the differing slots' positions
    uint32_t records = 0, firstSlot = 0, hullFrame = 0, present = 0, poolFrame = 0;
    int eye = 0;
    bool pool = false, poolKnown = false, dump = false;
    wchar_t stamp[32] = L"";
    std::vector<uint64_t> own, written, fresh;   // the CPU sets of that moment, `records` bits from the view's first record
    Window win;
    Target target;
};
std::vector<Pending> g_pending;
bool g_due = false;
uint64_t g_windowStartMs = 0;
uint64_t g_lastEmitTotal = 0, g_lastEmitChangeMs = 0;
bool g_gapOpen = false;
uint32_t g_lastTargetGen = ~0u;
uint32_t g_targetLines = 0, g_gapLines = 0;
bool g_saidStageFail = false;

void releasePending(Pending& p) {
    if (p.maskStage) p.maskStage->Release();
    if (p.poolStage) p.poolStage->Release();
    p.maskStage = p.poolStage = nullptr;
}

ID3D11Buffer* makeStaging(ID3D11Device* device, uint32_t bytes) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = bytes;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* b = nullptr;
    if (FAILED(device->CreateBuffer(&d, nullptr, &b))) return nullptr;
    return b;
}

const char* targetName(const Target& t, int r) { return t.counts[r] ? (t.names[r][0] ? t.names[r] : "?") : "-"; }

// One line a comparison. gpu < 0: no GPU mask was read (the state says why).
struct Result {
    int64_t cpu = -1, fresh = -1, retained = -1, gpu = -1, both = -1, cpuOnly = -1, gpuOnly = -1, gpuOnlyWritten = -1, gpuOnlyHeld = -1, written = -1;
};
void logLine(uint32_t hullFrame, uint32_t present, int eye, const Target& t, const Window& w, uint32_t records, const char* pool, const Result& r, const char* state) {
    const uint64_t* n = w.n;
    Log::get().note("hull layer: rig_probe frame=%u present=%u eye=%d target=%s/%s ship_recs=%llu cockpit_recs=%llu own_emits=%llu,%llu appending=%llu,%llu "
                    "claimed=%llu,%llu copier_hits=%llu,%llu merges_moved=%llu spliced=%llu merge_failed=%llu unwound=%llu unconsumed=%llu stale=%llu "
                    "pose_mismatch=%llu locate_failed=%llu,%llu hits_no_pool=%llu emit_calls=%llu copier_calls=%llu copier_refused=%llu copier_no_pool=%llu "
                    "maps=%llu,%llu,%llu pool=%s records=%u written=%lld cpu=%lld cpu_fresh=%lld cpu_retained=%lld gpu=%lld both=%lld cpu_only=%lld "
                    "gpu_only=%lld gpu_only_written=%lld gpu_only_held=%lld state=%s",
                    hullFrame, present, eye, targetName(t, 0), targetName(t, 1), static_cast<unsigned long long>(t.counts[0]),
                    static_cast<unsigned long long>(t.counts[1]), (unsigned long long)n[kOwnEmitsShip], (unsigned long long)n[kOwnEmitsCockpit],
                    (unsigned long long)n[kAppendingShip], (unsigned long long)n[kAppendingCockpit], (unsigned long long)n[kClaimedShip],
                    (unsigned long long)n[kClaimedCockpit], (unsigned long long)n[kHitsShip], (unsigned long long)n[kHitsCockpit],
                    (unsigned long long)n[kMergeMoved], (unsigned long long)n[kMergeSpliced], (unsigned long long)n[kMergeFailed],
                    (unsigned long long)n[kMergeUnwound], (unsigned long long)n[kUnconsumed], (unsigned long long)n[kStale],
                    (unsigned long long)n[kPoseMismatch], (unsigned long long)n[kLocateFailedShip], (unsigned long long)n[kLocateFailedCockpit],
                    (unsigned long long)n[kHitsNoPool], (unsigned long long)n[kEmitCalls], (unsigned long long)n[kCopierCalls],
                    (unsigned long long)n[kCopierRefused], (unsigned long long)n[kCopierNoPool], (unsigned long long)n[kMapsDiscard],
                    (unsigned long long)n[kMapsNoOverwrite], (unsigned long long)n[kMapsOther], pool, records, (long long)r.written, (long long)r.cpu,
                    (long long)r.fresh, (long long)r.retained, (long long)r.gpu, (long long)r.both, (long long)r.cpuOnly, (long long)r.gpuOnly,
                    (long long)r.gpuOnlyWritten, (long long)r.gpuOnlyHeld, state);
}

// The first problem of a window, before the GPU's part: no target, no own emit.
const char* cpuState(const Target& t, const Window& w) {
    if (!t.known()) return "no_target";
    if (!w.n[kOwnEmitsShip] && !w.n[kOwnEmitsCockpit]) return "no_emits";
    return nullptr;
}

void writeJson(const Pending& p, const uint32_t* mask, const uint8_t* pool, const Result& r, const char* state) {
    const std::wstring dir = Log::get().dir() + L"\\eyes";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\eye_%s_HullRigProbe.json", dir.c_str(), p.stamp);
    FILE* f = nullptr;
    bool ok = false;
    const auto list = [&](const char* key, auto&& pick) {
        bool okList = std::fprintf(f, ",\n \"%s\": [", key) > 0;
        uint32_t listed = 0, total = 0;
        for (uint32_t q = 0; q < p.records && okList; ++q) {
            if (!pick(q)) continue;
            ++total;
            if (listed >= kDumpListCap) continue;
            okList = std::fprintf(f, "%s%u", listed ? "," : "", q) > 0;
            ++listed;
        }
        return std::fprintf(f, "], \"%s_listed\": %u, \"%s_total\": %u", key, listed, key, total) > 0 && okList;
    };
    // The differing slots, each with its record's position from the layer's pool copy (t33 record +16, the seed's own read) when the dump has it.
    const auto diff = [&](const char* key, bool cpuSide) {
        bool okList = std::fprintf(f, ",\n \"%s\": [", key) > 0;
        uint32_t listed = 0;
        for (uint32_t q = 0; q < p.records && okList && listed < kDumpListCap; ++q) {
            const bool c = testBit(p.own, q), g = mask[q] != 0;
            if (cpuSide ? !(c && !g) : !(g && !c)) continue;
            char pos[96] = "null";
            if (pool) {
                float v[3];
                std::memcpy(v, pool + static_cast<size_t>(q) * kItem + 16, sizeof v);
                std::snprintf(pos, sizeof pos, "[%.3f,%.3f,%.3f]", v[0], v[1], v[2]);
            }
            okList = std::fprintf(f, "%s\n  {\"slot\": %u, \"written\": %s, \"fresh\": %s, \"pos\": %s}", listed ? "," : "", q,
                                  testBit(p.written, q) ? "true" : "false", testBit(p.fresh, q) ? "true" : "false", pos) > 0;
            ++listed;
        }
        return std::fprintf(f, "\n ]") > 0 && okList;
    };
    if (_wfopen_s(&f, path, L"wb") == 0 && f) {
        const uint64_t* n = p.win.n;
        ok = std::fprintf(f, "{\"version\": 1, \"stamp\": \"%ls\", \"frame\": %u, \"present\": %u, \"eye\": %d, \"state\": \"%s\", \"records\": %u, "
                             "\"first_slot\": %u, \"pool\": \"%s\", \"slots_are\": \"own-mask indices (t33 record = first_slot + index)\", "
                             "\"positions\": \"%s\",\n \"target\": {\"ship\": \"%s\", \"ship_recs\": %llu, \"cockpit\": \"%s\", \"cockpit_recs\": %llu},\n "
                             "\"counts\": {\"written\": %lld, \"cpu\": %lld, \"cpu_fresh\": %lld, \"cpu_retained\": %lld, \"gpu\": %lld, \"both\": %lld, "
                             "\"cpu_only\": %lld, \"gpu_only\": %lld, \"gpu_only_written\": %lld, \"gpu_only_held\": %lld},\n \"window\": {\"own_emits\": [%llu, %llu], "
                             "\"appending\": [%llu, %llu], \"claimed\": [%llu, %llu], \"copier_hits\": [%llu, %llu], \"merges_moved\": %llu, \"spliced\": %llu, "
                             "\"merge_failed\": %llu, \"unwound\": %llu, \"unconsumed\": %llu, \"stale\": %llu, \"pose_mismatch\": %llu, \"emit_calls\": %llu, "
                             "\"copier_calls\": %llu, \"maps\": [%llu, %llu, %llu]}",
                          p.stamp, p.hullFrame, p.present, p.eye, state, p.records, p.firstSlot, !p.pool ? "none" : p.poolKnown ? "known" : "partial",
                          pool ? "the layer's pool copy at the seed, t33 record +16" : "not read (no pool staging)", targetName(p.target, 0),
                          (unsigned long long)p.target.counts[0], targetName(p.target, 1), (unsigned long long)p.target.counts[1], (long long)r.written,
                          (long long)r.cpu, (long long)r.fresh, (long long)r.retained, (long long)r.gpu, (long long)r.both, (long long)r.cpuOnly,
                          (long long)r.gpuOnly, (long long)r.gpuOnlyWritten, (long long)r.gpuOnlyHeld, (unsigned long long)n[kOwnEmitsShip],
                          (unsigned long long)n[kOwnEmitsCockpit], (unsigned long long)n[kAppendingShip], (unsigned long long)n[kAppendingCockpit],
                          (unsigned long long)n[kClaimedShip], (unsigned long long)n[kClaimedCockpit], (unsigned long long)n[kHitsShip],
                          (unsigned long long)n[kHitsCockpit], (unsigned long long)n[kMergeMoved], (unsigned long long)n[kMergeSpliced],
                          (unsigned long long)n[kMergeFailed], (unsigned long long)n[kMergeUnwound], (unsigned long long)n[kUnconsumed],
                          (unsigned long long)n[kStale], (unsigned long long)n[kPoseMismatch], (unsigned long long)n[kEmitCalls],
                          (unsigned long long)n[kCopierCalls], (unsigned long long)n[kMapsDiscard], (unsigned long long)n[kMapsNoOverwrite],
                          (unsigned long long)n[kMapsOther]) > 0;
        if (ok && mask) {
            ok = list("cpu_slots", [&](uint32_t q) { return testBit(p.own, q); }) && ok;
            ok = list("cpu_fresh_slots", [&](uint32_t q) { return testBit(p.fresh, q); }) && ok;
            ok = list("gpu_slots", [&](uint32_t q) { return mask[q] != 0; }) && ok;
            ok = diff("cpu_only", true) && ok;
            ok = diff("gpu_only", false) && ok;
        }
        ok = std::fprintf(f, "\n}\n") > 0 && ok;
        ok = (std::fclose(f) == 0) && ok;
    }
    Log::get().note("hull layer: rig_probe dump %ls: HullRigProbe %s (frame %u eye %d, state=%s; slot lists capped at %u, positions %s)", p.stamp,
                    ok ? "written" : "write failed", p.hullFrame, p.eye, state, kDumpListCap, pool ? "read" : "not read");
}

// A pending readback: false while the GPU is still on it.
bool finish(ID3D11DeviceContext* ctx, Pending& p, uint32_t hullFrame) {
    if (hullFrame - p.hullFrame < kLagFrames) return false;
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = ctx->Map(p.maskStage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    const char* why = cpuState(p.target, p.win);
    Result r;
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        if (hullFrame - p.hullFrame < kLateFrames) return false;
        logLine(p.hullFrame, p.present, p.eye, p.target, p.win, p.records, !p.pool ? "none" : p.poolKnown ? "known" : "partial", r, why ? why : "readback_late");
        return true;
    }
    if (FAILED(hr)) {
        logLine(p.hullFrame, p.present, p.eye, p.target, p.win, p.records, !p.pool ? "none" : p.poolKnown ? "known" : "partial", r, why ? why : "readback_failed");
        return true;
    }
    std::vector<uint32_t> mask(p.records);
    std::memcpy(mask.data(), m.pData, static_cast<size_t>(p.records) * 4);
    ctx->Unmap(p.maskStage, 0);
    r.gpu = r.both = r.cpuOnly = r.gpuOnly = 0;
    if (p.pool) r.cpu = r.fresh = r.retained = r.gpuOnlyWritten = r.gpuOnlyHeld = r.written = 0;
    for (uint32_t q = 0; q < p.records; ++q) {
        const bool g = mask[q] != 0;
        r.gpu += g;
        if (!p.pool) continue;
        const bool c = testBit(p.own, q), w = testBit(p.written, q);
        r.written += w;
        r.cpu += c;
        r.fresh += testBit(p.fresh, q);
        r.retained += c && !w;
        r.both += c && g;
        r.cpuOnly += c && !g;
        if (g && !c) {
            ++r.gpuOnly;
            if (w) ++r.gpuOnlyWritten;
            else ++r.gpuOnlyHeld;
        }
    }
    if (!p.pool) r.gpuOnly = r.gpu;
    const char* state = why ? why : !p.pool ? "no_pool" : "ok";
    logLine(p.hullFrame, p.present, p.eye, p.target, p.win, p.records, !p.pool ? "none" : p.poolKnown ? "known" : "partial", r, state);
    if (p.dump) {
        std::vector<uint8_t> pool;
        if (p.poolStage && SUCCEEDED(ctx->Map(p.poolStage, 0, D3D11_MAP_READ, 0, &m))) {   // the dump's: a wait here is the dump's
            pool.assign(static_cast<const uint8_t*>(m.pData), static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(p.records) * kItem);
            ctx->Unmap(p.poolStage, 0);
        }
        writeJson(p, mask.data(), pool.empty() ? nullptr : pool.data(), r, state);
    }
    return true;
}

}  // namespace

void hullRigProbeConfigure(bool on) {
    if (on == g_on.load(std::memory_order_relaxed)) return;
    g_on.store(on, std::memory_order_relaxed);
    if (on) {
        for (auto& c : g_n) c.store(0, std::memory_order_relaxed);
        g_windowStartMs = 0;
        g_due = false;
        g_lastTargetGen = ~0u;
        g_hullRigProbeTaps.store(&kTaps, std::memory_order_release);
        Log::get().note("hull layer: rig_probe armed (diagnostic, changes nothing drawn: the own rigs' emitted items, claimed at the emit, followed through "
                        "merges to the pool copier's slots, against the GPU own mask; a line every ~30 s and on an eye dump, HullRigProbe.json with the dump)");
    } else {
        g_hullRigProbeTaps.store(nullptr, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            dropClaimsLocked();
            for (Pool& p : g_pools) p = Pool{};
        }
        Log::get().note("hull layer: rig_probe off");
    }
}

void hullRigProbeAtSeed(ID3D11DeviceContext* ctx, ID3D11Device* device, uint32_t hullFrame, int eye, ID3D11Buffer* mask, ID3D11Buffer* poolCopy,
                        const void* gamePool, uint32_t firstSlot, uint32_t records, const wchar_t* dumpStamp) {
    if (!g_on.load(std::memory_order_relaxed) || !ctx || !device || !mask || !records) return;
    const bool dump = dumpStamp && dumpStamp[0];
    if (!g_due && !dump) return;
    if (dump)
        for (const Pending& q : g_pending)
            if (q.dump && !std::wcscmp(q.stamp, dumpStamp)) return;   // the dump's first seeded eye only
    if (g_pending.size() >= kMaxPending) return;
    Pending p;
    p.records = records;
    p.firstSlot = firstSlot;
    p.hullFrame = hullFrame;
    p.present = engineVelocityPresentClock();
    p.eye = eye;
    p.dump = dump;
    if (dump) wcsncpy_s(p.stamp, dumpStamp, _TRUNCATE);
    p.maskStage = makeStaging(device, records * 4u);
    if (dump && poolCopy) p.poolStage = makeStaging(device, records * kItem);
    if (!p.maskStage) {
        releasePending(p);
        if (!g_saidStageFail) {
            g_saidStageFail = true;
            Log::get().note("hull layer: rig_probe: a staging buffer of %u bytes could not be made; no comparison (said once)", records * 4u);
        }
        return;
    }
    D3D11_BOX box{0, 0, 0, records * 4u, 1, 1};
    vScreenCopySubresourceRegionRaw(ctx, p.maskStage, 0, mask, &box);
    if (p.poolStage) {
        D3D11_BOX pb{0, 0, 0, records * kItem, 1, 1};
        vScreenCopySubresourceRegionRaw(ctx, p.poolStage, 0, poolCopy, &pb);
    }
    // The CPU sets of this moment: the pool the seed read, its bits from the view's first record.
    try {
        const size_t words = (static_cast<size_t>(records) + 63) / 64;
        p.own.assign(words, 0ull);
        p.written.assign(words, 0ull);
        p.fresh.assign(words, 0ull);
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const Pool& q : g_pools) {
            if (!gamePool || q.buffer != gamePool) continue;
            p.pool = true;
            p.poolKnown = q.known;
            p.poolFrame = q.frame;
            for (uint32_t i = 0; i < records && firstSlot + i < q.slots; ++i) {
                if (testBit(q.own, firstSlot + i)) setBit(p.own, i);
                if (testBit(q.written, firstSlot + i)) setBit(p.written, i);
                if (testBit(q.fresh, firstSlot + i)) setBit(p.fresh, i);
            }
            break;
        }
        p.win = takeWindow();
        p.target = readTarget();
        g_pending.push_back(std::move(p));
    } catch (...) {
        releasePending(p);
        return;
    }
    if (!dump) g_due = false;
}

uint32_t hullRigProbeOwnSlots(const void* gamePool, uint32_t firstSlot, uint32_t records, uint32_t* out, uint32_t cap, uint32_t* gen) {
    if (gen) *gen = g_vehicleRigOwn.gen.load(std::memory_order_acquire);
    if (!g_on.load(std::memory_order_relaxed) || !gamePool || !out || !cap || !records) return 0;
    if (!(g_vehicleRigOwn.hi[0].load(std::memory_order_acquire) | g_vehicleRigOwn.hi[1].load(std::memory_order_acquire))) return 0;   // no own rig (on foot)
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const Pool& q : g_pools) {
        if (q.buffer != gamePool) continue;
        if (firstSlot >= q.slots) return 0;
        const uint32_t end = firstSlot + std::min(records, q.slots - firstSlot);
        uint32_t n = 0;
        // Word by word: the set bits only (a few dozen of a few thousand slots).
        for (uint32_t w = firstSlot >> 6; w <= (end - 1) >> 6 && n < cap; ++w) {
            uint64_t bits = q.own[w];
            while (bits && n < cap) {
                unsigned long b = 0;
                _BitScanForward64(&b, bits);
                bits &= bits - 1;
                const uint32_t slot = (w << 6) + static_cast<uint32_t>(b);
                if (slot >= firstSlot && slot < end) out[n++] = slot - firstSlot;
            }
        }
        return n;
    }
    return 0;
}

void hullRigProbeFrameEnd(ID3D11DeviceContext* ctx, uint32_t hullFrame) {
    if (!g_on.load(std::memory_order_relaxed)) {
        hullRigProbeRelease();
        return;
    }
    const uint64_t now = GetTickCount64();
    for (size_t i = 0; i < g_pending.size();) {
        if (ctx && finish(ctx, g_pending[i], hullFrame)) {
            releasePending(g_pending[i]);
            g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    // The own rigs: a line when they change.
    const uint32_t gen = g_vehicleRigOwn.gen.load(std::memory_order_acquire);
    if (gen != g_lastTargetGen) {
        g_lastTargetGen = gen;
        if (g_targetLines < kLineCap) {
            ++g_targetLines;
            const Target t = readTarget();
            Log::get().note("hull layer: rig_probe target ship=\"%s\" ship_recs=%llu cockpit=\"%s\" cockpit_recs=%llu frame=%u (the own rigs whose emits the "
                            "probe claims; - is none: on foot, or no target read)",
                            targetName(t, 0), static_cast<unsigned long long>(t.counts[0]), targetName(t, 1), static_cast<unsigned long long>(t.counts[1]), hullFrame);
        }
    }
    // The own emits' gaps (hyperspace, a load, a menu).
    const uint64_t total = g_ownEmitTotal.load(std::memory_order_relaxed);
    if (total != g_lastEmitTotal || !g_lastEmitChangeMs) {
        if (g_gapOpen && g_gapLines < kLineCap) {
            ++g_gapLines;
            Log::get().note("hull layer: rig_probe own emits resumed after %llu ms (frame %u)", static_cast<unsigned long long>(now - g_lastEmitChangeMs), hullFrame);
        }
        g_gapOpen = false;
        g_lastEmitTotal = total;
        g_lastEmitChangeMs = now;
    } else if (!g_gapOpen && now - g_lastEmitChangeMs >= kGapMs) {
        g_gapOpen = true;
        const bool known = (g_vehicleRigOwn.hi[0].load(std::memory_order_relaxed) | g_vehicleRigOwn.hi[1].load(std::memory_order_relaxed)) != 0;
        if (g_gapLines < kLineCap) {
            ++g_gapLines;
            Log::get().note("hull layer: rig_probe own emits stopped: none for %llu ms, target %s (frame %u; hyperspace, a load or a menu, or the own "
                            "rigs' records are not the ones emitted)",
                            static_cast<unsigned long long>(now - g_lastEmitChangeMs), known ? "known" : "none", hullFrame);
        }
    }
    // The 30 s window: a comparison is due at the next seed; one never taken gives its own line.
    if (!g_windowStartMs) g_windowStartMs = now;
    if (now - g_windowStartMs >= kWindowMs) {
        g_windowStartMs = now;
        if (g_due) {
            const Target t = readTarget();
            const Window w = takeWindow();
            const char* why = cpuState(t, w);
            logLine(hullFrame, engineVelocityPresentClock(), -1, t, w, 0, "none", Result{}, why ? why : "no_seed");
        }
        g_due = true;
    }
}

void hullRigProbeRelease() {
    for (Pending& p : g_pending) releasePending(p);
    g_pending.clear();
}

}  // namespace edvr

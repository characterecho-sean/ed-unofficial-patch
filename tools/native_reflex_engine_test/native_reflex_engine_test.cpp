#include "../../src/d3d11/native_reflex_engine_hook.h"
#include "../../src/common/native_reflex_frame.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace edvr::native_reflex;
namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
}
void callbackOrder() {
    std::vector<unsigned> calls;
    bool entered = false;
    unsigned forwards = 0;
    const bool accepted = engine_detail::forwardBoundary(true, true, entered,
        [&] { check(entered, "reentry gate is active before injected Sleep callback"); calls.push_back(1); },
        [&] { ++forwards; calls.push_back(2); },
        [&] { check(entered, "gate remains active through afterTick callback"); calls.push_back(3); });
    check(accepted && forwards == 1 && calls == std::vector<unsigned>{1, 2, 3} && !entered,
          "qualified boundary calls before, original exactly once, after, then releases gate");
    for (unsigned refusal = 0; refusal < 3; ++refusal) {
        calls.clear(); forwards = 0; entered = refusal == 2;
        const bool prior = entered;
        const bool rejected = !engine_detail::forwardBoundary(refusal != 0, refusal != 1, entered,
            [&] { calls.push_back(1); }, [&] { ++forwards; calls.push_back(2); }, [&] { calls.push_back(3); });
        check(rejected && forwards == 1 && calls == std::vector<unsigned>{2} && entered == prior,
              "foreign caller, wrong receiver, and reentry always forward without callbacks");
    }
    entered = false; forwards = 0; unsigned sleeps = 0;
    engine_detail::forwardBoundary(true, true, entered,
        [&] {
            ++sleeps;
            check(!engine_detail::forwardBoundary(true, true, entered, [&] { ++sleeps; },
                [&] { ++forwards; }, [] {}), "nested boundary refuses second Sleep");
        }, [&] { ++forwards; }, [] {});
    check(sleeps == 1 && forwards == 2 && !entered, "callback reentry sleeps once and preserves both originals");
}
void exceptionalForward() {
    bool entered = false; unsigned before = 0, after = 0, original = 0;
    try {
        engine_detail::forwardBoundary(true, true, entered, [&] { ++before; },
            [&] { ++original; throw std::runtime_error("original"); }, [&] { ++after; });
        check(false, "original exception must propagate");
    } catch (const std::runtime_error&) {
        check(before == 1 && original == 1 && !after && !entered,
              "original failure propagates and releases reentry gate without inventing successful completion");
    }
}
void callbackTable() {
    struct Fake { uint64_t token = 0; unsigned calls = 0; } fake;
    FrameBoundaryCallbacks table{};
    table.context = &fake;
    table.beforeTick = [](void* context, uint64_t token) noexcept {
        auto& observed = *static_cast<Fake*>(context); observed.token = token; ++observed.calls;
    };
    table.afterTick = table.beforeTick;
    table.inputBegin = table.beforeTick; table.inputEnd = table.beforeTick;
    table.beforeTick(table.context, 18); table.inputBegin(table.context, 18);
    table.inputEnd(table.context, 18); table.afterTick(table.context, 18);
    check(fake.calls == 4 && fake.token == 18, "injectable observer preserves caller context and one opaque tick token");
    FrameBoundaryRecord empty{};
    check(!empty.token && !empty.flags && !empty.inputCalls && !empty.inputBeginQpc && !empty.inputEndQpc,
          "absent input is explicit rather than a synthesized zero-duration qualified input");
}
void inputTokens() {
    check(engine_detail::inputFrameToken(true, false, true, 71) == 71,
          "actual reads inside qualified original retain exact owning tick token");
    check(!engine_detail::inputFrameToken(false, false, true, 71),
          "before and after InputManager callbacks cannot claim original input scope");
    check(!engine_detail::inputFrameToken(true, true, true, 71) &&
          !engine_detail::inputFrameToken(true, false, false, 71) &&
          !engine_detail::inputFrameToken(true, false, true, 0),
          "foreign, nested, and unowned input never borrow a token");
}
void supportedIdentity() {
    constexpr uint32_t stamp=0x6ADA8048,image=0x640C000;
    check(engine_detail::supportedBuildIdentity(true,stamp,image,stamp,image),
          "exact supported known game identity admits the pinned engine hooks");
    check(!engine_detail::supportedBuildIdentity(false,stamp,image,stamp,image),
          "an unknown build cannot admit even an equal caller-supplied identity");
    check(!engine_detail::supportedBuildIdentity(true,stamp+1,image,stamp,image),
          "a future globally known game build does not inherit this layout admission");
    check(!engine_detail::supportedBuildIdentity(true,stamp,image+1,stamp,image),
          "image size mismatch refuses the engine boundary before address resolution");
    check(!engine_detail::supportedBuildIdentity(true,0,image,stamp,image)&&
          !engine_detail::supportedBuildIdentity(true,stamp,0,stamp,image),
          "an unreadable or partial PE identity cannot admit any hook");
}
void distantReplacement() {
    std::array<uint8_t,14> relay{};
    const uintptr_t site=reinterpret_cast<uintptr_t>(relay.data())+0x10000;
    const uintptr_t replacement=site+uintptr_t(0x200000000ull);
    unsigned allocations=0,seals=0,installs=0,releases=0;
    const auto result=engine_detail::installEntryRelay(site,replacement,
        [&](uintptr_t at) { ++allocations;return at==site?relay.data():nullptr; },
        [&](uint8_t*,size_t) { ++seals;return true; },
        [&](void* destination) { ++installs;return engine_detail::relativeEntryReachable(site,reinterpret_cast<uintptr_t>(destination)); },
        [&](uint8_t*) { ++releases; });
    check(!engine_detail::relativeEntryReachable(site,replacement),
          "distant DLL callback reproduces CodeHook's direct rel32 refusal");
    check(result==engine_detail::EntryInstallResult::Installed&&allocations==1&&seals==1&&installs==1&&!releases,
          "distant callback installs through a nearby sealed entry relay rather than the DLL address");
    uint64_t encoded=0;std::memcpy(&encoded,relay.data()+6,sizeof(encoded));
    check(relay[0]==0xFF&&relay[1]==0x25&&!relay[2]&&!relay[3]&&!relay[4]&&!relay[5]&&encoded==replacement,
          "relay is a register-preserving absolute tail jump with the complete distant pointer");
    const uintptr_t next=site+5;
    check(engine_detail::relativeEntryReachable(site,next+uintptr_t(INT32_MAX))&&
          !engine_detail::relativeEntryReachable(site,next+uintptr_t(INT32_MAX)+1)&&
          engine_detail::relativeEntryReachable(site,next-uintptr_t(INT32_MAX)-1)&&
          !engine_detail::relativeEntryReachable(site,next-uintptr_t(INT32_MAX)-2)&&
          !engine_detail::relativeEntryReachable(UINTPTR_MAX-4,site),
          "relative jump admission includes exact signed32 limits and refuses address wrap");
    for (unsigned refusal=0;refusal<4;++refusal) {
        relay.fill(0xA5);allocations=seals=installs=releases=0;
        const uintptr_t attemptedSite=refusal==1?site+uintptr_t(0x200000000ull):site;
        const auto failed=engine_detail::installEntryRelay(attemptedSite,replacement,
            [&](uintptr_t) { ++allocations;return refusal==0?nullptr:relay.data(); },
            [&](uint8_t*,size_t bytes) { ++seals;return bytes==14&&refusal!=2; },
            [&](void*) { ++installs;return refusal!=3; },
            [&](uint8_t* page) { if (page==relay.data()) ++releases; });
        const auto expected=refusal==0?engine_detail::EntryInstallResult::RelayAllocationFailed:
            refusal==1?engine_detail::EntryInstallResult::RelayOutOfRange:
            refusal==2?engine_detail::EntryInstallResult::RelayProtectionFailed:engine_detail::EntryInstallResult::HookRefused;
        check(failed==expected&&allocations==1&&releases==(refusal?1u:0u)&&
              seals==(refusal>=2?1u:0u)&&installs==(refusal==3?1u:0u)&&
              (refusal!=1||relay[0]==0xA5),
              "allocation, out-of-range, executable protection, and hook refusal free only unpublished relays");
    }
}
void retainedJobs() {
    FrameBoundaryCallbacks observer{};
    engine_detail::TicketRegistry<2> registry;
    engine_detail::JobTicket first{{101, 201, 301, 7}, 81, 401, &observer};
    engine_detail::JobTicket second{{102, 201, 302, 8}, 82, 401, &observer};
    engine_detail::JobTicket result{};
    check(engine_detail::ownedJobFrameToken(true, true, first, first.key) == first.token,
          "qualified dynamic scope with exact active head carries the retained token");
    check(!engine_detail::ownedJobFrameToken(false, true, first, first.key) &&
          !engine_detail::ownedJobFrameToken(true, false, first, first.key) &&
          !engine_detail::ownedJobFrameToken(true, true, first, second.key),
          "callbacks, nested stolen jobs, and a different active head cannot borrow worker ownership");
    check(registry.insert(first) && registry.insert(second), "distinct retained tickets may overlap across ticks");
    auto wrong = first.key; ++wrong.generation;
    check(!registry.claim(wrong, first.app, result) && !result.token,
          "same group address with a different generation cannot borrow a token");
    wrong = first.key; ++wrong.executor;
    check(!registry.claim(wrong, first.app, result), "foreign executor fails ticket matching");
    wrong = first.key; ++wrong.payload;
    check(!registry.claim(wrong, first.app, result) && !registry.claim(first.key, first.app + 1, result),
          "payload identity and concrete app ownership must both match");
    check(registry.claim(first.key, first.app, result) && result.token == 81 && result.observer == &observer,
          "an older retained job carries its issuing token rather than the latest issued token");
    check(!registry.claim(first.key, first.app, result) && !result.token,
          "duplicate or nested worker dispatch cannot claim a running ticket twice");
    check(registry.finish(first) && !registry.claim(first.key, first.app, result),
          "completed ticket remains a tombstone until its tick returns");
    auto collision = first; collision.token = 83; ++collision.key.generation;
    check(!registry.insert(collision), "live group address reuse is refused even with a different generation");
    check(!registry.retire(first.token) && registry.insert(collision),
          "retirement releases a completed slot for a future exact reservation");
    check(registry.retire(collision.token) == 1 && !registry.claim(collision.key, collision.app, result),
          "an unexecuted reservation expires at its owner tick return");
    check(registry.claim(second.key, second.app, result) && registry.retire(second.token) == 1,
          "unexpected running work is retired without replacing its stable ownership");
    check(!registry.claim(second.key, second.app, result) && registry.finish(second) && registry.insert(second),
          "retired running work cannot be reclaimed and frees only after callback completion");
    auto invalid = first; invalid.token = 0;
    check(!registry.insert(invalid), "zero invocation identity cannot produce an owned worker");
    registry.retire(second.token);
    check(!registry.claim(first.key, first.app, result), "missing ticket has no global counter fallback");
}
void mirrorSources() {
    const EdvrNativeReflexSource first{sizeof(EdvrNativeReflexSource),EDVR_NATIVE_REFLEX_FRAME_VERSION_1,4,81,9001};
    const EdvrNativeReflexSource second{sizeof(EdvrNativeReflexSource),EDVR_NATIVE_REFLEX_FRAME_VERSION_1,4,82,9002};
    EdvrNativeReflexSource eyes[2]{first,first};
    check(validSource(first) && sameSource(selectMirrorSource(eyes,3,{}),first),
          "matching retained stereo sources select their exact pose and issuing token");
    eyes[1]=second;
    check(!validSource(selectMirrorSource(eyes,3,{})), "mixed stereo eye generations remain unowned");
    check(sameSource(selectMirrorSource(eyes,1,{}),first) && sameSource(selectMirrorSource(eyes,2,{}),second),
          "single-eye mirror ownership comes only from the eye actually selected");
    check(!validSource(selectMirrorSource(eyes,1,first)) && !validSource(selectMirrorSource(eyes,1,second)),
          "repeated and backward sources cannot assign a new Present");
    check(!validSource(selectMirrorSource(eyes,0,{})) && !validSource(selectMirrorSource(eyes,4,{})),
          "game mirror mode and malformed eye masks cannot fabricate a source");
    eyes[0]={};
    check(!validSource(selectMirrorSource(eyes,1,{})) && !validSource(selectMirrorSource(eyes,3,{})),
          "unsupported peer and absent eye metadata leave ownership unknown");
    eyes[0]=first; ++eyes[0].version;
    check(!validSource(eyes[0]), "unsupported source schema is rejected");
    eyes[0]=first; --eyes[0].size;
    check(!validSource(eyes[0]), "truncated source metadata is rejected");
    eyes[0]=first; eyes[0].generation=0;
    check(!validSource(eyes[0]), "missing native session cannot claim a texture source");
    eyes[0]=first; eyes[0].poseSequence=0;
    check(!validSource(eyes[0]), "missing actual runtime pose receipt cannot claim a source");
    eyes[0]=first; ++eyes[0].poseSequence;
    check(!sameSource(eyes[0],first), "matching job token alone cannot join different pose receipts");
    eyes[0]=first; ++eyes[0].generation;
    check(!sameSource(eyes[0],first), "matching pose and token from another native generation cannot join");
}
} // namespace
int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "--dry-run")) {
        std::puts("native_reflex_engine_test: dry-run (no game hooks or graphics)"); return 0;
    }
    callbackOrder(); exceptionalForward(); callbackTable(); inputTokens(); supportedIdentity(); distantReplacement(); retainedJobs(); mirrorSources();
    std::printf("native_reflex_engine_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

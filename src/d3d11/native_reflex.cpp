#include "native_reflex.h"

#include "../common/guard.h"
#include "../common/log.h"
#include "nvapi_module_path.h"

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <string>

namespace edvr::native_reflex::detail {
namespace {

constexpr char kNonNvidiaAdapter[] = "non_nvidia_adapter";

using Query = void* (__cdecl*)(uint32_t);
using Initialize = int (__cdecl*)();
using SetMode = int (__cdecl*)(IUnknown*, SetSleepMode*);
using GetStatus = int (__cdecl*)(IUnknown*, GetSleepStatus*);
using Sleep = int (__cdecl*)(IUnknown*);
using SetMarker = int (__cdecl*)(IUnknown*, MarkerParams*);
using GetLatency = int (__cdecl*)(IUnknown*, LatencyResult*);

struct Driver {
    SetMode set = nullptr;
    GetStatus get = nullptr;
    const char* reason = "driver_unavailable";
    Sleep sleep = nullptr;
    SetMarker setMarker = nullptr;
    GetLatency getLatency = nullptr;
};

HMODULE moduleOf(void* address, DWORD flags) noexcept {
    HMODULE module = nullptr;
    if (address)
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | flags,
                           reinterpret_cast<LPCWSTR>(address), &module);
    return module;
}

std::wstring_view modulePath(HMODULE module, wchar_t (&buffer)[MAX_PATH]) noexcept {
    const DWORD size = module ? GetModuleFileNameW(module, buffer, MAX_PATH) : 0;
    return size && size < MAX_PATH ? std::wstring_view(buffer, size) : std::wstring_view{};
}

// Use System32's NVAPI DLL. Check the actual loaded modules before calling or
// pinning their entry points. A successful load stays resident for this process.
Driver loadDriver() {
    wchar_t system[MAX_PATH]{}, stubPath[MAX_PATH]{};
    const UINT size = GetSystemDirectoryW(system, MAX_PATH);
    if (!size || size >= MAX_PATH) return {};
    const std::wstring path = std::wstring(system, size) + L"\\nvapi64.dll";
    HMODULE stub = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!stub) return {};
    const auto driver = [&]() -> Driver {
        const auto publicPath = modulePath(stub, stubPath);
        if (!nvapi::nvapiSystemStubPath(system, publicPath))
            return {nullptr, nullptr, "driver_path_refused"};
        const auto query = reinterpret_cast<Query>(GetProcAddress(stub, "nvapi_QueryInterface"));
        if (moduleOf(reinterpret_cast<void*>(query), GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT) != stub)
            return {nullptr, nullptr, "driver_query_unavailable"};
        void* initialize = nullptr;
        void* set = nullptr;
        void* get = nullptr;
        if (!guarded("nativeReflex/query", [&] {
                // https://github.com/NVIDIA/nvapi/blob/main/nvapi_interface.h
                initialize = query(0x0150e828); // NvAPI_Initialize
                set = query(0xac1ca9e0);        // NvAPI_D3D_SetSleepMode
                get = query(0xaef96ca1);        // NvAPI_D3D_GetSleepStatus
            }) || !initialize || !set || !get)
            return {nullptr, nullptr, "driver_api_unavailable"};
        const auto trusted = [&](void* address) {
            wchar_t buffer[MAX_PATH]{};
            const auto owner = moduleOf(address, GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT);
            const auto ownerPath = modulePath(owner, buffer);
            return nvapi::nvapiOwnerPathKind(system, publicPath, ownerPath, ownerPath) != 0 &&
                   moduleOf(address, GET_MODULE_HANDLE_EX_FLAG_PIN) == owner;
        };
        if (!trusted(initialize) || !trusted(set) || !trusted(get))
            return {nullptr, nullptr, "driver_owner_refused"};
        int initialized = kDriverFault;
        guarded("nativeReflex/initialize", [&] { initialized = reinterpret_cast<Initialize>(initialize)(); });
        if (initialized != 0) return {nullptr, nullptr, "driver_initialize_failed"};
        Driver loaded{reinterpret_cast<SetMode>(set), reinterpret_cast<GetStatus>(get), nullptr};
        void* sleep = nullptr;
        void* marker = nullptr;
        void* latency = nullptr;
        if (guarded("nativeReflex/frameQuery", [&] {
                sleep = query(0x852cd1d2);   // NvAPI_D3D_Sleep
                marker = query(0xd9984c05);  // NvAPI_D3D_SetLatencyMarker
                latency = query(0x1a587f9c); // NvAPI_D3D_GetLatency
            }) && trusted(sleep) && trusted(marker) && trusted(latency)) {
            loaded.sleep = reinterpret_cast<Sleep>(sleep);
            loaded.setMarker = reinterpret_cast<SetMarker>(marker);
            loaded.getLatency = reinterpret_cast<GetLatency>(latency);
        }
        return loaded;
    }();
    if (!driver.set) FreeLibrary(stub);
    return driver;
}

const Driver& driverFunctions() {
    static const Driver driver = loadDriver();
    return driver;
}

const char* deviceRefusal(ID3D11Device* device) {
    using Microsoft::WRL::ComPtr;
    if (!device) return "device_unavailable";
    if (FAILED(device->GetDeviceRemovedReason())) return "device_removed";
    if (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
        return "singlethreaded_device";
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
        FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc)))
        return "adapter_unavailable";
    return desc.VendorId != 0x10de ? kNonNvidiaAdapter : nullptr;
}

int readStatus(ID3D11Device* device, const Driver& driver, GetSleepStatus& status) {
    int code = kDriverFault;
    guarded("nativeReflex/getStatus", [&] { code = driver.get(device, &status); });
    return code;
}

int setMode(ID3D11Device* device, const Driver& driver, SetSleepMode& params) {
    int code = kDriverFault;
    guarded("nativeReflex/setMode", [&] { code = driver.set(device, &params); });
    return code;
}

// Keep our accepted Sleep proof on the device itself, so XR sessions can
// share it without pinning retired devices or risking pointer reuse.
// https://learn.microsoft.com/windows/win32/api/d3d11/nf-d3d11-id3d11device-setprivatedata
constexpr GUID kOwnSleepKey{0x5dfc31c3, 0xf76c, 0x4bfa, {0xb8, 0xdc, 0xfa, 0x3e, 0x6f, 0x09, 0xed, 0x5c}};
const char kOwnSleepToken = 0;
const char kPendingSleepToken = 1;
enum class DeviceSleepProof { None, Pending, Owned };

DeviceSleepProof deviceSleepProof(ID3D11Device* device) noexcept {
    uintptr_t token = 0;
    UINT bytes = static_cast<UINT>(sizeof(token));
    HRESULT result = E_FAIL;
    guarded("nativeReflex/readSleepProof", [&] { result = device->GetPrivateData(kOwnSleepKey, &bytes, &token); });
    if (FAILED(result) || bytes != sizeof(token)) return DeviceSleepProof::None;
    if (token == reinterpret_cast<uintptr_t>(&kPendingSleepToken)) return DeviceSleepProof::Pending;
    return token == reinterpret_cast<uintptr_t>(&kOwnSleepToken) ? DeviceSleepProof::Owned : DeviceSleepProof::None;
}

bool storeDeviceSleepProof(ID3D11Device* device, DeviceSleepProof proof) noexcept {
    const bool clear = proof == DeviceSleepProof::None;
    const auto token = reinterpret_cast<uintptr_t>(
        proof == DeviceSleepProof::Owned ? &kOwnSleepToken : &kPendingSleepToken);
    HRESULT result = E_FAIL;
    guarded("nativeReflex/storeSleepProof", [&] {
        result = device->SetPrivateData(kOwnSleepKey, clear ? 0 : static_cast<UINT>(sizeof(token)),
                                        clear ? nullptr : &token);
    });
    return SUCCEEDED(result);
}

std::string statusWord(int status) {
    if (status == kUnavailable) return "na";
    if (status == kDriverFault) return "fault";
    return std::to_string(status);
}

}  // namespace

// The enrolled D3D11 device remains alive through normal XR close. Validate
// vendor and threading before issuing a device-scoped mode request.
Result apply(ID3D11Device* device, Mode mode, bool alreadyOwned) {
    if (const char* refused = deviceRefusal(device)) return {refused};
    const auto& driver = driverFunctions();
    if (!driver.set) return {driver.reason};
    return requestMode(mode, alreadyOwned,
        [&](GetSleepStatus& status) { return readStatus(device, driver, status); },
        [&](SetSleepMode& params) { return setMode(device, driver, params); });
}

Result applyFrameMode(FrameDriver& frameDriver, ID3D11Device* device, Mode mode, bool alreadyOwned) {
    if (device != frameDriver.device_) return {"frame_device_unavailable"};
    if (frameDriver.frames_.retired()) {
        if (canWithdrawRetiredMode(mode, alreadyOwned)) return apply(device, mode, true);
        return {"frame_device_unavailable"};
    }
    if (FAILED(device->GetDeviceRemovedReason())) return {"device_removed"};
    const auto& driver = driverFunctions();
    const auto admission = frameDriver.firstSleep_.beginModeUpdate();
    if (admission == FirstSleepGate::Admission::Busy) {
        Result pending{"first_sleep_pending", true};
        pending.retry = true;
        return pending;
    }
    // The game-sleep bit can describe our calls in Off or the preceding XR
    // session. Continue to check effective mode and interval until we own a
    // mode request.
    const bool ownGameSleep = admission == FirstSleepGate::Admission::Owned;
    auto result = requestMode(mode, alreadyOwned,
        [&](GetSleepStatus& status) { return readStatus(device, driver, status); },
        [&](SetSleepMode& params) { return setMode(device, driver, params); },
        ownGameSleep, true);
    if (result.setStatus == 0) frameDriver.configured_.store(true, std::memory_order_release);
    if (!ownGameSleep) frameDriver.firstSleep_.finish(false);
    return result;
}

// An accepted request on this device already resolved and pinned the API.
Result observe(ID3D11Device* device, Mode mode) {
    Result result{"device_removed", true};
    if (SUCCEEDED(device->GetDeviceRemovedReason()))
        readMode(result, mode, [&](GetSleepStatus& status) { return readStatus(device, driverFunctions(), status); });
    return result;
}

void logMode(Mode mode, const char* event, uint64_t session, const Result& result, unsigned presents) {
    const char* requested = modeWord(mode);
    const auto set = statusWord(result.setStatus), get = statusWord(result.getStatus);
    const auto readbackBit = [&](uint8_t value) {
        return result.getStatus != 0 ? "unknown" : value != 0 ? "1" : "0";
    };
    const std::string observation = presents
        ? ",after_presents=" + std::to_string(presents) +
              ",final=" + (presents == ModeHistory::kLastObservation ? "1" : "0")
        : std::string{};
    Log::get().note(
        "%s,requested=%s,event=%s,api_resolved=%u,set_status=%s,get_status=%s,"
        "driver_enabled=%s,boost_requested=%u,game_sleep=%s,reason=%s,session=%llu%s",
        presents ? "native_reflex_readback" : "native_reflex", requested, event,
        unsigned(result.apiResolved), set.c_str(), get.c_str(),
        readbackBit(result.readback.lowLatencyMode), unsigned(mode == Mode::Boost),
        readbackBit(result.readback.useGameSleep), result.reason,
        static_cast<unsigned long long>(session), observation.c_str());
}

}  // namespace edvr::native_reflex::detail

namespace edvr::native_reflex {

bool isNonNvidiaAdapter(ID3D11Device* device) {
    return detail::deviceRefusal(device) == detail::kNonNvidiaAdapter;
}

FrameDriver::FrameDriver(ID3D11Device* device, bool ownGameSleep)
    : device_(device), firstSleep_(ownGameSleep) {
    device_->AddRef();
}

FrameDriver::~FrameDriver() {
    device_->Release();
}

std::shared_ptr<FrameDriver> openFrameDriver(ID3D11Device* device) {
    using namespace detail;
    const auto refuse = [](const char* reason) -> std::shared_ptr<FrameDriver> {
        Log::get().note("native_reflex_frame,event=enroll,reason=%s", reason);
        return {};
    };
    if (const auto* reason = deviceRefusal(device)) return refuse(reason);
    const auto& driver = driverFunctions();
    if (!driver.set) return refuse(driver.reason);
    if (!driver.sleep || !driver.setMarker || !driver.getLatency) return refuse("frame_api_unavailable");
    const auto proof = deviceSleepProof(device);
    if (proof == DeviceSleepProof::Pending) return refuse("first_sleep_pending");
    GetSleepStatus baseline{};
    baseline.version = version1(sizeof(baseline));
    if (readStatus(device, driver, baseline) != 0) return refuse("baseline_unknown");
    const bool ownGameSleep = baseline.useGameSleep && proof == DeviceSleepProof::Owned;
    if (hasExternalDriverPolicy(baseline, ownGameSleep)) return refuse("existing_driver_policy");
    return std::shared_ptr<FrameDriver>(new FrameDriver(device, ownGameSleep));
}

// A shell/driver fault or lost device invalidates transport for this device.
// Probe the device only on failure, outside the normal per-frame path.
void FrameDriver::reportFailure(std::atomic<bool>& logged, const char* event, int code) noexcept {
    if (code == detail::kDriverFault || FAILED(device_->GetDeviceRemovedReason())) retire();
    if (!logged.exchange(true, std::memory_order_relaxed))
        Log::get().note("native_reflex_frame,event=%s,nvapi_status=%d", event, code);
}

FrameDriver::SleepResult FrameDriver::sleepFrame(uint64_t sourceFrameId) noexcept {
    using namespace detail;
    if (!configured_.load(std::memory_order_acquire)) return SleepResult::Skipped;
    SleepResult result = SleepResult::Skipped;
    const bool accepted = frames_.sleep(sourceFrameId, [&] {
        const auto admission = firstSleep_.beginSleep();
        if (admission == FirstSleepGate::Admission::Busy) return false;
        const bool first = admission == FirstSleepGate::Admission::Unproven;
        if (first && !storeDeviceSleepProof(device_, DeviceSleepProof::Pending)) {
            retire();
            firstSleep_.finish(false);
            Log::get().note("native_reflex_frame,event=sleep,reason=device_proof_unavailable");
            result = SleepResult::Failed;
            return false;
        }
        // If close raced the reservation, do not initiate a vendor wait.
        if (frames_.retired()) {
            if (first) {
                storeDeviceSleepProof(device_, DeviceSleepProof::None);
                firstSleep_.finish(false);
            }
            return false;
        }
        const auto call = driverFunctions().sleep;
        int code = kDriverFault;
        guarded("nativeReflex/sleep", [&] { code = call(device_); });
        if (first) {
            if (code != 0 || !storeDeviceSleepProof(device_, DeviceSleepProof::Owned))
                storeDeviceSleepProof(device_, DeviceSleepProof::None);
            firstSleep_.finish(code == 0);
        }
        if (code != 0) {
            reportFailure(loggedSleepFailure_, "sleep", code);
            result = SleepResult::Failed;
        }
        return code == 0;
    });
    return accepted ? SleepResult::Accepted : result;
}

bool FrameDriver::mark(uint64_t sourceFrameId, Marker marker) noexcept {
    using namespace detail;
    if (marker > Marker::PresentEnd && marker != Marker::TriggerFlash && marker != Marker::LatencyPing)
        return false;
    if (!frames_.contains(sourceFrameId)) return false;
    auto params = markerParameters(sourceFrameId, marker);
    const auto call = driverFunctions().setMarker;
    int code = kDriverFault;
    guarded("nativeReflex/setLatencyMarker", [&] { code = call(device_, &params); });
    if (code != 0) reportFailure(loggedMarkerFailure_, "marker", code);
    return code == 0;
}

bool FrameDriver::getLatency(LatencyBatch& output, uint64_t afterFrameId) noexcept {
    using namespace detail;
    output.count = 0;
    const auto first = frames_.firstFrame();
    if (!first || frames_.retired()) return false;
    LatencyResult raw{};
    raw.version = version1(sizeof(raw));
    const auto call = driverFunctions().getLatency;
    int code = kDriverFault;
    guarded("nativeReflex/getLatency", [&] { code = call(device_, &raw); });
    if (code != 0) {
        reportFailure(loggedReportFailure_, "get_latency", code);
        return false;
    }
    if (frames_.retired()) return false;
    return collectLatency(raw, first, frames_.lastAttemptedFrame(), afterFrameId, output);
}

}  // namespace edvr::native_reflex

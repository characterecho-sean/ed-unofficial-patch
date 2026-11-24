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

using Query = void* (__cdecl*)(uint32_t);
using Initialize = int (__cdecl*)();
using SetMode = int (__cdecl*)(IUnknown*, SetSleepMode*);
using GetStatus = int (__cdecl*)(IUnknown*, GetSleepStatus*);

struct Driver {
    SetMode set = nullptr;
    GetStatus get = nullptr;
    const char* reason = "driver_unavailable";
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
                // IDs from NVIDIA/nvapi's nvapi_interface.h.
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
        return {reinterpret_cast<SetMode>(set), reinterpret_cast<GetStatus>(get), nullptr};
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
    if (FAILED(device->GetDeviceRemovedReason())) return "device_removed";
    if (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
        return "singlethreaded_device";
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
        FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc)))
        return "adapter_unavailable";
    return desc.VendorId != 0x10de ? "non_nvidia_adapter" : nullptr;
}

int readStatus(ID3D11Device* device, const Driver& driver, GetSleepStatus& status) {
    int code = kDriverFault;
    guarded("nativeReflex/getStatus", [&] { code = driver.get(device, &status); });
    return code;
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
        [&](SetSleepMode& params) {
            int code = kDriverFault;
            guarded("nativeReflex/setMode", [&] { code = driver.set(device, &params); });
            return code;
        });
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

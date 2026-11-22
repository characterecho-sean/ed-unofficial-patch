// Flat frame generation (fix.frame_generation). NVIDIA and AMD, through AMD's signed
// frame-generation DLL and a null swap chain. A VR session does not start it. AMD's
// frame-generation swap chain is never created. The presenting chain is ours: one
// generated frame, then the real frame, each after a wait on that chain's frame-latency object.
//
// The 64x64 probe dispatch is discarded. Its output is not presented.

#include "frame_gen.h"

#include "frame_gen_decision.h"
#include "fsr4_engine.h"
#include "fsr4_version.h"
#include "flat_runtime.h"
#include "flat_ui_layer.h"
#include "perf_monitor.h"
#include "temporal_pass.h"
#include "ui_layer.h"

#include "../common/config.h"
#include "../common/log.h"
#include "../common/runtime_profile.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef EDVR_HAVE_FSR4
#define EDVR_HAVE_FSR4 0
#endif

#if EDVR_HAVE_FSR4
#include <d3d12.h>
#include <wrl/client.h>
#pragma warning(push, 0)
#include <FidelityFX/api/include/ffx_api.h>
#include <FidelityFX/api/include/dx12/ffx_api_dx12.h>
#include <FidelityFX/framegeneration/include/ffx_framegeneration.h>
#pragma warning(pop)
using Microsoft::WRL::ComPtr;
#endif

namespace edvr {
namespace {

struct Boot {
    FrameGenKey key = FrameGenKey::Off;
    FrameGenKey readyKey = FrameGenKey::Off;
    bool blocked = false;
    bool probeFailed = false;
    bool productionFailed = false;
    bool hdrNoLuma = false;
    bool formatUnsupported = false;
    bool xessOwns = false;
    bool ready = false;
    bool engaged = false;
    bool totals = false;
    bool stopped = false;
    uint64_t applied = 0;
    char temporalMode[32] = "off";
    char offered[96] = "";
    char providerText[240] = "";
};

Boot g_boot;

FrameGenKey readKey() {
    const std::string v = Config::get().getString("fix.frame_generation", "off");
    if (_stricmp(v.c_str(), "on") == 0) return FrameGenKey::On;
    if (_stricmp(v.c_str(), "auto") == 0) return FrameGenKey::Auto;
    return FrameGenKey::Off;
}

const char* keyName(FrameGenKey key) {
    if (key == FrameGenKey::On) return "on";
    if (key == FrameGenKey::Auto) return "auto";
    return "off";
}

void rememberLine(const char* line) {
    static char seen[24][220];
    static int count = 0;
    if (!line || !line[0]) return;
    for (int i = 0; i < count; ++i)
        if (std::strcmp(seen[i], line) == 0) return;
    if (count < 24) std::snprintf(seen[count++], sizeof(seen[0]), "%s", line);
    Log::get().note("%s", line);
}

void logRefusal(const FrameGenFacts& facts) {
    char scratch[512];
    const char* line = frameGenHardRefusal(facts, scratch, sizeof(scratch));
    if (line) rememberLine(line);
}

void logTotals() {
    if (!g_boot.ready || g_boot.totals) return;
    g_boot.totals = true;
    const char* key = keyName(g_boot.readyKey);
    const bool never = (g_boot.readyKey == FrameGenKey::On && g_boot.applied == 0 && !runtimeVrProfile()) ||
                       (g_boot.readyKey == FrameGenKey::Auto && g_boot.engaged && g_boot.applied == 0);
    if (never)
        Log::get().note("frame generation totals: ready 1, key %s, applied %llu, NEVER APPLIED",
                        key, static_cast<unsigned long long>(g_boot.applied));
    else
        Log::get().note("frame generation totals: ready 1, key %s, applied %llu",
                        key, static_cast<unsigned long long>(g_boot.applied));
}

void onProcessExit() { logTotals(); }

bool startupRefusal() {
    g_boot.key = readKey();
    if (g_boot.key == FrameGenKey::Off) return true;
    g_boot.blocked = runtimeVrProfile();
    if (g_boot.blocked) {
        FrameGenFacts facts;
        facts.key = g_boot.key;
        facts.vr = true;
        logRefusal(facts);
        return true;
    }
    const std::string aa = Config::get().getString("fix.temporal_aa", "off");
    std::snprintf(g_boot.temporalMode, sizeof(g_boot.temporalMode), "%s", aa.c_str());
    if (_stricmp(aa.c_str(), "off") == 0) {
        g_boot.blocked = true;
        FrameGenFacts facts;
        facts.key = g_boot.key;
        facts.temporalAaOff = true;
        logRefusal(facts);
        return true;
    }
    return false;
}

#if EDVR_HAVE_FSR4

constexpr DWORD kKeyWaitMs = 100;
constexpr DWORD kPresentWaitMs = 500;
constexpr DWORD kProbeWaitMs = 2000;
constexpr unsigned kRing = 3;
constexpr uint32_t kProbeSize = 64;

using CreateD3D12DeviceFn = HRESULT (WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using CreateFactory2Fn = HRESULT (WINAPI*)(UINT, REFIID, void**);

struct FfxApi {
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
};

struct SharedTex {
    ComPtr<ID3D11Texture2D> d11;
    ComPtr<IDXGIKeyedMutex> key;
    ComPtr<ID3D12Resource> d12;
    HANDLE handle = nullptr;
    bool held = false;
    void releaseHeld() {
        if (held && key) key->ReleaseSync(0);
        held = false;
    }
    void reset() {
        releaseHeld();
        d11.Reset();
        key.Reset();
        d12.Reset();
        if (handle) {
            CloseHandle(handle);
            handle = nullptr;
        }
    }
};

struct CommandSlot {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> readback;
    const uint64_t* ticks = nullptr;
    uint64_t doneValue = 0;
    bool timingPending = false;
    void reset() {
        if (readback && ticks) readback->Unmap(0, nullptr);
        ticks = nullptr;
        timingPending = false;
        doneValue = 0;
        list.Reset();
        allocator.Reset();
        heap.Reset();
        readback.Reset();
    }
};

struct FgDescs {
    ffxCreateContextDescFrameGeneration create{};
    ffxCreateBackendDX12Desc backend{};
    ffxCreateContextDescFrameGenerationVersion version{};
    ffxOverrideVersion overrideVersion{};
};

struct Pending {
    bool valid = false;
    bool haveInputs = false;
    bool haveBasis = false;
    bool hdr = false;
    bool reset = false;
    bool signaled = false;
    uint64_t frame = 0;
    uint64_t producer = 0;
    uint32_t renderW = 0, renderH = 0, outW = 0, outH = 0;
    float deltaMs = 0, jitterX = 0, jitterY = 0;
    float camNear = 0, camFar = 0, fovY = 0;
    float pos[3]{};
    float up[3]{};
    float right[3]{};
    float forward[3]{};
    char inputWhy[64] = "no dlOut depth or motion";
};

struct Session {
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11Device5> d11v5;
    ComPtr<ID3D11DeviceContext> immediate;
    ComPtr<ID3D11DeviceContext4> ctx4;
    ComPtr<ID3D11Device1> dev1;
    ComPtr<ID3D11DeviceContext1> ctx1;
    ComPtr<ID3DDeviceContextState> isolate;
    ComPtr<ID3D12Device> d12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence12;
    ComPtr<ID3D11Fence> fence11;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGISwapChain2> presenter;
    ComPtr<ID3D11Texture2D> blendTarget;
    HMODULE d3d12Module = nullptr;
    HMODULE dxgiModule = nullptr;
    HMODULE loaderModule = nullptr;
    HANDLE fenceHandle = nullptr;
    HANDLE cpuEvent = nullptr;
    HANDLE latency = nullptr;
    HWND hidden = nullptr;
    HWND real = nullptr;
    IDXGISwapChain* hiddenChain = nullptr;
    FfxApi api;
    ffxContext context = nullptr;
    FgDescs* descs = nullptr;
    SharedTex colour, depth, motion, output;
    CommandSlot ring[kRing];
    Pending pending;
    bool borrowed = false;
    bool isolateReady = false;
    bool bridged = false;
    bool tearing = false;
    bool exclusive = false;
    bool flagsNoted = false;
    bool forceReset = true;
    bool uiOffNoted = false;
    bool heldOffNoted = false;
    bool refreshNoted = false;
    bool wasEngaged = false;
    bool haveTimingSeed = false;
    uint64_t nextFence = 0;
    uint64_t frameId = 1;
    uint64_t skipped = 0;
    uint64_t resets = 0;
    uint64_t presented = 0;
    uint64_t rendered = 0;
    uint64_t lastCadenceMs = 0;
    int64_t lastReturnQpc = 0;
    uint32_t vendor = 0;
    uint32_t displayW = 0, displayH = 0;
    uint32_t maxRenderW = 0, maxRenderH = 0;
    uint32_t presenterFlags = 0;
    DXGI_FORMAT presenterFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT colourFormat = DXGI_FORMAT_UNKNOWN;
    FrameGenColourContract colourContract{};
    double dispatchGpuMs = -1;
    double gpuSamples[64]{};
    unsigned gpuCount = 0;
    unsigned gpuWrite = 0;
    uint64_t timestampHz = 0;
    uint64_t versionId = 0;
    struct Skip {
        char reason[48];
        uint64_t count = 0;
        uint64_t lastMs = 0;
    };
    Skip skips[8];
};

Session* g_session = nullptr;

const char* ffxReturnName(ffxReturnCode_t rc) {
    switch (rc) {
    case FFX_API_RETURN_OK: return "ok";
    case FFX_API_RETURN_ERROR: return "error";
    case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "unknown descriptor type";
    case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "runtime error";
    case FFX_API_RETURN_NO_PROVIDER: return "no provider";
    case FFX_API_RETURN_ERROR_MEMORY: return "out of memory";
    case FFX_API_RETURN_ERROR_PARAMETER: return "bad parameter";
    case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return "provider too old for this descriptor";
    default: return "unrecognised code";
    }
}

template <typename T>
bool loadProc(HMODULE module, const char* name, T& target) {
    target = reinterpret_cast<T>(GetProcAddress(module, name));
    return target != nullptr;
}

uint64_t takeFence(Session& s) {
    if (s.borrowed) {
        const uint64_t value = fsr4TakeFenceValue();
        return value ? value : 0;
    }
    return ++s.nextFence;
}

bool moduleDirectory(wchar_t* directory, size_t count) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&frameGenStartup), &self) ||
        !self)
        return false;
    const DWORD length = GetModuleFileNameW(self, directory, static_cast<DWORD>(count));
    if (!length || length >= count) return false;
    wchar_t* slash = wcsrchr(directory, L'\\');
    if (!slash) return false;
    slash[1] = L'\0';
    return true;
}

bool fileBeside(const wchar_t* directory, const wchar_t* name, wchar_t* path, size_t count) {
    if (wcslen(directory) + wcslen(name) >= count) return false;
    wcscpy_s(path, count, directory);
    wcscat_s(path, count, name);
    const DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void setProvider(const char* text) {
    std::snprintf(g_boot.providerText, sizeof(g_boot.providerText), "%s", text ? text : "");
}

void failProbe(const char* text) {
    g_boot.probeFailed = true;
    setProvider(text);
    FrameGenFacts facts;
    facts.key = g_boot.key;
    facts.probeFailed = true;
    facts.providerText = g_boot.providerText;
    logRefusal(facts);
}

void failMissingDll() {
    g_boot.probeFailed = true;
    FrameGenFacts facts;
    facts.key = g_boot.key;
    facts.providerDllMissing = true;
    logRefusal(facts);
}

void failNoVersion(const char* text) {
    g_boot.probeFailed = true;
    setProvider(text ? text : "");
    FrameGenFacts facts;
    facts.key = g_boot.key;
    facts.queryHasNoVersion = true;
    facts.providerText = g_boot.providerText[0] ? g_boot.providerText : nullptr;
    logRefusal(facts);
}

void failRejected() {
    g_boot.probeFailed = true;
    FrameGenFacts facts;
    facts.key = g_boot.key;
    facts.createRejectedOffer = true;
    facts.offeredVersion = g_boot.offered[0] ? g_boot.offered : nullptr;
    logRefusal(facts);
}

bool makeShared(Session& s, SharedTex& target, const char* role, uint32_t width, uint32_t height,
                DXGI_FORMAT format, bool writable) {
    target.reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET |
                     (writable ? D3D11_BIND_UNORDERED_ACCESS : 0);
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = s.d11->CreateTexture2D(&desc, nullptr, target.d11.GetAddressOf());
    if (FAILED(hr)) {
        Log::get().note("fg bridge: shared %s texture failed 0x%08X", role, unsigned(hr));
        return false;
    }
    if (FAILED(target.d11.As(&target.key))) target.key.Reset();
    ComPtr<IDXGIResource1> resource;
    hr = target.d11.As(&resource);
    if (FAILED(hr)) return false;
    hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                      &target.handle);
    if (FAILED(hr) || !target.handle) return false;
    hr = s.d12->OpenSharedHandle(target.handle, IID_PPV_ARGS(target.d12.GetAddressOf()));
    if (FAILED(hr)) {
        Log::get().note("fg bridge: shared %s D3D12 open failed 0x%08X", role, unsigned(hr));
        return false;
    }
    return true;
}

bool takeKey(SharedTex& tex) {
    if (tex.held || !tex.key) return true;
    const HRESULT hr = tex.key->AcquireSync(0, kKeyWaitMs);
    if (hr == S_OK) return true;
    if (hr == static_cast<HRESULT>(WAIT_ABANDONED)) tex.key->ReleaseSync(0);
    return false;
}

void keepKey(SharedTex& tex) {
    if (tex.key) tex.held = true;
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* const* resources,
                const D3D12_RESOURCE_STATES* before, const D3D12_RESOURCE_STATES* after, UINT count) {
    D3D12_RESOURCE_BARRIER barriers[4]{};
    for (UINT i = 0; i < count && i < 4; ++i) {
        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition.pResource = resources[i];
        barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[i].Transition.StateBefore = before[i];
        barriers[i].Transition.StateAfter = after[i];
    }
    list->ResourceBarrier(count, barriers);
}

bool makeTiming(Session& s, CommandSlot& slot) {
    if (!s.timestampHz) return false;
    D3D12_QUERY_HEAP_DESC heap{};
    heap.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heap.Count = 2;
    if (FAILED(s.d12->CreateQueryHeap(&heap, IID_PPV_ARGS(slot.heap.GetAddressOf())))) return false;
    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 2 * sizeof(uint64_t);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(s.d12->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(slot.readback.GetAddressOf()))))
        return false;
    void* mapped = nullptr;
    if (FAILED(slot.readback->Map(0, nullptr, &mapped)) || !mapped) return false;
    slot.ticks = static_cast<const uint64_t*>(mapped);
    return true;
}

void noteGpuSample(Session& s, double ms) {
    if (!(ms >= 0.0)) return;
    s.dispatchGpuMs = ms;
    s.gpuSamples[s.gpuWrite % 64] = ms;
    s.gpuWrite++;
    if (s.gpuCount < 64) s.gpuCount++;
}

void readSlotTiming(Session& s, CommandSlot& slot, uint64_t completed) {
    if (!slot.timingPending || !slot.ticks || completed < slot.doneValue) return;
    slot.timingPending = false;
    if (s.timestampHz && slot.ticks[1] > slot.ticks[0])
        noteGpuSample(s, static_cast<double>(slot.ticks[1] - slot.ticks[0]) * 1000.0 /
                             static_cast<double>(s.timestampHz));
}

bool ensureIsolate(Session& s) {
    if (s.isolateReady) return true;
    if (!s.dev1 || !s.ctx1) return false;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL chosen = D3D_FEATURE_LEVEL_11_0;
    const HRESULT hr = s.dev1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device),
                                                        &chosen, s.isolate.GetAddressOf());
    s.isolateReady = SUCCEEDED(hr) && s.isolate;
    return s.isolateReady;
}

struct IsolateScope {
    Session* session = nullptr;
    ComPtr<ID3DDeviceContextState> prior;
    bool on = false;
    explicit IsolateScope(Session& s) : session(&s) {
        if (!ensureIsolate(s)) return;
        s.ctx1->SwapDeviceContextState(s.isolate.Get(), prior.GetAddressOf());
        on = true;
    }
    ~IsolateScope() {
        if (on && session && session->ctx1) session->ctx1->SwapDeviceContextState(prior.Get(), nullptr);
    }
};

void copyBox(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, ID3D11Texture2D* src, uint32_t w, uint32_t h, bool exact) {
    if (!ctx || !dst || !src) return;
    if (exact) {
        ctx->CopyResource(dst, src);
        return;
    }
    D3D11_BOX box{};
    box.right = w;
    box.bottom = h;
    box.back = 1;
    ctx->CopySubresourceRegion(dst, 0, 0, 0, 0, src, 0, &box);
}

bool sameSize(ID3D11Texture2D* tex, uint32_t w, uint32_t h) {
    if (!tex) return false;
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    return desc.Width == w && desc.Height == h;
}

void releaseContext(Session& s) {
    if (s.context && s.api.destroy) s.api.destroy(&s.context, nullptr);
    s.context = nullptr;
    delete s.descs;
    s.descs = nullptr;
    s.colour.reset();
    s.depth.reset();
    s.motion.reset();
    s.output.reset();
    s.blendTarget.Reset();
    s.forceReset = true;
    s.frameId = 1;
}

void releaseGpu(Session& s) {
    releaseContext(s);
    for (CommandSlot& slot : s.ring) slot.reset();
    if (s.borrowed) {
        s.d12.Reset();
        s.queue.Reset();
        s.fence12.Reset();
        s.fence11.Reset();
        s.borrowed = false;
    } else {
        s.fence11.Reset();
        s.fence12.Reset();
        s.queue.Reset();
        s.d12.Reset();
    }
    if (s.fenceHandle) {
        CloseHandle(s.fenceHandle);
        s.fenceHandle = nullptr;
    }
    if (s.cpuEvent) {
        CloseHandle(s.cpuEvent);
        s.cpuEvent = nullptr;
    }
    s.d11v5.Reset();
    s.ctx4.Reset();
    s.ctx1.Reset();
    s.dev1.Reset();
    s.isolate.Reset();
    s.isolateReady = false;
    s.immediate.Reset();
    s.d11.Reset();
    s.latency = nullptr;
    s.presenter.Reset();
    if (s.d3d12Module) {
        FreeLibrary(s.d3d12Module);
        s.d3d12Module = nullptr;
    }
    if (s.dxgiModule) {
        FreeLibrary(s.dxgiModule);
        s.dxgiModule = nullptr;
    }
    // The loader stays mapped for the process. FreeLibrary is not called on it.
}

bool openOwnDevice(Session& s, IDXGIAdapter* adapter) {
    s.d3d12Module = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!s.d3d12Module) {
        failProbe("d3d12.dll could not be loaded");
        return false;
    }
    CreateD3D12DeviceFn createD3D12 = nullptr;
    if (!loadProc(s.d3d12Module, "D3D12CreateDevice", createD3D12)) {
        failProbe("D3D12CreateDevice export unavailable");
        return false;
    }
    const HRESULT hr = createD3D12(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(s.d12.GetAddressOf()));
    if (FAILED(hr)) {
        char text[96];
        std::snprintf(text, sizeof(text), "D3D12CreateDevice failed 0x%08X", unsigned(hr));
        failProbe(text);
        return false;
    }
    DXGI_ADAPTER_DESC desc{};
    adapter->GetDesc(&desc);
    const LUID luid = s.d12->GetAdapterLuid();
    if (luid.HighPart != desc.AdapterLuid.HighPart || luid.LowPart != desc.AdapterLuid.LowPart) {
        failProbe("D3D11/D3D12 adapter LUID mismatch");
        return false;
    }
    s.vendor = desc.VendorId;
    D3D12_COMMAND_QUEUE_DESC queue{};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(s.d12->CreateCommandQueue(&queue, IID_PPV_ARGS(s.queue.GetAddressOf())))) {
        failProbe("D3D12 queue failed");
        return false;
    }
    if (FAILED(s.queue->GetTimestampFrequency(&s.timestampHz))) s.timestampHz = 0;
    if (FAILED(s.d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(s.fence12.GetAddressOf())))) {
        failProbe("D3D12 shared fence failed");
        return false;
    }
    if (FAILED(s.d12->CreateSharedHandle(s.fence12.Get(), nullptr, GENERIC_ALL, nullptr, &s.fenceHandle))) {
        failProbe("D3D12 fence NT handle failed");
        return false;
    }
    if (FAILED(s.d11v5->OpenSharedFence(s.fenceHandle, IID_PPV_ARGS(s.fence11.GetAddressOf())))) {
        failProbe("D3D11 shared fence open failed");
        return false;
    }
    return true;
}

bool loadApi(Session& s, const wchar_t* loaderPath) {
    s.loaderModule = LoadLibraryExW(loaderPath, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!s.loaderModule) {
        char text[160];
        std::snprintf(text, sizeof(text), "amd_fidelityfx_loader_dx12.dll failed to load (Win32=%lu)", GetLastError());
        failProbe(text);
        return false;
    }
    wchar_t loaded[MAX_PATH]{};
    const DWORD loadedLength = GetModuleFileNameW(s.loaderModule, loaded, MAX_PATH);
    if (!loadedLength || loadedLength >= MAX_PATH || _wcsicmp(loaded, loaderPath) != 0) {
        failProbe("amd_fidelityfx_loader_dx12.dll resolved to another module");
        return false;
    }
    if (!loadProc(s.loaderModule, "ffxCreateContext", s.api.create) ||
        !loadProc(s.loaderModule, "ffxDestroyContext", s.api.destroy) ||
        !loadProc(s.loaderModule, "ffxConfigure", s.api.configure) ||
        !loadProc(s.loaderModule, "ffxQuery", s.api.query) ||
        !loadProc(s.loaderModule, "ffxDispatch", s.api.dispatch)) {
        failProbe("amd_fidelityfx_loader_dx12.dll lacks a required ffx-api export");
        return false;
    }
    return true;
}

bool queryVersion(Session& s) {
    ffxQueryDescGetVersions query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    query.device = s.d12.Get();
    uint64_t count = 0;
    query.outputCount = &count;
    const ffxReturnCode_t counted = s.api.query(nullptr, &query.header);
    if (counted != FFX_API_RETURN_OK) {
        char text[160];
        std::snprintf(text, sizeof(text), "frame-generation query returned %u (%s)", unsigned(counted), ffxReturnName(counted));
        failNoVersion(text);
        return false;
    }
    if (count == 0) {
        failNoVersion(nullptr);
        return false;
    }
    if (count > 32) count = 32;
    uint64_t ids[32]{};
    const char* names[32]{};
    uint64_t capacity = count;
    query.outputCount = &capacity;
    query.versionIds = ids;
    query.versionNames = names;
    const ffxReturnCode_t filled = s.api.query(nullptr, &query.header);
    if (filled != FFX_API_RETURN_OK || capacity == 0) {
        failNoVersion(nullptr);
        return false;
    }
    s.versionId = ids[0];
    const char* name = names[0];
    if (name && name[0]) std::snprintf(g_boot.offered, sizeof(g_boot.offered), "%s", name);
    else std::snprintf(g_boot.offered, sizeof(g_boot.offered), "id 0x%llX", static_cast<unsigned long long>(ids[0]));
    Log::get().note("fg bridge: frame-generation query offered %s", g_boot.offered);
    return true;
}

bool chainCreate(Session& s, FgDescs& d, uint32_t width, uint32_t height, uint32_t maxW, uint32_t maxH,
                 DXGI_FORMAT presenter, DXGI_FORMAT colour) {
    d = {};
    const FrameGenColourContract contract = frameGenColourContract(static_cast<uint32_t>(colour),
                                                                    static_cast<uint32_t>(presenter));
    s.colourContract = contract;
    d.create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    d.create.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
    if (contract.highDynamicRange)
        d.create.flags |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
    d.create.displaySize = FfxApiDimensions2D{width, height};
    d.create.maxRenderSize = FfxApiDimensions2D{maxW, maxH};
    // The presentation source is the copied colour when that colour is the HUD-less radiance.
    // The formats match, so the different-format HUD-less create descriptor stays unlinked.
    const DXGI_FORMAT backFormat = contract.backBufferIsColour ? colour : presenter;
    d.create.backBufferFormat = ffxApiGetSurfaceFormatDX12(backFormat);
    d.backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    d.backend.device = s.d12.Get();
    d.version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    d.version.version = FFX_FRAMEGENERATION_VERSION;
    d.overrideVersion.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    d.overrideVersion.versionId = s.versionId;
    d.create.header.pNext = &d.backend.header;
    d.backend.header.pNext = &d.version.header;
    d.version.header.pNext = &d.overrideVersion.header;
    ffxContext created = nullptr;
    const ffxReturnCode_t rc = s.api.create(&created, &d.create.header, nullptr);
    if (rc != FFX_API_RETURN_OK || !created) {
        if (created) s.api.destroy(&created, nullptr);
        return false;
    }
    s.context = created;
    return true;
}

bool configureFg(Session& s, uint64_t frameId, uint32_t width, uint32_t height, bool reset) {
    ffxConfigureDescFrameGeneration desc{};
    desc.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    desc.swapChain = nullptr;
    desc.frameGenerationEnabled = true;
    desc.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    desc.generationRect = FfxApiRect2D{0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
    desc.frameID = frameId;
    desc.allowAsyncWorkloads = false;
    desc.onlyPresentGenerated = false;
    if (s.colourContract.bindHudlessColor && s.colour.d12)
        desc.HUDLessColor = ffxApiGetResourceDX12(s.colour.d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ, 0);
    (void)reset;
    return s.api.configure(&s.context, &desc.header) == FFX_API_RETURN_OK;
}

struct DispatchIn {
    uint32_t renderW = 0, renderH = 0, outW = 0, outH = 0;
    float jitterX = 0, jitterY = 0, deltaMs = 0;
    float camNear = 0, camFar = 0, fovY = 0;
    float pos[3]{};
    float up[3]{};
    float right[3]{};
    float forward[3]{};
    bool reset = false;
    uint64_t frameId = 1;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12QueryHeap* heap = nullptr;
    ID3D12Resource* readback = nullptr;
    bool time = false;
};

bool recordDispatch(Session& s, const DispatchIn& in) {
    ID3D12Resource* resources[4] = {s.colour.d12.Get(), s.depth.d12.Get(), s.motion.d12.Get(), s.output.d12.Get()};
    const D3D12_RESOURCE_STATES from[4] = {D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
                                           D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON};
    const D3D12_RESOURCE_STATES to[4] = {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    transition(in.list, resources, from, to, 4);
    if (in.time && in.heap) in.list->EndQuery(in.heap, D3D12_QUERY_TYPE_TIMESTAMP, 0);

    ffxDispatchDescFrameGenerationPrepareV2 prepare{};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prepare.frameID = in.frameId;
    prepare.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    prepare.commandList = in.list;
    prepare.renderSize = FfxApiDimensions2D{in.renderW, in.renderH};
    prepare.jitterOffset = FfxApiFloatCoords2D{in.jitterX, in.jitterY};
    prepare.motionVectorScale = FfxApiFloatCoords2D{1.0f, 1.0f};
    prepare.frameTimeDelta = fsr4FrameTimeMs(in.deltaMs);
    prepare.reset = in.reset;
    prepare.cameraNear = in.camNear;
    prepare.cameraFar = in.camFar;
    prepare.cameraFovAngleVertical = in.fovY;
    prepare.viewSpaceToMetersFactor = 1.0f;
    prepare.depth = ffxApiGetResourceDX12(s.depth.d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ, 0);
    prepare.motionVectors = ffxApiGetResourceDX12(s.motion.d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ, 0);
    std::memcpy(prepare.cameraPosition, in.pos, sizeof(in.pos));
    std::memcpy(prepare.cameraUp, in.up, sizeof(in.up));
    std::memcpy(prepare.cameraRight, in.right, sizeof(in.right));
    std::memcpy(prepare.cameraForward, in.forward, sizeof(in.forward));
    const ffxReturnCode_t prepared = s.api.dispatch(&s.context, &prepare.header);
    if (prepared != FFX_API_RETURN_OK) {
        const D3D12_RESOURCE_STATES back[4] = {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        const D3D12_RESOURCE_STATES common[4] = {D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
                                                  D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON};
        transition(in.list, resources, back, common, 4);
        char text[160];
        std::snprintf(text, sizeof(text), "prepare returned %u (%s)", unsigned(prepared), ffxReturnName(prepared));
        setProvider(text);
        return false;
    }

    const FrameGenColourContract& contract = s.colourContract;
    ffxDispatchDescFrameGeneration generate{};
    generate.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    generate.commandList = in.list;
    generate.presentColor = ffxApiGetResourceDX12(s.colour.d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ, 0);
    generate.outputs[0] = ffxApiGetResourceDX12(s.output.d12.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS,
                                                FFX_API_RESOURCE_USAGE_UAV);
    generate.numGeneratedFrames = 1;
    generate.reset = in.reset;
    generate.backbufferTransferFunction = static_cast<uint32_t>(contract.transfer);
    generate.minMaxLuminance[0] = contract.minLuminance;
    generate.minMaxLuminance[1] = contract.maxLuminance;
    generate.generationRect = FfxApiRect2D{0, 0, static_cast<int32_t>(in.outW), static_cast<int32_t>(in.outH)};
    generate.frameID = in.frameId;
    const ffxReturnCode_t generated = s.api.dispatch(&s.context, &generate.header);
    if (in.time && in.heap) {
        in.list->EndQuery(in.heap, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        if (in.readback)
            in.list->ResolveQueryData(in.heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, in.readback, 0);
    }
    const D3D12_RESOURCE_STATES back[4] = {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    const D3D12_RESOURCE_STATES common[4] = {D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
                                              D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON};
    transition(in.list, resources, back, common, 4);
    if (generated != FFX_API_RETURN_OK) {
        char text[160];
        std::snprintf(text, sizeof(text), "dispatch returned %u (%s)", unsigned(generated), ffxReturnName(generated));
        setProvider(text);
        return false;
    }
    return true;
}

bool waitFence(Session& s, uint64_t value, DWORD timeoutMs) {
    if (!s.fence12 || !s.cpuEvent || !value) return false;
    if (s.fence12->GetCompletedValue() >= value) return true;
    if (FAILED(s.fence12->SetEventOnCompletion(value, s.cpuEvent))) return false;
    return WaitForSingleObject(s.cpuEvent, timeoutMs) == WAIT_OBJECT_0;
}

bool clearProbe(Session& s) {
    if (!takeKey(s.colour) || !takeKey(s.depth) || !takeKey(s.motion) || !takeKey(s.output)) return false;
    IsolateScope isolate(s);
    ComPtr<ID3D11RenderTargetView> colourView, depthView, motionView;
    s.d11->CreateRenderTargetView(s.colour.d11.Get(), nullptr, colourView.GetAddressOf());
    s.d11->CreateRenderTargetView(s.depth.d11.Get(), nullptr, depthView.GetAddressOf());
    s.d11->CreateRenderTargetView(s.motion.d11.Get(), nullptr, motionView.GetAddressOf());
    const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    const float farDepth[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    const float zero[4] = {};
    if (colourView) s.immediate->ClearRenderTargetView(colourView.Get(), grey);
    if (depthView) s.immediate->ClearRenderTargetView(depthView.Get(), farDepth);
    if (motionView) s.immediate->ClearRenderTargetView(motionView.Get(), zero);
    keepKey(s.colour);
    keepKey(s.depth);
    keepKey(s.motion);
    keepKey(s.output);
    const uint64_t producer = takeFence(s);
    if (!producer || FAILED(s.ctx4->Signal(s.fence11.Get(), producer))) return false;
    s.immediate->Flush();
    if (FAILED(s.queue->Wait(s.fence12.Get(), producer))) return false;
    return true;
}

bool runProbe(Session& s) {
    wchar_t directory[MAX_PATH]{};
    if (!moduleDirectory(directory, MAX_PATH)) {
        failProbe("EDVR module path could not be read");
        return false;
    }
    wchar_t frameDll[MAX_PATH]{};
    wchar_t loader[MAX_PATH]{};
    if (!fileBeside(directory, L"amd_fidelityfx_framegeneration_dx12.dll", frameDll, MAX_PATH)) {
        failMissingDll();
        return false;
    }
    if (!fileBeside(directory, L"amd_fidelityfx_loader_dx12.dll", loader, MAX_PATH)) {
        failProbe("amd_fidelityfx_loader_dx12.dll is missing beside EDVR");
        return false;
    }
    Fsr4DeviceShare share{};
    if (fsr4ShareDevice(s.d11.Get(), &share) && share.borrowed && share.device && share.queue && share.fence12 && share.fence11) {
        // fsr4ShareDevice already AddRef'd. Attach takes that reference; assignment would AddRef again.
        s.d12.Attach(share.device);
        s.queue.Attach(share.queue);
        s.fence12.Attach(share.fence12);
        s.fence11.Attach(share.fence11);
        s.borrowed = true;
        if (FAILED(s.queue->GetTimestampFrequency(&s.timestampHz))) s.timestampHz = 0;
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(s.d11.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(adapter.GetAddressOf())) && SUCCEEDED(adapter->GetDesc(&desc)))
            s.vendor = desc.VendorId;
    } else {
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(s.d11.As(&dxgi)) || FAILED(dxgi->GetAdapter(adapter.GetAddressOf()))) {
            failProbe("D3D11 adapter query failed");
            return false;
        }
        if (!openOwnDevice(s, adapter.Get())) return false;
    }
    s.cpuEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s.cpuEvent) {
        failProbe("GPU fence CPU event failed");
        return false;
    }
    if (!loadApi(s, loader) || !queryVersion(s)) return false;
    if (!ensureIsolate(s)) {
        failProbe("context state could not be isolated");
        return false;
    }
    if (!makeShared(s, s.colour, "probe-colour", kProbeSize, kProbeSize, DXGI_FORMAT_R8G8B8A8_UNORM, false) ||
        !makeShared(s, s.depth, "probe-depth", kProbeSize, kProbeSize, DXGI_FORMAT_R32_FLOAT, false) ||
        !makeShared(s, s.motion, "probe-motion", kProbeSize, kProbeSize, DXGI_FORMAT_R16G16_FLOAT, false) ||
        !makeShared(s, s.output, "probe-output", kProbeSize, kProbeSize, DXGI_FORMAT_R8G8B8A8_UNORM, true)) {
        failProbe("probe textures could not be shared");
        return false;
    }
    FgDescs descs;
    if (!chainCreate(s, descs, kProbeSize, kProbeSize, kProbeSize, kProbeSize, DXGI_FORMAT_R8G8B8A8_UNORM,
                     DXGI_FORMAT_R8G8B8A8_UNORM)) {
        failRejected();
        return false;
    }
    // The create descriptors are on this stack. Destroy the context before any return so they are still alive.
    auto dropProbeContext = [&]() {
        if (s.context && s.api.destroy) s.api.destroy(&s.context, nullptr);
        s.context = nullptr;
    };
    float camNear = 0, camFar = 0;
    fsr4CameraPlanes(kFsr4DefaultNearZ, kFsr4DefaultFarZ, true, &camNear, &camFar, nullptr);
    if (!clearProbe(s)) {
        dropProbeContext();
        failProbe("probe dispatch failed");
        return false;
    }
    CommandSlot slot;
    if (FAILED(s.d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(slot.allocator.GetAddressOf()))) ||
        FAILED(s.d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                        IID_PPV_ARGS(slot.list.GetAddressOf())))) {
        dropProbeContext();
        failProbe("probe command list failed");
        return false;
    }
    makeTiming(s, slot);
    if (!configureFg(s, 1, kProbeSize, kProbeSize, true)) {
        slot.list->Close();
        dropProbeContext();
        failProbe("probe configure failed");
        return false;
    }
    DispatchIn in;
    in.renderW = in.renderH = in.outW = in.outH = kProbeSize;
    in.camNear = camNear;
    in.camFar = camFar;
    in.fovY = kFsr4DefaultFovYRadians;
    in.right[0] = 1.0f;
    in.up[1] = 1.0f;
    in.forward[2] = 1.0f;
    in.reset = true;
    in.frameId = 1;
    in.deltaMs = fsr4FrameTimeMs(0);
    in.list = slot.list.Get();
    in.heap = slot.heap.Get();
    in.readback = slot.readback.Get();
    in.time = slot.heap && slot.readback;
    const bool recorded = recordDispatch(s, in);
    slot.list->Close();
    if (!recorded) {
        dropProbeContext();
        failProbe(g_boot.providerText[0] ? g_boot.providerText : "probe dispatch failed");
        return false;
    }
    ID3D12CommandList* lists[] = {slot.list.Get()};
    s.queue->ExecuteCommandLists(1, lists);
    const uint64_t done = takeFence(s);
    if (!done || FAILED(s.queue->Signal(s.fence12.Get(), done)) || !waitFence(s, done, kProbeWaitMs)) {
        dropProbeContext();
        failProbe("probe dispatch failed");
        return false;
    }
    if (in.time && slot.ticks && s.timestampHz && slot.ticks[1] > slot.ticks[0]) {
        noteGpuSample(s, static_cast<double>(slot.ticks[1] - slot.ticks[0]) * 1000.0 / static_cast<double>(s.timestampHz));
        s.haveTimingSeed = true;
    }
    dropProbeContext();
    s.colour.reset();
    s.depth.reset();
    s.motion.reset();
    s.output.reset();
    slot.reset();
    Log::get().note("frame generation: probe dispatch discarded, camera basis identity");
    return true;
}

DXGI_FORMAT presenterFormatFor(UINT format) {
    switch (static_cast<DXGI_FORMAT>(format)) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

bool productionFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM: return true;
    default: return false;
    }
}

static_assert(kFrameGenR11G11B10 == DXGI_FORMAT_R11G11B10_FLOAT, "scene hdr format");
static_assert(kFrameGenR8G8B8A8Unorm == DXGI_FORMAT_R8G8B8A8_UNORM, "8-bit presenter format");
static_assert(kFrameGenB8G8R8A8Unorm == DXGI_FORMAT_B8G8R8A8_UNORM, "bgra presenter format");
static_assert(static_cast<uint32_t>(FrameGenTransfer::Srgb) == FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB,
              "srgb transfer");
static_assert(static_cast<uint32_t>(FrameGenTransfer::Pq) == FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ, "pq transfer");
static_assert(static_cast<uint32_t>(FrameGenTransfer::ScRgb) == FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB,
              "scrgb transfer");

void logReady(Session& s) {
    if (g_boot.ready) return;
    perfMonitorNoteSwapChain(s.presenter.Get());
    const float hz = perfMonitorRefreshHz();
    g_boot.ready = true;
    g_boot.readyKey = readKey();
    if (g_boot.readyKey != FrameGenKey::On && g_boot.readyKey != FrameGenKey::Auto) g_boot.readyKey = g_boot.key;
    Log::get().note("frame generation: ready: key %s, provider %s, vendor 0x%04X, output %ux%u, refresh %.0f, ui layer %s, temporal_aa %s",
                    keyName(g_boot.readyKey), g_boot.offered[0] ? g_boot.offered : "unknown", s.vendor, s.displayW, s.displayH,
                    static_cast<double>(hz), flatUiLayerOn() ? "on" : "off", g_boot.temporalMode);
}

bool makeRing(Session& s) {
    for (CommandSlot& slot : s.ring) {
        slot.reset();
        if (FAILED(s.d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(slot.allocator.GetAddressOf()))))
            return false;
        if (FAILED(s.d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                            IID_PPV_ARGS(slot.list.GetAddressOf()))))
            return false;
        slot.list->Close();
        makeTiming(s, slot);
    }
    return true;
}

void queryMemory(Session& s, DXGI_FORMAT presenter, DXGI_FORMAT colour) {
    const FrameGenColourContract& contract = s.colourContract;
    ffxQueryDescFrameGenerationGetGPUMemoryUsageV2 query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE_V2;
    query.device = s.d12.Get();
    query.maxRenderSize = FfxApiDimensions2D{s.maxRenderW, s.maxRenderH};
    query.displaySize = FfxApiDimensions2D{s.displayW, s.displayH};
    query.createFlags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
    if (contract.highDynamicRange)
        query.createFlags |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
    query.dispatchFlags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    const DXGI_FORMAT described = contract.backBufferIsColour ? colour : presenter;
    query.backBufferFormat = ffxApiGetSurfaceFormatDX12(described);
    query.hudlessBackBufferFormat = contract.bindHudlessColor ? ffxApiGetSurfaceFormatDX12(colour)
                                                              : static_cast<uint32_t>(FFX_API_SURFACE_FORMAT_UNKNOWN);
    FfxApiEffectMemoryUsage usage{};
    query.gpuMemoryUsageFrameGeneration = &usage;
    if (s.api.query(&s.context, &query.header) == FFX_API_RETURN_OK)
        Log::get().note("fg bridge: frame-generation gpu memory %llu bytes (aliasable %llu)",
                        static_cast<unsigned long long>(usage.totalUsageInBytes),
                        static_cast<unsigned long long>(usage.aliasableUsageInBytes));
}

bool ensureProduction(Session& s, uint32_t renderW, uint32_t renderH, DXGI_FORMAT colour, DXGI_FORMAT depth,
                      DXGI_FORMAT motion) {
    if (g_boot.productionFailed || g_boot.hdrNoLuma || g_boot.formatUnsupported || g_boot.probeFailed) return false;
    if (!productionFormat(s.presenterFormat)) return false;
    if (ffxApiGetSurfaceFormatDX12(colour) == FFX_API_SURFACE_FORMAT_UNKNOWN ||
        ffxApiGetSurfaceFormatDX12(depth) == FFX_API_SURFACE_FORMAT_UNKNOWN ||
        ffxApiGetSurfaceFormatDX12(motion) == FFX_API_SURFACE_FORMAT_UNKNOWN)
        return false;
    const uint32_t maxW = renderW > s.displayW ? renderW : s.displayW;
    const uint32_t maxH = renderH > s.displayH ? renderH : s.displayH;
    D3D11_TEXTURE2D_DESC depthHave{};
    if (s.depth.d11) s.depth.d11->GetDesc(&depthHave);
    const bool sizeOk = s.context && renderW == depthHave.Width && renderH == depthHave.Height &&
                        renderW <= s.maxRenderW && renderH <= s.maxRenderH && s.displayW && s.displayH &&
                        s.colourFormat == colour;
    if (sizeOk && s.colour.d11 && s.output.d11) return true;
    if (s.context) {
        const uint64_t completed = s.fence12 ? s.fence12->GetCompletedValue() : 0;
        uint64_t pending = 0;
        for (const CommandSlot& slot : s.ring)
            if (slot.doneValue > pending) pending = slot.doneValue;
        if (pending && completed < pending && !waitFence(s, pending, kPresentWaitMs)) return false;
        releaseContext(s);
    }
    s.maxRenderW = maxW;
    s.maxRenderH = maxH;
    s.colourFormat = colour;
    if (!s.ring[0].allocator && !makeRing(s)) {
        g_boot.productionFailed = true;
        failProbe("production command ring failed");
        return false;
    }
    if (!makeShared(s, s.colour, "colour", s.displayW, s.displayH, colour, false) ||
        !makeShared(s, s.depth, "depth", renderW, renderH, depth, false) ||
        !makeShared(s, s.motion, "motion", renderW, renderH, motion, false) ||
        !makeShared(s, s.output, "output", s.displayW, s.displayH, s.presenterFormat, true)) {
        g_boot.productionFailed = true;
        failProbe("production textures could not be shared");
        return false;
    }
    D3D11_TEXTURE2D_DESC blend{};
    blend.Width = s.displayW;
    blend.Height = s.displayH;
    blend.MipLevels = 1;
    blend.ArraySize = 1;
    blend.Format = s.presenterFormat;
    blend.SampleDesc.Count = 1;
    blend.Usage = D3D11_USAGE_DEFAULT;
    blend.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    s.d11->CreateTexture2D(&blend, nullptr, s.blendTarget.ReleaseAndGetAddressOf());
    s.descs = new FgDescs;
    if (!chainCreate(s, *s.descs, s.displayW, s.displayH, maxW, maxH, s.presenterFormat, colour)) {
        delete s.descs;
        s.descs = nullptr;
        g_boot.productionFailed = true;
        char text[192];
        std::snprintf(text, sizeof(text), "production context create failed");
        failProbe(text);
        return false;
    }
    queryMemory(s, s.presenterFormat, colour);
    logReady(s);
    return true;
}

void clientSize(HWND window, UINT* width, UINT* height) {
    RECT rect{};
    if (window && GetClientRect(window, &rect)) {
        if (width && !*width) *width = static_cast<UINT>(rect.right - rect.left);
        if (height && !*height) *height = static_cast<UINT>(rect.bottom - rect.top);
    }
    if (width && !*width) *width = 8;
    if (height && !*height) *height = 8;
}

HWND makeHiddenWindow() {
    static ATOM atom = 0;
    HINSTANCE instance = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&frameGenStartup), &instance);
    if (!atom) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = instance;
        wc.lpszClassName = L"EDVR.FrameGen.Hidden";
        atom = RegisterClassExW(&wc);
        if (!atom) return nullptr;
    }
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, MAKEINTATOM(atom), L"EDVR Frame Generation", WS_POPUP,
                           0, 0, 8, 8, nullptr, nullptr, instance, nullptr);
}

bool makePresenter(Session& s, UINT width, UINT height, DXGI_FORMAT format) {
    s.dxgiModule = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    CreateFactory2Fn createFactory = nullptr;
    if (!s.dxgiModule || !loadProc(s.dxgiModule, "CreateDXGIFactory2", createFactory) ||
        FAILED(createFactory(0, IID_PPV_ARGS(s.factory.GetAddressOf()))))
        return false;
    ComPtr<IDXGIFactory5> factory5;
    BOOL allow = FALSE;
    if (SUCCEEDED(s.factory.As(&factory5)) &&
        SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
        s.tearing = allow != FALSE;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width ? width : 8;
    desc.Height = height ? height : 8;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (s.tearing) desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    s.presenterFlags = desc.Flags;
    ComPtr<IDXGISwapChain1> created;
    const HRESULT hr = s.factory->CreateSwapChainForHwnd(s.d11.Get(), s.real, &desc, nullptr, nullptr, created.GetAddressOf());
    if (FAILED(hr) || FAILED(created.As(&s.presenter))) return false;
    s.presenter->SetMaximumFrameLatency(2);
    s.latency = s.presenter->GetFrameLatencyWaitableObject();
    s.displayW = desc.Width;
    s.displayH = desc.Height;
    s.presenterFormat = format;
    return s.latency != nullptr;
}

void destroySession() {
    if (!g_session) return;
    Session* session = g_session;
    g_session = nullptr;
    releaseGpu(*session);
    if (session->hidden) DestroyWindow(session->hidden);
    delete session;
}

void noteSkip(Session& s, const char* reason) {
    s.skipped++;
    const uint64_t now = GetTickCount64();
    Session::Skip* slot = nullptr;
    for (Session::Skip& skip : s.skips) {
        if (skip.reason[0] && std::strcmp(skip.reason, reason) == 0) {
            slot = &skip;
            break;
        }
        if (!slot && !skip.reason[0]) slot = &skip;
    }
    if (!slot) slot = &s.skips[0];
    if (!slot->reason[0]) std::snprintf(slot->reason, sizeof(slot->reason), "%s", reason);
    slot->count++;
    if (slot->lastMs && now - slot->lastMs < 1000) return;
    slot->lastMs = now;
    Log::get().note("frame generation: skipped, %s", reason);
}

void noteReset(Session& s) {
    s.resets++;
    s.forceReset = true;
    static uint64_t lastMs = 0;
    const uint64_t now = GetTickCount64();
    if (lastMs && now - lastMs < 1000) return;
    lastMs = now;
    Log::get().note("frame generation: reset, no generated frame");
}

void cadence(Session& s, double periodMs) {
    if (!s.context) return;
    const uint64_t now = GetTickCount64();
    if (s.lastCadenceMs && now - s.lastCadenceMs < 1000) return;
    s.lastCadenceMs = now;
    double samples[64];
    const unsigned count = s.gpuCount;
    for (unsigned i = 0; i < count; ++i) samples[i] = s.gpuSamples[i];
    for (unsigned i = 1; i < count; ++i) {
        const double value = samples[i];
        unsigned j = i;
        while (j > 0 && samples[j - 1] > value) {
            samples[j] = samples[j - 1];
            --j;
        }
        samples[j] = value;
    }
    const double p50 = count ? samples[count / 2] : 0;
    const double p95 = count ? samples[(count * 95) / 100] : 0;
    char reasons[240] = "";
    for (const Session::Skip& skip : s.skips) {
        if (!skip.count) continue;
        char piece[64];
        std::snprintf(piece, sizeof(piece), "%s%s %llu", reasons[0] ? ", " : "", skip.reason,
                      static_cast<unsigned long long>(skip.count));
        const size_t used = std::strlen(reasons);
        const size_t pieceLen = std::strlen(piece);
        if (used + pieceLen + 1 < sizeof(reasons)) std::memcpy(reasons + used, piece, pieceLen + 1);
    }
    Log::get().note("frame generation: applied %llu, skipped %llu (%s), reset %llu, gpu p50 %.1f p95 %.1f, presented %llu, rendered %llu, period %.2f",
                    static_cast<unsigned long long>(g_boot.applied), static_cast<unsigned long long>(s.skipped), reasons,
                    static_cast<unsigned long long>(s.resets), p50, p95, static_cast<unsigned long long>(s.presented),
                    static_cast<unsigned long long>(s.rendered), periodMs);
}

UINT presentFlags(Session& s, UINT syncInterval, UINT flags) {
    if (flags & DXGI_PRESENT_DO_NOT_SEQUENCE) {
        flags &= ~static_cast<UINT>(DXGI_PRESENT_DO_NOT_SEQUENCE);
        if (!s.flagsNoted) {
            s.flagsNoted = true;
            Log::get().note("fg bridge: Elite's Present asked for DXGI_PRESENT_DO_NOT_SEQUENCE, which a flip-model swap chain refuses; dropped");
        }
    }
    if (syncInterval == 0 && s.tearing && !s.exclusive) flags |= DXGI_PRESENT_ALLOW_TEARING;
    return flags;
}

bool waitLatency(Session& s) {
    if (!s.latency) return false;
    return WaitForSingleObject(s.latency, kPresentWaitMs) == WAIT_OBJECT_0;
}

bool copyHiddenToPresenter(Session& s, IDXGISwapChain* hidden) {
    ComPtr<ID3D11Texture2D> source, dest;
    if (FAILED(hidden->GetBuffer(0, IID_PPV_ARGS(source.GetAddressOf())))) return false;
    if (FAILED(s.presenter->GetBuffer(0, IID_PPV_ARGS(dest.GetAddressOf())))) return false;
    s.immediate->CopyResource(dest.Get(), source.Get());
    return true;
}

HRESULT presentReal(Session& s, IDXGISwapChain* hidden, UINT syncInterval, UINT flags) {
    copyHiddenToPresenter(s, hidden);
    const HRESULT hr = s.presenter->Present(syncInterval, presentFlags(s, syncInterval, flags));
    if (SUCCEEDED(hr)) s.presented++;
    return hr;
}

void stopSession(const char* why) {
    if (g_boot.stopped) return;
    g_boot.stopped = true;
    Log::get().note("frame generation: stopped for this session: %s", why ? why : "dispatch failed");
}

bool planesFor(const float proj[2], float* camNear, float* camFar) {
    const float A = proj[0];
    const float B = proj[1];
    const bool infinite = std::fabs(A) < 1e-4f && B > 0.0f;
    if (fsr4CameraPlanes(B, 0.0f, infinite, camNear, camFar, nullptr)) return true;
    return fsr4CameraPlanes(kFsr4DefaultNearZ, kFsr4DefaultFarZ, true, camNear, camFar, nullptr);
}

void storeBasis(Pending& pending, const float rows[12]) {
    pending.right[0] = rows[0];
    pending.right[1] = rows[4];
    pending.right[2] = rows[8];
    pending.up[0] = rows[1];
    pending.up[1] = rows[5];
    pending.up[2] = rows[9];
    pending.forward[0] = rows[2];
    pending.forward[1] = rows[6];
    pending.forward[2] = rows[10];
    pending.pos[0] = rows[3];
    pending.pos[1] = rows[7];
    pending.pos[2] = rows[11];
    pending.haveBasis = true;
}

bool signalProducer(Session& s, uint64_t* producer) {
    const uint64_t value = takeFence(s);
    if (!value || FAILED(s.ctx4->Signal(s.fence11.Get(), value))) return false;
    s.immediate->Flush();
    *producer = value;
    return true;
}

CommandSlot* claimSlot(Session& s) {
    if (!s.fence12) return nullptr;
    const uint64_t completed = s.fence12->GetCompletedValue();
    if (completed == UINT64_MAX) {
        stopSession("device removed");
        return nullptr;
    }
    for (CommandSlot& slot : s.ring) {
        readSlotTiming(s, slot, completed);
        if (slot.doneValue == 0 || completed >= slot.doneValue) return &slot;
    }
    return nullptr;
}

bool submitGenerated(Session& s, CommandSlot& slot, const Pending& pending) {
    if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator.Get(), nullptr))) return false;
    if (!configureFg(s, s.frameId, pending.outW ? pending.outW : s.displayW, pending.outH ? pending.outH : s.displayH,
                     s.forceReset || pending.reset)) {
        slot.list->Close();
        stopSession(g_boot.providerText[0] ? g_boot.providerText : "configure failed");
        return false;
    }
    DispatchIn in;
    in.renderW = pending.renderW;
    in.renderH = pending.renderH;
    in.outW = s.displayW;
    in.outH = s.displayH;
    in.jitterX = pending.jitterX;
    in.jitterY = pending.jitterY;
    in.deltaMs = pending.deltaMs;
    in.camNear = pending.camNear;
    in.camFar = pending.camFar;
    in.fovY = pending.fovY;
    std::memcpy(in.pos, pending.pos, sizeof(in.pos));
    std::memcpy(in.up, pending.up, sizeof(in.up));
    std::memcpy(in.right, pending.right, sizeof(in.right));
    std::memcpy(in.forward, pending.forward, sizeof(in.forward));
    in.reset = s.forceReset || pending.reset;
    in.frameId = s.frameId;
    in.list = slot.list.Get();
    in.heap = slot.heap.Get();
    in.readback = slot.readback.Get();
    in.time = slot.heap && slot.readback;
    const bool recorded = recordDispatch(s, in);
    slot.list->Close();
    if (!recorded) {
        stopSession(g_boot.providerText[0] ? g_boot.providerText : "dispatch failed");
        return false;
    }
    if (pending.signaled && FAILED(s.queue->Wait(s.fence12.Get(), pending.producer))) {
        stopSession("producer wait failed");
        return false;
    }
    ID3D12CommandList* lists[] = {slot.list.Get()};
    s.queue->ExecuteCommandLists(1, lists);
    const uint64_t done = takeFence(s);
    if (!done || FAILED(s.queue->Signal(s.fence12.Get(), done))) {
        stopSession("submit signal failed");
        return false;
    }
    slot.doneValue = done;
    slot.timingPending = in.time;
    if (FAILED(s.ctx4->Wait(s.fence11.Get(), done))) {
        stopSession("presentation wait failed");
        return false;
    }
    s.frameId++;
    if (!s.frameId) s.frameId = 1;
    s.forceReset = false;
    return true;
}

bool blendGenerated(Session& s, const Pending& pending, ID3D11Texture2D** shown) {
    *shown = nullptr;
    if (!s.output.d11 || !s.blendTarget) return false;
    const bool outputFresh = !s.output.held;
    if (!takeKey(s.output)) return false;
    IsolateScope isolate(s);
    if (!isolate.on) {
        if (outputFresh && s.output.key) s.output.key->ReleaseSync(0);
        return false;
    }
    const bool exact = sameSize(s.output.d11.Get(), s.displayW, s.displayH);
    copyBox(s.immediate.Get(), s.blendTarget.Get(), s.output.d11.Get(), s.displayW, s.displayH, exact);
    keepKey(s.output);
    if (!flatUiLayerOn()) {
        if (!s.uiOffNoted) {
            s.uiOffNoted = true;
            Log::get().note("frame generation: ui layer off, HUD is in the interpolated image");
        }
        s.blendTarget->AddRef();
        *shown = s.blendTarget.Get();
        return true;
    }
    const uint32_t region[4] = {0, 0, s.displayW, s.displayH};
    const float uv[4] = {0, 0, 1, 1};
    *shown = uiLayerBlendGenerated(pending.frame, s.blendTarget.Get(), region, uv);
    return *shown != nullptr;
}

void logAutoHold(Session& s, const FrameGenFacts& facts) {
    if (facts.key != FrameGenKey::Auto || g_boot.stopped) return;
    if (!facts.refreshRead) {
        if (!s.refreshNoted) {
            s.refreshNoted = true;
            Log::get().note("frame generation: display refresh not read");
        }
        return;
    }
    const bool times = facts.realGpuMs >= 0.0 && facts.dispatchGpuMs >= 0.0 && facts.periodMs > 0.0;
    if (times && !(facts.renderedMs > facts.periodMs)) {
        if (!s.heldOffNoted) {
            s.heldOffNoted = true;
            Log::get().note("frame generation: held off, game is on time");
        }
        return;
    }
    if (times && facts.renderedMs > facts.periodMs &&
        (facts.realGpuMs + facts.dispatchGpuMs) > (2.0 * facts.periodMs - 1.0)) {
        if (s.wasEngaged) {
            Log::get().note("frame generation: disengaged, over budget (real %.1f ms, dispatch %.1f ms, period %.1f ms)",
                            facts.realGpuMs, facts.dispatchGpuMs, facts.periodMs);
        }
        s.wasEngaged = false;
    }
}

FrameGenFacts makeFacts(Session& s, const Pending* pending, double renderedMs) {
    FrameGenFacts facts;
    facts.key = readKey();
    facts.vr = runtimeVrProfile();
    facts.temporalAaOff = _stricmp(g_boot.temporalMode, "off") == 0;
    facts.xessOwnsSwapChain = g_boot.xessOwns;
    facts.hdrWithoutLuminance = g_boot.hdrNoLuma;
    facts.probeFailed = g_boot.probeFailed;
    facts.probeOk = !g_boot.probeFailed && s.bridged;
    facts.providerText = g_boot.providerText[0] ? g_boot.providerText : nullptr;
    facts.offeredVersion = g_boot.offered[0] ? g_boot.offered : nullptr;
    perfMonitorNoteSwapChain(s.presenter.Get());
    const float hz = perfMonitorRefreshHz();
    facts.refreshRead = hz > 1.0f;
    facts.periodMs = facts.refreshRead ? 1000.0 / static_cast<double>(hz) : 0.0;
    facts.renderedMs = renderedMs;
    double realGpu = -1;
    if (!flatRuntimeLastGpuFrameMs(&realGpu)) realGpu = -1;
    facts.realGpuMs = realGpu;
    facts.dispatchGpuMs = s.haveTimingSeed || s.dispatchGpuMs >= 0.0 ? s.dispatchGpuMs : -1.0;
    facts.haveScene = pending && pending->valid;
    facts.haveInputs = pending && pending->haveInputs;
    facts.haveBasis = pending && pending->haveBasis;
    facts.exclusiveFullscreen = s.exclusive;
    facts.reset = pending && pending->reset;
    return facts;
}

double renderedSinceReturn(const Session& s) {
    if (!s.lastReturnQpc) return 0;
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (!freq.QuadPart) return 0;
    return static_cast<double>(now.QuadPart - s.lastReturnQpc) * 1000.0 / static_cast<double>(freq.QuadPart);
}

void markReturn(Session& s) {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    s.lastReturnQpc = now.QuadPart;
}

#endif  // EDVR_HAVE_FSR4

}  // namespace

void frameGenStartup() {
    if (startupRefusal()) return;
#if !EDVR_HAVE_FSR4
    g_boot.blocked = true;
    Log::get().note("frame generation: not started: frame-generation provider is not in this build");
    return;
#else
    std::atexit(onProcessExit);
#endif
}

void frameGenShutdown() { logTotals(); }

void frameGenOnDeviceLost() {
#if EDVR_HAVE_FSR4
    if (!g_session) return;
    releaseGpu(*g_session);
    g_session->bridged = false;
    g_session->presenter.Reset();
#endif
}

void frameGenNoteXessStandDown() {
    if (g_boot.key == FrameGenKey::Off || g_boot.xessOwns) return;
    g_boot.key = readKey();
    if (g_boot.key != FrameGenKey::On && g_boot.key != FrameGenKey::Auto) return;
    g_boot.xessOwns = true;
    FrameGenFacts facts;
    facts.key = g_boot.key;
    facts.xessOwnsSwapChain = true;
    logRefusal(facts);
}

HWND frameGenBeginCreate(IUnknown* device, HWND realWindow, UINT* width, UINT* height, UINT format, bool fullscreen) {
#if !EDVR_HAVE_FSR4
    (void)device;
    (void)realWindow;
    (void)width;
    (void)height;
    (void)format;
    (void)fullscreen;
    return nullptr;
#else
    (void)format;
    (void)fullscreen;
    if (g_boot.blocked || g_boot.probeFailed || g_boot.productionFailed || g_boot.xessOwns) return nullptr;
    const FrameGenKey key = readKey();
    if (key != FrameGenKey::On && key != FrameGenKey::Auto) return nullptr;
    if (runtimeVrProfile()) return nullptr;
    if (!device || !realWindow) return nullptr;
    if (g_session) destroySession();
    Session* session = new Session;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(session->d11.GetAddressOf())))) {
        delete session;
        return nullptr;
    }
    session->d11->GetImmediateContext(session->immediate.GetAddressOf());
    session->d11.As(&session->d11v5);
    session->d11.As(&session->dev1);
    if (session->immediate) session->immediate.As(&session->ctx1);
    if (session->immediate) session->immediate.As(&session->ctx4);
    if (!session->immediate || !session->d11v5 || !session->ctx4) {
        delete session;
        return nullptr;
    }
    clientSize(realWindow, width, height);
    session->real = realWindow;
    session->hidden = makeHiddenWindow();
    if (!session->hidden) {
        delete session;
        return nullptr;
    }
    session->displayW = width ? *width : 8;
    session->displayH = height ? *height : 8;
    g_session = session;
    return session->hidden;
#endif
}

bool frameGenFinishCreate(IDXGISwapChain* hidden, HWND realWindow) {
#if !EDVR_HAVE_FSR4
    (void)hidden;
    (void)realWindow;
    return false;
#else
    Session* session = g_session;
    if (!session || !hidden || realWindow != session->real) return false;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(hidden->GetDesc(&desc))) return false;
    session->hiddenChain = hidden;
    session->displayW = desc.BufferDesc.Width ? desc.BufferDesc.Width : session->displayW;
    session->displayH = desc.BufferDesc.Height ? desc.BufferDesc.Height : session->displayH;
    const DXGI_FORMAT format = presenterFormatFor(static_cast<UINT>(desc.BufferDesc.Format));
    if (!runProbe(*session)) {
        destroySession();
        return false;
    }
    if (format == DXGI_FORMAT_UNKNOWN) {
        g_boot.formatUnsupported = true;
        Log::get().note("frame generation: not started: presenting chain format is not supported");
        destroySession();
        return false;
    }
    if (!makePresenter(*session, session->displayW, session->displayH, format)) {
        destroySession();
        return false;
    }
    session->bridged = true;
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        g_boot.hdrNoLuma = true;
        FrameGenFacts facts;
        facts.key = readKey();
        facts.hdrWithoutLuminance = true;
        logRefusal(facts);
        return true;
    }
    if (!productionFormat(format)) {
        g_boot.formatUnsupported = true;
        Log::get().note("frame generation: not started: presenting chain format is not supported");
        return true;
    }
    return true;
#endif
}

void frameGenAbandon() {
#if EDVR_HAVE_FSR4
    destroySession();
#endif
}

bool frameGenBridged() {
#if !EDVR_HAVE_FSR4
    return false;
#else
    return g_session && g_session->bridged && g_session->presenter;
#endif
}

bool frameGenOwns(const void* swapChain) {
#if !EDVR_HAVE_FSR4
    (void)swapChain;
    return false;
#else
    return frameGenBridged() && g_session->hiddenChain && g_session->hiddenChain == swapChain;
#endif
}

void frameGenBeforeResize(UINT* width, UINT* height) {
#if EDVR_HAVE_FSR4
    if (g_session) clientSize(g_session->real, width, height);
#else
    (void)width;
    (void)height;
#endif
}

HRESULT frameGenAfterResize(HRESULT hiddenResult) {
#if !EDVR_HAVE_FSR4
    return hiddenResult;
#else
    Session* session = g_session;
    if (!session || !session->presenter || FAILED(hiddenResult)) return hiddenResult;
    if (!session->hiddenChain) return hiddenResult;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(session->hiddenChain->GetDesc(&desc))) return hiddenResult;
    const UINT width = desc.BufferDesc.Width;
    const UINT height = desc.BufferDesc.Height;
    if (!width || !height) return hiddenResult;
    if (width == session->displayW && height == session->displayH) return hiddenResult;
    const HRESULT hr = session->presenter->ResizeBuffers(0, width, height, session->presenterFormat, session->presenterFlags);
    if (SUCCEEDED(hr)) {
        session->displayW = width;
        session->displayH = height;
        releaseContext(*session);
    } else {
        Log::get().note("fg bridge: presenter resize failed 0x%08X", unsigned(hr));
    }
    return hiddenResult;
#endif
}

HWND frameGenWindow() {
#if !EDVR_HAVE_FSR4
    return nullptr;
#else
    return g_session ? g_session->real : nullptr;
#endif
}

HRESULT frameGenSetFullscreenState(BOOL fullscreen, IDXGIOutput* target) {
#if !EDVR_HAVE_FSR4
    (void)fullscreen;
    (void)target;
    return E_FAIL;
#else
    Session* session = g_session;
    if (!session || !session->presenter) return DXGI_ERROR_INVALID_CALL;
    const HRESULT hr = session->presenter->SetFullscreenState(fullscreen, target);
    if (SUCCEEDED(hr)) session->exclusive = fullscreen != FALSE;
    Log::get().note("fg bridge: exclusive fullscreen %s (0x%08X)", fullscreen ? "on" : "off", unsigned(hr));
    return hr;
#endif
}

HRESULT frameGenGetFullscreenState(BOOL* fullscreen, IDXGIOutput** target) {
#if !EDVR_HAVE_FSR4
    (void)fullscreen;
    (void)target;
    return E_FAIL;
#else
    return g_session && g_session->presenter ? g_session->presenter->GetFullscreenState(fullscreen, target)
                                             : DXGI_ERROR_INVALID_CALL;
#endif
}

HRESULT frameGenResizeTarget(const DXGI_MODE_DESC* mode) {
#if !EDVR_HAVE_FSR4
    (void)mode;
    return E_FAIL;
#else
    return g_session && g_session->presenter ? g_session->presenter->ResizeTarget(mode) : DXGI_ERROR_INVALID_CALL;
#endif
}

HRESULT frameGenGetContainingOutput(IDXGIOutput** output) {
#if !EDVR_HAVE_FSR4
    (void)output;
    return E_FAIL;
#else
    return g_session && g_session->presenter ? g_session->presenter->GetContainingOutput(output) : DXGI_ERROR_INVALID_CALL;
#endif
}

HRESULT frameGenGetFrameStatistics(DXGI_FRAME_STATISTICS* stats) {
#if !EDVR_HAVE_FSR4
    (void)stats;
    return E_FAIL;
#else
    return g_session && g_session->presenter ? g_session->presenter->GetFrameStatistics(stats) : DXGI_ERROR_INVALID_CALL;
#endif
}

HRESULT frameGenGetLastPresentCount(UINT* count) {
#if !EDVR_HAVE_FSR4
    (void)count;
    return E_FAIL;
#else
    return g_session && g_session->presenter ? g_session->presenter->GetLastPresentCount(count) : DXGI_ERROR_INVALID_CALL;
#endif
}

void frameGenRelease() {
#if EDVR_HAVE_FSR4
    if (!g_session) return;
    Log::get().note("fg bridge: releasing the window for a new swap chain");
    destroySession();
#endif
}

HRESULT frameGenPresent(IDXGISwapChain* hidden, UINT syncInterval, UINT flags) {
#if !EDVR_HAVE_FSR4
    (void)hidden;
    (void)syncInterval;
    (void)flags;
    return DXGI_ERROR_INVALID_CALL;
#else
    Session* session = g_session;
    if (!session || hidden != session->hiddenChain || !session->presenter) return DXGI_ERROR_INVALID_CALL;
    if (flags & DXGI_PRESENT_TEST)
        return session->presenter->Present(syncInterval, presentFlags(*session, syncInterval, flags));
    const double renderedMs = renderedSinceReturn(*session);
    session->rendered++;
    Pending pending = session->pending;
    session->pending.valid = false;
    FrameGenFacts facts = makeFacts(*session, &pending, renderedMs);
    const double periodMs = facts.periodMs;
    cadence(*session, periodMs);
    HRESULT hr = S_OK;
    const FrameGenKey key = facts.key;
    const bool want = key == FrameGenKey::On || key == FrameGenKey::Auto;
    if (!want || g_boot.stopped) {
        hr = presentReal(*session, hidden, syncInterval, flags);
        markReturn(*session);
        return hr;
    }
    if (frameGenHardRefusal(facts, nullptr, 0)) {
        logRefusal(facts);
        hr = presentReal(*session, hidden, syncInterval, flags);
        markReturn(*session);
        return hr;
    }
    if (!session->context) {
        hr = presentReal(*session, hidden, syncInterval, flags);
        markReturn(*session);
        return hr;
    }
    if (pending.reset) noteReset(*session);
    else if (!pending.valid) noteSkip(*session, "no scene");
    else if (!pending.haveInputs) noteSkip(*session, pending.inputWhy);
    else if (session->exclusive) noteSkip(*session, "exclusive fullscreen");
    else if (!pending.haveBasis) noteSkip(*session, "no world camera basis");
    else if (key == FrameGenKey::Auto && !frameGenAutoEngaged(facts)) logAutoHold(*session, facts);
    const bool may = frameGenPresentAllowsGenerated(facts, pending.hdr) && session->context && !g_boot.stopped;
    if (may && frameGenAutoEngaged(facts)) {
        if (!g_boot.engaged) {
            g_boot.engaged = true;
            Log::get().note("frame generation: engaged");
        }
        session->wasEngaged = true;
    }
    if (!may) {
        hr = presentReal(*session, hidden, syncInterval, flags);
        markReturn(*session);
        return hr;
    }
    CommandSlot* slot = claimSlot(*session);
    if (!slot) {
        if (!g_boot.stopped) noteSkip(*session, "command ring full");
        session->forceReset = true;
        hr = presentReal(*session, hidden, syncInterval, flags);
        markReturn(*session);
        return hr;
    }
    const bool firstWait = waitLatency(*session);
    if (!firstWait) {
        noteSkip(*session, "present wait did not run");
        session->forceReset = true;
        hr = presentReal(*session, hidden, syncInterval, flags);
        markReturn(*session);
        return hr;
    }
    bool generated = false;
    if (submitGenerated(*session, *slot, pending)) {
        ID3D11Texture2D* shown = nullptr;
        if (blendGenerated(*session, pending, &shown) && shown) {
            ComPtr<ID3D11Texture2D> held;
            held.Attach(shown);
            ComPtr<ID3D11Texture2D> dest;
            if (SUCCEEDED(session->presenter->GetBuffer(0, IID_PPV_ARGS(dest.GetAddressOf())))) {
                session->immediate->CopyResource(dest.Get(), held.Get());
                const HRESULT phr = session->presenter->Present(syncInterval, presentFlags(*session, syncInterval, flags));
                generated = SUCCEEDED(phr);
                if (generated) session->presented++;
            }
        } else {
            noteSkip(*session, "context state could not be isolated");
            session->forceReset = true;
        }
    }
    const bool secondWait = waitLatency(*session);
    hr = presentReal(*session, hidden, syncInterval, flags);
    if (frameGenPairCountsApplied(generated, firstWait && secondWait)) g_boot.applied++;
    markReturn(*session);
    return hr;
#endif
}

void frameGenNoteResolved(ID3D11DeviceContext* ctx, ID3D11Texture2D* colour, ID3D11Texture2D* depth,
                          ID3D11Texture2D* motion, uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH,
                          float deltaMs, float jitterX, float jitterY, float nearPlane, bool hdr, bool reset,
                          uint64_t frame) {
#if !EDVR_HAVE_FSR4
    (void)ctx;
    (void)colour;
    (void)depth;
    (void)motion;
    (void)renderW;
    (void)renderH;
    (void)outW;
    (void)outH;
    (void)deltaMs;
    (void)jitterX;
    (void)jitterY;
    (void)nearPlane;
    (void)hdr;
    (void)reset;
    (void)frame;
#else
    (void)nearPlane;
    (void)outW;
    (void)outH;
    Session* session = g_session;
    if (!session || !session->bridged || g_boot.probeFailed || g_boot.hdrNoLuma || g_boot.formatUnsupported || g_boot.stopped)
        return;
    Pending& pending = session->pending;
    pending = {};
    pending.valid = true;
    pending.frame = frame;
    pending.hdr = hdr;
    pending.reset = reset;
    pending.deltaMs = deltaMs;
    pending.jitterX = jitterX;
    pending.jitterY = jitterY;
    pending.renderW = renderW;
    pending.renderH = renderH;
    pending.outW = session->displayW;
    pending.outH = session->displayH;
    float rows[12]{};
    float proj[2]{};
    if (temporalPassLatestAcceptedRows(rows, proj)) {
        storeBasis(pending, rows);
        planesFor(proj, &pending.camNear, &pending.camFar);
        pending.fovY = kFsr4DefaultFovYRadians;
    }
    // hdr is the internal flat HDR route. On an 8-bit window its HUD-less image is copied.
    // Only a presenting chain with no luminance range refuses the copy.
    const FrameGenResolveAction action = frameGenResolveAction(
        hdr, g_boot.hdrNoLuma, colour != nullptr, depth != nullptr, motion != nullptr, renderW != 0 && renderH != 0);
    if (action != FrameGenResolveAction::CopyInputs) {
        if (action == FrameGenResolveAction::SkipMissingInputs)
            std::snprintf(pending.inputWhy, sizeof(pending.inputWhy), "no dlOut depth or motion");
        return;
    }
    D3D11_TEXTURE2D_DESC colourDesc{}, depthDesc{}, motionDesc{};
    colour->GetDesc(&colourDesc);
    depth->GetDesc(&depthDesc);
    motion->GetDesc(&motionDesc);
    if (!ensureProduction(*session, renderW, renderH, colourDesc.Format, depthDesc.Format, motionDesc.Format)) {
        std::snprintf(pending.inputWhy, sizeof(pending.inputWhy), "input copy failed");
        return;
    }
    const bool colourFresh = !session->colour.held;
    const bool depthFresh = !session->depth.held;
    const bool motionFresh = !session->motion.held;
    auto releaseFresh = [&]() {
        if (colourFresh && session->colour.key && !session->colour.held) session->colour.key->ReleaseSync(0);
        if (depthFresh && session->depth.key && !session->depth.held) session->depth.key->ReleaseSync(0);
        if (motionFresh && session->motion.key && !session->motion.held) session->motion.key->ReleaseSync(0);
    };
    if (!takeKey(session->colour) || !takeKey(session->depth) || !takeKey(session->motion)) {
        releaseFresh();
        std::snprintf(pending.inputWhy, sizeof(pending.inputWhy), "input copy failed");
        return;
    }
    ID3D11DeviceContext* copyCtx = ctx ? ctx : session->immediate.Get();
    {
        IsolateScope isolate(*session);
        if (!isolate.on) {
            releaseFresh();
            std::snprintf(pending.inputWhy, sizeof(pending.inputWhy), "context state could not be isolated");
            return;
        }
        copyBox(copyCtx, session->colour.d11.Get(), colour, session->displayW < colourDesc.Width ? session->displayW : colourDesc.Width,
                session->displayH < colourDesc.Height ? session->displayH : colourDesc.Height,
                colourDesc.Width == session->displayW && colourDesc.Height == session->displayH &&
                    colourDesc.Format == session->colourFormat);
        copyBox(copyCtx, session->depth.d11.Get(), depth, renderW, renderH,
                depthDesc.Width == renderW && depthDesc.Height == renderH);
        copyBox(copyCtx, session->motion.d11.Get(), motion, renderW, renderH,
                motionDesc.Width == renderW && motionDesc.Height == renderH);
    }
    keepKey(session->colour);
    keepKey(session->depth);
    keepKey(session->motion);
    if (!signalProducer(*session, &pending.producer)) {
        std::snprintf(pending.inputWhy, sizeof(pending.inputWhy), "input copy failed");
        return;
    }
    pending.signaled = true;
    pending.haveInputs = true;
#endif
}

}  // namespace edvr

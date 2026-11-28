// The frame-generation synthesizer: see framegen_synth.h.
#include "temporal_shader_bytecode.h"
#include "framegen_synth.h"
#include "framegen_synth_pass.h"
#include "cs_stage_save.h"
#include "gpu_frame_timing.h"
#include "gpu_timing.h"
#include "shader_swap.h"
#include "../common/framegen_replay_math.h"
#include "../common/framegen_synth_report.h"
#include "../common/log.h"

#include <windows.h>
#include <d3d11.h>

#include <dxgi1_2.h>

#include <atomic>
#include <mutex>
#include <cstdio>
#include <string>
#include <thread>
#include <cmath>
#include <cstring>
#include <vector>

namespace edvr {
namespace {

constexpr int kStagingDepth = 4;
constexpr int kTimerRing = 8;
constexpr int kEntries = kFrameGenSynthEntries;   // splat, source, resolve, holes, resample, luma, score (framegen_synth_pass.h)

std::atomic<bool> g_on{false};
std::atomic<bool> g_told{false};   // the armed line has been said since the key last went on

struct Tex {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
};

// One eye's resources and the f = 1 S waiting to be scored.
struct EyeRes {
    bool created = false;
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0, qw = 0, qh = 0;
    DXGI_FORMAT sFormat = DXGI_FORMAT_UNKNOWN;
    Tex s;                        // output size, the upscaler's format: S
    Tex sLuma, bInfo;             // quarter grid: the kept S luma, B's luma (x) and class (y)
    ID3D11Buffer* stats = nullptr;
    ID3D11UnorderedAccessView* statsUav = nullptr;
    ID3D11Buffer* staging[kStagingDepth] = {};
    bool pending[kStagingDepth] = {};
    uint8_t slotKind[kStagingDepth] = {};      // 0 the f = 0.5 S, 1 the f = 1 S
    bool slotScored[kStagingDepth] = {};
    uint32_t writes = 0;
    uint32_t calls = 0;           // built frames so far: the even ones build f = 0.5
    bool havePending = false;     // an f = 1 S luma is kept, built on frame bFrame
    uint32_t bFrame = 0;
    float keptRot[9] = {};        // the head rotation the kept f = 1 S was built for (B's eye space to S's)
    float keptTan[4] = {};        // the tangents it was built with
    bool shownValid = false;      // the latest call built the f = 0.5 S into `s`
    uint32_t shownFrame = 0;
    // Phase 2.2, S's transfer: the producer texture the runtime opens by its NT handle (keyed mutex: 0 the game device's, 1 the runtime's), made while the runtime consumes S.
    // Path 1 (uav): the resample pass writes it and `s` is not made; path 2 (copy): it refused UAV binding, so S is built in `s` and copied in. Made again (a new serial)
    // when the eye's size or format changes, or when kBusyRemake builds in a row found it still unconsumed (the runtime let go of it).
    Tex shared;
    IDXGIKeyedMutex* mutex = nullptr;
    HANDLE handle = nullptr;
    uint32_t sharedPath = 0;
    uint64_t serial = 0;
    uint32_t busyRun = 0;
    // Phase 2.3b: the f = 0.5 S waiting for B's door (frameGenSynthFinish). finHeld: the producer texture's key 0 is held from the build to the door (uav path); finInShared: S
    // was written in it (the door sharpens it from there), else in `s`; finConsumer: hand it over once finished. The step and tangents S was built with move the UI layer;
    // finDump is the eye dump's frame. fin: the finished S when neither the producer texture nor `s` can take it (no consumer and no sharpening). finFrameSrv views the door's
    // sharpened S (finFrameRes, identity only).
    bool finPending = false, finHeld = false, finInShared = false, finConsumer = false;
    float finRot[9] = {}, finT[3] = {}, finTan[4] = {};
    int finDump = -1;
    EdvrNativeFrameGenS finAnswer = {};
    Tex fin;
    void* finFrameRes = nullptr;
    ID3D11ShaderResourceView* finFrameSrv = nullptr;
};
constexpr uint32_t kBusyRemake = 8;

// The render-size scratch of one call (the key grid, the read offsets and the hole list): rebuilt from nothing by every call, so both eyes share one set (they run one after the
// other on the same context). 8.5 bytes per render pixel in all; recreated when a call's render size differs from the last's.
struct Scratch {
    uint32_t rw = 0, rh = 0;
    Tex key, read;                              // R32_UINT both: the keys; the read offsets as two packed half floats
    ID3D11Buffer* holes = nullptr;              // frameGenSynthHoleCapacity uints
    ID3D11UnorderedAccessView* holesUav = nullptr;
};

struct TimerSlot {
    GpuTimer timer;
    bool inUse = false;
    int kind = 0;
};

struct State {
    ID3D11Device* dev = nullptr;   // identity only
    bool failed = false;           // creation failed on this device: stand down until it changes or the key is toggled
    ID3D11ComputeShader* cs[kEntries] = {};
    ID3D11ComputeShader* csFinish = nullptr;   // Phase 2.3b: fgSynFinish, outside the pass sequence
    TimerSlot* finTimer[2] = {};               // the finish's timer per eye, from frameGenSynthFinishSource to frameGenSynthFinish
    ID3D11Buffer* cb = nullptr;
    ID3D11SamplerState* linear = nullptr;      // the resample's paired taps (framegen_synth_pass.h frameGenSynthCreateSampler)
    DXGI_FORMAT lumaFormat = DXGI_FORMAT_R16_FLOAT;
    EyeRes eye[2];
    Scratch scratch;
    TimerSlot timers[kTimerRing];
    FrameGenSynthWindow win;
    std::vector<float> gpuMs[3];   // [0] the f = 0.5 build, [1] the f = 1 build, [2] the finish (Phase 2.3b)
    std::vector<float> rotRes;     // the residual rotation's angle of each scored frame in the window, degrees
    int64_t windowTicks = 0;
    // Phase 2.2: the runtime's answer per eye (frameGenSynthTakeS), the serial the next producer texture takes, whether any slot reached the module since the key went on (and
    // whether its absence was said), and whether the shared texture was refused outright on this device (S then stays private, said once).
    EdvrNativeFrameGenS answer[2] = {};
    uint64_t nextSerial = 1;
    bool slotSeen = false, slotAbsenceSaid = false, sharedRefused = false, remakeSaid = false;
    uint32_t pathSaid = 0;
    bool sampled[2] = {};   // Phase 2.4: the eye was called (built) in the open window (frameGenSynthSampleDue)
};
std::mutex g_answerMu;   // the answers: written by the render thread inside a treat, taken by the runtime's owner after it
// Never destroyed: the process teardown can run under the loader lock, where releasing D3D objects is not safe (see temporalPassShutdown).
State* const g_statePtr = new State();
State& g = *g_statePtr;

template <class T>
void releaseCom(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

void releaseTex(Tex& t) {
    releaseCom(t.uav);
    releaseCom(t.srv);
    releaseCom(t.tex);
}

// The producer texture and its handle. Nothing waits: a handoff the runtime has not consumed is simply let go (its opened copy stays valid on its side).
void releaseShared(EyeRes& r) {
    releaseCom(r.mutex);
    releaseTex(r.shared);
    if (r.handle) { CloseHandle(r.handle); r.handle = nullptr; }
    r.sharedPath = 0;
    r.busyRun = 0;
}

void releaseEye(EyeRes& r) {
    releaseShared(r);
    releaseTex(r.fin);
    releaseCom(r.finFrameSrv);
    releaseTex(r.s);
    releaseTex(r.sLuma);
    releaseTex(r.bInfo);
    releaseCom(r.statsUav);
    releaseCom(r.stats);
    for (auto& s : r.staging) releaseCom(s);
    r = EyeRes{};
}

void releaseScratch() {
    releaseTex(g.scratch.key);
    releaseTex(g.scratch.read);
    releaseCom(g.scratch.holesUav);
    releaseCom(g.scratch.holes);
    g.scratch = Scratch{};
}

bool anyResources() {
    if (g.cb || g.scratch.key.tex) return true;
    for (const EyeRes& r : g.eye) if (r.created) return true;
    for (ID3D11ComputeShader* c : g.cs) if (c) return true;
    return g.csFinish != nullptr;
}

void windowReset() {
    g.win = FrameGenSynthWindow{};
    for (auto& v : g.gpuMs) v.clear();
    g.rotRes.clear();
    g.sampled[0] = g.sampled[1] = false;
}

// The residual rotation's median and maximum over the scored frames of the window.
void fillRotation(FrameGenSynthWindow& w) {
    w.rotSamples = static_cast<uint32_t>(g.rotRes.size());
    w.rotMax = 0.0f;
    for (float v : g.rotRes) if (v > w.rotMax) w.rotMax = v;
    w.rotMedian = frameGenMedian(g.rotRes.data(), g.rotRes.size());
}

// ---- Replay capture (.agents/framegen-replay-format.md) ----
// The eye dump's left-eye run asks for the synthesizer's inputs of its first six frames. Each is copied (CopyResource, GPU side, no wait) into three staging textures; nothing is
// mapped on the frame it was copied. When the dump writes its files frameGenReplayFinish hands the set to the pump, which maps one copy per turn with DO_NOT_WAIT (a copy still in
// flight is tried again next frame) and lets a worker thread hash and write it straight from the mapping; the render thread only maps, unmaps and joins.
#ifndef EDVR_VERSION_STRING
#define EDVR_VERSION_STRING "unversioned test build"
#endif

constexpr uint32_t kReplayFrames = kFrameGenReplayFrames;

struct ReplaySet {
    ID3D11Texture2D* tex[kReplayFrames][3] = {};    // out, depth, mv
    uint32_t texFormat[kReplayFrames][3] = {};
    FrameGenReplayFrame info[kReplayFrames];
    uint32_t count = 0;
    bool stopped = false;
    std::string stop;                               // why the run of consecutive frames ended early
    bool open = false;                              // frame 0 was seen and the set is being filled
    bool writing = false;                           // frameGenReplayFinish handed it over
    std::wstring dir, stamp;
    std::vector<FrameGenReplayFile> files;
    uint32_t job = 0;                               // next copy to write: frame job / 3, texture job % 3
    bool mapped = false;
    D3D11_MAPPED_SUBRESOURCE map{};
    ID3D11DeviceContext* mapCtx = nullptr;          // holds a reference while mapped
    std::thread worker;
    std::atomic<bool> workerDone{false};
    bool workerRunning = false;
    FrameGenReplayFile result;
    bool resultOk = false;
};
// Never destroyed, like the state: a joinable thread member must not be destroyed at process teardown.
ReplaySet* const g_replayPtr = new ReplaySet();
ReplaySet& rp = *g_replayPtr;

const char* const kReplayPart[3] = {"out", "depth", "mv"};

void replayRelease() {
    if (rp.workerRunning) { rp.worker.join(); rp.workerRunning = false; }
    if (rp.mapped && rp.mapCtx && rp.job / 3 < kReplayFrames) {
        ID3D11Texture2D* t = rp.tex[rp.job / 3][rp.job % 3];
        if (t) rp.mapCtx->Unmap(t, 0);
    }
    rp.mapped = false;
    releaseCom(rp.mapCtx);
    for (auto& row : rp.tex) for (auto& t : row) releaseCom(t);
    rp.count = 0; rp.stopped = false; rp.stop.clear(); rp.open = false; rp.writing = false;
    rp.files.clear(); rp.job = 0; rp.dir.clear(); rp.stamp.clear();
}

void replayStop(const std::string& why) {
    if (rp.writing || !rp.open || rp.stopped) return;
    rp.stopped = true;
    rp.stop = why;
}

std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 0x80 ? static_cast<char>(c) : '?';
    return s;
}

bool replayStageCopy(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srv, ID3D11Texture2D*& out, uint32_t& format) {
    out = nullptr;
    if (!srv) return false;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    if (!res) return false;
    ID3D11Texture2D* src = nullptr;
    const bool isTex = SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src))) && src;
    res->Release();
    if (!isTex) return false;
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    bool ok = false;
    if (d.SampleDesc.Count == 1 && frameGenReplayFormatBytes(d.Format) != 0) {
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0; sd.MipLevels = 1; sd.ArraySize = 1;
        if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &out)) && out) {
            ctx->CopyResource(out, src);
            format = static_cast<uint32_t>(d.Format);
            ok = true;
        }
    }
    src->Release();
    return ok;
}

// Called from frameGenSynthEyeFrame on a frame that built its S. cb is the constant block built for it.
void replayCapture(ID3D11Device* dev, ID3D11DeviceContext* ctx, const FrameGenSynthFrame& f, uint32_t frame, const FrameGenSynthCb& cb) {
    if (!rp.open || rp.stopped || rp.writing || rp.count >= kReplayFrames) return;
    if (f.replayIndex != rp.count) { replayStop("frame " + std::to_string(rp.count) + " of the run was not captured (the synthesizer skipped it or the run broke)"); return; }
    ID3D11ShaderResourceView* const srv[3] = {f.outSrv, f.depthSrv, f.mvSrv};
    const uint32_t k = rp.count;
    for (int i = 0; i < 3; ++i) {
        if (!replayStageCopy(dev, ctx, srv[i], rp.tex[k][i], rp.texFormat[k][i])) {
            for (int j = 0; j < 3; ++j) releaseCom(rp.tex[k][j]);
            replayStop(std::string("frame ") + std::to_string(k) + ": the " + kReplayPart[i] + " texture could not be staged (multisampled, or a format the capture does not know)");
            return;
        }
    }
    FrameGenReplayFrame& r = rp.info[k];
    r = FrameGenReplayFrame{};
    r.index = k;
    r.renderW = f.renderW; r.renderH = f.renderH; r.outW = f.outW; r.outH = f.outH;
    r.outFormat = rp.texFormat[k][0]; r.depthFormat = rp.texFormat[k][1]; r.mvFormat = rp.texFormat[k][2];
    for (int i = 0; i < 4; ++i) r.tanNow[i] = f.tanNow[i];
    if (f.tanPrev) { r.haveTanPrev = true; for (int i = 0; i < 4; ++i) r.tanPrev[i] = f.tanPrev[i]; }
    r.depthA = f.depthA; r.depthB = f.depthB; r.splitMetres = f.splitMetres; r.f = cb.dk[3];
    std::memcpy(r.Hd, f.deltas.Hd, sizeof(r.Hd)); std::memcpy(r.th, f.deltas.th, sizeof(r.th));
    std::memcpy(r.W, f.deltas.W, sizeof(r.W)); std::memcpy(r.tvCam, f.deltas.tv, sizeof(r.tvCam));
    r.worldValid = f.worldValid; r.cameraJump = f.cameraJump;
    r.passFrame = frame;
    r.qpc = qpcNow(); r.qpcFrequency = qpcFrequency();
    ++rp.count;
}

bool replayWriteText(const std::wstring& path, const std::string& text, FrameGenReplayFile& out) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"wb") || !fp) return false;
    Sha256 h;
    h.update(text.data(), text.size());
    bool ok = std::fwrite(text.data(), 1, text.size(), fp) == text.size();
    if (std::fclose(fp) != 0) ok = false;
    out.bytes = text.size();
    out.sha256 = h.hex();
    return ok;
}

void replayWorker(const uint8_t* data, uint32_t rowPitch, uint32_t rows, uint32_t rowBytes, std::wstring path) {
    rp.resultOk = false;
    FILE* fp = nullptr;
    if (!_wfopen_s(&fp, path.c_str(), L"wb") && fp) {
        Sha256 h;
        bool ok = true;
        for (uint32_t y = 0; y < rows && ok; ++y) {
            const uint8_t* row = data + static_cast<size_t>(y) * rowPitch;
            h.update(row, rowBytes);
            ok = std::fwrite(row, 1, rowBytes, fp) == rowBytes;
        }
        if (std::fclose(fp) != 0) ok = false;
        rp.result.bytes = static_cast<uint64_t>(rowBytes) * rows;
        rp.result.sha256 = h.hex();
        rp.resultOk = ok;
    }
    rp.workerDone.store(true, std::memory_order_release);
}

// The per-frame json and the manifest: small, written on the render thread once every bin is out.
void replayFinishSet() {
    for (uint32_t k = 0; k < rp.count; ++k) {
        FrameGenReplayFile fl;
        char name[32];
        std::snprintf(name, sizeof(name), "f%u.json", k);
        fl.name = name;
        if (replayWriteText(rp.dir + L"\\" + std::wstring(name, name + std::strlen(name)), frameGenReplayFrameJson(rp.info[k]), fl)) rp.files.push_back(fl);
        else if (rp.stop.empty()) rp.stop = std::string("could not write ") + name;
    }
    std::string why = rp.stop;
    if (why.empty() && rp.count < kReplayFrames) why = "the run ended after " + std::to_string(rp.count) + " frames";
    FrameGenReplayFile mf;
    if (replayWriteText(rp.dir + L"\\manifest.json", frameGenReplayManifestJson(narrow(rp.stamp), EDVR_VERSION_STRING, rp.count, why, rp.files), mf))
        Log::get().note("frame generation (synth): replay capture %ls: %u frames, %zu files written%s%s.", rp.stamp.c_str(), rp.count, rp.files.size() + 1, why.empty() ? "" : "; stopped: ", why.c_str());
    else
        Log::get().note("frame generation (synth): replay capture %ls: could not write manifest.json.", rp.stamp.c_str());
    replayRelease();
}

// The shaders, the constant buffer and the formats: made once per device. The timers are left as they are (a timer with a query in flight is released only by its owner's
// context, gpu_timing.h); the ring simply keeps polling them.
void releaseAll() {
    replayRelease();
    for (EyeRes& r : g.eye) releaseEye(r);
    releaseScratch();
    for (auto& c : g.cs) releaseCom(c);
    releaseCom(g.csFinish);
    g.finTimer[0] = g.finTimer[1] = nullptr;   // a timer slot in flight is polled on as before (pollTimers); only the eye's claim on it goes
    releaseCom(g.cb);
    releaseCom(g.linear);
    g.dev = nullptr;
    g.failed = false;
    g.slotSeen = g.slotAbsenceSaid = g.sharedRefused = g.remakeSaid = false;
    g.pathSaid = 0;
    std::lock_guard<std::mutex> l(g_answerMu);
    for (EdvrNativeFrameGenS& a : g.answer) a = EdvrNativeFrameGenS{};
}

bool uavStoreOk(ID3D11Device* dev, DXGI_FORMAT fmt) {
    UINT support = 0;
    const UINT want = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    return SUCCEEDED(dev->CheckFormatSupport(fmt, &support)) && (support & want) == want;
}

bool createCommon(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    struct Item { int entry; const unsigned char* code; size_t size; const char* name; };
    const Item entries[kEntries] = {   // in framegen_synth_pass.h's entry order
        {kFgSynSplat, kFrameGenSynthSplatBytecode, sizeof(kFrameGenSynthSplatBytecode), "framegen_synth_splat"},
        {kFgSynSrc, kFrameGenSynthSrcBytecode, sizeof(kFrameGenSynthSrcBytecode), "framegen_synth_src"},
        {kFgSynResolve, kFrameGenSynthResolveBytecode, sizeof(kFrameGenSynthResolveBytecode), "framegen_synth_resolve"},
        {kFgSynHoles, kFrameGenSynthHolesBytecode, sizeof(kFrameGenSynthHolesBytecode), "framegen_synth_holes"},
        {kFgSynResample, kFrameGenSynthResampleBytecode, sizeof(kFrameGenSynthResampleBytecode), "framegen_synth_resample"},
        {kFgSynLuma, kFrameGenSynthLumaBytecode, sizeof(kFrameGenSynthLumaBytecode), "framegen_synth_luma"},
        {kFgSynScore, kFrameGenSynthScoreBytecode, sizeof(kFrameGenSynthScoreBytecode), "framegen_synth_score"},
    };
    for (const Item& e : entries) {
        g.cs[e.entry] = shaderSwapCreateCs(ctx, e.code, e.size, e.name, "frame generation (synth)");
        if (!g.cs[e.entry]) return false;
    }
    g.csFinish = shaderSwapCreateCs(ctx, kFrameGenSynthFinishBytecode, sizeof(kFrameGenSynthFinishBytecode), "framegen_synth_finish", "frame generation (synth)");
    if (!g.csFinish) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(FrameGenSynthCb);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g.cb))) return false;
    if (!frameGenSynthCreateSampler(dev, &g.linear)) return false;
    // The formats the textures are written in: R32_FLOAT and R32_UINT (the keys, the packed read offsets) are in every D3D11 part's typed set, stores and loads; the two-float
    // class map needs the adapter to store it (every D3D11 part that matters does); the luma is R16_FLOAT where the adapter stores it, else R32_FLOAT, as the scorer's.
    if (!uavStoreOk(dev, DXGI_FORMAT_R32G32_FLOAT) || !uavStoreOk(dev, DXGI_FORMAT_R32_UINT) || !uavStoreOk(dev, DXGI_FORMAT_R32_FLOAT)) return false;
    g.lumaFormat = uavStoreOk(dev, DXGI_FORMAT_R16_FLOAT) ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R32_FLOAT;
    return true;
}

bool makeTex(ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT fmt, Tex& out) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &out.tex))) return false;
    if (FAILED(dev->CreateShaderResourceView(out.tex, nullptr, &out.srv))) return false;
    return SUCCEEDED(dev->CreateUnorderedAccessView(out.tex, nullptr, &out.uav));
}

// The format the upscaler's output is read in (the SRV's, else the texture's).
DXGI_FORMAT viewFormat(ID3D11ShaderResourceView* v) {
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    v->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_UNKNOWN) return d.Format;
    ID3D11Resource* res = nullptr;
    v->GetResource(&res);
    if (!res) return DXGI_FORMAT_UNKNOWN;
    ID3D11Texture2D* t = nullptr;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
        D3D11_TEXTURE2D_DESC td{};
        t->GetDesc(&td);
        fmt = td.Format;
        t->Release();
    }
    res->Release();
    return fmt;
}

bool createEye(ID3D11Device* dev, EyeRes& r, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, DXGI_FORMAT sFormat) {
    r = EyeRes{};
    r.rw = rw; r.rh = rh; r.ow = ow; r.oh = oh;
    r.qw = (rw + 3u) / 4u;
    r.qh = (rh + 3u) / 4u;
    r.sFormat = sFormat;
    if (r.qw == 0 || r.qh == 0 || !uavStoreOk(dev, sFormat)) return false;
    // S itself is made by ensureTarget: in the private texture `s`, or (Phase 2.2) in the producer texture the runtime takes it from.
    bool ok = makeTex(dev, r.qw, r.qh, g.lumaFormat, r.sLuma) && makeTex(dev, r.qw, r.qh, DXGI_FORMAT_R32G32_FLOAT, r.bInfo);
    if (ok) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = kFrameGenSynthStatsUints * 4u;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &r.stats));
        if (ok) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_UNKNOWN;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = kFrameGenSynthStatsUints;
            ok = SUCCEEDED(dev->CreateUnorderedAccessView(r.stats, &ud, &r.statsUav));
        }
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (int i = 0; i < kStagingDepth && ok; ++i) ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &r.staging[i]));
    }
    if (!ok) {
        releaseEye(r);
        return false;
    }
    r.created = true;
    return true;
}

bool ensureScratch(ID3D11Device* dev, uint32_t rw, uint32_t rh) {
    Scratch& sc = g.scratch;
    if (sc.key.tex && sc.rw == rw && sc.rh == rh) return true;
    releaseScratch();
    if (makeTex(dev, rw, rh, DXGI_FORMAT_R32_UINT, g.scratch.key) && makeTex(dev, rw, rh, DXGI_FORMAT_R32_UINT, g.scratch.read)) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = frameGenSynthHoleCapacity(rw, rh) * 4u;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = frameGenSynthHoleCapacity(rw, rh);
        if (bd.ByteWidth && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g.scratch.holes)) && SUCCEEDED(dev->CreateUnorderedAccessView(g.scratch.holes, &ud, &g.scratch.holesUav))) {
            g.scratch.rw = rw;
            g.scratch.rh = rh;
            return true;
        }
    }
    releaseScratch();
    return false;
}

// The bytes of what was made, from the descriptors (a texture: width x height x bytes per texel; a buffer: its byte width), for the VRAM line.
uint64_t texBytes(const Tex& t) {
    if (!t.tex) return 0;
    D3D11_TEXTURE2D_DESC d{};
    t.tex->GetDesc(&d);
    return static_cast<uint64_t>(d.Width) * d.Height * frameGenReplayFormatBytes(static_cast<uint32_t>(d.Format));
}

uint64_t bufBytes(ID3D11Buffer* b) {
    if (!b) return 0;
    D3D11_BUFFER_DESC d{};
    b->GetDesc(&d);
    return d.ByteWidth;
}

FrameGenSynthVram vramOf(const EyeRes& r, const Scratch& sc) {
    FrameGenSynthVram v;
    v.s = texBytes(r.s) + texBytes(r.fin);   // Phase 2.3b: the finished S's own texture, when it has one, counts as S
    v.shared = texBytes(r.shared);
    v.luma = texBytes(r.sLuma);
    v.cls = texBytes(r.bInfo);
    v.stats = bufBytes(r.stats);
    for (ID3D11Buffer* s : r.staging) v.stats += bufBytes(s);
    v.key = texBytes(sc.key);
    v.read = texBytes(sc.read);
    v.holes = bufBytes(sc.holes);
    return v;
}

// The staging slots already copied, oldest first, read without waiting: a slot not done yet is left for the next frame.
void pollReadbacks(ID3D11DeviceContext* ctx, EyeRes& r, uint32_t nextSlot) {
    for (int k = 0; k < kStagingDepth; ++k) {
        const int slot = static_cast<int>((nextSlot + static_cast<uint32_t>(k)) % kStagingDepth);   // the slot written longest ago first
        if (!r.pending[slot]) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = ctx->Map(r.staging[slot], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        r.pending[slot] = false;
        if (FAILED(hr) || !m.pData) continue;
        FrameGenSynthEyeResult res;
        frameGenSynthDecodeStats(static_cast<const uint32_t*>(m.pData), res);
        ctx->Unmap(r.staging[slot], 0);
        frameGenSynthMergeFrame(g.win, r.slotKind[slot], static_cast<uint64_t>(r.rw) * r.rh, res);
        if (r.slotScored[slot]) frameGenSynthMergeScore(g.win, res);
    }
}

void pollTimers(ID3D11DeviceContext* ctx) {
    if (!gpuTimingOwns(ctx)) return;
    for (TimerSlot& t : g.timers) {
        if (!t.inUse) continue;
        double ms = 0.0;
        const GpuTimerPoll p = t.timer.poll(ctx, ms);
        if (p == GpuTimerPoll::Pending) continue;
        t.inUse = false;
        if (p == GpuTimerPoll::Ready) g.gpuMs[t.kind].push_back(static_cast<float>(ms));
    }
}

// A timer pair is begun only inside a frequency scope that is already open (the census's, or the application-frame clock's): without one, GpuTimer::begin would open a scope of
// its own and hold a record of the clock's eight for several frames (gpu_census.cpp, frameWorkFirstCommand).
TimerSlot* beginTimer(ID3D11Device* dev, ID3D11DeviceContext* ctx, int kind) {
    if (!(gpuTimingCensusScopeOpen() || gpuFrameClockLive())) return nullptr;
    if (!gpuTimingAccepts(ctx) && !gpuTimingBind(dev, ctx)) return nullptr;
    for (TimerSlot& t : g.timers) {
        if (t.inUse) continue;
        if (t.timer.begin(dev, ctx)) { t.inUse = true; t.kind = kind; return &t; }
        return nullptr;
    }
    return nullptr;
}

// ---- Phase 2.2, S's transfer ----
// The producer texture S crosses in: output size, S's format, a keyed mutex and an NT handle the runtime opens on its own device. Made with UAV binding first (the resample pass
// then writes it, and no private S is needed); a texture that refuses that is made for copies only. False when neither could be made shared on this device.
bool makeShared(ID3D11Device* dev, EyeRes& r) {
    for (uint32_t path = 1; path <= 2; ++path) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = r.ow; td.Height = r.oh; td.MipLevels = 1; td.ArraySize = 1; td.Format = r.sFormat; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (path == 1 ? D3D11_BIND_UNORDERED_ACCESS : 0u);
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        Tex t;
        IDXGIKeyedMutex* km = nullptr;
        IDXGIResource1* res = nullptr;
        HANDLE h = nullptr;
        const bool ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &t.tex)) && t.tex &&
                        SUCCEEDED(t.tex->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&km))) && km &&
                        SUCCEEDED(t.tex->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&res))) && res &&
                        SUCCEEDED(res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h)) && h &&
                        SUCCEEDED(dev->CreateShaderResourceView(t.tex, nullptr, &t.srv)) &&
                        (path != 1 || SUCCEEDED(dev->CreateUnorderedAccessView(t.tex, nullptr, &t.uav)));
        releaseCom(res);
        if (ok) {
            r.shared = t;
            r.mutex = km;
            r.handle = h;
            r.sharedPath = path;
            r.serial = g.nextSerial++;
            r.busyRun = 0;
            return true;
        }
        releaseCom(km);
        releaseTex(t);
        if (h) CloseHandle(h);
    }
    return false;
}

// Where this call's S goes: the producer texture while the runtime consumes S (made here when first needed, let go when it stops), else the private `s`; on the copy path both.
// `changed` says the eye's set was made or let go (its VRAM line is due). False only when the private texture could not be made.
bool ensureTarget(ID3D11Device* dev, EyeRes& r, uint32_t eye, bool consumer, bool& changed, bool needPrivate = false) {
    if (consumer && !r.shared.tex && !g.sharedRefused) {
        if (makeShared(dev, r)) {
            changed = true;
        } else {
            g.sharedRefused = true;
            Log::get().note("frame generation (synth): eye %u: the producer texture S would cross in could not be made shared (format %u, %ux%u, keyed mutex and NT handle); S stays on "
                            "the game device and nothing crosses.", static_cast<unsigned>(eye), static_cast<unsigned>(r.sFormat), static_cast<unsigned>(r.ow), static_cast<unsigned>(r.oh));
        }
    }
    if (!consumer && r.shared.tex) {
        releaseShared(r);
        changed = true;
    }
    const bool wantPrivate = r.sharedPath != 1 || needPrivate;   // needPrivate (Phase 2.3b): S is finished from `s` into the producer texture
    if (wantPrivate && !r.s.tex) {
        if (!makeTex(dev, r.ow, r.oh, r.sFormat, r.s)) return false;
        changed = true;
    }
    if (!wantPrivate && r.s.tex) {
        releaseTex(r.s);
        changed = true;
    }
    if (changed && r.sharedPath && g.pathSaid != r.sharedPath) {
        g.pathSaid = r.sharedPath;
        Log::get().note(r.sharedPath == 1 ? "frame generation (synth): S crosses to the runtime, path uav: the resample pass writes the producer texture (no private S on the game device)."
                                          : "frame generation (synth): S crosses to the runtime, path copy: the producer texture refused UAV binding, so S is built privately and copied in.");
    }
    return true;
}

// A producer texture the runtime has not consumed for kBusyRemake builds in a row is made again (a new serial): the runtime let go of its side, and the key would stay 1.
void noteBusy(EyeRes& r, uint32_t eye) {
    ++g.win.busy;
    if (++r.busyRun < kBusyRemake) return;
    releaseShared(r);
    if (!g.remakeSaid) {
        g.remakeSaid = true;
        Log::get().note("frame generation (synth): eye %u: the runtime has not consumed S for %u builds; the producer texture is made again.", static_cast<unsigned>(eye), kBusyRemake);
    }
}

// The keyed mutex failed in a way that is not a timeout (abandoned, or the device): S stays private for the rest of the arm.
void sharedFault(EyeRes& r, uint32_t eye, HRESULT hr) {
    releaseShared(r);
    g.sharedRefused = true;
    Log::get().note("frame generation (synth): eye %u: the producer texture's keyed mutex failed (0x%08X); S stays on the game device for the rest of the arm.", static_cast<unsigned>(eye),
                    static_cast<unsigned>(hr));
}

// Phase 2.3b: a waiting S whose door did not come before the eye's next build: the producer texture's key goes back to the game device, the finish's timer is closed, and the S is
// counted unfinished (its answer already said no_door, and it was not handed over).
void abandonFinish(ID3D11DeviceContext* ctx, EyeRes& r, uint32_t eye) {
    if (!r.finPending) return;
    r.finPending = false;
    if (r.finHeld && r.mutex) {
        const HRESULT rel = r.mutex->ReleaseSync(0);
        if (rel != S_OK) sharedFault(r, eye, rel);
    }
    r.finHeld = false;
    if (g.finTimer[eye]) {
        g.finTimer[eye]->timer.end(ctx);
        g.finTimer[eye] = nullptr;
    }
    ++g.win.unfinished;
    g.win.unfinishedWhy = kFrameGenFinishNoDoor;
}

// The SRV a finish reads its frame through: the module's own views for S's textures, else a view over the door's sharpened S (made when that texture changes).
ID3D11ShaderResourceView* finishFrameView(EyeRes& r, ID3D11Texture2D* tex) {
    if (tex == r.shared.tex) return r.shared.srv;
    if (tex == r.s.tex) return r.s.srv;
    if (r.finFrameRes != static_cast<void*>(tex) || !r.finFrameSrv) {
        releaseCom(r.finFrameSrv);
        r.finFrameRes = nullptr;
        ID3D11Device* dev = nullptr;
        tex->GetDevice(&dev);
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = r.sFormat;   // the sharpener writes S's own format family; read it in S's view format
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
        const bool ok = dev && (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) && SUCCEEDED(dev->CreateShaderResourceView(tex, &vd, &r.finFrameSrv));
        if (dev) dev->Release();
        if (!ok) {
            releaseCom(r.finFrameSrv);
            return nullptr;
        }
        r.finFrameRes = tex;
    }
    return r.finFrameSrv;
}

}  // namespace

HRESULT frameGenSynthTakeS(uint32_t eye, EdvrNativeFrameGenS* out) {
    if (!out || eye > 1 || out->size < EDVR_NATIVE_FRAMEGEN_S_SIZE_1 || out->version < EDVR_NATIVE_FRAMEGEN_VERSION_1) return E_INVALIDARG;
    EdvrNativeFrameGenS a;
    {
        std::lock_guard<std::mutex> l(g_answerMu);
        a = g.answer[eye];
        g.answer[eye] = EdvrNativeFrameGenS{};
    }
    // Only the bytes both sides know are written: a reader with a size 1 struct (a runtime from before the menu on S) gets size 1's, and one with a longer struct keeps its own
    // past size 2's.
    const uint32_t n = out->size < EDVR_NATIVE_FRAMEGEN_S_SIZE_2 ? EDVR_NATIVE_FRAMEGEN_S_SIZE_1 : EDVR_NATIVE_FRAMEGEN_S_SIZE_2;
    a.size = n;
    a.version = EDVR_NATIVE_FRAMEGEN_VERSION_1;
    a.eye = eye;
    std::memcpy(out, &a, n);
    return a.sequence ? S_OK : S_FALSE;
}

void frameGenSynthConfigure(bool on) {
    g_on.store(on);
    if (!on) g_told.store(false);
}

bool frameGenSynthOn() { return g_on.load(); }

bool frameGenSynthResident() { return anyResources(); }

FrameGenSynthWindow frameGenSynthOpenWindow() {
    FrameGenSynthWindow w = g.win;
    fillRotation(w);
    return w;
}

ID3D11Texture2D* frameGenSynthShown(uint32_t eye, uint32_t frame) {
    if (!g_on.load() || eye > 1) return nullptr;
    const EyeRes& r = g.eye[eye];
    return (r.created && r.shownValid && r.shownFrame == frame) ? r.s.tex : nullptr;   // shownValid: built in the private texture (frameGenSynthEyeFrame)
}

bool frameGenSynthSampleDue(uint32_t eye) { return g_on.load() && eye <= 1 && !g.sampled[eye]; }

void frameGenSynthIdle(ID3D11DeviceContext* ctx, uint32_t eye) {
    if (!g_on.load() || !ctx || eye > 1 || !g.dev || g.failed) return;
    EyeRes& r = g.eye[eye];
    if (!r.created) return;
    r.shownValid = false;
    r.havePending = false;        // no f = 1 S is kept across a frame that built nothing
    abandonFinish(ctx, r, eye);   // a sample's S whose door never came
    pollReadbacks(ctx, r, r.writes);
    pollTimers(ctx);
}

void frameGenSynthEyeFrame(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t eye, uint32_t frame, const FrameGenSynthFrame& f) {
    if (!g_on.load()) return;
    if (!dev || !ctx || eye > 1 || !f.tanNow) return;
    g.sampled[eye] = true;
    if (g.dev && g.dev != dev) releaseAll();   // a new device: everything starts over
    if (g.failed) return;
    if (!g.dev) {
        g.dev = dev;
        if (!createCommon(dev, ctx)) {
            g.failed = true;
            Log::get().note("frame generation (synth): the synthesizer's shaders, constant buffer or texture formats could not be created; it stands down.");
            return;
        }
    }
    EyeRes& r = g.eye[eye];
    r.shownValid = false;
    abandonFinish(ctx, r, eye);   // Phase 2.3b: the last S's door never came
    if (f.slotSequence) g.slotSeen = true;
    // This call's answer to the runtime (frameGenSynthTakeS), stored on every way out below.
    EdvrNativeFrameGenS answer{};
    answer.size = EDVR_NATIVE_FRAMEGEN_S_SIZE_1;
    answer.version = EDVR_NATIVE_FRAMEGEN_VERSION_1;
    answer.sequence = f.slotSequence;
    answer.eye = eye;
    answer.state = EDVR_NATIVE_FRAMEGEN_S_NONE;
    const auto storeAnswer = [&] {
        answer.path = r.sharedPath;
        answer.sharedHandle = r.handle;
        answer.textureSerial = r.shared.tex ? r.serial : 0;
        answer.width = r.ow;
        answer.height = r.oh;
        answer.format = static_cast<uint32_t>(r.sFormat);
        answer.gameBytes = texBytes(r.s) + texBytes(r.shared);
        std::lock_guard<std::mutex> l(g_answerMu);
        g.answer[eye] = answer;
    };
    if (f.replay && f.replayIndex == 0 && !rp.writing) { replayRelease(); rp.open = true; }   // the eye dump's first frame: a new capture (one still being written is not disturbed)
    if (!f.outSrv || !f.depthSrv || !f.mvSrv || !f.depthWritten || f.renderW == 0 || f.renderH == 0 || f.outW == 0 || f.outH == 0) {
        // Nothing to build from: counted, so a window in which the module was called and built nothing says so.
        ++g.win.skipped;
        r.havePending = false;
        if (f.replay) replayStop("frame " + std::to_string(f.replayIndex) + " of the run: the synthesizer had no inputs for it");
        storeAnswer();
        return;
    }
    const DXGI_FORMAT sFormat = viewFormat(f.outSrv);
    bool made = false;   // the eye's textures were made on this call: the VRAM line follows once the scratch exists too
    if (!r.created || r.rw != f.renderW || r.rh != f.renderH || r.ow != f.outW || r.oh != f.outH || r.sFormat != sFormat) {
        made = true;
        releaseEye(r);
        if (!createEye(dev, r, f.renderW, f.renderH, f.outW, f.outH, sFormat)) {
            g.failed = true;
            Log::get().note("frame generation (synth): the eye's textures could not be created (%ux%u render, %ux%u output, format %u); it stands down.",
                            static_cast<unsigned>(f.renderW), static_cast<unsigned>(f.renderH), static_cast<unsigned>(f.outW), static_cast<unsigned>(f.outH),
                            static_cast<unsigned>(sFormat));
            return;
        }
        if (!g_told.exchange(true)) {
            const std::string line = formatFrameGenSynthArmed(f.renderW, f.renderH, f.outW, f.outH, static_cast<uint32_t>(sFormat));
            Log::get().note("%s", line.c_str());
        }
    }
    if (!ensureScratch(dev, f.renderW, f.renderH)) {
        g.failed = true;
        Log::get().note("frame generation (synth): the scratch textures could not be created (%ux%u render); it stands down.", static_cast<unsigned>(f.renderW), static_cast<unsigned>(f.renderH));
        return;
    }
    bool changed = false;
    // Phase 2.3b: S finished at the door without sharpening is written in `s` and finished into the producer texture, so `s` is kept on the uav path too.
    if (!ensureTarget(dev, r, eye, f.consumer, changed, f.finish && !f.finishSharpen)) {
        g.failed = true;
        Log::get().note("frame generation (synth): the S texture could not be created (%ux%u output, format %u); it stands down.", static_cast<unsigned>(f.outW), static_cast<unsigned>(f.outH),
                        static_cast<unsigned>(sFormat));
        return;
    }
    if (made || changed) {
        const std::string vram = formatFrameGenSynthVram(eye, vramOf(r, g.scratch));
        Log::get().note("%s", vram.c_str());
    }
    pollReadbacks(ctx, r, r.writes);
    pollTimers(ctx);

    const bool half = f.wantShown || f.everyHalf || ((r.calls++ & 1u) == 0u);   // the even frames (and the dump's, and every frame under Lock's key) build the f = 0.5 S
    const bool finishing = half && f.finish;   // Phase 2.3b: this S waits for B's door
    const bool score = r.havePending && r.bFrame + 1u == frame && !f.cameraJump;
    const bool keep = !half && !f.cameraJump;

    // Phase 2.2: on the uav path S is written into the producer texture, which the game device must hold (key 0). Still the runtime's (key 1, not consumed): this call builds
    // nothing and is counted busy.
    bool held = false;
    if (r.sharedPath == 1) {
        const HRESULT a = r.mutex->AcquireSync(0, 0);
        if (a == static_cast<HRESULT>(WAIT_TIMEOUT)) {
            noteBusy(r, eye);
            r.havePending = false;
            if (half) answer.state = EDVR_NATIVE_FRAMEGEN_S_BUSY;
            storeAnswer();
            return;
        }
        if (a != S_OK) {
            sharedFault(r, eye, a);
            bool again = false;
            if (!ensureTarget(dev, r, eye, false, again)) {
                g.failed = true;
                return;
            }
        } else {
            held = true;
            r.busyRun = 0;
        }
    }
    // The head step: the runtime's pose for S's slot when it handed one (the f = 0.5 S only; a replay-capture frame keeps the extrapolation the bench rebuilds), else the extrapolation.
    const bool usePose = half && f.slotPose && !f.replay;
    if (half) {
        if (usePose) ++g.win.poseRuntime;
        else ++g.win.poseFallback;
    }
    answer.poseRuntime = usePose ? 1u : 0u;

    // On a score, the residual head rotation: the rotation the kept f = 1 S was built for against C's true one (this frame's backward head delta), and the tangents S was built with.
    float residual[9];
    if (score) {
        frameGenSynthResidual(r.keptRot, f.deltas.Hd, residual);
        g.rotRes.push_back(temporalRotationAngleDeg(residual));
    }
    FrameGenSynthCb cb;
    frameGenSynthFillCb(cb, f.renderW, f.renderH, f.outW, f.outH, f.tanNow, f.depthA, f.depthB, f.splitMetres, half ? 0.5f : 1.0f, f.deltas, score ? residual : nullptr, score ? r.keptTan : nullptr,
                        usePose ? f.slotRot : nullptr, usePose ? f.slotT : nullptr);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        ++g.win.skipped;
        r.havePending = false;
        if (held) r.mutex->ReleaseSync(0);
        storeAnswer();
        return;
    }
    std::memcpy(m.pData, &cb, sizeof(cb));
    ctx->Unmap(g.cb, 0);
    Scratch& sc = g.scratch;

    // A staging slot still unread when it comes round again is overwritten (its frame is lost to the window, not waited for).
    const int slot = static_cast<int>(r.writes % kStagingDepth);
    r.pending[slot] = false;

    TimerSlot* timer = beginTimer(dev, ctx, half ? 0 : 1);
    CsStageSave save;
    save.save(ctx);
    ctx->CSSetConstantBuffers(0, 1, &g.cb);
    FrameGenSynthPassViews v;
    v.out = f.outSrv;
    v.depth = f.depthSrv;
    v.mv = f.mvSrv;
    v.keySrv = sc.key.srv; v.keyUav = sc.key.uav;
    v.readSrv = sc.read.srv; v.readUav = sc.read.uav;
    v.holesUav = sc.holesUav;
    // Phase 2.3b: an S the door finishes without sharpening is built in `s` (the finish then writes the producer texture from it); one it sharpens is built where it is handed from.
    const bool sharedTarget = held && !(finishing && !f.finishSharpen);
    const Tex& target = sharedTarget ? r.shared : r.s;
    v.sSrv = target.srv; v.sUav = target.uav;
    v.sLumaSrv = r.sLuma.srv; v.sLumaUav = r.sLuma.uav;
    v.bInfoSrv = r.bInfo.srv; v.bInfoUav = r.bInfo.uav;
    v.statsUav = r.statsUav;
    v.linear = g.linear;
    v.rw = r.rw; v.rh = r.rh; v.ow = r.ow; v.oh = r.oh; v.qw = r.qw; v.qh = r.qh;
    frameGenSynthRunPasses(ctx, g.cs, v, score, true, keep);
    ctx->CopyResource(r.staging[slot], r.stats);
    r.pending[slot] = true;
    r.slotKind[slot] = half ? 0 : 1;
    r.slotScored[slot] = score;
    ++r.writes;
    if (timer) timer->timer.end(ctx);
    save.restore(ctx);

    // The dump stages the f = 0.5 S while the module still holds it (an S the door finishes is staged there, finished).
    if (half && f.wantShown && f.shownSink && !finishing) f.shownSink(f.shownSinkContext, ctx, target.tex);
    // Phase 2.3b: an S B's door finishes is not handed over here. The producer texture's key 0 stays held (uav path) until the door; the answer says no_door until the door
    // replaces it, so a door that never comes leaves an S nobody shows.
    if (finishing) {
        r.finPending = true;
        r.finHeld = held;
        r.finInShared = sharedTarget;
        r.finConsumer = f.consumer;
        for (int i = 0; i < 3; ++i) {
            for (int k = 0; k < 3; ++k) r.finRot[i * 3 + k] = cb.rs[i][k];
            r.finT[i] = cb.rs[i][3];
        }
        for (int i = 0; i < 4; ++i) r.finTan[i] = f.tanNow[i];
        r.finDump = f.wantShown ? f.dumpIndex : -1;
        if (held) r.busyRun = 0;
        g.win.pathBits |= held ? 2u : (r.sharedPath == 2 && f.consumer ? 4u : 1u);
        answer.state = EDVR_NATIVE_FRAMEGEN_S_KEPT;
        answer.finish = kFrameGenFinishNoDoor;
        storeAnswer();
        r.finAnswer = answer;
    } else if (held) {
        HRESULT rel;
        if (half && f.consumer) {
            ctx->Flush();
            rel = r.mutex->ReleaseSync(1);
            if (rel == S_OK) answer.state = EDVR_NATIVE_FRAMEGEN_S_PRODUCED;
        } else {
            rel = r.mutex->ReleaseSync(0);
            answer.state = EDVR_NATIVE_FRAMEGEN_S_KEPT;
        }
        g.win.pathBits |= 2u;
        if (rel != S_OK) {
            answer.state = EDVR_NATIVE_FRAMEGEN_S_KEPT;
            sharedFault(r, eye, rel);
        }
    } else if (half && f.consumer && r.sharedPath == 2) {
        const HRESULT a = r.mutex->AcquireSync(0, 0);
        if (a == static_cast<HRESULT>(WAIT_TIMEOUT)) {
            noteBusy(r, eye);
            answer.state = EDVR_NATIVE_FRAMEGEN_S_BUSY;
        } else if (a == S_OK) {
            ctx->CopyResource(r.shared.tex, r.s.tex);
            ctx->Flush();
            const HRESULT rel = r.mutex->ReleaseSync(1);
            if (rel == S_OK) {
                answer.state = EDVR_NATIVE_FRAMEGEN_S_PRODUCED;
                r.busyRun = 0;
            } else {
                answer.state = EDVR_NATIVE_FRAMEGEN_S_KEPT;
                sharedFault(r, eye, rel);
            }
        } else {
            answer.state = EDVR_NATIVE_FRAMEGEN_S_KEPT;
            sharedFault(r, eye, a);
        }
        g.win.pathBits |= 4u;
    } else {
        answer.state = EDVR_NATIVE_FRAMEGEN_S_KEPT;
        g.win.pathBits |= r.sharedPath == 2 ? 4u : 1u;   // the copy path's other builds are its own; private means no producer texture at all
    }
    if (half && !finishing) answer.finish = kFrameGenFinishNoDoor;   // Phase 2.3b: no door finishes this S (the runtime does not show it)
    if (!finishing) storeAnswer();

    r.havePending = keep;
    if (keep) {
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 3; ++k) r.keptRot[i * 3 + k] = cb.rs[i][k];
        for (int i = 0; i < 4; ++i) r.keptTan[i] = f.tanNow[i];
    }
    r.bFrame = frame;
    r.shownValid = half && !held;   // frameGenSynthShown hands out the private S only
    r.shownFrame = frame;
    if (f.replay) replayCapture(dev, ctx, f, frame, cb);
}

// ---- Phase 2.3b: S finished as B is, at B's door ----
ID3D11Texture2D* frameGenSynthFinishSource(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t eye) {
    if (!g_on.load() || eye > 1 || !dev || !ctx) return nullptr;
    EyeRes& r = g.eye[eye];
    if (!r.finPending) return nullptr;
    if (!g.finTimer[eye]) g.finTimer[eye] = beginTimer(dev, ctx, 2);
    return r.finInShared ? r.shared.tex : r.s.tex;
}

ID3D11Texture2D* frameGenSynthFinish(ID3D11DeviceContext* ctx, uint32_t eye, const FrameGenSynthFinishInputs& in) {
    if (!g_on.load() || eye > 1 || !ctx) return nullptr;
    EyeRes& r = g.eye[eye];
    if (!r.finPending) return nullptr;
    r.finPending = false;
    EdvrNativeFrameGenS answer = r.finAnswer;
    const auto store = [&] {
        answer.path = r.sharedPath;
        answer.sharedHandle = r.handle;
        answer.textureSerial = r.shared.tex ? r.serial : 0;
        answer.gameBytes = texBytes(r.s) + texBytes(r.shared) + texBytes(r.fin);
        std::lock_guard<std::mutex> l(g_answerMu);
        g.answer[eye] = answer;
    };
    const auto endTimer = [&] {
        if (g.finTimer[eye]) {
            g.finTimer[eye]->timer.end(ctx);
            g.finTimer[eye] = nullptr;
        }
    };
    // Unfinished: the key goes back to the game device, nothing is handed over, and the runtime is told why.
    const auto unfinished = [&](uint32_t why) -> ID3D11Texture2D* {
        if (r.finHeld && r.mutex) {
            const HRESULT rel = r.mutex->ReleaseSync(0);
            if (rel != S_OK) sharedFault(r, eye, rel);
        }
        r.finHeld = false;
        ++g.win.unfinished;
        g.win.unfinishedWhy = why;
        answer.state = EDVR_NATIVE_FRAMEGEN_S_KEPT;
        answer.finish = why;
        store();
        endTimer();
        return nullptr;
    };
    if (in.why != kFrameGenFinishDone) return unfinished(in.why);
    ID3D11Texture2D* const sTex = r.finInShared ? r.shared.tex : r.s.tex;
    ID3D11Texture2D* const src = in.frame ? in.frame : sTex;
    if (!src || !g.csFinish) return unfinished(kFrameGenFinishFault);
    // S itself in the producer texture is there to be sharpened from: a door that did not sharpen it leaves nothing to finish into (the strength changed between the build and
    // the door).
    if (!in.frame && r.finInShared) return unfinished(kFrameGenFinishSharpen);
    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);
    if (sd.Width != r.ow || sd.Height != r.oh) return unfinished(kFrameGenFinishCropped);
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return unfinished(kFrameGenFinishFault);
    // Where the finished S goes: the producer texture while its key is held (uav path); else `s` when S is not being read from it; else the module's own texture.
    Tex* dst = nullptr;
    if (r.finHeld && r.sharedPath == 1) dst = &r.shared;
    else if (src != r.s.tex && r.s.tex) dst = &r.s;
    else {
        if (!r.fin.tex && makeTex(dev, r.ow, r.oh, r.sFormat, r.fin)) Log::get().note("%s", formatFrameGenSynthVram(eye, vramOf(r, g.scratch)).c_str());
        dst = r.fin.tex ? &r.fin : nullptr;
    }
    dev->Release();
    ID3D11ShaderResourceView* const frameSrv = finishFrameView(r, src);
    if (!dst || !dst->uav || !frameSrv) return unfinished(kFrameGenFinishFault);
    float H[9];
    frameGenSynthFinishHomography(r.finRot, r.finT, r.finTan, r.ow, r.oh, kFrameGenFinishPanelMetres, H);
    FrameGenSynthCb cb;
    frameGenSynthFinishFillCb(cb, r.ow, r.oh, 0, 0, H, in.layerUv, in.layer ? in.layerW : 0u, in.layer ? in.layerH : 0u, in.layer && in.mult);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(g.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return unfinished(kFrameGenFinishFault);
    std::memcpy(m.pData, &cb, sizeof(cb));
    ctx->Unmap(g.cb, 0);
    {
        CsStageSave save;
        save.save(ctx);
        ID3D11ShaderResourceView* nullSrv[11] = {};
        ID3D11UnorderedAccessView* nullUav[7] = {};   // u0..u6, the slots CsStageSave puts back
        ctx->CSSetShaderResources(0, 11, nullSrv);
        ctx->CSSetUnorderedAccessViews(0, 7, nullUav, nullptr);
        ctx->CSSetShader(g.csFinish, nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, &g.cb);
        ctx->CSSetShaderResources(3, 1, &frameSrv);
        ID3D11ShaderResourceView* layer = in.layer;
        ctx->CSSetShaderResources(5, 1, &layer);
        ID3D11ShaderResourceView* mult = in.layer ? in.mult : nullptr;
        ctx->CSSetShaderResources(10, 1, &mult);
        ctx->CSSetUnorderedAccessViews(2, 1, &dst->uav, nullptr);
        ctx->Dispatch((r.ow + 7u) / 8u, (r.oh + 7u) / 8u, 1);
        ctx->CSSetShaderResources(0, 11, nullSrv);
        ctx->CSSetUnorderedAccessViews(0, 7, nullUav, nullptr);
        save.restore(ctx);
    }
    // EDVR's menu over the finished S, as the runtime draws it over B after B's door, before anything stages or takes S. A menu showing that cannot be drawn leaves S
    // unfinished (menu): the slot then replays B, which has it.
    if (in.menuSink) {
        const uint32_t menuWhy = in.menuSink(ctx, eye, dst->tex, in.menuBox, r.finRot, r.finT);
        if (menuWhy != kFrameGenFinishDone) return unfinished(menuWhy);
        answer.menu = 1u;
    }
    // The eye dump stages the finished S while the module still holds it.
    if (r.finDump >= 0 && in.dumpSink) in.dumpSink(ctx, r.finDump, dst->tex, in.bFinished);
    // Handed over as 2.2 hands S: on the uav path the held key goes to the runtime; on the copy path the finished S is copied in under the key.
    answer.state = EDVR_NATIVE_FRAMEGEN_S_KEPT;
    if (r.finConsumer && r.finHeld) {
        ctx->Flush();
        const HRESULT rel = r.mutex->ReleaseSync(1);
        r.finHeld = false;
        if (rel == S_OK) answer.state = EDVR_NATIVE_FRAMEGEN_S_PRODUCED;
        else sharedFault(r, eye, rel);
    } else if (r.finHeld) {
        const HRESULT rel = r.mutex->ReleaseSync(0);
        r.finHeld = false;
        if (rel != S_OK) sharedFault(r, eye, rel);
    } else if (r.finConsumer && r.sharedPath == 2 && r.mutex) {
        const HRESULT a = r.mutex->AcquireSync(0, 0);
        if (a == static_cast<HRESULT>(WAIT_TIMEOUT)) {
            noteBusy(r, eye);
            answer.state = EDVR_NATIVE_FRAMEGEN_S_BUSY;
        } else if (a == S_OK) {
            ctx->CopyResource(r.shared.tex, dst->tex);
            ctx->Flush();
            const HRESULT rel = r.mutex->ReleaseSync(1);
            if (rel == S_OK) {
                answer.state = EDVR_NATIVE_FRAMEGEN_S_PRODUCED;
                r.busyRun = 0;
            } else {
                sharedFault(r, eye, rel);
            }
        } else {
            sharedFault(r, eye, a);
        }
    }
    answer.finish = kFrameGenFinishDone;
    ++g.win.finished;
    store();
    endTimer();
    return dst->tex;
}

bool frameGenReplayFinish(const wchar_t* eyesDir, const wchar_t* stamp) {
    if (!rp.open || rp.writing || !eyesDir || !stamp) return false;
    if (rp.count == 0) {
        Log::get().note("frame generation (synth): replay capture %ls: nothing captured (%s).", stamp, rp.stop.empty() ? "the synthesizer did not run on the run's first frame" : rp.stop.c_str());
        replayRelease();
        return false;
    }
    const std::wstring dir = std::wstring(eyesDir) + L"\\fgreplay_" + stamp;
    CreateDirectoryW(eyesDir, nullptr);
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        Log::get().note("frame generation (synth): replay capture %ls: could not make %ls.", stamp, dir.c_str());
        replayRelease();
        return false;
    }
    rp.dir = dir;
    rp.stamp = stamp;
    rp.files.clear();
    rp.job = 0;
    rp.writing = true;
    return true;
}

void frameGenReplayPump() {
    if (!rp.writing) return;
    if (rp.workerRunning) {
        if (!rp.workerDone.load(std::memory_order_acquire)) return;
        rp.worker.join();
        rp.workerRunning = false;
        ID3D11Texture2D*& t = rp.tex[rp.job / 3][rp.job % 3];
        if (rp.mapped && rp.mapCtx && t) rp.mapCtx->Unmap(t, 0);
        rp.mapped = false;
        releaseCom(rp.mapCtx);
        releaseCom(t);
        if (rp.resultOk) rp.files.push_back(rp.result);
        else if (rp.stop.empty()) rp.stop = "could not write " + rp.result.name;
        ++rp.job;
    }
    if (rp.job >= rp.count * 3) { replayFinishSet(); return; }
    const uint32_t k = rp.job / 3, part = rp.job % 3;
    ID3D11Texture2D* t = rp.tex[k][part];
    ID3D11Device* dev = nullptr;
    t->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr;
    if (dev) { dev->GetImmediateContext(&ctx); dev->Release(); }
    if (!ctx) { releaseCom(t); ++rp.job; if (rp.stop.empty()) rp.stop = "no device context to read the capture back"; return; }
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = ctx->Map(t, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);   // never waits: the copy is tried again next frame
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { ctx->Release(); return; }
    char name[32];
    std::snprintf(name, sizeof(name), "f%u_%s.bin", k, kReplayPart[part]);
    if (FAILED(hr)) {
        ctx->Release();
        releaseCom(t);
        ++rp.job;
        if (rp.stop.empty()) rp.stop = std::string("could not read back ") + name;
        return;
    }
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    rp.mapped = true;
    rp.mapCtx = ctx;   // keeps the reference until the unmap
    rp.map = m;
    rp.result = FrameGenReplayFile{};
    rp.result.name = name;
    rp.workerDone.store(false, std::memory_order_relaxed);
    rp.workerRunning = true;
    rp.worker = std::thread(replayWorker, static_cast<const uint8_t*>(m.pData), static_cast<uint32_t>(m.RowPitch), d.Height, d.Width * frameGenReplayFormatBytes(d.Format), rp.dir + L"\\" + std::wstring(name, name + std::strlen(name)));
}

bool frameGenReplayIdle() { return !rp.open && !rp.writing && !rp.workerRunning && rp.count == 0; }
uint32_t frameGenReplayCaptured() { return rp.count; }

namespace {
// The Monitor page's copy of the last closed window (frameGenSynthMonitor): written here once a second, read by the page once a second.
std::mutex g_monitorMu;
FrameGenMonitorSynth g_monitor;
void publishMonitor(bool called) {
    FrameGenMonitorSynth m;
    m.valid = called;
    m.built = g.win.frames[0];
    m.gpuMs = g.win.gpuMedian[0];
    m.gpuN = g.win.gpuSamples[0];
    m.finishMs = g.win.gpuMedian[2];
    m.finishN = g.win.gpuSamples[2];
    // The game device's bytes, as the VRAM line's groups count them: both eyes and the shared scratch.
    m.gameBytes = vramOf(g.eye[0], g.scratch).eye() + vramOf(g.eye[1], g.scratch).eye() + vramOf(g.eye[0], g.scratch).scratch();
    std::lock_guard<std::mutex> l(g_monitorMu);
    g_monitor = m;
}
}  // namespace

FrameGenMonitorSynth frameGenSynthMonitor() {
    std::lock_guard<std::mutex> l(g_monitorMu);
    return g_monitor;
}

void frameGenSynthFrameBoundary() {
    static int64_t s_ticks = 0;
    if (g_on.load()) frameGenReplayPump();
    if (!g_on.load()) {
        if (rp.open || rp.writing || rp.count) replayRelease();   // the key went off: nothing captured is kept
        // The key went off: nothing stays resident, and the next time on starts a fresh window.
        if (anyResources() || g.dev || g.failed) releaseAll();
        if (s_ticks != 0 || g.win.frames[0] || g.win.frames[1] || g.win.skipped) {
            windowReset();
            s_ticks = 0;
            std::lock_guard<std::mutex> l(g_monitorMu);
            g_monitor = FrameGenMonitorSynth{};
        }
        return;
    }
    const int64_t now = qpcNow();
    const int64_t freq = qpcFrequency();
    if (s_ticks == 0) {
        s_ticks = now;
        return;
    }
    if (freq <= 0 || now - s_ticks < freq) return;
    s_ticks = now;
    if (g.win.frames[0] == 0 && g.win.frames[1] == 0 && g.win.skipped == 0) {   // the module was not called in the window (a menu, a loading screen): nothing to say
        publishMonitor(false);
        windowReset();
        return;
    }
    for (int k = 0; k < 3; ++k) {
        g.win.gpuSamples[k] = static_cast<uint32_t>(g.gpuMs[k].size());
        g.win.gpuMax[k] = 0.0f;
        for (float v : g.gpuMs[k]) if (v > g.win.gpuMax[k]) g.win.gpuMax[k] = v;
        g.win.gpuMedian[k] = frameGenMedian(g.gpuMs[k].data(), g.gpuMs[k].size());
    }
    fillRotation(g.win);
    const std::string line = formatFrameGenSynthReport(g.win);
    Log::get().note("%s", line.c_str());
    publishMonitor(true);
    // Phase 2.2: a window that built S and was never handed a slot since the key went on: the runtime does not have the channel (an older openvr_api.dll). Said once per arm.
    if (!g.slotSeen && !g.slotAbsenceSaid && g.win.frames[0] > 0) {
        g.slotAbsenceSaid = true;
        Log::get().note("frame generation (synth): no slot from the runtime (an older openvr_api.dll, or a runtime that is not EDVR's): S's head stays on the synthesizer's extrapolation "
                        "and S on the game device.");
    }
    windowReset();
}

}  // namespace edvr

// Phase 2.2: the runtime's read of one eye's S after its treat (common/native_temporal.h).
extern "C" HRESULT WINAPI edvrFrameGenTakeS(uint32_t eye, EdvrNativeFrameGenS* out) { return edvr::frameGenSynthTakeS(eye, out); }

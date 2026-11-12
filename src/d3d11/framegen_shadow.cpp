// The frame-generation shadow scorer: see framegen_shadow.h.
#include "temporal_shader_bytecode.h"
#include "framegen_shadow.h"
#include "cs_stage_save.h"
#include "gpu_frame_timing.h"
#include "gpu_timing.h"
#include "shader_swap.h"
#include "../common/framegen_replay_math.h"   // frameGenReplayFormatBytes: the DXGI format table the VRAM line reads the descriptors with
#include "../common/framegen_shadow_report.h"
#include "../common/log.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace edvr {
namespace {

constexpr int kStagingDepth = 4;
constexpr int kTimerRing = 8;
constexpr int kEntries = 5;      // capture, clear, splat, score, score B
constexpr int kSrvSlots = 7;     // t0..t6
constexpr int kUavSlots = 4;     // u0..u3

std::atomic<bool> g_on{false};
std::atomic<bool> g_told{false};   // the armed line has been said since the key last went on

struct Tex {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
};

// One eye's resources and its last frame's capture and deltas.
struct EyeRes {
    bool created = false;
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0, qw = 0, qh = 0;
    Tex luma[2], depth[2], key;
    ID3D11Buffer* stats = nullptr;
    ID3D11UnorderedAccessView* statsUav = nullptr;
    ID3D11Buffer* staging[kStagingDepth] = {};
    bool pending[kStagingDepth] = {};
    uint8_t slotSet[kStagingDepth] = {};   // the stats set (FrameGenShadowSet) each pending slot's frame belongs to
    uint32_t writes = 0;
    int64_t lastT = 0;            // QPC at the previous frame call, 0 before the first
    double prevDtMs = 0.0;        // the previous frame's interval (A minus its own predecessor), 0 when unknown
    int cur = 0;                  // the slot of the latest capture
    bool haveCapture = false;
    uint32_t captureFrame = 0;
    FrameGenDeltas prev;          // the latest frame's deltas
    bool haveDeltas = false, prevWorldValid = false, prevJump = false;
};

struct TimerSlot {
    GpuTimer timer;
    bool inUse = false;
};

struct State {
    ID3D11Device* dev = nullptr;   // identity only
    bool failed = false;           // creation failed on this device: stand down until it changes or the key is toggled
    ID3D11ComputeShader* cs[kEntries] = {};   // capture, clear, splat, score, score B
    ID3D11Buffer* cb = nullptr;
    DXGI_FORMAT lumaFormat = DXGI_FORMAT_R16_FLOAT;
    EyeRes eye[2];
    TimerSlot timers[kTimerRing];
    // The one-second window.
    FrameGenShadowWindow win;
    std::vector<float> gpuMs, rigDeg, rigMm, motionMm, magRatio, dtRatio, dtMs, rigDtMm;
    uint32_t eyeFrames = 0;
    int64_t windowTicks = 0;
};
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

void releaseEye(EyeRes& r) {
    for (Tex& t : r.luma) releaseTex(t);
    for (Tex& t : r.depth) releaseTex(t);
    releaseTex(r.key);
    releaseCom(r.statsUav);
    releaseCom(r.stats);
    for (auto& s : r.staging) releaseCom(s);
    r = EyeRes{};
}

bool anyResources() {
    if (g.cb) return true;
    for (const EyeRes& r : g.eye) if (r.created) return true;
    for (ID3D11ComputeShader* c : g.cs) if (c) return true;
    return false;
}

void windowReset() {
    g.win = FrameGenShadowWindow{};
    g.gpuMs.clear();
    g.rigDeg.clear();
    g.rigMm.clear();
    g.motionMm.clear();
    g.magRatio.clear();
    g.dtRatio.clear();
    g.dtMs.clear();
    g.rigDtMm.clear();
    g.eyeFrames = 0;
}

// The shaders, the constant buffer and the formats: made once per device. The timers are left as they are (a timer with a query in flight is released only by
// its owner's context, gpu_timing.h); the ring simply keeps polling them.
void releaseAll() {
    for (EyeRes& r : g.eye) releaseEye(r);
    for (auto& c : g.cs) releaseCom(c);
    releaseCom(g.cb);
    g.dev = nullptr;
    g.failed = false;
}

bool createCommon(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    struct Entry { const unsigned char* code; size_t size; const char* name; };
    const Entry entries[kEntries] = {
        {kFrameGenShadowCaptureBytecode, sizeof(kFrameGenShadowCaptureBytecode), "framegen_shadow_capture"},
        {kFrameGenShadowClearBytecode, sizeof(kFrameGenShadowClearBytecode), "framegen_shadow_clear"},
        {kFrameGenShadowSplatBytecode, sizeof(kFrameGenShadowSplatBytecode), "framegen_shadow_splat"},
        {kFrameGenShadowScoreBytecode, sizeof(kFrameGenShadowScoreBytecode), "framegen_shadow_score"},
        {kFrameGenShadowScoreBBytecode, sizeof(kFrameGenShadowScoreBBytecode), "framegen_shadow_score_b"},
    };
    for (int i = 0; i < kEntries; ++i) {
        g.cs[i] = shaderSwapCreateCs(ctx, entries[i].code, entries[i].size, entries[i].name, "frame generation (shadow)");
        if (!g.cs[i]) return false;
    }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(FrameGenShadowCb);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g.cb))) return false;
    // R16_FLOAT where the adapter does typed UAV stores on it (every D3D11 part that matters does), else R32_FLOAT.
    UINT support = 0;
    const UINT want = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    g.lumaFormat = (SUCCEEDED(dev->CheckFormatSupport(DXGI_FORMAT_R16_FLOAT, &support)) && (support & want) == want) ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R32_FLOAT;
    return true;
}

bool makeTex(ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool srv, Tex& out) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | (srv ? D3D11_BIND_SHADER_RESOURCE : 0u);
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &out.tex))) return false;
    if (srv && FAILED(dev->CreateShaderResourceView(out.tex, nullptr, &out.srv))) return false;
    return SUCCEEDED(dev->CreateUnorderedAccessView(out.tex, nullptr, &out.uav));
}

bool createEye(ID3D11Device* dev, EyeRes& r, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    r = EyeRes{};
    r.rw = rw; r.rh = rh; r.ow = ow; r.oh = oh;
    r.qw = (rw + 3u) / 4u;
    r.qh = (rh + 3u) / 4u;
    if (r.qw == 0 || r.qh == 0) return false;
    bool ok = true;
    for (int i = 0; i < 2 && ok; ++i) {
        ok = makeTex(dev, r.qw, r.qh, g.lumaFormat, true, r.luma[i]) && makeTex(dev, r.qw, r.qh, DXGI_FORMAT_R32_FLOAT, true, r.depth[i]);
    }
    ok = ok && makeTex(dev, r.qw, r.qh, DXGI_FORMAT_R32_UINT, false, r.key);
    if (ok) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = kFrameGenShadowStatsUints * 4u;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &r.stats));
        if (ok) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_UNKNOWN;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = kFrameGenShadowStatsUints;
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

FrameGenShadowVram eyeVram(const EyeRes& r) {
    FrameGenShadowVram v;
    for (int i = 0; i < 2; ++i) v.captures += texBytes(r.luma[i]) + texBytes(r.depth[i]);
    v.keys = texBytes(r.key);
    v.stats = bufBytes(r.stats);
    for (ID3D11Buffer* s : r.staging) v.stats += bufBytes(s);
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
        FrameGenShadowEyeResult res;
        frameGenDecodeStats(static_cast<const uint32_t*>(m.pData), res);
        ctx->Unmap(r.staging[slot], 0);
        const int set = r.slotSet[slot];
        frameGenShadowMergeProbe(g.win, res);   // the second kernel's set, on every scored frame whatever its set
        if (set == kSetJump) {
            frameGenShadowMergeSet(g.win.jump, res);
        } else {
            frameGenShadowMerge(g.win, res);
            ++g.win.framesScored;
        }
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
        if (p == GpuTimerPoll::Ready) g.gpuMs.push_back(static_cast<float>(ms));
    }
}

// A timer pair is begun only inside a frequency scope that is already open (the census's, or the application-frame clock's): without one, GpuTimer::begin would
// open a scope of its own and hold a record of the clock's eight for several frames (gpu_census.cpp, frameWorkFirstCommand).
TimerSlot* beginTimer(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (!(gpuTimingCensusScopeOpen() || gpuFrameClockLive())) return nullptr;
    if (!gpuTimingAccepts(ctx) && !gpuTimingBind(dev, ctx)) return nullptr;
    for (TimerSlot& t : g.timers) {
        if (t.inUse) continue;
        if (t.timer.begin(dev, ctx)) { t.inUse = true; return &t; }
        return nullptr;
    }
    return nullptr;
}

void bindAndDispatch(ID3D11DeviceContext* ctx, int entry, ID3D11ShaderResourceView* const srv[kSrvSlots], ID3D11UnorderedAccessView* const uav[kUavSlots], uint32_t gx, uint32_t gy) {
    ctx->CSSetShader(g.cs[entry], nullptr, 0);
    ctx->CSSetShaderResources(0, kSrvSlots, srv);
    ctx->CSSetUnorderedAccessViews(0, kUavSlots, uav, nullptr);
    ctx->Dispatch(gx, gy, 1);
}

}  // namespace

void frameGenShadowConfigure(bool on) {
    g_on.store(on);
    if (!on) g_told.store(false);
}

bool frameGenShadowOn() { return g_on.load(); }

bool frameGenShadowResident() { return anyResources(); }

// The Monitor page: the scorer's game-device bytes (its VRAM line's groups, both eyes), refreshed by the frame boundary once a second; 0 while the key is off.
namespace {
std::atomic<uint64_t> g_vramBytes{0};
}
uint64_t frameGenShadowVramBytes() { return g_vramBytes.load(std::memory_order_relaxed); }

FrameGenShadowWindow frameGenShadowOpenWindow() { return g.win; }

void frameGenShadowEyeFrame(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t eye, uint32_t frame, const FrameGenShadowFrame& f) {
    if (!g_on.load()) return;
    if (!g_told.exchange(true)) {
        const std::string line = formatFrameGenShadowArmed((f.renderW + 3u) / 4u, (f.renderH + 3u) / 4u);
        Log::get().note("%s", line.c_str());
    }
    if (!dev || !ctx || eye > 1 || !f.tanNow) return;
    if (g.dev && g.dev != dev) releaseAll();   // a new device: everything starts over
    if (g.failed) return;
    if (!g.dev) {
        g.dev = dev;
        if (!createCommon(dev, ctx)) {
            g.failed = true;
            Log::get().note("frame generation (shadow): the scorer's shaders or constant buffer could not be created; it stands down.");
            return;
        }
    }
    EyeRes& r = g.eye[eye];
    if (!r.created || r.rw != f.renderW || r.rh != f.renderH || r.ow != f.outW || r.oh != f.outH) {
        releaseEye(r);
        if (!createEye(dev, r, f.renderW, f.renderH, f.outW, f.outH)) {
            g.failed = true;
            Log::get().note("frame generation (shadow): the eye's quarter-size resources could not be created (%ux%u render); it stands down.",
                            static_cast<unsigned>(f.renderW), static_cast<unsigned>(f.renderH));
            return;
        }
        const std::string vram = formatFrameGenShadowVram(eye, eyeVram(r));
        Log::get().note("%s", vram.c_str());
    }
    ++g.eyeFrames;
    pollReadbacks(ctx, r, r.writes);
    pollTimers(ctx);

    // The interval to the eye's previous frame call (B minus A) and the one before it (A minus its own predecessor), in ms; 0 when unknown.
    const int64_t nowT = qpcNow(), freq = qpcFrequency();
    const double dtB = (r.lastT != 0 && freq > 0 && nowT > r.lastT) ? static_cast<double>(nowT - r.lastT) * 1000.0 / static_cast<double>(freq) : 0.0;
    const double dtA = r.prevDtMs;
    r.lastT = nowT;
    r.prevDtMs = dtB;

    // Why this frame is not scored, in the plan's order; -1 scores it. A camera_jump frame is scored too, into the jump set.
    int skip = -1;
    if (!r.haveCapture || r.captureFrame + 1u != frame || !f.prevDepthValid || !f.depthWritten) skip = kSkipNoPrevDepth;
    else if (!r.haveDeltas || !f.tanPrev) skip = kSkipNoPrevDelta;
    else if (!f.worldValid || !r.prevWorldValid) skip = kSkipRowsInvalid;
    const bool jump = f.cameraJump || r.prevJump;
    const bool canCapture = f.depthWritten && f.outSrv && f.depthSrv;
    if (skip < 0 && !canCapture) skip = kSkipNoPrevDepth;
    const int next = r.haveCapture ? 1 - r.cur : 0;
    bool scored = skip < 0 && canCapture;
    const int set = (scored && jump) ? kSetJump : kSetPredicted;   // jump frames apart

    // A staging slot still unread when it comes round again is overwritten: counted, not waited for.
    const int slot = static_cast<int>(r.writes % kStagingDepth);
    if (scored && r.pending[slot]) {
        ++g.win.skipped[kSkipReadbackLate];
        r.pending[slot] = false;
    }

    if (canCapture) {
        FrameGenShadowCb cb;
        float Wp[9], tp[3];
        FrameGenAffine bwd[kFrameGenShadowClasses];
        if (scored) {
            frameGenPredictWorld(r.prev, f.deltas, Wp, tp);
            frameGenBackward(f.deltas, Wp, tp, bwd);
            // The prediction's own miss, and the same with the ship's step scaled by dt_B / dt_A; a jump frame's miss is the jump, so it is left out of both.
            const float scale = (dtA > 0.0 && dtB > 0.0) ? static_cast<float>(dtB / dtA) : 0.0f;
            if (!jump) {
                float deg = 0.0f, mm = 0.0f;
                frameGenRigMiss(Wp, tp, f.deltas, &deg, &mm);
                g.rigDeg.push_back(deg);
                g.rigMm.push_back(mm);
                if (scale > 0.0f) {
                    float tpDt[3], dtMm = 0.0f;
                    for (int i = 0; i < 3; ++i) tpDt[i] = (r.prev.tv[i] - r.prev.th[i]) * scale + f.deltas.th[i];
                    frameGenRigMiss(Wp, tpDt, f.deltas, nullptr, &dtMm);
                    g.rigDtMm.push_back(dtMm);
                }
            }
            // The motion facts.
            const auto mag = [](const float* v) { return static_cast<float>(std::sqrt(static_cast<double>(v[0]) * v[0] + static_cast<double>(v[1]) * v[1] + static_cast<double>(v[2]) * v[2]) * 1000.0); };
            const float mB = mag(f.deltas.tv), mA = mag(r.prev.tv);
            g.motionMm.push_back(mB);
            if (mA > 1.0f) g.magRatio.push_back(mB / mA);
            if (scale > 0.0f) g.dtRatio.push_back(scale);
            if (dtB > 0.0) g.dtMs.push_back(static_cast<float>(dtB));
        }
        frameGenFillCb(cb, f.renderW, f.renderH, f.outW, f.outH, scored ? f.tanPrev : f.tanNow, f.tanNow, f.depthA, f.depthB, f.splitMetres, bwd);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(g.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, &cb, sizeof(cb));
            ctx->Unmap(g.cb, 0);
        } else {
            scored = false;
            skip = skip < 0 ? kSkipNoPrevDelta : skip;
        }
        TimerSlot* timer = beginTimer(dev, ctx);
        CsStageSave save;
        save.save(ctx);
        ctx->CSSetConstantBuffers(0, 1, &g.cb);
        const uint32_t gx = (r.qw + 7u) / 8u, gy = (r.qh + 7u) / 8u;
        ID3D11ShaderResourceView* noSrv[kSrvSlots] = {};
        ID3D11UnorderedAccessView* noUav[kUavSlots] = {};
        {   // capture this frame into the slot the next frame will read as its previous: luma and depth
            ID3D11ShaderResourceView* srv[kSrvSlots] = {f.outSrv, f.depthSrv};
            ID3D11UnorderedAccessView* uav[kUavSlots] = {r.luma[next].uav, r.depth[next].uav};
            bindAndDispatch(ctx, 0, srv, uav, gx, gy);
        }
        if (scored) {
            const int prevSlot = r.cur;
            {   // clear
                ID3D11UnorderedAccessView* uav[kUavSlots] = {nullptr, nullptr, r.key.uav, r.statsUav};
                bindAndDispatch(ctx, 1, noSrv, uav, gx, gy);
            }
            {   // splat: the previous depth, forward
                ID3D11ShaderResourceView* srv[kSrvSlots] = {nullptr, nullptr, nullptr, r.depth[prevSlot].srv};
                ID3D11UnorderedAccessView* uav[kUavSlots] = {nullptr, nullptr, r.key.uav, nullptr};
                bindAndDispatch(ctx, 2, srv, uav, gx, gy);
            }
            {   // score: the previous and this frame's luma against the keys, backward
                ID3D11ShaderResourceView* srv[kSrvSlots] = {nullptr, nullptr, r.luma[prevSlot].srv, nullptr, r.luma[next].srv};
                ID3D11UnorderedAccessView* uav[kUavSlots] = {nullptr, nullptr, r.key.uav, r.statsUav};
                bindAndDispatch(ctx, 3, srv, uav, gx, gy);
            }
            if (f.mvSrv) {   // score B: this frame's cells by its own depth, carried back by the DLSS vector (the mv set's block of the stats words)
                ID3D11ShaderResourceView* srv[kSrvSlots] = {nullptr, nullptr, r.luma[prevSlot].srv, nullptr, r.luma[next].srv, f.mvSrv, r.depth[next].srv};
                ID3D11UnorderedAccessView* uav[kUavSlots] = {nullptr, nullptr, nullptr, r.statsUav};
                bindAndDispatch(ctx, 4, srv, uav, gx, gy);
            }
            ctx->CSSetShaderResources(0, kSrvSlots, noSrv);
            ctx->CSSetUnorderedAccessViews(0, kUavSlots, noUav, nullptr);
            ctx->CopyResource(r.staging[slot], r.stats);
            r.pending[slot] = true;
            r.slotSet[slot] = static_cast<uint8_t>(set);
            if (set == kSetJump) ++g.win.skipped[kSkipCameraJump];
            ++r.writes;
        } else {
            ctx->CSSetShaderResources(0, kSrvSlots, noSrv);
            ctx->CSSetUnorderedAccessViews(0, kUavSlots, noUav, nullptr);
        }
        if (timer) timer->timer.end(ctx);
        save.restore(ctx);
        r.cur = next;
        r.haveCapture = true;
        r.captureFrame = frame;
    } else {
        r.haveCapture = false;
    }
    if (!scored && skip >= 0) ++g.win.skipped[skip];
    r.prev = f.deltas;
    r.haveDeltas = true;
    r.prevWorldValid = f.worldValid;
    r.prevJump = f.cameraJump;
}

void frameGenShadowFrameBoundary() {
    if (!g_on.load()) {
        // The key went off: nothing stays resident, and the next time on starts a fresh window.
        if (anyResources() || g.dev || g.failed) releaseAll();
        if (g.windowTicks != 0 || g.eyeFrames != 0) { windowReset(); g.windowTicks = 0; }
        g_vramBytes.store(0, std::memory_order_relaxed);
        return;
    }
    const int64_t now = qpcNow();
    const int64_t freq = qpcFrequency();
    if (g.windowTicks == 0) {
        g.windowTicks = now;
        return;
    }
    if (freq <= 0 || now - g.windowTicks < freq) return;
    g.windowTicks = now;
    g_vramBytes.store(eyeVram(g.eye[0]).eye() + eyeVram(g.eye[1]).eye(), std::memory_order_relaxed);
    if (g.eyeFrames == 0) {   // the pass treated no frame in the window (a menu, a loading screen): nothing to say
        windowReset();
        return;
    }
    g.win.gpuSamples = static_cast<uint32_t>(g.gpuMs.size());
    g.win.gpuMsMax = 0.0f;
    for (float v : g.gpuMs) if (v > g.win.gpuMsMax) g.win.gpuMsMax = v;
    g.win.gpuMsMedian = frameGenMedian(g.gpuMs.data(), g.gpuMs.size());
    g.win.rigDegMax = 0.0f;
    for (float v : g.rigDeg) if (v > g.win.rigDegMax) g.win.rigDegMax = v;
    g.win.rigDegMedian = frameGenMedian(g.rigDeg.data(), g.rigDeg.size());
    g.win.rigMmMax = 0.0f;
    for (float v : g.rigMm) if (v > g.win.rigMmMax) g.win.rigMmMax = v;
    g.win.rigMmMedian = frameGenMedian(g.rigMm.data(), g.rigMm.size());
    FrameGenShadowMotion& m = g.win.motion;
    const auto maxOf = [](const std::vector<float>& v) { float x = 0.0f; for (float e : v) if (e > x) x = e; return x; };
    m.motionN = static_cast<uint32_t>(g.motionMm.size());
    m.motionMax = maxOf(g.motionMm);
    m.motionMedian = frameGenMedian(g.motionMm.data(), g.motionMm.size());
    m.magN = static_cast<uint32_t>(g.magRatio.size());
    m.magP5 = frameGenQuantile(g.magRatio.data(), g.magRatio.size(), 0.05f);
    m.magP50 = frameGenQuantile(g.magRatio.data(), g.magRatio.size(), 0.50f);
    m.magP95 = frameGenQuantile(g.magRatio.data(), g.magRatio.size(), 0.95f);
    m.dtRatioN = static_cast<uint32_t>(g.dtRatio.size());
    m.dtRatioP5 = frameGenQuantile(g.dtRatio.data(), g.dtRatio.size(), 0.05f);
    m.dtRatioP50 = frameGenQuantile(g.dtRatio.data(), g.dtRatio.size(), 0.50f);
    m.dtRatioP95 = frameGenQuantile(g.dtRatio.data(), g.dtRatio.size(), 0.95f);
    m.dtN = static_cast<uint32_t>(g.dtMs.size());
    m.dtMax = maxOf(g.dtMs);
    m.dtMedian = frameGenMedian(g.dtMs.data(), g.dtMs.size());
    m.rigDtN = static_cast<uint32_t>(g.rigDtMm.size());
    m.rigDtMax = maxOf(g.rigDtMm);
    m.rigDtMedian = frameGenMedian(g.rigDtMm.data(), g.rigDtMm.size());
    const std::string line = formatFrameGenShadowReport(g.win);
    Log::get().note("%s", line.c_str());
    windowReset();
}

}  // namespace edvr

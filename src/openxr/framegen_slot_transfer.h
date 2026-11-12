#pragma once
// Frame generation, Phase 2.2 (fix.frame_generation_shadow, TEMPORARY; docs/design-frame-generation-2026-10-26.md): the runtime's side of one eye's S transfer. The d3d11 half
// makes the producer texture on the game device (src\d3d11\framegen_synth.cpp: S's format and output size, a keyed mutex, an NT handle) and writes S into it under key 0, then
// releases key 1. This side opens that texture by its handle on the XR device, keeps a private copy of the same size and format, and consumes a handoff: AcquireSync(1) with a
// zero or short timeout, a copy to the private texture, ReleaseSync(0). Separate from the real frames' EyeCapture sets (`captured`, `previousPair`), which it never touches.
// Owner thread only; nothing here calls the game device or its context. Header-only, so the runtime's build list does not change.
#include <d3d11_1.h>
#include <dxgi.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>

#include "../common/framegen_lock_report.h"   // FrameGenLockReason (the slot's size 3 default); it includes framegen_slot_report.h
#include "../common/framegen_slot_report.h"
#include "../common/native_temporal.h"

#include <cstring>

namespace edvr::openxr {

// The runtime's end of the slot channel (common/native_temporal.h): the d3d11 half's two exports, both or neither (an older d3d11.dll has neither: nothing is handed then), the
// slot as handed, and the read of one eye's answer, which counts only for the pair it is asked for.
struct FrameGenSlotChannel {
  EdvrNativeFrameGenNoteSlot note = nullptr;
  EdvrNativeFrameGenTakeS take = nullptr;
  void bind(EdvrNativeFrameGenNoteSlot n, EdvrNativeFrameGenTakeS t) noexcept {
    note = n && t ? n : nullptr;
    take = n && t ? t : nullptr;
  }
  bool resolve(HMODULE provider) noexcept {
    bind(provider ? reinterpret_cast<EdvrNativeFrameGenNoteSlot>(GetProcAddress(provider, "edvrFrameGenNoteSlot")) : nullptr,
         provider ? reinterpret_cast<EdvrNativeFrameGenTakeS>(GetProcAddress(provider, "edvrFrameGenTakeS")) : nullptr);
    return present();
  }
  bool present() const noexcept { return note != nullptr; }
  // The slot for pair `sequence` at T (displayTime), S's time T_S (0: none) and the head located there (null: none), with whether this runtime consumes S, and (size 2, Phase 2.3)
  // whether Lock is engaged as the pair begins.
  static EdvrNativeFrameGenSlot slot(uint64_t sequence, int64_t displayTime, int64_t slotTime, const float* head, bool consumer, bool lockEngaged = false) noexcept {
    EdvrNativeFrameGenSlot s{};
    s.size = EDVR_NATIVE_FRAMEGEN_SLOT_SIZE_4;   // the size 3 fields (the Monitor page's) and size 4's lockMode are the caller's to fill; left as made: Lock's key off, no window, no miss
    s.lockReason = static_cast<uint32_t>(FrameGenLockReason::KeyOff);
    s.slotDegMedian = -1.0f;
    s.lockEngaged = lockEngaged ? 1u : 0u;
    s.version = EDVR_NATIVE_FRAMEGEN_VERSION_1;
    s.sequence = sequence;
    s.displayTime = displayTime;
    s.slotTime = slotTime;
    s.consumer = consumer ? 1u : 0u;
    if (head && slotTime) {
      std::memcpy(s.head, head, sizeof(s.head));
      s.poseValid = 1u;
    }
    return s;
  }
  HRESULT hand(const EdvrNativeFrameGenSlot& s) const noexcept { return note ? note(&s) : S_FALSE; }
  // S_OK with `out` filled only when the d3d11 half answers for this very pair; S_FALSE otherwise (none built, another pair's, or no channel).
  HRESULT answer(uint32_t eye, uint64_t sequence, EdvrNativeFrameGenS& out) const noexcept {
    out = EdvrNativeFrameGenS{};
    out.size = EDVR_NATIVE_FRAMEGEN_S_SIZE_2;   // a d3d11.dll from before size 2 writes size 1's bytes and says so: menu stays 0
    out.version = EDVR_NATIVE_FRAMEGEN_VERSION_1;
    if (!take || !sequence) return S_FALSE;
    const HRESULT hr = take(eye, &out);
    if (hr != S_OK || out.sequence != sequence || out.eye != eye) return FAILED(hr) ? hr : S_FALSE;
    return S_OK;
  }
};

// The private copy's format: the typeless family of an 8-bit RGBA/BGRA format, any other format as it is (an S in it cannot be shown; the S slot replays B).
inline DXGI_FORMAT frameGenSlotPrivateFormat(DXGI_FORMAT f) noexcept {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    default: return f;
  }
}

class FrameGenSlotTransfer final {
 public:
  // Opens the producer texture `handle` names on `consumer` (borrowed; the d3d11 half keeps it open while `serial` stands) and makes the private copy. A transfer already open
  // on this serial is kept. A handoff pending on the previous texture is dropped (its key is the game device's to remake). S_OK, or the failure with nothing kept.
  HRESULT open(ID3D11Device* consumer, HANDLE handle, uint64_t serial) noexcept {
    if (!consumer || !handle || !serial) return E_INVALIDARG;
    if (shared_ && serial_ == serial && consumer_.Get() == consumer) return S_OK;
    release();
    Microsoft::WRL::ComPtr<ID3D11Device1> device1;
    HRESULT hr = consumer->QueryInterface(IID_PPV_ARGS(&device1));
    Microsoft::WRL::ComPtr<ID3D11Texture2D> shared, copy;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
    if (hr == S_OK) hr = device1->OpenSharedResource1(handle, IID_PPV_ARGS(&shared));
    if (hr == S_OK) hr = shared.As(&mutex);
    D3D11_TEXTURE2D_DESC d{};
    if (hr == S_OK) {
      shared->GetDesc(&d);
      // Phase 2.3: an 8-bit RGBA/BGRA S is kept typeless (the copy is within its family), so the stereo renderer views it as it views a captured eye (EyeCapture::adopt).
      d.Format = frameGenSlotPrivateFormat(d.Format);
      d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      d.MiscFlags = 0;
      d.CPUAccessFlags = 0;
      d.Usage = D3D11_USAGE_DEFAULT;
      hr = consumer->CreateTexture2D(&d, nullptr, &copy);
    }
    if (hr != S_OK || !copy) return hr == S_OK ? E_FAIL : hr;
    consumer->GetImmediateContext(&context_);
    if (!context_) return E_NOINTERFACE;
    consumer_ = consumer;
    shared_ = shared;
    mutex_ = mutex;
    copy_ = copy;
    serial_ = serial;
    bytes_ = static_cast<uint64_t>(d.Width) * d.Height * frameGenSlotFormatBytes(static_cast<uint32_t>(d.Format));
    return S_OK;
  }

  uint64_t serial() const noexcept { return shared_ ? serial_ : 0; }
  bool pending() const noexcept { return pending_; }
  int64_t pendingSlotTime() const noexcept { return slotTime_; }
  uint64_t pendingSequence() const noexcept { return pending_ ? pendingSequence_ : 0; }   // the pair whose S waits to be taken (0: none)
  // The d3d11 half said this eye's S was produced (key 1) for the slot at slotTime.
  // Phase 2.3: sequence is the pair it was built for, which consumedSequence() says once it is taken.
  void markProduced(int64_t slotTime, uint64_t sequence = 0) noexcept {
    pending_ = shared_ != nullptr;
    slotTime_ = slotTime;
    pendingSequence_ = sequence;
  }

  // AcquireSync(1, timeoutMs), the copy to the private texture, ReleaseSync(0). S_OK: consumed. WAIT_TIMEOUT (as an HRESULT): not released yet, still pending. Anything else: the
  // transfer is let go (the next produced S opens it again).
  HRESULT consume(DWORD timeoutMs) noexcept {
    if (!pending_ || !mutex_) return E_UNEXPECTED;
    const HRESULT a = mutex_->AcquireSync(1, timeoutMs);
    if (a == static_cast<HRESULT>(WAIT_TIMEOUT)) return a;
    if (a != S_OK) {
      release();
      return FAILED(a) ? a : E_FAIL;   // WAIT_ABANDONED is a positive value
    }
    context_->CopyResource(copy_.Get(), shared_.Get());
    context_->Flush();
    const HRESULT r = mutex_->ReleaseSync(0);
    pending_ = false;
    consumedSequence_ = pendingSequence_;
    if (r != S_OK) {
      release();
      return FAILED(r) ? r : E_FAIL;
    }
    ++consumed_;
    return S_OK;
  }

  ID3D11Texture2D* privateTexture() const noexcept { return copy_.Get(); }
  uint64_t xrBytes() const noexcept { return copy_ ? bytes_ : 0; }
  uint64_t consumedCount() const noexcept { return consumed_; }
  // The pair whose S the private texture holds (0: none, or released).
  uint64_t consumedSequence() const noexcept { return copy_ ? consumedSequence_ : 0; }

  // Drops everything (the key went off, the runtime closes, or a fault). A handoff still pending is tried once at 0 ms first, so the key goes back to the game device when it can.
  void release() noexcept {
    if (pending_ && mutex_ && mutex_->AcquireSync(1, 0) == S_OK) mutex_->ReleaseSync(0);
    pending_ = false;
    slotTime_ = 0;
    pendingSequence_ = consumedSequence_ = 0;
    copy_.Reset();
    mutex_.Reset();
    shared_.Reset();
    context_.Reset();
    consumer_.Reset();
    serial_ = 0;
    bytes_ = 0;
  }

 private:
  Microsoft::WRL::ComPtr<ID3D11Device> consumer_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> shared_, copy_;
  Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex_;
  uint64_t serial_ = 0, bytes_ = 0, consumed_ = 0;
  bool pending_ = false;
  int64_t slotTime_ = 0;
  uint64_t pendingSequence_ = 0, consumedSequence_ = 0;
};

}  // namespace edvr::openxr

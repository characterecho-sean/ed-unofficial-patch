#pragma once

#include "../common/native_reflex_frame.h"
#include <mutex>

namespace edvr::openxr {
// Optional CPU-only capability. Each call borrows the provider under this
// mutex, so close cannot retire it during a game-thread receipt. No lock spans
// runtime dispatch, original game work, a context call, or a driver's Sleep.
class NativeReflexFrameClient final {
 public:
  HRESULT acquire(HMODULE provider, ID3D11Device* device, uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (table_.context) return E_PENDING;
    if (!provider || !device || !generation) return E_INVALIDARG;
    const auto entry = GetProcAddress(provider, "edvrAcquireNativeReflexFrame");
    if (!owned(provider, entry)) return E_NOINTERFACE;
    EdvrNativeReflexFrameRequest request{sizeof(request), EDVR_NATIVE_REFLEX_FRAME_VERSION_1, device, generation};
    EdvrNativeReflexFrameTable candidate{sizeof(candidate), EDVR_NATIVE_REFLEX_FRAME_VERSION_1};
    const auto result = reinterpret_cast<decltype(&edvrAcquireNativeReflexFrame)>(entry)(&request, &candidate);
    if (result != S_OK) return FAILED(result) ? result : E_NOINTERFACE;
    if (candidate.size != sizeof(candidate) || candidate.version != EDVR_NATIVE_REFLEX_FRAME_VERSION_1 ||
        !candidate.context || !owned(provider, candidate.poseConsumed) ||
        !owned(provider, candidate.eyeSource) || !owned(provider, candidate.eyeSubmitted) || !owned(provider, candidate.close)) {
      if (candidate.context && owned(provider, candidate.close)) candidate.close(candidate.context);
      return E_NOINTERFACE;
    }
    table_ = candidate; generation_ = generation; return S_OK;
  }
  uint64_t poseConsumed(uint64_t sequence) {
    std::lock_guard<std::mutex> lock(mutex_);
    return table_.context && sequence ? table_.poseConsumed(table_.context, sequence) : 0;
  }
  bool eyeSource(ID3D11Texture2D* texture, unsigned eye, uint64_t sequence) {
    std::lock_guard<std::mutex> lock(mutex_);
    native_reflex::clearSource(texture);
    if (!table_.context || !texture || eye > 1 || !sequence) return false;
    EdvrNativeReflexSource source{};
    const auto result = table_.eyeSource(table_.context, texture, eye, sequence, &source);
    EdvrNativeReflexSource stamped{};
    const bool valid = result == S_OK && native_reflex::validSource(source) &&
        source.generation == generation_ && source.poseSequence == sequence &&
        native_reflex::readSource(texture, stamped) && native_reflex::sameSource(stamped, source);
    if (!valid) native_reflex::clearSource(texture);
    return valid;
  }
  void eyeSubmitted(unsigned eye, uint64_t sequence, bool accepted) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (table_.context && eye < 2 && sequence)
      table_.eyeSubmitted(table_.context,eye,sequence,accepted?1u:0u);
  }
  HRESULT close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!table_.context) return S_FALSE;
    const auto result = table_.close(table_.context);
    if (SUCCEEDED(result)) { table_ = {}; generation_ = 0; }
    return result;
  }
 private:
  template<class T> static bool owned(HMODULE provider, T address) {
    if (!address) return false;
    HMODULE implementation = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address), &implementation) && implementation == provider;
  }
  std::mutex mutex_;
  EdvrNativeReflexFrameTable table_{};
  uint64_t generation_ = 0;
};
} // namespace edvr::openxr

// Flat DXGI frame generation (fix.frame_generation). NVIDIA and AMD, through AMD's signed
// frame-generation DLL and a null swap chain. A VR session does not start it. The presenting chain
// is ours; AMD's frame-generation swap chain is never created.
#pragma once

#include <windows.h>
#include <dxgi.h>
#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace edvr {

void frameGenStartup();
void frameGenShutdown();
void frameGenOnDeviceLost();

// XeSS took Elite's window. One stand-down line when this key asked to run.
void frameGenNoteXessStandDown();

// Swap-chain create. Null when this session must not hijack the window: the caller creates the
// chain the usual way. On success the hidden window is Elite's, and finish creates our presenter.
HWND frameGenBeginCreate(IUnknown* device, HWND realWindow, UINT* width, UINT* height, UINT format,
                         bool fullscreen);
bool frameGenFinishCreate(IDXGISwapChain* hidden, HWND realWindow);
void frameGenAbandon();
bool frameGenBridged();
bool frameGenOwns(const void* swapChain);
HRESULT frameGenPresent(IDXGISwapChain* hidden, UINT syncInterval, UINT flags);

// The hidden chain's resize and the window queries Elite sends it. XeSS is asked first.
void frameGenBeforeResize(UINT* width, UINT* height);
HRESULT frameGenAfterResize(HRESULT hiddenResult);
HWND frameGenWindow();
HRESULT frameGenSetFullscreenState(BOOL fullscreen, IDXGIOutput* target);
HRESULT frameGenGetFullscreenState(BOOL* fullscreen, IDXGIOutput** target);
HRESULT frameGenResizeTarget(const DXGI_MODE_DESC* mode);
HRESULT frameGenGetContainingOutput(IDXGIOutput** output);
HRESULT frameGenGetFrameStatistics(DXGI_FRAME_STATISTICS* stats);
HRESULT frameGenGetLastPresentCount(UINT* count);
// Elite is making another swap chain on the bridged window.
void frameGenRelease();

// The resolver's HUD-less output, copied before RCAS. Pointers are not referenced and are only
// valid for this call. nearPlane is the projection's B term (0 when it was not read).
void frameGenNoteResolved(ID3D11DeviceContext* ctx, ID3D11Texture2D* colour, ID3D11Texture2D* depth,
                          ID3D11Texture2D* motion, uint32_t renderW, uint32_t renderH, uint32_t outW,
                          uint32_t outH, float deltaMs, float jitterX, float jitterY, float nearPlane,
                          bool hdr, bool reset, uint64_t frame);

}  // namespace edvr

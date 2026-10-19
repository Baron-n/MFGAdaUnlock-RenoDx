// SPDX-License-Identifier: MIT
#pragma once
#include "native_bridge.hpp"
#include "vendor/port/port_support.h"
#include "vendor/overlay_native.h"
#include <xefg_swapchain_d3d12.h>
#include <xell_d3d12.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <atomic>
#include <map>
#include <memory>
#include <optional>

namespace mfgunlock::xemfg::native {
struct Api {
#define F(n) decltype(&n) n{};
  F(xefgSwapChainD3D12CreateContext) F(xefgSwapChainD3D12InitFromSwapChainDesc)
  F(xefgSwapChainD3D12GetSwapChainPtr) F(xefgSwapChainD3D12TagFrameResource)
  F(xefgSwapChainTagFrameConstants) F(xefgSwapChainSetPresentId)
  F(xefgSwapChainSetEnabled) F(xefgSwapChainSetNumInterpolatedFrames)
  F(xefgSwapChainSetLatencyReduction) F(xefgSwapChainGetLastPresentStatus)
  F(xefgSwapChainDestroy) F(xefgSwapChainSetUiCompositionState)
  F(xefgSwapChainSetLoggingCallback)
  F(xellD3D12CreateContext) F(xellSetSleepMode) F(xellSleep)
  F(xellAddMarkerData) F(xellDestroyContext)
#undef F
  bool Load();
};
struct Frame {
  uint32_t id{}, viewport{}, resources{}, markers{};
  bool constants{}, valid{}, sleeping{}, failed{};
  uint32_t motionWidth{}, motionHeight{};
  xefg_swapchain_frame_constant_data_t data{};
};
struct Output {
  std::recursive_mutex io;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  xefg_swapchain_handle_t fg{};
  xell_context_handle_t ll{};
  std::map<uint32_t,Frame> frames;
  uint32_t last{}, appliedCount{}, width{}, height{};
  uint32_t lastAdmission{UINT32_MAX};
  uint64_t admissionTick{};
  bool invert{}, jitter{}, enabled{}, compatible{true};
  ~Output();
};
struct Bridge {
  std::recursive_mutex lock;
  Api api;
  Status status;
  std::weak_ptr<Output> output;
  std::atomic<uint32_t> recording{0}, presenting{0}, viewport{UINT32_MAX};
  std::atomic<bool> nativeOff{false}, reflexOff{false}, requested{false};
  std::atomic<uint32_t> multiplier{4}, limitUs{0};
  std::atomic<uint64_t> outputPresents{0};
  bool lowLatency{true}, invert{true}, jitter{false};
};
Bridge& State();
void Report(const char* reason);
bool Hook(void* entry,void* replacement,void** trampoline);
void DiscoverStreamline();
std::shared_ptr<Output> Active();
void BeforePresent(const std::shared_ptr<Output>& o);
void AfterPresent(const std::shared_ptr<Output>& o);
HRESULT CreateOutput(IDXGIFactory2*,IUnknown*,HWND,const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,IDXGIOutput*,IDXGISwapChain1**);
IDXGISwapChain1* Wrap(ComPtr<IDXGISwapChain4> chain,std::shared_ptr<Output> owner);
// Token zero maps to one. Used consistently by inputs, markers and Present.
inline uint32_t FrameId(uint32_t token) { return token==UINT32_MAX?0:token+1; }
}

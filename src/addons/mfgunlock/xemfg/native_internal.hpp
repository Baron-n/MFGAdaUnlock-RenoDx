// SPDX-License-Identifier: MIT
#pragma once
#include "native_bridge.hpp"
#include "native_controls.hpp"
#include "frame_store.hpp"
#include "native_diagnostics.hpp"
#include "native_trace.hpp"
#include "hud_inputs.hpp"
#include "native_reframework.hpp"
#include "vendor/port/port_support.h"
#include "vendor/overlay_native.h"
#include <xefg_swapchain_d3d12.h>
#include <xell_d3d12.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <atomic>
#include <memory>
#include <optional>

namespace mfgunlock::xemfg::native {
struct Api {
  HMODULE fgModule{},llModule{};
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
  uint64_t sleepUs{};
  uint32_t id{}, viewport{}, resources{}, markers{};
  bool constants{}, valid{}, sleeping{}, sleepInFlight{}, failed{};
  uint32_t motionWidth{}, motionHeight{};
  uint32_t hudIssues{};
  HudTagIdentity hudTags[2]{};
  // Pin the allocation identity until frame retirement, never infer its
  // generation from a reused address. Contents still obey the SDK lifecycle.
  ComPtr<IUnknown> hudAllocations[2];
  uint32_t constantIssues{};
  LatencyOutput latencyOutput{LatencyOutput::XeLL};
  xefg_swapchain_frame_constant_data_t data{};
};
struct Output {
  HWND hwnd{};
  std::recursive_mutex io;
  std::recursive_mutex presents;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<IDXGIFactory2> factory;
  std::shared_ptr<WindowControl> window;
  std::unique_ptr<QueueTrace> gpuTrace;
  DWORD windowBindError{};
  xefg_swapchain_handle_t fg{};
  xell_context_handle_t ll{};
  FrameStore<Frame> frames;
  uint32_t last{}, appliedCount{}, width{}, height{};
  uint32_t lastAdmission{UINT32_MAX};
  DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
  uint32_t hudLogBudget{},hudKindsSeen{};
  uint32_t rejectedTagLogBudget{};
  uint32_t constantLogBudget{},lastConstantIssues{UINT32_MAX};
  uint32_t bypassPresentLogBudget{};
  uint64_t admissionTick{};
  uint64_t epoch{};
  uint64_t diagnosticOutput{};
  uint32_t sleepInterval{};
  bool sleepConfigured{}, sleepActive{},sleepLowLatency{true};
  // XeFG initializes with composition disabled. A live preference is applied
  // only at the source Present boundary, with this frame's HUD-less input.
  bool uiCompositionKnown{true}, uiCompositionApplied{}, uiCompositionAvailable{};
  int uiCompositionResult{};
  uint64_t uiCompositionUpdates{};
  bool invert{}, jitter{}, enabled{}, compatible{true},retired{};
  std::shared_ptr<reframework::Peer> reframeworkPeer;
  ComPtr<IDXGISwapChain4> quarantinedProxy;
  // The application proxy owns presentation. Shared operation leases may keep
  // XeLL alive after this method has released the window/Intel FG context.
  void RetirePresentation(ComPtr<IDXGISwapChain4> publicProxy={});
  ~Output();
};
struct Bridge {
  std::recursive_mutex lock;
  Api api;
  Status status;
  std::weak_ptr<Output> output;
  std::atomic<uint32_t> recording{0}, presenting{0}, viewport{UINT32_MAX};
  std::atomic<bool> nativeOff{false}, reflexOff{false}, requested{false};
  std::atomic<bool> retirementFailed{false};
  std::atomic<bool> reflexOptionsReady{false};
  // Output changes are saved by the UI and take effect only after restart.
  // This avoids replaying a game's Reflex setter on an arbitrary UI thread.
  std::atomic<LatencyOutput> latencyOutput{LatencyOutput::XeLL};
  std::atomic<uint64_t> reflexOutputTick{0},reflexSleeps{0},xellSleeps{0};
  std::atomic<uint32_t> multiplier{4}, limitUs{0};
  std::atomic<uint64_t> outputPresents{0};
  std::atomic<uint64_t> latencySleeps{0}, latencyMarkers{0}, inputTick{0}, outputTick{0};
  std::atomic<uint64_t> latencyModeUpdates{0}, lastSleepUs{0};
  std::atomic<uint32_t> gameReflexMode{0};
  std::atomic<uint32_t> lastHudIssues{0},lastHudInputs{0};
  std::atomic<uint64_t> hudRejected{0};
  bool lowLatency{true}, invert{true}, jitter{false};
};
Bridge& State();
inline Frame* FindFrame(Output& o,uint32_t id) {
  return o.frames.Acquire(id);
}
sl::Result SleepSourceFrame(const std::shared_ptr<Output>& output,uint32_t id,
    PFun_slReflexSleep* nativeSleep=nullptr,const sl::FrameToken* token=nullptr);
bool ForwardSourceMarker(const std::shared_ptr<Output>& output,uint32_t id,uint32_t marker);
bool ApplyUiComposition(Output& output,bool hudlessAvailable);
void Report(const char* reason);
bool Hook(void* entry,void* replacement,void** trampoline);
void DiscoverStreamline();
std::shared_ptr<Output> Active();
void BeforePresent(const std::shared_ptr<Output>& o);
void AfterPresent(const std::shared_ptr<Output>& o,CpuProbe* probe=nullptr,HRESULT result=S_OK);
HRESULT CreateOutput(IDXGIFactory2*,IUnknown*,HWND,const DXGI_SWAP_CHAIN_DESC1*,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,IDXGIOutput*,IDXGISwapChain1**);
IDXGISwapChain1* Wrap(ComPtr<IDXGISwapChain4> chain,std::shared_ptr<Output> owner);
// Token zero maps to one. Used consistently by inputs, markers and Present.
inline uint32_t FrameId(uint32_t token) { return token==UINT32_MAX?0:token+1; }
}

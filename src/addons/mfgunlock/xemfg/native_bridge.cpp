// SPDX-License-Identifier: MIT
// Independent Streamline/DXGI transport. No OptiScaler source is used here.
#include "native_internal.hpp"
#include "native_overlay.hpp"
#include "native_reflex_profile.hpp"
#include "backend_api.hpp"
#include "../backend_selection.hpp"
#include "vendor/third_party/minhook/include/MinHook.h"
#include <deps/imgui/imgui.h>
#include <reshade.hpp>
#include <set>

namespace mfgunlock::xemfg::native {
Bridge& State() { static auto* s=new Bridge; return *s; }
Status Snapshot() { auto& s=State(); std::lock_guard lock(s.lock); auto status=s.status; status.outputPresents=s.outputPresents; return status; }
void Report(const char* reason) {
  auto& s=State(); std::lock_guard lock(s.lock);
  if(strcmp(s.status.detail,reason)==0) return;
  strncpy_s(s.status.detail,reason,_TRUNCATE);
  xess_mod::WriteLog("native_bridge","\"detail\":\""+escape(reason)+"\"");
}
std::shared_ptr<Output> Active() {
  auto& s=State(); std::lock_guard lock(s.lock); return s.output.lock();
}
bool Hook(void* entry,void* replacement,void** original) {
  static std::recursive_mutex lock; std::lock_guard guard(lock);
  static std::set<void*> installed;
  if(!entry) return false;
  if(installed.contains(entry)) return *original!=nullptr;
  const auto init=MH_Initialize();
  if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) return false;
  if(MH_CreateHook(entry,replacement,original)!=MH_OK) return false;
  if(MH_EnableHook(entry)!=MH_OK) { MH_RemoveHook(entry); *original=nullptr; return false; }
  installed.insert(entry); return true;
}
bool Api::Load() {
  if(!EnsureModel()) return false;
  NativeRuntimePair selected;
  if(!NativeRuntimeModules(selected)) return false;
  auto fg=static_cast<HMODULE>(selected.frameGeneration),ll=static_cast<HMODULE>(selected.latency);
  fgModule=fg; llModule=ll;
#define F(n,m) n=reinterpret_cast<decltype(n)>(GetProcAddress(m,#n)); if(!n) return false;
  F(xefgSwapChainD3D12CreateContext,fg) F(xefgSwapChainD3D12InitFromSwapChainDesc,fg)
  F(xefgSwapChainD3D12GetSwapChainPtr,fg) F(xefgSwapChainD3D12TagFrameResource,fg)
  F(xefgSwapChainTagFrameConstants,fg) F(xefgSwapChainSetPresentId,fg)
  F(xefgSwapChainSetEnabled,fg) F(xefgSwapChainSetNumInterpolatedFrames,fg)
  F(xefgSwapChainSetLatencyReduction,fg) F(xefgSwapChainGetLastPresentStatus,fg)
  F(xefgSwapChainDestroy,fg) F(xefgSwapChainSetUiCompositionState,fg)
  F(xefgSwapChainSetLoggingCallback,fg)
  F(xellD3D12CreateContext,ll) F(xellSetSleepMode,ll) F(xellSleep,ll)
  F(xellAddMarkerData,ll) F(xellDestroyContext,ll)
#undef F
  return true;
}
Output::~Output() {
  // Only the outer application's COM lifetime owns this object. The SDK joins
  // its workers before the XeLL context and queue can be destroyed.
  auto& a=State().api;
  if(window) window->Retire();
  if(fg) {
    a.xefgSwapChainSetEnabled(fg,0);
    const auto result=a.xefgSwapChainDestroy(fg);
    if(result!=0) {
      // Preserve every dependency if the SDK refuses retirement. Destruction
      // must not convert a leaked SDK reference into a freed XeLL/device.
      struct Pending { ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue; xell_context_handle_t ll; };
      new Pending{device,queue,ll};
      xess_mod::WriteLog("native_retirement_pending","\"result\":"+std::to_string(result));
      return;
    }
  }
  if(ll) a.xellDestroyContext(ll);
}
HRESULT CreateOutput(IDXGIFactory2* factory,IUnknown* source,HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full,
    IDXGIOutput* restrictOutput,IDXGISwapChain1** result) {
  if(!result || !desc || !hwnd || restrictOutput) return E_NOINTERFACE;
  const bool forceBorderless=ControlsSettings().forceBorderless;
  if(full && !full->Windowed && !forceBorderless) {
    Report("XeMFG requires windowed/borderless mode; enable Force Borderless before restarting");
    return E_NOINTERFACE;
  }
  auto windowed=full?*full:DXGI_SWAP_CHAIN_FULLSCREEN_DESC{};
  if(forceBorderless) windowed.Windowed=TRUE;
  if(desc->Format!=DXGI_FORMAT_R10G10B10A2_UNORM && desc->Format!=DXGI_FORMAT_R8G8B8A8_UNORM
      && desc->Format!=DXGI_FORMAT_B8G8R8A8_UNORM) {
    Report("XeMFG: unsupported swapchain format (FP16/scRGB requires a separate conversion path)");
    return E_NOINTERFACE;
  }
  if(Active()) return E_NOINTERFACE; // One selected Streamline viewport/output in this implementation.
  if(!OwnsPresentationHooks()) {
    Report("XeMFG needs early Streamline initialization to own presentation; restart with the addon loaded before slInit");
    return E_NOINTERFACE;
  }
  ComPtr<ID3D12CommandQueue> queue;
  if(!source || FAILED(source->QueryInterface(IID_PPV_ARGS(&queue)))) return E_NOINTERFACE;
  if(queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT) return E_NOINTERFACE;
  auto& s=State();
  if(!s.api.Load()) { Report("XeMFG provider pair or model hooks unavailable"); return E_NOINTERFACE; }
  auto o=std::make_shared<Output>(); o->queue=queue;
  static std::atomic<uint64_t> outputGeneration{0}; o->diagnosticOutput=++outputGeneration;
  ComPtr<ID3D12Device> device;
  if(FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))
      || !single_overlay::native::Unwrap(device.Get(),o->device)) return E_NOINTERFACE;
  o->invert=s.invert; o->jitter=s.jitter; o->width=desc->Width; o->height=desc->Height; o->format=desc->Format;
  auto& a=s.api;
  if(a.xellD3D12CreateContext(o->device.Get(),&o->ll)!=XELL_RESULT_SUCCESS) {
    Report("XeLL D3D12 context creation failed"); return E_NOINTERFACE;
  }
  xell_sleep_params_t sleep{}; sleep.bLowLatencyMode=s.latencyOutput==LatencyOutput::Reflex?0:1;
  if(a.xellSetSleepMode(o->ll,&sleep)!=XELL_RESULT_SUCCESS) return E_NOINTERFACE;
  o->sleepConfigured=true; o->sleepLowLatency=sleep.bLowLatencyMode!=0; ++s.latencyModeUpdates;
  if(a.xefgSwapChainD3D12CreateContext(o->device.Get(),&o->fg)!=0) return E_NOINTERFACE;
  a.xefgSwapChainSetLoggingCallback(o->fg,XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING,
      [](const char* message,xefg_swapchain_logging_level_t level,void*) {
        static std::atomic<uint32_t> messages{0};
        if(messages.fetch_add(1)<100) xess_mod::WriteLog("native_sdk_log","\"level\":"+std::to_string(level)+",\"message\":\""+escape(message?message:"")+"\"");
      },nullptr);
  const bool nativeReflex=s.latencyOutput==LatencyOutput::Reflex;
  if(nativeReflex && !reflex_profile::Available(a.fgModule)) {
    Report("Native Reflex output unavailable for this XeFG profile; select XeLL and restart");
    return E_NOINTERFACE;
  }
  const auto latencyResult=a.xefgSwapChainSetLatencyReduction(o->fg,
      nativeReflex?reflex_profile::ExternalSelector():static_cast<void*>(o->ll));
  if(latencyResult!=0) return E_NOINTERFACE;
  xess_mod::WriteLog("native_latency_binding",std::string("\"output\":\"")+(nativeReflex?"Reflex":"XeLL")+
      "\",\"external_profile\":"+(nativeReflex?"true":"false")+",\"result\":"+std::to_string(latencyResult));
  xefg_swapchain_d3d12_init_params_t init{};
  init.maxInterpolatedFrames=5;
  init.initFlags=(o->invert?XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH:0)
      |(o->jitter?XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV:0);
  init.uiMode=XEFG_SWAPCHAIN_UI_MODE_AUTO;
  const auto code=a.xefgSwapChainD3D12InitFromSwapChainDesc(o->fg,hwnd,desc,forceBorderless?&windowed:full,queue.Get(),factory,&init);
  if(code!=0) {
    xess_mod::WriteLog("native_swapchain_init_failed","\"result\":"+std::to_string(code));
    Report("XeFG swapchain initialization failed; inspect the XeMFG log"); return E_NOINTERFACE;
  }
  ComPtr<IDXGISwapChain4> chain;
  if(a.xefgSwapChainD3D12GetSwapChainPtr(o->fg,IID_PPV_ARGS(&chain))!=0) return E_FAIL;
  DXGI_SWAP_CHAIN_DESC1 actual{};
  if(SUCCEEDED(chain->GetDesc1(&actual))) { o->width=actual.Width; o->height=actual.Height; o->format=actual.Format; }
  using Debug=xefg_swapchain_result_t(*)(xefg_swapchain_handle_t,uint32_t,uint32_t,void*);
  if(auto debug=reinterpret_cast<Debug>(GetProcAddress(a.fgModule,"xefgSwapChainEnableDebugFeature")))
    debug(o->fg,0,0,nullptr);
  // Keep Intel's default (composition disabled). The user's opt-in preference
  // is consumed at Present, after the matching frame's inputs are known.
  a.xefgSwapChainSetEnabled(o->fg,0);
  SetLastError(0); o->window=WindowControl::Bind(hwnd,forceBorderless);
  if(!o->window) o->windowBindError=GetLastError();
  xess_mod::WriteLog("native_window_bind","\"hwnd\":"+std::to_string(uintptr_t(hwnd))
      +",\"thread\":"+std::to_string(GetCurrentThreadId())+",\"available\":"+(o->window?"true":"false")
      +",\"error\":"+std::to_string(o->windowBindError));
  *result=Wrap(std::move(chain),o);
  { std::lock_guard guard(s.lock); s.output=o; s.status.output=true; s.status.xell=true; }
  Report("XeMFG swapchain ready; waiting for Streamline inputs and XeLL markers");
  return S_OK;
}

using FactoryCreate=HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*,IUnknown*,HWND,
    const DXGI_SWAP_CHAIN_DESC1*,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,IDXGIOutput*,IDXGISwapChain1**);
static FactoryCreate originalFactory{};
static thread_local bool internalCreate{};
static thread_local HWND creatingWindow{};
static HRESULT STDMETHODCALLTYPE FactoryHook(IDXGIFactory2* f,IUnknown* d,HWND w,
    const DXGI_SWAP_CHAIN_DESC1* desc,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full,
    IDXGIOutput* restrictOutput,IDXGISwapChain1** out) {
  if(internalCreate) {
    const auto hr=originalFactory(f,d,w,desc,full,restrictOutput,out);
    if(SUCCEEDED(hr) && creatingWindow==w) WrapFinalOutput(d,w,out);
    return hr;
  }
  struct Scope { Scope(HWND w){internalCreate=true;creatingWindow=w;} ~Scope(){internalCreate=false;creatingWindow=nullptr;} } scope(w);
  DiscoverStreamline();
  const auto hr=CreateOutput(f,d,w,desc,full,restrictOutput,out);
  if(hr!=E_NOINTERFACE) return hr;
  // Unsupported integration remains native with our NVIDIA requests forced off.
  // It is explicitly reported; never announce an active XeMFG output here.
  return originalFactory(f,d,w,desc,full,restrictOutput,out);
}
bool Install() {
  if(single_overlay::native::InsideLoader() || StreamlineStopping()) return false;
  DiscoverStreamline();
  static std::mutex installLock; std::lock_guard guard(installLock);
  if(originalFactory) return true;
  ComPtr<IDXGIFactory2> factory;
  if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
  auto** table=*reinterpret_cast<void***>(factory.Get());
  if(!single_overlay::native::PinInterface(factory.Get(),15)
      || !Hook(table[15],reinterpret_cast<void*>(&FactoryHook),reinterpret_cast<void**>(&originalFactory))) {
    Report("XeMFG factory hook unavailable; restart with early ReShade addon loading"); return false;
  }
  std::lock_guard lock(State().lock); State().status.factoryHook=true; return true;
}
static void DeviceEvent(reshade::api::device* device) {
  if(device->get_api()==reshade::api::device_api::d3d12) Install();
}
static void OutputPresent(reshade::api::command_queue*,reshade::api::swapchain*,const reshade::api::rect*,
    const reshade::api::rect*,uint32_t,const reshade::api::rect*) {
  if(Active()) ++State().outputPresents;
}
static DWORD WINAPI DiscoverWorker(void*) {
  for(unsigned i=0;i<80 && !StreamlineStopping();++i) { DiscoverStreamline(); Install(); Sleep(250); }
  return 0;
}
void Attach(HMODULE module) {
  CaptureStreamlineStartup();
  auto& s=State(); auto ini=backend::Directory()/L"MFGAdaUnlock.ini";
  s.multiplier=std::clamp(GetPrivateProfileIntW(L"XeFG",L"Multiplier",4,ini.c_str()),2u,6u);
  s.invert=GetPrivateProfileIntW(L"XeFG",L"InvertedDepth",1,ini.c_str())!=0;
  s.jitter=GetPrivateProfileIntW(L"XeFG",L"JitteredMotion",0,ini.c_str())!=0;
  LoadControls();
  SetTimingEnabled(GetPrivateProfileIntW(L"Diagnostics",L"CpuTiming",0,ini.c_str())!=0);
  AttachModel(module);
  reshade::register_event<reshade::addon_event::init_device>(&DeviceEvent);
  reshade::register_event<reshade::addon_event::present>(&OutputPresent);
  // Bounded discovery only; the resolver catches feature queries after this
  // bootstrap window. No module scan is added to generated-frame Present.
  HMODULE pin{};
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
      reinterpret_cast<LPCWSTR>(module),&pin);
  if(auto worker=CreateThread(nullptr,0,&DiscoverWorker,nullptr,0,nullptr)) CloseHandle(worker);
}

}
extern "C" __declspec(dllexport) bool MFGAXeNativeStatus(mfgunlock::xemfg::native::Status* out,uint32_t size) {
  if(!out || size!=sizeof(*out)) return false;
  *out=mfgunlock::xemfg::native::Snapshot(); return true;
}
extern "C" __declspec(dllexport) void MFGAXeNativeRefresh() {
  if(auto o=mfgunlock::xemfg::native::Active()) mfgunlock::xemfg::native::AfterPresent(o);
}

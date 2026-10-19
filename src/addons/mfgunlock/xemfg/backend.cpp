// SPDX-License-Identifier: MIT
// OptiScaler transport -> Intel XeSS-FG model port. The transport owns input
// resources, XeLL markers/sleep and its UI. This companion owns only the port,
// count admission and the extra-output pacing for the admitted provider pair.
#include "../backend_selection.hpp"
#include "backend_api.hpp"
#include "vendor/port/port_support.h"
#include "vendor/port/model_port.h"
#include "vendor/port/shader_port.h"
#include "vendor/host_unlock.h"
#include "vendor/sdk_pacing.h"
#include "vendor/cuda/cuda_route.h"
#include "vendor/adapter_discovery.h"
#include "embedded_xefg_cuda.generated.h"
#include "vendor/third_party/minhook/include/MinHook.h"
#include <xefg_swapchain_d3d12.h>
#include <xell_d3d12.h>
#include <psapi.h>
#include <intrin.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace {
using Handle = xefg_swapchain_handle_t;
using Result = xefg_swapchain_result_t;
constexpr auto success = XEFG_SWAPCHAIN_RESULT_SUCCESS;
constexpr auto unsupported = XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED;
HMODULE self{}, host{}, fg{}, ll{};
std::recursive_mutex mutex;
std::mutex sinkMutex;
std::once_flag initializeOnce;
std::once_flag startupOnce;
HMODULE bootstrapFg{};
bool bootstrapCreate{};
bool InsideLoader() noexcept {
  using Query = BOOLEAN (NTAPI*)();
  const auto query = reinterpret_cast<Query>(GetProcAddress(
      GetModuleHandleW(L"ntdll.dll"), "RtlIsThreadWithinLoaderCallout"));
  return !query || query(); // Unknown context never authorizes SDK entry calls.
}
void CompleteStartup() noexcept;
struct Context {
  ComPtr<ID3D12Device> device;
  uint64_t generation{};
  bool initialized{}, enabled{};
  uint32_t generated{};
};
struct State {
  mfgunlock::xemfg::Status status;
  std::map<Handle, Context> contexts;
  std::set<Handle> nativeContexts;
  std::unique_ptr<ShaderPort> shaders;
  std::unique_ptr<ModelPort> model;
  std::unique_ptr<xess_host_unlock::Unlock> unlock;
  bool attempted{}, deferred{}, rejected{};
  uint64_t generation{};
};
// Once hooks exist, callbacks, payloads and provider stay alive until exit.
State& Get() { static State* state = new State; return *state; }
void Detail(const std::string& value) {
  auto& s = Get().status;
  strncpy_s(s.detail, value.c_str(), _TRUNCATE);
}
std::filesystem::path ModulePath(HMODULE module) {
  std::wstring p(32768,L'\0');
  const auto n=GetModuleFileNameW(module,p.data(),DWORD(p.size()));
  if (!n || n>=p.size()) throw std::runtime_error("module_path");
  p.resize(n); return p;
}
std::string HashFile(const std::filesystem::path& path) {
  std::ifstream f(path,std::ios::binary);
  if (!f) throw std::runtime_error("hash_file_open");
  sha256_util::Hasher h; std::array<char,65536> b{};
  while (f) { f.read(b.data(),b.size()); if (f.gcount() && !h.Update(b.data(),size_t(f.gcount()))) throw std::runtime_error("hash_update"); }
  sha256_util::Digest d{};
  if (f.bad() || !h.Finish(d)) throw std::runtime_error("hash_finish");
  return sha256_util::LowerHex(d);
}
bool HostAddress(const void* p) noexcept {
  MEMORY_BASIC_INFORMATION m{};
  return host && p && VirtualQuery(p,&m,sizeof(m))==sizeof(m)
      && m.AllocationBase==host && m.Type==MEM_IMAGE;
}
bool SdkAddress(const void* p) noexcept {
  MEMORY_BASIC_INFORMATION m{};
  return p && VirtualQuery(p,&m,sizeof(m))==sizeof(m) && m.Type==MEM_IMAGE
      && (m.AllocationBase==fg || m.AllocationBase==ll);
}
std::string CallerName(const void* p) noexcept {
  try {
    MEMORY_BASIC_INFORMATION m{};
    if(!p || VirtualQuery(p,&m,sizeof(m))!=sizeof(m) || m.Type!=MEM_IMAGE) return "unknown";
    return ModulePath(static_cast<HMODULE>(m.AllocationBase)).filename().string();
  } catch(...) { return "unknown"; }
}
// Respect XeLL entry redirects that belong to OptiScaler; no second sleep hook.
bool HostRedirect(const void* entry) noexcept {
  auto* p=static_cast<const unsigned char*>(entry);
  for (unsigned i=0; p && i<4; ++i) {
    unsigned char bytes[6]{}; SIZE_T n{};
    if (!ReadProcessMemory(GetCurrentProcess(),p,bytes,6,&n) || n!=6) return false;
    int32_t rel{};
    if (bytes[0]==0xe9) { memcpy(&rel,bytes+1,4); p+=5+rel; }
    else if (bytes[0]==0xff && bytes[1]==0x25) {
      memcpy(&rel,bytes+2,4); uintptr_t target{};
      if (!ReadProcessMemory(GetCurrentProcess(),p+6+rel,&target,8,&n) || n!=8) return false;
      p=reinterpret_cast<const unsigned char*>(target);
    } else return false;
    if (HostAddress(p)) return true;
  }
  return false;
}
bool IsOptiScaler10(HMODULE module) {
  auto path=ModulePath(module); DWORD unused{};
  const auto bytes=GetFileVersionInfoSizeW(path.c_str(),&unused);
  if (!bytes || bytes>1'000'000) return false;
  std::vector<unsigned char> info(bytes);
  if (!GetFileVersionInfoW(path.c_str(),0,bytes,info.data())) return false;
  VS_FIXEDFILEINFO* fixed{}; UINT n{};
  if (!VerQueryValueW(info.data(),L"\\",reinterpret_cast<void**>(&fixed),&n)
      || n<sizeof(*fixed) || HIWORD(fixed->dwFileVersionMS)!=10) return false;
  struct Translation { WORD language, codepage; }* translations{};
  if (!VerQueryValueW(info.data(),L"\\VarFileInfo\\Translation",reinterpret_cast<void**>(&translations),&n)) return false;
  for (unsigned i=0;i<n/sizeof(Translation);++i) {
    wchar_t key[96]{}; swprintf_s(key,L"\\StringFileInfo\\%04x%04x\\ProductName",translations[i].language,translations[i].codepage);
    wchar_t* product{}; UINT size{};
    if (VerQueryValueW(info.data(),key,reinterpret_cast<void**>(&product),&size)
        && size && wcscmp(product,L"OptiScaler")==0) return true;
  }
  return false;
}
uint32_t Vendor(ID3D12Device* device) {
  if (!device) return 0;
  ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))
      || FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(),IID_PPV_ARGS(&adapter)))) return 0;
  DXGI_ADAPTER_DESC1 desc{};
  return SUCCEEDED(adapter->GetDesc1(&desc)) ? desc.VendorId : 0;
}
void Pin(HMODULE module) {
  HMODULE pinned{};
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
      reinterpret_cast<LPCWSTR>(module),&pinned) || pinned!=module) throw std::runtime_error("module_pin");
}
void Prepare(ID3D12Device* device) {
  auto& s=Get(); if (s.attempted) return;
  // Native capability probes are often created and destroyed before the real
  // output context. Only live native contexts forbid process-wide publication.
  // Include host contexts created during a deferred attempt in this lifetime.
  if(!s.nativeContexts.empty()) {
    Detail("Native XeFG 2x: waiting for earlier native contexts to retire");
    if(!s.deferred) xess_mod::WriteLog("port_deferred","\"reason\":\"native_contexts_live\",\"contexts\":"+std::to_string(s.nativeContexts.size()));
    s.deferred=true; return;
  }
  s.attempted=true; s.deferred=false;
  try {
    if (Vendor(device)!=0x10de) throw std::runtime_error("first_build_targets_nvidia_only");
    if (!xess_layout::Pair(fg,ll)) throw std::runtime_error("provider_layout_rejected");
    const auto fatbin=xess_payload::Read(self,xefg_cuda::embedded::fatbin);
    // Publish only the true multi-frame model. Never repeat the stock 2x model
    // at the wrong phases as an alternative unlock.
    auto shaders=std::make_unique<ShaderPort>(fg,self);
    auto model=std::make_unique<ModelPort>(fg,true,true);
    auto unlock=std::make_unique<xess_host_unlock::Unlock>(fg,ll,5,false);
    s.shaders=std::move(shaders); s.model=std::move(model); s.unlock=std::move(unlock);
    s.status.model=1;
    xefg_cuda::SetLog(&xess_mod::WriteLog);
    std::string reason;
    xefg_cuda::Install(device,fg,fatbin,reason);
    Detail("Intel multi-frame model ready; OptiScaler owns inputs and XeLL");
  } catch (const std::exception& e) {
    s.rejected=true; Detail(std::string("Native XeFG 2x fallback: ")+e.what());
    xess_mod::WriteLog("port_rejected","\"reason\":\""+escape(e.what())+"\"");
  }
}
void ConfigureFlip() noexcept {
  static std::once_flag once;
  std::call_once(once,[] {
    try {
      const auto ini=ModulePath(host).parent_path()/L"OptiScaler.ini";
      wchar_t setting[32]{};
      GetPrivateProfileStringW(L"XeFG",L"XeFGFlipMetering",L"auto",setting,32,ini.c_str());
      const bool enabled=_wcsicmp(setting,L"off") && _wcsicmp(setting,L"false") && wcscmp(setting,L"0");
      if (!enabled) { sdk_pacing::SetFlipMetering(nullptr,false,0); return; }
      wchar_t system[1024]{};
      if (!GetSystemDirectoryW(system,1024)) throw std::runtime_error("system_directory");
      const auto dll=std::filesystem::path(system)/L"nvapi64.dll";
      const auto dispatcher=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!dispatcher || !adapter_discovery::NvapiOwnerModule(system,dispatcher)) throw std::runtime_error("NVAPI_owner");
      using Query=void*(__cdecl*)(uint32_t);
      const auto query=reinterpret_cast<Query>(GetProcAddress(dispatcher,"nvapi_QueryInterface"));
      if (!adapter_discovery::OwnedCode(dispatcher,reinterpret_cast<void*>(query))) throw std::runtime_error("NVAPI_query");
      auto resolve=[&](uint32_t id) {
        auto target=query(id); adapter_discovery::ModuleReference owner;
        if (!target || !owner.Acquire(target) || !adapter_discovery::NvapiOwnerModule(system,owner.module)
            || !adapter_discovery::OwnerFactoryMatches(owner.module,id,target)) throw std::runtime_error("NVAPI_function_owner");
        Pin(owner.module); return target;
      };
      auto init=reinterpret_cast<int(__cdecl*)()>(resolve(0x0150e828u));
      const auto set=reinterpret_cast<sdk_pacing::SetFlipConfigFunction>(resolve(0xf3148c42u));
      if (init()!=0) throw std::runtime_error("NVAPI_initialize");
      Pin(dispatcher);
      auto lead=GetPrivateProfileIntW(L"XeFG",L"XeFGFlipMeteringLeadUs",2500,ini.c_str());
      if (lead>8000) lead=2500;
      sdk_pacing::SetFlipMetering(set,true,int64_t(lead)*1000);
      xess_mod::WriteLog("flip_metering_available","\"lead_us\":"+std::to_string(lead));
    } catch(const std::exception& e) {
      sdk_pacing::SetFlipMetering(nullptr,false,0);
      xess_mod::WriteLog("flip_metering_unavailable","\"fallback\":\"SDK_extra_pacing\",\"reason\":\""+escape(e.what())+"\"");
    } catch(...) { sdk_pacing::SetFlipMetering(nullptr,false,0); }
  });
}
#define API(name) decltype(&name) original_##name{}
API(xefgSwapChainD3D12CreateContext);
API(xefgSwapChainD3D12InitFromSwapChain);
API(xefgSwapChainD3D12InitFromSwapChainDesc);
API(xefgSwapChainDestroy);
API(xefgSwapChainSetEnabled);
API(xefgSwapChainSetNumInterpolatedFrames);
API(xefgSwapChainGetLastPresentStatus);
#undef API
__declspec(noinline) Result Create(ID3D12Device* device, Handle* output) {
  // The public entry is captured early, but SDK code/metadata is never called
  // from an OptiScaler DllMain callback. The first real client call can join
  // the worker here before a context consumes the model or count settings.
  if (InsideLoader()) return unsupported;
  const auto caller=_ReturnAddress();
  CompleteStartup();
  std::lock_guard lock(mutex); auto& s=Get();
  if (!s.status.hooks) return original_xefgSwapChainD3D12CreateContext(device,output);
  // SDK self-calls retain their original forwarding/lifecycle semantics.
  if(SdkAddress(caller)) return original_xefgSwapChainD3D12CreateContext(device,output);
  if (!HostAddress(caller)) {
    if (s.model) return unsupported;
    const auto r=original_xefgSwapChainD3D12CreateContext(device,output);
    if(r==success && output && *output) s.nativeContexts.insert(*output);
    xess_mod::WriteLog("native_context_create","\"result\":"+std::to_string(r)+",\"caller\":\""+escape(CallerName(caller))+"\",\"handle\":"+std::to_string(output&&r==success?uintptr_t(*output):0));
    return r;
  }
  if (!device || !output) return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
  Prepare(device);
  const auto r=original_xefgSwapChainD3D12CreateContext(device,output);
  s.status.result=r;
  if (r==success && *output && s.model) {
    s.contexts.emplace(*output,Context{device,++s.generation});
    s.status.contexts=uint32_t(s.contexts.size());
  }
  else if(r==success && *output) s.nativeContexts.insert(*output);
  xess_mod::WriteLog("context_create","\"result\":"+std::to_string(r)+",\"mfg_model\":"+(s.model?"true":"false")+",\"handle\":"+std::to_string(r==success?uintptr_t(*output):0));
  return r;
}
template<class Init> Result Initialize(Handle handle, Init call) {
  std::lock_guard lock(mutex); auto& s=Get();
  const auto r=call(); s.status.result=r;
  auto it=s.contexts.find(handle);
  if (r!=success || it==s.contexts.end()) return r;
  auto& c=it->second;
  if (c.initialized) sdk_pacing::Unregister(handle,c.generation);
  c.initialized=false;
  std::string failure;
  ConfigureFlip();
  if (!sdk_pacing::Install(fg,failure) || !sdk_pacing::Register(handle,c.generation,c.device.Get(),failure)) {
    Detail("XeMFG disabled: extra-output pacing unavailable: "+failure);
    original_xefgSwapChainSetEnabled(handle,0);
    s.status.initialized=0; s.status.enabled=0;
    xess_mod::WriteLog("pacing_rejected","\"reason\":\""+escape(failure)+"\"");
    return unsupported;
  }
  c.initialized=true; s.status.initialized=1;
  Detail("XeMFG ready (2x-6x); use OptiScaler Frame Generation controls");
  xess_mod::WriteLog("context_initialized","\"paced\":true,\"generation\":"+std::to_string(c.generation));
  return r;
}
__declspec(noinline) Result InitChain(Handle h, ID3D12CommandQueue* q,const xefg_swapchain_d3d12_init_params_t* p) {
  if(SdkAddress(_ReturnAddress())) return original_xefgSwapChainD3D12InitFromSwapChain(h,q,p);
  return Initialize(h,[&] { return original_xefgSwapChainD3D12InitFromSwapChain(h,q,p); });
}
__declspec(noinline) Result InitDesc(Handle h,HWND w,const DXGI_SWAP_CHAIN_DESC1* d,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* f,
                ID3D12CommandQueue* q,IDXGIFactory2* factory,const xefg_swapchain_d3d12_init_params_t* p) {
  if(SdkAddress(_ReturnAddress())) return original_xefgSwapChainD3D12InitFromSwapChainDesc(h,w,d,f,q,factory,p);
  return Initialize(h,[&] { return original_xefgSwapChainD3D12InitFromSwapChainDesc(h,w,d,f,q,factory,p); });
}
__declspec(noinline) Result Enable(Handle h,uint32_t enabled) {
  if(SdkAddress(_ReturnAddress())) return original_xefgSwapChainSetEnabled(h,enabled);
  std::lock_guard lock(mutex); auto& s=Get(); auto it=s.contexts.find(h);
  if (it!=s.contexts.end() && enabled && !it->second.initialized) return unsupported;
  auto r=original_xefgSwapChainSetEnabled(h,enabled);
  if (r==success && it!=s.contexts.end()) {
    it->second.enabled=enabled!=0; s.status.enabled=enabled!=0;
    sdk_pacing::Reset(h,it->second.generation);
  }
  s.status.result=r; return r;
}
__declspec(noinline) Result Count(Handle h,uint32_t count) {
  if(SdkAddress(_ReturnAddress())) return original_xefgSwapChainSetNumInterpolatedFrames(h,count);
  std::lock_guard lock(mutex); auto& s=Get(); auto it=s.contexts.find(h);
  if (it!=s.contexts.end() && (!count || count>5 || !it->second.initialized)) return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
  const auto r=original_xefgSwapChainSetNumInterpolatedFrames(h,count);
  if (r==success && it!=s.contexts.end()) {
    if (it->second.generated!=count) sdk_pacing::Reset(h,it->second.generation);
    it->second.generated=count; s.status.generated=count;
  }
  s.status.result=r; return r;
}
Result LastStatus(Handle h,xefg_swapchain_present_status_t* p) {
  const auto r=original_xefgSwapChainGetLastPresentStatus(h,p);
  if (r==success && p) {
    std::lock_guard lock(mutex); auto& s=Get();
    if (s.contexts.contains(h)) {
      s.status.presented=p->framesPresented; s.status.result=p->frameGenResult;
      s.status.enabled=p->isFrameGenEnabled;
    }
  }
  return r;
}
__declspec(noinline) Result Destroy(Handle h) {
  if(SdkAddress(_ReturnAddress())) return original_xefgSwapChainDestroy(h);
  std::lock_guard lock(mutex); auto& s=Get(); const auto it=s.contexts.find(h);
  const auto r=original_xefgSwapChainDestroy(h); // SDK joins its workers first.
  if(r==success && s.nativeContexts.erase(h)) {
    xess_mod::WriteLog("native_context_destroy","\"handle\":"+std::to_string(uintptr_t(h))+",\"remaining\":"+std::to_string(s.nativeContexts.size()));
  }
  if (r==success && it!=s.contexts.end()) {
    sdk_pacing::Unregister(h,it->second.generation); s.contexts.erase(it);
    s.status.contexts=uint32_t(s.contexts.size());
    if (s.contexts.empty()) {
      s.status.initialized=s.status.enabled=s.status.presented=0;
      xefg_cuda::ForgetLayers();
    }
  }
  return r;
}
void Start() {
  std::lock_guard lock(mutex);
  if (mfgunlock::backend::Read()!=mfgunlock::backend::Kind::XeMfg) { Detail("DLSS-G selected; XeMFG companion inactive"); return; }
  if (GetModuleHandleW(L"MFGUnlocker.asi")) throw std::runtime_error("other_mfg_plugin_present");
  std::array<HMODULE,2048> modules{}; DWORD bytes{};
  if (!EnumProcessModules(GetCurrentProcess(),modules.data(),DWORD(sizeof(modules)),&bytes) || bytes>sizeof(modules)) throw std::runtime_error("module_inventory");
  for (size_t i=0;i<bytes/sizeof(HMODULE);++i) if (IsOptiScaler10(modules[i])) {
    if (host && host!=modules[i]) throw std::runtime_error("multiple_optiscaler_hosts"); host=modules[i];
  }
  if (!host) throw std::runtime_error("OptiScaler_10_required");
  const auto dir=ModulePath(host).parent_path();
  const auto fgPath=std::filesystem::exists(dir/L"OptiScaler/libxess_fg.dll")?dir/L"OptiScaler/libxess_fg.dll":dir/L"libxess_fg.dll";
  const auto llPath=std::filesystem::exists(dir/L"OptiScaler/libxell.dll")?dir/L"OptiScaler/libxell.dll":dir/L"libxell.dll";
  const auto fh=HashFile(fgPath), lh=HashFile(llPath);
  if (fh!="ec5e0c65e075570c6ede72618bb666d0be0c2e10b2ea9762c0fe8cb8e375ab27"
      || (lh!="61aeeab7fdcfb6d1742f86c14c68c663d759607d01a6132645c857291eaebb86"
          && lh!="d2030dcd694fda8f2ec7e044b13e6db8f0b56d4ba9113a5efad334e3f3ded8c7")) throw std::runtime_error("unreviewed_Intel_runtime_pair");
  fg=LoadLibraryExW(fgPath.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  ll=LoadLibraryExW(llPath.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!fg || !ll) throw std::runtime_error("Intel_runtime_load");
  if (bootstrapCreate && bootstrapFg!=fg) throw std::runtime_error("bootstrap_provider_differs");
  if (!std::filesystem::equivalent(ModulePath(fg),fgPath)
      || !std::filesystem::equivalent(ModulePath(ll),llPath)) throw std::runtime_error("loaded_runtime_path_differs");
  Pin(self); Pin(host); Pin(fg); Pin(ll);
  xess_layout::entryRedirectFilter.store(&HostRedirect);
  if (!xess_layout::Pair(fg,ll)) throw std::runtime_error("Intel_instruction_profile_rejected");
  // Verify all private writers before public entrypoints change. Publication
  // itself is deferred until OptiScaler's first real CreateContext call.
  const auto mh=MH_Initialize();
  if (mh!=MH_OK && mh!=MH_ERROR_ALREADY_INITIALIZED) throw std::runtime_error("hook_init");
  struct Hook { const char* name; void* target; void** original; };
#define H(name,fn) {#name,reinterpret_cast<void*>(&fn),reinterpret_cast<void**>(&original_##name)}
  const Hook hooks[]={H(xefgSwapChainD3D12CreateContext,Create),H(xefgSwapChainD3D12InitFromSwapChain,InitChain),
    H(xefgSwapChainD3D12InitFromSwapChainDesc,InitDesc),H(xefgSwapChainDestroy,Destroy),
    H(xefgSwapChainSetEnabled,Enable),H(xefgSwapChainSetNumInterpolatedFrames,Count),H(xefgSwapChainGetLastPresentStatus,LastStatus)};
#undef H
  std::vector<void*> targets;
  for (const auto& h:hooks) {
    if (bootstrapCreate && h.original==reinterpret_cast<void**>(&original_xefgSwapChainD3D12CreateContext)) continue;
    void* target=reinterpret_cast<void*>(GetProcAddress(fg,h.name));
    // OptiScaler's GameSetEnabled (among others) branches on its caller.
    // Chaining through our wrapper turns a host output call into a game call.
    // Keep the host's entry untouched, as with its XeLL redirects.
    if(target && HostRedirect(target)) {
      *h.original=target;
      xess_mod::WriteLog("host_entry_preserved","\"api\":\""+std::string(h.name)+"\"");
      continue;
    }
    if (!target || MH_CreateHook(target,h.target,h.original)!=MH_OK) {
      for (void* t:targets) MH_RemoveHook(t);
      throw std::runtime_error("SDK_hook_preflight");
    }
    targets.push_back(target);
  }
  for (void* t:targets) if (MH_QueueEnableHook(t)!=MH_OK) throw std::runtime_error("SDK_hook_queue");
  if (MH_ApplyQueued()!=MH_OK) throw std::runtime_error("SDK_hook_commit");
  Get().status.hooks=1;
  Detail("SDK hooks ready; waiting for OptiScaler XeFG context");
  xess_mod::WriteLog("ready","\"provider_sha256\":\""+fh+"\",\"xell_sha256\":\""+lh+"\"");
}
void CompleteStartup() noexcept {
  // Do not enter call_once while inside the loader: another initializer may
  // need that same loader before it can complete.
  if (InsideLoader()) return;
  std::call_once(startupOnce,[] {
    try { Start(); }
    catch (const std::exception& e) { std::lock_guard lock(mutex); Detail(e.what()); xess_mod::WriteLog("startup_rejected","\"reason\":\""+escape(e.what())+"\""); }
    catch (...) { std::lock_guard lock(mutex); Detail("unexpected_startup_failure"); }
  });
}
bool CaptureEarlyCreate() noexcept {
  // Only inspect a mapped public export. In the initial dependency graph the
  // Intel image may already be mapped even though its CRT/DllMain has not run.
  // Calling GetVersion, touching its catalogs or loading another SDK here is
  // unsafe (the X1 Cyberpunk dump reproduced exactly that failure).
  const auto module=GetModuleHandleW(L"libxess_fg.dll");
  if (!module) return false;
  void* target=reinterpret_cast<void*>(GetProcAddress(module,"xefgSwapChainD3D12CreateContext"));
  const auto mh=MH_Initialize();
  if (!target || (mh!=MH_OK && mh!=MH_ERROR_ALREADY_INITIALIZED)) return false;
  for (const auto owner : {self,module}) {
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(owner),&pinned) || pinned!=owner) return false;
  }
  if (MH_CreateHook(target,reinterpret_cast<void*>(&Create),
      reinterpret_cast<void**>(&original_xefgSwapChainD3D12CreateContext))!=MH_OK) return false;
  bootstrapFg=module;
  if (MH_EnableHook(target)!=MH_OK) return false; // Retain any published trampoline.
  bootstrapCreate=true;
  return true;
}
DWORD WINAPI StartupWorker(void* reference) {
  // Windows starts this thread after the process's DLL initialization phase.
  // No wait/join is performed by InitializeASI or DllMain.
  CompleteStartup();
  FreeLibraryAndExitThread(static_cast<HMODULE>(reference),0);
}
} // namespace

namespace xess_mod {
HMODULE Self() noexcept { return self; }
void WriteLog(const char* stage,const std::string& fields) noexcept {
  try {
    std::lock_guard lock(sinkMutex);
    std::ofstream f(mfgunlock::backend::Directory()/L"MFGAdaUnlock-XeMFG.log",std::ios::app);
    f<<"{\"tick\":"<<GetTickCount64()<<",\"event\":\""<<stage<<"\""<<(fields.empty()?"":",")<<fields<<"}\n";
  } catch (...) {}
}
}
extern "C" __declspec(dllexport) void InitializeASI() {
  std::call_once(initializeOnce,[] {
    if (!InsideLoader()) { CompleteStartup(); return; }
    if (mfgunlock::backend::Read()!=mfgunlock::backend::Kind::XeMfg) {
      std::lock_guard lock(mutex); Detail("DLSS-G selected; XeMFG companion inactive"); return;
    }
    if (!CaptureEarlyCreate()) {
      std::lock_guard lock(mutex); Detail("Early XeFG Create entry unavailable; native startup retained"); return;
    }
    { std::lock_guard lock(mutex); Detail("Waiting for DLL initialization to finish"); }
    HMODULE reference{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(self),&reference)) return;
    const auto thread=CreateThread(nullptr,0,&StartupWorker,reference,0,nullptr);
    if (thread) CloseHandle(thread);
    else FreeLibrary(reference); // First captured Create can still complete startup.
  });
}
extern "C" __declspec(dllexport) bool MFGAXeGetStatus(mfgunlock::xemfg::Status* out,uint32_t size) {
  if (!out || size!=sizeof(*out)) return false;
  std::lock_guard lock(mutex); auto& s=Get();
  s.status.tensor_launches=xefg_cuda::Snapshot().launches; *out=s.status; return true;
}
BOOL WINAPI DllMain(HMODULE module,DWORD reason,LPVOID) {
  if (reason==DLL_PROCESS_ATTACH) { self=module; DisableThreadLibraryCalls(module); }
  return TRUE;
}

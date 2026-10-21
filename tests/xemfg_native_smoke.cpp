// SPDX-License-Identifier: MIT
// Real ReShade loader, real Intel SDK/model/pacer, synthetic Streamline caller.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <deps/imgui/imgui.h>
#include <reshade.hpp>
#include "../src/addons/mfgunlock/xemfg/backend_api.hpp"
#include "../src/addons/mfgunlock/xemfg/native_bridge.hpp"
#include "../src/addons/mfgunlock/xemfg/native_controls.hpp"
#include "../src/addons/mfgunlock/xemfg/native_overlay.hpp"
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <filesystem>
#include <dbghelp.h>
#include <future>
#include <array>
#include <algorithm>
using Microsoft::WRL::ComPtr;
#include "xemfg_queue_proxy.hpp"
static bool keyTest{};
static bool hotkeyTest{};
static unsigned overlayOpens{},overlayCloses{};
static unsigned commands{};
static bool selectPanel{};
static bool retainNativeOutput{};
static ComPtr<IDXGISwapChain> foreignNativeA,foreignNativeB;
static void RetainNativeOutput(reshade::api::effect_runtime* chain) {
  if(!retainNativeOutput || foreignNativeA) return;
  auto* native=reinterpret_cast<IUnknown*>(chain->get_native());
  if(native && SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&foreignNativeA)))) foreignNativeB=foreignNativeA;
}
static LONG WINAPI CaptureIsolatedException(EXCEPTION_POINTERS* exception) {
  if(exception->ExceptionRecord->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
  static LONG count{}; const auto index=InterlockedIncrement(&count);
  if(index>8) return EXCEPTION_CONTINUE_SEARCH;
  wchar_t name[80]{}; wsprintfW(name,L"isolated-exception-%ld.dmp",index);
  const auto file=CreateFileW(name,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file!=INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(),exception,FALSE};
    MiniDumpWriteDump(GetCurrentProcess(),GetCurrentProcessId(),file,MiniDumpWithThreadInfo,&info,nullptr,nullptr);
    CloseHandle(file);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
static void OverlayGui(reshade::api::effect_runtime*) {
  if(selectPanel) ImGui::SetWindowFocus("MFG Unlock");
}
static LRESULT CALLBACK TestWindowProc(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
  if(message==WM_KEYDOWN && w==VK_F23) ++commands;
  return DefWindowProcW(hwnd,message,w,l);
}
static bool OverlayChange(reshade::api::effect_runtime*,bool open,reshade::api::input_source) {
  if(open) ++overlayOpens; else ++overlayCloses;
  return false;
}
static void Check(HRESULT code,const char* where) { if(FAILED(code)) { std::cerr<<where<<" 0x"<<std::hex<<code<<std::endl; throw std::runtime_error(where); } }
static void Require(bool value,const char* what) { if(!value) throw std::runtime_error(what); }
static HRESULT CreateTestSwapchain(bool legacy,IDXGIFactory2* factory,IUnknown* source,HWND window,
    const DXGI_SWAP_CHAIN_DESC1& desc,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* full,IDXGISwapChain1** output) {
  if(!legacy) return factory->CreateSwapChainForHwnd(source,window,&desc,full,nullptr,output);
  DXGI_SWAP_CHAIN_DESC old{};
  old.BufferDesc.Width=desc.Width; old.BufferDesc.Height=desc.Height; old.BufferDesc.Format=desc.Format;
  if(full) {
    old.BufferDesc.RefreshRate=full->RefreshRate; old.BufferDesc.Scaling=full->Scaling;
    old.BufferDesc.ScanlineOrdering=full->ScanlineOrdering;
  }
  old.SampleDesc=desc.SampleDesc; old.BufferUsage=desc.BufferUsage; old.BufferCount=desc.BufferCount;
  old.OutputWindow=window; old.Windowed=full?full->Windowed:TRUE;
  old.SwapEffect=desc.SwapEffect; old.Flags=desc.Flags;
  const auto original=old;
  ComPtr<IDXGISwapChain> result;
  const auto hr=factory->CreateSwapChain(source,&old,&result);
  Require(memcmp(&old,&original,sizeof(old))==0,"legacy factory mutated caller description");
  if(FAILED(hr)) return hr;
  return result.CopyTo(output);
}
struct Token : sl::FrameToken { uint32_t id{}; operator uint32_t() const override {return id;} };
template<class T> T Import(HMODULE m,const char* name) { auto p=reinterpret_cast<T>(GetProcAddress(m,name)); Require(p!=nullptr,name); return p; }
// A valid public COM wrapper whose object-owned dispatch table is in the heap.
// Its methods remain image-backed. This reproduces the admission constraint
// without importing another overlay or guessing any native-object offsets.
class ProducerCommandProxy final:public ID3D12CommandList {
  std::atomic<ULONG> refs{1};
  ComPtr<ID3D12GraphicsCommandList> native;
  std::array<void*,9> table{};
public:
  explicit ProducerCommandProxy(ID3D12GraphicsCommandList* value):native(value) {
    auto*** slot=reinterpret_cast<void***>(static_cast<ID3D12CommandList*>(this));
    std::copy_n(*slot,table.size(),table.begin()); *slot=table.data();
  }
  void RejectQueryImplementation() { table[0]=table.data(); } // readable data, never callable
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
    if(!out) return E_POINTER; *out=nullptr;
    constexpr GUID base={0xadec44e2,0x61f0,0x45c3,{0xad,0x9f,0x1b,0x37,0x37,0x92,0x84,0xff}};
    if(id==base) { *out=native.Get(); native->AddRef(); return S_OK; }
    if(id==__uuidof(IUnknown)||id==__uuidof(ID3D12Object)||id==__uuidof(ID3D12DeviceChild)||id==__uuidof(ID3D12CommandList)) {
      *out=static_cast<ID3D12CommandList*>(this); AddRef(); return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { const auto count=--refs; if(!count) delete this; return count; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID id,UINT* size,void* data) override {return native->GetPrivateData(id,size,data);}
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID id,UINT size,const void* data) override {return native->SetPrivateData(id,size,data);}
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID id,const IUnknown* value) override {return native->SetPrivateDataInterface(id,value);}
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override {return native->SetName(name);}
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID id,void** out) override {return native->GetDevice(id,out);}
  D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override {return native->GetType();}
};
static void Barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; list->ResourceBarrier(1,&b);
}
struct RealInputs {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  ComPtr<ID3D12DescriptorHeap> rtv;
  ComPtr<ID3D12Resource> depth,motion;
  uint64_t value{};
  HANDLE event{};
  RealInputs(ID3D12Device* d,ID3D12CommandQueue* q,uint32_t width,uint32_t height):device(d),queue(q) {
    Check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"real allocator");
    Check(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"real list"); list->Close();
    Check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"real fence");
    event=CreateEventW(nullptr,FALSE,FALSE,nullptr); Require(event,"real fence event");
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors=1;
    Check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)),"real RTV");
    for(auto item:{std::pair{std::addressof(depth),DXGI_FORMAT_R32_FLOAT},std::pair{std::addressof(motion),DXGI_FORMAT_R16G16_FLOAT}}) {
      D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width=width; desc.Height=height;
      desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1; desc.Format=item.second;
      Check(d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(item.first->ReleaseAndGetAddressOf())),"real input texture");
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows{}; UINT64 bytes{};
      d->GetCopyableFootprints(&desc,0,1,0,&fp,&rows,nullptr,&bytes);
      heap.Type=D3D12_HEAP_TYPE_UPLOAD; desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width=bytes; desc.Height=1;
      desc.Format=DXGI_FORMAT_UNKNOWN; desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      ComPtr<ID3D12Resource> upload;
      Check(d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)),"real input upload");
      void* data{}; Check(upload->Map(0,nullptr,&data),"real upload map"); memset(data,0,size_t(bytes));
      if(item.first==std::addressof(depth)) for(UINT y=0;y<rows;++y) for(UINT x=0;x<width;++x)
        reinterpret_cast<float*>(static_cast<char*>(data)+y*fp.Footprint.RowPitch)[x]=0.5f;
      upload->Unmap(0,nullptr); allocator->Reset(); list->Reset(allocator.Get(),nullptr);
      D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=upload.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=fp;
      D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=item.first->Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
      Barrier(list.Get(),item.first->Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      list->Close(); ID3D12CommandList* raw=list.Get(); q->ExecuteCommandLists(1,&raw); Wait();
    }
  }
  void Wait() {
    Check(queue->Signal(fence.Get(),++value),"real signal");
    Check(fence->SetEventOnCompletion(value,event),"real fence completion");
    Require(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"real GPU wait timed out");
  }
  void Render(ID3D12Resource* back) {
    allocator->Reset(); list->Reset(allocator.Get(),nullptr);
    auto handle=rtv->GetCPUDescriptorHandleForHeapStart(); device->CreateRenderTargetView(back,nullptr,handle);
    Barrier(list.Get(),back,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float color[]={0.1f,0.2f,0.3f,1.f}; list->ClearRenderTargetView(handle,color,0,nullptr);
    Barrier(list.Get(),back,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
    list->Close(); ID3D12CommandList* raw=list.Get(); queue->ExecuteCommandLists(1,&raw); Wait();
  }
  ~RealInputs() { if(event) CloseHandle(event); }
};
int main(int argc,char** argv) {
  try {
    const unsigned multiplier=argc>1?unsigned(std::stoul(argv[1])):4;
    Require(multiplier>=2 && multiplier<=6,"test multiplier");
    const auto flag=[&](const char* name) { for(int i=2;i<argc;++i) if(std::string(argv[i])==name) return true; return false; };
    const bool legacyFactory=flag("--legacy-factory");
    const bool refLegacy=flag("--ref-legacy"),refLate=flag("--ref-late"),refAmbiguous=flag("--ref-ambiguous");
    const bool refBlocked=flag("--ref-blocked") || flag("--ref-invalid") || flag("--ref-exception") || refAmbiguous;
    const bool refTest=refBlocked || refLegacy || refLate || flag("--ref-detached") || flag("--ref-not-tracked");
    HMODULE refFixture{},refOther{};
    if(refTest && !refLate) {
      refFixture=LoadLibraryW(refLegacy?L".\\reframework-legacy-fixture.dll":L".\\reframework-v1-fixture.dll");
      Require(refFixture,"REFramework fixture preload");
    }
    if(flag("--real-streamline")) AddVectoredExceptionHandler(1,&CaptureIsolatedException);
    if(flag("--preload-runtimes")) {
      Require(LoadLibraryW(L".\\_storage_\\libxess_fg.dll")!=nullptr,"preload stored XeFG");
      Require(LoadLibraryW(L".\\_storage_\\libxell.dll")!=nullptr,"preload stored XeLL");
    }
    auto fixture=LoadLibraryW(L"sl.interposer.dll"); Require(fixture,"fixture");
    if(flag("--legacy-markers") && !flag("--real-streamline")) Import<void(*)(uint32_t)>(fixture,"FixtureMarkerRoute")(1);
    if(flag("--alias-markers")) Import<void(*)(uint32_t)>(fixture,"FixtureMarkerRoute")(2);
    if(flag("--early-addon")) {
      auto module=LoadLibraryW(L".\\dxgi.dll"); Require(module,"early ReShade");
      ComPtr<IDXGIFactory4> earlyFactory;
      Check(Import<HRESULT(WINAPI*)(REFIID,void**)>(module,"CreateDXGIFactory1")(IID_PPV_ARGS(&earlyFactory)),"early addon discovery");
      // ReShade loads addons on its first device, rather than factory creation.
      ComPtr<ID3D12Device> earlyDevice;
      Check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&earlyDevice)),"early ReShade device");
      Require(GetModuleHandleW(L"renodx-mfgunlock-native.addon64"),"early addon not loaded");
    }
    if(flag("--real-streamline")) {
      sl::Preferences preferences;
      const sl::Feature features[]={sl::kFeatureDLSS_G,sl::kFeatureReflex};
      const auto directory=std::filesystem::current_path().wstring();
      const wchar_t* paths[]={directory.c_str()};
      preferences.pathsToPlugins=paths; preferences.numPathsToPlugins=1;
      preferences.pathToLogsAndData=directory.c_str();
      preferences.featuresToLoad=features; preferences.numFeaturesToLoad=2;
      preferences.flags=sl::PreferenceFlags::eDisableCLStateTracking|sl::PreferenceFlags::eUseDXGIFactoryProxy|sl::PreferenceFlags::eUseFrameBasedResourceTagging;
      preferences.engine=sl::EngineType::eCustom; preferences.engineVersion="1";
      // Dedicated test project identity, not a game's application identity.
      preferences.projectId="e7b960d2-8f54-4c19-a63e-5d927bf104a8";
      auto init=Import<PFun_slInit*>(fixture,"slInit");
      const auto result=init(preferences,sl::kSDKVersion);
      std::cout<<"Real Streamline initialization="<<uint32_t(result)<<std::endl;
      Require(result==sl::Result::eOk,"real Streamline init");
      if(flag("--isolate-nvidia-hooks"))
        Require(Import<PFun_slSetFeatureLoaded*>(fixture,"slSetFeatureLoaded")(sl::kFeatureDLSS_G,false)==sl::Result::eOk,"isolate NVIDIA presentation hooks");
    } else { sl::Preferences preferences; Require(Import<PFun_slInit*>(fixture,"slInit")(preferences,sl::kSDKVersion)==sl::Result::eOk,"fixture startup"); }
    auto reshade=LoadLibraryW(L".\\dxgi.dll"); Require(reshade,"ReShade");
    hotkeyTest=flag("--overlay-foreground");
    keyTest=flag("--overlay-keys") || hotkeyTest;
    retainNativeOutput=flag("--foreign-refs") || flag("--retained-output");
    if(keyTest || retainNativeOutput) {
      Require(reshade::register_addon(GetModuleHandleW(nullptr),reshade),"register keyboard test observer");
      if(retainNativeOutput) reshade::register_event<reshade::addon_event::init_effect_runtime>(&RetainNativeOutput);
    }
    if(keyTest) {
      reshade::register_event<reshade::addon_event::reshade_open_overlay>(&OverlayChange);
      reshade::register_event<reshade::addon_event::reshade_overlay>(&OverlayGui);
      selectPanel=true;
    }
    auto factoryCreate=Import<HRESULT(WINAPI*)(REFIID,void**)>(reshade,"CreateDXGIFactory1");
    ComPtr<IDXGIFactory4> factory; Check(factoryCreate(IID_PPV_ARGS(&factory)),"factory");
    if(flag("--real-streamline")) {
      void* upgraded=factory.Detach();
      Require(Import<PFun_slUpgradeInterface*>(fixture,"slUpgradeInterface")(&upgraded)==sl::Result::eOk,"real Streamline factory proxy");
      factory.Attach(static_cast<IDXGIFactory4*>(upgraded));
    }
    ComPtr<ID3D12Device> device;
    for(UINT i=0;!device;++i) {
      ComPtr<IDXGIAdapter1> adapter;
      if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
      if(desc.VendorId==0x10de) D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device));
    }
    Require(device!=nullptr,"NVIDIA device");
    auto addon=GetModuleHandleW(L"renodx-mfgunlock-native.addon64");
    if(!flag("--stock-streamline")) Require(addon,"ReShade did not load the addon");
    if(flag("--stall-trace")) Import<void(*)(bool,bool)>(addon,"MFGAXeStallTraceEnable")(true,true);
    Require(Import<PFun_slSetD3DDevice*>(fixture,"slSetD3DDevice")(device.Get())==sl::Result::eOk,"Streamline device initialization");
    if(flag("--real-streamline")) {
      bool loaded{};
      Require(Import<PFun_slIsFeatureLoaded*>(fixture,"slIsFeatureLoaded")(sl::kFeatureDLSS_G,loaded)==sl::Result::eOk && loaded!=flag("--isolate-nvidia-hooks"),
          "real DLSS-G plugin must be initialized for this reproduction");
      void* setter{};
      Require(Import<PFun_slGetFeatureFunction*>(fixture,"slGetFeatureFunction")(sl::kFeatureDLSS_G,"slDLSSGSetOptions",setter)==sl::Result::eOk && setter,
          "real DLSS-G setter");
      auto resolveReal=Import<PFun_slGetFeatureFunction*>(fixture,"slGetFeatureFunction");
      void *reflexFn{},*sleepFn{},*markerFn{};
      Require(resolveReal(sl::kFeatureReflex,"slReflexSetOptions",reflexFn)==sl::Result::eOk && reflexFn,"real Reflex options");
      Require(resolveReal(sl::kFeatureReflex,"slReflexSleep",sleepFn)==sl::Result::eOk && sleepFn,"real Reflex sleep");
      Require(resolveReal(flag("--legacy-markers")?sl::kFeatureReflex:sl::kFeaturePCL,
          flag("--legacy-markers")?"slReflexSetMarker":"slPCLSetMarker",markerFn)==sl::Result::eOk && markerFn,"real latency markers");
      auto tokenFn=Import<PFun_slGetNewFrameToken*>(fixture,"slGetNewFrameToken");
      auto model=addon?Import<mfgunlock::xemfg::GetStatus>(addon,"MFGAXeGetStatus"):nullptr;
      auto bridge=addon?Import<bool(*)(mfgunlock::xemfg::native::Status*,uint32_t)>(addon,"MFGAXeNativeStatus"):nullptr;
      auto overlays=addon?Import<bool(*)(mfgunlock::xemfg::native::OverlayStatus*,uint32_t)>(addon,"MFGAXeOverlayStatus"):nullptr;
      WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.lpszClassName=L"XeMFGRealStreamline"; wc.hInstance=GetModuleHandleW(nullptr); RegisterClassW(&wc);
      const auto window=CreateWindowW(wc.lpszClassName,L"Isolated Streamline output recreation",WS_OVERLAPPEDWINDOW,0,0,640,360,nullptr,nullptr,wc.hInstance,nullptr);
      Require(window,"real recreation window");
      const bool visible=flag("--visible-recreation");
      if(visible) { ShowWindow(window,SW_SHOWNOACTIVATE); UpdateWindow(window); }
      for(unsigned cycle=0;cycle<4;++cycle) {
        D3D12_COMMAND_QUEUE_DESC queueDesc{}; ComPtr<ID3D12CommandQueue> queue;
        Check(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)),"real recreation queue");
        DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=cycle?2560:1920; desc.Height=cycle?1440:1080;
        desc.Format=DXGI_FORMAT_R10G10B10A2_UNORM; desc.SampleDesc.Count=1;
        desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=cycle%2?4:3;
        desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD; desc.Flags=legacyFactory?0x802:0x842;
        ComPtr<IDXGISwapChain1> base;
        Check(CreateTestSwapchain(legacyFactory,factory.Get(),queue.Get(),window,desc,nullptr,&base),"real Streamline replacement");
        ComPtr<IDXGISwapChain4> chain; Check(base.As(&chain),"real replacement chain4"); base.Reset();
        mfgunlock::xemfg::native::Status bs;
        if(bridge) Require(bridge(&bs,sizeof(bs)) && bs.output,"real replacement did not use Intel output");
        sl::DLSSGOptions off; off.structVersion=3; off.mode=sl::DLSSGMode::eOff;
        Require(reinterpret_cast<PFun_slDLSSGSetOptions*>(setter)(sl::ViewportHandle{0},off)==sl::Result::eOk,"real native FG disable");
        sl::ReflexOptions latency; latency.mode=sl::ReflexMode::eLowLatency;
        Require(reinterpret_cast<PFun_slReflexSetOptions*>(reflexFn)(latency)==sl::Result::eOk,"real latency options");
        std::unique_ptr<RealInputs> inputs;
        const bool generate=flag("--generate") && addon;
        if(generate) {
          inputs=std::make_unique<RealInputs>(device.Get(),queue.Get(),desc.Width,desc.Height);
          sl::DLSSGOptions on; on.structVersion=3; on.mode=sl::DLSSGMode::eOn; on.numFramesToGenerate=3;
          Require(reinterpret_cast<PFun_slDLSSGSetOptions*>(setter)(sl::ViewportHandle{0},on)==sl::Result::eOk,"real replacement FG on");
        }
        unsigned shown{};
        for(unsigned frame=0;frame<(visible?90u:generate?8u:3u);++frame) {
          MSG pending{}; while(PeekMessageW(&pending,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&pending);
          sl::FrameToken* token{};
          Require(tokenFn(token,nullptr)==sl::Result::eOk && token,"real frame token");
          if(!flag("--tail-sleep")) Require(reinterpret_cast<PFun_slReflexSleep*>(sleepFn)(*token)==sl::Result::eOk,"real source sleep");
          const auto mark=[&](sl::PCLMarker marker) { Require(reinterpret_cast<PFun_slPCLSetMarker*>(markerFn)(marker,*token)==sl::Result::eOk,"real source marker"); };
          mark(sl::PCLMarker::eSimulationStart); mark(sl::PCLMarker::eSimulationEnd);
          std::future<sl::Result> tailSleep;
          if(flag("--tail-sleep") && flag("--overlap-tail-sleep")) tailSleep=std::async(std::launch::async,[sleepFn,token] {
            return reinterpret_cast<PFun_slReflexSleep*>(sleepFn)(*token);
          });
          mark(sl::PCLMarker::eRenderSubmitStart);
          ComPtr<ID3D12Resource> back;
          Check(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&back)),"real input buffer");
          if(generate) {
            inputs->Render(back.Get());
            sl::Constants constants;
            DirectX::XMFLOAT4X4 matrix;
            auto projection=DirectX::XMMatrixPerspectiveFovLH(1.f,float(desc.Width)/desc.Height,.1f,1000.f);
            DirectX::XMStoreFloat4x4(&matrix,projection); memcpy(&constants.cameraViewToClip,&matrix,64);
            DirectX::XMStoreFloat4x4(&matrix,DirectX::XMMatrixInverse(nullptr,projection)); memcpy(&constants.clipToCameraView,&matrix,64);
            DirectX::XMStoreFloat4x4(&matrix,DirectX::XMMatrixIdentity());
            memcpy(&constants.clipToPrevClip,&matrix,64); memcpy(&constants.prevClipToClip,&matrix,64);
            constants.cameraPos={0,0,0}; constants.cameraRight={1,0,0}; constants.cameraUp={0,1,0}; constants.cameraFwd={0,0,1};
            constants.cameraNear=.1f; constants.cameraFar=1000.f; constants.cameraFOV=1.f; constants.cameraAspectRatio=float(desc.Width)/desc.Height;
            constants.jitterOffset={0,0}; constants.cameraPinholeOffset={0,0}; constants.mvecScale={1.f/desc.Width,1.f/desc.Height};
            constants.depthInverted=sl::eTrue; constants.cameraMotionIncluded=sl::eTrue; constants.motionVectors3D=sl::eFalse; constants.reset=frame?sl::eFalse:sl::eTrue;
            Require(Import<PFun_slSetConstants*>(fixture,"slSetConstants")(constants,*token,sl::ViewportHandle{0})==sl::Result::eOk,"real frame constants");
            sl::Resource depth(sl::ResourceType::eTex2d,inputs->depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            sl::Resource motion(sl::ResourceType::eTex2d,inputs->motion.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            sl::ResourceTag tags[]={{&depth,sl::kBufferTypeDepth,sl::eValidUntilPresent},{&motion,sl::kBufferTypeMotionVectors,sl::eValidUntilPresent}};
            if(auto tagFrame=reinterpret_cast<PFun_slSetTagForFrame*>(GetProcAddress(fixture,"slSetTagForFrame")))
              Require(tagFrame(*token,sl::ViewportHandle{0},tags,2,nullptr)==sl::Result::eOk,"real frame inputs");
            else
              Require(Import<PFun_slSetTag*>(fixture,"slSetTag")(sl::ViewportHandle{0},tags,2,nullptr)==sl::Result::eOk,"real legacy frame inputs");
          }
          back.Reset();
          mark(sl::PCLMarker::eRenderSubmitEnd); mark(sl::PCLMarker::ePresentStart);
          const auto presented=chain->Present(0,0);
          Check(presented,"real passthrough Present");
          if(presented==S_OK) ++shown;
          mark(sl::PCLMarker::ePresentEnd);
          if(inputs) inputs->Wait();
          if(tailSleep.valid()) Require(tailSleep.get()==sl::Result::eOk,"real tail source sleep");
          else if(flag("--tail-sleep")) Require(reinterpret_cast<PFun_slReflexSleep*>(sleepFn)(*token)==sl::Result::eOk,"real serial tail source sleep");
          if(visible) Sleep(16);
        }
        if(visible) { Require(shown>60,"visible recreation did not submit visible Presents"); std::cout<<"Visible Presents cycle "<<cycle<<"="<<shown<<std::endl; }
        if(generate) {
          mfgunlock::xemfg::Status ms; model(&ms,sizeof(ms));
          Require(bridge(&bs,sizeof(bs)) && bs.enabled && (flag("--tail-sleep")?bs.rejected==cycle+1:!bs.rejected) && ms.tensor_launches>0,"real Streamline frame inputs did not reach Intel inference");
          std::cout<<"Real Streamline inference cycle "<<cycle<<" tensor launches="<<ms.tensor_launches<<std::endl;
          Require(reinterpret_cast<PFun_slDLSSGSetOptions*>(setter)(sl::ViewportHandle{0},off)==sl::Result::eOk,"real replacement disable");
        }
        chain.Reset();
        if(model) {
          mfgunlock::xemfg::Status ms; model(&ms,sizeof(ms));
          Require(ms.contexts==0,"real Streamline retained an Intel context");
          mfgunlock::xemfg::native::OverlayStatus os; overlays(&os,sizeof(os));
          Require(os.created && os.created==os.destroyed,"real Streamline retained its final overlay");
        }
        MSG message{}; while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&message);
        std::cout<<"Real Streamline recreation cycle "<<cycle<<" complete"<<std::endl;
      }
      Require(Import<PFun_slShutdown*>(fixture,"slShutdown")()==sl::Result::eOk,"real Streamline shutdown");
      DestroyWindow(window);
      if(addon) Require(Import<bool(*)(uint32_t)>(addon,"MFGAXeLogFlush")(2000),"real recreation log flush");
      if(legacyFactory) std::cout<<"Legacy factory: four same-window recreations, real Streamline inference and shutdown verified"<<std::endl;
      std::cout<<"PASS: real Streamline, ReShade and Intel SDK; four same-window output recreations"<<std::endl;
      return 0;
    }
    auto panelDraws=Import<uint64_t(*)()>(addon,"MFGAXePanelDraws");
    auto modelStatus=Import<mfgunlock::xemfg::GetStatus>(addon,"MFGAXeGetStatus");
    if(flag("--expect-startup-rejection")) {
      mfgunlock::xemfg::Status status;
      const auto deadline=GetTickCount64()+10000;
      do {
        modelStatus(&status,sizeof(status));
        if(strstr(status.detail,"unreviewed_Intel_runtime_pair") || strstr(status.detail,"missing_Intel_runtime:")) break;
        Sleep(20);
      } while(GetTickCount64()<deadline);
      Require(!status.hooks && !status.model && !status.initialized,"rejected runtime published model hooks");
      Require(strstr(status.detail,"unreviewed_Intel_runtime_pair") || strstr(status.detail,"missing_Intel_runtime:"),"expected startup refusal missing");
      Sleep(250); // Allow the bounded asynchronous log writer to flush evidence.
      std::cout<<"PASS: runtime rejected safely: "<<status.detail<<std::endl;
      return 0;
    }
    using BridgeStatus=bool(*)(mfgunlock::xemfg::native::Status*,uint32_t);
    auto bridgeStatus=Import<BridgeStatus>(addon,"MFGAXeNativeStatus");
    using ControlsStatus=bool(*)(mfgunlock::xemfg::native::ControlStatus*,uint32_t);
    using Configure=bool(*)(const mfgunlock::xemfg::native::ControlSettings*,uint32_t);
    auto controlStatus=Import<ControlsStatus>(addon,"MFGAXeControlsStatus");
    auto configure=Import<Configure>(addon,"MFGAXeConfigure");
    auto timingEnable=Import<void(*)(bool)>(addon,"MFGAXeTimingEnable");
    auto timingReport=Import<uint32_t(*)(char*,uint32_t)>(addon,"MFGAXeTimingReport");
    auto overlayStatus=Import<bool(*)(mfgunlock::xemfg::native::OverlayStatus*,uint32_t)>(addon,"MFGAXeOverlayStatus");
    auto overlayInput=Import<bool(*)(mfgunlock::xemfg::native::OverlayInputStatus*,uint32_t)>(addon,"MFGAXeOverlayInputStatus");
    timingEnable(true);
    mfgunlock::xemfg::native::ControlStatus cs;
    Require(controlStatus(&cs,sizeof(cs)),"control status ABI");
    Require(cs.version==5 && cs.settings.version==5 && !cs.settings.uiComposition && !cs.settings.sdkDeadlines,"default/ABI");
    cs.version=4; Require(!controlStatus(&cs,sizeof(cs)),"old status ABI admitted"); cs.version=5;
    Require(controlStatus(&cs,sizeof(cs)),"current status ABI rejected");
    const bool nativeReflex=cs.activeLatencyOutput==mfgunlock::xemfg::native::LatencyOutput::Reflex;
    auto settings=cs.settings; settings.input=mfgunlock::xemfg::native::LatencyInput::StreamlineReflex;
    settings.output=nativeReflex?mfgunlock::xemfg::native::LatencyOutput::Reflex:mfgunlock::xemfg::native::LatencyOutput::XeLL;
    settings.forceBorderless=true;
    Require(configure(&settings,sizeof(settings)),"explicit Reflex to XeLL preference");
    auto unsupported=settings; unsupported.output=static_cast<mfgunlock::xemfg::native::LatencyOutput>(3);
    Require(!configure(&unsupported,sizeof(unsupported)),"unsupported output accepted");
    auto oldAbi=settings; oldAbi.version=3;
    Require(!configure(&oldAbi,sizeof(oldAbi)),"old settings padding interpreted as UI composition");
    oldAbi.version=4; Require(!configure(&oldAbi,sizeof(oldAbi)),"old display-status ABI admitted");
    controlStatus(&cs,sizeof(cs)); Require(cs.settings.output==settings.output,"invalid selection changed live policy");
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC q{};
    Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
    WNDCLASSW wc{}; wc.lpfnWndProc=TestWindowProc; wc.lpszClassName=L"XeMFGNativeSmoke"; wc.hInstance=GetModuleHandleW(nullptr); RegisterClassW(&wc);
    HWND hwnd=CreateWindowW(wc.lpszClassName,L"XeMFG native bridge smoke",WS_OVERLAPPEDWINDOW,0,0,640,360,nullptr,nullptr,wc.hInstance,nullptr);
    Require(hwnd,"window");
    if(hotkeyTest) { ShowWindow(hwnd,SW_SHOWNORMAL); SetForegroundWindow(hwnd); }
    DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=640; desc.Height=360; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=3; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if(flag("--capcom-flags")) desc.Flags=0x842; // waitable, mode switch, tearing: captured RE Engine description
    if(legacyFactory) desc.Flags=0x802; // Control Resonant: legacy API, tearing + mode switch
    if(flag("--parallel-factory")) {
      const auto secondWindow=CreateWindowW(wc.lpszClassName,L"Independent factory caller",WS_OVERLAPPEDWINDOW,0,0,640,360,nullptr,nullptr,wc.hInstance,nullptr);
      Require(secondWindow,"second factory window");
      std::promise<void> start;auto gate=start.get_future().share();
      auto create=[&](HWND window) {
        gate.wait();ComPtr<IDXGISwapChain1> result;
        Check(factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,nullptr,nullptr,&result),"parallel creation");return result;
      };
      auto first=std::async(std::launch::async,create,hwnd);
      auto second=std::async(std::launch::async,create,secondWindow);start.set_value();
      const auto end=GetTickCount64()+30000;
      while((first.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready
          || second.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) && GetTickCount64()<end) {
        MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg);Sleep(1);
      }
      auto a=first.get(),b=second.get();
      mfgunlock::xemfg::Status status;modelStatus(&status,sizeof(status));
      Require(status.contexts==1,"concurrent factory callers published multiple Intel owners");
      a.Reset();b.Reset();modelStatus(&status,sizeof(status));Require(status.contexts==0,"parallel output teardown leaked");
      DestroyWindow(secondWindow);DestroyWindow(hwnd);
      Require(Import<PFun_slShutdown*>(fixture,"slShutdown")()==sl::Result::eOk,"parallel shutdown");
      Require(Import<bool(*)(uint32_t)>(addon,"MFGAXeLogFlush")(2000),"parallel trace flush");
      std::cout<<"PASS: simultaneous factory callers published one Intel owner; both outputs and contexts retired"<<std::endl;
      return 0;
    }
    ComPtr<IDXGISwapChain1> base;
    if(legacyFactory) {
      // An unsupported XeFG format must still return the original native
      // output, without publishing an Intel context or a manual final overlay.
      auto unsupported=desc; unsupported.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
      Check(CreateTestSwapchain(true,factory.Get(),queue.Get(),hwnd,unsupported,nullptr,&base),"legacy native fallback");
      DXGI_SWAP_CHAIN_DESC1 actual{}; Check(base->GetDesc1(&actual),"fallback description");
      Require(actual.Format==unsupported.Format && actual.BufferCount==unsupported.BufferCount
          && actual.Flags==unsupported.Flags,"legacy fallback changed native description");
      mfgunlock::xemfg::native::Status fallback; bridgeStatus(&fallback,sizeof(fallback));
      mfgunlock::xemfg::native::OverlayStatus overlay; overlayStatus(&overlay,sizeof(overlay));
      Require(!fallback.output && overlay.created==0,"legacy fallback falsely published Intel/manual overlay");
      base.Reset();
      std::cout<<"Legacy native fallback: FP16 unchanged; no Intel context or manual final overlay"<<std::endl;
    }
    // Normal inference runs in the game's windowed mode. The bridge must
    // preserve it even if a legacy INI still requests forced borderless.
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC exclusive{}; exclusive.Windowed=TRUE;
    ComPtr<ID3D12CommandQueue> proxyQueue;
    NativeQueueProxy* queueProxy{};
    if(flag("--queue-proxy")) {queueProxy=new NativeQueueProxy(queue.Get());proxyQueue.Attach(queueProxy);}
    Check(CreateTestSwapchain(legacyFactory,factory.Get(),proxyQueue?proxyQueue.Get():queue.Get(),hwnd,desc,&exclusive,&base),"swapchain");
    ComPtr<IDXGISwapChain4> chain; Check(base.As(&chain),"chain4"); base.Reset();
    mfgunlock::xemfg::native::Status bs; bridgeStatus(&bs,sizeof(bs));
    Require(bs.output && bs.xell && bs.streamline,"native output/Streamline/XeLL not ready");
    if(retainNativeOutput) {
      Require(foreignNativeA && foreignNativeB,"observer did not retain native references");
      chain.Reset();
      mfgunlock::xemfg::Status status;modelStatus(&status,sizeof(status));Require(status.contexts==0,"SDK context retained by a native reference");
      if(flag("--retained-output")) {
        const auto expectedNative=foreignNativeA.Get();
        Check(factory->CreateSwapChainForHwnd(queue.Get(),hwnd,&desc,nullptr,nullptr,&base),"retained native recreation");
        Require(foreignNativeA.Get()==expectedNative,"observer native identity changed");
        ComPtr<ID3D12Resource> buffer;Check(base->GetBuffer(0,IID_PPV_ARGS(&buffer)),"rebound output buffer");buffer.Reset();
        Check(base.As(&chain),"rebound Intel interface");base.Reset();
        Require(foreignNativeA && foreignNativeB,"observer references stolen");
        retainNativeOutput=false;foreignNativeB.Reset();foreignNativeA.Reset();
      } else {
      const auto refused=factory->CreateSwapChainForHwnd(queue.Get(),hwnd,&desc,nullptr,nullptr,&base);
      Require(FAILED(refused) && !base,"native HWND recreated while foreign native references remained");
      retainNativeOutput=false;foreignNativeB.Reset();foreignNativeA.Reset();
      Check(factory->CreateSwapChainForHwnd(queue.Get(),hwnd,&desc,nullptr,nullptr,&base),"recreate after owned observer releases");
      base.Reset();modelStatus(&status,sizeof(status));Require(status.contexts==0,"recovered output leaked");
      DestroyWindow(hwnd);Require(Import<PFun_slShutdown*>(fixture,"slShutdown")()==sl::Result::eOk,"foreign reference shutdown");
      Require(Import<bool(*)(uint32_t)>(addon,"MFGAXeLogFlush")(2000),"foreign reference trace flush");
      std::cout<<"PASS: foreign native references block HWND recreation; only observer-owned releases permit recovery"<<std::endl;
      return 0;
      }
    }
    if(queueProxy) Require(queueProxy->References()==1,"Intel context retained the external queue wrapper");
    mfgunlock::xemfg::native::OverlayStatus os;
    Require(overlayStatus(&os,sizeof(os)) && os.created==(flag("--retained-output")?2u:1u) && os.failed==0,"final output has no manual ReShade runtime");
    const auto windowStyle=GetWindowLongPtrW(hwnd,GWL_STYLE);
    const auto windowExStyle=GetWindowLongPtrW(hwnd,GWL_EXSTYLE);
    RECT windowRect{};GetWindowRect(hwnd,&windowRect);
    settings.forceBorderless=true; Require(configure(&settings,sizeof(settings)),"legacy borderless preference");
    { MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg); }
    if(!flag("--steam-observer")) {
      RECT after{};GetWindowRect(hwnd,&after);controlStatus(&cs,sizeof(cs));
      Require(GetWindowLongPtrW(hwnd,GWL_STYLE)==windowStyle && GetWindowLongPtrW(hwnd,GWL_EXSTYLE)==windowExStyle
          && EqualRect(&windowRect,&after),"XeMFG changed the game's display mode");
      Require(!cs.settings.forceBorderless && !cs.window.managed,"legacy preference still controls window mode");
    }
    settings.forceBorderless=false; Require(configure(&settings,sizeof(settings)),"game display preference");
    { MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg); }
    auto resolve=Import<PFun_slGetFeatureFunction*>(fixture,"slGetFeatureFunction");
    auto setLoaded=Import<PFun_slSetFeatureLoaded*>(fixture,"slSetFeatureLoaded");
    auto isLoaded=Import<PFun_slIsFeatureLoaded*>(fixture,"slIsFeatureLoaded");
    bool loaded{};
    Require(isLoaded(sl::kFeatureDLSS_G,loaded)==sl::Result::eOk && loaded,"replacement not logically available");
    Require(!Import<uint32_t(*)()>(fixture,"FixturePhysicalFgLoaded")(),"NVIDIA presentation hooks still own the Intel output");
    Require(setLoaded(sl::kFeatureDLSS_G,false)==sl::Result::eOk && isLoaded(sl::kFeatureDLSS_G,loaded)==sl::Result::eOk && !loaded,"feature disable not preserved");
    Require(setLoaded(sl::kFeatureDLSS_G,true)==sl::Result::eOk && isLoaded(sl::kFeatureDLSS_G,loaded)==sl::Result::eOk && loaded,"replacement re-enable not preserved");
    Require(!Import<uint32_t(*)()>(fixture,"FixturePhysicalFgLoaded")(),"re-enable restored conflicting NVIDIA hooks");
    Require(setLoaded(sl::kFeatureReflex,false)==sl::Result::eOk && isLoaded(sl::kFeatureReflex,loaded)==sl::Result::eOk && !loaded,"other feature disable not forwarded");
    Require(setLoaded(sl::kFeatureReflex,true)==sl::Result::eOk && isLoaded(sl::kFeatureReflex,loaded)==sl::Result::eOk && loaded,"other feature re-enable not forwarded");
    auto feature=[&]<class T>(sl::Feature id,const char* name) { void* fn{}; Require(resolve(id,name,fn)==sl::Result::eOk,name); return reinterpret_cast<T>(fn); };
    auto options=feature.operator()<PFun_slDLSSGSetOptions*>(sl::kFeatureDLSS_G,"slDLSSGSetOptions");
    auto getState=feature.operator()<PFun_slDLSSGGetState*>(sl::kFeatureDLSS_G,"slDLSSGGetState");
    auto reflex=feature.operator()<PFun_slReflexSetOptions*>(sl::kFeatureReflex,"slReflexSetOptions");
    auto sleep=feature.operator()<PFun_slReflexSleep*>(sl::kFeatureReflex,"slReflexSleep");
    auto marker=feature.operator()<PFun_slPCLSetMarker*>(flag("--legacy-markers")?sl::kFeatureReflex:sl::kFeaturePCL,
        flag("--legacy-markers")?"slReflexSetMarker":"slPCLSetMarker");
    auto secondaryMarker=(flag("--mixed-markers") || flag("--alias-markers"))?
        feature.operator()<PFun_slPCLSetMarker*>(sl::kFeatureReflex,"slReflexSetMarker"):nullptr;
    const auto primaryMarker=marker;
    const auto mark=[&](sl::PCLMarker value,const sl::FrameToken& token) {
      Require(primaryMarker(value,token)==sl::Result::eOk,"original marker result");
      if(secondaryMarker) Require(secondaryMarker(value,token)==sl::Result::eOk,"secondary marker result");
    };
    auto constants=Import<PFun_slSetConstants*>(fixture,"slSetConstants");
    auto tags=Import<PFun_slSetTagForFrame*>(fixture,"slSetTagForFrame");
    auto legacyTags=Import<PFun_slSetTag*>(fixture,"slSetTag");
    sl::ViewportHandle viewport{0}; sl::DLSSGOptions opt; opt.mode=sl::DLSSGMode::eOn; opt.numFramesToGenerate=3;
    if(flag("--secondary-viewport")) {
      sl::DLSSGOptions idle;idle.mode=sl::DLSSGMode::eOff;
      Require(options(sl::ViewportHandle{1},idle)==sl::Result::eOk,"secondary viewport OFF");
    }
    Require(options(viewport,opt)==sl::Result::eOk,"native FG disable");
    sl::ReflexOptions ro; ro.mode=sl::ReflexMode::eLowLatency; ro.frameLimitUs=25000;
    Require(reflex(ro)==sl::Result::eOk,"Reflex disable");
    Require(Import<uint32_t(*)()>(fixture,"FixtureModes")()==(nativeReflex?0x100u:0u),"native FG/Reflex mode routing");
    Require(Import<uint32_t(*)()>(fixture,"FixtureReflexLimit")()==(nativeReflex?25000u:0u),"frame cap duplicated/lost in latency translation");
    Require(cs.activeLatencyOutput==settings.output,"startup latency route changed");
    auto pending=settings; pending.output=nativeReflex?mfgunlock::xemfg::native::LatencyOutput::XeLL:mfgunlock::xemfg::native::LatencyOutput::Reflex;
    Require(configure(&pending,sizeof(pending)),"save alternate latency output");
    controlStatus(&cs,sizeof(cs)); Require(cs.latencyRestartPending && cs.activeLatencyOutput==settings.output,"UI-thread latency switch applied live");
    Require(configure(&settings,sizeof(settings)),"restore requested route");
    if(flag("--retire-sleep")) {
      Require(nativeReflex,"retirement stress requires native Reflex output");
      auto hold=Import<void(*)(bool)>(fixture,"FixtureHoldSleep");
      auto entered=Import<bool(*)()>(fixture,"FixtureSleepEntered");
      hold(true);
      Token pendingToken; pendingToken.id=400;
      auto pendingSleep=std::async(std::launch::async,[&] { return sleep(pendingToken); });
      const auto deadline=GetTickCount64()+3000;
      while(!entered() && GetTickCount64()<deadline) ::Sleep(1);
      Require(entered(),"source sleep did not enter the gate");
      chain.Reset();
      mfgunlock::xemfg::Status retired; modelStatus(&retired,sizeof(retired));
      if(retired.contexts) { hold(false); pendingSleep.wait(); throw std::runtime_error("latency lease retained the old FG presentation context"); }
      // Keep the old source call suspended while a new output binds the HWND.
      Check(CreateTestSwapchain(legacyFactory,factory.Get(),queue.Get(),hwnd,desc,nullptr,&base),"replacement during old source sleep");
      Check(base.As(&chain),"new output during old source sleep"); base.Reset();
      hold(false);
      const auto result=pendingSleep.get();
      Require(result==sl::Result::eErrorInvalidState,"retired source sleep published a stale frame");
      sl::DLSSGOptions retiredOff; retiredOff.mode=sl::DLSSGMode::eOff;
      Require(options(viewport,retiredOff)==sl::Result::eOk,"replacement native FG off");
      Check(chain->Present(0,0),"new passthrough output"); chain.Reset();
      modelStatus(&retired,sizeof(retired)); Require(!retired.contexts,"replacement context retained");
      DestroyWindow(hwnd);
      Require(Import<bool(*)(uint32_t)>(addon,"MFGAXeLogFlush")(2000),"retirement evidence flush");
      std::cout<<"PASS: source sleep lease kept XeLL alive; old FG context retired; same HWND rebound before sleep returned"<<std::endl;
      return 0;
    }
    ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
    auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr); uint64_t value=0;
    auto wait=[&] { Check(queue->Signal(fence.Get(),++value),"signal"); if(fence->GetCompletedValue()<value) {Check(fence->SetEventOnCompletion(value,event),"event"); Require(WaitForSingleObject(event,30000)==WAIT_OBJECT_0,"GPU timeout");} };
    ComPtr<ID3D12CommandAllocator> allocator; Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
    ComPtr<ID3D12GraphicsCommandList> list; Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list"); list->Close();
    auto texture=[&](DXGI_FORMAT format,D3D12_RESOURCE_FLAGS flags) {
      D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=640; d.Height=360;
      d.DepthOrArraySize=1; d.MipLevels=1; d.Format=format; d.SampleDesc.Count=1; d.Flags=flags;
      ComPtr<ID3D12Resource> r; Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)),"texture"); return r;
    };
    auto depth=texture(DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_FLAG_NONE);
    auto motion=texture(DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_FLAG_NONE);
    auto color=texture(DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    auto upload=[&](ID3D12Resource* resource,bool isDepth) {
      auto d=resource->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows{}; UINT64 rowBytes{},bytes{};
      device->GetCopyableFootprints(&d,0,1,0,&fp,&rows,&rowBytes,&bytes);
      D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_UPLOAD; D3D12_RESOURCE_DESC bd{};
      bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width=bytes; bd.Height=1; bd.DepthOrArraySize=1; bd.MipLevels=1; bd.SampleDesc.Count=1; bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      ComPtr<ID3D12Resource> buffer; Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&buffer)),"upload");
      void* p{}; buffer->Map(0,nullptr,&p); memset(p,0,size_t(bytes));
      if(isDepth) for(UINT y=0;y<rows;++y) for(UINT x=0;x<640;++x) reinterpret_cast<float*>(static_cast<char*>(p)+y*fp.Footprint.RowPitch)[x]=0.5f;
      buffer->Unmap(0,nullptr); allocator->Reset(); list->Reset(allocator.Get(),nullptr);
      D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=buffer.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=fp;
      D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=resource; dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      list->CopyTextureRegion(&dst,0,0,0,&src,nullptr); Barrier(list.Get(),resource,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      list->Close(); ID3D12CommandList* raw=list.Get(); queue->ExecuteCommandLists(1,&raw); wait();
    };
    upload(depth.Get(),true); upload(motion.Get(),false);
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors=2;
    ComPtr<ID3D12DescriptorHeap> rtvs; Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvs)),"rtvs");
    auto rtv=rtvs->GetCPUDescriptorHandleForHeapStart(); auto second=rtv; second.ptr+=device->GetDescriptorHandleIncrementSize(hd.Type);
    device->CreateRenderTargetView(color.Get(),nullptr,second);
    sl::Constants c; DirectX::XMFLOAT4X4 projection;
    DirectX::XMStoreFloat4x4(&projection,DirectX::XMMatrixPerspectiveFovLH(1.0f,640.f/360.f,.1f,1000.f)); memcpy(&c.cameraViewToClip,&projection,64);
    c.cameraRight={1,0,0}; c.cameraUp={0,1,0}; c.cameraFwd={0,0,1}; c.cameraPos={0,0,0};
    c.jitterOffset={0,0}; c.mvecScale={1.f/640,1.f/360}; c.depthInverted=flag("--normal-depth")?sl::eFalse:sl::eTrue; c.cameraMotionIncluded=sl::eTrue;
    c.motionVectors3D=sl::eFalse; c.reset=sl::eFalse;
    sl::Resource rd(sl::ResourceType::eTex2d,depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource rm(sl::ResourceType::eTex2d,motion.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource rc(sl::ResourceType::eTex2d,color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::ResourceTag tagged[]={ {&rd,sl::kBufferTypeDepth,sl::eValidUntilPresent},
      {&rm,sl::kBufferTypeMotionVectors,sl::eValidUntilPresent}, {&rc,sl::kBufferTypeHUDLessColor,sl::eValidUntilPresent} };
    HMODULE observer{};
    if(flag("--steam-observer")) {observer=LoadLibraryW(L".\\gameoverlayrenderer64.dll");Require(observer,"independent observer fixture");}
    if(flag("--stale-input")) {
      const auto hold=Import<void(*)(bool)>(fixture,"FixtureHoldConstants");
      const auto entered=Import<bool(*)()>(fixture,"FixtureConstantsEntered");
      hold(true);Token oldToken;oldToken.id=500;
      auto pending=std::async(std::launch::async,[&] {return constants(c,oldToken,viewport);});
      const auto deadline=GetTickCount64()+3000;
      while(!entered() && GetTickCount64()<deadline)Sleep(1);
      if(!entered()) {hold(false);pending.wait();throw std::runtime_error("constants did not enter the upstream gate");}
      const auto resized=chain->ResizeBuffers(0,640,360,DXGI_FORMAT_UNKNOWN,desc.Flags);
      hold(false);Require(pending.get()==sl::Result::eOk,"upstream constants result was modified");Check(resized,"resize during upstream constants");
      const auto exportReport=Import<uint32_t(*)(char*,uint32_t)>(addon,"MFGAXeCompatibilityReport");
      std::vector<char> report(exportReport(nullptr,0));Require(exportReport(report.data(),uint32_t(report.size()))>0,"compatibility report unavailable");
      Require(std::string(report.data()).find("\"stale_inputs_discarded\": 1")!=std::string::npos,"old constants crossed the resize epoch");
      std::cout<<"Stale source input: original return preserved; old epoch discarded before new-session admission"<<std::endl;
    }
    bool first=true;
    for(uint32_t frame=1;frame<=48;++frame) {
      if(observer && frame==12) {
        Require(Import<sl::Result(*)(HMODULE)>(observer,"ObserverDisable")(fixture)==sl::Result::eOk,"observer original options");
        Require(Import<uint32_t(*)(HMODULE)>(observer,"ObserverFrames")(fixture)==1,"observer received replacement frame state");
      }

      if(flag("--secondary-viewport") && frame==5) {
        sl::DLSSGOptions idle;idle.mode=sl::DLSSGMode::eOff;
        Require(options(sl::ViewportHandle{1},idle)==sl::Result::eOk,"secondary OFF while primary enabled");
      }
      if(keyTest && (frame==3 || frame==7 || frame==11 || frame==15)) {
        PostMessageW(hwnd,WM_KEYDOWN,VK_HOME,1);
      }
      if(keyTest && (frame==4 || frame==8 || frame==12 || frame==16)) PostMessageW(hwnd,WM_KEYUP,VK_HOME,LPARAM(1u<<31));
      if(keyTest) { MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg); }
      if(frame==36 || frame==42) {
        settings.sdkDeadlines=frame==36; Require(configure(&settings,sizeof(settings)),"deadline comparison request");
      }
      if(frame==8) {
        controlStatus(&cs,sizeof(cs));
        Require(cs.uiCompositionKnown && !cs.uiCompositionApplied && cs.uiCompositionUpdates==(flag("--stale-input")?1u:0u),"SDK default interpolation was overridden");
      }
      if(frame==9 || frame==25) {
        settings.uiComposition=frame==9;
        Require(configure(&settings,sizeof(settings)),"UI composition preference");
      }
      if(frame==16 || frame==32) {
        settings.flipMetering=frame==32;
        Require(configure(&settings,sizeof(settings)),"live flip request during inference");
      }
      if(frame==30) {
        controlStatus(&cs,sizeof(cs));
        Require(!cs.flipRequested && !cs.flipEnabled,"flip request did not commit at a batch boundary");
        Require(!cs.flipConfigured && !cs.flipMetered,"disabled metering still reported configured");
      }
      Token token; token.id=frame;
      if(!flag("--tail-sleep")) Require(sleep(token)==sl::Result::eOk,"XeLL sleep");
      else mark(static_cast<sl::PCLMarker>(6),token); // Actual legacy input marker before simulation.
      mark(sl::PCLMarker::eSimulationStart,token); mark(sl::PCLMarker::eSimulationEnd,token);
      std::future<sl::Result> tailSleep;
      if(flag("--tail-sleep")) tailSleep=std::async(std::launch::async,[sleep,&token] {return sleep(token);});
      mark(sl::PCLMarker::eRenderSubmitStart,token); constants(c,token,viewport);
      allocator->Reset(); list->Reset(allocator.Get(),nullptr);
      ComPtr<ID3D12Resource> back; Check(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&back)),"backbuffer");
      device->CreateRenderTargetView(back.Get(),nullptr,rtv);
      Barrier(list.Get(),back.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
      Barrier(list.Get(),color.Get(),first?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET); first=false;
      const float rgba[]={.1f,.2f,.3f,1.f}; list->ClearRenderTargetView(rtv,rgba,0,nullptr); list->ClearRenderTargetView(second,rgba,0,nullptr);
      Barrier(list.Get(),color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      Barrier(list.Get(),back.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
      // Cover both tag APIs and both lifetime contracts with real SDK copies.
      const bool volatileInputs=frame>=17 && frame<=32;
      ComPtr<ProducerCommandProxy> producer;
      if(volatileInputs && flag("--producer-shadow")) producer.Attach(new ProducerCommandProxy(list.Get()));
      const bool invalidProducer=producer && frame==22;
      if(invalidProducer) producer->RejectQueryImplementation();
      auto* producerCommand=reinterpret_cast<sl::CommandBuffer*>(producer?static_cast<ID3D12CommandList*>(producer.Get()):static_cast<ID3D12CommandList*>(list.Get()));
      for(auto& tag:tagged) tag.lifecycle=volatileInputs?sl::eOnlyValidNow:sl::eValidUntilPresent;
      const uint32_t tagCount=frame==34?1:frame==17?2:3; // Missing MV bypass; missing HUD-less uses UI interpolation.
      if(frame%2) tags(token,viewport,tagged,tagCount,producerCommand);
      else legacyTags(viewport,tagged,tagCount,producerCommand);
      if(frame==37 || frame==38) {
        sl::ResourceTag invalidOptional{&rm,frame==37?sl::kBufferTypeUIColorAndAlpha:sl::kBufferTypeHUDLessColor,sl::eValidUntilPresent};
        tags(token,viewport,&invalidOptional,1,reinterpret_cast<sl::CommandBuffer*>(list.Get()));
      }
      if(frame==39 && flag("--hud-conflict")) {
        sl::ResourceTag removed{nullptr,sl::kBufferTypeHUDLessColor,sl::eValidUntilPresent};
        Require(tags(token,viewport,&removed,1,producerCommand)==sl::Result::eOk,"original removed-tag result");
      }
      if(frame==40 && flag("--hud-conflict")) {
        auto reject=Import<void(*)(bool)>(fixture,"FixtureRejectTags"); reject(true);
        Require(tags(token,viewport,tagged,1,producerCommand)==sl::Result::eErrorInvalidState,"original input error was masked");
        reject(false);
      }
      list->Close(); ID3D12CommandList* raw=list.Get(); queue->ExecuteCommandLists(1,&raw);
      const bool lateSubmitEnd=frame>=17;
      if(!lateSubmitEnd) mark(sl::PCLMarker::eRenderSubmitEnd,token);
      mark(sl::PCLMarker::ePresentStart,token);
      Check(chain->Present(0,0),"Present"); mark(sl::PCLMarker::ePresentEnd,token); wait();
      if(lateSubmitEnd) mark(sl::PCLMarker::eRenderSubmitEnd,token);
      if(tailSleep.valid()) Require(tailSleep.get()==sl::Result::eOk,"tail source sleep");
      if(flag("--tail-sleep")) {
        if(frame==5) Require(sleep(token)==sl::Result::eOk,"duplicate tail sleep result");
        if(frame==1 || frame==3) {
          bridgeStatus(&bs,sizeof(bs));
          Require(frame==1?!bs.enabled:bs.enabled,"tail wait did not reach the following source frame");
        }
      }
      if(observer && frame==12) {bridgeStatus(&bs,sizeof(bs));Require(bs.enabled,"observer options changed the game's FG request");}
      if(frame==9 || frame==17 || frame==18 || frame==25) {
        controlStatus(&cs,sizeof(cs));
        const bool composed=frame==9 || frame==18;
        Require(cs.uiCompositionKnown && cs.uiCompositionResult>=0 && cs.uiCompositionApplied==composed,"UI composition effective state mismatch");
        Require(cs.settings.uiComposition==(frame!=25),"missing HUD-less erased UI preference");
      }
      if(frame==34 || frame==38 || invalidProducer || ((frame==39 || frame==40) && flag("--hud-conflict"))) {
        bridgeStatus(&bs,sizeof(bs)); Require(!bs.enabled,"invalid input did not bypass FG");
        if(frame==39) { controlStatus(&cs,sizeof(cs)); Require(cs.hudIssues&16u,"conflicting removal not diagnosed"); }
      }
      else if(lateSubmitEnd) { bridgeStatus(&bs,sizeof(bs)); Require(bs.enabled,producer?"heap producer command was not accepted":"valid overlapping RenderSubmit/Present was rejected"); }
      if(frame==37) {
        controlStatus(&cs,sizeof(cs)); Require(cs.hudIssues && cs.hudRejected && !cs.uiCompositionApplied,"invalid optional HUD did not fall back to default interpolation");
      }
      if(frame==40 || frame==48) {
        controlStatus(&cs,sizeof(cs)); Require(cs.sdkDeadlinesActive==(frame==40),"deadline policy did not commit/restore");
      }
      MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg);
    }
    if(flag("--producer-shadow")) std::cout<<"Heap producer commands: accepted; non-code implementation: rejected; generation resumed"<<std::endl;
    if(flag("--hud-conflict")) std::cout<<"HUD tag removal: admitted allocation pinned; conflicting batch bypassed; generation resumed"<<std::endl;
    mfgunlock::xemfg::Status ms; modelStatus(&ms,sizeof(ms)); bridgeStatus(&bs,sizeof(bs));
    Sleep(100);
    auto refresh=Import<void(*)()>(addon,"MFGAXeNativeRefresh"); refresh();
    modelStatus(&ms,sizeof(ms));
    std::cout<<"model="<<ms.model<<" initialized="<<ms.initialized<<" frames="<<ms.presented<<" tensor="<<ms.tensor_launches<<" result="<<ms.result<<" rejected="<<bs.rejected<<" outputPresents="<<bs.outputPresents<<std::endl;
    // An occluded window can report only its generated submissions. Do not
    // turn this into a claim about four displayed frames or scanout cadence.
    Require(ms.model && ms.initialized && ms.tensor_launches>0 && ms.generated==multiplier-1 && ms.presented>=multiplier-1 && ms.result==0 && bs.enabled,"actual XeMFG interpolation missing");
    Require(bs.lateSubmitEnds>=30,"late RenderSubmitEnd timestamps were not forwarded to XeLL");
    Require(overlayStatus(&os,sizeof(os)) && os.updates>=48 && os.failed==0,"ReShade did not render final outputs");
    if(keyTest) {
      std::cout<<"Production panel draws="<<panelDraws()<<std::endl;
      Require(panelDraws()>0,"Home test never rendered the production MFG Unlock panel");
      std::cout<<"Overlay key transitions: opens="<<overlayOpens<<" closes="<<overlayCloses<<std::endl;
      Require(overlayOpens==2 && overlayCloses==2,"native ReShade Home transitions lost");
      mfgunlock::xemfg::native::OverlayInputStatus input;
      Require(overlayInput(&input,sizeof(input)) && input.recovered==0 && input.dropped==0,"addon-owned key recovery is still active");
    }
    controlStatus(&cs,sizeof(cs));
    Require(cs.cudaState==1 && cs.cudaLaunches==ms.tensor_launches && cs.cudaLayers>0,"CUDA status not tied to real inference");
    Require(cs.reflexObserved && cs.reflexOptionsReady && (nativeReflex?(cs.reflexOutputObserved && !cs.xellObserved && !cs.nativeReflexDisabled && cs.reflexSleeps==48 && cs.xellSleeps==0):(!cs.reflexOutputObserved && cs.xellObserved && cs.nativeReflexDisabled && cs.reflexSleeps==0 && cs.xellSleeps==48)) && cs.sleeps==48 && cs.markers>=(flag("--tail-sleep")?47u:48u)*6,
        "observed latency route/counters missing");
    Require(Import<uint32_t(*)()>(fixture,"FixtureReflexSleeps")()==(nativeReflex?48u:0u),"original Reflex wait dispatch count");
    if(nativeReflex) Require(Import<uint32_t(*)()>(fixture,"FixtureLastReflexSleepToken")()==48,"native Reflex token was shifted");
    std::cout<<"Latency route: Reflex -> "<<(nativeReflex?"Reflex":"XeLL")<<"; native waits="<<cs.reflexSleeps<<"; XeLL waits="<<cs.xellSleeps<<std::endl;
    if(flag("--tail-sleep")) std::cout<<"Tail sleep: first frame bypassed, following frames interpolated, original Reflex token and wait count preserved"<<std::endl;
    Require(cs.modeUpdates<=(flag("--retained-output")?3u:2u),"identical XeLL configuration reapplied every frame");
    // An invalid producer removes this frame's HUD-less input, then the next
    // valid frame restores it: one additional disable/enable transition.
    const uint64_t compositionTransitions=(flag("--producer-shadow")?6:4)+(flag("--stale-input")?1:0);
    Require(!cs.settings.uiComposition && !cs.uiCompositionApplied && cs.uiCompositionUpdates==compositionTransitions,"UI composition setter repeated or did not restore default");
    const auto requiredBytes=timingReport(nullptr,0); std::vector<char> cpuReport(requiredBytes);
    Require(timingReport(cpuReport.data(),uint32_t(cpuReport.size()))==requiredBytes,"timing report size changed without new Presents");
    Require(std::string(cpuReport.data()).find("Samples=48")!=std::string::npos,"actual Present calls missing from timing ring");
    std::cout<<cpuReport.data(); timingEnable(false);
    settings.flipMetering=false; Require(configure(&settings,sizeof(settings)),"flip toggle off");
    controlStatus(&cs,sizeof(cs)); Require(!cs.flipRequested,"flip request did not reach pacer");
    settings.flipMetering=true; Require(configure(&settings,sizeof(settings)),"flip toggle on");
    sl::DLSSGState live; Require(getState(viewport,live,nullptr)==sl::Result::eOk,"GetState bridge");
    Require(live.numFramesActuallyPresented==ms.presented && live.numFramesToGenerateMax==5,"requested and submitted counts confused");
    auto fixtureState=Import<void(*)(uint32_t)>(fixture,"FixtureFgStatus");
    fixtureState(2|16);
    Require(getState(viewport,live,nullptr)==sl::Result::eOk && uint32_t(live.status)==0,"disabled NVIDIA-only status leaked to Intel output");
    fixtureState(2|16|1|4|8);
    Require(getState(viewport,live,nullptr)==sl::Result::eOk && uint32_t(live.status)==(1|4|8),"real format/resolution/input errors were masked");
    fixtureState(0);
    live.structVersion=1; live.numFramesToGenerateMax=0xdeadbeef;
    Require(getState(viewport,live,nullptr)==sl::Result::eOk && live.numFramesToGenerateMax==0xdeadbeef,"V1 state extension overwritten");
    sl::DLSSGOptions off; off.mode=sl::DLSSGMode::eOff; options(viewport,off);
    Check(chain->Present(0,0),"disabled Present");
    Check(chain->ResizeBuffers(3,800,450,DXGI_FORMAT_UNKNOWN,desc.Flags),"ResizeBuffers");
    if(keyTest) {
      for(unsigned i=0;i<2;++i) {
        // Configure the rebound runtime before sending a new completed tap.
        Check(chain->Present(0,0),"resized runtime warmup");
        PostMessageW(hwnd,WM_KEYDOWN,VK_HOME,1);
        MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg);
        Check(chain->Present(0,0),"resized runtime Home press"); wait();
        PostMessageW(hwnd,WM_KEYUP,VK_HOME,LPARAM(1u<<31));
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg);
        Check(chain->Present(0,0),"resized runtime Home release"); wait();
      }
      Require(overlayOpens==3 && overlayCloses==3,"native Home did not survive resize or FG-off output");
      std::cout<<"Overlay keys after resize/FG-off: opens="<<overlayOpens<<" closes="<<overlayCloses<<std::endl;
      // ReShade also owns keyboard input from children of the render window.
      const auto child=CreateWindowW(wc.lpszClassName,L"input child",WS_CHILD,0,0,32,32,hwnd,nullptr,wc.hInstance,nullptr);
      Require(child,"input child");
      PostMessageW(child,WM_KEYDOWN,VK_HOME,1);
      { MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg); }
      Check(chain->Present(0,0),"child held Home"); wait();
      PostMessageW(child,WM_KEYUP,VK_HOME,LPARAM(1u<<31));
      { MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg); }
      Check(chain->Present(0,0),"child Home release"); wait();
      PostMessageW(child,WM_KEYDOWN,VK_HOME,1);
      { MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg); }
      Check(chain->Present(0,0),"second child Home press"); wait();
      PostMessageW(child,WM_KEYUP,VK_HOME,LPARAM(1u<<31));
      { MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg); }
      Check(chain->Present(0,0),"second child Home release"); wait();
      std::cout<<"Overlay keys with child input: opens="<<overlayOpens<<" closes="<<overlayCloses<<std::endl;
      Require(overlayOpens==4 && overlayCloses==4,"Home tap from render-window child was lost");
      mfgunlock::xemfg::native::OverlayInputStatus childStatus;
      Require(overlayInput(&childStatus,sizeof(childStatus)) && childStatus.recovered==0 && !childStatus.dropped,
          "child input was intercepted by the addon");
      DestroyWindow(child);
      if(hotkeyTest) {
        // SendInput is used ONLY after confirming this test's own window is
        // foreground. Never send keys to a game or another application.
        ShowWindow(hwnd,SW_SHOWNORMAL); SetForegroundWindow(hwnd);
        const auto focusDeadline=GetTickCount64()+3000;
        auto pump=[&] { MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg); };
        while(GetForegroundWindow()!=hwnd && GetTickCount64()<focusDeadline) { SetForegroundWindow(hwnd); pump(); Sleep(5); }
        Require(GetForegroundWindow()==hwnd,"own test window not foreground; no input sent");
        for(unsigned i=0;i<5;++i) { pump(); Check(chain->Present(0,0),"hotkey focus warmup"); wait(); }
        auto press=[&](WORD key) {
          Require(GetForegroundWindow()==hwnd,"focus left test window; no input sent");
          INPUT input{}; input.type=INPUT_KEYBOARD; input.ki.wVk=key;
          Require(SendInput(1,&input,sizeof(INPUT))==1,"own test key press failed");
          for(unsigned i=0;i<8;++i) { pump(); Check(chain->Present(0,0),"native key press"); wait(); Sleep(5); }
          input.ki.dwFlags=KEYEVENTF_KEYUP;
          Require(SendInput(1,&input,sizeof(INPUT))==1,"own test key release failed");
          for(unsigned i=0;i<8;++i) { pump(); Check(chain->Present(0,0),"native key release"); wait(); Sleep(5); }
        };
        press(VK_F23); const auto beforeCommands=commands;
        Require(beforeCommands>0,"closed overlay captures test game commands");
        press(VK_HOME);
        Require(overlayOpens==5 && overlayCloses==4,"ReShade Home did not open exactly once");
        press(VK_F23); Require(commands==beforeCommands,"open overlay did not capture test commands");
        press(VK_HOME);
        Require(overlayOpens==5 && overlayCloses==5,"ReShade Home could not close input-capturing overlay");
        press(VK_F23); Require(commands==beforeCommands+1,"closing overlay did not release game commands");
        SendMessageW(hwnd,WM_ACTIVATEAPP,FALSE,0);
        SendMessageW(hwnd,WM_ACTIVATEAPP,TRUE,0); pump();
        press(VK_HOME); press(VK_HOME);
        Require(overlayOpens==6 && overlayCloses==6,"native Home did not survive reactivation");
        ShowWindow(hwnd,SW_HIDE); pump();
        std::cout<<"Foreground ReShade: opens=6 closes=6; production panel drawn; game commands captured/released\n";
      }
    }
    // The foreign peer re-enters AfterPresent on a separate thread while our
    // output is retiring. It must see the retired epoch without an io deadlock.
    if(observer)std::cout<<"Observer isolation: native plugin state preserved; game inference continued after observer OFF"<<std::endl;
    {
      auto exportReport=Import<uint32_t(*)(char*,uint32_t)>(addon,"MFGAXeCompatibilityReport");
      const auto size=exportReport(nullptr,0);Require(size>100,"empty compatibility metadata");
      char tiny[2]={'x','y'};
      Require(exportReport(tiny,2)>2 && tiny[0]=='x' && tiny[1]=='y',"undersized support buffer was overwritten");
      std::vector<char> json(size);Require(exportReport(json.data(),uint32_t(json.size()))==size,"unstable compatibility export size");
      const std::string text(json.data());
      Require(text.find("mfgunlock.compatibility")!=std::string::npos && text.find("\"version\": 1")!=std::string::npos,"compatibility schema missing");
      Require(text.find("C:\\")==std::string::npos && text.find("reframework_path")==std::string::npos,"personal path leaked in compatibility export");
      std::ofstream("compatibility.json",std::ios::binary)<<text;
    }
    if(refLate) {refFixture=LoadLibraryW(L".\\reframework-v1-fixture.dll");Require(refFixture,"late REFramework load");}
    if(refAmbiguous) {refOther=LoadLibraryW(L".\\reframework-legacy-fixture.dll");Require(refOther,"late second REFramework fixture");}
    uint32_t(*refCalls)(){};
    uint32_t(*refCallbacks)(){};
    if(refTest) {
      refCalls=Import<uint32_t(*)()>(refFixture,"REFTestCalls");
      refCallbacks=Import<uint32_t(*)()>(refFixture,"REFTestCallbacks");
      const auto response=flag("--ref-blocked")?2u:flag("--ref-invalid")?99u:flag("--ref-exception")?0xfffffffeu:flag("--ref-not-tracked")?0u:1u;
      Import<void(*)(uint32_t,void(*)())>(refFixture,"REFTestConfigure")(response,Import<void(*)()>(addon,"MFGAXeNativeRefresh"));
    }
    chain.Reset();
    if(refBlocked) {
      modelStatus(&ms,sizeof(ms));Require(ms.contexts==1,"blocked peer released the Intel context");
      Require(overlayStatus(&os,sizeof(os)) && os.created>os.destroyed,"blocked peer released final native proxy/runtime");
      Require(refCalls()==(refAmbiguous?0u:1u),"failed peer called repeatedly or ambiguous peer invoked");
      if(!refAmbiguous)Require(refCallbacks()==1,"foreign callback could not re-enter after closing admission");
      const auto refused=CreateTestSwapchain(legacyFactory,factory.Get(),queue.Get(),hwnd,desc,nullptr,&base);
      Require(FAILED(refused) && !base,"new context created after refused retirement");
      modelStatus(&ms,sizeof(ms));Require(ms.contexts==1,"retry destroyed quarantined context");
      Require(refCalls()==(refAmbiguous?0u:1u),"automatic handshake retry");
      Require(Import<bool(*)(uint32_t)>(addon,"MFGAXeLogFlush")(2000),"blocked handoff log flush");
      std::cout<<"PASS: REFramework refusal preserved proxy/context, blocked recreation and avoided retry"<<std::endl;
      return 0; // quarantine deliberately lives until process exit
    }
    Require(overlayStatus(&os,sizeof(os)) && os.created>=2 && os.created==os.destroyed && os.failed==0,"manual ReShade runtime resize/teardown leaked");
    sl::ViewportHandle replacementViewport{7};
    Require(options(replacementViewport,off)==sl::Result::eOk,"retired output kept the old viewport selection");
    // RE Engine rebuilds its window output during boot and FG changes. A
    // successful teardown must release the old HWND binding before the next
    // Intel context is initialized on that same window.
    for(unsigned cycle=0;cycle<3;++cycle) {
      desc.Width=800; desc.Height=450; desc.BufferCount=cycle%2?4:3;
      desc.Format=DXGI_FORMAT_R10G10B10A2_UNORM;
      ComPtr<ID3D12CommandQueue> replacementQueue;
      Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&replacementQueue)),"replacement queue");
      Check(CreateTestSwapchain(legacyFactory,factory.Get(),replacementQueue.Get(),hwnd,desc,nullptr,&base),"same-window replacement");
      Check(base.As(&chain),"replacement chain4"); base.Reset();
      Require(options(replacementViewport,off)==sl::Result::eOk,"replacement FG options");
      Check(chain->Present(0,0),"replacement disabled Present");
      Check(replacementQueue->Signal(fence.Get(),++value),"replacement signal");
      Check(fence->SetEventOnCompletion(value,event),"replacement event");
      Require(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"replacement GPU timeout");
      chain.Reset();
      modelStatus(&ms,sizeof(ms)); Require(ms.contexts==0,"same-window replacement retained an Intel context");
      Require(overlayStatus(&os,sizeof(os)) && os.created==os.destroyed,"same-window replacement retained its ReShade runtime");
      MSG message{}; while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&message);
    }
    std::cout<<"Same-window output recreation: 3 cycles; context and overlay retired"<<std::endl;
    if(refTest) {
      Require(refCalls()==(refLegacy?0u:4u),"handoff did not run once per live context");
      Require(refCallbacks()==(refLegacy?0u:4u),"retirement held input locks across a foreign callback");
      std::cout<<"REFramework safe handoff: reentrant callback, module lease and recreation verified"<<std::endl;
    }
    CloseHandle(event); DestroyWindow(hwnd);
    modelStatus(&ms,sizeof(ms)); Require(ms.contexts==0,"context/COM lifetime leaked");
    Require(Import<PFun_slShutdown*>(fixture,"slShutdown")()==sl::Result::eOk,"fixture shutdown");
    Sleep(300); // The bounded discovery worker must stay out of retired plugins.
    Require(!Import<uint32_t(*)()>(fixture,"FixtureQueriesAfterShutdown")(),"discovery queried a retired Streamline session");
    std::cout<<"Presentation owner: Intel only; feature requests preserved; discovery stopped before shutdown"<<std::endl;
    Require(Import<bool(*)(uint32_t)>(addon,"MFGAXeLogFlush")(2000),"diagnostic log did not drain at test boundary");
    Require(!GetModuleHandleW(L"OptiScaler.dll") && !GetModuleHandleW(L"MFGAdaUnlock.XeMFG.asi"),"external companion loaded");
    if(flag("--legacy-markers") || secondaryMarker) {
      Require(Import<uint32_t(*)()>(fixture,"FixtureHistoricalMarkerCalls")()>=48*6 || flag("--alias-markers"),"original historical marker skipped");
      std::cout<<"Historical markers: original calls preserved, real inference and duplicate forwarding verified"<<std::endl;
    }
    if(legacyFactory) std::cout<<"Legacy factory: inference, resize, fallback and recreation verified"<<std::endl;
    if(flag("--retained-output"))std::cout<<"PASS: retained native HWND rebound to a new Intel context with observer references preserved; inference and resize verified"<<std::endl;
    if(queueProxy) {Require(queueProxy->References()==1,"external queue wrapper retained after teardown");std::cout<<"Queue native identity: wrapper unwrapped; inference and lifetime verified"<<std::endl;}
    std::cout<<"PASS: native ReShade addon, "<<multiplier<<"x requested, real XeMFG inference, Tensor launches, XeLL, UI composition default/opt-in/off/missing-HUDless, volatile/legacy tags, missing-input bypass, disable, resize and context retirement; synthetic/occluded, no display cadence or gameplay quality claim"<<std::endl;
    return 0;
  } catch(const std::exception& e) { std::cerr<<e.what()<<std::endl; return 1; }
}

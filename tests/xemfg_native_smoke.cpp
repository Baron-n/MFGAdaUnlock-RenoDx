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
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
static bool keyTest{};
static bool hotkeyTest{};
static unsigned overlayOpens{},overlayCloses{};
static unsigned commands{};
static bool selectPanel{};
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
struct Token : sl::FrameToken { uint32_t id{}; operator uint32_t() const override {return id;} };
template<class T> T Import(HMODULE m,const char* name) { auto p=reinterpret_cast<T>(GetProcAddress(m,name)); Require(p!=nullptr,name); return p; }
static void Barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; list->ResourceBarrier(1,&b);
}
int main(int argc,char** argv) {
  try {
    const unsigned multiplier=argc>1?unsigned(std::stoul(argv[1])):4;
    Require(multiplier>=2 && multiplier<=6,"test multiplier");
    const auto flag=[&](const char* name) { for(int i=2;i<argc;++i) if(std::string(argv[i])==name) return true; return false; };
    if(flag("--preload-runtimes")) {
      Require(LoadLibraryW(L".\\_storage_\\libxess_fg.dll")!=nullptr,"preload stored XeFG");
      Require(LoadLibraryW(L".\\_storage_\\libxell.dll")!=nullptr,"preload stored XeLL");
    }
    auto fixture=LoadLibraryW(L"sl.interposer.dll"); Require(fixture,"fixture");
    auto reshade=LoadLibraryW(L".\\dxgi.dll"); Require(reshade,"ReShade");
    hotkeyTest=flag("--overlay-foreground");
    keyTest=flag("--overlay-keys") || hotkeyTest;
    if(keyTest) {
      Require(reshade::register_addon(GetModuleHandleW(nullptr),reshade),"register keyboard test observer");
      reshade::register_event<reshade::addon_event::reshade_open_overlay>(&OverlayChange);
      reshade::register_event<reshade::addon_event::reshade_overlay>(&OverlayGui);
      selectPanel=true;
    }
    auto factoryCreate=Import<HRESULT(WINAPI*)(REFIID,void**)>(reshade,"CreateDXGIFactory1");
    ComPtr<IDXGIFactory4> factory; Check(factoryCreate(IID_PPV_ARGS(&factory)),"factory");
    ComPtr<ID3D12Device> device;
    for(UINT i=0;!device;++i) {
      ComPtr<IDXGIAdapter1> adapter;
      if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
      if(desc.VendorId==0x10de) D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device));
    }
    Require(device!=nullptr,"NVIDIA device");
    auto addon=GetModuleHandleW(L"renodx-mfgunlock-native.addon64"); Require(addon,"ReShade did not load the addon");
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
    Require(cs.version==4 && cs.settings.version==4 && !cs.settings.uiComposition && !cs.settings.sdkDeadlines,"default/ABI");
    const bool nativeReflex=cs.activeLatencyOutput==mfgunlock::xemfg::native::LatencyOutput::Reflex;
    auto settings=cs.settings; settings.input=mfgunlock::xemfg::native::LatencyInput::StreamlineReflex;
    settings.output=nativeReflex?mfgunlock::xemfg::native::LatencyOutput::Reflex:mfgunlock::xemfg::native::LatencyOutput::XeLL;
    settings.forceBorderless=true;
    Require(configure(&settings,sizeof(settings)),"explicit Reflex to XeLL preference");
    auto unsupported=settings; unsupported.output=static_cast<mfgunlock::xemfg::native::LatencyOutput>(3);
    Require(!configure(&unsupported,sizeof(unsupported)),"unsupported output accepted");
    auto oldAbi=settings; oldAbi.version=3;
    Require(!configure(&oldAbi,sizeof(oldAbi)),"old settings padding interpreted as UI composition");
    controlStatus(&cs,sizeof(cs)); Require(cs.settings.output==settings.output,"invalid selection changed live policy");
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC q{};
    Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
    WNDCLASSW wc{}; wc.lpfnWndProc=TestWindowProc; wc.lpszClassName=L"XeMFGNativeSmoke"; wc.hInstance=GetModuleHandleW(nullptr); RegisterClassW(&wc);
    HWND hwnd=CreateWindowW(wc.lpszClassName,L"XeMFG native bridge smoke",WS_OVERLAPPEDWINDOW,0,0,640,360,nullptr,nullptr,wc.hInstance,nullptr);
    Require(hwnd,"window");
    if(hotkeyTest) { ShowWindow(hwnd,SW_SHOWNORMAL); SetForegroundWindow(hwnd); }
    DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=640; desc.Height=360; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=3; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> base;
    // Simulate a game's exclusive startup request. The persisted force policy
    // must turn it into a valid Intel windowed output before SDK init.
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC exclusive{}; exclusive.Windowed=FALSE;
    Check(factory->CreateSwapChainForHwnd(queue.Get(),hwnd,&desc,&exclusive,nullptr,&base),"swapchain");
    ComPtr<IDXGISwapChain4> chain; Check(base.As(&chain),"chain4"); base.Reset();
    mfgunlock::xemfg::native::Status bs; bridgeStatus(&bs,sizeof(bs));
    Require(bs.output && bs.xell && bs.streamline,"native output/Streamline/XeLL not ready");
    mfgunlock::xemfg::native::OverlayStatus os;
    Require(overlayStatus(&os,sizeof(os)) && os.created==1 && os.failed==0,"final output has no manual ReShade runtime");
    settings.forceBorderless=true; Require(configure(&settings,sizeof(settings)),"borderless request");
    { MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg); }
    // The owner-thread controller is asynchronous; SDK/ReShade startup may
    // still post window notifications after CreateSwapChain returns.
    const auto windowDeadline=GetTickCount64()+2000;
    do {
      MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg);
      controlStatus(&cs,sizeof(cs));
      if(cs.window.borderless && cs.window.coversMonitor && !cs.window.pending) break;
      Sleep(2);
    } while(GetTickCount64()<windowDeadline);
    if(!cs.window.borderless || !cs.window.coversMonitor) {
      RECT rect{}; GetWindowRect(hwnd,&rect);
      std::cerr<<"borderless available="<<cs.window.available<<" pending="<<cs.window.pending<<" managed="<<cs.window.managed
          <<" error="<<cs.window.error<<" borders="<<cs.window.borderless<<" monitor="<<cs.window.coversMonitor
          <<" rect="<<rect.left<<','<<rect.top<<','<<rect.right<<','<<rect.bottom<<std::endl;
    }
    Require(cs.window.borderless && cs.window.coversMonitor,"overlay borderless not applied");
    Check(chain->SetFullscreenState(TRUE,nullptr),"exclusive request redirected to borderless");
    settings.forceBorderless=false; Require(configure(&settings,sizeof(settings)),"window restore request");
    { MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg); }
    controlStatus(&cs,sizeof(cs)); Require(!cs.window.managed && !cs.window.error,"overlay window restore failed");
    auto resolve=Import<PFun_slGetFeatureFunction*>(fixture,"slGetFeatureFunction");
    auto feature=[&]<class T>(sl::Feature id,const char* name) { void* fn{}; Require(resolve(id,name,fn)==sl::Result::eOk,name); return reinterpret_cast<T>(fn); };
    auto options=feature.operator()<PFun_slDLSSGSetOptions*>(sl::kFeatureDLSS_G,"slDLSSGSetOptions");
    auto getState=feature.operator()<PFun_slDLSSGGetState*>(sl::kFeatureDLSS_G,"slDLSSGGetState");
    auto reflex=feature.operator()<PFun_slReflexSetOptions*>(sl::kFeatureReflex,"slReflexSetOptions");
    auto sleep=feature.operator()<PFun_slReflexSleep*>(sl::kFeatureReflex,"slReflexSleep");
    auto marker=feature.operator()<PFun_slPCLSetMarker*>(sl::kFeaturePCL,"slPCLSetMarker");
    auto constants=Import<PFun_slSetConstants*>(fixture,"slSetConstants");
    auto tags=Import<PFun_slSetTagForFrame*>(fixture,"slSetTagForFrame");
    auto legacyTags=Import<PFun_slSetTag*>(fixture,"slSetTag");
    sl::ViewportHandle viewport{0}; sl::DLSSGOptions opt; opt.mode=sl::DLSSGMode::eOn; opt.numFramesToGenerate=3;
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
    c.jitterOffset={0,0}; c.mvecScale={1.f/640,1.f/360}; c.depthInverted=sl::eTrue; c.cameraMotionIncluded=sl::eTrue;
    c.motionVectors3D=sl::eFalse; c.reset=sl::eFalse;
    sl::Resource rd(sl::ResourceType::eTex2d,depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource rm(sl::ResourceType::eTex2d,motion.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource rc(sl::ResourceType::eTex2d,color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::ResourceTag tagged[]={ {&rd,sl::kBufferTypeDepth,sl::eValidUntilPresent},
      {&rm,sl::kBufferTypeMotionVectors,sl::eValidUntilPresent}, {&rc,sl::kBufferTypeHUDLessColor,sl::eValidUntilPresent} };
    bool first=true;
    for(uint32_t frame=1;frame<=48;++frame) {
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
        Require(cs.uiCompositionKnown && !cs.uiCompositionApplied && cs.uiCompositionUpdates==0,"SDK default interpolation was overridden");
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
      Require(sleep(token)==sl::Result::eOk,"XeLL sleep");
      marker(sl::PCLMarker::eSimulationStart,token); marker(sl::PCLMarker::eSimulationEnd,token);
      marker(sl::PCLMarker::eRenderSubmitStart,token); constants(c,token,viewport);
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
      for(auto& tag:tagged) tag.lifecycle=volatileInputs?sl::eOnlyValidNow:sl::eValidUntilPresent;
      const uint32_t tagCount=frame==34?1:frame==17?2:3; // Missing MV bypass; missing HUD-less uses UI interpolation.
      if(frame%2) tags(token,viewport,tagged,tagCount,reinterpret_cast<sl::CommandBuffer*>(list.Get()));
      else legacyTags(viewport,tagged,tagCount,reinterpret_cast<sl::CommandBuffer*>(list.Get()));
      if(frame==37 || frame==38) {
        sl::ResourceTag invalidOptional{&rm,frame==37?sl::kBufferTypeUIColorAndAlpha:sl::kBufferTypeHUDLessColor,sl::eValidUntilPresent};
        tags(token,viewport,&invalidOptional,1,reinterpret_cast<sl::CommandBuffer*>(list.Get()));
      }
      list->Close(); ID3D12CommandList* raw=list.Get(); queue->ExecuteCommandLists(1,&raw);
      const bool lateSubmitEnd=frame>=17;
      if(!lateSubmitEnd) marker(sl::PCLMarker::eRenderSubmitEnd,token);
      marker(sl::PCLMarker::ePresentStart,token);
      Check(chain->Present(0,0),"Present"); marker(sl::PCLMarker::ePresentEnd,token); wait();
      if(lateSubmitEnd) marker(sl::PCLMarker::eRenderSubmitEnd,token);
      if(frame==9 || frame==17 || frame==18 || frame==25) {
        controlStatus(&cs,sizeof(cs));
        const bool composed=frame==9 || frame==18;
        Require(cs.uiCompositionKnown && cs.uiCompositionResult>=0 && cs.uiCompositionApplied==composed,"UI composition effective state mismatch");
        Require(cs.settings.uiComposition==(frame!=25),"missing HUD-less erased UI preference");
      }
      if(frame==34 || frame==38) { bridgeStatus(&bs,sizeof(bs)); Require(!bs.enabled,"missing MV or invalid retag did not bypass FG"); }
      else if(lateSubmitEnd) { bridgeStatus(&bs,sizeof(bs)); Require(bs.enabled,"valid overlapping RenderSubmit/Present was rejected"); }
      if(frame==37) {
        controlStatus(&cs,sizeof(cs)); Require(cs.hudIssues && cs.hudRejected && !cs.uiCompositionApplied,"invalid optional HUD did not fall back to default interpolation");
      }
      if(frame==40 || frame==48) {
        controlStatus(&cs,sizeof(cs)); Require(cs.sdkDeadlinesActive==(frame==40),"deadline policy did not commit/restore");
      }
      MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg);
    }
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
    Require(cs.reflexObserved && cs.reflexOptionsReady && (nativeReflex?(cs.reflexOutputObserved && !cs.xellObserved && !cs.nativeReflexDisabled && cs.reflexSleeps==48 && cs.xellSleeps==0):(!cs.reflexOutputObserved && cs.xellObserved && cs.nativeReflexDisabled && cs.reflexSleeps==0 && cs.xellSleeps==48)) && cs.sleeps==48 && cs.markers>=48*6,
        "observed latency route/counters missing");
    Require(Import<uint32_t(*)()>(fixture,"FixtureReflexSleeps")()==(nativeReflex?48u:0u),"original Reflex wait dispatch count");
    std::cout<<"Latency route: Reflex -> "<<(nativeReflex?"Reflex":"XeLL")<<"; native waits="<<cs.reflexSleeps<<"; XeLL waits="<<cs.xellSleeps<<std::endl;
    Require(cs.modeUpdates<=2,"identical XeLL configuration reapplied every frame");
    Require(!cs.settings.uiComposition && !cs.uiCompositionApplied && cs.uiCompositionUpdates==4,"UI composition setter repeated or did not restore default");
    const auto requiredBytes=timingReport(nullptr,0); std::vector<char> cpuReport(requiredBytes);
    Require(timingReport(cpuReport.data(),uint32_t(cpuReport.size()))==requiredBytes,"timing report size changed without new Presents");
    Require(std::string(cpuReport.data()).find("Samples=48")!=std::string::npos,"actual Present calls missing from timing ring");
    std::cout<<cpuReport.data(); timingEnable(false);
    settings.flipMetering=false; Require(configure(&settings,sizeof(settings)),"flip toggle off");
    controlStatus(&cs,sizeof(cs)); Require(!cs.flipRequested,"flip request did not reach pacer");
    settings.flipMetering=true; Require(configure(&settings,sizeof(settings)),"flip toggle on");
    sl::DLSSGState live; Require(getState(viewport,live,nullptr)==sl::Result::eOk,"GetState bridge");
    Require(live.numFramesActuallyPresented==ms.presented && live.numFramesToGenerateMax==5,"requested and submitted counts confused");
    live.structVersion=1; live.numFramesToGenerateMax=0xdeadbeef;
    Require(getState(viewport,live,nullptr)==sl::Result::eOk && live.numFramesToGenerateMax==0xdeadbeef,"V1 state extension overwritten");
    sl::DLSSGOptions off; off.mode=sl::DLSSGMode::eOff; options(viewport,off);
    Check(chain->Present(0,0),"disabled Present");
    Check(chain->ResizeBuffers(3,800,450,DXGI_FORMAT_UNKNOWN,0),"ResizeBuffers");
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
    chain.Reset();
    Require(overlayStatus(&os,sizeof(os)) && os.created>=2 && os.created==os.destroyed && os.failed==0,"manual ReShade runtime resize/teardown leaked");
    sl::ViewportHandle replacementViewport{7};
    Require(options(replacementViewport,off)==sl::Result::eOk,"retired output kept the old viewport selection");
    CloseHandle(event); DestroyWindow(hwnd);
    modelStatus(&ms,sizeof(ms)); Require(ms.contexts==0,"context/COM lifetime leaked");
    Require(Import<bool(*)(uint32_t)>(addon,"MFGAXeLogFlush")(2000),"diagnostic log did not drain at test boundary");
    Require(!GetModuleHandleW(L"OptiScaler.dll") && !GetModuleHandleW(L"MFGAdaUnlock.XeMFG.asi"),"external companion loaded");
    std::cout<<"PASS: native ReShade addon, "<<multiplier<<"x requested, real XeMFG inference, Tensor launches, XeLL, UI composition default/opt-in/off/missing-HUDless, volatile/legacy tags, missing-input bypass, disable, resize and context retirement; synthetic/occluded, no display cadence or gameplay quality claim"<<std::endl;
    return 0;
  } catch(const std::exception& e) { std::cerr<<e.what()<<std::endl; return 1; }
}

// Real OptiScaler + Intel dependency graph and disabled-FG Present path.
// This deliberately does not claim to test generated image quality.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <xefg_swapchain_d3d12.h>
#include "../src/addons/mfgunlock/xemfg/backend_api.hpp"
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT r,const char* where) {
  if(FAILED(r)) { std::cerr<<where<<" HRESULT="<<std::hex<<r<<std::endl; throw std::runtime_error(where); }
}
int main(int argc,char**) {
  try {
    const bool keepNative=argc>1;
    xefg_swapchain_version_t version{};
    if(xefgSwapChainGetVersion(&version)!=XEFG_SWAPCHAIN_RESULT_SUCCESS) return 2;
    ComPtr<IDXGIFactory4> factory; Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
    ComPtr<ID3D12Device> device;
    for(UINT i=0;!device;++i) {
      ComPtr<IDXGIAdapter1> adapter;
      if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
      if(desc.VendorId==0x10de) D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device));
    }
    if(!device) throw std::runtime_error("NVIDIA device");
    xefg_swapchain_handle_t probe{};
    auto created=xefgSwapChainD3D12CreateContext(device.Get(),&probe);
    std::cout<<"Native capability probe create="<<created<<std::endl;
    if(created!=XEFG_SWAPCHAIN_RESULT_SUCCESS) return 3;
    xefg_swapchain_properties_t properties{};
    if(xefgSwapChainGetProperties(probe,&properties)!=XEFG_SWAPCHAIN_RESULT_SUCCESS) return 4;
    if(!keepNative && xefgSwapChainDestroy(probe)!=XEFG_SWAPCHAIN_RESULT_SUCCESS) return 5;
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC q{};
    Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
    WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.lpszClassName=L"XeMFGRealHostSmoke"; wc.hInstance=GetModuleHandleW(nullptr);
    RegisterClassW(&wc);
    HWND hwnd=CreateWindowW(wc.lpszClassName,L"XeMFG hidden smoke",WS_OVERLAPPEDWINDOW,0,0,640,360,nullptr,nullptr,wc.hInstance,nullptr);
    if(!hwnd) throw std::runtime_error("window");
    DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width=640; desc.Height=360; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount=2; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swapchain;
    Check(factory->CreateSwapChainForHwnd(queue.Get(),hwnd,&desc,nullptr,nullptr,&swapchain),"swapchain");
    const auto plugin=GetModuleHandleW(L"MFGAdaUnlock.XeMFG.asi");
    auto get=plugin?reinterpret_cast<mfgunlock::xemfg::GetStatus>(GetProcAddress(plugin,"MFGAXeGetStatus")):nullptr;
    mfgunlock::xemfg::Status status;
    if(!get || !get(&status,sizeof(status))) throw std::runtime_error("companion status");
    if(keepNative ? status.model!=0 : !status.model || !status.initialized)
      throw std::runtime_error(keepNative ? "unsafe port over a live native context" : "MFG port did not activate after native probe retirement");
    std::cout<<"Real host swapchain created"<<std::endl;
    for(unsigned i=0;i<24;++i) {
      if(i==12) Check(swapchain->ResizeBuffers(2,800,450,DXGI_FORMAT_UNKNOWN,0),"ResizeBuffers");
      Check(swapchain->Present(0,0),"Present");
      MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
      Sleep(5);
    }
    swapchain.Reset(); DestroyWindow(hwnd);
    if(keepNative && xefgSwapChainDestroy(probe)!=XEFG_SWAPCHAIN_RESULT_SUCCESS) return 5;
    std::cout<<"PASS: "<<(keepNative?"live native context prevented global port":"retired native probe admitted MFG port")
             <<"; real OptiScaler swapchain, resize and 24 disabled-FG Presents; no generated images tested"<<std::endl;
    return 0;
  } catch(const std::exception& e) { std::cerr<<e.what()<<std::endl; return 1; }
}

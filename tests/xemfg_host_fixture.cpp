// A synthetic calling host with real Intel APIs and a real D3D12 device.
// No engine inputs, no Present, no generated output, no gameplay benchmark.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <xefg_swapchain_d3d12.h>
#include <xell_d3d12.h>
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
#define PROC(m,n) auto n##_fn=reinterpret_cast<decltype(&n)>(GetProcAddress(m,#n)); if(!n##_fn) throw std::runtime_error(#n)
#define CALL(n,...) do { const auto r=n##_fn(__VA_ARGS__); std::cout<<#n<<"="<<r<<std::endl; if(r!=0) throw std::runtime_error(#n); } while(false)
extern "C" __declspec(dllexport) int RunFixture() {
  try {
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter; ComPtr<ID3D12Device> device;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 2;
    for (UINT i=0;factory->EnumAdapters1(i,&adapter)!=DXGI_ERROR_NOT_FOUND;++i) {
      DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
      if(desc.VendorId==0x10de && SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)))) break;
      adapter.Reset();
    }
    if(!device) throw std::runtime_error("NVIDIA device missing");
    const auto fg=GetModuleHandleW(L"libxess_fg.dll"),ll=GetModuleHandleW(L"libxell.dll");
    PROC(fg,xefgSwapChainD3D12CreateContext); PROC(fg,xefgSwapChainDestroy);
    PROC(fg,xefgSwapChainGetProperties); PROC(fg,xefgSwapChainSetLatencyReduction);
    PROC(fg,xefgSwapChainD3D12InitFromSwapChainDesc); PROC(fg,xefgSwapChainSetNumInterpolatedFrames);
    PROC(fg,xefgSwapChainSetEnabled);
    PROC(ll,xellD3D12CreateContext); PROC(ll,xellSetSleepMode); PROC(ll,xellDestroyContext);
    xell_context_handle_t latency{}; CALL(xellD3D12CreateContext,device.Get(),&latency);
    xell_sleep_params_t sleep{}; sleep.bLowLatencyMode=1;
    CALL(xellSetSleepMode,latency,&sleep);
    xefg_swapchain_handle_t context{}; CALL(xefgSwapChainD3D12CreateContext,device.Get(),&context);
    xefg_swapchain_properties_t properties{}; CALL(xefgSwapChainGetProperties,context,&properties);
    std::cout<<"maximum_generated="<<properties.maxSupportedInterpolations<<std::endl;
    if(properties.maxSupportedInterpolations!=5) throw std::runtime_error("MFG port not active");
    CALL(xefgSwapChainSetLatencyReduction,context,latency);
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    if(FAILED(device->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue)))) return 3;
    WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.lpszClassName=L"MFGAXeFixture"; wc.hInstance=GetModuleHandleW(nullptr);
    RegisterClassW(&wc);
    HWND hwnd=CreateWindowW(wc.lpszClassName,L"XeMFG fixture",WS_OVERLAPPEDWINDOW,0,0,640,360,nullptr,nullptr,wc.hInstance,nullptr);
    if(!hwnd) throw std::runtime_error("fixture_window");
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width=640; desc.Height=360; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount=2; desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    xefg_swapchain_d3d12_init_params_t init{}; init.maxInterpolatedFrames=5;
    init.initFlags=XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH;
    CALL(xefgSwapChainD3D12InitFromSwapChainDesc,context,hwnd,&desc,nullptr,queue.Get(),factory.Get(),&init);
    for(unsigned count:{1,2,3,5,3,1}) CALL(xefgSwapChainSetNumInterpolatedFrames,context,count);
    CALL(xefgSwapChainSetEnabled,context,1);
    CALL(xefgSwapChainSetEnabled,context,0);
    CALL(xefgSwapChainDestroy,context);
    CALL(xellDestroyContext,latency);
    DestroyWindow(hwnd);
    std::cout<<"PASS: real SDK initialization + pacing owner + 2x/3x/4x/6x live count changes + destruction; no presentation tested"<<std::endl;
    return 0;
  } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<std::endl; return 1; }
}

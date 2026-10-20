// SPDX-License-Identifier: MIT
// ReShade's public manual-runtime API for Intel's unproxied presentation queue.
// Effects and input run at the final output; never draw into an FG input.
#include "native_internal.hpp"
#include "native_overlay.hpp"
#include "../backend_selection.hpp"
#include <deps/imgui/imgui.h>
#include <reshade.hpp>
#include <charconv>
namespace mfgunlock::xemfg::native {
namespace {
std::atomic<uint64_t> created{},destroyed{},updates{},failed{};
std::atomic<uint64_t> keyRequests{},keyRecovered{},keyDropped{};
class FinalOutput final:public IDXGISwapChain4 {
  std::atomic<ULONG> refs{1};
  ComPtr<IDXGISwapChain4> chain;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  reshade::api::effect_runtime* runtime{};
  std::recursive_mutex lock;
  DXGI_COLOR_SPACE_TYPE color{DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
  bool colorKnown{};
  std::weak_ptr<WindowControl> window;
  OverlayChord chord;
  uint64_t nextConfig{};
  unsigned keyLogs{};
  unsigned inputLogs{};
  uint64_t inputActivity{UINT64_MAX};
  uint64_t hotkeyChanges{};
  uint32_t shortcutBlockers{};
  bool overlayOpen{},recovering{};
  static thread_local FinalOutput* rendering;
  void Shortcut() {
    auto input=window.lock();
    if(!input) { if(auto o=Active()) { input=o->window; window=input; nextConfig=0; } }
    if(!input || GetTickCount64()<nextConfig) return;
    nextConfig=GetTickCount64()+500;
    auto read=[&](const char* key,char* buffer,size_t& length) {
      const auto capacity=length;
      if(reshade::get_config_value(runtime,"INPUT",key,buffer,&length)) return true;
      length=capacity; return reshade::get_config_value(nullptr,"INPUT",key,buffer,&length);
    };
    OverlayChord value;
    char buffer[128]{}; size_t length=sizeof(buffer);
    if(read("KeyOverlay",buffer,length)) {
      uint32_t fields[4]{VK_HOME,0,0,0};
      size_t pos{}; bool valid=true;
      for(unsigned i=0;i<4 && pos<length && buffer[pos];++i) {
        auto end=pos; while(end<length && buffer[end]) ++end;
        const auto result=std::from_chars(buffer+pos,buffer+end,fields[i]);
        if(result.ec!=std::errc{} || result.ptr!=buffer+end) valid=false;
        pos=end+1;
      }
      value.key=valid && fields[0]<256?fields[0]:0;
      value.modifiers=(fields[1]?1:0)|(fields[2]?2:0)|(fields[3]?4:0);
    }
    length=sizeof(buffer);
    if(read("ForceShortcutModifiers",buffer,length)) value.exact=buffer[0]!='0';
    chord=value; input->OverlayKey(chord);
  }
  void Color() {
    if(!runtime || !colorKnown) return;
    using Space=reshade::api::color_space;
    if(color==DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) runtime->set_color_space(Space::hdr10_st2084);
    else if(color==DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) runtime->set_color_space(Space::extended_srgb_linear);
    else if(color==DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) runtime->set_color_space(Space::srgb_nonlinear);
  }
  void Create() {
    const auto config=(backend::Directory()/L"ReShade.ini").u8string();
    if(!reshade::create_effect_runtime(reshade::api::device_api::d3d12,device.Get(),queue.Get(),chain.Get(),
        reinterpret_cast<const char*>(config.c_str()),&runtime)) { ++failed; return; }
    ++created; Color();
  }
  void Destroy() {
    if(!runtime) return;
    if(auto input=window.lock()) input->OverlayKey({0,0,true});
    reshade::destroy_effect_runtime(runtime); runtime=nullptr; ++destroyed;
    overlayOpen=false; nextConfig=0;
  }
  void Render(UINT flags) {
    if(runtime && !(flags&DXGI_PRESENT_TEST)) {
      struct Scope { FinalOutput* previous; Scope(FinalOutput* value):previous(rendering){rendering=value;} ~Scope(){rendering=previous;} } scope(this);
      Shortcut(); shortcutBlockers=runtime->is_key_pressed(VK_ESCAPE)?1:0;
      const bool nativePressed=chord.key && runtime->is_key_pressed(chord.key);
      reshade::update_and_present_effect_runtime(runtime); ++updates;
      if(auto input=window.lock()) {
        OverlayReceipt receipt;
        uint64_t dropped{}; const auto taps=input->OverlayTaps(shortcutBlockers,dropped,&receipt);
        keyDropped.store(dropped);
        if(receipt.taps.activity!=inputActivity || nativePressed || receipt.hotkeyChanges!=hotkeyChanges) {
          inputActivity=receipt.taps.activity;
          hotkeyChanges=receipt.hotkeyChanges;
          if(inputLogs++<64) xess_mod::WriteLog("native_overlay_input","\"key\":"+std::to_string(chord.key)
              +",\"presses\":"+std::to_string(receipt.taps.presses)+",\"releases\":"+std::to_string(receipt.taps.releases)
              +",\"matched\":"+std::to_string(receipt.taps.matched)+",\"native\":"+std::to_string(receipt.taps.native)
              +",\"queued\":"+std::to_string(receipt.taps.queued)+",\"handled\":"+std::to_string(receipt.taps.handled)
              +",\"blocked\":"+std::to_string(receipt.taps.blocked)+",\"recover\":"+std::to_string(taps)
              +",\"blockers\":"+std::to_string(receipt.blockers)+",\"modifiers\":"+std::to_string(receipt.taps.modifiers)
              +",\"canceled\":"+std::to_string(receipt.taps.canceled)+",\"child_messages\":"+std::to_string(receipt.childMessages)
              +",\"hotkeys\":"+std::to_string(receipt.taps.hotkeys)+",\"hotkey_registered\":"+(receipt.hotkeyRegistered?"true":"false")
              +",\"hotkey_error\":"+std::to_string(receipt.hotkeyError)
              +",\"down\":"+(receipt.taps.down?"true":"false")+",\"native_pressed\":"+(nativePressed?"true":"false")
              +",\"requested_open\":"+(overlayOpen?"true":"false"));
        }
        for(uint32_t i=0;i<taps;++i) {
          const bool previous=overlayOpen; recovering=true;
          const bool accepted=runtime->open_overlay(!previous,reshade::api::input_source::keyboard);
          recovering=false;
          if(!accepted) overlayOpen=previous;
          else ++keyRecovered;
          if(keyLogs++<16) xess_mod::WriteLog("native_overlay_key_recovery","\"open\":"+std::string(overlayOpen?"true":"false")
              +",\"accepted\":"+(accepted?"true":"false")+",\"key\":"+std::to_string(chord.key));
        }
      }
    }
  }
public:
  static bool OverlayChange(reshade::api::effect_runtime* value,bool open,reshade::api::input_source source) {
    auto* self=rendering;
    if(!self || self->runtime!=value) return false;
    if(!self->recovering && value->is_key_pressed(self->chord.key) && !value->is_key_pressed(VK_ESCAPE))
      if(auto input=self->window.lock(); input && input->OverlayOwnsHotkey(self->chord)) return true;
    self->overlayOpen=open; ++keyRequests;
    // ReShade labels the request by the last ImGui navigation source, even
    // when the configured keyboard shortcut caused it. Test the actual key.
    if(!self->recovering && value->is_key_pressed(self->chord.key))
      if(auto input=self->window.lock()) input->OverlayNative();
    if(self->keyLogs++<16) xess_mod::WriteLog("native_overlay_request","\"open\":"+std::string(open?"true":"false")
        +",\"recovered\":"+(self->recovering?"true":"false")+",\"source\":"+std::to_string(uint32_t(source)));
    return false; // Respect ReShade and any other addon's veto.
  }
  static void OverlayGui(reshade::api::effect_runtime* value) {
    if(rendering && rendering->runtime==value) {
      if(ImGui::IsAnyItemActive()) rendering->shortcutBlockers|=2;
      if(ImGui::GetIO().WantTextInput) rendering->shortcutBlockers|=4;
      if(auto input=rendering->window.lock()) input->OverlayTextCapture(ImGui::GetIO().WantTextInput);
    }
  }
  FinalOutput(ComPtr<IDXGISwapChain4> value,ComPtr<ID3D12Device> d,ComPtr<ID3D12CommandQueue> q)
      :chain(std::move(value)),device(std::move(d)),queue(std::move(q)) { Create(); }
  ~FinalOutput() { Destroy(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
    if(!out) return E_POINTER; *out=nullptr;
    if(id==__uuidof(IUnknown)||id==__uuidof(IDXGIObject)||id==__uuidof(IDXGIDeviceSubObject)
        ||id==__uuidof(IDXGISwapChain)||id==__uuidof(IDXGISwapChain1)||id==__uuidof(IDXGISwapChain2)
        ||id==__uuidof(IDXGISwapChain3)||id==__uuidof(IDXGISwapChain4)) {
      *out=static_cast<IDXGISwapChain4*>(this); AddRef(); return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { const auto n=--refs; if(!n) delete this; return n; }
  HRESULT STDMETHODCALLTYPE Present(UINT sync,UINT flags) override {
    { std::lock_guard guard(lock); Render(flags); }
    // DXGI may wait on the window thread. The SDK owns external Present/Resize
    // ordering; do not hold our runtime lock across the native call.
    return chain->Present(sync,flags);
  }
  HRESULT STDMETHODCALLTYPE Present1(UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* parameters) override {
    { std::lock_guard guard(lock); Render(flags); }
    return chain->Present1(sync,flags,parameters);
  }
  HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT a,UINT b,UINT c,DXGI_FORMAT d,UINT e) override {
    { std::lock_guard guard(lock); Destroy(); }
    const auto result=chain->ResizeBuffers(a,b,c,d,e);
    { std::lock_guard guard(lock); Create(); }
    return result;
  }
  HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT a,UINT b,UINT c,DXGI_FORMAT d,UINT e,const UINT* f,IUnknown*const* g) override {
    { std::lock_guard guard(lock); Destroy(); }
    const auto result=chain->ResizeBuffers1(a,b,c,d,e,f,g);
    { std::lock_guard guard(lock); Create(); }
    return result;
  }
  HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE value) override {
    const auto result=chain->SetColorSpace1(value);
    if(SUCCEEDED(result)) { std::lock_guard guard(lock); color=value; colorKnown=true; Color(); } return result;
  }
#define HR(name,args,call) HRESULT STDMETHODCALLTYPE name args override { return chain->name call; }
  HR(SetPrivateData,(REFGUID a,UINT b,const void* c),(a,b,c))
  HR(SetPrivateDataInterface,(REFGUID a,const IUnknown* b),(a,b))
  HR(GetPrivateData,(REFGUID a,UINT* b,void* c),(a,b,c))
  HR(GetParent,(REFIID a,void** b),(a,b)) HR(GetDevice,(REFIID a,void** b),(a,b))
  HR(GetBuffer,(UINT a,REFIID b,void** c),(a,b,c))
  HR(SetFullscreenState,(BOOL a,IDXGIOutput* b),(a,b)) HR(GetFullscreenState,(BOOL* a,IDXGIOutput** b),(a,b))
  HR(GetDesc,(DXGI_SWAP_CHAIN_DESC* a),(a)) HR(ResizeTarget,(const DXGI_MODE_DESC* a),(a))
  HR(GetContainingOutput,(IDXGIOutput** a),(a)) HR(GetFrameStatistics,(DXGI_FRAME_STATISTICS* a),(a))
  HR(GetLastPresentCount,(UINT* a),(a)) HR(GetDesc1,(DXGI_SWAP_CHAIN_DESC1* a),(a))
  HR(GetFullscreenDesc,(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* a),(a)) HR(GetHwnd,(HWND* a),(a))
  HR(GetCoreWindow,(REFIID a,void** b),(a,b))
  HR(GetRestrictToOutput,(IDXGIOutput** a),(a)) HR(SetBackgroundColor,(const DXGI_RGBA* a),(a))
  HR(GetBackgroundColor,(DXGI_RGBA* a),(a)) HR(SetRotation,(DXGI_MODE_ROTATION a),(a))
  HR(GetRotation,(DXGI_MODE_ROTATION* a),(a)) HR(SetSourceSize,(UINT a,UINT b),(a,b))
  HR(GetSourceSize,(UINT* a,UINT* b),(a,b)) HR(SetMaximumFrameLatency,(UINT a),(a))
  HR(GetMaximumFrameLatency,(UINT* a),(a)) HR(SetMatrixTransform,(const DXGI_MATRIX_3X2_F* a),(a))
  HR(GetMatrixTransform,(DXGI_MATRIX_3X2_F* a),(a)) HR(CheckColorSpaceSupport,(DXGI_COLOR_SPACE_TYPE a,UINT* b),(a,b))
  HR(SetHDRMetaData,(DXGI_HDR_METADATA_TYPE a,UINT b,void* c),(a,b,c))
#undef HR
  BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return chain->IsTemporaryMonoSupported(); }
  HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override { return chain->GetFrameLatencyWaitableObject(); }
  UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return chain->GetCurrentBackBufferIndex(); }
};
thread_local FinalOutput* FinalOutput::rendering{};
}
OverlayStatus OverlaySnapshot() noexcept { return {created,destroyed,updates,failed}; }
OverlayInputStatus OverlayInputSnapshot() noexcept { return {keyRequests,keyRecovered,keyDropped}; }
void WrapFinalOutput(IUnknown* source,HWND hwnd,IDXGISwapChain1** output) noexcept {
  try {
    if(!output || !*output || !single_overlay::native::SystemDxgiObject(*output,40)) return;
    HWND actual{}; if(FAILED((*output)->GetHwnd(&actual)) || actual!=hwnd) return;
    ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12Device> device; ComPtr<IDXGISwapChain4> chain;
    if(!source || FAILED(source->QueryInterface(IID_PPV_ARGS(&queue)))
        || queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT
        || FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || FAILED((*output)->QueryInterface(IID_PPV_ARGS(&chain)))) return;
    static std::once_flag callbacks;
    std::call_once(callbacks,[] {
      reshade::register_event<reshade::addon_event::reshade_open_overlay>(&FinalOutput::OverlayChange);
      reshade::register_event<reshade::addon_event::reshade_overlay>(&FinalOutput::OverlayGui);
    });
    auto* replacement=new FinalOutput(std::move(chain),std::move(device),std::move(queue));
    (*output)->Release(); *output=replacement;
    xess_mod::WriteLog("native_final_overlay","\"manual_runtime\":true");
  } catch(...) { ++failed; }
}
}
extern "C" __declspec(dllexport) bool MFGAXeOverlayStatus(mfgunlock::xemfg::native::OverlayStatus* out,uint32_t bytes) {
  if(!out || bytes!=sizeof(*out)) return false; *out=mfgunlock::xemfg::native::OverlaySnapshot(); return true;
}
extern "C" __declspec(dllexport) bool MFGAXeOverlayInputStatus(mfgunlock::xemfg::native::OverlayInputStatus* out,uint32_t bytes) {
  if(!out || bytes!=sizeof(*out)) return false; *out=mfgunlock::xemfg::native::OverlayInputSnapshot(); return true;
}

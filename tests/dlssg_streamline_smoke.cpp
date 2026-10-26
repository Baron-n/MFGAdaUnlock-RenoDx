// SPDX-License-Identifier: MIT
// Plan 6.1: the DLSS-G backend against a fake Streamline interposer. The game
// side is this executable: it initializes Streamline, lets ReShade load the
// addon with [Backend] Selected=DLSSG and calls the DLSS-G and Reflex
// functions it obtains through slGetFeatureFunction, as a game does. The
// fixture records what reached the provider. ReShade.ini sets the fixed
// multiplier (ForceMultiplier=4).
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <iostream>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
namespace {
void Require(bool value,const char* what) { if(!value) throw std::runtime_error(what); }
struct Token : sl::FrameToken { uint32_t id{}; operator uint32_t() const override {return id;} };
template<class T> T Import(HMODULE m,const char* name) { auto p=reinterpret_cast<T>(GetProcAddress(m,name)); Require(p!=nullptr,name); return p; }
HMODULE Owner(const void* address) {
  HMODULE module{};
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      static_cast<LPCWSTR>(address),&module);
  return module;
}
}
int main() {
  try {
    auto fixture=LoadLibraryW(L"sl.interposer.dll"); Require(fixture,"fixture");
    sl::Preferences preferences;
    Require(Import<PFun_slInit*>(fixture,"slInit")(preferences,sl::kSDKVersion)==sl::Result::eOk,"fixture startup");
    // ReShade loads its addons on the first device; the DLSS-G backend hooks
    // the already mapped interposer from its DllMain.
    auto reshade=LoadLibraryW(L".\\dxgi.dll"); Require(reshade,"ReShade");
    ComPtr<IDXGIFactory4> factory;
    Require(SUCCEEDED(Import<HRESULT(WINAPI*)(REFIID,void**)>(reshade,"CreateDXGIFactory1")(IID_PPV_ARGS(&factory))),"factory");
    ComPtr<ID3D12Device> device;
    for(UINT i=0;!device;++i) {
      ComPtr<IDXGIAdapter1> adapter;
      if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND) break;
      DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
      if(desc.VendorId==0x10de) D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device));
    }
    Require(device!=nullptr,"NVIDIA device");
    auto addon=GetModuleHandleW(L"renodx-mfgunlock-native.addon64"); Require(addon,"ReShade did not load the addon");
    const auto getFeature=Import<PFun_slGetFeatureFunction*>(fixture,"slGetFeatureFunction");
    const auto resolve=[&](sl::Feature feature,const char* name) {
      void* fn{}; Require(getFeature(feature,name,fn)==sl::Result::eOk && fn,name); return fn;
    };
    // The game's functions are the backend's wrappers.
    auto setOptions=reinterpret_cast<PFun_slDLSSGSetOptions*>(resolve(sl::kFeatureDLSS_G,"slDLSSGSetOptions"));
    auto getState=reinterpret_cast<PFun_slDLSSGGetState*>(resolve(sl::kFeatureDLSS_G,"slDLSSGGetState"));
    auto reflexOptions=reinterpret_cast<PFun_slReflexSetOptions*>(resolve(sl::kFeatureReflex,"slReflexSetOptions"));
    auto reflexSleep=reinterpret_cast<PFun_slReflexSleep*>(resolve(sl::kFeatureReflex,"slReflexSleep"));
    Require(Owner(reinterpret_cast<void*>(setOptions))==addon,"slDLSSGSetOptions was not wrapped by the DLSS-G backend");
    Require(Owner(reinterpret_cast<void*>(getState))==addon,"slDLSSGGetState was not wrapped by the DLSS-G backend");
    Require(Owner(reinterpret_cast<void*>(reflexOptions))==addon,"slReflexSetOptions was not wrapped by the DLSS-G backend");
    const auto lastGenerated=Import<uint32_t(*)()>(fixture,"FixtureLastGenerated");
    const auto modes=Import<uint32_t(*)()>(fixture,"FixtureModes");
    const sl::ViewportHandle viewport{0};

    // Fixed multiplier: the game's 2x (one generated frame) reaches the provider as 4x.
    sl::DLSSGOptions on; on.structVersion=sl::kStructVersion3; on.mode=sl::DLSSGMode::eOn; on.numFramesToGenerate=1;
    Require(setOptions(viewport,on)==sl::Result::eOk,"SetOptions on");
    std::cout<<"SetOptions 2x -> provider generated="<<lastGenerated()<<std::endl;
    Require(lastGenerated()==3,"the fixed 4x multiplier did not reach the provider");
    Require((modes()&0xff)==uint32_t(sl::DLSSGMode::eOn),"the game's mode was changed");
    // The game's own request equal to the multiplier is forwarded unchanged.
    on.numFramesToGenerate=3;
    Require(setOptions(viewport,on)==sl::Result::eOk && lastGenerated()==3,"matching request changed");
    // Frame generation off is never rewritten.
    sl::DLSSGOptions off=on; off.mode=sl::DLSSGMode::eOff; off.numFramesToGenerate=1;
    Require(setOptions(viewport,off)==sl::Result::eOk,"SetOptions off");
    Require(lastGenerated()==1 && (modes()&0xff)==uint32_t(sl::DLSSGMode::eOff),"frame generation off was rewritten");
    // An options structure newer than the backend knows is forwarded unchanged.
    sl::DLSSGOptions newer=on; newer.structVersion=6; newer.numFramesToGenerate=1;
    Require(setOptions(viewport,newer)==sl::Result::eOk && lastGenerated()==1,"an unknown options ABI was overridden");
    Require(setOptions(viewport,on)==sl::Result::eOk && lastGenerated()==3,"override did not resume after an unknown ABI");

    // GetState: the provider's answer reaches the game; a v1 caller is not extended.
    sl::DLSSGState state; state.structVersion=sl::kStructVersion3;
    Require(getState(viewport,state,nullptr)==sl::Result::eOk,"GetState");
    Require(state.numFramesActuallyPresented==1 && state.numFramesToGenerateMax==1 && uint32_t(state.status)==0,"GetState changed the provider's state");
    Import<void(*)(uint32_t)>(fixture,"FixtureFgStatus")(4);
    Require(getState(viewport,state,nullptr)==sl::Result::eOk && uint32_t(state.status)==4,"a provider failure flag was masked");
    Import<void(*)(uint32_t)>(fixture,"FixtureFgStatus")(0);
    sl::DLSSGState v1; v1.structVersion=sl::kStructVersion1; v1.numFramesToGenerateMax=0xdeadbeef;
    Require(getState(viewport,v1,nullptr)==sl::Result::eOk && v1.numFramesToGenerateMax==0xdeadbeef,"a v1 state was written past its version");

    // Reflex without a configured cap: the game's options reach Streamline unchanged.
    sl::ReflexOptions reflex; reflex.mode=sl::ReflexMode::eLowLatency; reflex.frameLimitUs=16666;
    Require(reflexOptions(reflex)==sl::Result::eOk,"Reflex options");
    Require((modes()>>8)==uint32_t(sl::ReflexMode::eLowLatency) && Import<uint32_t(*)()>(fixture,"FixtureReflexLimit")()==16666,
        "the game's Reflex options were changed without a configured cap");
    for(uint32_t frame=1;frame<=4;++frame) {
      Token token; token.id=frame;
      Require(reflexSleep(token)==sl::Result::eOk,"Reflex sleep");
    }
    Require(Import<uint32_t(*)()>(fixture,"FixtureReflexSleeps")()==4,"a Reflex sleep did not reach Streamline");

    Require(Import<PFun_slShutdown*>(fixture,"slShutdown")()==sl::Result::eOk,"fixture shutdown");
    std::cout<<"PASS: DLSS-G backend: fixed multiplier, off, unknown ABI, GetState and Reflex through a Streamline fixture"<<std::endl;
    return 0;
  } catch(const std::exception& e) { std::cerr<<e.what()<<std::endl; return 1; }
}

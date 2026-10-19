// SPDX-License-Identifier: MIT
#include "native_internal.hpp"
#include <cmath>
#include <algorithm>
#include <psapi.h>

namespace mfgunlock::xemfg::native {
namespace {
std::recursive_mutex discovery;
PFun_slGetFeatureFunction* getFeature{};
PFun_slSetTag* setTag{};
PFun_slSetTagForFrame* setTagFrame{};
PFun_slSetConstants* setConstants{};
PFun_slEvaluateFeature* evaluate{};
PFun_slDLSSGSetOptions* setOptions{};
PFun_slDLSSGGetState* getState{};
PFun_slReflexSetOptions* reflexOptions{};
PFun_slReflexSleep* reflexSleep{};
PFun_slPCLSetMarker* pclMarker{};

bool SelectViewport(uint32_t v) {
  auto& s=State(); auto expected=UINT32_MAX;
  s.viewport.compare_exchange_strong(expected,v);
  return s.viewport.load()==v;
}
Frame* FindFrame(Output& o,uint32_t id) {
  if(!id) return nullptr;
  if(!o.frames.contains(id)) {
    // Reject a far future frame without silently evicting a live older token.
    if(o.frames.size()>=16) return nullptr;
    o.frames[id].id=id;
  }
  return &o.frames[id];
}
bool Finite(float value) { return std::isfinite(value) && std::abs(value)<1.e30f; }
bool Constants(const sl::Constants& c,Output& o,Frame& frame) {
  if(c.structType!=sl::Constants::s_structType || c.structVersion<1 || c.structVersion>2) return false;
  if(c.cameraMotionIncluded!=sl::eTrue || c.motionVectors3D!=sl::eFalse
      || c.depthInverted==sl::eInvalid || (c.depthInverted==sl::eTrue)!=o.invert
      || (c.motionVectorsJittered==sl::eTrue)!=o.jitter || c.reset==sl::eInvalid) return false;
  auto& data=frame.data;
  memcpy(data.projectionMatrix,&c.cameraViewToClip,sizeof(data.projectionMatrix));
  // Streamline supplies a world position and camera basis, not a view matrix.
  const sl::float3 basis[]={c.cameraRight,c.cameraUp,c.cameraFwd};
  for(unsigned col=0;col<3;++col) {
    const auto b=basis[col];
    data.viewMatrix[col]=b.x; data.viewMatrix[4+col]=b.y; data.viewMatrix[8+col]=b.z;
    data.viewMatrix[12+col]=-(b.x*c.cameraPos.x+b.y*c.cameraPos.y+b.z*c.cameraPos.z);
  }
  data.viewMatrix[3]=data.viewMatrix[7]=data.viewMatrix[11]=0; data.viewMatrix[15]=1;
  data.jitterOffsetX=c.jitterOffset.x; data.jitterOffsetY=c.jitterOffset.y;
  // SL normalizes motion to image coordinates. We use Intel's pixel mode;
  // multiply by the valid MV extent at Present (not the output resolution).
  data.motionVectorScaleX=c.mvecScale.x; data.motionVectorScaleY=c.mvecScale.y;
  data.resetHistory=c.reset==sl::eTrue;
  data.frameRenderTime=0; // No invented timing derived from generated frames.
  for(float x:data.viewMatrix) if(!Finite(x)) return false;
  for(float x:data.projectionMatrix) if(!Finite(x)) return false;
  return Finite(data.jitterOffsetX)&&Finite(data.jitterOffsetY)
      &&Finite(data.motionVectorScaleX)&&Finite(data.motionVectorScaleY);
}
std::optional<xefg_swapchain_resource_type_t> ResourceType(sl::BufferType type) {
  switch(type) {
    case sl::kBufferTypeDepth: return XEFG_SWAPCHAIN_RES_DEPTH;
    case sl::kBufferTypeMotionVectors: return XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
    case sl::kBufferTypeHUDLessColor: return XEFG_SWAPCHAIN_RES_HUDLESS_COLOR;
    case sl::kBufferTypeUIColorAndAlpha: return XEFG_SWAPCHAIN_RES_UI;
    default: return {};
  }
}
void Tags(uint32_t id,uint32_t viewport,const sl::ResourceTag* tags,uint32_t count,sl::CommandBuffer* command) {
  if(!id || !tags || count>64 || !SelectViewport(viewport)) return;
  auto o=Active(); if(!o) return;
  std::lock_guard lock(o->io); auto* frame=FindFrame(*o,id); if(!frame) return;
  frame->viewport=viewport;
  for(uint32_t i=0;i<count;++i) {
    const auto& t=tags[i]; auto type=ResourceType(t.type); if(!type) continue;
    const auto bit=1u<<*type;
    if(!t.resource || !t.resource->native) { frame->resources&=~bit; continue; }
    if(t.structVersion!=1 || t.resource->structVersion!=1 || t.resource->type!=sl::ResourceType::eTex2d
        || t.resource->state==UINT_MAX) { frame->failed=true; continue; }
    ComPtr<ID3D12Resource> resource;
    if(!single_overlay::native::Unwrap(reinterpret_cast<IUnknown*>(t.resource->native),resource)) {
      if(id<6) xess_mod::WriteLog("native_tag_rejected","\"reason\":\"resource_unwrap\"");
      frame->failed=true; continue;
    }
    const auto desc=resource->GetDesc();
    ComPtr<ID3D12Device> device, nativeDevice; resource->GetDevice(IID_PPV_ARGS(&device));
    single_overlay::native::Unwrap(device.Get(),nativeDevice);
    ComPtr<IUnknown> identity, expectedIdentity;
    if(nativeDevice) nativeDevice.As(&identity); o->device.As(&expectedIdentity);
    if(identity.Get()!=expectedIdentity.Get() || desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D
        || desc.SampleDesc.Count!=1 || desc.DepthOrArraySize!=1) { frame->failed=true; continue; }
    xefg_swapchain_d3d12_resource_data_t data{};
    data.type=*type; data.pResource=resource.Get();
    data.incomingState=static_cast<D3D12_RESOURCE_STATES>(t.resource->state);
    data.resourceBase={t.extent.left,t.extent.top};
    data.resourceSize={t.extent.width?t.extent.width:uint32_t(desc.Width),t.extent.height?t.extent.height:desc.Height};
    if(uint64_t(data.resourceBase.x)+data.resourceSize.x>desc.Width
        || uint64_t(data.resourceBase.y)+data.resourceSize.y>desc.Height) { frame->failed=true; continue; }
    ComPtr<ID3D12CommandList> list;
    if(t.lifecycle==sl::eValidUntilPresent) data.validity=XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
    else if(t.lifecycle==sl::eOnlyValidNow || t.lifecycle==sl::eValidUntilEvaluate) {
      // Intel's documented ONLY_NOW contract records the copy at the tag,
      // using the producer's list and restoring its resource state. A COM
      // AddRef alone would not preserve the contents of a volatile resource.
      if(!command || !single_overlay::native::Unwrap(reinterpret_cast<IUnknown*>(command),list)
          || list->GetType()==D3D12_COMMAND_LIST_TYPE_COPY) { frame->failed=true; continue; }
      data.validity=XEFG_SWAPCHAIN_RV_ONLY_NOW;
    } else { frame->failed=true; continue; }
    const auto code=State().api.xefgSwapChainD3D12TagFrameResource(o->fg,list.Get(),id,&data);
    if(id<6) xess_mod::WriteLog("native_tag","\"type\":"+std::to_string(*type)+",\"result\":"+std::to_string(code)
        +",\"format\":"+std::to_string(desc.Format)+",\"width\":"+std::to_string(data.resourceSize.x)
        +",\"height\":"+std::to_string(data.resourceSize.y)+",\"lifecycle\":"+std::to_string(t.lifecycle));
    if(code<0) frame->failed=true;
    else {
      frame->resources|=bit;
      if(*type==XEFG_SWAPCHAIN_RES_MOTION_VECTOR) {
        frame->motionWidth=data.resourceSize.x; frame->motionHeight=data.resourceSize.y;
      }
    }
  }
}
sl::Result TagHook(const sl::ViewportHandle& v,const sl::ResourceTag* t,uint32_t n,sl::CommandBuffer* c) {
  const auto r=setTag(v,t,n,c);
  Tags(State().recording.load(),uint32_t(v),t,n,c); return r;
}
sl::Result TagFrameHook(const sl::FrameToken& f,const sl::ViewportHandle& v,
    const sl::ResourceTag* t,uint32_t n,sl::CommandBuffer* c) {
  const auto r=setTagFrame(f,v,t,n,c); Tags(FrameId(uint32_t(f)),uint32_t(v),t,n,c); return r;
}
sl::Result ConstantsHook(const sl::Constants& c,const sl::FrameToken& f,const sl::ViewportHandle& v) {
  const auto r=setConstants(c,f,v); const auto id=FrameId(uint32_t(f));
  if(SelectViewport(uint32_t(v))) if(auto o=Active()) {
    std::lock_guard lock(o->io);
    if(auto* frame=FindFrame(*o,id)) { frame->constants=true; frame->valid=Constants(c,*o,*frame); }
  }
  return r;
}
sl::Result OptionsHook(const sl::ViewportHandle& v,const sl::DLSSGOptions& opts) {
  if(opts.structType!=sl::DLSSGOptions::s_structType || opts.structVersion<1 || opts.structVersion>5)
    return sl::Result::eErrorUnsupportedInterface;
  auto& s=State(); if(!SelectViewport(uint32_t(v))) return sl::Result::eErrorFeatureNotSupported;
  // A known V1 prefix is enough to turn NVIDIA generation off. Do not copy
  // the newer structure from an older/smaller allocation supplied by a game.
  sl::DLSSGOptions disabled; disabled.structVersion=1; disabled.mode=sl::DLSSGMode::eOff;
  const auto r=setOptions(v,disabled);
  s.nativeOff=r==sl::Result::eOk;
  s.requested=opts.mode!=sl::DLSSGMode::eOff;
  return r;
}
sl::Result StateHook(const sl::ViewportHandle& v,sl::DLSSGState& state,const sl::DLSSGOptions*) {
  const auto r=getState(v,state,nullptr); if(r!=sl::Result::eOk) return r;
  if(state.structType!=sl::DLSSGState::s_structType || state.structVersion<1 || state.structVersion>4) return r;
  const auto status=Snapshot();
  state.numFramesActuallyPresented=status.submitted;
  if(state.structVersion>=2) state.numFramesToGenerateMax=5;
  if(state.structVersion>=4) state.bIsDynamicMFGSupported=sl::eFalse;
  return r;
}
sl::Result ReflexOptionsHook(const sl::ReflexOptions& opts) {
  if(opts.structType!=sl::ReflexOptions::s_structType || opts.structVersion!=1 || opts.next)
    return sl::Result::eErrorUnsupportedInterface;
  sl::ReflexOptions disabled; disabled.mode=sl::ReflexMode::eOff;
  const auto result=reflexOptions(disabled);
  State().reflexOff=result==sl::Result::eOk;
  State().gameReflexMode=uint32_t(opts.mode);
  State().limitUs=opts.frameLimitUs;
  return result;
}
sl::Result SleepHook(const sl::FrameToken& f) {
  auto o=Active(); if(!o) return reflexSleep(f);
  State().inputTick=GetTickCount64();
  std::lock_guard lock(o->io); const auto id=FrameId(uint32_t(f));
  auto* frame=FindFrame(*o,id); if(!frame) return sl::Result::eErrorInvalidState;
  if(frame->sleeping) return sl::Result::eOk;
  xell_sleep_params_t sleep{}; sleep.bLowLatencyMode=1; sleep.minimumIntervalUs=State().limitUs;
  auto& a=State().api;
  if(a.xellSetSleepMode(o->ll,&sleep)!=XELL_RESULT_SUCCESS || a.xellSleep(o->ll,id)!=XELL_RESULT_SUCCESS)
    return sl::Result::eErrorReflexAPI;
  frame->sleeping=true;
  ++State().latencySleeps; State().outputTick=GetTickCount64();
  return sl::Result::eOk;
}
void CaptureMarker(sl::PCLMarker marker,const sl::FrameToken& f) {
  const auto id=FrameId(uint32_t(f)); auto& s=State(); const auto value=uint32_t(marker);
  if(marker==sl::PCLMarker::eRenderSubmitStart) s.recording=id;
  if(marker==sl::PCLMarker::ePresentStart) s.presenting=id;
  if(auto o=Active()) {
    std::lock_guard lock(o->io);
    if(auto* frame=FindFrame(*o,id)) {
      uint32_t mapped=value;
      if(marker==sl::PCLMarker::eControllerInputSample) mapped=XELL_INPUT_SAMPLE;
      if(mapped<=6) {
        if(!(frame->markers&(1u<<mapped))) {
          const auto r=s.api.xellAddMarkerData(o->ll,id,static_cast<xell_latency_marker_type_t>(mapped));
          if(r!=XELL_RESULT_SUCCESS) frame->failed=true;
          else {
            ++s.latencyMarkers;
            frame->markers|=1u<<mapped;
            if(mapped==XELL_RENDERSUBMIT_END && id<=o->last) {
              // XeLL explicitly allows RenderSubmit and Present to overlap.
              // Keep the real timestamp; do not fabricate an early END.
              auto& state=State(); std::lock_guard guard(state.lock);
              if(++state.status.lateSubmitEnds<=4)
                xess_mod::WriteLog("native_late_submit_end","\"id\":"+std::to_string(id)
                    +",\"markers\":"+std::to_string(frame->markers));
            }
          }
        }
      }
    }
  }
}
sl::Result MarkerHook(sl::PCLMarker marker,const sl::FrameToken& f) {
  CaptureMarker(marker,f);
  return pclMarker(marker,f); // Preserve PCL statistics; Reflex sleeping is disabled separately.
}
sl::Result EvaluateHook(sl::Feature feature,const sl::FrameToken& f,const sl::BaseStructure** inputs,
    uint32_t count,sl::CommandBuffer* command) {
  if(inputs && count<=16 && (feature==sl::kFeatureReflex || feature==sl::kFeaturePCL)) {
    for(uint32_t i=0;i<count;++i) {
      const auto* in=inputs[i]; if(!in || in->structVersion!=1) continue;
      if(in->structType==sl::ReflexHelper::s_structType) {
        const auto marker=sl::MarkerUnderlying(*static_cast<const sl::ReflexHelper*>(in));
        // Only named public marker values are admitted. Unknown historical
        // sleep sentinels are not guessed from a private numeric constant.
        if(marker<uint32_t(sl::PCLMarker::eMaximum)) CaptureMarker(static_cast<sl::PCLMarker>(marker),f);
      } else if(in->structType==sl::PCLHelper::s_structType) {
        CaptureMarker(static_cast<const sl::PCLHelper*>(in)->get(),f);
      }
    }
  }
  return evaluate(feature,f,inputs,count,command);
}
bool Feature(const char* name,void* entry) {
#define H(n,f) if(strcmp(name,#n)==0) return Hook(entry,reinterpret_cast<void*>(&f),reinterpret_cast<void**>(&n));
  // Function pointer variable names intentionally differ from exported names.
#undef H
#define H(n,f,p) if(strcmp(name,n)==0) return Hook(entry,reinterpret_cast<void*>(&f),reinterpret_cast<void**>(&p));
  H("slDLSSGSetOptions",OptionsHook,setOptions) H("slDLSSGGetState",StateHook,getState)
  H("slReflexSetOptions",ReflexOptionsHook,reflexOptions) H("slReflexSleep",SleepHook,reflexSleep)
  H("slPCLSetMarker",MarkerHook,pclMarker)
#undef H
  return false;
}
sl::Result ResolveHook(sl::Feature feature,const char* name,void*& fn) {
  const auto r=getFeature(feature,name,fn);
  if(r==sl::Result::eOk && name && fn) { std::lock_guard guard(discovery); Feature(name,fn); }
  return r;
}
} // namespace
void DiscoverStreamline() {
  if(single_overlay::native::InsideLoader()) return;
  std::lock_guard guard(discovery);
  auto module=GetModuleHandleW(L"sl.interposer.dll"); if(!module) return;
  HMODULE pinned{};
  if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
      reinterpret_cast<LPCWSTR>(module),&pinned) || pinned!=module) return;
#define H(n,f,p) if(!p) Hook(reinterpret_cast<void*>(GetProcAddress(module,n)),reinterpret_cast<void*>(&f),reinterpret_cast<void**>(&p));
  H("slGetFeatureFunction",ResolveHook,getFeature)
  H("slSetTag",TagHook,setTag) H("slSetTagForFrame",TagFrameHook,setTagFrame)
  H("slSetConstants",ConstantsHook,setConstants)
  H("slEvaluateFeature",EvaluateHook,evaluate)
#undef H
  if(getFeature) {
    struct Entry {sl::Feature feature; const char* name;};
    for(const auto& e:{Entry{sl::kFeatureDLSS_G,"slDLSSGSetOptions"},Entry{sl::kFeatureDLSS_G,"slDLSSGGetState"},
        Entry{sl::kFeatureReflex,"slReflexSetOptions"},Entry{sl::kFeatureReflex,"slReflexSleep"},Entry{sl::kFeaturePCL,"slPCLSetMarker"}}) {
      void* fn{}; if(getFeature(e.feature,e.name,fn)==sl::Result::eOk && fn) Feature(e.name,fn);
    }
  }
  auto& s=State(); std::lock_guard lock(s.lock);
  s.status.streamline=setOptions && getState && reflexOptions && reflexSleep
      && (pclMarker || evaluate) && setConstants && (setTag || setTagFrame);
}
void BeforePresent(const std::shared_ptr<Output>& o) {
  auto& s=State(); auto& a=s.api; std::lock_guard lock(o->io);
  const auto id=s.presenting.load(); auto it=o->frames.find(id);
  constexpr uint32_t required=(1u<<XEFG_SWAPCHAIN_RES_DEPTH)|(1u<<XEFG_SWAPCHAIN_RES_MOTION_VECTOR);
  bool ready=it!=o->frames.end() && id>o->last && s.nativeOff && s.reflexOff && s.requested && o->compatible;
  if(ready) {
    auto& f=it->second;
    // RenderSubmit END can legally arrive after Present starts or returns.
    // Admission requires the observed frame's starts, simulation and sleep;
    // END continues to be forwarded at its actual arrival in CaptureMarker.
    constexpr uint32_t beforePresent=(1u<<XELL_SIMULATION_START)|(1u<<XELL_SIMULATION_END)
        |(1u<<XELL_RENDERSUBMIT_START)|(1u<<XELL_PRESENT_START);
    ready=f.constants && f.valid && !f.failed && f.sleeping && (f.markers&beforePresent)==beforePresent
        && (f.resources&required)==required && f.motionWidth && f.motionHeight;
    if(ready) {
      auto data=f.data; data.motionVectorScaleX*=f.motionWidth; data.motionVectorScaleY*=f.motionHeight;
      data.resetHistory|=o->last==0 || id!=o->last+1;
      xefg_swapchain_d3d12_resource_data_t region{}; region.type=XEFG_SWAPCHAIN_RES_BACKBUFFER;
      region.validity=XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT; region.resourceSize={o->width,o->height};
      ready=a.xefgSwapChainD3D12TagFrameResource(o->fg,nullptr,id,&region)>=0
          && a.xefgSwapChainTagFrameConstants(o->fg,id,&data)>=0
          && a.xefgSwapChainSetPresentId(o->fg,id)>=0;
    }
  }
  if(ready!=o->enabled) {
    if(a.xefgSwapChainSetEnabled(o->fg,ready?1:0)!=0) ready=false;
    o->enabled=ready;
    o->appliedCount=0;
  }
  const auto count=s.multiplier.load()-1;
  if(ready && count!=o->appliedCount) {
    ready=a.xefgSwapChainSetNumInterpolatedFrames(o->fg,count)==0;
    if(ready) o->appliedCount=count;
    else { a.xefgSwapChainSetEnabled(o->fg,0); o->enabled=false; }
  }
  const auto* f=it==o->frames.end()?nullptr:&it->second;
  const uint32_t admission=(f?f->resources:0)|((f?f->markers:0)<<8)
      |(uint32_t(f&&f->constants)<<16)|(uint32_t(f&&f->valid)<<17)
      |(uint32_t(f&&f->failed)<<18)|(uint32_t(f&&f->sleeping)<<19)
      |(uint32_t(s.nativeOff.load())<<20)|(uint32_t(s.reflexOff.load())<<21);
  const auto tick=GetTickCount64();
  if(Snapshot().frames<4 || (admission!=o->lastAdmission && tick-o->admissionTick>=1000)) {
    o->lastAdmission=admission; o->admissionTick=tick;
    xess_mod::WriteLog("native_frame_admission","\"id\":"+std::to_string(id)
        +",\"native_off\":"+std::to_string(s.nativeOff.load())+",\"requested\":"+std::to_string(s.requested.load())
        +",\"reflex_off\":"+std::to_string(s.reflexOff.load())
        +",\"resources\":"+std::to_string(f?f->resources:0)+",\"markers\":"+std::to_string(f?f->markers:0)
        +",\"constants\":"+std::to_string(f?f->constants:0)+",\"valid\":"+std::to_string(f?f->valid:0)
        +",\"failed\":"+std::to_string(f?f->failed:0)+",\"sleep\":"+std::to_string(f?f->sleeping:0));
  }
  { std::lock_guard guard(s.lock); s.status.inputs=ready; s.status.enabled=ready;
    s.status.multiplier=s.multiplier; ++s.status.frames; if(!ready) ++s.status.rejected; }
  if(ready) Report("XeMFG active: Streamline inputs, XeLL, Tensor model and metered output");
  else if(s.requested) Report("XeMFG bypass: missing matching inputs/markers, invalid constants, or NVIDIA disable acknowledgement");
  o->last=id;
}
void AfterPresent(const std::shared_ptr<Output>& o) {
  std::lock_guard lock(o->io); xefg_swapchain_present_status_t status{};
  if(State().api.xefgSwapChainGetLastPresentStatus(o->fg,&status)==0) {
    auto& s=State(); std::lock_guard guard(s.lock);
    s.status.submitted=status.framesPresented; s.status.sdkResult=status.frameGenResult;
  }
  // Keep late PresentEnd markers for the preceding frame, but bound memory.
  for(auto it=o->frames.begin();it!=o->frames.end();) {
    if(it->first+2<o->last) it=o->frames.erase(it); else ++it;
  }
}
}

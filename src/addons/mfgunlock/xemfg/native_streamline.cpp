// SPDX-License-Identifier: MIT
#include "native_internal.hpp"
#include "native_constants.hpp"
#include "native_continuity.hpp"
#include "../latency_marker_policy.hpp"
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
// Public pre-PCL Streamline ABI: 32-bit ReflexMarker and FrameToken reference.
// Keep a separate original: some plugins export both entrypoints with distinct bodies.
using LegacyMarker=sl::Result(uint32_t,const sl::FrameToken&);
LegacyMarker* legacyMarker{};
void* markerEntries[2]{};
PFun_slInit* initialize{};
PFun_slSetD3DDevice* setDevice{};
PFun_slShutdown* shutdown{};
PFun_slIsFeatureLoaded* getLoaded{};
PFun_slSetFeatureLoaded* setLoaded{};
std::atomic<bool> lifecycleApi{},presentationIsolated{},logicalLoaded{true},stopping{};

// slSetFeatureLoaded rebuilds Streamline's hook lists and is not thread safe.
// Use ONLY the game's startup API boundary, never a worker or Present callback.
bool IsolatePresentation(const char* boundary,bool captureRequest) {
  if(single_overlay::native::InsideLoader() || !lifecycleApi || !getLoaded || !setLoaded || Active()) return false;
  bool loaded{};
  const auto queried=getLoaded(sl::kFeatureDLSS_G,loaded);
  const auto result=queried==sl::Result::eOk?setLoaded(sl::kFeatureDLSS_G,false):queried;
  if(captureRequest && queried==sl::Result::eOk) logicalLoaded=loaded;
  bool physical=true;
  const auto verified=result==sl::Result::eOk?getLoaded(sl::kFeatureDLSS_G,physical):result;
  presentationIsolated=result==sl::Result::eOk && verified==sl::Result::eOk && !physical;
  xess_mod::WriteLog("native_presentation_isolation","\"boundary\":\""+std::string(boundary)
      +"\",\"result\":"+std::to_string(uint32_t(result))+",\"isolated\":"+(presentationIsolated.load()?"true":"false")
      +",\"game_loaded\":"+(logicalLoaded.load()?"true":"false"));
  return presentationIsolated;
}
sl::Result InitHook(const sl::Preferences& preferences,uint64_t version) {
  std::lock_guard guard(discovery);
  stopping=false; presentationIsolated=false;
  const auto result=initialize(preferences,version);
  if(result==sl::Result::eOk) IsolatePresentation("slInit",true);
  return result;
}
sl::Result DeviceHook(void* device) {
  std::lock_guard guard(discovery);
  // Covers integrations that initialized SL before the addon was loaded.
  // This explicit initialization call belongs to the host's quiescent startup.
  if(!presentationIsolated && !stopping && !Snapshot().frames)
    IsolatePresentation("slSetD3DDevice",true);
  return setDevice(device);
}
sl::Result ShutdownHook() {
  stopping=true;
  // Drain discovery, then release the lock before SL joins its plugin workers.
  // A plugin's shutdown may itself resolve a function on another thread.
  { std::lock_guard guard(discovery); presentationIsolated=false; }
  xess_mod::WriteLog("native_streamline_shutdown","\"phase\":\"drained\"");
  ClearRetainedNative();
  const auto result=shutdown();
  xess_mod::WriteLog("native_streamline_shutdown","\"phase\":\"complete\",\"result\":"+std::to_string(uint32_t(result)));
  return result;
}
sl::Result SetLoadedHook(sl::Feature feature,bool enabled) {
  std::lock_guard guard(discovery);
  if(feature!=sl::kFeatureDLSS_G || stopping || !getLoaded) return setLoaded(feature,enabled);
  const auto result=setLoaded(feature,false);
  if(result==sl::Result::eOk) {
    bool physical=true;
    const auto verified=getLoaded(feature,physical);
    logicalLoaded=enabled; presentationIsolated=verified==sl::Result::eOk && !physical;
    if(!enabled) State().requested=false;
  }
  return result;
}
sl::Result GetLoadedHook(sl::Feature feature,bool& enabled) {
  std::lock_guard guard(discovery);
  const auto result=getLoaded(feature,enabled);
  if(result==sl::Result::eOk && feature==sl::kFeatureDLSS_G && presentationIsolated)
    enabled=logicalLoaded;
  return result;
}

bool SelectViewport(uint32_t v) {
  auto& s=State(); auto expected=UINT32_MAX;
  s.viewport.compare_exchange_strong(expected,v);
  return s.viewport.load()==v;
}
bool SelectedViewport(uint32_t v) {return State().viewport.load()==v;}
void TraceInput(const char* api,uint32_t id,uint32_t viewport=UINT32_MAX,int marker=-1) {
  if(!TraceEnabled() || id%64>=2)return;
  xess_mod::WriteLog("native_input_call","\"api\":\""+std::string(api)+"\",\"id\":"+std::to_string(id)
      +",\"viewport\":"+std::to_string(viewport)+",\"marker\":"+std::to_string(marker)+",\"thread\":"+std::to_string(GetCurrentThreadId()));
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
  if(!id || !tags || count>64 || !SelectedViewport(viewport)) return;
  auto o=Active(); if(!o) return;
  std::lock_guard lock(o->io); auto* frame=FindFrame(*o,id); if(!frame) return;
  if(o->retired || !o->fg) return;
  frame->viewport=viewport;
  for(uint32_t i=0;i<count;++i) {
    const auto& t=tags[i]; auto type=ResourceType(t.type); if(!type) continue;
    const auto bit=1u<<*type;
    const bool optional=*type==XEFG_SWAPCHAIN_RES_HUDLESS_COLOR || *type==XEFG_SWAPCHAIN_RES_UI;
    const char* rejection="metadata";
    D3D12_RESOURCE_DESC observed{};
    int sdkResult{};
    const auto reject=[&](uint32_t issues) {
      if(!optional || (frame->resources&bit)) frame->failed=true;
      // Cannot retract an earlier successful tag for this same frame: bypass
      // that batch instead. Otherwise omit only the incompatible optional tag.
      if(optional) {
        frame->hudIssues|=issues; ++State().hudRejected;
        if(o->hudLogBudget<8) { ++o->hudLogBudget; xess_mod::WriteLog("native_hud_filtered","\"id\":"+std::to_string(id)+",\"type\":"+std::to_string(*type)+",\"issues\":"+std::to_string(issues)); }
      }
      if(o->rejectedTagLogBudget<12) {
        ++o->rejectedTagLogBudget;
        xess_mod::WriteLog("native_tag_rejected","\"id\":"+std::to_string(id)+",\"type\":"+std::to_string(*type)
            +",\"reason\":\""+rejection+"\",\"lifecycle\":"+std::to_string(t.lifecycle)
            +",\"tag_version\":"+std::to_string(t.structVersion)+",\"resource_version\":"+std::to_string(t.resource?t.resource->structVersion:0)
            +",\"state\":"+std::to_string(t.resource?t.resource->state:UINT_MAX)
            +",\"format\":"+std::to_string(observed.Format)+",\"width\":"+std::to_string(observed.Width)
            +",\"height\":"+std::to_string(observed.Height)+",\"array_size\":"+std::to_string(observed.DepthOrArraySize)
            +",\"extent_x\":"+std::to_string(t.extent.left)+",\"extent_y\":"+std::to_string(t.extent.top)
            +",\"extent_width\":"+std::to_string(t.extent.width)+",\"extent_height\":"+std::to_string(t.extent.height)
            +",\"command\":"+std::string(command?"true":"false")+",\"sdk_result\":"+std::to_string(sdkResult));
      }
    };
    if(!t.resource || !t.resource->native) {
      if(frame->resources&bit) { rejection="tag_removed_after_admission"; reject(HudConflict); }
      frame->resources&=~bit; continue;
    }
    if(t.structType!=sl::ResourceTag::s_structType || t.structVersion!=1 || t.resource->structVersion!=1
        || t.resource->type!=sl::ResourceType::eTex2d || t.resource->state==UINT_MAX) { reject(HudMetadata); continue; }
    ComPtr<ID3D12Resource> resource;
    if(!single_overlay::native::Unwrap(reinterpret_cast<IUnknown*>(t.resource->native),resource)) {
      rejection="resource_unwrap";
      reject(HudMetadata); continue;
    }
    const auto desc=resource->GetDesc();
    observed=desc;
    ComPtr<ID3D12Device> device, nativeDevice; resource->GetDevice(IID_PPV_ARGS(&device));
    single_overlay::native::Unwrap(device.Get(),nativeDevice);
    ComPtr<IUnknown> identity, expectedIdentity;
    if(nativeDevice) nativeDevice.As(&identity); o->device.As(&expectedIdentity);
    if(identity.Get()!=expectedIdentity.Get() || desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D
        || desc.SampleDesc.Count!=1 || desc.DepthOrArraySize!=1) { rejection="device_or_shape"; reject(HudMetadata); continue; }
    xefg_swapchain_d3d12_resource_data_t data{};
    data.type=*type; data.pResource=resource.Get();
    data.incomingState=static_cast<D3D12_RESOURCE_STATES>(t.resource->state);
    data.resourceBase={t.extent.left,t.extent.top};
    data.resourceSize={t.extent.width?t.extent.width:uint32_t(desc.Width),t.extent.height?t.extent.height:desc.Height};
    if(uint64_t(data.resourceBase.x)+data.resourceSize.x>desc.Width
        || uint64_t(data.resourceBase.y)+data.resourceSize.y>desc.Height) { rejection="extent"; reject(HudExtent); continue; }
    if(optional) {
      const auto issues=CheckHudInput(desc,o->format,o->width,o->height,data.resourceBase.x,data.resourceBase.y,data.resourceSize.x,data.resourceSize.y);
      if(issues) { rejection="optional_hud"; reject(issues); continue; }
    }
    const unsigned hudSlot=*type==XEFG_SWAPCHAIN_RES_UI?1:0;
    ComPtr<IUnknown> allocation;
    HudTagIdentity hudIdentity{};
    if(optional) {
      if(FAILED(resource.As(&allocation))) { rejection="allocation_identity"; reject(HudMetadata); continue; }
      hudIdentity={reinterpret_cast<uintptr_t>(allocation.Get()),uint32_t(data.incomingState),uint32_t(t.lifecycle),
          data.resourceBase.x,data.resourceBase.y,data.resourceSize.x,data.resourceSize.y};
      if(ConflictingHudTag(frame->hudTags[hudSlot],hudIdentity)) {
        rejection="conflicting_hud_tag"; reject(HudConflict); continue;
      }
    }
    ComPtr<ID3D12CommandList> list;
    if(t.lifecycle==sl::eValidUntilPresent) data.validity=XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
    else if(t.lifecycle==sl::eOnlyValidNow || t.lifecycle==sl::eValidUntilEvaluate) {
      // Intel's documented ONLY_NOW contract records the copy at the tag,
      // using the producer's list and restoring its resource state. A COM
      // AddRef alone would not preserve the contents of a volatile resource.
      if(!command || !single_overlay::native::UnwrapProducerCommand(reinterpret_cast<IUnknown*>(command),list)
          || list->GetType()==D3D12_COMMAND_LIST_TYPE_COPY) { rejection="volatile_command"; reject(HudMetadata); continue; }
      ComPtr<ID3D12Device> producerDevice,nativeProducerDevice;
      ComPtr<IUnknown> producerIdentity;
      if(FAILED(list->GetDevice(IID_PPV_ARGS(&producerDevice)))
          || !single_overlay::native::Unwrap(producerDevice.Get(),nativeProducerDevice)
          || FAILED(nativeProducerDevice.As(&producerIdentity)) || producerIdentity!=expectedIdentity) {
        rejection="producer_device"; reject(HudMetadata); continue;
      }
      data.validity=XEFG_SWAPCHAIN_RV_ONLY_NOW;
    } else { rejection="lifecycle"; reject(HudMetadata); continue; }
    const auto code=[&] {
      RuntimeCall trace("xefg_tag_resource",o->diagnosticOutput,id);
      const auto r=State().api.xefgSwapChainD3D12TagFrameResource(o->fg,list.Get(),id,&data);trace.Result(r);return r;
    }();
    if(id<6) xess_mod::WriteLog("native_tag","\"type\":"+std::to_string(*type)+",\"result\":"+std::to_string(code)
        +",\"format\":"+std::to_string(desc.Format)+",\"width\":"+std::to_string(data.resourceSize.x)
        +",\"height\":"+std::to_string(data.resourceSize.y)+",\"lifecycle\":"+std::to_string(t.lifecycle));
    if(code<0) { sdkResult=code; rejection="sdk_tag"; frame->failed=true; reject(HudMetadata); }
    else {
      frame->resources|=bit;
      if(optional) { frame->hudTags[hudSlot]=hudIdentity; frame->hudAllocations[hudSlot]=std::move(allocation); }
      if(optional && !(o->hudKindsSeen&bit)) {
        o->hudKindsSeen|=bit;
        xess_mod::WriteLog("native_hud_input","\"type\":"+std::to_string(*type)+",\"format\":"+std::to_string(desc.Format)
            +",\"output_format\":"+std::to_string(o->format)+",\"width\":"+std::to_string(data.resourceSize.x)
            +",\"height\":"+std::to_string(data.resourceSize.y)+",\"encoding_verified\":false");
      }
      if(*type==XEFG_SWAPCHAIN_RES_MOTION_VECTOR) {
        frame->motionWidth=data.resourceSize.x; frame->motionHeight=data.resourceSize.y;
      }
    }
  }
}
void RejectUpstreamInput(uint32_t id,uint32_t viewport,sl::Result result,const char* api) {
  if(!id || !SelectedViewport(viewport)) return;
  if(auto output=Active()) {
    std::lock_guard lock(output->io);
    if(output->retired) return;
    if(auto* frame=FindFrame(*output,id)) frame->failed=true;
    static std::atomic<uint32_t> messages{};
    if(messages.fetch_add(1)<8) xess_mod::WriteLog("native_input_api_error","\"api\":\""+std::string(api)
        +"\",\"id\":"+std::to_string(id)+",\"result\":"+std::to_string(uint32_t(result)));
  }
}
sl::Result TagHook(const sl::ViewportHandle& v,const sl::ResourceTag* t,uint32_t n,sl::CommandBuffer* c) {
  const auto id=State().recording.load();
  TraceInput("slSetTag",id,uint32_t(v));
  const auto r=setTag(v,t,n,c);
  if(r==sl::Result::eOk) Tags(id,uint32_t(v),t,n,c);
  else RejectUpstreamInput(id,uint32_t(v),r,"slSetTag");
  return r;
}
sl::Result TagFrameHook(const sl::FrameToken& f,const sl::ViewportHandle& v,
    const sl::ResourceTag* t,uint32_t n,sl::CommandBuffer* c) {
  TraceInput("slSetTagForFrame",FrameId(uint32_t(f)),uint32_t(v));
  const auto id=FrameId(uint32_t(f)); const auto r=setTagFrame(f,v,t,n,c);
  if(r==sl::Result::eOk) Tags(id,uint32_t(v),t,n,c);
  else RejectUpstreamInput(id,uint32_t(v),r,"slSetTagForFrame");
  return r;
}
sl::Result ConstantsHook(const sl::Constants& c,const sl::FrameToken& f,const sl::ViewportHandle& v) {
  TraceInput("slSetConstants",FrameId(uint32_t(f)),uint32_t(v));
  const auto r=setConstants(c,f,v); const auto id=FrameId(uint32_t(f));
  if(r!=sl::Result::eOk) { RejectUpstreamInput(id,uint32_t(v),r,"slSetConstants");return r; }
  if(SelectedViewport(uint32_t(v))) if(auto o=Active()) {
    std::lock_guard lock(o->io);
    if(o->retired) return r;
    if(auto* frame=FindFrame(*o,id)) {
      frame->constants=true;
      frame->constantIssues=TranslateConstants(c,o->invert,o->jitter,frame->data);
      frame->valid=frame->constantIssues==0;
      if(frame->constantIssues!=o->lastConstantIssues && o->constantLogBudget<12) {
        o->lastConstantIssues=frame->constantIssues; ++o->constantLogBudget;
        std::string flags;
        if(!(frame->constantIssues&ConstantMetadata)) flags=",\"camera_motion\":"+std::to_string(int(c.cameraMotionIncluded))
            +",\"motion_3d\":"+std::to_string(int(c.motionVectors3D))+",\"inverted\":"+std::to_string(int(c.depthInverted))
            +",\"jittered\":"+std::to_string(int(c.motionVectorsJittered))+",\"reset\":"+std::to_string(int(c.reset));
        xess_mod::WriteLog("native_constants_status","\"id\":"+std::to_string(id)+",\"issues\":"+std::to_string(frame->constantIssues)
            +",\"version\":"+std::to_string(c.structVersion)+flags);
      }
    }
  }
  return r;
}
sl::Result OptionsHook(const sl::ViewportHandle& v,const sl::DLSSGOptions& opts) {
  if(opts.structType!=sl::DLSSGOptions::s_structType || opts.structVersion<1 || opts.structVersion>5)
    return sl::Result::eErrorUnsupportedInterface;
  auto& s=State();
  // OFF from a menu/secondary viewport cannot claim the selected FG output.
  static std::atomic<uint32_t> optionBudget{};
  if(optionBudget.fetch_add(1)<16) xess_mod::WriteLog("native_viewport_options","\"viewport\":"+std::to_string(uint32_t(v))
      +",\"mode\":"+std::to_string(uint32_t(opts.mode))+",\"selected\":"+std::to_string(s.viewport.load()));
  if(opts.mode!=sl::DLSSGMode::eOff && !SelectViewport(uint32_t(v))) return sl::Result::eErrorFeatureNotSupported;
  // A known V1 prefix is enough to turn NVIDIA generation off. Do not copy
  // the newer structure from an older/smaller allocation supplied by a game.
  sl::DLSSGOptions disabled; disabled.structVersion=1; disabled.mode=sl::DLSSGMode::eOff;
  const auto r=setOptions(v,disabled);
  if(!SelectedViewport(uint32_t(v))) return r;
  s.nativeOff=r==sl::Result::eOk;
  s.requested=logicalLoaded && opts.mode!=sl::DLSSGMode::eOff;
  static std::atomic<uint64_t> previous{UINT64_MAX};
  const uint64_t signature=uint64_t(uint32_t(opts.mode))|(uint64_t(uint32_t(r))<<32);
  if(previous.exchange(signature)!=signature)
    xess_mod::WriteLog("native_game_options","\"viewport\":"+std::to_string(uint32_t(v))+",\"mode\":"+std::to_string(uint32_t(opts.mode))
        +",\"version\":"+std::to_string(opts.structVersion)+",\"native_disable_result\":"+std::to_string(uint32_t(r)));
  return r;
}
sl::Result StateHook(const sl::ViewportHandle& v,sl::DLSSGState& state,const sl::DLSSGOptions*) {
  const auto r=getState(v,state,nullptr); if(r!=sl::Result::eOk) return r;
  if(state.structType!=sl::DLSSGState::s_structType || state.structVersion<1 || state.structVersion>4) return r;
  const auto status=Snapshot();
  const auto originalStatus=uint32_t(state.status);
  // The NVIDIA engine is deliberately disabled. Its Reflex/backbuffer hooks
  // do not own the Intel output, so their absence is not a XeMFG failure.
  // Preserve all other errors; never mask format, resolution or input faults.
  if(State().nativeOff && status.output) {
    constexpr auto nativeOnly=uint32_t(sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime)
        |uint32_t(sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled);
    state.status=sl::DLSSGStatus(originalStatus&~nativeOnly);
  }
  static std::atomic<uint64_t> previous{UINT64_MAX};
  const uint64_t signature=uint64_t(originalStatus)|(uint64_t(uint32_t(state.status))<<32);
  if(previous.exchange(signature)!=signature)
    xess_mod::WriteLog("native_game_state","\"native_status\":"+std::to_string(originalStatus)+",\"reported_status\":"+std::to_string(uint32_t(state.status)));
  state.numFramesActuallyPresented=status.submitted;
  if(state.structVersion>=2) state.numFramesToGenerateMax=5;
  if(state.structVersion>=4) state.bIsDynamicMFGSupported=sl::eFalse;
  return r;
}
sl::Result ReflexOptionsHook(const sl::ReflexOptions& opts) {
  if(opts.structType!=sl::ReflexOptions::s_structType || opts.structVersion!=1 || opts.next)
    return sl::Result::eErrorUnsupportedInterface;
  auto& s=State();
  const bool native=s.latencyOutput.load()==LatencyOutput::Reflex;
  // The original setter stays on the game's calling thread. Native output
  // preserves every V1 field; XeLL output suppresses the native limiter too.
  sl::ReflexOptions forwarded=opts;
  if(!native) { forwarded.mode=sl::ReflexMode::eOff; forwarded.frameLimitUs=0; }
  const auto result=reflexOptions(forwarded);
  s.reflexOptionsReady=result==sl::Result::eOk;
  s.reflexOff=result==sl::Result::eOk && forwarded.mode==sl::ReflexMode::eOff;
  s.gameReflexMode=uint32_t(opts.mode);
  s.limitUs=opts.frameLimitUs;
  return result;
}
sl::Result SleepHook(const sl::FrameToken& f) {
  const auto source=FrameId(uint32_t(f));
  TraceInput("slReflexSleep",source);
  auto o=Active(); if(!o) return reflexSleep(f);
  uint32_t target{};
  {
    std::lock_guard guard(o->io);
    auto* frame=FindFrame(*o,source);
    if(o->retired || !frame) return sl::Result::eErrorInvalidState;
    if(!frame->sourceSleepResolved) {
      const auto selected=latencymarkers::SourceSleepFrame(source,frame->observedMarkers);
      if(!selected) return sl::Result::eErrorInvalidState;
      // A next simulation that has already started cannot receive this wait.
      // Duplicates reuse the first resolved target and never dispatch twice.
      if(selected!=source) {
        const auto* next=o->frames.Find(selected);
        if(next && (next->observedMarkers || next->sleeping || next->sleepInFlight))
          return sl::Result::eErrorInvalidState;
      }
      frame->sourceSleepResolved=true;frame->sourceSleepTarget=selected;
      if(selected!=source && o->sleepAssociationLogBudget++<8)
        xess_mod::WriteLog("native_sleep_association","\"source\":"+std::to_string(source)
            +",\"target\":"+std::to_string(selected)+",\"observed\":"+std::to_string(frame->observedMarkers));
    }
    target=frame->sourceSleepTarget;
  }
  State().inputTick=GetTickCount64();
  // Native Reflex, when selected, keeps the ORIGINAL token and calling thread.
  // Only the XeLL frame association follows the simulation after this wait.
  return SleepSourceFrame(o,target,reflexSleep,&f);
}
void CaptureMarker(sl::PCLMarker marker,const sl::FrameToken& f) {
  TraceInput("latency_marker",FrameId(uint32_t(f)),UINT32_MAX,int(marker));
  const auto id=FrameId(uint32_t(f)); auto& s=State(); const auto value=uint32_t(marker);
  if(marker==sl::PCLMarker::eRenderSubmitStart) s.recording=id;
  if(marker==sl::PCLMarker::ePresentStart) s.presenting=id;
  if(auto o=Active()) {
    const auto mapped=marker==sl::PCLMarker::eControllerInputSample?uint32_t(XELL_INPUT_SAMPLE):value;
    if(mapped<=6) {
      std::lock_guard guard(o->io);
      if(!o->retired) if(auto* frame=FindFrame(*o,id)) frame->observedMarkers|=1u<<mapped;
    }
    ForwardSourceMarker(o,id,mapped);
  }
}
sl::Result MarkerHook(sl::PCLMarker marker,const sl::FrameToken& f) {
  CaptureMarker(marker,f);
  return pclMarker(marker,f); // Preserve native PCL/Reflex markers; the selected output owns the sleep.
}
sl::Result LegacyMarkerHook(uint32_t marker,const sl::FrameToken& frame) {
  if(marker<uint32_t(sl::PCLMarker::eMaximum)) CaptureMarker(static_cast<sl::PCLMarker>(marker),frame);
  return legacyMarker(marker,frame);
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
  if(strcmp(name,"slPCLSetMarker")==0 || strcmp(name,"slReflexSetMarker")==0) {
    const bool legacy=strcmp(name,"slReflexSetMarker")==0;
    auto** target=legacy?reinterpret_cast<void**>(&legacyMarker):reinterpret_cast<void**>(&pclMarker);
    if(*target) return markerEntries[legacy]==entry;
    HMODULE pin{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(entry),&pin)) return false;
    const auto other=legacy?reinterpret_cast<void*>(pclMarker):reinterpret_cast<void*>(legacyMarker);
    bool installed{};
    if(other && markerEntries[!legacy]==entry) { *target=other; installed=true; }
    else installed=Hook(entry,legacy?reinterpret_cast<void*>(&LegacyMarkerHook):reinterpret_cast<void*>(&MarkerHook),target);
    if(installed) {
      markerEntries[legacy]=entry;
      xess_mod::WriteLog("native_marker_hook",std::string("\"api\":\"")+name+"\",\"aliased\":"+(other && markerEntries[!legacy]==entry?"true":"false"));
    }
    return installed;
  }
#define H(n,f) if(strcmp(name,#n)==0) return Hook(entry,reinterpret_cast<void*>(&f),reinterpret_cast<void**>(&n));
  // Function pointer variable names intentionally differ from exported names.
#undef H
#define H(n,f,p) if(strcmp(name,n)==0) { \
  HMODULE pin{}; \
  if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN, \
      reinterpret_cast<LPCWSTR>(entry),&pin)) return false; \
  return Hook(entry,reinterpret_cast<void*>(&f),reinterpret_cast<void**>(&p)); }
  H("slDLSSGSetOptions",OptionsHook,setOptions) H("slDLSSGGetState",StateHook,getState)
  H("slReflexSetOptions",ReflexOptionsHook,reflexOptions) H("slReflexSleep",SleepHook,reflexSleep)
#undef H
  return false;
}
sl::Result ResolveHook(sl::Feature feature,const char* name,void*& fn) {
  // The host owns the shutdown contract. Do not acquire our discovery lock or
  // publish new detours from a plugin's teardown callback.
  if(stopping) return getFeature(feature,name,fn);
  std::lock_guard guard(discovery);
  const auto r=getFeature(feature,name,fn);
  if(!stopping && r==sl::Result::eOk && name && fn) Feature(name,fn);
  return r;
}
} // namespace
bool OwnsPresentationHooks() noexcept { return lifecycleApi && presentationIsolated && !stopping; }
bool StreamlineStopping() noexcept { return stopping; }
void CaptureStreamlineStartup() {
  // No SDK entry calls here. These exports can be captured before slInit even
  // when the host's interposer is only mapped during the loader's attach phase.
  auto module=GetModuleHandleW(L"sl.interposer.dll"); if(!module) return;
  HMODULE pin{};
  if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
      reinterpret_cast<LPCWSTR>(module),&pin) || pin!=module) return;
#define H(n,f,p) if(!p) Hook(reinterpret_cast<void*>(GetProcAddress(module,n)),reinterpret_cast<void*>(&f),reinterpret_cast<void**>(&p));
  H("slIsFeatureLoaded",GetLoadedHook,getLoaded)
  H("slSetFeatureLoaded",SetLoadedHook,setLoaded)
  H("slShutdown",ShutdownHook,shutdown)
  H("slSetD3DDevice",DeviceHook,setDevice)
  H("slInit",InitHook,initialize)
#undef H
  // Incomplete installation must not disable NVIDIA or create an Intel output
  // without the query/control/shutdown hooks needed to keep that owner safe.
  lifecycleApi=getLoaded && setLoaded && shutdown && (initialize || setDevice);
}
void DiscoverStreamline() {
  if(single_overlay::native::InsideLoader() || stopping) return;
  std::lock_guard guard(discovery);
  if(stopping) return;
  CaptureStreamlineStartup();
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
        Entry{sl::kFeatureReflex,"slReflexSetOptions"},Entry{sl::kFeatureReflex,"slReflexSleep"},
        Entry{sl::kFeaturePCL,"slPCLSetMarker"},Entry{sl::kFeatureReflex,"slReflexSetMarker"}}) {
      void* fn{}; if(getFeature(e.feature,e.name,fn)==sl::Result::eOk && fn) Feature(e.name,fn);
    }
  }
  auto& s=State(); std::lock_guard lock(s.lock);
  s.status.streamline=setOptions && getState && reflexOptions && reflexSleep
      && (pclMarker || legacyMarker || evaluate) && setConstants && (setTag || setTagFrame);
}
void BeforePresent(const std::shared_ptr<Output>& o) {
  auto& s=State(); auto& a=s.api; std::lock_guard lock(o->io);
  if(o->retired || !o->fg) return;
  const auto id=s.presenting.load(); auto* frame=o->frames.Find(id);
  s.lastHudIssues=frame?frame->hudIssues:0;
  s.lastHudInputs=frame?frame->resources&((1u<<XEFG_SWAPCHAIN_RES_HUDLESS_COLOR)|(1u<<XEFG_SWAPCHAIN_RES_UI)):0;
  const bool hudless=frame && !frame->failed
      && (frame->resources&(1u<<XEFG_SWAPCHAIN_RES_HUDLESS_COLOR));
  const bool compositionReady=ApplyUiComposition(*o,hudless);
  constexpr uint32_t required=(1u<<XEFG_SWAPCHAIN_RES_DEPTH)|(1u<<XEFG_SWAPCHAIN_RES_MOTION_VECTOR);
  const bool nativeReflex=s.latencyOutput.load()==LatencyOutput::Reflex;
  const bool latencyReady=s.reflexOptionsReady && (nativeReflex?s.gameReflexMode!=0:s.reflexOff.load());
  // Present identity belongs to the source frame, including passthrough.
  // Missing FG inputs must not leave the Intel proxy on the previous ID.
  const auto presentIdResult=id && id>o->last?a.xefgSwapChainSetPresentId(o->fg,id):XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
  bool ready=OwnsPresentationHooks() && presentIdResult>=0 && compositionReady && frame && id>o->last && s.nativeOff && latencyReady && s.requested && o->compatible;
  if(ready) {
    auto& f=*frame;
    // RenderSubmit END can legally arrive after Present starts or returns.
    // Admission requires the observed frame's starts, simulation and sleep;
    // END continues to be forwarded at its actual arrival in CaptureMarker.
    constexpr uint32_t beforePresent=(1u<<XELL_SIMULATION_START)|(1u<<XELL_SIMULATION_END)
        |(1u<<XELL_RENDERSUBMIT_START)|(1u<<XELL_PRESENT_START);
    ready=f.constants && f.valid && !f.failed && f.sleeping && f.latencyOutput==s.latencyOutput.load() && (f.markers&beforePresent)==beforePresent
        && (f.resources&required)==required && f.motionWidth && f.motionHeight;
    if(ready) {
      auto data=f.data; data.motionVectorScaleX*=f.motionWidth; data.motionVectorScaleY*=f.motionHeight;
      data.resetHistory|=o->last==0 || id!=o->last+1;
      xefg_swapchain_d3d12_resource_data_t region{}; region.type=XEFG_SWAPCHAIN_RES_BACKBUFFER;
      region.validity=XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT; region.resourceSize={o->width,o->height};
      ready=a.xefgSwapChainD3D12TagFrameResource(o->fg,nullptr,id,&region)>=0
          && a.xefgSwapChainTagFrameConstants(o->fg,id,&data)>=0;
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
  const auto* f=frame;
  if(id && !ready && o->bypassPresentLogBudget<8) {
    ++o->bypassPresentLogBudget;
    xess_mod::WriteLog("native_bypass_present_id","\"id\":"+std::to_string(id)+",\"result\":"+std::to_string(presentIdResult));
  }
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
  if(ready) Report(nativeReflex?"XeMFG active: Streamline inputs and native Reflex; pacing state is reported separately":
      "XeMFG active: Streamline inputs and XeLL; pacing state is reported separately");
  else if(!s.requested) Report("XeMFG paused: the game is not requesting frame generation");
  else if(s.requested && nativeReflex && !latencyReady) Report("XeMFG bypass: enable native NVIDIA Reflex in the game for the selected latency output");
  else if(frame && !(frame->resources&(1u<<XEFG_SWAPCHAIN_RES_DEPTH))) Report("XeMFG bypass: this frame has no accepted depth input");
  else if(frame && !(frame->resources&(1u<<XEFG_SWAPCHAIN_RES_MOTION_VECTOR))) Report("XeMFG bypass: this frame has no accepted motion vectors");
  else if(frame && frame->constantIssues) Report("XeMFG bypass: camera/motion constants incompatible; inspect native_constants_status");
  else if(frame && !frame->markers) Report("XeMFG bypass: no matching source-frame latency markers");
  else if(s.requested) Report("XeMFG bypass: missing matching inputs/markers, invalid constants, or NVIDIA disable acknowledgement");
  o->last=id;
}
void AfterPresent(const std::shared_ptr<Output>& o,CpuProbe* probe,HRESULT result) {
  std::lock_guard lock(o->io); xefg_swapchain_present_status_t status{};
  if(o->retired || !o->fg) return;
  // Log before querying SDK status: retain the original failing Present and
  // the device's more specific reason without swallowing/retrying the error.
  if(FAILED(result) && o->presentFailureLogBudget<4) {
    ++o->presentFailureLogBudget;
    const auto removed=o->device?o->device->GetDeviceRemovedReason():E_NOINTERFACE;
    xess_mod::WriteLog("native_present_failure","\"output\":"+std::to_string(o->diagnosticOutput)
        +",\"frame\":"+std::to_string(o->last)+",\"present_hresult\":"+std::to_string(uint32_t(result))
        +",\"device_removed_reason\":"+std::to_string(uint32_t(removed))
        +",\"fg_enabled\":"+std::to_string(o->enabled)+",\"epoch\":"+std::to_string(o->epoch));
  }
  const auto queryResult=State().api.xefgSwapChainGetLastPresentStatus(o->fg,&status);
  if(queryResult==0) {
    auto& s=State(); std::lock_guard guard(s.lock);
    s.status.submitted=status.framesPresented; s.status.sdkResult=status.frameGenResult;
  }
  // Keep late PresentEnd markers for the preceding frame, but bound memory.
  o->frames.Prune(o->last);
  if(probe && probe->active) {
    auto& sample=probe->sample; sample.frame=o->last; sample.epoch=o->epoch; sample.output=o->diagnosticOutput;
    sample.presentResult=result; sample.admitted=o->enabled; sample.requestedOutputs=State().multiplier;
    sample.sdkOutputs=status.framesPresented; sample.sdkResult=status.frameGenResult; sample.sdkEnabled=status.isFrameGenEnabled!=0;
    sample.sdkQueryResult=queryResult;
    if(auto* frame=o->frames.Find(o->last)) sample.sleepUs=frame->sleepUs;
    probe->Finish();
  }
}
}

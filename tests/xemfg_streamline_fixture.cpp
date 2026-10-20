// SPDX-License-Identifier: MIT
// Streamline ABI fixture only. Rendering and XeFG/XeLL use the real D3D12 SDK.
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <atomic>
#include <cstring>
static std::atomic<uint32_t> fgMode{}, reflexMode{}, calls{}, reflexLimit{}, reflexSleeps{};
static std::atomic<uint32_t> fgStatus{};
#define EXPORT extern "C" __declspec(dllexport) __declspec(noinline)
static __declspec(noinline) sl::Result Options(const sl::ViewportHandle&,const sl::DLSSGOptions& o) {
  ++calls; fgMode=uint32_t(o.mode); return sl::Result::eOk;
}
static __declspec(noinline) sl::Result State(const sl::ViewportHandle&,sl::DLSSGState& s,const sl::DLSSGOptions*) {
  ++calls; s.numFramesActuallyPresented=1; s.status=sl::DLSSGStatus(fgStatus.load()); return sl::Result::eOk;
}
static __declspec(noinline) sl::Result Reflex(const sl::ReflexOptions& o) {
  ++calls; reflexMode=uint32_t(o.mode); reflexLimit=o.frameLimitUs; return sl::Result::eOk;
}
static __declspec(noinline) sl::Result Sleep(const sl::FrameToken& f) { ++reflexSleeps; calls+=uint32_t(f)+1; return sl::Result::eOk; }
static __declspec(noinline) sl::Result Marker(sl::PCLMarker m,const sl::FrameToken& f) {
  calls+=uint32_t(f)+uint32_t(m)+1; return sl::Result::eOk;
}
EXPORT sl::Result slGetFeatureFunction(sl::Feature,const char* name,void*& fn) {
  ++calls;
  if(!strcmp(name,"slDLSSGSetOptions")) fn=reinterpret_cast<void*>(&Options);
  else if(!strcmp(name,"slDLSSGGetState")) fn=reinterpret_cast<void*>(&State);
  else if(!strcmp(name,"slReflexSetOptions")) fn=reinterpret_cast<void*>(&Reflex);
  else if(!strcmp(name,"slReflexSleep")) fn=reinterpret_cast<void*>(&Sleep);
  else if(!strcmp(name,"slPCLSetMarker")) fn=reinterpret_cast<void*>(&Marker);
  else { fn=nullptr; return sl::Result::eErrorFeatureMissing; }
  return sl::Result::eOk;
}
EXPORT sl::Result slSetTag(const sl::ViewportHandle& v,const sl::ResourceTag*,uint32_t n,sl::CommandBuffer*) {
  calls+=uint32_t(v)+n+1; return sl::Result::eOk;
}
EXPORT sl::Result slSetTagForFrame(const sl::FrameToken& f,const sl::ViewportHandle&,const sl::ResourceTag*,uint32_t n,sl::CommandBuffer*) {
  calls+=uint32_t(f)+n+1; return sl::Result::eOk;
}
EXPORT sl::Result slSetConstants(const sl::Constants&,const sl::FrameToken& f,const sl::ViewportHandle&) {
  calls+=uint32_t(f)+1; return sl::Result::eOk;
}
EXPORT uint32_t FixtureModes() { return fgMode|(reflexMode<<8); }
EXPORT void FixtureFgStatus(uint32_t value) { fgStatus=value; }

EXPORT uint32_t FixtureReflexSleeps() { return reflexSleeps; }
EXPORT uint32_t FixtureReflexLimit() { return reflexLimit; }

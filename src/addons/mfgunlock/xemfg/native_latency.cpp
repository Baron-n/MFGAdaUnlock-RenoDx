// SPDX-License-Identifier: MIT
// Own XeLL translation. A shared owner pins context lifetime; bookkeeping
// locks never span xellSleep, which may need another thread's late marker.
#include "native_internal.hpp"
#include <chrono>
namespace mfgunlock::xemfg::native {
sl::Result SleepSourceFrame(const std::shared_ptr<Output>& o,uint32_t id) {
  auto& s=State(); auto& a=s.api;
  uint64_t epoch{};
  {
    std::lock_guard guard(o->io);
    auto* frame=FindFrame(*o,id); if(!frame) return sl::Result::eErrorInvalidState;
    if(frame->sleeping) return sl::Result::eOk;
    if(o->sleepActive || frame->sleepInFlight) return sl::Result::eErrorInvalidState;
    const auto interval=s.limitUs.load();
    if(!o->sleepConfigured || o->sleepInterval!=interval) {
      xell_sleep_params_t mode{}; mode.bLowLatencyMode=1; mode.minimumIntervalUs=interval;
      if(a.xellSetSleepMode(o->ll,&mode)!=XELL_RESULT_SUCCESS) return sl::Result::eErrorReflexAPI;
      o->sleepConfigured=true; o->sleepInterval=interval; ++s.latencyModeUpdates;
      xess_mod::WriteLog("native_sleep_mode","\"interval_us\":"+std::to_string(interval)+",\"low_latency\":true");
    }
    epoch=o->epoch; frame->sleepInFlight=true; o->sleepActive=true;
  }
  const auto start=std::chrono::steady_clock::now();
  const auto result=a.xellSleep(o->ll,id);
  s.lastSleepUs=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
  {
    std::lock_guard guard(o->io);
    o->sleepActive=false;
    auto found=o->frames.find(id);
    if(epoch!=o->epoch || found==o->frames.end()) return sl::Result::eErrorInvalidState;
    auto& frame=found->second; frame.sleepInFlight=false;
    if(result!=XELL_RESULT_SUCCESS) { frame.failed=true; return sl::Result::eErrorReflexAPI; }
    frame.sleeping=true; ++s.latencySleeps; s.outputTick=GetTickCount64();
  }
  return sl::Result::eOk;
}
bool ForwardSourceMarker(const std::shared_ptr<Output>& o,uint32_t id,uint32_t marker) {
  if(marker>6) return false;
  auto& s=State(); std::lock_guard guard(o->io);
  auto* frame=FindFrame(*o,id); if(!frame) return false;
  // XeLL requires sleep to finish before any marker of the SAME frame.
  // Older frames may still submit their real END timestamp during this sleep.
  if(!frame->sleeping || frame->sleepInFlight) { frame->failed=true; return false; }
  if(frame->markers&(1u<<marker)) return true;
  if(s.api.xellAddMarkerData(o->ll,id,static_cast<xell_latency_marker_type_t>(marker))!=XELL_RESULT_SUCCESS) {
    frame->failed=true; return false;
  }
  ++s.latencyMarkers; frame->markers|=1u<<marker;
  if(marker==XELL_RENDERSUBMIT_END && id<=o->last) {
    std::lock_guard lock(s.lock);
    if(++s.status.lateSubmitEnds<=4)
      xess_mod::WriteLog("native_late_submit_end","\"id\":"+std::to_string(id)+",\"markers\":"+std::to_string(frame->markers));
  }
  return true;
}
}

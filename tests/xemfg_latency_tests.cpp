// SPDX-License-Identifier: MIT
#include "../src/addons/mfgunlock/xemfg/native_internal.hpp"
#include <condition_variable>
#include <future>
#include <iostream>
#include <stdexcept>
using namespace mfgunlock::xemfg::native;
namespace {
Bridge state;
std::mutex gateMutex;
std::condition_variable gateCv;
bool entered{}, released{};
std::atomic<bool> block{}, modeFail{};
std::atomic<unsigned> sleeps{}, modes{}, markers{};
void Require(bool value,const char* why) { if(!value) throw std::runtime_error(why); }
xell_result_t SetMode(xell_context_handle_t,const xell_sleep_params_t*) { ++modes; return modeFail?XELL_RESULT_ERROR_UNKNOWN:XELL_RESULT_SUCCESS; }
xell_result_t Sleep(xell_context_handle_t,uint32_t) {
  ++sleeps;
  if(block) { std::unique_lock lock(gateMutex); entered=true; gateCv.notify_all(); gateCv.wait_for(lock,std::chrono::seconds(2),[]{return released;}); }
  return XELL_RESULT_SUCCESS;
}
xell_result_t Marker(xell_context_handle_t,uint32_t,xell_latency_marker_type_t) {
  ++markers; { std::lock_guard lock(gateMutex); released=true; } gateCv.notify_all(); return XELL_RESULT_SUCCESS;
}
void Arm() { std::lock_guard lock(gateMutex); entered=released=false; block=true; }
void Entered() { std::unique_lock lock(gateMutex); Require(gateCv.wait_for(lock,std::chrono::seconds(1),[]{return entered;}),"sleep did not start"); }
void Release() { {std::lock_guard lock(gateMutex); released=true;} gateCv.notify_all(); }
}
namespace xess_mod { void WriteLog(const char*,const std::string&) noexcept {} }
namespace mfgunlock::xemfg::native { Bridge& State(){return state;} Output::~Output()=default; }
int main() {
  try {
    state.api.xellSetSleepMode=&SetMode; state.api.xellSleep=&Sleep; state.api.xellAddMarkerData=&Marker;
    auto output=std::make_shared<Output>(); output->last=1;
    FindFrame(*output,1)->sleeping=true;
    Arm(); auto sleeping=std::async(std::launch::async,[&]{return SleepSourceFrame(output,2);}); Entered();
    Require(SleepSourceFrame(output,3)==sl::Result::eErrorInvalidState,"concurrent sleep accepted");
    auto marker=std::async(std::launch::async,[&]{return ForwardSourceMarker(output,1,XELL_RENDERSUBMIT_END);});
    const bool timely=marker.wait_for(std::chrono::milliseconds(250))==std::future_status::ready;
    Release(); Require(marker.get(),"late marker failed"); Require(sleeping.get()==sl::Result::eOk,"sleep failed");
    Require(timely,"sleep held bookkeeping lock and blocked the marker needed to wake it");
    block=false; const auto count=sleeps.load();
    Require(SleepSourceFrame(output,2)==sl::Result::eOk && sleeps==count,"duplicate frame slept twice");
    Require(SleepSourceFrame(output,3)==sl::Result::eOk && modes==1,"identical mode reset every frame");
    state.limitUs=10000; modeFail=true;
    Require(SleepSourceFrame(output,4)==sl::Result::eErrorReflexAPI && output->sleepInterval==0,"failed mode was cached");
    modeFail=false; Require(SleepSourceFrame(output,4)==sl::Result::eOk && output->sleepInterval==10000,"mode retry missing");
    const auto markerCount=markers.load();
    Require(!ForwardSourceMarker(output,5,XELL_SIMULATION_START) && markers==markerCount,"marker sent before same-frame sleep");
    Arm(); auto stale=std::async(std::launch::async,[&]{return SleepSourceFrame(output,6);}); Entered();
    { std::lock_guard lock(output->io); ++output->epoch; output->frames.clear(); }
    Release(); Require(stale.get()==sl::Result::eErrorInvalidState && output->frames.empty(),"sleep republished an old resource epoch");
    Require(!output->sleepActive,"sleep lease leaked");
    std::cout<<"PASS: late marker during sleep, deduplication, mode cache/retry, same-frame ordering and reset epoch\n";
    return 0;
  } catch(const std::exception& e) { Release(); std::cerr<<e.what()<<'\n'; return 1; }
}

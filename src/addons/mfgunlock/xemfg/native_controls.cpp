// SPDX-License-Identifier: MIT
// Own overlay/control implementation. No OptiScaler code is incorporated.
#include "native_internal.hpp"
#include "native_controls.hpp"
#include "backend_api.hpp"
#include "../backend_selection.hpp"
#include "vendor/cuda/cuda_route.h"
#include "vendor/sdk_pacing.h"
#include <deps/imgui/imgui.h>

namespace mfgunlock::xemfg::native {
namespace {
std::mutex settingsLock;
ControlSettings settings;
std::atomic<bool> settingsSaved{true};
auto Ini() { return backend::Directory()/L"MFGAdaUnlock.ini"; }
bool ReadChoice(const wchar_t* section,const wchar_t* key,const wchar_t* choice) {
  wchar_t value[64]{};
  GetPrivateProfileStringW(section,key,L"Auto",value,64,Ini().c_str());
  return _wcsicmp(value,choice)==0;
}
}
void LoadControls() {
  std::lock_guard guard(settingsLock);
  settings.input=ReadChoice(L"Latency",L"Input",L"Reflex")?LatencyInput::StreamlineReflex:LatencyInput::Auto;
  settings.output=ReadChoice(L"Latency",L"Output",L"XeLL")?LatencyOutput::XeLL:LatencyOutput::Auto;
  settings.forceBorderless=GetPrivateProfileIntW(L"XeFG",L"ForceBorderless",0,Ini().c_str())!=0;
  wchar_t flip[32]{}; GetPrivateProfileStringW(L"XeFG",L"XeFGFlipMetering",L"auto",flip,32,Ini().c_str());
  settings.flipMetering=_wcsicmp(flip,L"off") && _wcsicmp(flip,L"false") && wcscmp(flip,L"0");
}
bool ConfigureControls(const ControlSettings& value) {
  // This transport has one implemented input and one compatible output. Never
  // persist unsupported enum values or silently combine Reflex sleep + XeLL.
  if(value.version!=1 || uint32_t(value.input)>1 || uint32_t(value.output)>1) return false;
  ControlSettings old;
  { std::lock_guard guard(settingsLock); old=settings; settings=value; }
  const auto ini=Ini();
  bool saved=WritePrivateProfileStringW(L"Latency",L"Input",value.input==LatencyInput::Auto?L"Auto":L"Reflex",ini.c_str())!=0;
  saved=WritePrivateProfileStringW(L"Latency",L"Output",value.output==LatencyOutput::Auto?L"Auto":L"XeLL",ini.c_str()) && saved;
  saved=WritePrivateProfileStringW(L"XeFG",L"XeFGFlipMetering",value.flipMetering?L"on":L"off",ini.c_str()) && saved;
  saved=WritePrivateProfileStringW(L"XeFG",L"ForceBorderless",value.forceBorderless?L"1":L"0",ini.c_str()) && saved;
  settingsSaved=saved;
  if(old.flipMetering!=value.flipMetering) sdk_pacing::RequestFlipMetering(value.flipMetering);
  if(old.forceBorderless!=value.forceBorderless)
    if(auto output=Active(); output && output->window) output->window->Request(value.forceBorderless);
  xess_mod::WriteLog("native_controls","\"input\":"+std::to_string(uint32_t(value.input))
      +",\"output\":"+std::to_string(uint32_t(value.output))+",\"flip_requested\":"+(value.flipMetering?"true":"false")
      +",\"borderless_requested\":"+(value.forceBorderless?"true":"false")+",\"saved\":"+(saved?"true":"false"));
  return saved;
}
ControlSettings ControlsSettings() { std::lock_guard guard(settingsLock); return settings; }
ControlStatus ControlsSnapshot() {
  ControlStatus result;
  result.settings=ControlsSettings();
  result.settingsSaved=settingsSaved;
  mfgunlock::xemfg::Status model;
  result.modelUpdating=!TryModelStatus(model);
  if(!result.modelUpdating) { result.modelPort=model.model!=0; result.modelInitialized=model.initialized!=0; }
  auto& s=State(); const auto now=GetTickCount64();
  result.sleeps=s.latencySleeps; result.markers=s.latencyMarkers;
  result.reflexObserved=s.inputTick && now-s.inputTick.load()<2000;
  result.xellObserved=s.outputTick && now-s.outputTick.load()<2000;
  result.nativeReflexDisabled=s.reflexOff; result.gameReflexMode=s.gameReflexMode;
  xefg_cuda::Status cuda;
  result.cudaUpdating=!xefg_cuda::TrySnapshot(cuda);
  result.cudaState=uint32_t(cuda.state); result.cudaLayers=cuda.layers;
  result.cudaMajor=cuda.major; result.cudaMinor=cuda.minor;
  result.cudaLaunches=cuda.launches; result.cudaFallbacks=cuda.fallbacks;
  strncpy_s(result.cudaReason,cuda.reason.c_str(),_TRUNCATE);
  auto output=Active();
  if(output && output->window) result.window=output->window->Snapshot();
  const auto flip=sdk_pacing::Snapshot(output?output->fg:nullptr);
  result.flipAvailable=flip.available; result.flipInstalled=flip.installed;
  result.flipRegistered=flip.registered; result.flipQueue=flip.queue; result.flipBusy=flip.busy;
  result.flipRequested=flip.requested; result.flipEnabled=flip.enabled;
  result.flipConfigured=flip.configured; result.flipMetered=flip.metered; result.flipRefused=flip.refused;
  result.flipOutputs=flip.outputs; result.flipFeedback=flip.feedback; result.flipResult=flip.status; result.flipCalls=flip.calls;
  return result;
}
void DrawControls() {
  const auto status=ControlsSnapshot(); auto value=status.settings;
  ImGui::Separator();
  ImGui::TextUnformatted("Model and presentation");
  const char* model=status.modelUpdating || status.cudaUpdating?"updating":!status.modelInitialized?"waiting for initialized output":!status.modelPort?"Native XeFG (model port unavailable)":
      status.cudaState==1 && status.cudaLaunches?"MFG CUDA-port / Tensor Cores":
      status.cudaState==1?"MFG port / CUDA ready (no launches yet)":"MFG port / DXIL";
  ImGui::Text("XeFG model: %s",model);
  ImGui::Text("CUDA: %s | Compute %d.%d | Layers: %u",status.cudaUpdating?"updating":status.cudaState==1?"ready":
      status.cudaState==2?"unavailable":status.cudaState==3?"disabled":"waiting",status.cudaMajor,status.cudaMinor,status.cudaLayers);
  ImGui::Text("Tensor launches: %llu | Dispatch fallbacks: %llu",status.cudaLaunches,status.cudaFallbacks);
  if(status.cudaReason[0]) ImGui::TextWrapped("CUDA detail: %s",status.cudaReason);
  if(ImGui::Checkbox("Driver flip metering",&value.flipMetering)) ConfigureControls(value);
  const char* flip=status.flipBusy?"updating":!status.flipAvailable?"unavailable (SDK pacing)":
      !status.flipRegistered || !status.flipQueue?"waiting for presentation queue":
      status.flipRefused?"driver refused (SDK pacing)":status.flipMetered?"accepted by driver":
      status.flipConfigured?"configured; independent flip unavailable":
      !status.flipEnabled?"off":!Snapshot().enabled?"waiting for frame generation":"waiting for cadence / SDK pacing";
  ImGui::Text("Flip metering: %s",flip);
  if(status.flipRequested!=status.flipEnabled) ImGui::TextUnformatted("Flip request pending until next generated batch");
  if(status.flipCalls) ImGui::Text("Driver result: %d | Feedback: 0x%X | Batch: %u",status.flipResult,status.flipFeedback,status.flipOutputs);
  if(ImGui::Checkbox("Force Borderless",&value.forceBorderless)) ConfigureControls(value);
  const auto& w=status.window;
  ImGui::Text("Window: %s%s",!w.available?"unavailable":w.borderless?(w.coversMonitor?"borderless fullscreen":"borderless window"):
      "windowed with borders",w.pending?" (change pending)":"");
  if(w.error) ImGui::Text("Window change error: %lu (game/another addon may own this state)",static_cast<unsigned long>(w.error));
  if(value.forceBorderless && w.available && !w.pending && (!w.borderless || !w.coversMonitor || w.error)) {
    if(ImGui::Button("Reapply borderless")) if(auto output=Active(); output && output->window) output->window->Request(true);
  }
  ImGui::Separator();
  ImGui::TextUnformatted("Low latency");
  ImGui::Text("Active input: %s | Active output: %s",status.reflexObserved?"Reflex / Streamline":"waiting for Reflex",
      status.xellObserved?"XeLL":"waiting for XeLL sleep");
  int input=int(value.input), output=int(value.output);
  if(ImGui::Combo("Latency input",&input,"Auto\0Reflex (Streamline)\0")) { value.input=LatencyInput(input); ConfigureControls(value); }
  if(ImGui::Combo("Latency output",&output,"Auto\0XeLL\0")) { value.output=LatencyOutput(output); ConfigureControls(value); }
  ImGui::TextWrapped("Auto uses the detected Reflex/Streamline input and XeLL output. This bridge currently supports this route. XeMFG requires XeLL; Reflex output belongs to the DLSS-G backend.");
  ImGui::Text("Native Reflex sleep: %s | Game Reflex request: %s",status.nativeReflexDisabled?"disabled":"not acknowledged",
      status.gameReflexMode==0?"off":status.gameReflexMode==1?"on":"on + boost");
  ImGui::Text("XeLL sleep calls: %llu | Markers forwarded: %llu",status.sleeps,status.markers);
  if(!status.settingsSaved) ImGui::TextWrapped("Settings are active, but writing MFGAdaUnlock.ini failed. Check directory permissions.");
}
}
extern "C" __declspec(dllexport) bool MFGAXeControlsStatus(mfgunlock::xemfg::native::ControlStatus* out,uint32_t size) {
  if(!out || size!=sizeof(*out)) return false;
  *out=mfgunlock::xemfg::native::ControlsSnapshot(); return true;
}
extern "C" __declspec(dllexport) bool MFGAXeConfigure(const mfgunlock::xemfg::native::ControlSettings* value,uint32_t size) {
  return value && size==sizeof(*value) && mfgunlock::xemfg::native::ConfigureControls(*value);
}

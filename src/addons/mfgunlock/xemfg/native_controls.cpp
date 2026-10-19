// SPDX-License-Identifier: MIT
// Own overlay/control implementation. No OptiScaler code is incorporated.
#include "native_internal.hpp"
#include "native_controls.hpp"
#include "backend_api.hpp"
#include "../backend_selection.hpp"
#include "vendor/cuda/cuda_route.h"
#include "vendor/sdk_pacing.h"
#include "../panel_ui.hpp"
#include <sstream>

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
  result.modeUpdates=s.latencyModeUpdates; result.lastSleepUs=s.lastSleepUs;
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
  else if(output) result.window.error=output->windowBindError;
  const auto flip=sdk_pacing::Snapshot(output?output->fg:nullptr);
  result.flipAvailable=flip.available; result.flipInstalled=flip.installed;
  result.flipRegistered=flip.registered; result.flipQueue=flip.queue; result.flipBusy=flip.busy;
  result.flipRequested=flip.requested; result.flipEnabled=flip.enabled;
  result.flipConfigured=flip.configured; result.flipMetered=flip.metered; result.flipRefused=flip.refused;
  result.flipOutputs=flip.outputs; result.flipFeedback=flip.feedback; result.flipResult=flip.status; result.flipCalls=flip.calls;
  return result;
}
void DrawControls(PanelPage page) {
  const auto status=ControlsSnapshot(); auto value=status.settings;
  const bool metering=status.flipMetered && !status.flipBusy;
  const char* flip=status.flipBusy?"Checking...":!status.flipAvailable?"Unavailable - using XeFG pacing":
      !status.flipRegistered || !status.flipQueue?"Waiting for frame generation":
      status.flipRefused?"Driver declined - using XeFG pacing":metering?"Active - accepted by the driver":
      status.flipConfigured?"Using XeFG pacing in this display mode":!status.flipEnabled?"Off":"Waiting for a stable frame rate";
  const auto& w=status.window;
  const char* window=!w.available?"Window not available":w.borderless?(w.coversMonitor?"Borderless fullscreen":"Borderless window"):"Windowed";
  if(page==PanelPage::General) {
    ui::Section("Smoothness and display");
    ui::Card card("##xe_presentation");
    ui::Setting("##xe_flip_row","Driver frame pacing","Let the NVIDIA driver space the generated frames. XeFG pacing remains available if the driver cannot use this mode.",[&] {
      if(ImGui::Checkbox("##xe_flip",&value.flipMetering)) ConfigureControls(value);
    });
    ImGui::TextColored(metering?ui::Accent:ui::Muted,"%s",flip);
    if(status.flipRequested!=status.flipEnabled) ui::Description("Change pending - applies with the next group of generated frames.");
    ImGui::Separator();
    ui::Setting("##xe_window_row","Force Borderless","Fill the current monitor without window borders. Use this if the game requests exclusive fullscreen.",[&] {
      if(ImGui::Checkbox("##xe_borderless",&value.forceBorderless)) ConfigureControls(value);
    });
    ImGui::Text("Display mode: %s%s",window,w.pending?" (change pending)":"");
    if(w.error==ERROR_BUSY) ui::Notice("The game or another addon changed the window. The current window state was preserved.");
    else if(w.error) ui::Notice("The window mode could not be changed. Diagnostics has the reported error.");
    if(value.forceBorderless && w.available && !w.pending && (!w.borderless || !w.coversMonitor || w.error))
      if(ImGui::Button("Apply borderless again")) if(auto output=Active(); output && output->window) output->window->Request(true);
  }
  if(page==PanelPage::Quality) {
    ui::Section("Image quality","XeMFG reconstructs frames with Intel's multi-frame model.");
    ui::Card card("##xe_quality");
    ImGui::TextUnformatted("XeMFG reconstruction");
    ui::Description("The model is selected automatically. DLSS-G quality profiles belong to the NVIDIA backend and are not applied to XeMFG.");
    ImGui::Separator();
    const char* acceleration=status.modelUpdating || status.cudaUpdating?"Checking acceleration...":!status.modelInitialized?"Waiting for the model":!status.modelPort?"Native XeFG compatibility model":
        status.cudaState==1 && status.cudaLaunches?"NVIDIA Tensor Core acceleration active":
        status.cudaState==1?"NVIDIA acceleration ready":"Compatibility shader route";
    ImGui::TextColored(ui::Accent,"%s",acceleration);
    ui::Description("A higher rendered frame rate gives frame generation more recent motion information. Extra generated frames improve smoothness; they do not add new input samples.");
  }
  if(page==PanelPage::Latency) {
    ui::Section("Mouse and input response","XeLL schedules the game's real frames. It is active alongside XeMFG; native Reflex sleeping is replaced to avoid two competing waits.");
    ui::Card card("##xe_latency");
    ImGui::TextColored(status.xellObserved?ui::Accent:ui::Warning,"XeLL: %s",status.xellObserved?"receiving game frames":"waiting for game timing");
    ImGui::Text("Timing source: %s",status.reflexObserved?"Game Reflex / Streamline":"not observed yet");
    ui::Description("Keep the game's low-latency option enabled when available. Avoid stacking external FPS limiters while comparing responsiveness.");
    if(ImGui::CollapsingHeader("Advanced latency routing")) {
      int input=int(value.input),output=int(value.output);
      ImGui::SetNextItemWidth(-1);
      if(ImGui::Combo("Timing input",&input,"Auto (recommended)\0Reflex / Streamline\0")) { value.input=LatencyInput(input); ConfigureControls(value); }
      ImGui::SetNextItemWidth(-1);
      if(ImGui::Combo("Latency output",&output,"Auto (recommended)\0XeLL\0")) { value.output=LatencyOutput(output); ConfigureControls(value); }
      ui::Description("Auto uses Reflex timing from the game and XeLL for the output. This bridge currently supports that route. To use native Reflex output, select the DLSS-G backend and restart.");
    }
  }
  if(page==PanelPage::Diagnostics) {
    ui::Section("Model and timing details");
    ui::Card card("##xe_model_details");
    ImGui::Text("Model port: %s | Initialized: %s",status.modelUpdating?"updating":status.modelPort?"MFG":"native XeFG",status.modelInitialized?"yes":"no");
    ImGui::Text("CUDA state: %u | Compute: %d.%d | Layers: %u",status.cudaState,status.cudaMajor,status.cudaMinor,status.cudaLayers);
    ImGui::Text("Tensor launches: %llu | Dispatch fallbacks: %llu",status.cudaLaunches,status.cudaFallbacks);
    if(status.cudaReason[0]) ImGui::TextWrapped("CUDA detail: %s",status.cudaReason);
    ImGui::TextWrapped("Flip metering: %s",flip);
    ImGui::Text("NVAPI result: %d | Feedback: 0x%X | Outputs: %u",status.flipResult,status.flipFeedback,status.flipOutputs);
    ImGui::Text("Window error: %lu | Managed: %s",static_cast<unsigned long>(w.error),w.managed?"yes":"no");
    ImGui::Text("Native Reflex disabled: %s | Game request: %u",status.nativeReflexDisabled?"yes":"no",status.gameReflexMode);
    ImGui::Text("XeLL sleeps: %llu | Markers: %llu",status.sleeps,status.markers);
    ImGui::Text("Sleep-mode changes: %llu | Last CPU sleep: %llu us",status.modeUpdates,status.lastSleepUs);
    ui::Description("Driver acceptance is not a scanout measurement. Counters are accumulated for this process.");
    if(ImGui::Button("Copy XeMFG diagnostics")) {
      std::ostringstream report; report<<"MFG Unlock XeMFG R1.3\nCUDA="<<status.cudaState<<" compute="<<status.cudaMajor<<'.'<<status.cudaMinor
          <<" launches="<<status.cudaLaunches<<"\nflip="<<flip<<" result="<<status.flipResult<<" feedback="<<status.flipFeedback
          <<"\nwindow="<<window<<" error="<<w.error<<"\nsleeps="<<status.sleeps<<" markers="<<status.markers;
      ImGui::SetClipboardText(report.str().c_str());
    }
  }
  if(!status.settingsSaved) ui::Notice("Settings are active but could not be saved. Check write access to the game folder.");
}
}
extern "C" __declspec(dllexport) bool MFGAXeControlsStatus(mfgunlock::xemfg::native::ControlStatus* out,uint32_t size) {
  if(!out || size!=sizeof(*out)) return false;
  *out=mfgunlock::xemfg::native::ControlsSnapshot(); return true;
}
extern "C" __declspec(dllexport) bool MFGAXeConfigure(const mfgunlock::xemfg::native::ControlSettings* value,uint32_t size) {
  return value && size==sizeof(*value) && mfgunlock::xemfg::native::ConfigureControls(*value);
}

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
  const char* flip=status.flipBusy?"Checking...":!status.flipAvailable?"XeFG pacing (driver unavailable)":
      !status.flipRegistered || !status.flipQueue?"Waiting for frame generation":
      status.flipRefused?"XeFG pacing (driver declined)":metering?"NVIDIA driver - active":
      status.flipConfigured?"XeFG pacing (current display mode)":!status.flipEnabled?"XeFG pacing (driver metering off)":"Waiting for a stable frame rate";
  const auto& w=status.window;
  const char* window=!w.available?"Window not available":w.borderless?(w.coversMonitor?"Borderless fullscreen":"Borderless window"):"Windowed";
  const char* acceleration=status.modelUpdating || status.cudaUpdating?"Checking...":!status.modelInitialized?"Waiting for the model":!status.modelPort?"Native XeFG model":
      status.cudaState==1 && status.cudaLaunches?"NVIDIA Tensor Cores (CUDA)":
      status.cudaState==1?"NVIDIA acceleration ready":"Compatibility shaders";
  constexpr const char* flipHelp="Uses the NVIDIA driver to space generated frames evenly. Leave enabled for driver pacing when available; otherwise XeFG uses its own pacing. Changes apply at the next frame group. Active reports driver acceptance, not a monitor scanout measurement.";
  if(page==PanelPage::General) {
    ui::Section("Smoothness and display");
    ui::Card card("##xe_presentation");
    ui::Setting("##xe_flip_row","Driver flip metering",flipHelp,[&] {
      if(ImGui::Checkbox("##xe_flip",&value.flipMetering)) ConfigureControls(value);
    });
    ui::Readout("##xe_flip_state","Pacing status",flip,flipHelp,metering?ui::Accent:ui::Muted);
    if(status.flipRequested!=status.flipEnabled) ui::Description("Change pending - applies with the next group of generated frames.");
    ImGui::Separator();
    ui::Setting("##xe_window_row","Force Borderless","Fill the current monitor without window borders. Use this if the game requests exclusive fullscreen.",[&] {
      if(ImGui::Checkbox("##xe_borderless",&value.forceBorderless)) ConfigureControls(value);
    });
    ui::Readout("##xe_window_state","Display mode",w.pending?"Change pending":window,
        "Shows the current window borders and monitor coverage. Turning Force Borderless off restores the previous mode only if the addon still owns the window changes.");
    if(w.error==ERROR_BUSY) ui::Notice("The game or another addon changed the window. The current window state was preserved.");
    else if(w.error) ui::Notice("The window mode could not be changed. Diagnostics has the reported error.");
    if(value.forceBorderless && w.available && !w.pending && (!w.borderless || !w.coversMonitor || w.error))
      if(ImGui::Button("Apply borderless again")) if(auto output=Active(); output && output->window) output->window->Request(true);
  }
  if(page==PanelPage::Quality) {
    ui::Section("Image quality");
    ui::Card card("##xe_quality");
    ui::Readout("##xe_model_quality","Model",status.modelUpdating?"Checking...":!status.modelInitialized?"Waiting for the model":status.modelPort?"Intel XeMFG (automatic)":"Intel XeFG (compatibility)",
        "The multi-frame model is selected automatically. DLSS-G quality profiles only affect the NVIDIA backend.");
    ui::Readout("##xe_acceleration_quality","Acceleration",acceleration,
        "CUDA runs the supported model layers on NVIDIA Tensor Cores. Compatibility shaders handle other dispatches. This is the execution route, not a different quality preset.",ui::Accent);
    ui::Description("For best results, keep the game's rendered frame rate stable.");
    ui::Tooltip("A higher base frame rate supplies more recent motion information. Generated frames improve smoothness but do not add new input samples.");
  }
  if(page==PanelPage::Latency) {
    ui::Section("Mouse and input response");
    ui::Card card("##xe_latency");
    ui::Readout("##xe_latency_state","Low latency",status.xellObserved?"XeLL - active":"Waiting for game timing",
        "XeLL schedules real game frames. The bridge replaces native Reflex sleeping with XeLL to avoid two competing waits. Active means recent successful XeLL sleeps, not a physical latency measurement.",status.xellObserved?ui::Accent:ui::Warning);
    ui::Readout("##xe_latency_source","Timing source",status.reflexObserved?"Reflex / Streamline":"Not observed yet",
        "Uses the game's Reflex timing and markers as input to XeLL. Keep the game's low-latency option enabled when available.");
    ui::Description("Keep the game's low-latency option enabled.");
    ui::Tooltip("Avoid stacking external FPS limiters while comparing responsiveness. Generated FPS and physical input latency measure different things.");
    if(ImGui::CollapsingHeader("Advanced latency routing")) {
      int input=int(value.input),output=int(value.output);
      ui::Setting("##xe_input_row","Timing input","Auto uses Reflex / Streamline timing. This bridge currently supports that input route.",[&] {
        if(ImGui::Combo("##xe_timing_input",&input,"Auto (recommended)\0Reflex / Streamline\0")) { value.input=LatencyInput(input); ConfigureControls(value); }
      });
      ui::Setting("##xe_output_row","Latency output","Auto uses XeLL. XeMFG needs XeLL; native Reflex output is available with the DLSS-G backend after a restart.",[&] {
        if(ImGui::Combo("##xe_latency_output",&output,"Auto (recommended)\0XeLL\0")) { value.output=LatencyOutput(output); ConfigureControls(value); }
      });
    }
  }
  if(page==PanelPage::Diagnostics) {
    const auto bridge=Snapshot();
    ui::Section("System status","Use Copy diagnostic report when reporting a problem.");
    ui::Card card("##xe_model_details");
    ui::Readout("##xe_diag_bridge","Game connection",bridge.streamline && bridge.output?"Connected":"Waiting for the game",
        "Checks the Streamline input hooks and DirectX 12 output. Load a scene and enable Frame Generation in the game.");
    ui::Readout("##xe_diag_model","Model",status.modelUpdating?"Checking...":!status.modelInitialized?"Waiting for the model":status.modelPort?"Intel XeMFG - ready":"Intel XeFG (compatibility)");
    ui::Readout("##xe_diag_cuda","Acceleration",acceleration,
        "NVIDIA Tensor Core execution is observed through the CUDA route. Raw counters are included in the copied report.");
    ui::Readout("##xe_diag_flip","Flip metering",flip,flipHelp,metering?ui::Accent:ui::Muted);
    ui::Readout("##xe_diag_latency","Latency output",status.xellObserved?"XeLL - active":"Waiting for game timing");
    ui::Readout("##xe_diag_window","Display mode",window);
    if(w.error) ui::Notice("Window mode could not be applied. Copy the report for details.");
    if(bridge.sdkResult<0) ui::Notice("Frame generation reported an error. Copy the report for details.");
    if(ImGui::Button("Copy diagnostic report")) {
      std::ostringstream report; report<<"MFG Unlock XeMFG R1.4\nmodel="<<status.modelPort<<" initialized="<<status.modelInitialized
          <<"\nCUDA="<<status.cudaState<<" compute="<<status.cudaMajor<<'.'<<status.cudaMinor<<" layers="<<status.cudaLayers
          <<" launches="<<status.cudaLaunches<<" fallbacks="<<status.cudaFallbacks<<" detail="<<status.cudaReason
          <<"\nflip="<<flip<<" result="<<status.flipResult<<" feedback="<<status.flipFeedback<<" outputs="<<status.flipOutputs<<" calls="<<status.flipCalls
          <<"\nwindow="<<window<<" error="<<w.error<<" managed="<<w.managed
          <<"\nnative_reflex_disabled="<<status.nativeReflexDisabled<<" game_reflex_mode="<<status.gameReflexMode
          <<"\nsleeps="<<status.sleeps<<" markers="<<status.markers<<" mode_changes="<<status.modeUpdates<<" last_sleep_us="<<status.lastSleepUs
          <<"\nbridge="<<bridge.detail<<" source_presents="<<bridge.frames<<" incomplete_inputs="<<bridge.rejected
          <<" submissions="<<bridge.submitted<<" SDK_result="<<bridge.sdkResult;
      ImGui::SetClipboardText(report.str().c_str());
    }
    ui::Tooltip("Copies technical status, error codes and accumulated counters for support. These counters are not displayed FPS or physical input latency.");
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

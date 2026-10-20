// SPDX-License-Identifier: MIT
// Own overlay/control implementation. No OptiScaler code is incorporated.
#include "native_internal.hpp"
#include "native_controls.hpp"
#include "native_overlay.hpp"
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
  settings.queueAhead=GetPrivateProfileIntW(L"XeFG",L"MeteredQueueAhead",1,Ini().c_str())!=0;
  sdk_pacing::RequestQueueAhead(settings.queueAhead);
  settings.output=ReadChoice(L"Latency",L"Output",L"Reflex")?LatencyOutput::Reflex:
      ReadChoice(L"Latency",L"Output",L"XeLL")?LatencyOutput::XeLL:LatencyOutput::Auto;
  State().latencyOutput=settings.output==LatencyOutput::Reflex?LatencyOutput::Reflex:LatencyOutput::XeLL;
  settings.forceBorderless=GetPrivateProfileIntW(L"XeFG",L"ForceBorderless",0,Ini().c_str())!=0;
  settings.uiComposition=GetPrivateProfileIntW(L"XeFG",L"UIComposition",0,Ini().c_str())!=0;
  settings.sdkDeadlines=GetPrivateProfileIntW(L"XeFG",L"SdkDeadlines",0,Ini().c_str())!=0;
  sdk_pacing::RequestSdkDeadlines(settings.sdkDeadlines);
  wchar_t flip[32]{}; GetPrivateProfileStringW(L"XeFG",L"XeFGFlipMetering",L"auto",flip,32,Ini().c_str());
  settings.flipMetering=_wcsicmp(flip,L"off") && _wcsicmp(flip,L"false") && wcscmp(flip,L"0");
}
bool ConfigureControls(const ControlSettings& value) {
  // Reject unknown ABI/enums. The latency output is fixed for this process;
  // changing the preference never replays a game setter on the UI thread.
  if(value.version!=4 || uint32_t(value.input)>1 || uint32_t(value.output)>2) return false;
  ControlSettings old;
  { std::lock_guard guard(settingsLock); old=settings; settings=value; }
  const auto ini=Ini();
  bool saved=WritePrivateProfileStringW(L"Latency",L"Input",value.input==LatencyInput::Auto?L"Auto":L"Reflex",ini.c_str())!=0;
  saved=WritePrivateProfileStringW(L"XeFG",L"MeteredQueueAhead",value.queueAhead?L"1":L"0",ini.c_str()) && saved;
  sdk_pacing::RequestQueueAhead(value.queueAhead);
  saved=WritePrivateProfileStringW(L"Latency",L"Output",value.output==LatencyOutput::Auto?L"Auto":value.output==LatencyOutput::Reflex?L"Reflex":L"XeLL",ini.c_str()) && saved;
  saved=WritePrivateProfileStringW(L"XeFG",L"XeFGFlipMetering",value.flipMetering?L"on":L"off",ini.c_str()) && saved;
  saved=WritePrivateProfileStringW(L"XeFG",L"ForceBorderless",value.forceBorderless?L"1":L"0",ini.c_str()) && saved;
  saved=WritePrivateProfileStringW(L"XeFG",L"UIComposition",value.uiComposition?L"1":L"0",ini.c_str()) && saved;
  saved=WritePrivateProfileStringW(L"XeFG",L"SdkDeadlines",value.sdkDeadlines?L"1":L"0",ini.c_str()) && saved;
  settingsSaved=saved;
  if(old.flipMetering!=value.flipMetering) sdk_pacing::RequestFlipMetering(value.flipMetering);
  if(old.sdkDeadlines!=value.sdkDeadlines) sdk_pacing::RequestSdkDeadlines(value.sdkDeadlines);
  if(old.forceBorderless!=value.forceBorderless)
    if(auto output=Active(); output && output->window) output->window->Request(value.forceBorderless);
  xess_mod::WriteLog("native_controls","\"input\":"+std::to_string(uint32_t(value.input))
      +",\"output\":"+std::to_string(uint32_t(value.output))+",\"flip_requested\":"+(value.flipMetering?"true":"false")
      +",\"borderless_requested\":"+(value.forceBorderless?"true":"false")
      +",\"ui_composition_requested\":"+(value.uiComposition?"true":"false")+",\"saved\":"+(saved?"true":"false"));
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
  result.activeLatencyOutput=s.latencyOutput;
  const auto wanted=result.settings.output==LatencyOutput::Reflex?LatencyOutput::Reflex:LatencyOutput::XeLL;
  result.latencyRestartPending=wanted!=result.activeLatencyOutput;
  result.reflexOutputObserved=s.reflexOutputTick && now-s.reflexOutputTick.load()<2000;
  result.reflexOptionsReady=s.reflexOptionsReady;
  result.reflexSleeps=s.reflexSleeps; result.xellSleeps=s.xellSleeps;
  result.xellObserved=result.activeLatencyOutput==LatencyOutput::XeLL && s.outputTick && now-s.outputTick.load()<2000;
  result.nativeReflexDisabled=s.reflexOff; result.gameReflexMode=s.gameReflexMode;
  result.hudIssues=s.lastHudIssues; result.hudInputs=s.lastHudInputs; result.hudRejected=s.hudRejected;
  xefg_cuda::Status cuda;
  result.cudaUpdating=!xefg_cuda::TrySnapshot(cuda);
  result.cudaState=uint32_t(cuda.state); result.cudaLayers=cuda.layers;
  result.cudaMajor=cuda.major; result.cudaMinor=cuda.minor;
  result.cudaLaunches=cuda.launches; result.cudaFallbacks=cuda.fallbacks;
  strncpy_s(result.cudaReason,cuda.reason.c_str(),_TRUNCATE);
  auto output=Active();
  if(output) {
    std::unique_lock guard(output->io,std::try_to_lock);
    if(guard.owns_lock()) {
      result.uiCompositionAvailable=output->uiCompositionAvailable;
      result.uiCompositionApplied=output->uiCompositionApplied;
      result.uiCompositionKnown=output->uiCompositionKnown;
      result.uiCompositionResult=output->uiCompositionResult;
      result.uiCompositionUpdates=output->uiCompositionUpdates;
    }
  }
  if(output && output->window) result.window=output->window->Snapshot();
  else if(output) result.window.error=output->windowBindError;
  const auto flip=sdk_pacing::Snapshot(output?output->fg:nullptr);
  result.flipAvailable=flip.available; result.flipInstalled=flip.installed;
  result.flipRegistered=flip.registered; result.flipQueue=flip.queue; result.flipBusy=flip.busy;
  result.flipRequested=flip.requested; result.flipEnabled=flip.enabled;
  result.flipConfigured=flip.configured; result.flipMetered=flip.metered; result.flipRefused=flip.refused;
  result.queueAheadActive=flip.queueAheadActive; result.queueAheadDeadlines=flip.queueAheadDeadlines;
  result.flipOutputs=flip.outputs; result.flipFeedback=flip.feedback; result.flipResult=flip.status; result.flipCalls=flip.calls;
  result.sdkDeadlinesRequested=flip.sdkDeadlinesRequested; result.sdkDeadlinesActive=flip.sdkDeadlinesActive;
  return result;
}
void DrawControls(PanelPage page) {
  const auto status=ControlsSnapshot(); auto value=status.settings;
  const bool metering=status.flipMetered && !status.flipBusy;
  const char* flip=status.sdkDeadlinesActive?"Intel SDK timing (experimental)":status.flipBusy?"Checking...":!status.flipAvailable?"XeFG pacing (driver unavailable)":
      !status.flipRegistered || !status.flipQueue?"Waiting for frame generation":
      status.flipRefused?"XeFG pacing (driver declined)":metering?"NVIDIA driver - active":
      status.flipConfigured?"XeFG pacing (current display mode)":!status.flipEnabled?"XeFG pacing (driver metering off)":"Waiting for a stable frame rate";
  const auto& w=status.window;
  const char* window=!w.available?"Window not available":w.borderless?(w.coversMonitor?"Borderless fullscreen":"Borderless window"):"Windowed";
  const char* acceleration=status.modelUpdating || status.cudaUpdating?"Checking...":!status.modelInitialized?"Waiting for the model":!status.modelPort?"Native XeFG model":
      status.cudaState==1 && status.cudaLaunches?"NVIDIA Tensor Cores (CUDA)":
      status.cudaState==1?"NVIDIA acceleration ready":"Compatibility shaders";
  constexpr const char* flipHelp="Uses the NVIDIA driver to space generated frames evenly. Leave enabled for driver pacing when available; otherwise XeFG uses its own pacing. Changes apply at the next frame group. Active reports driver acceptance, not a monitor scanout measurement.";
  constexpr const char* compositionHelp="Off by default: XeFG interpolates the HUD with the image. Enable only if HUD interpolation causes artifacts. Composition needs HUD-less color from the game and updates the HUD at the real frame rate, so moving HUD elements may look less smooth. Changes apply at the next source frame.";
  const char* composition=!status.uiCompositionKnown?"Waiting for frame status":status.uiCompositionApplied?"UI composition":
      value.uiComposition && !status.uiCompositionAvailable?"Waiting for HUD-less input":
      value.uiComposition?"Change pending":"UI interpolation (default)";
  if(page==PanelPage::General) {
    ui::Section("Smoothness and display");
    ui::Card card("##xe_presentation");
    ui::Setting("##xe_flip_row","Driver flip metering",flipHelp,[&] {
      ImGui::BeginDisabled(value.sdkDeadlines);
      if(ImGui::Checkbox("##xe_flip",&value.flipMetering)) ConfigureControls(value);
      ImGui::EndDisabled();
    });
    ui::Readout("##xe_flip_state","Pacing status",flip,flipHelp,metering?ui::Accent:ui::Muted);
    if(!value.sdkDeadlines && status.flipRequested!=status.flipEnabled) ui::Description("Change pending - applies with the next group of generated frames.");
    if(ImGui::CollapsingHeader("Advanced pacing comparison")) {
      ui::Setting("##xe_queue_row","Queue metered frames early","Lets the driver space a verified group of frames while the SDK prepares its next work. GPU fences and resource waits remain owned by Intel. Applies only when the driver accepts flip metering and Present uses sync interval 0. Turn off to compare with the previous CPU timing.",[&] {
        ImGui::BeginDisabled(value.sdkDeadlines || !value.flipMetering);
        if(ImGui::Checkbox("##xe_queue_ahead",&value.queueAhead)) ConfigureControls(value);
        ImGui::EndDisabled();
      });
      ui::Setting("##xe_deadline_row","Use Intel SDK timing (test)","Off keeps the current driver/adaptive path. On keeps Intel's original deadlines and disables driver metering at the next generated group. This experimental comparison does not add a pacer, wait or FPS cap. Keep the same scene and compare displayed intervals and mouse response; turn off to restore the usual path.",[&] {
        if(ImGui::Checkbox("##xe_sdk_deadlines",&value.sdkDeadlines)) ConfigureControls(value);
      });
      ui::Description(status.sdkDeadlinesActive?"Intel SDK timing active (experimental)":"Driver / adaptive timing active");
      if(status.sdkDeadlinesRequested!=status.sdkDeadlinesActive) ui::Description("Waiting for the next generated group.");
    }
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
    ImGui::Separator();
    ui::Setting("##xe_composition_row","UI Composition",compositionHelp,[&] {
      ImGui::BeginDisabled(!status.uiCompositionAvailable && !value.uiComposition);
      if(ImGui::Checkbox("##xe_ui_composition",&value.uiComposition)) ConfigureControls(value);
      ImGui::EndDisabled();
    });
    ui::Readout("##xe_composition_state","HUD handling",composition,compositionHelp);
    ui::Readout("##xe_hud_safety","HUD input safety",status.hudIssues?"Incompatible input filtered":status.hudInputs?"Game HUD inputs accepted":"No separate HUD texture",
        "Checks the game's optional HUD textures for matching format, full output size and valid shader input. Compatible textures can help Intel's UI interpolation with composition off. Invalid optional input falls back to backbuffer interpolation. This does not prove matching HDR encoding or remove every model artifact.");
    if(status.uiCompositionResult<0) ui::Notice("UI composition could not be applied. Copy Diagnostics for details.");
    ui::Description("For best results, keep the game's rendered frame rate stable.");
    ui::Tooltip("A higher base frame rate supplies more recent motion information. Generated frames improve smoothness but do not add new input samples.");
  }
  if(page==PanelPage::Latency) {
    ui::Section("Mouse and input response");
    ui::Card card("##xe_latency");
    ui::Readout("##xe_latency_state","Low latency",status.xellObserved?"XeLL - active":status.reflexOutputObserved?"Reflex - active":"Waiting for game timing",
        "The selected output controls latency: native Reflex or XeLL. The other latency limiter is disabled. Active means recent successful calls, not a physical latency measurement.",(status.xellObserved || status.reflexOutputObserved)?ui::Accent:ui::Warning);
    ui::Readout("##xe_latency_source","Timing source",status.reflexObserved?"Reflex / Streamline":"Not observed yet",
        "Uses the game's Reflex timing and markers for the selected latency output. Keep the game's low-latency option enabled when available.");
    ui::Description("Keep the game's low-latency option enabled.");
    ui::Tooltip("Avoid stacking external FPS limiters while comparing responsiveness. Generated FPS and physical input latency measure different things.");
    if(status.activeLatencyOutput==LatencyOutput::Reflex && status.reflexOptionsReady && !status.gameReflexMode) ui::Notice("Enable NVIDIA Reflex in the game to use this latency output.");
    if(status.latencyRestartPending) ui::Notice("Latency output saved. Restart the game to apply it.");
    if(ImGui::CollapsingHeader("Advanced latency routing")) {
      int input=int(value.input),output=int(value.output);
      ui::Setting("##xe_input_row","Timing input","Auto uses Reflex / Streamline timing. This bridge currently supports that input route.",[&] {
        if(ImGui::Combo("##xe_timing_input",&input,"Auto (recommended)\0Reflex / Streamline\0")) { value.input=LatencyInput(input); ConfigureControls(value); }
      });
      ui::Setting("##xe_output_row","Latency output","Auto keeps XeLL. Reflex uses the game's native Reflex sleep and options, with the XeLL latency limiter disabled. Keep Reflex enabled in the game's settings. Restart the game after changing this selection.",[&] {
        if(ImGui::Combo("##xe_latency_output",&output,"Auto (XeLL)\0XeLL\0Reflex\0")) { value.output=LatencyOutput(output); ConfigureControls(value); }
      });
    }
  }
  if(page==PanelPage::Diagnostics) {
    const auto bridge=Snapshot();
    const auto overlay=OverlaySnapshot();
    const auto overlayInput=OverlayInputSnapshot();
    ui::Section("System status","Use Copy diagnostic report when reporting a problem.");
    ui::Card card("##xe_model_details");
    ui::Readout("##xe_diag_bridge","Game connection",bridge.streamline && bridge.output?"Connected":"Waiting for the game",
        "Checks the Streamline input hooks and DirectX 12 output. Load a scene and enable Frame Generation in the game.");
    ui::Readout("##xe_diag_overlay","ReShade output",overlay.created>overlay.destroyed?"Final output linked":"Automatic hook / waiting",
        "Links ReShade effects and keyboard input to Intel's final output when its private queue is not automatically hooked. Recreated on resize and released before output destruction.");
    ui::Readout("##xe_diag_model","Model",status.modelUpdating?"Checking...":!status.modelInitialized?"Waiting for the model":status.modelPort?"Intel XeMFG - ready":"Intel XeFG (compatibility)");
    ui::Readout("##xe_diag_cuda","Acceleration",acceleration,
        "NVIDIA Tensor Core execution is observed through the CUDA route. Raw counters are included in the copied report.");
    ui::Readout("##xe_diag_flip","Flip metering",flip,flipHelp,metering?ui::Accent:ui::Muted);
    ui::Readout("##xe_diag_latency","Latency output",status.xellObserved?"XeLL - active":status.reflexOutputObserved?"Reflex - active":"Waiting for game timing");
    ui::Readout("##xe_diag_ui","HUD handling",composition,compositionHelp);
    ui::Readout("##xe_diag_window","Display mode",window);
    if(w.error) ui::Notice("Window mode could not be applied. Copy the report for details.");
    if(bridge.sdkResult<0) ui::Notice("Frame generation reported an error. Copy the report for details.");
    if(ImGui::Button("Copy diagnostic report")) {
      mfgunlock::xemfg::Status model;
      const bool modelAvailable=TryModelStatus(model);
      std::ostringstream report; report<<"MFG Unlock XeMFG R1.9\nstartup_failed="<<ModelStartupFailed()
          <<" model_status_available="<<modelAvailable<<" model_detail="<<(modelAvailable?model.detail:"busy")
          <<"\nmodel="<<status.modelPort<<" initialized="<<status.modelInitialized
          <<"\nCUDA="<<status.cudaState<<" compute="<<status.cudaMajor<<'.'<<status.cudaMinor<<" layers="<<status.cudaLayers
          <<" launches="<<status.cudaLaunches<<" fallbacks="<<status.cudaFallbacks<<" detail="<<status.cudaReason
          <<"\nqueue_ahead="<<status.queueAheadActive<<" deadlines="<<status.queueAheadDeadlines
          <<"\nflip="<<flip<<" result="<<status.flipResult<<" feedback="<<status.flipFeedback<<" outputs="<<status.flipOutputs<<" calls="<<status.flipCalls
          <<"\nsdk_deadlines_requested="<<status.sdkDeadlinesRequested<<" active="<<status.sdkDeadlinesActive
          <<"\nhud_inputs="<<status.hudInputs<<" issues="<<status.hudIssues<<" filtered_tags="<<status.hudRejected
          <<"\noverlay_created="<<overlay.created<<" destroyed="<<overlay.destroyed<<" updates="<<overlay.updates<<" failed="<<overlay.failed
          <<"\noverlay_requests="<<overlayInput.requests<<" recovered_taps="<<overlayInput.recovered<<" dropped_taps="<<overlayInput.dropped
          <<"\nwindow="<<window<<" error="<<w.error<<" managed="<<w.managed
          <<"\nlatency_output="<<uint32_t(status.activeLatencyOutput)<<" pending_restart="<<status.latencyRestartPending
          <<" reflex_sleeps="<<status.reflexSleeps<<" xell_sleeps="<<status.xellSleeps
          <<"\nnative_reflex_disabled="<<status.nativeReflexDisabled<<" game_reflex_mode="<<status.gameReflexMode
          <<"\nsleeps="<<status.sleeps<<" markers="<<status.markers<<" mode_changes="<<status.modeUpdates<<" last_sleep_us="<<status.lastSleepUs
          <<"\nui_composition_requested="<<value.uiComposition<<" applied="<<status.uiCompositionApplied<<" known="<<status.uiCompositionKnown
          <<" available="<<status.uiCompositionAvailable<<" result="<<status.uiCompositionResult<<" updates="<<status.uiCompositionUpdates
          <<"\nbridge="<<bridge.detail<<" source_presents="<<bridge.frames<<" incomplete_inputs="<<bridge.rejected
          <<" submissions="<<bridge.submitted<<" SDK_result="<<bridge.sdkResult;
      const auto log=LogSnapshot();
      report<<"\nasync_log_started="<<log.started<<" pending="<<log.pending<<" dropped="<<log.dropped<<" failed_batches="<<log.failed;
      ImGui::SetClipboardText(report.str().c_str());
    }
    ui::Tooltip("Copies technical status, error codes and accumulated counters for support. These counters are not displayed FPS or physical input latency.");
    if(ImGui::CollapsingHeader("Performance capture (advanced)")) {
      bool enabled=TimingEnabled();
      ui::Setting("##xe_cpu_capture","Collect CPU timings","Off by default. Records a bounded window of source Present timing, SDK-call duration and XeLL sleep. Adds profiling overhead; does not measure displayed frames or GPU execution.",[&] {
        if(ImGui::Checkbox("##xe_cpu_enabled",&enabled)) {
          SetTimingEnabled(enabled);
          WritePrivateProfileStringW(L"Diagnostics",L"CpuTiming",enabled?L"1":L"0",Ini().c_str());
        }
      });
      if(ImGui::Button("Copy timing summary")) ImGui::SetClipboardText(TimingReport().c_str());
      ui::Tooltip("Includes CPU p95/p99/p99.9 and reports unavailable display/GPU/physical measurements explicitly. Use PresentMon for displayed intervals.");
      if(ImGui::Button("Clear timing window")) ClearTiming();
    }
  }
  if(status.latencyRestartPending) ui::Notice("Latency output saved. Restart the game to apply it.");
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

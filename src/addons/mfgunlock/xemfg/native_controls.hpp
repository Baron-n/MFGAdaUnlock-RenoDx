// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include "native_window.hpp"
namespace mfgunlock::xemfg::native {
enum class LatencyInput : uint32_t { Auto, StreamlineReflex };
enum class LatencyOutput : uint32_t { Auto, XeLL };
struct ControlSettings {
  uint32_t version{1};
  LatencyInput input{LatencyInput::Auto};
  LatencyOutput output{LatencyOutput::Auto};
  bool flipMetering{true}, forceBorderless{};
};
struct ControlStatus {
  uint32_t version{1};
  ControlSettings settings;
  WindowStatus window;
  bool reflexObserved{}, xellObserved{}, nativeReflexDisabled{};
  uint32_t gameReflexMode{};
  uint64_t sleeps{}, markers{};
  bool modelPort{}, modelInitialized{}, modelUpdating{}, cudaUpdating{}, settingsSaved{true};
  uint32_t cudaState{}, cudaLayers{};
  int cudaMajor{}, cudaMinor{};
  uint64_t cudaLaunches{}, cudaFallbacks{};
  bool flipAvailable{}, flipInstalled{}, flipRegistered{}, flipQueue{}, flipBusy{};
  bool flipRequested{}, flipEnabled{}, flipConfigured{}, flipMetered{}, flipRefused{};
  uint32_t flipOutputs{}, flipFeedback{};
  int flipResult{};
  uint64_t flipCalls{};
  char cudaReason[128]{};
};
void LoadControls();
ControlSettings ControlsSettings();
bool ConfigureControls(const ControlSettings& settings);
ControlStatus ControlsSnapshot();
void DrawControls();
}

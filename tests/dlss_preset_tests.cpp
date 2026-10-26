// SPDX-License-Identifier: MIT
// 1.6 (plan B): the DLSS Super Resolution preset override.
#include <cstdlib>
#include <iostream>

#include "../src/addons/mfgunlock/dlss_preset.hpp"

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition     \
                << '\n';                                                        \
      std::abort();                                                              \
    }                                                                           \
  } while (false)

using namespace mfgunlock::dlsspreset;
using sl::DLSSPreset;

namespace {
constexpr uint32_t K = static_cast<uint32_t>(DLSSPreset::ePresetK);

bool AllPresets(const sl::DLSSOptions& o, DLSSPreset p) {
  return o.dlaaPreset == p && o.qualityPreset == p && o.balancedPreset == p &&
         o.performancePreset == p && o.ultraPerformancePreset == p && o.ultraQualityPreset == p;
}
}  // namespace

int main() {
  // Only the current presets (J, K, L, M) and "game default" are selectable.
  CHECK(IsSelectable(0));
  for (auto p : {DLSSPreset::ePresetJ, DLSSPreset::ePresetK, DLSSPreset::ePresetL, DLSSPreset::ePresetM})
    CHECK(IsSelectable(static_cast<uint32_t>(p)));
  for (auto p : {DLSSPreset::ePresetE, DLSSPreset::ePresetF, DLSSPreset::ePresetI, DLSSPreset::ePresetN})
    CHECK(!IsSelectable(static_cast<uint32_t>(p)));

  // The presets are in version 1; later versions only append fields.
  CHECK(KnownSize(sl::kStructVersion1) > offsetof(sl::DLSSOptions, ultraQualityPreset));
  CHECK(KnownSize(sl::kStructVersion1) < KnownSize(sl::kStructVersion2));
  CHECK(KnownSize(sl::kStructVersion2) < KnownSize(sl::kStructVersion3));
  CHECK(KnownSize(sl::kStructVersion3) == sizeof(sl::DLSSOptions));
  CHECK(KnownSize(4) == 0);

  sl::DLSSOptions game;
  game.mode = sl::DLSSMode::eMaxPerformance;
  game.outputWidth = 3840;
  game.outputHeight = 2160;
  game.performancePreset = DLSSPreset::ePresetM;
  game.qualityPreset = DLSSPreset::ePresetJ;
  game.alphaUpscalingEnabled = sl::Boolean::eTrue;
  CHECK(GamePreset(game) == DLSSPreset::ePresetM);
  game.mode = sl::DLSSMode::eDLAA;
  CHECK(GamePreset(game) == DLSSPreset::eDefault);
  game.mode = sl::DLSSMode::eMaxQuality;
  CHECK(GamePreset(game) == DLSSPreset::ePresetJ);

  // Every mode's preset changes; mode, size and the other fields are kept,
  // and the game's structure is not written.
  sl::DLSSOptions out;
  CHECK(Override(game, K, out));
  CHECK(AllPresets(out, DLSSPreset::ePresetK));
  CHECK(out.mode == game.mode && out.outputWidth == 3840 && out.outputHeight == 2160);
  CHECK(out.alphaUpscalingEnabled == sl::Boolean::eTrue && out.structVersion == sl::kStructVersion3);
  CHECK(game.qualityPreset == DLSSPreset::ePresetJ);

  // A version-1 caller: only its bytes are copied; the rest keeps defaults.
  sl::DLSSOptions v1 = game;
  v1.structVersion = sl::kStructVersion1;
  sl::DLSSOptions out1;
  CHECK(Override(v1, K, out1));
  CHECK(out1.structVersion == sl::kStructVersion1 && AllPresets(out1, DLSSPreset::ePresetK));
  CHECK(out1.alphaUpscalingEnabled == sl::Boolean::eFalse);

  // Nothing selected, DLSS off, an invalid preset or an unknown version: no override.
  CHECK(!Override(game, 0, out));
  CHECK(!Override(game, static_cast<uint32_t>(DLSSPreset::ePresetF), out));
  sl::DLSSOptions off = game;
  off.mode = sl::DLSSMode::eOff;
  CHECK(!Override(off, K, out));
  sl::DLSSOptions newer = game;
  newer.structVersion = 4;
  CHECK(!Override(newer, K, out));

  std::cout << "dlss_preset_tests passed\n";
  return 0;
}

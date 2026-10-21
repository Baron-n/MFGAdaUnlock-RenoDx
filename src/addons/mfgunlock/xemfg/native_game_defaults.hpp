// SPDX-License-Identifier: MIT
#pragma once
#include <string_view>
namespace mfgunlock::xemfg::native {
// Defaults backed by real Streamline constant receipts. Runtime validation
// and explicit INI overrides retain priority over these startup defaults.
inline bool DefaultInvertedDepth(std::wstring_view executable) noexcept {
  const auto file=executable.substr(executable.find_last_of(L"/\\")+1);
  return file!=L"tlou-ii.exe" && file!=L"tlou-ii-l.exe" && file!=L"AlanWake2.exe";
}
inline bool DefaultJitteredMotion(std::wstring_view executable) noexcept {
  const auto file=executable.substr(executable.find_last_of(L"/\\")+1);
  // RC3 runtime receipt: valid Constants V2, inverted depth, jittered MV.
  // This is an initialization convention, not permission to ignore a mismatch.
  return file==L"DS2.exe" || file==L"ds2.exe";
}
}

// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <atomic>
#include <memory>
#include <mutex>
#include "overlay_shortcut.hpp"

namespace mfgunlock::xemfg::native {
struct WindowStatus {
  bool available{}, requested{}, pending{}, borderless{}, coversMonitor{}, managed{};
  uint32_t error{};
};
struct OverlayReceipt {
  OverlayTapResult taps;
  uint64_t childMessages{};
  uint64_t hotkeyChanges{};
  uint32_t hotkeyError{};
  bool hotkeyRegistered{};
  uint32_t blockers{}; // Escape=1, active widget=2, text=4, unfocused=8, retiring=16.
};
// Window changes run on the HWND's owning thread, through its message pump.
// Neither the overlay nor Intel's presentation worker calls SetWindowPos.
class WindowControl : public std::enable_shared_from_this<WindowControl> {
public:
  static std::shared_ptr<WindowControl> Bind(HWND hwnd, bool force);
  static LRESULT CALLBACK MessageHook(int code, WPARAM w, LPARAM l);
  static LRESULT CALLBACK ActivationHook(int code, WPARAM w, LPARAM l);
  void Request(bool force);
  void Retire();
  WindowStatus Snapshot();
  void OverlayKey(OverlayChord chord);
  void OverlayNative();
  void OverlayTextCapture(bool capture);
  bool OverlayOwnsHotkey(OverlayChord chord);
  uint32_t OverlayTaps(uint32_t blockers,uint64_t& dropped,OverlayReceipt* receipt=nullptr);
private:
  HWND hwnd{};
  std::atomic<HHOOK> hook{};
  HHOOK activationHook{};
  std::atomic<bool> rawKeyboard{};
  DWORD thread{};
  uintptr_t generation{};
  std::mutex lock;
  bool requested{}, pending{}, managed{}, retiring{}, applying{};
  DWORD error{};
  LONG_PTR originalStyle{}, originalEx{}, appliedStyle{}, appliedEx{};
  WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
  RECT appliedRect{};
  OverlayShortcut shortcut;
  uint64_t childMessages{};
  OverlayChord hotkeyWanted{0,0,true},hotkeyApplied{0,0,true};
  ATOM hotkeyId{};
  bool hotkeyRegistered{},textCapture{};
  uint64_t hotkeyChanges{};
  DWORD hotkeyError{},hotkeySince{};
  void Apply();
  void ApplyHotkey(bool deactivate=false);
  void Cleanup();
};
}

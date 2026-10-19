// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <atomic>
#include <memory>
#include <mutex>

namespace mfgunlock::xemfg::native {
struct WindowStatus {
  bool available{}, requested{}, pending{}, borderless{}, coversMonitor{}, managed{};
  uint32_t error{};
};
// Window changes run on the HWND's owning thread, through its message pump.
// Neither the overlay nor Intel's presentation worker calls SetWindowPos.
class WindowControl : public std::enable_shared_from_this<WindowControl> {
public:
  static std::shared_ptr<WindowControl> Bind(HWND hwnd, bool force);
  static LRESULT CALLBACK MessageHook(int code, WPARAM w, LPARAM l);
  void Request(bool force);
  void Retire();
  WindowStatus Snapshot();
private:
  HWND hwnd{};
  std::atomic<HHOOK> hook{};
  DWORD thread{};
  uintptr_t generation{};
  std::mutex lock;
  bool requested{}, pending{}, managed{}, retiring{}, applying{};
  DWORD error{};
  LONG_PTR originalStyle{}, originalEx{}, appliedStyle{}, appliedEx{};
  WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
  RECT appliedRect{};
  void Apply();
};
}

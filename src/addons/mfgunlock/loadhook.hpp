/*
 * Load-time trigger for the DLSS-G snippet patches.
 * SPDX-License-Identifier: MIT
 *
 * ---------------------------------------------------------------------------
 * WHY
 *
 * NGX populates the snippet's capabilities within milliseconds of loading it --
 * PopulateParameters, the function carrying the arch gate, runs almost
 * immediately. Patching on the next present is therefore a race we only win by
 * accident:
 *
 *   Cyberpunk 2077 : nvngx_dlssg.dll already loaded when the addon initialised,
 *                    so DllMain patched it ~10ms in, long before the query.
 *   Deep Rock Gal. : the DLL loads seconds AFTER the addon, NGX populates
 *                    immediately, and our next-frame patch lands too late. The
 *                    gates get rewritten correctly, but the capability was
 *                    already computed and cached as 1.
 *
 * So catch the module as it is mapped. A single bootstrap scan covers providers
 * that predate the addon; this trigger covers later providers without polling
 * or enumerating modules from the presentation thread.
 *
 * LOADER LOCK: this callback runs inside LoadLibrary, holding the loader lock.
 * It must never call anything that takes that lock again --
 * CreateToolhelp32Snapshot(TH32CS_SNAPMODULE) in particular would deadlock,
 * which is why the callback receives the HMODULE and patches it directly
 * instead of scanning for it.
 * ---------------------------------------------------------------------------
 */

#pragma once

#include <windows.h>

#include <atomic>
#include <array>
#include <cstring>
#include <cwctype>
#include <vector>

#include <include/reshade.hpp>

#include "./ngx_hook.hpp"

namespace mfgunlock::loadhook {

// Called with the freshly loaded DLSS-G snippet. Set by the addon.
inline void (*g_on_dlssg_loaded)(HMODULE) = nullptr;
// Called with the freshly loaded Streamline DLSS-G wrapper. This is separate
// from the NGX snippet above: native multiplier menus query the wrapper's
// compiled ceiling, so it must be handled even when legacy pacing is disabled.
inline void (*g_on_dlssg_plugin_loaded)(HMODULE) = nullptr;
// slInit must be hooked before the game calls it, and it calls it early --
// so catch the interposer as it is mapped rather than hoping to beat it.
inline void (*g_on_interposer_loaded)() = nullptr;
inline std::atomic_bool g_hooked{false};
inline std::atomic_bool g_installing{false};
inline std::atomic<unsigned int> g_catches{0};
// 1.5.7: the loader notification sees every mapped module, also one loaded
// through ntdll!LdrLoadDll directly (past the kernel32 hooks), and before its
// DllMain (plan 5.3, loader_notification_probe). Runs UNDER THE LOADER LOCK:
// the callback must not enumerate modules or take ReShade's log lock.
inline void (*g_on_dlssg_mapped)(HMODULE) = nullptr;
inline std::atomic_bool g_notification_registered{false};
inline std::atomic<unsigned int> g_notification_catches{0};

namespace internal {

constexpr wchar_t kNeedle[] = L"nvngx_dlssg";
constexpr wchar_t kOtaNeedle[] = L"\\models\\dlssg\\";
constexpr wchar_t kInterposerNeedle[] = L"sl.interposer";
constexpr wchar_t kDlssgPluginNeedle[] = L"sl.dlss_g";
constexpr wchar_t kDlssgPluginOtaNeedle[] = L"\\models\\sl_dlss_g_0\\";

inline bool NameContains(const wchar_t* path, const wchar_t* needle) {
  if (path == nullptr) return false;
  // Compare case-insensitively without touching the CRT locale machinery.
  for (const wchar_t* p = path; *p != L'\0'; ++p) {
    size_t i = 0;
    while (needle[i] != L'\0') {
      const wchar_t a = p[i];
      if (a == L'\0') break;
      const wchar_t lower = (a >= L'A' && a <= L'Z') ? static_cast<wchar_t>(a - L'A' + L'a') : a;
      if (lower != needle[i]) break;
      ++i;
    }
    if (needle[i] == L'\0') return true;
  }
  return false;
}

inline void Notify(HMODULE module, const wchar_t* path) {
  if (module == nullptr || path == nullptr) return;
  // Driver OTA snippets are commonly mapped from ...\models\dlssg\... under
  // opaque numeric .bin names. The loader call may receive only that basename,
  // so also inspect the resolved module path after the mapping completes.
  std::array<wchar_t, 512> short_path{};
  std::vector<wchar_t> long_path;
  wchar_t* resolved = short_path.data();
  DWORD length = GetModuleFileNameW(module, resolved, static_cast<DWORD>(short_path.size()));
  if (length >= short_path.size()) {
    long_path.resize(32768);
    resolved = long_path.data();
    length = GetModuleFileNameW(module, resolved, static_cast<DWORD>(long_path.size()));
    if (length >= long_path.size()) resolved[0] = L'\0';
  }
  const bool is_dlssg =
      NameContains(path, kNeedle) || NameContains(path, kOtaNeedle) ||
      NameContains(resolved, kNeedle) ||
      NameContains(resolved, kOtaNeedle);
  const bool is_dlssg_plugin =
      NameContains(path, kDlssgPluginNeedle) ||
      NameContains(path, kDlssgPluginOtaNeedle) ||
      NameContains(resolved, kDlssgPluginNeedle) ||
      NameContains(resolved, kDlssgPluginOtaNeedle);
  const bool is_interposer = NameContains(path, kInterposerNeedle) || NameContains(resolved, kInterposerNeedle);
  if (g_on_dlssg_plugin_loaded != nullptr && is_dlssg_plugin) {
    g_on_dlssg_plugin_loaded(module);
  }
  if (g_on_dlssg_loaded != nullptr && is_dlssg) {
    g_catches.fetch_add(1, std::memory_order_relaxed);
    g_on_dlssg_loaded(module);
  }
  if (g_on_interposer_loaded != nullptr && is_interposer) {
    g_on_interposer_loaded();
  }
}

struct UnicodeString { USHORT Length, MaximumLength; PWSTR Buffer; };
struct NotificationData { ULONG Flags; const UnicodeString* FullDllName; const UnicodeString* BaseDllName; PVOID DllBase; ULONG SizeOfImage; };
using NotificationCallback = VOID(CALLBACK*)(ULONG, const NotificationData*, PVOID);
using RegisterNotification = LONG(NTAPI*)(ULONG, NotificationCallback, PVOID, PVOID*);
using UnregisterNotification = LONG(NTAPI*)(PVOID);
inline PVOID g_notification_cookie = nullptr;

inline VOID CALLBACK LoaderNotification(ULONG reason, const NotificationData* data, PVOID) {
  constexpr ULONG kLoaded = 1;
  if (reason != kLoaded || data == nullptr || data->DllBase == nullptr || data->FullDllName == nullptr ||
      data->FullDllName->Buffer == nullptr || g_on_dlssg_mapped == nullptr) return;
  // The name is not terminated; copy it to the stack (no heap in this path).
  std::array<wchar_t, 1024> path{};
  const size_t length = (std::min)(size_t(data->FullDllName->Length / sizeof(wchar_t)), path.size() - 1);
  std::memcpy(path.data(), data->FullDllName->Buffer, length * sizeof(wchar_t));
  if (!NameContains(path.data(), kNeedle) && !NameContains(path.data(), kOtaNeedle)) return;
  g_notification_catches.fetch_add(1, std::memory_order_relaxed);
  g_on_dlssg_mapped(static_cast<HMODULE>(data->DllBase));
}

using LoadLibraryExWFn = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using LoadLibraryWFn = HMODULE(WINAPI*)(LPCWSTR);

inline LoadLibraryExWFn g_real_load_library_ex_w = nullptr;
inline LoadLibraryWFn g_real_load_library_w = nullptr;

inline HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR file_name, HANDLE file, DWORD flags) {
  HMODULE module = g_real_load_library_ex_w(file_name, file, flags);
  // Data-file mappings are not executable images; patching one would be
  // meaningless and the caller is not going to run code from it.
  constexpr DWORD kDataOnly = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                              LOAD_LIBRARY_AS_IMAGE_RESOURCE;
  if ((flags & kDataOnly) == 0) Notify(module, file_name);
  return module;
}

inline HMODULE WINAPI HookedLoadLibraryW(LPCWSTR file_name) {
  HMODULE module = g_real_load_library_w(file_name);
  Notify(module, file_name);
  return module;
}

inline const std::vector<hook::HookItem> kHooks = {
    {"LoadLibraryExW", reinterpret_cast<void**>(&g_real_load_library_ex_w),
     reinterpret_cast<void*>(&HookedLoadLibraryExW)},
    {"LoadLibraryW", reinterpret_cast<void**>(&g_real_load_library_w),
     reinterpret_cast<void*>(&HookedLoadLibraryW)},
};

}  // namespace internal

inline void TryRegisterNotification() {
  if (g_on_dlssg_mapped == nullptr || g_notification_registered.exchange(true, std::memory_order_acq_rel)) return;
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto reg = ntdll == nullptr ? nullptr : reinterpret_cast<internal::RegisterNotification>(
      reinterpret_cast<void*>(GetProcAddress(ntdll, "LdrRegisterDllNotification")));
  if (reg == nullptr || reg(0, &internal::LoaderNotification, nullptr, &internal::g_notification_cookie) < 0) {
    internal::g_notification_cookie = nullptr;
    g_notification_registered.store(false, std::memory_order_release);
  }
}

inline void TryInstall() {
  TryRegisterNotification();
  if (g_hooked.load(std::memory_order_acquire)) return;
  if (g_on_dlssg_loaded == nullptr) return;
  bool expected = false;
  if (!g_installing.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel)) {
    return;
  }
  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  if (kernel32 == nullptr) {
    g_installing.store(false, std::memory_order_release);
    return;
  }
  if (!hook::Install(kernel32, internal::kHooks, "kernel32.dll")) {
    g_installing.store(false, std::memory_order_release);
    return;
  }
  g_hooked.store(true, std::memory_order_release);
  g_installing.store(false, std::memory_order_release);
}

inline void Uninstall() {
  // Unregistering takes the loader lock, so no callback is running into this
  // module once it returns; explicit addon unload must not leave it behind.
  if (g_notification_registered.load(std::memory_order_acquire) && internal::g_notification_cookie != nullptr) {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto unreg = ntdll == nullptr ? nullptr : reinterpret_cast<internal::UnregisterNotification>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "LdrUnregisterDllNotification")));
    if (unreg != nullptr && unreg(internal::g_notification_cookie) >= 0) {
      internal::g_notification_cookie = nullptr;
      g_notification_registered.store(false, std::memory_order_release);
    }
  }
  if (!g_hooked.load(std::memory_order_acquire)) return;
  if (!hook::Uninstall(internal::kHooks)) return;
  g_hooked.store(false, std::memory_order_release);
}

}  // namespace mfgunlock::loadhook

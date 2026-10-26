// SPDX-License-Identifier: MIT
// Plan 5.4: the DLSS-G provider is seen by the loader notification of
// loadhook.hpp even when it is loaded through ntdll!LdrLoadDll directly, past
// the kernel32 LoadLibrary hooks; Uninstall unregisters it.
#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "../src/addons/mfgunlock/loadhook.hpp"

extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int, const char*) {}

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition     \
                << '\n';                                                        \
      std::exit(1);                                                             \
    }                                                                           \
  } while (false)

namespace {
using LdrLoadDll = LONG(NTAPI*)(PWSTR, PULONG, const mfgunlock::loadhook::internal::UnicodeString*, PVOID*);
HMODULE mapped = nullptr;
int calls = 0;

HMODULE LoadDirect(const std::wstring& path) {
  const auto load = reinterpret_cast<LdrLoadDll>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrLoadDll"));
  CHECK(load != nullptr);
  mfgunlock::loadhook::internal::UnicodeString name{USHORT(path.size() * sizeof(wchar_t)),
      USHORT(path.size() * sizeof(wchar_t)), const_cast<PWSTR>(path.c_str())};
  PVOID handle = nullptr;
  CHECK(load(nullptr, nullptr, &name, &handle) >= 0 && handle != nullptr);
  return static_cast<HMODULE>(handle);
}
}  // namespace

int main(int argc, char** argv) {
  CHECK(argc == 2);
  namespace l = mfgunlock::loadhook;
  // Two copies with provider names, in a folder of their own.
  const auto folder = std::filesystem::temp_directory_path() /
      (L"mfg-loader-notification-" + std::to_wstring(GetCurrentProcessId()));
  std::filesystem::create_directories(folder);
  const auto first = folder / L"nvngx_dlssg.dll";
  const auto models = folder / L"models" / L"dlssg";
  std::filesystem::create_directories(models);
  const auto second = models / L"opaque_provider.dll";
  std::filesystem::copy_file(argv[1], first, std::filesystem::copy_options::overwrite_existing);
  std::filesystem::copy_file(argv[1], second, std::filesystem::copy_options::overwrite_existing);

  l::g_on_dlssg_mapped = [](HMODULE module) { mapped = module; ++calls; };
  l::TryRegisterNotification();
  CHECK(l::g_notification_registered.load());

  // Unrelated modules never reach the callback.
  const HMODULE unrelated = LoadLibraryW(L"version.dll");
  CHECK(unrelated != nullptr && calls == 0);

  const HMODULE provider = LoadDirect(first.wstring());
  CHECK(calls == 1 && mapped == provider);
  CHECK(l::g_notification_catches.load() == 1);

  // Uninstall unregisters: a later provider is no longer reported.
  l::Uninstall();
  CHECK(!l::g_notification_registered.load());
  const HMODULE later = LoadDirect(second.wstring());
  CHECK(later != nullptr && calls == 1);

  FreeLibrary(provider);
  FreeLibrary(later);
  std::error_code ignored;
  std::filesystem::remove_all(folder, ignored);
  std::cout << "PASS: provider loaded through LdrLoadDll reported by the loader notification; unregistered by Uninstall\n";
  return 0;
}

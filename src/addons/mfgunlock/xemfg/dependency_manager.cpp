// SPDX-License-Identifier: MIT
#include "dependency_manager.hpp"
#include "../backend_selection.hpp"
#include "../panel_ui.hpp"
#include "vendor/sha256_util.h"
#include <nlohmann/json.hpp>
#include <shellapi.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <fstream>

namespace mfgunlock::dependencies {
namespace {
std::mutex mutex;
Status status;
std::atomic<bool> busy{};
std::atomic<bool> initial{};
HMODULE self{};
std::string Utf8(const std::wstring& w) {
  if(w.empty()) return {};
  const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,w.data(),int(w.size()),nullptr,0,nullptr,nullptr);
  std::string result(n,0); if(n) WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),result.data(),n,nullptr,nullptr);return result;
}
std::string Resource(unsigned id) {
  const auto resource=FindResourceW(self,MAKEINTRESOURCEW(id),RT_RCDATA);
  if(!resource) throw std::runtime_error("Repair resources are unavailable. Use the local candidate package's helper.");
  const auto size=SizeofResource(self,resource); const auto bytes=LockResource(LoadResource(self,resource));
  if(!bytes || !size) throw std::runtime_error("Repair resource could not be read.");
  return std::string(static_cast<const char*>(bytes),size);
}
std::string Hash(const std::filesystem::path& path) {
  std::ifstream input(path,std::ios::binary); if(!input) throw std::runtime_error("Cannot read the dependency file.");
  sha256_util::Hasher hash; std::array<char,65536> buffer{};
  while(input) { input.read(buffer.data(),buffer.size()); if(input.gcount() && !hash.Update(buffer.data(),size_t(input.gcount()))) throw std::runtime_error("Hash failed."); }
  sha256_util::Digest digest{}; if(input.bad() || !hash.Finish(digest)) throw std::runtime_error("Hash failed.");
  return sha256_util::LowerHex(digest);
}
std::wstring ModulePath(HMODULE module) {
  std::wstring path(32768,0); const auto n=GetModuleFileNameW(module,path.data(),DWORD(path.size()));
  if(!n || n>=path.size()) throw std::runtime_error("Cannot identify a mapped module."); path.resize(n); return path;
}
std::string Version(const std::filesystem::path& path) {
  DWORD unused{}; const auto size=GetFileVersionInfoSizeW(path.c_str(),&unused); if(!size) return "unavailable";
  std::vector<char> data(size); VS_FIXEDFILEINFO* info{}; UINT length{};
  if(!GetFileVersionInfoW(path.c_str(),0,size,data.data()) || !VerQueryValueW(data.data(),L"\\",reinterpret_cast<void**>(&info),&length) || length<sizeof(*info)) return "unavailable";
  return std::to_string(HIWORD(info->dwFileVersionMS))+'.'+std::to_string(LOWORD(info->dwFileVersionMS))+'.'+std::to_string(HIWORD(info->dwFileVersionLS))+'.'+std::to_string(LOWORD(info->dwFileVersionLS));
}
std::wstring Quote(const std::wstring& s) {
  if(s.find(L'"')!=s.npos || s.find(L'\n')!=s.npos) throw std::runtime_error("Invalid helper argument.");
  return L"\""+s+L"\"";
}
void Run(bool repair) noexcept {
  try {
    Status next; next.checked=true;
    const auto catalog=nlohmann::json::parse(Resource(9010));
    const auto root=backend::Directory(); if(root.empty()) throw std::runtime_error("Game directory is unavailable.");
    for(const auto& entry:catalog.at("files")) {
      if(!entry.at("required").get<bool>()) continue;
      const auto name=entry.at("name").get<std::string>(); const std::wstring wide(name.begin(),name.end());
      std::filesystem::path mapped;
      HMODULE module{};
      if(GetModuleHandleExW(0,wide.c_str(),&module)) {
        try { mapped=ModulePath(module); } catch(...) { FreeLibrary(module); throw; }
        FreeLibrary(module);
      }
      std::vector<std::filesystem::path> paths{root/wide};
      if(std::filesystem::exists(root/L"_storage_"/wide)) paths.push_back(root/L"_storage_"/wide);
      bool listed=false; for(const auto& p:paths) if(!mapped.empty() && _wcsicmp(p.c_str(),mapped.c_str())==0) listed=true;
      if(!mapped.empty() && !listed) paths.push_back(mapped);
      for(const auto& path:paths) {
        File file; file.name=name; file.path=Utf8(path.wstring()); file.required=true;
        file.mapped=!mapped.empty() && _wcsicmp(path.c_str(),mapped.c_str())==0;
        if(std::filesystem::exists(path)) {
          try {
            file.sha256=Hash(path); file.version=Version(path); file.health=Health::Unknown;
            if(file.sha256==entry.at("sha256").get<std::string>()) file.health=Health::Valid;
            else if(entry.contains("accepted_older")) for(const auto& hash:entry.at("accepted_older"))
              if(file.sha256==hash.get<std::string>()) file.health=Health::UpdateOptional;
          } catch(...) { file.health=Health::Error; }
        }
        next.files.push_back(std::move(file));
      }
    }
    next.message="Exact hashes checked. XeSS upscaling (libxess.dll) is not required by this bridge.";
    if(repair) {
      const auto helper=Resource(9011);
      wchar_t temp[MAX_PATH]{}; if(!GetTempPathW(MAX_PATH,temp)) throw std::runtime_error("Cannot locate helper staging directory.");
      const auto folder=std::filesystem::path(temp)/(L"MFG-XeHelper-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
      if(!std::filesystem::create_directory(folder)) throw std::runtime_error("Cannot create private helper staging directory.");
      const auto write=[&](const wchar_t* name,const std::string& bytes) {
        std::ofstream output(folder/name,std::ios::binary); output.write(bytes.data(),bytes.size()); output.close();
        std::ifstream verify(folder/name,std::ios::binary); const std::string actual((std::istreambuf_iterator<char>(verify)),{});
        if(!verify || actual!=bytes) throw std::runtime_error("Helper staging failed verification.");
      };
      write(L"catalog.json",Resource(9010)); write(L"Repair-XeDependencies.ps1",helper);
      FILETIME created{},exited{},kernel{},user{};
      if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)) throw std::runtime_error("Cannot identify the game process.");
      ULARGE_INTEGER ticks{}; ticks.LowPart=created.dwLowDateTime; ticks.HighPart=created.dwHighDateTime;
      wchar_t system[MAX_PATH]{}; if(!GetSystemDirectoryW(system,MAX_PATH)) throw std::runtime_error("Cannot locate PowerShell.");
      const auto exe=std::filesystem::path(system)/L"WindowsPowerShell/v1.0/powershell.exe";
      const auto arguments=L"-NoProfile -ExecutionPolicy Bypass -File "+Quote((folder/L"Repair-XeDependencies.ps1").wstring())
          +L" -GameDirectory "+Quote(root.wstring())+L" -GameProcessId "+std::to_wstring(GetCurrentProcessId())
          +L" -GameCreationTicks "+std::to_wstring(ticks.QuadPart)+L" -GameExecutable "+Quote(ModulePath(nullptr))
          +L" -CatalogPath "+Quote((folder/L"catalog.json").wstring());
      SHELLEXECUTEINFOW launch{sizeof(launch)}; launch.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_FLAG_NO_UI;
      launch.lpFile=exe.c_str(); launch.lpParameters=arguments.c_str(); launch.nShow=SW_SHOWNORMAL;
      if(!ShellExecuteExW(&launch) || !launch.hProcess) throw std::runtime_error("Could not start the external repair helper. Run the packaged helper manually.");
      next.helperRunning=true; next.busy=false; next.message="External helper opened. Review its file list; changes apply only after confirmation and game exit.";
      { std::lock_guard guard(mutex); status=next; }
      WaitForSingleObject(launch.hProcess,INFINITE); DWORD code{}; GetExitCodeProcess(launch.hProcess,&code); CloseHandle(launch.hProcess);
      next.helperRunning=false; next.message=code?"Repair stopped. Review the helper's error; no restart success is assumed.":"Helper finished. Restart after a repair, then check dependencies again.";
    }
    { std::lock_guard guard(mutex); status=std::move(next); }
  } catch(const std::exception& e) { std::lock_guard guard(mutex); status.checked=true; status.busy=false; status.message=e.what(); }
  busy=false;
}
void Queue(bool repair) {
  if(busy.exchange(true)) return;
  // This process already pins the presentation hooks; the optional worker must
  // also never return into an unloaded addon. No file work runs in Draw().
  if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&Queue),&self)) { busy=false; return; }
  { std::lock_guard guard(mutex); status.busy=true; }
  try { std::thread(Run,repair).detach(); } catch(...) { busy=false; std::lock_guard guard(mutex); status.busy=false;status.message="Could not start the dependency worker."; }
}
}
void Inspect() { initial=true; Queue(false); }
void Repair() { initial=true; Queue(true); }
Status Snapshot() { std::lock_guard guard(mutex); return status; }
void Draw() {
#if defined(MFGUNLOCK_UI_PREVIEW)
  const Status value{true,false,false,"Synthetic dependency status; no files were checked.",
      {{"libxess_fg.dll","game/libxess_fg.dll","1.3.1.78","fixture",Health::Valid,true,true},
       {"libxell.dll","game/libxell.dll","1.3.2.10","fixture",Health::Valid,true,true}}};
#else
  if(!initial.exchange(true)) Queue(false);
  const auto value=Snapshot();
#endif
  ui::Section("Intel dependencies","Check the files this game uses. Repairs are reviewed in a separate helper.");
  ui::Card card("##intel_dependencies");
  if(value.busy) ui::Description("Checking files in the background...");
  for(size_t i=0;i<value.files.size();++i) {
    const auto& f=value.files[i]; const char* health=f.health==Health::Valid?"Compatible":f.health==Health::UpdateOptional?"Compatible - optional update":
        f.health==Health::Missing?"Missing":f.health==Health::Unknown?"Unknown / incompatible":"Cannot read file";
    const auto label=f.name+(f.mapped?" (loaded)":" (on disk)");
    const auto detail=f.path+"\nVersion: "+f.version+"\nSHA-256: "+f.sha256+"\nFile presence does not prove selection. Loaded identifies a mapped module, not a live feature.";
    ui::Readout(("##dep_"+std::to_string(i)).c_str(),label.c_str(),health,detail.c_str(),f.health==Health::Valid || f.health==Health::UpdateOptional?ui::Accent:ui::Warning);
  }
  if(!value.message.empty()) ui::Description(value.message.c_str());
  ImGui::BeginDisabled(busy.load());
  if(ImGui::Button("Check Intel files")) Inspect(); ui::Tooltip("Reads exact file hashes off the rendering thread. Does not load or replace DLLs.");
  if(ImGui::Button("Review install / repair")) Repair(); ui::Tooltip("Opens an external helper. Original Intel files come from a pinned official commit. Review and confirm before any game file changes. The helper waits for game exit, creates backups and provides rollback.");
  ImGui::EndDisabled();
}
}

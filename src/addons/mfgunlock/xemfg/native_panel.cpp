// SPDX-License-Identifier: MIT
#include "native_internal.hpp"
#include "backend_api.hpp"
#include "../panel_ui.hpp"
#include <sstream>
namespace mfgunlock::xemfg::native {
static std::atomic<uint64_t> panelDraws{};
void Draw() {
  ++panelDraws;
  auto& s=State(); const auto status=Snapshot();
  const bool unavailable=ModelStartupFailed();
  const bool generating=status.enabled && status.sdkResult>=0 && status.submitted>1;
  const bool paused=status.output && s.nativeOff && !s.requested;
  const auto multiplier=std::to_string(s.multiplier.load())+"x";
  ui::Hero(multiplier.c_str(),"XeMFG","DX12",generating?"Generating frames":
      unavailable?"XeMFG unavailable":paused?"Game has paused frame generation":status.output?"Waiting for compatible frame inputs":"Waiting for the game",generating);
  if(unavailable) {
    mfgunlock::xemfg::Status model;
    TryModelStatus(model);
    if(strstr(model.detail,"missing_Intel_runtime") || strstr(model.detail,"hash_file_open"))
      ui::Notice("Intel frame generation files are missing. Install the supported XeFG and XeLL runtime pair beside the game executable, then restart.");
    else if(strstr(model.detail,"unreviewed_Intel_runtime_pair"))
      ui::Notice("This game's Intel runtime pair is unsupported. Update XeFG and XeLL together, including the mod loader's active copies, then restart.");
    else if(strstr(model.detail,"remove_OptiScaler") || strstr(model.detail,"other_mfg_plugin") || strstr(model.detail,"remove_companion"))
      ui::Notice("Another frame-generation loader is active. Use this addon with ReShade directly, then restart the game.");
    else ui::Notice("XeMFG could not initialize. Open Diagnostics and copy the report to see the reason.");
  }
  else if(!status.output) ui::Notice("Load a scene with Frame Generation enabled. XeMFG currently uses Streamline inputs in DirectX 12.");
  else if(paused) ui::Notice("The game's latest request has frame generation disabled. If it is enabled in the game, copy Diagnostics so the connection can be checked.");
  else if(status.enabled && status.sdkResult<0) ui::Notice("Frame generation is recovering. If this persists, switch FG off and on in the game.");
  static PanelPage page=PanelPage::General;
  if(ImGui::GetContentRegionAvail().x<ImGui::GetFontSize()*26.f) {
    int selected=int(page); ImGui::SetNextItemWidth(-1);
    if(ImGui::Combo("##xe_page",&selected,"General\0Quality\0Latency\0Diagnostics\0")) page=PanelPage(selected);
  } else if(ImGui::BeginTabBar("##xemfg_pages")) {
    if(ImGui::BeginTabItem("General")) { page=PanelPage::General; ImGui::EndTabItem(); }
    if(ImGui::BeginTabItem("Quality")) { page=PanelPage::Quality; ImGui::EndTabItem(); }
    if(ImGui::BeginTabItem("Latency")) { page=PanelPage::Latency; ImGui::EndTabItem(); }
    if(ImGui::BeginTabItem("Diagnostics")) { page=PanelPage::Diagnostics; ImGui::EndTabItem(); }
    ImGui::EndTabBar();
  }
  if(page==PanelPage::General) {
    ui::Section("Frame generation","Keep Frame Generation enabled in the game.");
    ui::Card card("##xemfg_generation");
    ui::BackendSelector();
    int choice=int(s.multiplier.load())-2;
    ui::Setting("##xe_multiplier_row","Frame multiplier","Applies while playing. 2x adds up to one generated frame per rendered frame; 4x adds up to three. Higher values use more GPU time.",[&] {
      if(ImGui::Combo("##xe_multiplier",&choice,"2x\0" "3x\0" "4x\0" "5x\0" "6x\0")) {
        s.multiplier=uint32_t(choice+2);
        const auto ini=backend::Directory()/L"MFGAdaUnlock.ini";
        WritePrivateProfileStringW(L"XeFG",L"Multiplier",std::to_wstring(choice+2).c_str(),ini.c_str());
      }
    });
  }
  DrawControls(page);
  ImGui::Spacing(); ui::Description("MFG Unlock | 1.4.5 + XeMFG R1.9.1");
}
}
extern "C" __declspec(dllexport) uint64_t MFGAXePanelDraws() {
  return mfgunlock::xemfg::native::panelDraws.load();
}

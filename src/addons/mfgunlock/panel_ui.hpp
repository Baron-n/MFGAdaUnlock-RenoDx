// SPDX-License-Identifier: MIT
// Shared player-facing presentation. Styles are scoped to this addon only.
#pragma once
#include <deps/imgui/imgui.h>
#ifndef MFGUNLOCK_UI_PREVIEW
#include <include/reshade.hpp>
#endif
#include "backend_selection.hpp"
#include <algorithm>

namespace mfgunlock::ui {
inline constexpr ImVec4 Accent{0.39f,0.84f,0.71f,1.f};
inline constexpr ImVec4 Muted{0.65f,0.72f,0.82f,1.f};
inline constexpr ImVec4 Warning{1.f,0.77f,0.35f,1.f};
struct Theme {
  Theme() {
    const float scale=ImGui::GetFontSize()/16.f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg,ImVec4(.135f,.155f,.19f,1));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,ImVec4(.10f,.12f,.15f,1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,ImVec4(.19f,.23f,.28f,1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,ImVec4(.22f,.29f,.32f,1));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,Accent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,Accent);
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.21f,.28f,.35f,1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(.25f,.36f,.42f,1));
    ImGui::PushStyleColor(ImGuiCol_Header,ImVec4(.22f,.29f,.37f,1));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,ImVec4(.27f,.36f,.44f,1));
    ImGui::PushStyleColor(ImGuiCol_Tab,ImVec4(.12f,.14f,.18f,1));
    ImGui::PushStyleColor(ImGuiCol_TabSelected,ImVec4(.23f,.31f,.40f,1));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled,Muted);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,7.f*scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,10.f*scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(10.f*scale,7.f*scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(10.f*scale,10.f*scale));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2(10.f*scale,8.f*scale));
  }
  ~Theme() { ImGui::PopStyleVar(5); ImGui::PopStyleColor(13); }
};
inline void Description(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text,Muted); ImGui::TextWrapped("%s",text); ImGui::PopStyleColor();
}
inline void Section(const char* title,const char* description=nullptr) {
  ImGui::Spacing(); ImGui::TextUnformatted(title);
  if(description) Description(description);
}
struct Card {
  bool visible;
  Card(const char* id) {
    visible=ImGui::BeginChild(id,ImVec2(0,0),ImGuiChildFlags_AutoResizeY|ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
  }
  ~Card() { ImGui::EndChild(); ImGui::Spacing(); }
  explicit operator bool() const { return visible; }
};
template<class F> void Setting(const char* id,const char* title,const char* description,F control) {
  if(ImGui::GetContentRegionAvail().x<ImGui::GetFontSize()*28.f) {
    ImGui::TextUnformatted(title); if(description) Description(description);
    ImGui::SetNextItemWidth(-1); control(); return;
  }
  if(ImGui::BeginTable(id,2,ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Setting",ImGuiTableColumnFlags_WidthStretch,.58f);
    ImGui::TableSetupColumn("Control",ImGuiTableColumnFlags_WidthStretch,.42f);
    ImGui::TableNextColumn(); ImGui::TextUnformatted(title); if(description) Description(description);
    ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); control(); ImGui::EndTable();
  }
}
inline void Notice(const char* text,ImVec4 color=Warning) {
  ImGui::PushStyleColor(ImGuiCol_Text,color); ImGui::TextWrapped("%s",text); ImGui::PopStyleColor();
}
inline void Hero(const char* multiplier,const char* backend,const char* renderer,const char* state,bool active) {
  ImGui::TextUnformatted("MFG Unlock");
  Description("Frame Generation Control Center");
  ImGui::TextColored(active?Accent:Warning,"%s",state);
  const char* values[]={multiplier,backend,renderer};
  const char* labels[]={"Multiplier","Backend","Renderer"};
  if(ImGui::GetContentRegionAvail().x<ImGui::GetFontSize()*26.f) {
    Card card("##compact_overview");
    if(ImGui::BeginTable("##compact_summary",2,ImGuiTableFlags_SizingStretchSame)) {
      for(int i=0;i<3;++i) {
        ImGui::TableNextColumn(); Description(labels[i]);
        ImGui::TableNextColumn(); ImGui::TextColored(i==0?Accent:ImVec4(.93f,.95f,.98f,1),"%s",values[i]);
      }
      ImGui::EndTable();
    }
    return;
  }
  if(ImGui::BeginTable("##mfg_summary_cards",3,ImGuiTableFlags_SizingStretchSame)) {
    for(int i=0;i<3;++i) {
      ImGui::TableNextColumn(); ImGui::PushID(i);
      ImGui::BeginChild("summary",ImVec2(0,ImGui::GetTextLineHeight()*4.5f+ImGui::GetStyle().WindowPadding.y*2),ImGuiChildFlags_AlwaysUseWindowPadding,
          ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
      ImGui::TextColored(i==0?Accent:ImVec4(.93f,.95f,.98f,1),"%s",values[i]);
      Description(labels[i]); ImGui::EndChild(); ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::Spacing();
}
inline void BackendSelector() {
#ifdef MFGUNLOCK_NATIVE_XEMFG
  const auto selected=backend::Read(); int choice=selected==backend::Kind::XeMfg?1:0;
  Setting("##backend_selection","Frame generation method","Choose the frame-generation system. Requires a game restart.",[&] {
    if(ImGui::Combo("##backend",&choice,"NVIDIA DLSS-G\0Intel XeMFG\0"))
      backend::Select(choice?backend::Kind::XeMfg:backend::Kind::DlssG);
  });
  if(backend::Read()!=backend::active) Notice("Saved. Restart the game to switch frame generation.");
#endif
}
}

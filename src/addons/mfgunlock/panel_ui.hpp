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
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,4.f*scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,6.f*scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(7.f*scale,3.f*scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(7.f*scale,5.f*scale));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2(4.f*scale,4.f*scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(8.f*scale,7.f*scale));
  }
  ~Theme() { ImGui::PopStyleVar(6); ImGui::PopStyleColor(13); }
};
inline void Tooltip(const char* text) {
  if(!text || !text[0] || !ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip|ImGuiHoveredFlags_AllowWhenDisabled)) return;
  if(ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize()*34.f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos(); ImGui::EndTooltip();
  }
}
inline void Help(const char* text) {
  if(!text || !text[0]) return;
  ImGui::SameLine(0,ImGui::GetStyle().ItemInnerSpacing.x);
  ImGui::TextDisabled("(?)"); Tooltip(text);
}
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
  const auto label=[&] {
    ImGui::AlignTextToFramePadding();
    const float helpWidth=description?ImGui::CalcTextSize("(?)").x+ImGui::GetStyle().ItemInnerSpacing.x:0.f;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+(std::max)(1.f,ImGui::GetContentRegionAvail().x-helpWidth));
    ImGui::TextUnformatted(title); ImGui::PopTextWrapPos(); Tooltip(description); Help(description);
  };
  if(ImGui::GetContentRegionAvail().x<ImGui::GetFontSize()*28.f) {
    label(); ImGui::SetNextItemWidth(-1); control(); Tooltip(description); return;
  }
  if(ImGui::BeginTable(id,2,ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_NoPadOuterX)) {
    ImGui::TableSetupColumn("Setting",ImGuiTableColumnFlags_WidthStretch,.55f);
    ImGui::TableSetupColumn("Control",ImGuiTableColumnFlags_WidthStretch,.45f);
    ImGui::TableNextColumn(); label();
    ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1); control(); Tooltip(description); ImGui::EndTable();
  }
}
inline void Notice(const char* text,ImVec4 color=Warning) {
  ImGui::PushStyleColor(ImGuiCol_Text,color); ImGui::TextWrapped("%s",text); ImGui::PopStyleColor();
}
inline void Readout(const char* id,const char* title,const char* value,const char* tooltip=nullptr,ImVec4 color=Muted) {
  Setting(id,title,tooltip,[&] {
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text,color); ImGui::TextWrapped("%s",value); ImGui::PopStyleColor();
  });
}
inline void Hero(const char* multiplier,const char* backend,const char* renderer,const char* state,bool active) {
  const float right=ImGui::GetCursorPosX()+ImGui::GetContentRegionAvail().x;
  ImGui::TextUnformatted("MFG Unlock");
  if(ImGui::GetContentRegionAvail().x>ImGui::CalcTextSize(state).x+ImGui::GetFontSize()*10.f) {
    ImGui::SameLine();
    ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(),right-ImGui::CalcTextSize(state).x));
  }
  ImGui::PushTextWrapPos(); ImGui::TextColored(active?Accent:Warning,"%s",state); ImGui::PopTextWrapPos();
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
  if(ImGui::BeginTable("##mfg_summary_cards",3,ImGuiTableFlags_SizingStretchSame|ImGuiTableFlags_NoPadOuterX)) {
    for(int i=0;i<3;++i) {
      ImGui::TableNextColumn(); ImGui::PushID(i);
      ImGui::BeginChild("summary",ImVec2(0,ImGui::GetTextLineHeight()*2.f+ImGui::GetStyle().ItemSpacing.y+ImGui::GetStyle().WindowPadding.y*2),ImGuiChildFlags_AlwaysUseWindowPadding,
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

#include "ui/profiles.h"
#include <imgui.h>

namespace remix::ui {
  void toolkitColors() {
    ImGui::StyleColorsDark();
    auto &s = ImGui::GetStyle();
    s.Colors[ImGuiCol_WindowBg] = ImVec4(.188f, .188f, .188f, .98f);
    s.Colors[ImGuiCol_PopupBg] = ImVec4(.188f, .188f, .188f, 1);
    s.Colors[ImGuiCol_ChildBg] = ImVec4(.16f, .16f, .16f, 1);
    s.Colors[ImGuiCol_Text] = ImVec4(.85f, .85f, .85f, 1);
    s.Colors[ImGuiCol_Border] = ImVec4(.34f, .34f, .34f, 1);
    s.Colors[ImGuiCol_FrameBg] = ImVec4(.188f, .188f, .188f, 1);
    s.Colors[ImGuiCol_FrameBgHovered] = ImVec4(.15f, .30f, .35f, 1);
    s.Colors[ImGuiCol_Button] = ImVec4(.121f, .129f, .141f, 1);
    s.Colors[ImGuiCol_ButtonHovered] = ImVec4(.27f, .27f, .27f, 1);
    s.Colors[ImGuiCol_Header] = ImVec4(.125f, .125f, .125f, 1);
    s.Colors[ImGuiCol_HeaderHovered] = ImVec4(.17f, .25f, .27f, 1);
    s.Colors[ImGuiCol_HeaderActive] = ImVec4(.17f, .25f, .27f, 1);
    s.Colors[ImGuiCol_TabSelected] = ImVec4(.11f, .42f, .51f, 1);
    s.Colors[ImGuiCol_TabHovered] = ImVec4(.22f, .33f, .36f, 1);
    s.Colors[ImGuiCol_CheckMark] = ImVec4(.35f, .75f, .85f, 1);
  }
  void toolkitStyle() {
    ImGui::StyleColorsDark();
    auto &s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.ChildRounding = s.PopupRounding = s.TabRounding = 2;
    s.FrameRounding = 3;
    s.FrameBorderSize = 1;
    s.WindowPadding = ImVec2(10, 8);
    s.FramePadding = ImVec2(5, 3);
    s.ItemSpacing = ImVec2(6, 5);
    s.ScrollbarSize = 14;
    toolkitColors();
  }
} // namespace remix::ui

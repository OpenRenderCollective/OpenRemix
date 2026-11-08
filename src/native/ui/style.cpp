#include "ui/profiles.h"
#include "ui/theme.h"
#include <imgui.h>
#include <imgui_internal.h>

namespace remix::ui {
  namespace theme {
    namespace {
      ImVec4 shift(ImVec4 c, float amount) {
        return ImVec4(ImClamp(c.x + amount, 0.f, 1.f), ImClamp(c.y + amount, 0.f, 1.f), ImClamp(c.z + amount, 0.f, 1.f),
                      c.w);
      }
      ImVec4 lighten(ImVec4 c, float t) { return ImLerp(c, ImVec4(1, 1, 1, c.w), t); }
      Colors derive(uint32_t background, uint32_t accent) {
        const ImVec4 region = rgb(background), picked = rgb(accent);
        // Panels and tabs a step above and below the background, as Blender's are.
        return {region,
                shift(region, 13.f / 255),
                shift(region, 22.f / 255),
                shift(region, -13.f / 255),
                shift(region, 13.f / 255),
                picked,
                lighten(picked, .12f)};
      }
      const config::PanelState defaults;
      Colors current = derive(defaults.background, defaults.accent);
      uint32_t applied[2] = {defaults.background, defaults.accent};
    } // namespace
    const Colors &colors() { return current; }
    void setColors(uint32_t background, uint32_t accent) {
      if (applied[0] == background && applied[1] == accent)
        return;
      applied[0] = background;
      applied[1] = accent;
      current = derive(background, accent);
      toolkitColors();
    }
  } // namespace theme

  void toolkitColors() {
    using namespace theme;
    const auto &[region, panel, subpanel, tab, tabHover, accent, accentHover] = colors();
    ImGui::StyleColorsDark();
    auto &c = ImGui::GetStyle().Colors;
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = textDim;
    c[ImGuiCol_WindowBg] = ImVec4(region.x, region.y, region.z, .98f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = menuBack;
    c[ImGuiCol_Border] = outline;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    // Frames are the text fields that ImGui still draws (InputText, InputFloat).
    c[ImGuiCol_FrameBg] = textField;
    c[ImGuiCol_FrameBgHovered] = textFieldHover;
    c[ImGuiCol_FrameBgActive] = textField;
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_MenuBarBg] = rgb(0x1d1d1d);
    c[ImGuiCol_TitleBgCollapsed] = rgb(0x1d1d1d, .8f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = field;
    c[ImGuiCol_ScrollbarGrabHovered] = fieldHover;
    c[ImGuiCol_ScrollbarGrabActive] = rgb(0x7a7a7a);
    c[ImGuiCol_CheckMark] = textSelected;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accentHover;
    c[ImGuiCol_Button] = field;
    c[ImGuiCol_ButtonHovered] = fieldHover;
    c[ImGuiCol_ButtonActive] = accent;
    // Headers are list rows and menu items (the panels draw their own headers).
    c[ImGuiCol_Header] = ImVec4(accent.x, accent.y, accent.z, .5f);
    c[ImGuiCol_HeaderHovered] = ImVec4(accent.x, accent.y, accent.z, .7f);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Separator] = c[ImGuiCol_SeparatorHovered] = separator;
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = rgb(0xffffff, .1f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(accent.x, accent.y, accent.z, .7f);
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_InputTextCursor] = rgb(0x71a8ff);
    c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = tab;
    c[ImGuiCol_TabHovered] = tabHover;
    c[ImGuiCol_TabSelected] = c[ImGuiCol_TabDimmedSelected] = region;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PlotLines] = rgb(0xcccccc);
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_TableHeaderBg] = panel;
    c[ImGuiCol_TableBorderStrong] = rgb(0x1d1d1d);
    c[ImGuiCol_TableBorderLight] = panel;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = rgb(0xffffff, .04f);
    c[ImGuiCol_TextLink] = rgb(0x6fa9e6);
    c[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, .8f);
    c[ImGuiCol_NavCursor] = c[ImGuiCol_DragDropTarget] = accent;
    c[ImGuiCol_ModalWindowDimBg] = rgb(0x000000, .35f);
  }
  void toolkitStyle() {
    ImGui::StyleColorsDark();
    auto &s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.ChildRounding = s.FrameRounding = s.PopupRounding = s.GrabRounding = s.TabRounding = theme::rounding;
    s.ScrollbarRounding = 12;
    s.WindowBorderSize = s.ChildBorderSize = 0;
    s.FrameBorderSize = 0;
    s.PopupBorderSize = 1;
    s.TabBorderSize = 0;
    s.TabBarOverlineSize = 2;
    s.WindowPadding = ImVec2(10, 8);
    s.FramePadding = ImVec2(6, 3);
    s.ItemSpacing = ImVec2(8, 2);
    s.ItemInnerSpacing = ImVec2(5, 4);
    s.IndentSpacing = 16;
    s.ScrollbarSize = 11;
    s.GrabMinSize = 10;
    s.DisabledAlpha = .5f;
    s.SeparatorTextBorderSize = 1;
    toolkitColors();
  }
} // namespace remix::ui

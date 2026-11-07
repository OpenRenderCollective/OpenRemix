#pragma once
#include <cstdint>
#include <imgui.h>

// The panel's Blender-like palette and metrics, shared by the ImGui style (ui/style.cpp) and the
// widgets drawn by ui/widgets.cpp.
namespace remix::ui::theme {
  constexpr ImVec4 rgb(unsigned hex, float alpha = 1) {
    return ImVec4(float((hex >> 16) & 0xff) / 255, float((hex >> 8) & 0xff) / 255, float(hex & 0xff) / 255, alpha);
  }
  // The two colors chosen in the panel (config::PanelState, 0xRRGGBB); setColors() derives the rest.
  struct Colors {
    ImVec4 region, panel, subpanel, tab, tabHover; // from the background
    ImVec4 accent, accentHover;                    // from the accent
  };
  // The current colors (ui/style.cpp).
  const Colors &colors();
  // Applies the chosen colors to the theme and the ImGui style, when they changed.
  void setColors(uint32_t background, uint32_t accent);

  inline const ImVec4 text = rgb(0xe6e6e6), textDim = rgb(0x999999), textSelected = rgb(0xffffff);
  inline const ImVec4 field = rgb(0x545454), fieldHover = rgb(0x656565), fieldActive = rgb(0x222222);
  inline const ImVec4 menu = rgb(0x282828), menuHover = rgb(0x333333), menuBack = rgb(0x181818);
  inline const ImVec4 textField = rgb(0x1d1d1d), textFieldHover = rgb(0x262626);
  inline const ImVec4 outline = rgb(0x262626), separator = rgb(0xe6e6e6, .15f);

  // Sizes at a display scale of 1 (multiplied by ImGuiStyle::FontScaleDpi).
  constexpr float fontSize = 14, rounding = 4, panelGap = 4, panelPadding = 8;
  // Width of the label column of a property row, as a fraction of the row.
  constexpr float labelColumn = .42f;
} // namespace remix::ui::theme

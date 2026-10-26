#include "ui/profiles.h"
#include "api/imgui_compat.h"
#include <algorithm>
#include <imgui.h>
#include <string>

namespace remix::ui {
  bool drawPanel(config::Profiles &profiles, ProfileUi &profileUi, void *user, DrawSettingsTab drawTab) {
    bool open = true;
    auto panel = profiles.panel();
    const auto *viewport = ImGui::GetMainViewport();
    const float scale = ImGui::GetStyle().FontScaleDpi;
    const float width = std::min(panel.width * scale, viewport->WorkSize.x);
    const ImVec2 origin(viewport->WorkPos.x + (panel.right ? viewport->WorkSize.x - width : 0),
                        viewport->WorkPos.y);
    ImGui::SetNextWindowPos(origin);
    ImGui::SetNextWindowSize(ImVec2(width, viewport->WorkSize.y));
    ImGui::Begin("OpenRemix", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const bool minimal = viewport->WorkSize.y < 200 * scale;
    if (!minimal) {
      const bool compact = width <= 340 * scale;
      ImGui::TextUnformatted("OpenRemix");
      ImGui::SameLine();
      if (!compact) {
        // The panel's shortcut: click, then press it (ui/menu_shortcut.cpp).
        const std::string label =
            (profileUi.capturingMenuShortcut ? std::string("Press a shortcut...") : profileUi.menuShortcut) +
            "###Panel shortcut";
        if (ImGui::SmallButton(label.c_str()))
          profileUi.captureMenuShortcut = true;
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Shortcut that opens and closes this panel.\nClick, then press a key while holding "
                            "the modifiers to use (Alt, Ctrl, Shift).\nEscape keeps the current shortcut.");
        ImGui::SameLine();
      }
      if (ImGui::SmallButton(compact ? (panel.right ? "<" : ">")
                                     : (panel.right ? "Dock left" : "Dock right")))
        panel.right = !panel.right;
      if (compact && ImGui::IsItemHovered())
        ImGui::SetTooltip("Dock to the other edge");
      ImGui::SameLine();
      if (ImGui::SmallButton(compact ? "X" : "Close"))
        open = false;
      if (compact && ImGui::IsItemHovered())
        ImGui::SetTooltip("Close developer panel (%s)", profileUi.menuShortcut.c_str());
    }
    const bool hasPlugin = remixapi_imgui_HasDrawCallback() != 0;
    // Tabs before kMods edit the profile's settings.
    enum { kTextures = 5, kMods, kProfiles, kPlugin };
    const int tabCount = hasPlugin ? 9 : 8;
    static const char *tabs[] = {"Rendering", "Lighting", "Sky", "Post-processing", "Diagnostics", "Textures", "Mods", "Profiles", "Plugin"};
    const float footerHeight = ImGui::GetFrameHeightWithSpacing() * 2 + ImGui::GetStyle().ItemSpacing.y * 2;
    auto draft = profiles.current();
    if (panel.tab >= tabCount)
      panel.tab = 0;
    const int restoreTab = panel.tab;
    if (ImGui::BeginTabBar("Settings tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
      if (minimal) {
        if (ImGui::TabItemButton(panel.right ? "<" : ">", ImGuiTabItemFlags_Trailing))
          panel.right = !panel.right;
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Dock to the other edge");
        if (ImGui::TabItemButton("X", ImGuiTabItemFlags_Trailing))
          open = false;
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Close developer panel (%s)", profileUi.menuShortcut.c_str());
      }
      for (int i = 0; i < tabCount; ++i) {
        const auto flags =
            profileUi.restoreTab && i == restoreTab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem(tabs[i], nullptr, flags)) {
          panel.tab = i;
          ImGui::BeginChild("Tab contents",
                            ImVec2(0, std::max(1.f, ImGui::GetContentRegionAvail().y - footerHeight)),
                            ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
          ImGui::PushItemWidth(std::max(80.f * scale, ImGui::GetContentRegionAvail().x * .46f));
          if (i == kMods)
            drawMods();
          else if (i == kProfiles)
            drawProfiles(profiles, profileUi);
          else if (i == kPlugin)
            remixapi_imgui_InvokeDrawCallback();
          else
            drawTab(user, i, draft);
          ImGui::PopItemWidth();
          ImGui::EndChild();
          ImGui::EndTabItem();
        }
      }
      ImGui::EndTabBar();
      profileUi.restoreTab = false;
    }
    if (panel.tab < kMods && draft != profiles.current())
      profiles.edit(draft);
    ImGui::BeginChild("Profile footer", ImVec2(0, std::max(1.f, ImGui::GetContentRegionAvail().y)),
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ui::drawProfileFooter(profiles, profileUi);
    ImGui::EndChild();
    // An edge handle allows horizontal resizing while preserving full height.
    const auto cursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(origin.x + (panel.right ? 0 : width - 5 * scale), origin.y));
    ImGui::InvisibleButton("Panel width", ImVec2(5 * scale, viewport->WorkSize.y));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActivated())
      panel.width = width / scale;
    else if (ImGui::IsItemActive())
      panel.width = std::clamp(panel.width + ImGui::GetIO().MouseDelta.x / scale * (panel.right ? -1.f : 1.f),
                               240.f, 1600.f);
    const bool resizing = ImGui::IsItemActive();
    ImGui::SetCursorScreenPos(cursor);
    ui::drawProfileDialogs(profiles, profileUi);
    ImGui::End();
    // Keep width edits live, but persist only after the drag ends.
    if (!resizing)
      profiles.savePanel(panel);
    else
      profiles.previewPanel(panel);
    return open;
  }
} // namespace remix::ui

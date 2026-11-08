#include "ui/profiles.h"
#include "api/imgui_compat.h"
#include "ui/theme.h"
#include "ui/widgets.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <string>

namespace remix::ui {
  namespace {
    // The panel's own colors: session state, like its width and side, not a rendering profile.
    void drawPanelColors(config::PanelState &panel) {
      Panel colors{"Panel colors", ImGuiTreeNodeFlags_DefaultOpen};
      if (!colors)
        return;
      auto edit = [](const char *label, uint32_t &stored) {
        float rgb[3] = {float((stored >> 16) & 0xff) / 255, float((stored >> 8) & 0xff) / 255,
                        float(stored & 0xff) / 255};
        if (!ColorEdit3(label, rgb))
          return;
        stored = 0;
        for (float channel : rgb)
          stored = stored << 8 | uint32_t(std::lround(std::clamp(channel, 0.f, 1.f) * 255));
      };
      edit("Background", panel.background);
      edit("Accent", panel.accent);
      if (ImGui::Button("Reset colors")) {
        const config::PanelState defaults;
        panel.background = defaults.background;
        panel.accent = defaults.accent;
      }
      theme::setColors(panel.background, panel.accent);
    }
  } // namespace

  bool drawPanel(config::Profiles &profiles, ProfileUi &profileUi, void *user, DrawSettingsTab drawTab) {
    bool open = true;
    auto panel = profiles.panel();
    const auto *viewport = ImGui::GetMainViewport();
    const float scale = ImGui::GetStyle().FontScaleDpi;
    const float width = std::min(panel.width * scale, viewport->WorkSize.x);
    const ImVec2 origin(viewport->WorkPos.x + (panel.right ? viewport->WorkSize.x - width : 0), viewport->WorkPos.y);
    ImGui::SetNextWindowPos(origin);
    ImGui::SetNextWindowSize(ImVec2(width, viewport->WorkSize.y));
    ImGui::Begin("OpenRemix", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
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
      if (ImGui::SmallButton(compact ? (panel.right ? "<" : ">") : (panel.right ? "Dock left" : "Dock right")))
        panel.right = !panel.right;
      if (compact && ImGui::IsItemHovered())
        ImGui::SetTooltip("Dock to the other edge");
      ImGui::SameLine();
      if (ImGui::SmallButton(compact ? "X" : "Close"))
        open = false;
      if (compact && ImGui::IsItemHovered())
        ImGui::SetTooltip("Close developer panel (%s)", profileUi.menuShortcut.c_str());
      // Property search across the settings tabs; Ctrl+F focuses it.
      const bool clear = profileUi.search[0] != 0;
      const float clearWidth = clear ? ImGui::CalcTextSize("Clear").x + ImGui::GetStyle().FramePadding.x * 2 +
                                           ImGui::GetStyle().ItemSpacing.x
                                     : 0;
      if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
          ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F))
        ImGui::SetKeyboardFocusHere();
      ImGui::SetNextItemWidth(std::max(1.f, ImGui::GetContentRegionAvail().x - clearWidth));
      ImGui::InputTextWithHint("##Search", "Search settings (Ctrl+F)", profileUi.search, sizeof(profileUi.search));
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shows the settings of every tab whose name holds all the words typed, in any order.\n"
                          "A panel's name matches all of its settings.");
      if (clear) {
        ImGui::SameLine();
        if (ImGui::Button("Clear"))
          profileUi.search[0] = 0;
      }
    }
    const bool hasPlugin = remixapi_imgui_HasDrawCallback() != 0;
    const int tabCount = int(hasPlugin ? Tab::Plugin : Tab::Profiles) + 1;
    static const char *tabs[] = {"Display",     "Lighting", "Materials", "Sky & weather", "Post-Process", "Performance",
                                 "Diagnostics", "Textures", "Capture",   "Profiles",      "Plugin"};
    const float footerHeight = ImGui::GetFrameHeightWithSpacing() * 2 + ImGui::GetStyle().ItemSpacing.y * 2;
    auto draft = profiles.current();
    if (panel.tab >= tabCount)
      panel.tab = 0;
    const int restoreTab = panel.tab;
    // A tab's scrolling contents; the panels' backdrops (ui::Panel) reach into its padding.
    const auto contents = [&](const char *id, auto draw) {
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                          ImVec2(2 * theme::panelPadding * scale, ImGui::GetStyle().WindowPadding.y));
      ImGui::BeginChild(id, ImVec2(0, std::max(1.f, ImGui::GetContentRegionAvail().y - footerHeight)),
                        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_AlwaysVerticalScrollbar);
      ImGui::PopStyleVar();
      ImGui::PushItemWidth(std::max(80.f * scale, ImGui::GetContentRegionAvail().x * .46f));
      draw();
      ImGui::PopItemWidth();
      ImGui::EndChild();
    };
    const bool searching = !minimal && profileUi.search[0] != 0;
    if (searching) {
      // Every settings tab at once, each a panel holding its matches.
      contents("Search results", [&] {
        setSearch(profileUi.search);
        for (int i = 0; i <= int(Tab::Capture); ++i) {
          if (i == int(Tab::Textures))
            continue;
          ImGui::PushID(i);
          if (Panel tab{tabs[i]})
            drawTab(user, i, draft);
          ImGui::PopID();
        }
        const bool found = searchMatches() > 0;
        setSearch(nullptr);
        if (!found)
          ImGui::TextDisabled("No settings match \"%s\".", profileUi.search);
      });
      profileUi.restoreTab = true;
    } else if (ImGui::BeginTabBar("Settings tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
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
          contents("Tab contents", [&] {
            if (i == int(Tab::Profiles)) {
              drawProfiles(profiles, profileUi);
              drawPanelColors(panel);
            } else if (i == int(Tab::Plugin))
              remixapi_imgui_InvokeDrawCallback();
            else
              drawTab(user, i, draft);
          });
          ImGui::EndTabItem();
        }
      }
      ImGui::EndTabBar();
      profileUi.restoreTab = false;
    }
    if ((searching || panel.tab <= int(Tab::Capture)) && draft != profiles.current())
      profiles.edit(draft);
    ImGui::BeginChild("Profile footer", ImVec2(0, std::max(1.f, ImGui::GetContentRegionAvail().y)),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
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
      panel.width =
          std::clamp(panel.width + ImGui::GetIO().MouseDelta.x / scale * (panel.right ? -1.f : 1.f), 240.f, 1600.f);
    const bool resizing = ImGui::IsItemActive();
    ImGui::SetCursorScreenPos(cursor);
    ui::drawProfileDialogs(profiles, profileUi);
    ImGui::End();
    // Keep width and color edits live, but persist only after the drag ends.
    if (!resizing && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
      profiles.savePanel(panel);
    else
      profiles.previewPanel(panel);
    return open;
  }
} // namespace remix::ui

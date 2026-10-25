#include "ui/profiles.h"
#include <algorithm>
#include <imgui.h>

namespace remix::ui {
  namespace {
    void continueButtonRow(const char *nextLabel) {
      const float end = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
      const float nextWidth = ImGui::CalcTextSize(nextLabel).x + 2 * ImGui::GetStyle().FramePadding.x;
      if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + nextWidth <= end)
        ImGui::SameLine();
    }
    void startName(config::Profiles &profiles, ProfileUi &ui, bool duplicate) {
      profiles.clearMessage();
      ui.name[0] = 0;
      ui.duplicate = duplicate;
      ui.nameDialog = ui.focusName = true;
    }
    void feedback(const config::Profiles &profiles) {
      if (profiles.message().empty())
        return;
      if (profiles.hasError())
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, .65f, .45f, 1.f));
      ImGui::TextWrapped("%s", profiles.message().c_str());
      if (profiles.hasError())
        ImGui::PopStyleColor();
    }
  } // namespace
  void drawProfiles(config::Profiles &profiles, ProfileUi &ui) {
    if (ImGui::Button("New"))
      startName(profiles, ui, false);
    continueButtonRow("Duplicate");
    if (ImGui::Button("Duplicate"))
      startName(profiles, ui, true);
    continueButtonRow("Refresh");
    if (ImGui::Button("Refresh"))
      profiles.refresh();
    ImGui::TextWrapped(
        "New starts from built-in defaults. Duplicate saves all current values, including unsaved edits.");
    ImGui::SeparatorText("Available files");
    if (profiles.files().empty())
      ImGui::TextDisabled("No saved profiles");
    for (const auto &name : profiles.files()) {
      if (ImGui::Selectable(name.c_str(), ui.selected == name))
        ui.selected = name;
    }
    ImGui::BeginDisabled(ui.selected.empty());
    if (ImGui::Button("Load"))
      ui.actions.request(profiles, ProfileAction::Load, ui.selected);
    ImGui::EndDisabled();
    ImGui::SeparatorText("Profile folder");
    const auto path = profiles.folder().u8string();
    ImGui::TextWrapped("%s", reinterpret_cast<const char *>(path.c_str()));
    ImGui::TextWrapped(
        "Add .conf files here, then Refresh. Saving is explicit; closing the panel keeps edits in memory.");
    feedback(profiles);
  }

  void drawProfileFooter(config::Profiles &profiles, ProfileUi &ui) {
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1);
    const auto label = profiles.active() + (profiles.dirty() ? " * unsaved" : "");
    if (ImGui::BeginCombo("##Active profile", label.c_str())) {
      if (profiles.files().empty())
        ImGui::TextDisabled("Save to create a profile");
      for (const auto &name : profiles.files()) {
        if (ImGui::Selectable(name.c_str(), name == profiles.active()))
          ui.actions.request(profiles, ProfileAction::Load, name);
        if (name == profiles.active())
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    if (ImGui::Button("Save"))
      profiles.save();
    ImGui::SameLine();
    ImGui::BeginDisabled(!profiles.dirty());
    if (ImGui::Button("Revert"))
      ui.actions.request(profiles, ProfileAction::Revert);
    ImGui::EndDisabled();
    if (profiles.dirty()) {
      ImGui::SameLine();
      ImGui::TextUnformatted("*");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Unsaved changes");
    }
    if (!profiles.message().empty()) {
      ImGui::SameLine();
      ImGui::TextColored(profiles.hasError() ? ImVec4(1, .65f, .45f, 1) : ImVec4(.7f, .85f, 1, 1), "%s",
                         ImGui::GetContentRegionAvail().x < 110 * ImGui::GetStyle().FontScaleDpi ? "!"
                         : profiles.hasError() ? "Profile error"
                                               : "Profile warning");
      if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(420);
        feedback(profiles);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
      }
    }
  }

  void drawProfileDialogs(config::Profiles &profiles, ProfileUi &ui) {
    if (ui.nameDialog)
      ImGui::OpenPopup("Profile name");
    const auto size = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0),
                                        ImVec2(std::max(1.f, size.x - 20), std::max(1.f, size.y - 20)));
    ImGui::SetNextWindowSize(ImVec2(std::min(420 * ImGui::GetStyle().FontScaleDpi, size.x - 20), 0),
                             ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Profile name", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextWrapped("%s",
                         ui.duplicate ? "Duplicate current settings" : "New profile from built-in defaults");
      if (ui.focusName) {
        ImGui::SetKeyboardFocusHere();
        ui.focusName = false;
      }
      ImGui::SetNextItemWidth(-1);
      const bool enter =
          ImGui::InputText("##Name", ui.name, sizeof(ui.name), ImGuiInputTextFlags_EnterReturnsTrue);
      if (ImGui::Button(ui.duplicate ? "Duplicate" : "Create") || enter) {
        bool close = false;
        if (ui.duplicate)
          close = profiles.duplicate(ui.name);
        else {
          // Check the name before asking about unsaved edits, without changing settings.
          if (!config::Profiles::validName(ui.name)) {
            ImGui::OpenPopup("Invalid name");
          } else {
            close = ui.actions.request(profiles, ProfileAction::New, ui.name) ||
                    ui.actions.pending != ProfileAction::None;
          }
        }
        if (close) {
          ui.nameDialog = false;
          ImGui::CloseCurrentPopup();
        }
      }
      continueButtonRow("Cancel");
      if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ui.nameDialog = false;
        ImGui::CloseCurrentPopup();
      }
      ImGui::TextWrapped(
          "Use a unique name without path components. The .conf extension is added automatically.");
      feedback(profiles);
      if (ImGui::BeginPopup("Invalid name")) {
        ImGui::TextUnformatted("Enter a name without path components or reserved characters.");
        ImGui::EndPopup();
      }
      ImGui::EndPopup();
    }
    if (ui.actions.pending != ProfileAction::None && !ui.nameDialog)
      ImGui::OpenPopup("Unsaved changes");
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0),
                                        ImVec2(std::max(1.f, size.x - 20), std::max(1.f, size.y - 20)));
    ImGui::SetNextWindowSize(ImVec2(std::min(420 * ImGui::GetStyle().FontScaleDpi, size.x - 20), 0),
                             ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextWrapped("Save changes to '%s' before continuing?", profiles.active().c_str());
      for (auto [label, choice] : {std::pair{"Save", UnsavedChoice::Save},
                                   {"Discard", UnsavedChoice::Discard},
                                   {"Cancel", UnsavedChoice::Cancel}}) {
        if (ImGui::Button(label) && ui.actions.resolve(profiles, choice))
          ImGui::CloseCurrentPopup();
        if (choice == UnsavedChoice::Save)
          ImGui::SetItemDefaultFocus();
        if (choice != UnsavedChoice::Cancel)
          continueButtonRow(choice == UnsavedChoice::Save ? "Discard" : "Cancel");
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ui.actions.resolve(profiles, UnsavedChoice::Cancel);
        ImGui::CloseCurrentPopup();
      }
      feedback(profiles);
      ImGui::EndPopup();
    }
  }
} // namespace remix::ui

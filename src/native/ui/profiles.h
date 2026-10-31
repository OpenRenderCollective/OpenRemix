#pragma once
#include "runtime/profiles.h"

namespace remix::ui {
  enum class ProfileAction { None, Load, New, Revert };
  enum class UnsavedChoice { Save, Discard, Cancel };
  // Shared by the modal UI and CPU tests; failed actions leave the dialog open.
  struct ProfileActions {
    ProfileAction pending = ProfileAction::None;
    std::string name;
    bool request(config::Profiles &profiles, ProfileAction action, const std::string &target = {});
    bool resolve(config::Profiles &profiles, UnsavedChoice choice);

  private:
    bool execute(config::Profiles &profiles);
  };
  struct ProfileUi {
    ProfileActions actions;
    std::string selected;
    char name[256]{};
    bool nameDialog = false, duplicate = false, focusName = false;
    bool restoreTab = true;
    // The panel's shortcut (ui/menu_shortcut.cpp): its name, whether the next key
    // pressed becomes it, and the header button's request for that.
    std::string menuShortcut = "Alt+X";
    bool capturingMenuShortcut = false, captureMenuShortcut = false;
    // The USD scene capture shortcut, likewise ("" while none is set).
    std::string sceneCaptureShortcut;
    bool capturingSceneCaptureShortcut = false, captureSceneCaptureShortcut = false;
    // The screenshot shortcut, the same way (ui::screenshotShortcut).
    std::string screenshotShortcut = "Alt+F12";
    bool capturingScreenshotShortcut = false, captureScreenshotShortcut = false;
  };
  void drawProfiles(config::Profiles &profiles, ProfileUi &ui);
  void drawProfileFooter(config::Profiles &profiles, ProfileUi &ui);
  void drawMods(ProfileUi &ui, config::Settings &settings);
  void drawProfileDialogs(config::Profiles &profiles, ProfileUi &ui);
  using DrawSettingsTab = void (*)(void *, int, config::Settings &);
  bool drawPanel(config::Profiles &profiles, ProfileUi &ui, void *user, DrawSettingsTab drawTab);
  void toolkitStyle();
  void toolkitColors();
} // namespace remix::ui

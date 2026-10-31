#pragma once
#include "platform/window.h"
#include "ui/profiles.h"

namespace remix::ui {
  // After each event pump: applies the saved shortcut to the window, stores one
  // the window captured, starts or stops a capture the panel asked for, and
  // returns whether the panel is to toggle.
  bool menuShortcut(RemixSdlWindow &window, config::Profiles &profiles, ProfileUi &ui, bool panelOpen);
  // The screenshot shortcut, captured and named the same way: whether it was
  // pressed since the last call.
  bool screenshotShortcut(RemixSdlWindow &window, config::Profiles &profiles, ProfileUi &ui, bool panelOpen);
} // namespace remix::ui

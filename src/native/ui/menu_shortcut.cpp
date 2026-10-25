#include "ui/menu_shortcut.h"

namespace remix::ui {
  bool menuShortcut(RemixSdlWindow &window, config::Profiles &profiles, ProfileUi &ui, bool panelOpen) {
    auto panel = profiles.panel();
    remixapi_Key key{};
    uint32_t modifiers = 0;
    if (window.takeCapturedMenuShortcut(key, modifiers)) {
      panel.menuKey = key;
      panel.menuModifiers = modifiers;
      profiles.savePanel(panel);
    }
    if (ui.captureMenuShortcut && panelOpen)
      window.captureMenuShortcut();
    else if (!panelOpen)
      window.cancelMenuShortcutCapture(); // No key is taken while the panel is closed.
    ui.captureMenuShortcut = false;
    window.setMenuShortcut(static_cast<remixapi_Key>(panel.menuKey), panel.menuModifiers);
    ui.capturingMenuShortcut = window.capturingMenuShortcut();
    ui.menuShortcut = RemixSdlWindow::shortcutName(static_cast<remixapi_Key>(panel.menuKey), panel.menuModifiers);
    return window.takeMenuToggle();
  }
} // namespace remix::ui

#include "ui/menu_shortcut.h"
#if REMIX_HAS_USD
#include "usd/capture.h"
#endif

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
    if (window.takeCapturedSceneCaptureShortcut(key, modifiers)) {
      panel.sceneCaptureKey = key;
      panel.sceneCaptureModifiers = modifiers;
      profiles.savePanel(panel);
    }
    if (ui.captureMenuShortcut && panelOpen)
      window.captureMenuShortcut();
    else if (ui.captureSceneCaptureShortcut && panelOpen)
      window.captureSceneCaptureShortcut();
    else if (!panelOpen)
      window.cancelMenuShortcutCapture(); // No key is taken while the panel is closed.
    ui.captureMenuShortcut = ui.captureSceneCaptureShortcut = false;
    window.setMenuShortcut(static_cast<remixapi_Key>(panel.menuKey), panel.menuModifiers);
    window.setSceneCaptureShortcut(static_cast<remixapi_Key>(panel.sceneCaptureKey), panel.sceneCaptureModifiers);
    ui.capturingMenuShortcut = window.capturingMenuShortcut();
    ui.capturingSceneCaptureShortcut = window.capturingSceneCaptureShortcut();
    ui.menuShortcut = RemixSdlWindow::shortcutName(static_cast<remixapi_Key>(panel.menuKey), panel.menuModifiers);
    ui.sceneCaptureShortcut =
        panel.sceneCaptureKey ? RemixSdlWindow::shortcutName(static_cast<remixapi_Key>(panel.sceneCaptureKey),
                                                             panel.sceneCaptureModifiers)
                              : std::string();
    // Pressed with the panel open or closed alike.
    if (window.takeSceneCapturePress()) {
#if REMIX_HAS_USD
      usd::requestCapture();
#endif
    }
    return window.takeMenuToggle();
  }
} // namespace remix::ui

#pragma once
#include "runtime/settings.h"
#include <remix/remix_c.h>
#include <filesystem>
#include <set>
#include <utility>
#include <vector>

namespace remix::config {
  // The panel's and the screenshot shortcut's default modifier: Cmd on macOS,
  // whose keyboards have Option rather than Alt; Alt elsewhere.
#if defined(__APPLE__)
  inline constexpr uint32_t defaultShortcutModifiers = 8;
#else
  inline constexpr uint32_t defaultShortcutModifiers = 1;
#endif
  struct PanelState {
    bool right = false;
    float width = 520;
    int tab = 0;
    // The panel's shortcut (RemixSdlWindow::setMenuShortcut): a key and exactly
    // the modifiers held with it (1 Alt, 2 Ctrl, 4 Shift, 8 Cmd/Win). Alt+X
    // (Cmd+X on macOS).
    uint32_t menuKey = REMIXAPI_KEY_X, menuModifiers = defaultShortcutModifiers;
    // The USD scene capture shortcut, kept the same way (key 0: none).
    uint32_t sceneCaptureKey = 0, sceneCaptureModifiers = 0;
    // The screenshot shortcut, the same way. Alt+F12 (Cmd+F12 on macOS).
    uint32_t screenshotKey = REMIXAPI_KEY_F12, screenshotModifiers = defaultShortcutModifiers;
    bool operator==(const PanelState &) const = default;
  };

  // CPU-only session owned by the runtime. File operations never mutate a live
  // snapshot until parsing/writing succeeds. UI and API edits share this state.
  class Profiles {
  public:
    void startup(const std::filesystem::path &directory = defaultDirectory());
    static std::filesystem::path defaultDirectory();
    static bool validName(std::string_view name);
    bool edit(const Settings &settings);
    // A host's default for a setting (remixapi_SetConfigDefaultEXT): applied
    // now, and to every profile loaded or created later, wherever the profile
    // has no value of its own (a key its file does not list). False: an
    // unknown key or an invalid value.
    bool setDefault(const std::string &key, const std::string &value);
    bool load(const std::string &name);
    bool create(const std::string &name);
    bool duplicate(const std::string &name);
    bool save();
    void revert();
    void refresh();
    void previewPanel(const PanelState &panel) {
      m_panel = panel;
      m_panelDirty = true;
    }
    void savePanel(const PanelState &panel);
    bool dirty() const { return m_needsSave || m_current != m_saved; }
    const Settings &current() const { return m_current; }
    const std::string &active() const { return m_active; }
    const std::vector<std::string> &files() const { return m_files; }
    const std::filesystem::path &folder() const { return m_folder; }
    const PanelState &panel() const { return m_panel; }
    const std::string &message() const { return m_message; }
    bool hasError() const { return m_error; }
    void clearMessage() {
      m_message.clear();
      m_error = false;
    }

  private:
    bool unique(const std::string &name);
    bool writeProfile(const std::string &name, bool replace);
    bool readProfile(const std::string &name, Settings &settings, UnknownSettings &unknown,
                     std::set<std::string> *listed = nullptr);
    void applyDefaults();
    bool fail(const std::string &message);
    void remember();
    Settings m_current, m_saved;
    UnknownSettings m_unknown;
    std::set<std::string> m_listed; // the keys the active profile's file has
    std::vector<std::pair<std::string, std::string>> m_defaults; // the host's, in order
    std::filesystem::path m_directory, m_folder;
    std::vector<std::string> m_files;
    std::string m_active = "Default", m_last, m_message;
    bool m_persisted = false, m_needsSave = false, m_error = false;
    PanelState m_panel;
    bool m_panelDirty = false;
  };
} // namespace remix::config

#include "runtime/profiles.h"
#include "runtime/float_chars.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace remix::config {
  namespace {
    std::filesystem::path pathFromUtf8(std::string_view text) {
      return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }
    std::string utf8(const std::filesystem::path &path) {
      auto text = path.u8string();
      return {text.begin(), text.end()};
    }
    std::string lower(std::string value) {
      for (auto &c : value)
        if (c >= 'A' && c <= 'Z')
          c += 'a' - 'A';
      return value;
    }
    bool regular(const std::filesystem::path &path) {
      std::error_code ec;
      return std::filesystem::is_regular_file(std::filesystem::symlink_status(path, ec));
    }
    bool readFile(const std::filesystem::path &path, std::string &text) {
      if (!regular(path))
        return false;
      std::ifstream input(path, std::ios::binary);
      std::ostringstream buffer;
      buffer << input.rdbuf();
      if (!input || input.bad())
        return false;
      text = buffer.str();
      return true;
    }
    // The temporary file is exclusively created beside its destination. Failed
    // writes/replacements only remove that temporary file, never the old data.
    bool atomicWrite(const std::filesystem::path &path, const std::string &text, bool replace,
                     std::string &error) {
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      if (ec) {
        error = ec.message();
        return false;
      }
      const auto status = std::filesystem::symlink_status(path, ec);
      if (!ec && std::filesystem::exists(status) && (!replace || !std::filesystem::is_regular_file(status))) {
        error = "Destination already exists or is not a regular file";
        return false;
      }
      static std::atomic<uint64_t> sequence{};
      auto temporary = path;
#if defined(_WIN32)
      temporary += ".tmp-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++sequence);
      HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) {
        error = "Cannot create temporary file";
        return false;
      }
      DWORD written = 0;
      bool ok = text.size() <= MAXDWORD &&
                WriteFile(file, text.data(), DWORD(text.size()), &written, nullptr) && written == text.size();
      ok = FlushFileBuffers(file) && ok;
      ok = CloseHandle(file) && ok;
      if (ok)
        ok = MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0));
#else
      temporary += ".tmp-" + std::to_string(getpid()) + "-" + std::to_string(++sequence);
      int file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
      if (file < 0) {
        error = std::generic_category().message(errno);
        return false;
      }
      size_t offset = 0;
      while (offset < text.size()) {
        auto count = ::write(file, text.data() + offset, text.size() - offset);
        if (count < 0 && errno == EINTR)
          continue;
        if (count <= 0)
          break;
        offset += size_t(count);
      }
      bool ok = offset == text.size() && ::fsync(file) == 0;
      ok = ::close(file) == 0 && ok;
      if (ok) {
        if (replace)
          ok = ::rename(temporary.c_str(), path.c_str()) == 0;
        else
          ok = ::link(temporary.c_str(), path.c_str()) == 0;
      }
#endif
      std::filesystem::remove(temporary, ec);
      if (!ok)
        error = "Could not write or atomically replace file";
      return ok;
    }
  } // namespace

  std::filesystem::path Profiles::defaultDirectory() {
    if (const char *directory = std::getenv("REMIX_CONFIG_DIR"); directory && *directory)
      return pathFromUtf8(directory);
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    return (ec ? std::filesystem::path(".") : cwd) / "openremix";
  }

  bool Profiles::validName(std::string_view name) {
    if (name.empty() || name.size() > 100 || name == "." || name == ".." || name.front() == ' ' ||
        name.back() == ' ' || name.back() == '.')
      return false;
    for (unsigned char c : name)
      if (c < 32 || c == 127 || std::string_view("/\\:<>\"|?*").find(c) != std::string_view::npos)
        return false;
    const auto stem = lower(std::string(name.substr(0, name.find('.'))));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
        (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '1' &&
         stem[3] <= '9'))
      return false;
    return true;
  }

  bool Profiles::fail(const std::string &message) {
    m_message = message;
    m_error = true;
    return false;
  }

  void Profiles::startup(const std::filesystem::path &directory) {
    *this = Profiles{};
    m_directory = directory;
    m_folder = directory / "profiles";
    std::string state;
    if (readFile(m_directory / "state.conf", state)) {
      std::istringstream lines(state);
      std::string line;
      while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') // written on Windows in text mode
          line.pop_back();
        const auto equal = line.find(" = ");
        if (equal == line.npos)
          continue;
        auto key = line.substr(0, equal), value = line.substr(equal + 3);
        if (key == "lastProfile" && validName(value))
          m_last = value;
        else if (key == "panel.side")
          m_panel.right = value == "right";
        else if (key == "panel.width") {
          float width = 0;
          if (parseFloat(value, width) && std::isfinite(width))
            m_panel.width = std::clamp(width, 240.f, 1600.f);
        } else if (key == "panel.tab" && value.size() == 1 && value[0] >= '0' && value[0] <= '5')
          m_panel.tab = value[0] - '0';
        else if (key == "panel.menuKey" || key == "panel.menuModifiers") {
          uint32_t number = 0;
          const auto end = value.data() + value.size();
          if (std::from_chars(value.data(), end, number).ptr != end)
            continue;
          if (key == "panel.menuKey" && number >= REMIXAPI_KEY_A && number <= REMIXAPI_KEY_PAGE_DOWN)
            m_panel.menuKey = number;
          else if (key == "panel.menuModifiers" && number <= 15)
            m_panel.menuModifiers = number;
        }
      }
    }
    refresh();
    if (!m_last.empty()) {
      // Do not write state during startup, including a fallback after failure.
      Settings loaded;
      UnknownSettings unknown;
      std::set<std::string> listed;
      if (readProfile(m_last, loaded, unknown, &listed)) {
        m_current = m_saved = loaded;
        m_unknown = std::move(unknown);
        m_listed = std::move(listed);
        m_active = m_last;
        m_persisted = true;
      } else {
        m_message = "Startup profile unavailable; using defaults. " + m_message;
      }
    }
  }

  void Profiles::refresh() {
    clearMessage();
    std::vector<std::string> names;
    std::error_code ec;
    if (!std::filesystem::exists(m_folder, ec) && !ec) {
      m_files.clear();
      return;
    }
    std::filesystem::directory_iterator it(m_folder, ec), end;
    while (!ec && it != end) {
      auto name = utf8(it->path().stem());
      if (it->path().extension() == ".conf" && regular(it->path()) && validName(name))
        names.push_back(name);
      it.increment(ec);
    }
    if (ec) {
      fail("Cannot list profiles: " + ec.message());
      return;
    }
    std::sort(names.begin(), names.end());
    m_files = std::move(names);
  }

  bool Profiles::edit(const Settings &settings) {
    std::string error;
    if (!validate(settings, error))
      return fail(error);
    m_current = settings;
    return true;
  }

  bool Profiles::setDefault(const std::string &key, const std::string &value) {
    Settings check = m_current;
    std::string error;
    if (set(check, key, value) != SetResult::Success || !validate(check, error))
      return false;
    auto known = std::find_if(m_defaults.begin(), m_defaults.end(), [&](const auto &d) { return d.first == key; });
    if (known != m_defaults.end())
      known->second = value;
    else
      m_defaults.emplace_back(key, value);
    if (!m_listed.count(key)) {
      set(m_current, key, value);
      set(m_saved, key, value);
    }
    return true;
  }

  // The host's defaults, where the active profile has no value of its own:
  // in what is saved as well (the profile holds them unchanged).
  void Profiles::applyDefaults() {
    for (const auto &[key, value] : m_defaults)
      if (!m_listed.count(key)) {
        set(m_current, key, value);
        set(m_saved, key, value);
      }
  }

  bool Profiles::readProfile(const std::string &name, Settings &settings, UnknownSettings &unknown,
                             std::set<std::string> *listed) {
    if (!validName(name))
      return fail("Invalid profile name");
    std::string text, error;
    std::vector<std::string> warnings;
    if (!readFile(m_folder / pathFromUtf8(name + ".conf"), text))
      return fail("Cannot read profile: " + name);
    if (!parse(text, settings, unknown, warnings, error))
      return fail(name + ": " + error);
    if (listed) {
      // "key = value" lines (comments and blank lines aside).
      listed->clear();
      auto trim = [](std::string_view v) {
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front())))
          v.remove_prefix(1);
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back())))
          v.remove_suffix(1);
        return v;
      };
      std::string_view rest = text;
      while (!rest.empty()) {
        const auto end = rest.find('\n');
        auto line = trim(rest.substr(0, end));
        rest = end == rest.npos ? std::string_view{} : rest.substr(end + 1);
        const auto equal = line.find('=');
        if (line.empty() || line[0] == '#' || equal == line.npos)
          continue;
        listed->insert(std::string(trim(line.substr(0, equal))));
      }
    }
    clearMessage();
    for (const auto &warning : warnings) {
      if (!m_message.empty())
        m_message += '\n';
      m_message += warning;
    }
    return true;
  }

  bool Profiles::load(const std::string &name) {
    Settings loaded;
    UnknownSettings unknown;
    std::set<std::string> listed;
    if (!readProfile(name, loaded, unknown, &listed))
      return false;
    m_current = m_saved = loaded;
    m_unknown = std::move(unknown);
    m_listed = std::move(listed);
    applyDefaults();
    m_active = m_last = name;
    m_persisted = true;
    m_needsSave = false;
    remember();
    return true;
  }

  bool Profiles::unique(const std::string &name) {
    if (!validName(name))
      return fail("Use a unique file name without path components or reserved characters");
    refresh();
    if (m_error)
      return false;
    for (const auto &file : m_files)
      if (lower(file) == lower(name))
        return fail("A profile with this name already exists");
    std::error_code ec;
    auto status = std::filesystem::symlink_status(m_folder / pathFromUtf8(name + ".conf"), ec);
    if (!ec && std::filesystem::exists(status))
      return fail("A file with this name already exists");
    return true;
  }

  bool Profiles::create(const std::string &name) {
    if (!unique(name))
      return false;
    m_active = name;
    m_current = m_saved = {};
    m_unknown.clear();
    m_listed.clear();
    applyDefaults();
    m_persisted = false;
    m_needsSave = true;
    return true;
  }

  bool Profiles::writeProfile(const std::string &name, bool replace) {
    std::string error;
    if (!validName(name) || !validate(m_current, error))
      return fail("Cannot save profile: " + error);
    if (!atomicWrite(m_folder / pathFromUtf8(name + ".conf"), serialize(m_current, m_unknown), replace,
                     error))
      return fail("Cannot save " + name + ": " + error);
    return true;
  }

  bool Profiles::save() {
    if (!m_persisted && !unique(m_active))
      return false;
    if (!writeProfile(m_active, m_persisted))
      return false;
    m_saved = m_current;
    m_last = m_active;
    m_persisted = true;
    m_needsSave = false;
    refresh();
    remember();
    return true;
  }

  bool Profiles::duplicate(const std::string &name) {
    if (lower(name) == lower(m_active))
      return fail("Choose a different profile name");
    if (!unique(name) || !writeProfile(name, false))
      return false;
    m_active = m_last = name;
    m_saved = m_current;
    m_persisted = true;
    m_needsSave = false;
    refresh();
    remember();
    return true;
  }

  void Profiles::revert() {
    m_current = m_saved;
    clearMessage();
  }

  void Profiles::remember() {
    std::string error;
    const auto text = "# OpenRemix session state (not rendering settings)\nlastProfile = " + m_last +
                      "\npanel.side = " + (m_panel.right ? "right" : "left") +
                      "\npanel.width = " + std::to_string(m_panel.width) +
                      "\npanel.tab = " + std::to_string(m_panel.tab) +
                      "\npanel.menuKey = " + std::to_string(m_panel.menuKey) +
                      "\npanel.menuModifiers = " + std::to_string(m_panel.menuModifiers) + "\n";
    if (!atomicWrite(m_directory / "state.conf", text, true, error)) {
      if (!m_message.empty())
        m_message += '\n';
      m_message += "Settings applied, but session state could not be saved: " + error;
      m_error = true;
    }
  }

  void Profiles::savePanel(const PanelState &panel) {
    if (panel == m_panel && !m_panelDirty)
      return;
    m_panel = panel;
    m_panelDirty = false;
    remember();
  }
} // namespace remix::config

#include "ui/sky.h"
#include "runtime/sky_quality.h"
#include "ui/sky_ranges.h"
#include "ui/widgets.h"
#include "scene/particles.h"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <imgui.h>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#if REMIX_HAS_NUMOS
#include <numos/seed.h>
#include <numos/settings.h>
#include <numos/weather.h>
#endif

namespace remix::ui {
#if REMIX_HAS_NUMOS
  namespace {
    constexpr std::string_view prefix = "rtx.atmosphere.";

    std::string_view shortKey(const numos::SettingInfo &info) {
      std::string_view key = info.key;
      return key.starts_with(prefix) ? key.substr(prefix.size()) : key;
    }

    // Groups, in display order: a setting goes to the first group whose key
    // prefixes match.
    const char *const groups[] = {"World and units",   "Sun and time",       "Atmosphere",     "Clouds",
                                  "Cloud layer 2",     "Cloud shadows",      "Cloud quality",  "Lightning",
                                  "Aerial perspective", "Night sky and moon", "Moons",          "Caching and debug"};
    int groupOf(std::string_view key) {
      const auto any = [&](std::initializer_list<std::string_view> prefixes) {
        return std::any_of(prefixes.begin(), prefixes.end(), [&](auto p) { return key.starts_with(p); });
      };
      if (key.starts_with("moon") && key.size() > 5 && key[5] == '.')
        return 10;
      if (any({"unitsPerMeter", "worldScale", "hazeWorldScale", "groundLevel", "groundSmoothing",
               "cloudVerticalOffset", "useCameraWorldOverride", "cameraWorldOverride", "cloudAnchorCut", "seaLevel",
               "altitudeScale", "viewAltitude", "flipUpAxis"}))
        return 0;
      if (any({"sun", "timeCycle", "timeOfDay", "dayLength", "dayOfYear", "calendarYear", "latitude",
               "northOffset", "directionalLight"}))
        return 1;
      if (any({"debug", "skyView", "skyLut", "cloudProfiling"}))
        return 11;
      if (any({"lightning"}))
        return 7;
      if (any({"aerialPerspective", "fog"}))
        return 8;
      if (any({"night", "star", "realMoon"}))
        return 9;
      if (any({"cloudLayer2"}))
        return 4;
      if (any({"cloudShadow", "cloudVoxelShadows", "cloudVoxelGrid", "cloudSkyAmbient"}))
        return 5;
      if (any({"cloudTemporal", "cloudHistory", "cloudSpatial", "cloudMarch", "cloudPerformance",
               "cloudViewSamples", "cloudViewStep", "cloudSecondary", "cloudDetailLod", "cloudLightingLod",
               "nvdfStepScale", "nubis3AdaptiveStep", "nubis3Jitter", "cloudRenderRT"}))
        return 6;
      if (any({"cloud", "nvdf", "nubis3"}))
        return 3;
      return 2;
    }

    bool bounded(float value) { return value != numos::NUMOS_NO_MIN && value != numos::NUMOS_NO_MAX; }

    bool isColour(std::string_view key) {
      return key.find("Color") != key.npos || key.find("Tint") != key.npos || key.find("Albedo") != key.npos;
    }

    // The NUMOS settings the profile describes: defaults overlaid with its
    // explicit values.
    numos::Settings effective(const config::SkySettings &sky) {
      numos::Settings result;
      for (const auto &[key, value] : sky.atmosphere)
        numos::setSetting(result, key, value);
      return result;
    }

    // Writes one edited value back to the profile.
    void store(config::SkySettings &sky, const numos::Settings &values, const numos::SettingInfo &info) {
      sky.atmosphere[info.key] = numos::formatSetting(values, info);
    }

    const SkySettingRange *curatedRange(std::string_view key) {
      // rtx.atmosphere.moon<N>.field shares the "moon.field" entry.
      std::string moon;
      if (const auto dot = key.find('.'); key.starts_with("moon") && dot != key.npos)
        key = moon.assign("moon").append(key.substr(dot));
      for (const auto &range : skySettingRanges)
        if (range.key == key)
          return &range;
      return nullptr;
    }

    // A curated control: a slider over the range, logarithmic where it spans
    // decades, or a clamped drag where the range is too wide to slide.
    bool curatedWidget(void *field, numos::SettingType type, const SkySettingRange &range, const char *label) {
      const float lo = range.minValue, hi = range.maxValue;
      const bool slide = hi - lo <= 1e5f;
      ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp;
      if (lo > 0 && hi / lo >= 1000)
        flags |= ImGuiSliderFlags_Logarithmic;
      switch (type) {
      case numos::SettingType::Int:
      case numos::SettingType::Uint: {
        const bool isInt = type == numos::SettingType::Int;
        auto value = isInt ? int64_t(*static_cast<int32_t *>(field)) : int64_t(*static_cast<uint32_t *>(field));
        const int64_t min = int64_t(lo), max = int64_t(hi);
        const bool changed = slide ? ImGui::SliderScalar(label, ImGuiDataType_S64, &value, &min, &max, nullptr, flags)
                                   : ImGui::DragScalar(label, ImGuiDataType_S64, &value, range.step, &min, &max,
                                                       nullptr, flags);
        if (changed && isInt)
          *static_cast<int32_t *>(field) = int32_t(value);
        else if (changed)
          *static_cast<uint32_t *>(field) = uint32_t(value);
        return changed;
      }
      case numos::SettingType::Float: {
        auto &stored = *static_cast<float *>(field);
        const float unit = range.metersPerSecond ? 1000.f : 1.f;
        float value = stored * unit;
        const char *format = range.format ? range.format : "%.5g";
        if (!(slide ? ImGui::SliderFloat(label, &value, lo, hi, format, flags)
                    : ImGui::DragFloat(label, &value, range.step, lo, hi, format, flags)))
          return false;
        stored = value / unit;
        return true;
      }
      case numos::SettingType::Vec3:
        return ImGui::DragFloat3(label, static_cast<float *>(field), range.step, lo, hi,
                                 range.format ? range.format : "%.5g", ImGuiSliderFlags_AlwaysClamp);
      default:
        return false;
      }
    }

    bool settingWidget(numos::Settings &values, const numos::SettingInfo &info, const char *label) {
      auto *field = reinterpret_cast<uint8_t *>(&values) + info.offset;
      const bool low = bounded(info.minValue), high = bounded(info.maxValue);
      if (const auto *range = curatedRange(shortKey(info)); range && info.type != numos::SettingType::Bool)
        return curatedWidget(field, info.type, *range, label);
      switch (info.type) {
      case numos::SettingType::Bool:
        return ImGui::Checkbox(label, reinterpret_cast<bool *>(field));
      case numos::SettingType::Int: {
        const int32_t lo = low ? int32_t(info.minValue) : INT32_MIN / 2, hi = high ? int32_t(info.maxValue) : INT32_MAX / 2;
        return low && high && hi - lo <= 4096
                   ? ImGui::SliderScalar(label, ImGuiDataType_S32, field, &lo, &hi, nullptr, ImGuiSliderFlags_AlwaysClamp)
                   : ImGui::DragScalar(label, ImGuiDataType_S32, field, .2f, &lo, &hi, nullptr,
                                       ImGuiSliderFlags_AlwaysClamp);
      }
      case numos::SettingType::Uint: {
        const uint32_t lo = low ? uint32_t(std::max(info.minValue, 0.f)) : 0u,
                       hi = high ? uint32_t(info.maxValue) : UINT32_MAX / 2;
        return low && high && hi - lo <= 4096
                   ? ImGui::SliderScalar(label, ImGuiDataType_U32, field, &lo, &hi, nullptr, ImGuiSliderFlags_AlwaysClamp)
                   : ImGui::DragScalar(label, ImGuiDataType_U32, field, .2f, &lo, &hi, nullptr,
                                       ImGuiSliderFlags_AlwaysClamp);
      }
      case numos::SettingType::Float: {
        auto &value = *reinterpret_cast<float *>(field);
        if (low && high) {
          ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp;
          if (info.minValue > 0 && info.maxValue / info.minValue >= 1000)
            flags |= ImGuiSliderFlags_Logarithmic;
          return ImGui::SliderFloat(label, &value, info.minValue, info.maxValue, "%.5g", flags);
        }
        return ImGui::DragFloat(label, &value, std::max(std::abs(value) * .005f, .001f), low ? info.minValue : -FLT_MAX,
                                high ? info.maxValue : FLT_MAX, "%.5g",
                                low || high ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None);
      }
      case numos::SettingType::Vec3: {
        auto *value = reinterpret_cast<float *>(field);
        const bool changed = isColour(shortKey(info))
                                 ? ImGui::ColorEdit3(label, value, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)
                                 : ImGui::DragFloat3(label, value, .01f, 0, 0, "%.5g");
        for (int i = 0; changed && i < 3; ++i)
          value[i] = std::clamp(value[i], low ? info.minValue : -FLT_MAX, high ? info.maxValue : FLT_MAX);
        return changed;
      }
      }
      return false;
    }

    // A setting's display name: skySettingLabels, or its key as words with a
    // unit suffix as the unit ("cloudBaseHeightMeters": "Cloud base height
    // (m)"). Moon settings are prefixed with their moon.
    std::string settingLabel(const numos::SettingInfo &info) {
      std::string_view key = shortKey(info);
      std::string moon, lookup(key);
      if (const auto dot = key.find('.'); key.starts_with("moon") && dot != key.npos) {
        moon = "Moon " + std::string(key.substr(4, dot - 4)) + ": ";
        lookup = "moon" + std::string(key.substr(dot));
        key = key.substr(dot + 1);
      }
      for (const auto &entry : skySettingLabels)
        if (entry.key == lookup)
          return moon + std::string(entry.label);
      std::vector<std::string> words;
      for (size_t i = 0; i < key.size(); ++i) {
        const char c = key[i], previous = i ? key[i - 1] : 0;
        const bool upper = std::isupper(uint8_t(c)), digit = std::isdigit(uint8_t(c));
        if (words.empty() || (upper && (std::islower(uint8_t(previous)) || std::isdigit(uint8_t(previous)))) ||
            (digit && !std::isdigit(uint8_t(previous))))
          words.emplace_back();
        words.back() += c;
      }
      static const std::pair<std::string_view, std::string_view> units[] = {
          {"Meters", "m"}, {"Km", "km"},          {"Deg", "deg"},       {"Degrees", "deg"},
          {"Hours", "h"},  {"Minutes", "min"},    {"Seconds", "s"},     {"Stops", "stops"},
          {"Pixels", "pixels"}};
      static const std::string_view acronyms[] = {"LUT", "LOD", "SDF", "NVDF", "RT", "AP"};
      std::string unit;
      if (words.size() > 2 && words[words.size() - 2] == "World" && words.back() == "Units") {
        unit = "world units";
        words.resize(words.size() - 2);
      } else if (words.size() > 1)
        for (const auto &[suffix, name] : units)
          if (words.back() == suffix) {
            unit = name;
            words.pop_back();
            break;
          }
      std::string label = moon;
      for (size_t i = 0; i < words.size(); ++i) {
        auto word = words[i];
        for (auto &ch : word)
          ch = char(std::tolower(uint8_t(ch)));
        const auto acronym = std::find_if(std::begin(acronyms), std::end(acronyms), [&](std::string_view entry) {
          return entry.size() == word.size() && std::equal(entry.begin(), entry.end(), word.begin(), [](char a, char b) {
                   return std::tolower(uint8_t(a)) == b;
                 });
        });
        if (acronym != std::end(acronyms))
          word = *acronym;
        else if (i == 0)
          word[0] = char(std::toupper(uint8_t(word[0])));
        label += (i ? " " : "") + word;
      }
      return unit.empty() ? label : label + " (" + unit + ")";
    }

    // Remix Plus's note on a setting, or null.
    const char *settingNote(const numos::SettingInfo &info) {
      for (const auto &entry : skySettingNotes)
        if (entry.key == shortKey(info))
          return entry.label.data();
      return nullptr;
    }

    // One row: the widget with the description as tooltip and, for a value
    // the profile sets, a reset button.
    void settingRow(config::SkySettings &sky, numos::Settings &values, const numos::SettingInfo &info) {
      ImGui::PushID(info.key.c_str());
      const std::string label = settingLabel(info);
      const bool set = sky.atmosphere.contains(info.key);
      if (field(label.c_str(), [&](const char *id) { return settingWidget(values, info, id); }))
        store(sky, values, info);
      if (ImGui::IsItemHovered() && info.description) {
        if (const char *note = settingNote(info))
          ImGui::SetItemTooltip("%s\n\n%s\n\n%s", info.key.c_str(), info.description, note);
        else
          ImGui::SetItemTooltip("%s\n\n%s", info.key.c_str(), info.description);
      }
      if (set) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset"))
          sky.atmosphere.erase(info.key);
      }
      ImGui::PopID();
    }

    void drawSetting(config::SkySettings &sky, numos::Settings &values, std::string_view key) {
      if (const auto *info = numos::findSetting(std::string(prefix) + std::string(key)))
        settingRow(sky, values, *info);
    }

    int qualityOf(const numos::Settings &values) {
      for (int i = 0; i < int(std::size(config::skyQualities)); ++i)
        if (std::all_of(config::skyQualities[i].values.begin(), config::skyQualities[i].values.end(), [&](auto &v) {
              const auto *info = numos::findSetting(v.key);
              return info && numos::formatSetting(values, *info) == v.value;
            }))
          return i;
      return int(std::size(config::skyQualities));
    }

    void drawQuality(config::SkySettings &sky, const numos::Settings &values) {
      int quality = qualityOf(values);
      if (Combo("Quality", &quality, "Low\0Medium\0High\0Ultra\0Custom (configuration)\0") &&
          quality < int(std::size(config::skyQualities)))
        for (const auto &v : config::skyQualities[quality].values)
          sky.atmosphere[std::string(v.key)] = std::string(v.value);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Cloud march rate and steps, the cloud shadow shortcut and the near haze's grid.\n"
                          "High is NUMOS's defaults; Custom means the profile sets other values.");
    }

    void drawWeather(config::SkySettings &sky, numos::Settings &values) {
      static int preset = int(numos::WeatherPreset::PartlyCloudy);
      const auto name = [](void *, int i) { return numos::weatherPresetName(numos::WeatherPreset(i)); };
      ImGui::PushItemWidth(ImGui::CalcItemWidth() * .7f);
      ImGui::Combo("##weather preset", &preset, name, nullptr, int(numos::WeatherPreset::Count));
      ImGui::PopItemWidth();
      ImGui::SameLine();
      if (ImGui::Button("Apply weather")) {
        auto next = values;
        numos::applyWeather(numos::weatherPreset(numos::WeatherPreset(preset), values), next);
        for (const auto &info : numos::settingInfos())
          if (!numos::isDeprecated(info) && numos::formatSetting(next, info) != numos::formatSetting(values, info))
            store(sky, next, info);
        values = next;
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Writes the preset's weather into the profile: cloud cover and type, layers, wind,\n"
                          "air, fog and lightning. The game can blend weather over time through the API.");
      size_t count = 0;
      const auto *fields = numos::weatherFields(count);
      if (ImGui::TreeNode("Weather fields")) {
        for (size_t i = 0; i < count; ++i)
          drawSetting(sky, values, fields[i].name);
        ImGui::TreePop();
      }
    }

    void drawSeeds(config::SkySettings &sky, const numos::Settings &values) {
      static char seed[4096] = {};
      static std::string message;
      if (ImGui::Button("Copy seed")) {
        const auto text = numos::encodeSeed(values);
        ImGui::SetClipboardText(text.c_str());
        message = "Copied to the clipboard.";
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(std::max(1.f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Apply").x -
                                                ImGui::GetStyle().FramePadding.x * 4));
      ImGui::InputTextWithHint("##seed", "NUMOS1-...", seed, sizeof(seed));
      ImGui::SameLine();
      if (ImGui::Button("Apply")) {
        numos::SeedContents contents;
        std::string error;
        if (numos::decodeSeed(seed, contents, &error)) {
          // A seed replaces the configuration: every setting not in it is a default.
          sky.atmosphere.clear();
          static const numos::Settings defaults;
          for (const auto &info : numos::settingInfos())
            if (!numos::isDeprecated(info) &&
                numos::formatSetting(contents.settings, info) != numos::formatSetting(defaults, info))
              store(sky, contents.settings, info);
          message = contents.unknownEntries ? std::to_string(contents.unknownEntries) +
                                                  " entries of the seed are unknown to this NUMOS and were skipped."
                                            : "Applied.";
        } else
          message = "Not a valid seed: " + error;
      }
      if (!message.empty())
        ImGui::TextWrapped("%s", message.c_str());
    }

    void drawAllSettings(config::SkySettings &sky, numos::Settings &values) {
      static ImGuiTextFilter filter;
      static bool changedOnly = false;
      filter.Draw("Search");
      Checkbox("Only settings the profile sets", &changedOnly);
      const auto &infos = numos::settingInfos();
      // The groups share names with the tab's own headers.
      ImGui::PushID("All NUMOS settings");
      for (int group = 0; group < int(std::size(groups)); ++group) {
        std::vector<const numos::SettingInfo *> rows;
        for (const auto &info : infos)
          if (!numos::isDeprecated(info) && groupOf(shortKey(info)) == group &&
              (!changedOnly || sky.atmosphere.contains(info.key)) &&
              (filter.PassFilter(info.key.c_str()) || filter.PassFilter(settingLabel(info).c_str())))
            rows.push_back(&info);
        if (rows.empty())
          continue;
        const std::string header = std::string(groups[group]) + " (" + std::to_string(rows.size()) + ")###" + groups[group];
        if (ImGui::TreeNode(header.c_str())) {
          for (const auto *info : rows)
            settingRow(sky, values, *info);
          ImGui::TreePop();
        }
      }
      ImGui::PopID();
    }
  } // namespace
#endif

  static void drawSky(config::Settings &settings, const SkyStatus &status) {
    auto &sky = settings.sky;
#if REMIX_HAS_NUMOS
    if (ImGui::CollapsingHeader("Sky", ImGuiTreeNodeFlags_DefaultOpen)) {
      Checkbox("NUMOS sky", &sky.enabled);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Physically based sky, atmosphere and volumetric clouds, with weather and night sky.");
      if (!status.supported)
        ImGui::TextWrapped("%s", status.reason.empty() ? "This backend does not render the sky." : status.reason.c_str());
      else if (sky.enabled && !status.active && !status.reason.empty())
        ImGui::TextWrapped("Inactive: %s", status.reason.c_str());
      else if (status.active && status.gpuMilliseconds > 0)
        ImGui::Text("Active, GPU %.2f ms", status.gpuMilliseconds);
      else if (status.active) {
        ImGui::TextUnformatted("Active");
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Turn on GPU pass timers (Diagnostics) to see the sky's GPU time here.");
      }
      int gameSky = int(sky.gameSky);
      if (Combo("Game sky", &gameSky, "Hidden\0Under clouds and air\0Over the sky\0"))
        sky.gameSky = config::SkySettings::GameSky(gameSky);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hidden: NUMOS draws in place of the game's sky geometry.\n"
                          "Under: the game's sky shows behind NUMOS's clouds and air.\n"
                          "Over: the game's sky covers NUMOS's sky.");
      // The same setting as Lighting > Volumetrics > Up axis; the sky needs Y or Z.
      const uint32_t axes[] = {3, 1, 2};
      int upAxis = settings.trace.volumetricUpAxis == 1 ? 1 : settings.trace.volumetricUpAxis == 2 ? 2 : 0;
      if (Combo("World up axis", &upAxis, "Auto (from the camera)\0Y\0Z\0"))
        settings.trace.volumetricUpAxis = axes[upAxis];
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The game world's up axis. A wrong one turns the sky on its side.");
      auto values = effective(sky);
      drawQuality(sky, values);
      DragFloat("Radiance scale", &sky.radianceScale, sky.radianceScale * .01f, 1e-6f, 1e6f, "%.4g",
                ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scale from NUMOS's physical radiance to the renderer's lighting units.");
      SliderFloat("Raster exposure", &sky.rasterExposure, .01f, 100.f, "%.3g", ImGuiSliderFlags_Logarithmic);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Exposure of the sky without the path tracer, which has no tonemapper.");
      Checkbox("Ignore game sky control", &sky.ignoreGameControl);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Keep this tab's time and weather when the game sets them through the API.");
    }
    if (!sky.enabled)
      return;
    auto values = effective(sky);
    if (ImGui::CollapsingHeader("Sun and time", ImGuiTreeNodeFlags_DefaultOpen)) {
      for (const auto key : {"timeCycleEnable", "timeOfDayHours", "dayLengthMinutes", "dayOfYear", "latitudeDegrees",
                             "northOffsetDegrees", "sunElevation", "sunRotation", "sunIntensity", "sunSize"})
        drawSetting(sky, values, key);
    }
    if (ImGui::CollapsingHeader("Weather", ImGuiTreeNodeFlags_DefaultOpen))
      drawWeather(sky, values);
    if (ImGui::CollapsingHeader("All NUMOS settings"))
      drawAllSettings(sky, values);
    if (ImGui::CollapsingHeader("Seeds"))
      drawSeeds(sky, values);
    ImGui::Spacing();
    ImGui::TextDisabled("Sky and clouds: NUMOS by Kim2091 (Sparkles)");
#else
    (void)sky;
    (void)status;
    ImGui::TextWrapped("This build has no NUMOS sky (third_party/numos/numos-sky was not checked out).");
#endif
  }

  // Particle emitters: the API's and these test emitters. Nothing spawns
  // unless an emitter asks.
  static void drawParticles(config::ParticleSettings &particles) {
    if (!ImGui::CollapsingHeader("Particles"))
      return;
    ImGui::PushID("Particles");
    const auto statistics = scene::particles().statistics();
    Checkbox("Particles", &particles.enabled);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Simulate and draw particle emitters (rain, snow and the API's emitters).");
    if (particles.enabled && !statistics.enabled && statistics.simulatedFrames == 0)
      ImGui::TextWrapped("This backend does not draw particles.");
    int budget = int(particles.budget), spawnBudget = int(particles.spawnBudget);
    if (SliderInt("Particle budget", &budget, 1024, 262144, "%d", ImGuiSliderFlags_Logarithmic))
      particles.budget = uint32_t(std::clamp(budget, 1024, 262144));
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Live particles of all emitters. A change clears every emitter.");
    if (SliderInt("Spawns per frame", &spawnBudget, 64, 65536, "%d", ImGuiSliderFlags_Logarithmic))
      particles.spawnBudget = uint32_t(std::clamp(spawnBudget, 64, 65536));
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("New particles a frame, shared by the emitters; the rest are dropped.");
    SliderFloat("Precipitation", &particles.precipitation, 0.f, 1.f, "%.2f");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Scales the rate of emitters that follow the weather while the NUMOS sky is off;\n"
                        "with the sky on, its weather's precipitation does.");
    ImGui::SeparatorText("Test emitters");
    Checkbox("Rain", &particles.rain);
    ImGui::SameLine();
    Checkbox("Snow", &particles.snow);
    SliderFloat("Intensity", &particles.intensity, 0.f, 1.f, "%.2f");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("The test emitters' share of their presets' spawn rates.");
    Checkbox("Shelter", &particles.shelter);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Keep rain and snow out from under cover and stop them on surfaces (path tracing).");
    Checkbox("Follow weather", &particles.followWeather);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Scale the test emitters by the precipitation.");
    ImGui::SeparatorText("Statistics");
    ImGui::Text("Emitters: %u (%u active)", statistics.emitters, statistics.activeEmitters);
    ImGui::Text("Live: %u of %u allocated, budget %u", statistics.liveParticles, statistics.allocatedParticles,
                statistics.budget);
    ImGui::Text("Spawns: %u, dropped %u, sheltered %u", statistics.spawnsRequested, statistics.spawnsDropped,
                statistics.spawnsSheltered);
    if (statistics.gpuMilliseconds > 0)
      ImGui::Text("GPU %.2f ms", statistics.gpuMilliseconds);
    ImGui::PopID();
  }

  void drawSkyTab(config::Settings &settings, const SkyStatus &status) {
    drawSky(settings, status);
    drawParticles(settings.particles);
  }
} // namespace remix::ui

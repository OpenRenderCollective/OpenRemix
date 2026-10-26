#include "runtime/settings.h"
#include "runtime/float_chars.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <type_traits>
#if REMIX_HAS_NUMOS
#include <numos/settings.h>
#endif

namespace remix::config {
  namespace {
    struct Entry {
      std::string key;
      std::function<std::string(const Settings &)> read;
      std::function<bool(Settings &, std::string_view)> write;
    };
    template <typename T> std::string number(T value) {
      if constexpr (std::is_floating_point_v<T>)
        return formatFloat(value);
      else {
        char buffer[64];
        auto result = std::to_chars(buffer, buffer + sizeof(buffer), unsigned(value));
        return {buffer, result.ptr};
      }
    }
    template <typename T> bool readNumber(std::string_view text, T &value) {
      if constexpr (std::is_unsigned_v<T>) {
        // Retain the configuration API's historical integer/boolean aliases.
        if (text == "true") {
          value = 1;
          return true;
        }
        if (text == "false") {
          value = 0;
          return true;
        }
      }
      if constexpr (std::is_floating_point_v<T>)
        return parseFloat(text, value);
      else {
        auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size();
      }
    }
    template <typename Access>
    Entry scalar(std::string key, Access access, double low, double high, double gap = 0,
                 bool sentinel = false) {
      return {std::move(key), [=](const Settings &s) { return number(access(s)); },
              [=](Settings &s, std::string_view text) {
                using T = std::remove_cvref_t<decltype(access(s))>;
                using Parsed = std::conditional_t<std::is_same_v<T, bool>, unsigned, T>;
                Parsed value{};
                if (!readNumber(text, value) || !std::isfinite(double(value)) ||
                    (sentinel && double(value) == -1
                         ? false
                         : value < Parsed(low) || value > Parsed(high) || (value > 0 && value < Parsed(gap))))
                  return false;
                access(s) = T(value);
                return true;
              }};
    }
    template <typename Access>
    Entry choices(std::string key, Access access, std::vector<std::string> values) {
      return {std::move(key),
              [=](const Settings &s) {
                size_t index = size_t(access(s));
                return index < values.size() ? values[index] : "invalid";
              },
              [=](Settings &s, std::string_view text) {
                auto found = std::find(values.begin(), values.end(), text);
                if (found == values.end())
                  return false;
                access(s) = std::remove_cvref_t<decltype(access(s))>(found - values.begin());
                return true;
              }};
    }
    // Stores one NUMOS setting in its normalised form; without NUMOS, as given.
    SetResult setAtmosphere(std::map<std::string, std::string> &atmosphere, std::string_view key,
                            std::string_view value) {
#if REMIX_HAS_NUMOS
      const auto *info = numos::findSetting(key);
      if (!info)
        return SetResult::Unknown;
      numos::Settings scratch;
      if (!numos::setSetting(scratch, key, value))
        return SetResult::Invalid;
      atmosphere[std::string(key)] = numos::formatSetting(scratch, *info);
#else
      atmosphere[std::string(key)] = std::string(value);
#endif
      return SetResult::Success;
    }
    // Moves deprecated NUMOS settings into their replacements, once per load.
    void migrateAtmosphere(Settings &settings, std::vector<std::string> &notices) {
#if REMIX_HAS_NUMOS
      auto &atmosphere = settings.sky.atmosphere;
      bool deprecated = false;
      for (const auto &[key, value] : atmosphere)
        deprecated |= numos::isDeprecated(*numos::findSetting(key));
      if (!deprecated)
        return;
      std::string text;
      for (const auto &[key, value] : atmosphere)
        text += key + " = " + value + "\n";
      numos::Settings before, after;
      numos::ExplicitSettings explicitlySet;
      numos::applyConfig(before, text);
      numos::applyConfig(after, text, &explicitlySet);
      // NUMOS's legacy scale is Remix's rtx.sceneScale: 100 x it is game units per metre.
      numos::migrateDeprecated(after, explicitlySet, .01f / settings.trace.sceneScale);
      for (const auto &info : numos::settingInfos()) {
        if (numos::isDeprecated(info)) {
          if (atmosphere.erase(info.key))
            notices.push_back("Migrated deprecated setting: " + info.key);
        } else if (numos::formatSetting(after, info) != numos::formatSetting(before, info))
          atmosphere[info.key] = numos::formatSetting(after, info);
      }
#else
      (void)settings;
      (void)notices;
#endif
    }
    std::string_view trim(std::string_view text) {
      const auto start = text.find_first_not_of(" \t\r");
      if (start == text.npos)
        return {};
      return text.substr(start, text.find_last_not_of(" \t\r") - start + 1);
    }
    const std::vector<Entry> &registry() {
      static const std::vector<Entry> entries = [] {
        std::vector<Entry> result;
#define FIELD(member) [](auto &s) -> auto & { return s.member; }
#define TRACE(member, low, high)                                                                             \
  result.push_back(scalar("rtx.native.pathtrace." #member, FIELD(trace.member), low, high))
        result.push_back(choices("rtx.native.backend", FIELD(backend), {"raster", "pathtrace"}));
        result.push_back({"rtx.native.vsync", [](const Settings &s) { return number(s.vsync); },
                          [](Settings &s, std::string_view value) {
                            std::string lower(value);
                            for (auto &c : lower)
                              if (c >= 'A' && c <= 'Z')
                                c += 'a' - 'A';
                            if (lower != "0" && lower != "1" && lower != "true" && lower != "false")
                              return false;
                            s.vsync = lower == "1" || lower == "true";
                            return true;
                          }});
        result.push_back(choices("rtx.native.pathtrace.upscaler", FIELD(trace.upscaler),
                                 {"off", "auto", "fsr31", "dlss"}));
        result.push_back(choices("rtx.native.pathtrace.dlssRayReconstruction",
                                 FIELD(trace.dlssRayReconstruction), {"0", "1"}));
        result.push_back(choices("rtx.native.pathtrace.dlssRayReconstructionModel",
                                 FIELD(trace.dlssRayReconstructionModel), {"transformer", "cnn"}));
        result.push_back(choices("rtx.native.pathtrace.dlssRayReconstructionPresetOverride",
                                 FIELD(trace.dlssRayReconstructionPresetOverride), {"0", "1"}));
        result.push_back(choices("rtx.native.pathtrace.dlssRayReconstructionPreset",
                                 FIELD(trace.dlssRayReconstructionPreset), {"default", "A", "B", "C", "D", "E", "F"}));
        result.push_back(
            choices("rtx.native.pathtrace.primaryVisibility", FIELD(trace.rasterPrimary), {"ray", "raster"}));
        result.push_back(
            choices("rtx.native.pathtrace.denoiser", FIELD(trace.denoiser), {"off", "reblur-sh"}));
        result.push_back(choices("rtx.native.pathtrace.fsrQuality", FIELD(trace.fsrQuality),
                                 {"native-aa", "quality", "balanced", "performance", "ultra-performance"}));
        result.push_back(choices("rtx.native.pathtrace.dlssQuality", FIELD(trace.dlssQuality),
                                 {"native-aa", "quality", "balanced", "performance", "ultra-performance"}));
        TRACE(samples, 1, 16);
        TRACE(bounces, 0, 16);
        TRACE(russianRoulette, 0, 1);
        TRACE(rouletteMinBounces, 1, 16);
        TRACE(accumulate, 0, 1);
        TRACE(sigma, 0, 1);
        TRACE(blueNoiseShadows, 0, 1);
        TRACE(blueNoiseLocalLights, 0, 1);
        TRACE(blueNoiseSky, 0, 1);
        TRACE(autoExposure, 0, 1);
        TRACE(exposure, .05, 8);
        TRACE(autoExposureTarget, .02, .6);
        TRACE(autoExposureSpeed, .1, 16);
        TRACE(autoExposureCentre, 0, 1);
        TRACE(autoExposureMinEv, -24, 0);
        TRACE(autoExposureMaxEv, 0, 24);
        TRACE(bloom, 0, 1);
        TRACE(bloomIntensity, 0, 1);
        TRACE(bloomRadius, 0, 1);
        TRACE(contrast, 0, 1);
        TRACE(contrastAmount, .5, 2);
        TRACE(localLights, 0, 1);
        TRACE(torchLights, 0, 1);
        TRACE(emissiveLights, 0, 1);
        TRACE(emissiveClusterSize, 1, 64);
        TRACE(adaptiveLocalSampling, 0, 1);
        TRACE(stabilizeEmissiveLighting, 0, 1);
        TRACE(emissiveSolidAngleSampling, 0, 1);
        TRACE(freezeEmissiveRadiance, 0, 1);
        TRACE(emissiveCandidates, 1, 8);
        TRACE(sphereCandidates, 1, 8);
        TRACE(separateLocalLighting, 0, 3);
        TRACE(localFireflyFilter, 0, 1);
        TRACE(localDenoiser, 0, 1);
        TRACE(localGuiding, 0, 1);
        TRACE(localGuideCellSize, .05, 100);
        TRACE(localAntilagSigma, .1, 16);
        TRACE(localAntilagSensitivity, .1, 16);
        TRACE(localHistoryFrames, 2, 64);
        TRACE(localHistoryConfidence, 0, 1);
        TRACE(movingShadowBlur, .01, 1);
        TRACE(translucentIndirect, 0, 1);
        TRACE(skipBlendedSecondaryRays, 0, 1);
        TRACE(viewModel, 0, 1);
        TRACE(viewModelScale, .05f, 1);
        TRACE(primarySurfaceReplacement, 0, 1);
        TRACE(sheetLocalLights, 0, 1);
        TRACE(layerReuse, 0, 1);
        TRACE(shortSheetPaths, 0, 1);
        TRACE(sceneScale, .0001, 1000);
        TRACE(lightVisibilityCulling, 0, 1);
        TRACE(cameraBackFaceCulling, 0, 1);
        TRACE(cameraVisibilityCulling, 0, 1);
        TRACE(restir, 0, 1);
        TRACE(regir, 0, 1);
        TRACE(regirCellSize, .125f, 16);
        TRACE(sparseRendering, 0, 1);
        TRACE(sparseAdaptiveRate, 0, 1);
        TRACE(sparseSamplingRate, .0625f, 1);
        TRACE(sparseResolutionExponent, 0, 1);
        TRACE(sparseQuality, 0, 1);
        TRACE(sparseMaxRate, .0625f, 1);
        TRACE(volumetrics, 0, 1);
        TRACE(volumetricOverride, 0, 1);
        TRACE(volumetricDistance, .01f, 100000);
        TRACE(volumetricHeightFalloff, 0, 100000);
        TRACE(volumetricBaseHeight, -100000, 100000);
        TRACE(volumetricUpAxis, 0, 3);
        TRACE(volumetricScattering, 0, 100);
        TRACE(volumetricSunScale, 0, 16);
        TRACE(volumetricLightScale, 0, 16);
        TRACE(volumetricEmissiveScale, 0, 16);
        TRACE(volumetricSkyScale, 0, 16);
        TRACE(volumetricMaxDistance, 1, 100000);
        TRACE(volumetricCellSize, .02f, 16);
        TRACE(volumetricCells, 16, 96);
        TRACE(volumetricLevels, 1, 6);
        TRACE(volumetricUpdateInterval, 1, 64);
        TRACE(volumetricHistory, 1, 256);
        TRACE(volumetricSteps, 1, 64);
        TRACE(orca, 0, 1);
        TRACE(orcaTile, 2, 8);
        TRACE(orcaCellSize, .01f, 4);
        TRACE(orcaLevels, 1, 8);
        TRACE(orcaMinSamples, 1, 64);
        TRACE(orcaMaxError, 0, 4);
        TRACE(indirectHistoryFrames, 0, 63);
        TRACE(skySamples, 1, 16);
        TRACE(sheetSkySamples, 1, 16);
        TRACE(skyGuiding, 0, 1);
        TRACE(skyGuideCellSize, .05, 100);
        TRACE(skyGuideShare, 0, .95);
        TRACE(taaMotionRate, .05, 1);
        TRACE(movingGradients, 0, 1);
        TRACE(movingLightTolerance, .001, 10);
        TRACE(movingLightScale, .01, 1000);
        TRACE(movingGradientNoise, 0, 100);
        TRACE(movingHistoryStart, 0, 1000);
        TRACE(movingHistoryEnd, 0, 1000);
        TRACE(movingIndirectHistory, 0, 1);
        TRACE(movingNeighborHistory, 0, 1);
        TRACE(movingLocalHistory, 0, 1);
        TRACE(restirCandidates, 1, 32);
        TRACE(restirSpatialSamples, 0, 8);
        TRACE(restirSpatialRadius, 1, 100);
        TRACE(restirTemporalHistory, 0, 64);
        TRACE(restirInitialVisibility, 0, 1);
        TRACE(restirPermutation, 0, 1);
        TRACE(localSamples, 1, 16);
        TRACE(shadowDebug, 0, 2);
        TRACE(debugView, 0, 14);
        TRACE(taa, 0, 1);
        TRACE(waterIndirectReuse, 0, 1);
        TRACE(caustics, 0, 1);
        TRACE(causticsStrength, 0, 1);
        TRACE(causticsContrast, 0, 7.96875);
        TRACE(causticsDepth, 0, 100);
        TRACE(translucencyDenoiser, 0, 1);
        TRACE(translucencyReuse, 0, 1);
        TRACE(pbrLocalReuse, 0, 1);
        TRACE(validation, 0, 1);
        TRACE(sharc, 0, 1);
        TRACE(sharcDebug, 0, 1);
        TRACE(sharcCapacityLog2, 16, 22);
        TRACE(sharcDownscale, 1, 16);
        TRACE(sharcSceneScale, 1, 200);
        TRACE(sharcAccumulationFrames, 1, 1024);
        TRACE(sharcStaleFrames, 8, 1024);
        TRACE(sharcMotionFrames, 1, 1024);
        TRACE(sharcFireflyClamp, 0, 1000);
        TRACE(sharcMaxUpdatePaths, 0, 16777216);
        TRACE(sharcDynamicSurfaces, 0, 1);
        TRACE(sharcGlossySurfaces, 0, 1);
        TRACE(sharcFootprintTest, 0, 1);
        TRACE(sky, 0, 1);
        TRACE(forceRebuild, 0, 1);
        TRACE(fsrSharpness, 0, 1);
        TRACE(localDirectClamp, 0, 1000000);
        TRACE(indirectClamp, 0, 1000000);
        TRACE(localIntensity, 0, 100);
        result.push_back(scalar("rtx.native.pathtrace.localRange", FIELD(trace.localRange), 0, 1024, .01));
        result.push_back(scalar("rtx.native.pathtrace.sunAngularDiameter", FIELD(trace.sunAngularDiameter), 0,
                                10, 0, true));
        TRACE(overrideLight, 0, 1);
        TRACE(lightIntensity, 0, 20);
        TRACE(hardwareFaceCulling, 0, 1);
        TRACE(combinedTlas, 0, 1);
        TRACE(shadowEarlyOut, 0, 1);
        for (size_t i = 0; i < 3; ++i) {
          result.push_back(scalar(
              "rtx.native.pathtrace.lightDirection." + std::string(1, "xyz"[i]),
              [i](auto &s) -> auto & { return s.trace.lightDirection[i]; }, -1, 1));
          result.push_back(scalar(
              "rtx.native.pathtrace.lightColor." + std::string(1, "rgb"[i]),
              [i](auto &s) -> auto & { return s.trace.lightColor[i]; }, 0, 1));
          result.push_back(scalar(
              "rtx.native.pathtrace.volumetricColor." + std::string(1, "rgb"[i]),
              [i](auto &s) -> auto & { return s.trace.volumetricColor[i]; }, 0, 1));
        }
        for (size_t i = 0; i < 4; ++i)
          result.push_back(scalar(
              "rtx.native.clearColor." + std::string(1, "rgba"[i]),
              [i](auto &s) -> auto & { return s.clearColor[i]; }, 0, 1));
        result.push_back(scalar("rtx.native.textures.keepCompressed", FIELD(compressedTextures), 0, 1));
        result.push_back(scalar("rtx.native.vramStatistics", FIELD(vramStatistics), 0, 1));
        result.push_back(scalar("rtx.native.meshes.refreshInPlace", FIELD(refreshGeometry), 0, 1));
        result.push_back(scalar("rtx.native.hostPasses", FIELD(hostPasses), 0, 1));
        result.push_back(scalar("rtx.native.scene.persistentStatic", FIELD(persistentStatic), 0, 1));
        result.push_back(
            scalar("rtx.native.scene.persistentStaticValidation", FIELD(persistentStaticValidation), 0, 1));
        result.push_back(scalar("rtx.native.asyncPresent", FIELD(asyncPresent), 0, 1));
        result.push_back(scalar("rtx.native.hdr", FIELD(hdr), 0, 1));
        result.push_back(scalar("rtx.native.hdrPeakNits", FIELD(hdrPeakNits), 80, 10000));
        result.push_back(scalar("rtx.native.hdrPaperWhiteNits", FIELD(hdrPaperWhiteNits), 80, 1000));
        result.push_back(scalar("rtx.native.hdrUiNits", FIELD(hdrUiNits), 80, 1000));
        result.push_back(scalar("rtx.native.frameGeneration", FIELD(frameGeneration), 0, 1));
        result.push_back(choices("rtx.native.frameGenerationBackend", FIELD(frameGenerationBackend),
                                 {"auto", "dlss", "fsr31"}));
        result.push_back(scalar("rtx.native.frameGenerationFrames", FIELD(frameGenerationFrames), 1, 3));
        result.push_back(scalar("rtx.native.lowLatency", FIELD(lowLatency), 0, 1));
        result.push_back(scalar("rtx.native.gpuPassTimers", FIELD(gpuPassTimers), 0, 1));
        result.push_back(scalar("rtx.native.sky.enabled", FIELD(sky.enabled), 0, 1));
        result.push_back(choices("rtx.native.sky.gameSky", FIELD(sky.gameSky), {"hide", "under", "over"}));
        result.push_back(scalar("rtx.native.sky.radianceScale", FIELD(sky.radianceScale), 1e-6, 1e6));
        result.push_back(scalar("rtx.native.sky.rasterExposure", FIELD(sky.rasterExposure), .01, 100));
        result.push_back(scalar("rtx.native.sky.ignoreGameControl", FIELD(sky.ignoreGameControl), 0, 1));
        result.push_back(scalar("rtx.native.particles.enabled", FIELD(particles.enabled), 0, 1));
        result.push_back(scalar("rtx.native.particles.budget", FIELD(particles.budget), 1024, 262144));
        result.push_back(scalar("rtx.native.particles.spawnBudget", FIELD(particles.spawnBudget), 64, 65536));
        result.push_back(scalar("rtx.native.particles.precipitation", FIELD(particles.precipitation), 0, 1));
        result.push_back(scalar("rtx.native.particles.rain", FIELD(particles.rain), 0, 1));
        result.push_back(scalar("rtx.native.particles.snow", FIELD(particles.snow), 0, 1));
        result.push_back(scalar("rtx.native.particles.shelter", FIELD(particles.shelter), 0, 1));
        result.push_back(scalar("rtx.native.particles.followWeather", FIELD(particles.followWeather), 0, 1));
        result.push_back(scalar("rtx.native.particles.intensity", FIELD(particles.intensity), 0, 1));
        result.push_back({
            "rtx.hideInstanceTextures",
            [](const Settings &s) {
              std::string text;
              for (size_t i = 0; i < s.hideTextures.size(); ++i) {
                if (i > 0)
                  text += ", ";
                char buf[32];
                snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)s.hideTextures[i]);
                text += buf;
              }
              return text;
            },
            [](Settings &s, std::string_view text) {
              s.hideTextures.clear();
              while (!text.empty()) {
                auto comma = text.find(',');
                auto item = trim(comma == text.npos ? text : text.substr(0, comma));
                if (comma == text.npos)
                  text = {};
                else
                  text = text.substr(comma + 1);
                if (item.empty())
                  continue;
                uint64_t hash = 0;
                if (item.size() > 2 && item[0] == '0' && (item[1] == 'x' || item[1] == 'X')) {
                  auto res = std::from_chars(item.data() + 2, item.data() + item.size(), hash, 16);
                  if (res.ec == std::errc{})
                    s.hideTextures.push_back(hash);
                } else {
                  auto res = std::from_chars(item.data(), item.data() + item.size(), hash, 10);
                  if (res.ec == std::errc{})
                    s.hideTextures.push_back(hash);
                }
              }
              return true;
            }});
#undef TRACE
#undef FIELD
        return result;
      }();
      return entries;
    }
  } // namespace

  bool isAtmosphereKey(std::string_view key) { return key.starts_with("rtx.atmosphere."); }

  SetResult set(Settings &settings, std::string_view key, std::string_view value) {
    for (const auto &entry : registry())
      if (entry.key == key)
        return entry.write(settings, value) ? SetResult::Success : SetResult::Invalid;
    if (isAtmosphereKey(key))
      return setAtmosphere(settings.sky.atmosphere, key, value);
    return SetResult::Unknown;
  }

  bool validate(const Settings &settings, std::string &error) {
    Settings checked;
    for (const auto &entry : registry()) {
      if (!entry.write(checked, entry.read(settings))) {
        error = "Invalid value for " + entry.key;
        return false;
      }
    }
    for (const auto &[key, value] : settings.sky.atmosphere) {
      if (setAtmosphere(checked.sky.atmosphere, key, value) != SetResult::Success) {
        error = "Invalid value for " + key;
        return false;
      }
    }
    if (settings.trace.sharcReset) {
      error = "Cache reset is an action, not a profile setting";
      return false;
    }
    return true;
  }

  std::string serialize(const Settings &settings, const UnknownSettings &unknown) {
    std::string text = "# OpenRemix configuration profile\nprofile.version = 1\n";
    for (const auto &entry : registry())
      text += entry.key + " = " + entry.read(settings) + "\n";
    for (const auto &[key, value] : settings.sky.atmosphere)
      text += key + " = " + value + "\n";
    if (!unknown.empty())
      text += "\n# Unrecognized settings retained for compatibility\n";
    for (const auto &[key, value] : unknown)
      text += key + " = " + value + "\n";
    return text;
  }

  bool parse(std::string_view text, Settings &settings, UnknownSettings &unknown,
             std::vector<std::string> &warnings, std::string &error) {
    if (text.starts_with("\xef\xbb\xbf"))
      text.remove_prefix(3);
    Settings next;
    UnknownSettings extra;
    std::vector<std::string> notices;
    std::set<std::string> seen;
    bool version = false;
    size_t line = 0;
    while (!text.empty()) {
      ++line;
      auto end = text.find('\n');
      auto item = trim(text.substr(0, end));
      text = end == text.npos ? std::string_view{} : text.substr(end + 1);
      if (item.empty() || item.front() == '#' || item.front() == ';')
        continue;
      const auto equal = item.find('=');
      const auto key = trim(item.substr(0, equal));
      if (equal == item.npos || key.empty() || !seen.emplace(key).second) {
        error = "Malformed or duplicate key at line " + std::to_string(line);
        return false;
      }
      const auto value = trim(item.substr(equal + 1));
      if (key == "profile.version") {
        if (value != "1") {
          error = "Unsupported profile version: " + std::string(value);
          return false;
        }
        version = true;
        continue;
      }
      const auto result = set(next, key, value);
      if (result == SetResult::Invalid) {
        error = "Invalid value for " + std::string(key);
        return false;
      }
      if (result == SetResult::Unknown) {
        // Never replay one-shot actions from a file.
        if (key == "rtx.native.pathtrace.sharcReset") {
          notices.push_back("Ignored one-shot action: " + std::string(key));
        } else {
          extra.emplace(key, value);
          notices.push_back("Unknown key retained: " + std::string(key));
        }
      }
    }
    if (!version) {
      error = "Missing profile.version";
      return false;
    }
    migrateAtmosphere(next, notices);
    if (!validate(next, error))
      return false;
    settings = next;
    unknown = std::move(extra);
    warnings = std::move(notices);
    return true;
  }
} // namespace remix::config

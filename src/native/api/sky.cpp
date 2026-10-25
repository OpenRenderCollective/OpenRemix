#include "api/state.h"
#include <remix/remix_sky_ext.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#if REMIX_HAS_NUMOS
#include <numos/weather.h>
#endif

using namespace remix::api;

namespace {
  // Runs `work` on the game's sky control under the API lock.
  template <class Work> remixapi_ErrorCode withControl(Work &&work) {
    try {
      std::lock_guard lock(g_apiMutex);
      if (!g_runtime.isInitialized())
        return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
      return work(g_runtime.skyControl());
    } catch (...) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
  }

  bool finite(float value) { return std::isfinite(value); }
} // namespace

extern "C" {
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetSkyTimeEXT(const remixapi_SkyTimeEXT *time) {
  if (time && (time->structSize != sizeof(*time) || !finite(time->timeOfDayHours) ||
               !finite(time->dayLengthMinutes) || time->dayOfYear < 0 || time->dayOfYear > 365))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  return withControl([&](remix::config::SkyControl &control) {
    ++control.timeGeneration;
    control.time = time != nullptr;
    if (time) {
      control.timeOfDayHours = std::clamp(time->timeOfDayHours, 0.f, 24.f);
      control.dayOfYear = time->dayOfYear;
      control.dayLengthMinutes = std::max(time->dayLengthMinutes, 0.f);
    }
    return REMIXAPI_ERROR_CODE_SUCCESS;
  });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetSkyWeatherPresetEXT(const char *preset,
                                                                          float transitionSeconds,
                                                                          float precipitation) {
  if (!finite(transitionSeconds) || !finite(precipitation))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  const std::string name = preset ? preset : "";
#if REMIX_HAS_NUMOS
  numos::WeatherPreset known;
  if (!name.empty() && !numos::weatherPresetFromName(name.c_str(), known))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
#endif
  return withControl([&](remix::config::SkyControl &control) {
    ++control.weatherGeneration;
    control.weather = true;
    control.preset = name;
    control.fields.clear();
    control.transitionSeconds = std::max(transitionSeconds, 0.f);
    control.precipitation = precipitation < 0 ? -1.f : std::min(precipitation, 1.f);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetSkyWeatherFieldEXT(const char *field, float value,
                                                                         float transitionSeconds) {
  if (!field || !finite(value) || !finite(transitionSeconds))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
#if REMIX_HAS_NUMOS
  size_t count = 0;
  const auto *fields = numos::weatherFields(count);
  if (std::none_of(fields, fields + count, [&](const auto &info) { return std::strcmp(info.name, field) == 0; }))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
#endif
  return withControl([&](remix::config::SkyControl &control) {
    ++control.weatherGeneration;
    control.weather = true;
    control.fields[field] = value;
    control.transitionSeconds = std::max(transitionSeconds, 0.f);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_ClearSkyWeatherEXT(float transitionSeconds) {
  if (!finite(transitionSeconds))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  return withControl([&](remix::config::SkyControl &control) {
    ++control.weatherGeneration;
    control.weather = false;
    control.preset.clear();
    control.fields.clear();
    control.transitionSeconds = std::max(transitionSeconds, 0.f);
    control.precipitation = -1;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_TriggerSkyLightningEXT(const float *position) {
  if (position && !(finite(position[0]) && finite(position[1]) && finite(position[2])))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  return withControl([&](remix::config::SkyControl &control) {
    ++control.lightningGeneration;
    control.lightningAtPosition = position != nullptr;
    if (position)
      std::copy_n(position, 3, control.lightningPosition);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetSkyStateEXT(remixapi_SkyStateEXT *out) {
  if (!out || out->structSize < 8)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  try {
    std::lock_guard lock(g_apiMutex);
    if (!g_runtime.isInitialized())
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    const auto state = g_runtime.skyState();
    remixapi_SkyStateEXT result{};
    result.structSize = sizeof(result);
    result.version = 1;
#if REMIX_HAS_NUMOS
    result.flags = 1u;
#endif
    result.flags |= uint32_t(state.active) << 1;
    result.light = state.light;
    std::copy_n(state.lightDirection, 3, result.lightDirection);
    result.timeOfDayHours = state.timeOfDayHours;
    result.weatherTransition = state.weatherTransition;
    result.cloudCoverage = state.cloudCoverage;
    result.precipitation = state.precipitation;
    std::strncpy(result.weatherPreset, state.preset.c_str(), std::size(result.weatherPreset) - 1);
    std::memcpy(out, &result, std::min(size_t(out->structSize), sizeof(result)));
    return REMIXAPI_ERROR_CODE_SUCCESS;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}
}

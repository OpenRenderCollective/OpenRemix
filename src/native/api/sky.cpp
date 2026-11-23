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
  bool finite(const float *v) { return finite(v[0]) && finite(v[1]) && finite(v[2]); }

  static_assert(REMIXAPI_SKY_LENS_COUNT_EXT == remix::config::skyLensCount);

  // v scaled to unit length; false for a vector too short to have a direction.
  bool normalize(float *v) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(length > 1e-12f))
      return false;
    for (int i = 0; i < 3; ++i)
      v[i] /= length;
    return true;
  }

  void cross(const float *a, const float *b, float *out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
  }
} // namespace

extern "C" {
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetSkyTimeEXT(const remixapi_SkyTimeEXT *time) {
  if (time && (time->structSize != sizeof(*time) || !finite(time->timeOfDayHours) || !finite(time->dayLengthMinutes) ||
               time->dayOfYear < 0 || time->dayOfYear > 365))
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

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetSkyWeatherPresetEXT(const char *preset, float transitionSeconds,
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

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetSkyLensEXT(uint32_t slot, const remixapi_SkyLensEXT *lens) {
  if (slot >= remix::config::skyLensCount)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  remix::config::SkyLens next;
  bool cut = false;
  if (lens) {
    if (lens->structSize != sizeof(*lens) || (lens->flags & ~REMIXAPI_SKY_LENS_FLAG_CUT_EXT) ||
        !finite(lens->eyepieceCenter) || !finite(lens->objectiveCenter) || !finite(lens->axis) || !finite(lens->up) ||
        !finite(lens->magnification) || !finite(lens->eyepieceRadius) || !finite(lens->apertureAspect) ||
        !(lens->magnification > 0) || !(lens->eyepieceRadius > 0) || lens->apertureAspect < 0)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    next.active = true;
    cut = (lens->flags & REMIXAPI_SKY_LENS_FLAG_CUT_EXT) != 0;
    std::copy_n(lens->eyepieceCenter, 3, next.eyepieceCenter);
    std::copy_n(lens->objectiveCenter, 3, next.objectiveCenter);
    std::copy_n(lens->axis, 3, next.axis);
    next.eyepieceRadius = lens->eyepieceRadius;
    next.apertureAspect = lens->apertureAspect;
    next.magnification = lens->magnification;
    if (!normalize(next.axis))
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    if (next.apertureAspect > 0) {
      // The rectangle's frame: its height direction made perpendicular to the axis.
      cross(lens->up, next.axis, next.right);
      if (!normalize(next.right))
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      cross(next.axis, next.right, next.up);
    }
  }
  return withControl([&](remix::config::SkyControl &control) {
    auto &stored = control.lenses[slot];
    next.cutGeneration = stored.cutGeneration + (cut || next.active != stored.active);
    stored = next;
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

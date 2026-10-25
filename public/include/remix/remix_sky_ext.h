// Optional control of the NUMOS sky (sky, clouds, weather and lightning) by
// the game. No change to the retained API table or layouts. Every setting
// stays reachable through SetConfigVariable as well (rtx.native.sky.* and
// rtx.atmosphere.*); these calls are the game's live state on top of the
// profile: never saved, and ignored while the profile sets
// rtx.native.sky.ignoreGameControl. Look the functions up by name.
#pragma once
#include "remix_c.h"

// The time of day. dayOfYear 0 keeps the profile's; dayLengthMinutes 0
// stops the clock at timeOfDayHours, above 0 runs it, one day in that many
// real minutes. Setting a new time moves a running clock to it.
// structSize is sizeof(remixapi_SkyTimeEXT); version is 1.
typedef struct remixapi_SkyTimeEXT {
  uint32_t structSize;
  uint32_t version;
  float timeOfDayHours; // 0 to 24; 12 is solar noon
  int32_t dayOfYear;    // 1 to 365, or 0
  float dayLengthMinutes;
} remixapi_SkyTimeEXT;

// What the sky did last frame. structSize is the caller's capacity (at least
// 8); only the known prefix is written, and the returned structSize is this
// implementation's. version is 1.
typedef struct remixapi_SkyStateEXT {
  uint32_t structSize;
  uint32_t version;
  // Bit 0: this build has the sky; bit 1: the sky rendered last frame.
  uint32_t flags;
  // The scene's distant light: 0 none, 1 sun, 2 moon, 3 lightning flash.
  uint32_t light;
  float lightDirection[3]; // its propagation direction, world space
  float timeOfDayHours;    // the running time of day
  float weatherTransition; // 0 to 1 through a weather change; 1 settled
  float cloudCoverage;     // the weather's cloud coverage, 0 to 1
  float precipitation;     // the precipitation intensity in force, 0 to 1
  float reserved;
  char weatherPreset[32];  // the game's weather preset, or empty
} remixapi_SkyStateEXT;

#ifdef __cplusplus
extern "C" {
#endif
// The game's time of day; null hands time back to the profile.
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetSkyTimeEXT(const remixapi_SkyTimeEXT *time);
// Eases the weather to a NUMOS preset over transitionSeconds (0 jumps). The
// presets: Clear, Fair, PartlyCloudy, Cloudy, Overcast, Haze, Fog, Storm,
// Drizzle, Altostratus (case-insensitive). Null or empty starts from the
// profile's weather, for remixapi_SetSkyWeatherFieldEXT. precipitation, 0 to
// 1, is the rain and snow intensity particles follow; negative takes the
// preset's own (Storm 1, Drizzle 0.35, the others 0). A new preset clears
// the fields set before it.
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetSkyWeatherPresetEXT(const char *preset,
                                                                          float transitionSeconds,
                                                                          float precipitation);
// Sets one weather field over the preset, eased in over transitionSeconds.
// The fields are NUMOS's weather fields, named as their rtx.atmosphere.*
// settings without the prefix: cloudCoverageMean, cloudTypeMean,
// cloudDensity, cloudBaseHeightMeters, cloudWindSpeed, cloudWindDirection,
// fogVisibilityMeters, lightningStrikesPerMinute and the rest of the list in
// NUMOS's include/numos/weather.h. An unknown name is INVALID_ARGUMENTS.
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetSkyWeatherFieldEXT(const char *field, float value,
                                                                         float transitionSeconds);
// Eases back to the profile's weather over transitionSeconds.
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_ClearSkyWeatherEXT(float transitionSeconds);
// A lightning strike next frame, at position (world space) or, with null,
// where an automatic strike would land. Needs rtx.atmosphere.lightningEnable.
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_TriggerSkyLightningEXT(const float *position);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetSkyStateEXT(remixapi_SkyStateEXT *state);
#ifdef __cplusplus
}
#endif

typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_SetSkyTimeEXT)(const remixapi_SkyTimeEXT *time);
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_SetSkyWeatherPresetEXT)(const char *preset,
                                                                            float transitionSeconds,
                                                                            float precipitation);
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_SetSkyWeatherFieldEXT)(const char *field, float value,
                                                                           float transitionSeconds);
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_ClearSkyWeatherEXT)(float transitionSeconds);
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_TriggerSkyLightningEXT)(const float *position);
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_GetSkyStateEXT)(remixapi_SkyStateEXT *state);

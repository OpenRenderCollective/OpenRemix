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

// Where the game's own sun and moon are, for a game whose day does not
// follow Earth's (a night that runs faster, a sun on its own path). The sun
// direction replaces the time of day's sun (the clock still sets the
// stars); the moon direction replaces Earth's Moon with one moon there.
// Directions are world space (as the lights'), toward the body; a moonPhase
// of 0 to 1 (0 new, 0.5 full) sets its phase, negative takes it from the
// sun. structSize is sizeof(remixapi_SkyLightsEXT); version is 1.
#define REMIXAPI_SKY_LIGHTS_SUN_EXT 0x1u
#define REMIXAPI_SKY_LIGHTS_MOON_EXT 0x2u
typedef struct remixapi_SkyLightsEXT {
  uint32_t structSize;
  uint32_t version;
  uint32_t flags; // REMIXAPI_SKY_LIGHTS_*_EXT: which directions are set
  float sunDirection[3];
  float moonDirection[3];
  float moonPhase;
} remixapi_SkyLightsEXT;

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
  char weatherPreset[32]; // the game's weather preset, or empty
} remixapi_SkyStateEXT;

// A magnifier in front of the camera, such as a telescopic sight: camera rays
// that meet the eyepiece plane inside the eyepiece leave the objective with
// the tangent of their angle to the axis divided by the magnification. The
// sky follows those rays where the frame shows it through the lens: its
// clouds, sun, moons and stars are magnified. The haze is not, and the
// renderer does not magnify the scene: this describes a lens whose magnified
// image the game supplies. Where that image has a surface, the frame's depth
// there must be the surface's virtual distance (the camera ray's distance to
// the eyepiece plane plus the distance from the objective to the surface).
// Positions and lengths are in world space, as the camera's.
// structSize is sizeof(remixapi_SkyLensEXT); version is 1.
#define REMIXAPI_SKY_LENS_COUNT_EXT 4u
// This call's lens is not the one the slot held before, or its image jumped
// (a zoom step): the sky seen through it starts without history.
#define REMIXAPI_SKY_LENS_FLAG_CUT_EXT 1u
typedef struct remixapi_SkyLensEXT {
  uint32_t structSize;
  uint32_t version;
  uint32_t flags;           // REMIXAPI_SKY_LENS_FLAG_*
  float magnification;      // above 0; 4 is a 4x sight
  float eyepieceCenter[3];
  float eyepieceRadius;     // above 0; a rectangular eyepiece's half height
  float objectiveCenter[3]; // where the magnified rays start
  float apertureAspect;     // a rectangle's half width over its half height; 0: a circular eyepiece
  float axis[3];            // the optical axis, pointing out of the objective; any length above 0
  float up[3];              // a rectangular eyepiece's height direction, not parallel to the axis; unused for a circle
} remixapi_SkyLensEXT;

#ifdef __cplusplus
extern "C" {
#endif
// The game's time of day; null hands time back to the profile.
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetSkyTimeEXT(const remixapi_SkyTimeEXT *time);
// The game's sun and moon; null hands them back to the time of day.
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetSkyLightsEXT(const remixapi_SkyLightsEXT *lights);
// Eases the weather to a NUMOS preset over transitionSeconds (0 jumps). The
// presets: Clear, Fair, PartlyCloudy, Cloudy, Overcast, Haze, Fog, Storm,
// Drizzle, Altostratus (case-insensitive). Null or empty starts from the
// profile's weather, for remixapi_SetSkyWeatherFieldEXT. precipitation, 0 to
// 1, is the rain and snow intensity particles follow; negative takes the
// preset's own (Storm 1, Drizzle 0.35, the others 0). A new preset clears
// the fields set before it.
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetSkyWeatherPresetEXT(const char *preset, float transitionSeconds,
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
// Places the magnifier lens of a slot (below REMIXAPI_SKY_LENS_COUNT_EXT), or
// removes it with null. A lens keeps its slot while it exists and stays where
// it was set: set it again each frame it moves. Not part of the sky's look,
// so rtx.native.sky.ignoreGameControl does not affect it.
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetSkyLensEXT(uint32_t slot, const remixapi_SkyLensEXT *lens);
#ifdef __cplusplus
}
#endif

typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_SetSkyTimeEXT)(const remixapi_SkyTimeEXT *time);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_SetSkyLightsEXT)(const remixapi_SkyLightsEXT *lights);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_SetSkyWeatherPresetEXT)(const char *preset,
                                                                              float transitionSeconds,
                                                                              float precipitation);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_SetSkyWeatherFieldEXT)(const char *field, float value,
                                                                             float transitionSeconds);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_ClearSkyWeatherEXT)(float transitionSeconds);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_TriggerSkyLightningEXT)(const float *position);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_GetSkyStateEXT)(remixapi_SkyStateEXT *state);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_SetSkyLensEXT)(uint32_t slot, const remixapi_SkyLensEXT *lens);

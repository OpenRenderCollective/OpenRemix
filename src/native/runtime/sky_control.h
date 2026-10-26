#pragma once
#include <cstdint>
#include <map>
#include <string>

namespace remix::config {
  // What the game sets on the sky through remix_sky_ext.h: runtime state,
  // never saved, applied over the profile's sky settings unless the profile
  // ignores game control (SkySettings::ignoreGameControl). Each part counts
  // its changes, so the renderer acts on a change once.
  struct SkyControl {
    // Weather: a NUMOS preset (empty: the profile's own weather) with
    // weather fields over it, eased in over transitionSeconds. Cleared, the
    // weather eases back to the profile's.
    uint64_t weatherGeneration = 0;
    bool weather = false;
    std::string preset;
    std::map<std::string, float> fields;
    float transitionSeconds = 0;
    // The game's precipitation intensity, 0 to 1 (rain and snow particles
    // follow it); negative: the preset's own.
    float precipitation = -1;

    // Time of day: hours 0 to 24, day of the year (0: the profile's), and
    // the day's length in real minutes while the clock runs (0: stopped).
    uint64_t timeGeneration = 0;
    bool time = false;
    float timeOfDayHours = 12;
    int32_t dayOfYear = 0;
    float dayLengthMinutes = 0;

    // A lightning strike, at a position (render space) or placed as an
    // automatic one.
    uint64_t lightningGeneration = 0;
    bool lightningAtPosition = false;
    float lightningPosition[3] = {};
  };

  // What the sky did last frame, for remix_sky_ext.h and the Sky tab.
  struct SkyState {
    bool active = false;
    // The scene's distant light: 0 none, 1 sun, 2 moon, 3 lightning.
    uint32_t light = 0;
    float lightDirection[3] = {0, 0, -1};
    float timeOfDayHours = 0;
    // The weather's transition, 0 to 1 (1: settled), and its cloud coverage.
    float weatherTransition = 1;
    float cloudCoverage = 0;
    // The precipitation intensity in force (SkyControl::precipitation, or
    // the weather preset's).
    float precipitation = 0;
    std::string preset;
  };
} // namespace remix::config

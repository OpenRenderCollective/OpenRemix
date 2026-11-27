// remix_sky_ext.h before startup: argument checks come first, then every
// valid call reports that the runtime is not initialized.
#include <remix/remix_c.h>
#include <remix/remix_sky_ext.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
} // namespace

int main(int argc, char **argv) {
  require(argc >= 2, "Provide runtime library");
  remixapi_Interface api = {};
  remixapi_HMODULE library = nullptr;
#if defined(_WIN32)
  std::wstring path(argv[1], argv[1] + std::strlen(argv[1]));
  require(remixapi_lib_loadRemixDllAndInitialize(path.c_str(), &api, &library) == REMIXAPI_ERROR_CODE_SUCCESS,
          "Load runtime");
  const auto symbol = [&](const char *name) { return reinterpret_cast<void *>(GetProcAddress(library, name)); };
#else
  require(remixapi_lib_loadRemixDllAndInitialize(argv[1], &api, &library) == REMIXAPI_ERROR_CODE_SUCCESS,
          "Load runtime");
  const auto symbol = [&](const char *name) { return dlsym(library, name); };
#endif
  const auto setTime = reinterpret_cast<PFN_remixapi_SetSkyTimeEXT>(symbol("remixapi_SetSkyTimeEXT"));
  const auto setLights = reinterpret_cast<PFN_remixapi_SetSkyLightsEXT>(symbol("remixapi_SetSkyLightsEXT"));
  const auto setPreset =
      reinterpret_cast<PFN_remixapi_SetSkyWeatherPresetEXT>(symbol("remixapi_SetSkyWeatherPresetEXT"));
  const auto setField = reinterpret_cast<PFN_remixapi_SetSkyWeatherFieldEXT>(symbol("remixapi_SetSkyWeatherFieldEXT"));
  const auto clearWeather = reinterpret_cast<PFN_remixapi_ClearSkyWeatherEXT>(symbol("remixapi_ClearSkyWeatherEXT"));
  const auto lightning =
      reinterpret_cast<PFN_remixapi_TriggerSkyLightningEXT>(symbol("remixapi_TriggerSkyLightningEXT"));
  const auto getState = reinterpret_cast<PFN_remixapi_GetSkyStateEXT>(symbol("remixapi_GetSkyStateEXT"));
  const auto setLens = reinterpret_cast<PFN_remixapi_SetSkyLensEXT>(symbol("remixapi_SetSkyLensEXT"));
  require(setTime && setLights && setPreset && setField && clearWeather && lightning && getState && setLens, "Sky exports");

  constexpr auto invalid = REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS, early = REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
  remixapi_SkyTimeEXT time{
      .structSize = sizeof(time), .version = 1, .timeOfDayHours = 22, .dayOfYear = 0, .dayLengthMinutes = 0};
  require(setTime(&time) == early && setTime(nullptr) == early, "Time before startup");
  time.structSize = 4;
  require(setTime(&time) == invalid, "Time struct size");
  time.structSize = sizeof(time);
  time.dayOfYear = 400;
  require(setTime(&time) == invalid, "Day of year range");
  time.dayOfYear = 0;
  time.timeOfDayHours = NAN;
  require(setTime(&time) == invalid, "Time of day finite");

  remixapi_SkyLightsEXT lights{.structSize = sizeof(lights),
                                .version = 1,
                                .flags = REMIXAPI_SKY_LIGHTS_SUN_EXT | REMIXAPI_SKY_LIGHTS_MOON_EXT,
                                .sunDirection = {0, 1, 1},
                                .moonDirection = {0, -1, 0},
                                .moonPhase = -1};
  require(setLights(&lights) == early && setLights(nullptr) == early, "Lights before startup");
  lights.structSize = 4;
  require(setLights(&lights) == invalid, "Lights struct size");
  lights.structSize = sizeof(lights);
  lights.sunDirection[0] = lights.sunDirection[1] = lights.sunDirection[2] = 0;
  require(setLights(&lights) == invalid, "Sun direction length");
  lights.flags = REMIXAPI_SKY_LIGHTS_MOON_EXT;
  require(setLights(&lights) == early, "Sun direction unset");
  lights.moonDirection[1] = NAN;
  require(setLights(&lights) == invalid, "Moon direction finite");

  require(setPreset("storm", 5, -1) == early && setPreset(nullptr, 0, 0) == early, "Preset before startup");
  require(setPreset("Storm", INFINITY, 0) == invalid, "Preset transition finite");
  require(setField(nullptr, 1, 0) == invalid && setField("cloudCoverageMean", NAN, 0) == invalid,
          "Weather field arguments");
  require(setField("cloudCoverageMean", .8f, 2) == early, "Weather field before startup");
  require(clearWeather(1) == early && clearWeather(NAN) == invalid, "Clear weather");
  const float position[3] = {0, 100, 0}, bad[3] = {0, NAN, 0};
  require(lightning(position) == early && lightning(nullptr) == early && lightning(bad) == invalid, "Lightning");

  remixapi_SkyLensEXT lens{};
  lens.structSize = sizeof(lens);
  lens.version = 1;
  lens.magnification = 4;
  lens.eyepieceCenter[2] = .1f;
  lens.eyepieceRadius = .02f;
  lens.objectiveCenter[2] = .4f;
  lens.axis[2] = 2;
  require(setLens(0, &lens) == early && setLens(3, nullptr) == early, "Lens before startup");
  require(setLens(REMIXAPI_SKY_LENS_COUNT_EXT, &lens) == invalid, "Lens slot range");
  const auto rejected = [&](auto &&change) {
    auto changed = lens;
    change(changed);
    return setLens(0, &changed) == invalid;
  };
  require(rejected([](auto &l) { l.structSize = 8; }), "Lens struct size");
  require(rejected([](auto &l) { l.flags = 2; }), "Lens flags");
  require(rejected([](auto &l) { l.magnification = 0; }) && rejected([](auto &l) { l.magnification = NAN; }),
          "Lens magnification");
  require(rejected([](auto &l) { l.eyepieceRadius = 0; }), "Lens eyepiece radius");
  require(rejected([](auto &l) { l.axis[2] = 0; }), "Lens axis");
  require(rejected([](auto &l) { l.objectiveCenter[1] = INFINITY; }), "Lens position finite");
  // A rectangular eyepiece needs a height direction off the axis.
  require(rejected([](auto &l) { l.apertureAspect = 1.5f; }), "Rectangular lens without up");
  require(rejected([](auto &l) {
            l.apertureAspect = 1.5f;
            l.up[2] = 1;
          }),
          "Rectangular lens with up along the axis");
  lens.apertureAspect = 1.5f;
  lens.up[1] = 1;
  lens.flags = REMIXAPI_SKY_LENS_FLAG_CUT_EXT;
  require(setLens(1, &lens) == early, "Rectangular lens before startup");

  remixapi_SkyStateEXT state{};
  require(getState(nullptr) == invalid, "State output");
  state.structSize = 4;
  require(getState(&state) == invalid, "State capacity");
  state.structSize = sizeof(state);
  require(getState(&state) == early, "State before startup");
  return 0;
}

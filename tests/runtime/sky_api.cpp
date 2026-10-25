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
  const auto setPreset = reinterpret_cast<PFN_remixapi_SetSkyWeatherPresetEXT>(symbol("remixapi_SetSkyWeatherPresetEXT"));
  const auto setField = reinterpret_cast<PFN_remixapi_SetSkyWeatherFieldEXT>(symbol("remixapi_SetSkyWeatherFieldEXT"));
  const auto clearWeather = reinterpret_cast<PFN_remixapi_ClearSkyWeatherEXT>(symbol("remixapi_ClearSkyWeatherEXT"));
  const auto lightning = reinterpret_cast<PFN_remixapi_TriggerSkyLightningEXT>(symbol("remixapi_TriggerSkyLightningEXT"));
  const auto getState = reinterpret_cast<PFN_remixapi_GetSkyStateEXT>(symbol("remixapi_GetSkyStateEXT"));
  require(setTime && setPreset && setField && clearWeather && lightning && getState, "Sky exports");

  constexpr auto invalid = REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS, early = REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
  remixapi_SkyTimeEXT time{.structSize = sizeof(time), .version = 1, .timeOfDayHours = 22, .dayOfYear = 0,
                           .dayLengthMinutes = 0};
  require(setTime(&time) == early && setTime(nullptr) == early, "Time before startup");
  time.structSize = 4;
  require(setTime(&time) == invalid, "Time struct size");
  time.structSize = sizeof(time);
  time.dayOfYear = 400;
  require(setTime(&time) == invalid, "Day of year range");
  time.dayOfYear = 0;
  time.timeOfDayHours = NAN;
  require(setTime(&time) == invalid, "Time of day finite");

  require(setPreset("storm", 5, -1) == early && setPreset(nullptr, 0, 0) == early, "Preset before startup");
  require(setPreset("Storm", INFINITY, 0) == invalid, "Preset transition finite");
  require(setField(nullptr, 1, 0) == invalid && setField("cloudCoverageMean", NAN, 0) == invalid,
          "Weather field arguments");
  require(setField("cloudCoverageMean", .8f, 2) == early, "Weather field before startup");
  require(clearWeather(1) == early && clearWeather(NAN) == invalid, "Clear weather");
  const float position[3] = {0, 100, 0}, bad[3] = {0, NAN, 0};
  require(lightning(position) == early && lightning(nullptr) == early && lightning(bad) == invalid, "Lightning");

  remixapi_SkyStateEXT state{};
  require(getState(nullptr) == invalid, "State output");
  state.structSize = 4;
  require(getState(&state) == invalid, "State capacity");
  state.structSize = sizeof(state);
  require(getState(&state) == early, "State before startup");
  return 0;
}

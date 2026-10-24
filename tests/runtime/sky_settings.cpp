#include "runtime/settings.h"
#include <cstdio>
#include <cstdlib>

namespace {
void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}
} // namespace

int main() {
  using namespace remix::config;
  Settings settings;
  require(!settings.sky.enabled && settings.sky.gameSky == SkySettings::GameSky::Hide, "sky defaults");
  require(set(settings, "rtx.native.sky.enabled", "1") == SetResult::Success, "enable sky");
  require(set(settings, "rtx.native.sky.gameSky", "under") == SetResult::Success, "game sky mode");
  require(set(settings, "rtx.native.sky.gameSky", "sideways") == SetResult::Invalid, "invalid game sky mode");
  // NUMOS settings are validated and stored in NUMOS's own format.
  require(set(settings, "rtx.atmosphere.sunSize", " 1.25 ") == SetResult::Success, "NUMOS float");
  require(settings.sky.atmosphere.at("rtx.atmosphere.sunSize") == "1.25", "NUMOS value normalised");
  require(set(settings, "rtx.atmosphere.cloudEnabled", "False") == SetResult::Success, "NUMOS bool");
  require(settings.sky.atmosphere.at("rtx.atmosphere.cloudEnabled") == "False", "NUMOS bool normalised");
  require(set(settings, "rtx.atmosphere.sunSize", "warm") == SetResult::Invalid, "invalid NUMOS value");
  require(set(settings, "rtx.atmosphere.noSuchSetting", "1") == SetResult::Unknown, "unknown NUMOS key");
  require(isAtmosphereKey("rtx.atmosphere.sunSize") && !isAtmosphereKey("rtx.native.sky.enabled"), "key prefix");

  // The profile round-trips.
  Settings loaded;
  UnknownSettings unknown;
  std::vector<std::string> warnings;
  std::string error;
  require(parse(serialize(settings), loaded, unknown, warnings, error), error.c_str());
  require(loaded == settings, "sky settings round trip");

  // Deprecated NUMOS keys migrate once, on load, and are not written back.
  const char *legacy = "profile.version = 1\nrtx.atmosphere.cloudQuarterMarch = False\n";
  require(parse(legacy, loaded, unknown, warnings, error), error.c_str());
  require(!loaded.sky.atmosphere.contains("rtx.atmosphere.cloudQuarterMarch"), "deprecated key dropped");
  require(loaded.sky.atmosphere.at("rtx.atmosphere.cloudMarchRate") == "1", "deprecated key migrated");
  require(!warnings.empty(), "migration reported");
  require(serialize(loaded).find("cloudQuarterMarch") == std::string::npos, "deprecated key not saved");
  return 0;
}

#include "runtime/settings.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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
  const auto &p = settings.particles;
  require(p.enabled && p.budget == 65536 && p.spawnBudget == 8192 && !p.rain && !p.snow && p.shelter,
          "particle defaults");
  require(set(settings, "rtx.native.particles.rain", "1") == SetResult::Success && p.rain, "rain test emitter");
  require(set(settings, "rtx.native.particles.budget", "100") == SetResult::Invalid, "budget below its range");
  require(set(settings, "rtx.native.particles.budget", "131072") == SetResult::Success && p.budget == 131072,
          "budget");
  require(set(settings, "rtx.native.particles.spawnBudget", "70000") == SetResult::Invalid,
          "spawn budget above its range");
  require(set(settings, "rtx.native.particles.precipitation", "0.25") == SetResult::Success &&
              p.precipitation == .25f,
          "precipitation");
  require(set(settings, "rtx.native.particles.intensity", "2") == SetResult::Invalid, "intensity above one");
  require(set(settings, "rtx.native.particles.noSuchKey", "1") == SetResult::Unknown, "unknown particle key");
  // The profile round-trips.
  Settings parsed;
  UnknownSettings unknown;
  std::vector<std::string> warnings;
  std::string error;
  require(parse(serialize(settings), parsed, unknown, warnings, error), error.c_str());
  require(parsed.particles == settings.particles, "particle settings round trip");
  std::puts("Particle settings checks passed");
  return 0;
}

#include "postfx/meter.h"
#include "runtime/settings.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace remix;
using namespace remix::postfx;
void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}
int main() {
  for (float albedo : {.05f, .18f, .9f}) {
    Meter m;
    m.sample({albedo, albedo, albedo}, {albedo, albedo, albedo}, 0, 0, {1, 1},
             .75);
    require(std::abs(std::exp(m.logSum / m.weightSum) - .18) < 1e-6,
            "Surface color changed illumination meter");
  }
  Meter red, blue, green;
  red.sample({3, 0, 0}, {1, 1, 1}, 0, 0, {1, 1}, 0);
  blue.sample({0, 0, 3}, {1, 1, 1}, 0, 0, {1, 1}, 0);
  green.sample({0, 3, 0}, {1, 1, 1}, 0, 0, {1, 1}, 0);
  require(red.logSum == blue.logSum && red.logSum == green.logSum,
          "Colored lights have unequal weight");
  Meter m;
  m.sample({NAN, 1, 1}, {1, 1, 1}, 0, 0, {1, 1}, 0);
  m.sample({1, 1, 1}, {INFINITY, 1, 1}, 0, 0, {1, 1}, 0);
  Exposure e;
  e.resolve(m, .2, -16, 2, 1);
  require(e.current == 1 && !e.initialized,
          "Invalid samples initialized meter");
  m.sample({0, 0, 0}, {0, 0, 0}, 0, 0, {1, 1}, 0);
  e.resolve(m, .2, -16, 2, 0);
  require(e.current == 4 && e.limit == 1,
          "First exposure did not snap to +2 EV limit");
  Meter bright;
  bright.sample({1e9, 1e9, 1e9}, {1, 1, 1}, 0, 0, {1, 1}, 0);
  e.resolve(bright, .2, -16, 2, 1);
  require(e.current == std::exp2(-16.f) && e.limit == -1,
          "Minimum exposure limit");
  e.resolve({}, .2, -16, 2, 1, true);
  require(e.current == std::exp2(-16.f),
          "Invalid frame lost previous exposure");
  e.resolve(m, .2, -16, 2, 0);
  require(e.current == 4, "Invalid reset did not defer snap until a valid frame");
  for (unsigned fps : {24, 30, 60, 144, 240}) {
    Exposure a{1, 1, 1, 0};
    for (unsigned i = 0; i < fps * 2; ++i)
      a.resolve(m, .2, -16, 2, adaptation(1.f / fps, 2));
    require(std::abs(a.current - (4 - 3 * std::exp(-4.f))) < 2e-5,
            "Frame-rate dependent adaptation");
  }
  require(adaptation(10, 2) == adaptation(.25, 2), "Elapsed time cap");
  Meter centre, edge;
  centre.sample({1, 1, 1}, {1, 1, 1}, 100, 50, {201, 101}, 1);
  edge.sample({1, 1, 1}, {1, 1, 1}, 0, 50, {201, 101}, 1);
  require(centre.weightSum == 1 && edge.weightSum == 0,
          "Aspect-correct center weighting");
  require(pyramid({1920, 1080}).size() == 8 &&
              pyramid({3840, 2160}).size() == 9,
          "Resolution-scaled pyramid depth");
  for (Size size :
       {Size{0, 0}, Size{1, 1}, Size{3, 7}, Size{17, 19}, Size{65535, 65535}}) {
    auto p = pyramid(size);
    require(p.size() >= 1 && p.size() <= 12, "Pyramid bounds");
    for (auto level : p)
      require(level.width && level.height, "Zero dimension pyramid");
  }
  config::Settings defaults, parsed;
  require(defaults.trace.autoExposure && defaults.trace.bloom &&
              defaults.trace.autoExposureMaxEv == 3.1f &&
              defaults.trace.exposure == .98f &&
              defaults.trace.bloomIntensity == .01f &&
              defaults.trace.bloomRadius == .85f,
          "New defaults");
  config::UnknownSettings unknown;
  std::vector<std::string> warnings;
  std::string error;
  require(
      config::parse("profile.version=1\n", parsed, unknown, warnings, error) &&
          parsed == defaults,
      "Legacy profiles default effects on");
  for (const char *key :
       {"autoExposure", "bloom", "exposure", "autoExposureTarget",
        "autoExposureSpeed", "autoExposureCentre", "autoExposureMinEv",
        "autoExposureMaxEv", "bloomIntensity", "bloomRadius"}) {
    auto full = std::string("rtx.native.pathtrace.") + key;
    require(config::set(parsed, full, "nan") == config::SetResult::Invalid,
            "Nonfinite configuration accepted");
    require(config::set(parsed, full, "100") == config::SetResult::Invalid,
            "Out of range accepted");
  }
  for (const auto &[key, low, high] : std::vector<std::array<const char *, 3>>{
      {"exposure", "0.05", "8"}, {"autoExposureTarget", "0.02", "0.6"},
      {"autoExposureSpeed", "0.1", "16"}, {"autoExposureCentre", "0", "1"},
      {"autoExposureMinEv", "-24", "0"}, {"autoExposureMaxEv", "0", "24"},
      {"bloomIntensity", "0", "1"}, {"bloomRadius", "0", "1"}}) {
    auto full = std::string("rtx.native.pathtrace.") + key;
    require(config::set(parsed, full, low) == config::SetResult::Success &&
            config::set(parsed, full, high) == config::SetResult::Success,
            "Float range endpoint rejected");
  }
  require(config::set(parsed, "rtx.native.pathtrace.autoExposure", "false") ==
                  config::SetResult::Success &&
              config::set(parsed, "rtx.native.pathtrace.bloom", "false") ==
                  config::SetResult::Success,
          "Boolean aliases");
  config::Settings roundtrip;
  require(config::parse(config::serialize(parsed), roundtrip, unknown, warnings,
                        error) &&
              parsed == roundtrip,
          "Post-processing profile roundtrip");
  std::puts("Post-processing CPU tests passed");
}

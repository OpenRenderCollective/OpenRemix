#include "pathtrace/sparse_policy.h"
#include <cstdio>
#include <cstdlib>

namespace {
  void require(bool value, const char *message) { if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); } }
  bool near(float a, float b) { return std::abs(a - b) < 5e-3f; }
} // namespace

int main() {
  using namespace remix;
  using namespace remix::trace;
  constexpr uint64_t reference = 2227ull * 1238;
  // The Ultra preset at neutral quality (the defaults are the Low preset's
  // rate and exponent at a lower quality): its rate as is at the reference
  // resolution.
  TraceOptions ultra;
  ultra.sparseSamplingRate = sparsePresets[0].samplingRate;
  ultra.sparseResolutionExponent = sparsePresets[0].resolutionExponent;
  ultra.sparseQuality = .5f;
  TraceOptions options = ultra;
  require(near(sparseSamplingRate(options, reference), .357f), "Reference resolution keeps the preset rate");
  // Lower render resolutions take a higher rate, up to every pixel.
  require(near(sparseSamplingRate(options, 1920 * 1080), .434f), "1080p Ultra rate");
  require(near(sparseSamplingRate(options, 1280 * 720), .761f), "720p Ultra rate");
  require(sparseSamplingRate(options, 640 * 360) == 1, "Low resolutions light every pixel");
  require(sparseSamplingRate(options, 640 * 360) > options.sparseMaxRate, "Low resolutions exceed the maximum rate");
  // Higher resolutions take a lower one, never below 1/16.
  require(sparseSamplingRate(options, 3840 * 2160) < .357f, "Above the reference the rate falls");
  options.sparseSamplingRate = .0625f;
  require(sparseSamplingRate(options, 7680ull * 4320) == .0625f, "Rate floor");
  // The presets as dxvk-remix scales them.
  options.sparseSamplingRate = sparsePresets[1].samplingRate;
  options.sparseResolutionExponent = sparsePresets[1].resolutionExponent;
  require(near(sparseSamplingRate(options, 1280 * 720), .566f), "720p High rate");
  options.sparseSamplingRate = sparsePresets[3].samplingRate;
  options.sparseResolutionExponent = sparsePresets[3].resolutionExponent;
  require(near(sparseSamplingRate(options, 960 * 540), .458f), "540p Low rate");
  // Quality moves the scaled rate between the bounds; 0.5 leaves it.
  options = ultra;
  const float scaled = sparseSamplingRate(options, 1920 * 1080);
  options.sparseQuality = 1;
  require(near(sparseSamplingRate(options, 1920 * 1080), 1), "Full quality lights every pixel");
  options.sparseQuality = 0;
  require(near(sparseSamplingRate(options, 1920 * 1080), .0625f), "Lowest quality reaches the floor");
  options.sparseQuality = .75f;
  require(near(sparseSamplingRate(options, 1920 * 1080), std::sqrt(scaled)), "Quality interpolates in log space");
  // Without scaling the rate is the setting at every resolution.
  options = ultra;
  options.sparseAdaptiveRate = false;
  options.sparseSamplingRate = .25f;
  require(sparseSamplingRate(options, 640 * 360) == .25f && sparseSamplingRate(options, reference) == .25f,
          "Fixed rate");
  // The shader's threshold: levels of 256, never none.
  require(sparseLevels(.25f) == 64 && sparseLevels(1) == 256 && sparseLevels(0) == 1, "Threshold levels");
  return 0;
}

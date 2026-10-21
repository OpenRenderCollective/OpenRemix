#include "scene/optics.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace remix::scene;
namespace {
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
}
int main() {
  Material water;
  water.transmission = true;
  water.refractiveIndex = 1.333f;
  water.transmittanceDistance = 800; // Source map units, not metres
  water.tint[0] = .25f;
  water.tint[1] = .5f;
  water.tint[2] = 1;
  auto medium = measuredMedium(water);
  require(bool(medium), "Measured transmissive bodies provide optical properties");
  const auto measured = medium->transmittance(800), deeper = medium->transmittance(1600);
  for (uint32_t c = 0; c < 3; ++c) {
    require(std::abs(measured[c] - water.tint[c]) < 1e-6f,
            "Measured distance reconstructs the authored RGB transmittance");
    require(std::abs(deeper[c] - water.tint[c] * water.tint[c]) < 1e-6f,
            "Twice the water depth composes two identical transmission segments");
  }
  require(medium->normalReflectance() > .02f && medium->normalReflectance() < .021f,
          "Water uses its IOR rather than opaque-material four-percent reflectance");
  water.refractiveIndex = 1.5f;
  require(std::abs(measuredMedium(water)->normalReflectance() - .04f) < 1e-6f,
          "An index-1.5 glass interface reflects four percent at normal incidence");
  water.transmittanceDistance *= .0254f;
  const auto metres = measuredMedium(water)->transmittance(800 * .0254f);
  for (uint32_t c = 0; c < 3; ++c)
    require(std::abs(metres[c] - measured[c]) < 1e-6f,
            "Changing distance units preserves the measured transmission");
  require(medium->transmittance(0) == std::array<float, 3>{1, 1, 1} &&
              medium->transmittance(-1) == std::array<float, 3>{1, 1, 1},
          "An empty or clamped path cannot absorb light");
  water.tint[0] = 0;
  const auto black = measuredMedium(water);
  require(black && std::isfinite(black->extinction[0]) &&
              black->transmittance(water.transmittanceDistance)[0] <= 1.01e-6f,
          "A black measurement retains bounded coefficients and near-zero transmission");
  water.transmittanceDistance = 0;
  require(!measuredMedium(water), "A thin sheet does not become a homogeneous water volume");
  require(!measuredSheet(water), "An unmeasured sheet has no invented absorption distance");
  water.transmittanceDistance = 800;
  water.thinWallThickness = 200;
  water.tint[0] = .25f;
  const auto sheet = measuredSheet(water);
  require(sheet && sheet->thicknessRatio == .25f &&
              std::abs(std::exp(-sheet->normalDepth[0]) - std::pow(.25f, .25f)) < 1e-6f &&
              sheet->normalDepth[2] == 0,
          "Measured sheet absorption uses finite thickness and preserves a clear channel");
  water.thinWallThickness *= .0254f;
  water.transmittanceDistance *= .0254f;
  const auto metricSheet = measuredSheet(water);
  require(metricSheet && std::abs(metricSheet->normalDepth[0] - sheet->normalDepth[0]) < 1e-6f,
          "Sheet optical depth is invariant under a consistent change of world units");
  water.thinWallThickness = std::numeric_limits<float>::max();
  water.transmittanceDistance = 1e-6f;
  const auto extremeSheet = measuredSheet(water);
  require(extremeSheet && extremeSheet->thicknessRatio == 1e10f &&
              extremeSheet->normalDepth[0] == 80 && extremeSheet->normalDepth[2] == 0,
          "Extreme sheet ratios retain bounded shader data without darkening exact white");
  water.transmission = false;
  require(!measuredMedium(water), "Opaque materials do not acquire volume extinction");
  bool refused = false;
  try {
    medium->transmittance(std::numeric_limits<float>::infinity());
  } catch (const std::invalid_argument &) {
    refused = true;
  }
  require(refused, "Invalid path lengths are refused before optical evaluation");
  std::puts("Measured optical units, water/glass reflectance, depth composition and thin sheets passed");
}

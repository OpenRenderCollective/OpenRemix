#include "pathtrace/material_optics.h"
#include "scene/fixtures.h"
#include <cmath>
#include <cstring>

using namespace remix;
using remix::test::require;
int main() {
  scene::Material material;
  const trace::SheetMaterial empty;
  auto optical = trace::sheetMaterial(material);
  require(!std::memcmp(&optical, &empty, sizeof(optical)), "Opaque material has no measured sheet data");
  material.transmission = true;
  material.refractiveIndex = 1.5f;
  material.transmittanceDistance = 2;
  material.tint[0] = .25f;
  material.tint[1] = .5f;
  material.tint[2] = 1;
  optical = trace::sheetMaterial(material);
  require(!std::memcmp(&optical, &empty, sizeof(optical)), "Containing bodies retain their existing optical path");
  material.thinWallThickness = .25f;
  optical = trace::sheetMaterial(material);
  require(optical.parameters == std::array<float, 4>{1.5f, .25f, .125f, 0} &&
              std::abs(optical.depth[0] + std::log(.25f) * .125f) < 1e-7f &&
              std::abs(optical.depth[1] + std::log(.5f) * .125f) < 1e-7f && optical.depth[2] == 0,
          "Sheet metadata retains full-precision IOR, thickness, tint depth and texture measurement ratio");
  material.thinWallThickness *= 1000;
  material.transmittanceDistance *= 1000;
  const auto scaled = trace::sheetMaterial(material);
  require(scaled.depth == optical.depth && scaled.parameters[2] == optical.parameters[2],
          "A common unit conversion preserves sheet absorption and texture measurement");
}

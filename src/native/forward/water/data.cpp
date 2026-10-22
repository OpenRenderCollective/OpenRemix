#include "forward/water/data.h"
#include "forward/bake/data.h"
#include "scene/optics.h"
#include <algorithm>

namespace remix::forward::water {
  bool accepts(const scene::Draw &draw) {
    return draw.surface && draw.material && draw.traced() && scene::validSmoothBoundary(*draw.material) &&
           (!draw.blend.enabled || bake::sourceOverCoverage(draw.blend)) && draw.blend.mask == 15 &&
           !draw.decal && !draw.cutGroup && !draw.wireframe && !draw.viewModel && !draw.spriteFlags &&
           draw.depth && (!draw.depthWriteOverride || draw.depthWrite) &&
           (draw.depthCompare == 1 || draw.depthCompare == 3);
  }
  MaterialData materialData(const scene::Material &material, double seconds) {
    if (!scene::validSmoothBoundary(material))
      throw std::invalid_argument("Refraction requires a measured transmission material");
    MaterialData result;
    std::copy_n(material.tint, 3, result.transmittance.begin());
    result.transmittance[3] = material.transmittanceDistance;
    if (const auto sheet = scene::measuredSheet(material)) {
      std::copy(sheet->normalDepth.begin(), sheet->normalDepth.end(), result.transmittance.begin());
      result.transmittance[3] = sheet->thicknessRatio;
    }
    result.optical = {material.refractiveIndex, float(seconds), float(material.scrollLayers),
                       material.thinWallThickness};
    std::copy_n(material.scroll, 4, result.scroll01.begin());
    std::copy_n(material.scroll + 4, 2, result.scroll2.begin());
    return result;
  }
} // namespace remix::forward::water

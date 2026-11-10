#pragma once
#include "scene/bake_scene.h"
#include <vector>

namespace remix::forward::probes {
  struct ReceiverSample {
    std::array<float, 3> position{}, normal{};
    double areaMetresSquared = 0;
    uint32_t draw = 0, chart = 0, firstIndex = 0;
  };
  // Bounded, deterministic area sampling of frozen chart geometry. Includes
  // charted props and terrain; does not claim coverage of uncharted surfaces or
  // test material alpha/visibility. Positions remain in host scene units.
  std::vector<ReceiverSample> sampleReceivers(const scene::BakeScene &source, uint32_t count,
                                             std::stop_token cancelled = {});
} // namespace remix::forward::probes

#pragma once
#include "forward/probes/coverage_baseline.h"
#include "forward/probes/image_asset.h"

namespace remix::forward::probes {
  // One retained physical image and the matching metadata used by replacement planning.
  struct ReflectionSnapshot {
    std::shared_ptr<const ImageAsset> image;
    CoverageBaselineSet baseline;
    LayerIdentities identities{};
  };
}

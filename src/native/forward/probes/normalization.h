#pragma once
#include "forward/probes/data.h"

namespace remix::forward::probes {
  struct Normalization {
    // Luminance of cosine-convolved capture radiance, divided by pi. Dot each
    // row with (world normal, 1). Zero is an unknown/dark reference, not unity.
    std::array<Vec4, maxProbes> coefficients{};
  };
  // Derived from canonical mip zero, so existing HDR caches remain usable.
  Normalization captureNormalization(const Data &data, std::stop_token cancelled = {});
}

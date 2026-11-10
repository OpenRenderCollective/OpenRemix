#pragma once
#include "forward/probes/data.h"
#include "forward/probes/normalization.h"

namespace remix::forward::probes {
  // One 2D-array layer per capture; its six faces are stacked vertically.
  // This needs no optional cube-array feature and keeps 64 captures below the
  // minimum array-layer limit. Each mip still has six complete square faces.
  struct ImageLevel {
    size_t byteOffset = 0;
    uint32_t faceSize = 0;
  };
  struct ImageData {
    Header header;
    std::vector<ImageLevel> levels;
    // Mip-major, then capture, face, row and column. RGBA16F, alpha unused.
    std::vector<uint16_t> pixels;
    Normalization normalization;
  };
  // Repack canonical CPU/cache data for image upload without changing it.
  // Upload records and image layers are sorted together by descending priority.
  // The canonical input remains unchanged, including equal-priority ordering.
  ImageData prepareImage(const Data &source, std::stop_token cancelled = {});
}

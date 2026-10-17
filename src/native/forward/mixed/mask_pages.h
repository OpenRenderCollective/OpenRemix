#pragma once
#include "forward/mixed/masks.h"
#include <optional>

namespace remix::forward::mixed {
  // Immutable R8 pages. Many chart/light rectangles share a page; a light does
  // not reserve a whole layer. Coordinates address texel centres, and filtering
  // must clamp to the rectangle before sampling to avoid adjacent masks.
  struct MaskPages {
    struct Rectangle {
      uint32_t chart, light, layer, x, y, width, height;
    };
    uint32_t size = 0, layers = 0;
    uint64_t occupiedTexels = 0;
    std::vector<Rectangle> rectangles; // sorted by chart, then captured light
    std::vector<uint8_t> texels;       // layer-major, exactly size * size * layers
    // Same chart-major lookup as compressed masks, with a page-record tag.
    std::vector<uint32_t> words(uint32_t charts) const;
  };
  // Decode the existing validated cache payload without changing visibility.
  // Limits include allocated page padding and the rectangle lookup records.
  // Failure to fit returns no result, allowing the compressed representation
  // to remain usable. Cancellation throws and never publishes partial pages.
  std::optional<MaskPages> packMaskPages(const Masks &masks, uint32_t pageSize,
                                       uint32_t maximumLayers, uint64_t maximumBytes,
                                       std::stop_token cancelled = {});
  // Use pages only when their allocated texels plus lookup texture are smaller
  // than today's compressed GPU representation. Dense compressed masks remain
  // available without changing cache format or dropping any chart/light pair.
  std::optional<MaskPages> selectMaskPages(const Masks &masks, uint32_t charts,
                                         uint32_t maximumDimension, uint32_t maximumLayers,
                                         std::stop_token cancelled = {});
} // namespace remix::forward::mixed

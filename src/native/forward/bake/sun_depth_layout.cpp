#include "forward/bake/sun_depth_layout.h"
#include <algorithm>
#include <stdexcept>
namespace remix::forward::bake {
  lightmap::Allocation SunDepthLayout::allocation(uint32_t slot) const {
    if (slot >= tiles.size()) throw std::out_of_range("Sun depth tile slot");
    const uint32_t perPage = tilesPerAxis * tilesPerAxis;
    const uint32_t local = slot % perPage, extent = options.tileTexels + options.border * 2;
    return {slot / perPage, local % tilesPerAxis * extent + options.border,
            local / tilesPerAxis * extent + options.border, options.tileTexels, options.tileTexels};
  }
  std::optional<SunDepthLayout> planSunDepthLayout(std::span<const std::array<int32_t, 2>> tiles,
      SunDepthLayoutOptions options, std::stop_token cancelled) {
    auto check = [&] { if (cancelled.stop_requested()) throw std::runtime_error("Sun depth layout superseded"); };
    check();
    if (!options.pageSize || options.pageSize > 16384 || !options.tileTexels ||
        options.tileTexels > options.pageSize || options.border > (options.pageSize - options.tileTexels) / 2 ||
        !options.maximumPages || options.maximumPages > 2048 ||
        !options.maximumDirectoryWords || options.maximumDirectoryWords > (1u << 20))
      throw std::invalid_argument("Invalid sun depth layout limits");
    if (tiles.size() > (1u << 20)) throw std::length_error("Too many sun depth tiles");
    SunDepthLayout result;
    result.options = options;
    result.tiles.assign(tiles.begin(), tiles.end());
    std::sort(result.tiles.begin(), result.tiles.end());
    result.tiles.erase(std::unique(result.tiles.begin(), result.tiles.end()), result.tiles.end());
    result.tilesPerAxis = options.pageSize / (options.tileTexels + options.border * 2);
    if (result.tiles.empty()) return result;
    result.minimum = result.tiles.front();
    auto maximum = result.minimum;
    for (const auto &tile : result.tiles) for (uint32_t axis = 0; axis < 2; ++axis) {
      result.minimum[axis] = std::min(result.minimum[axis], tile[axis]);
      maximum[axis] = std::max(maximum[axis], tile[axis]);
    }
    uint64_t words = 1;
    for (uint32_t axis = 0; axis < 2; ++axis) {
      const uint64_t count = uint64_t(int64_t(maximum[axis]) - result.minimum[axis] + 1);
      if (count > options.maximumDirectoryWords / words) return {};
      result.dimensions[axis] = uint32_t(count);
      words *= count;
    }
    const uint64_t perPage = uint64_t(result.tilesPerAxis) * result.tilesPerAxis;
    const uint64_t pages = (result.tiles.size() + perPage - 1) / perPage;
    if (pages > options.maximumPages) return {};
    result.pages = uint32_t(pages);
    result.depthBytes = pages * options.pageSize * options.pageSize * sizeof(float);
    result.directoryBytes = (words * sizeof(uint32_t) + 15) & ~uint64_t(15);
    if (result.depthBytes > options.maximumBytes || result.directoryBytes > options.maximumBytes - result.depthBytes)
      return {};
    result.directory.resize(size_t(words));
    for (uint32_t slot = 0; slot < result.tiles.size(); ++slot) {
      check();
      const auto &tile = result.tiles[slot];
      const uint32_t x = uint32_t(int64_t(tile[0]) - result.minimum[0]);
      const uint32_t y = uint32_t(int64_t(tile[1]) - result.minimum[1]);
      result.directory[size_t(y) * result.dimensions[0] + x] = slot + 1;
    }
    return result;
  }
}

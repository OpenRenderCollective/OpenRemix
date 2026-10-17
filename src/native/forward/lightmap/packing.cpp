#include "forward/lightmap/packing.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace remix::forward::lightmap {
  namespace {
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Surface packing superseded");
    }
  } // namespace
  Layout packCharts(const scene::BakeScene &scene, const Options &options, std::stop_token cancelled) {
    check(cancelled);
    if (!scene.ready || !std::isfinite(scene.sceneScale) || scene.sceneScale <= 0 || options.pageSize < 8 ||
        options.pageSize > 4096 || !std::has_single_bit(options.pageSize) || !options.maxPages ||
        options.maxPages > 256 || options.padding < 1 || options.padding > 16 || !options.maxTexels ||
        options.maxTexels > (64u << 20) || !options.texelStep || options.texelStep > 4096 ||
        !std::isfinite(options.luxelMetres) || options.luxelMetres < 0 || options.luxelMetres > 1000)
      throw std::invalid_argument("Invalid surface lightmap scene or atlas budget");
    try {
      scene::validateBakeCharts(scene.draws, scene.charts, cancelled);
    } catch (remixapi_ErrorCode) {
      throw std::invalid_argument("Invalid surface lightmap chart ownership or mapping");
    }
    Layout result;
    result.pageSize = options.pageSize;
    result.luxelMetres = options.luxelMetres * options.texelStep;
    result.mappings = parameterize(scene, options.luxelMetres, options.texelStep, cancelled);
    result.charts.resize(scene.charts.size());
    std::vector<uint32_t> order(scene.charts.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
      const auto &first = result.mappings[a], &second = result.mappings[b];
      return std::tie(first.height, first.width) > std::tie(second.height, second.width);
    });
    struct Shelf {
      uint32_t x = 0, y = 0, height = 0;
    };
    std::vector<Shelf> shelves;
    for (uint32_t index : order) {
      check(cancelled);
      const auto &chart = result.mappings[index];
      const uint32_t chartWidth = chart.width, chartHeight = chart.height;
      const uint32_t border = 2 * options.padding;
      if (border >= options.pageSize || chartWidth > options.pageSize - border ||
          chartHeight > options.pageSize - border)
        throw std::length_error("Surface lightmap chart exceeds atlas page size");
      const uint32_t width = chartWidth + border, height = chartHeight + border;
      uint32_t page = 0;
      for (; page < shelves.size(); ++page) {
        auto candidate = shelves[page];
        if (candidate.x + width > options.pageSize) {
          candidate.y += candidate.height;
          candidate.x = candidate.height = 0;
        }
        if (candidate.y + height <= options.pageSize) {
          shelves[page] = candidate;
          break;
        }
      }
      if (page == shelves.size()) {
        const uint64_t texels = uint64_t(page + 1) * options.pageSize * options.pageSize;
        if (page == options.maxPages || texels > options.maxTexels)
          throw std::length_error("Surface lightmap atlas budget exhausted");
        shelves.emplace_back();
      }
      auto &shelf = shelves[page];
      result.charts[index] = {page, shelf.x + options.padding, shelf.y + options.padding, chartWidth,
                              chartHeight};
      shelf.x += width;
      shelf.height = std::max(shelf.height, height);
    }
    result.pages = uint32_t(shelves.size());
    return result;
  }
} // namespace remix::forward::lightmap

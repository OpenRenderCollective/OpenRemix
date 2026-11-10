#include "forward/lightmap/packing.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
  std::vector<ChartShape> prepareCharts(const scene::BakeScene &scene, const Options &options,
                                        std::stop_token cancelled) {
    check(cancelled);
    if (!scene.ready || !std::isfinite(scene.sceneScale) || scene.sceneScale <= 0 || options.pageSize < 8 ||
        options.pageSize > 4096 || !std::has_single_bit(options.pageSize) || !options.maxPages ||
        options.maxPages > 256 || options.padding > 16 || !options.maxTexels ||
        options.maxTexels > (64u << 20) || !options.texelStep || options.texelStep > 4096 ||
        !std::isfinite(options.luxelMetres) || options.luxelMetres < 0 || options.luxelMetres > 1000)
      throw std::invalid_argument("Invalid surface lightmap scene or atlas budget");
    try {
      scene::validateBakeCharts(scene.draws, scene.charts, cancelled);
    } catch (remixapi_ErrorCode) {
      throw std::invalid_argument("Invalid surface lightmap chart ownership or mapping");
    }
    return prepareShapes(scene, options.luxelMetres > 0, cancelled);
  }
  Layout packCharts(std::span<const ChartShape> shapes, const Options &options, std::stop_token cancelled) {
    check(cancelled);
    Layout result;
    result.pageSize = options.pageSize;
    result.luxelMetres = options.luxelMetres * options.texelStep;
    result.mappings = parameterize(shapes, options.luxelMetres, options.texelStep, cancelled);
    if (const char *capture = std::getenv("REMIX_DEBUG_BAKE_CAPTURE_PREFIX"); capture && *capture) {
      uint64_t paddedArea = 0;
      uint32_t maximumWidth = 0, maximumHeight = 0;
      for (const auto &mapping : result.mappings) {
        const uint64_t width = uint64_t(mapping.width) + 2 * options.padding,
                       height = uint64_t(mapping.height) + 2 * options.padding;
        paddedArea += width * height;
        maximumWidth = std::max(maximumWidth, mapping.width);
        maximumHeight = std::max(maximumHeight, mapping.height);
      }
      std::fprintf(stderr, "Surface packing probe: step %u, %.3g m, %llu padded texels, largest %ux%u\n",
                   options.texelStep, result.luxelMetres, static_cast<unsigned long long>(paddedArea),
                   maximumWidth, maximumHeight);
    }
    result.charts.resize(shapes.size());
    std::vector<uint32_t> order(shapes.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
      const auto &first = result.mappings[a], &second = result.mappings[b];
      return std::tie(first.height, first.width) > std::tie(second.height, second.width);
    });
    struct Shelf {
      uint32_t x = 0, y = 0, height = 0;
    };
    // Retain every row: short charts can fill earlier gaps instead of forcing
    // a new row/page whenever the most recently opened row is full.
    struct Page {
      uint32_t height = 0;
      std::vector<Shelf> shelves;
    };
    std::vector<Page> pages;
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
      uint32_t row = ~0u, remaining = ~0u;
      for (uint32_t p = 0; p < pages.size(); ++p) {
        for (uint32_t r = 0; r < pages[p].shelves.size(); ++r) {
          const auto &shelf = pages[p].shelves[r];
          if (height <= shelf.height && width <= options.pageSize - shelf.x &&
              options.pageSize - shelf.x - width < remaining) {
            page = p;
            row = r;
            remaining = options.pageSize - shelf.x - width;
          }
        }
      }
      if (row == ~0u) {
        for (page = 0; page < pages.size(); ++page)
          if (height <= options.pageSize - pages[page].height)
            break;
        if (page == pages.size()) {
          const uint64_t texels = uint64_t(page + 1) * options.pageSize * options.pageSize;
          if (page == options.maxPages || texels > options.maxTexels)
            throw std::length_error("Surface lightmap atlas budget exhausted");
          pages.emplace_back();
        }
        auto &target = pages[page];
        row = uint32_t(target.shelves.size());
        target.shelves.push_back({0, target.height, height});
        target.height += height;
      }
      auto &shelf = pages[page].shelves[row];
      result.charts[index] = {page, shelf.x + options.padding, shelf.y + options.padding, chartWidth,
                              chartHeight};
      shelf.x += width;
      shelf.height = std::max(shelf.height, height);
    }
    result.pages = uint32_t(pages.size());
    return result;
  }
} // namespace remix::forward::lightmap

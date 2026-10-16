#pragma once
#include "forward/bake/data.h"
#include <stop_token>

namespace remix::forward::lightmap {
  constexpr uint32_t unowned = ~0u;
  struct Options {
    uint32_t pageSize = 1024, maxPages = 16, padding = 2;
    uint64_t maxTexels = 4u << 20;
    uint32_t texelStep = 1; // coarsen sampling while keeping both chart endpoints
  };
  struct Allocation {
    uint32_t page = 0, x = 0, y = 0, width = 0, height = 0;
  };
  struct Layout {
    uint32_t pageSize = 0, pages = 0;
    std::vector<Allocation> charts;  // indexed like BakeScene::charts
    std::vector<bake::Point> points; // actual world-surface samples, never probe reconstruction
    std::vector<uint32_t> pointCharts; // chart ownership of each actual surface sample
    std::vector<uint32_t> addresses; // atlas texel for each point
    // Atlas texel -> point, including chart-local dilation/padding. Unused
    // space is unowned; padding never borrows a different chart's light data.
    std::vector<uint32_t> owners;
  };
  // CPU-only planning for the existing hardware bake transport. Throws on
  // invalid/folded charts or exhausted budgets rather than silently dropping
  // static surfaces. Does not allocate GPU resources or publish lighting.
  Layout plan(const scene::BakeScene &scene, Options options = {}, std::stop_token cancelled = {});
} // namespace remix::forward::lightmap

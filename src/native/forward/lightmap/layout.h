#pragma once
#include "forward/bake/data.h"
#include "forward/lightmap/parameterization.h"
#include <stop_token>

namespace remix::forward::lightmap {
  constexpr uint32_t unowned = ~0u;
  struct Options {
    uint32_t pageSize = 1024, maxPages = 16, padding = 2; // zero is valid with explicit sampling bounds
    uint64_t maxTexels = 4u << 20;
    uint32_t texelStep = 1; // global spacing multiplier; authored fallback retains chart endpoints
    float luxelMetres = 0;  // zero preserves authored UVs; positive selects world-metric surface spacing
  };
  struct Allocation {
    uint32_t page = 0, x = 0, y = 0, width = 0, height = 0;
  };
  struct Layout {
    uint32_t pageSize = 0, pages = 0, seamSamples = 0;
    uint32_t curvedSeamSamples = 0;
    float luxelMetres = 0; // effective surface spacing after global budget coarsening
    std::vector<Mapping> mappings;
    std::vector<Allocation> charts;    // indexed like BakeScene::charts
    std::vector<bake::Point> points;   // actual world-surface samples, never probe reconstruction
    std::vector<uint32_t> pointCharts; // chart ownership of each actual surface sample
    // Receiver checks can share an address with an extended filtering sample.
    std::vector<uint32_t> addresses;
    std::vector<uint8_t> receiverSamples; // true only for samples on the receiving chart itself
    std::vector<uint8_t> footprints;    // conservative bilinear support, excluding unused chart-box interiors
    std::vector<uint8_t> sampleCenters; // sample lies at its integer grid coordinate
    // Atlas texel -> point, including dilation and verified coplanar completion.
    // Every shading donor retains the receiving chart's mixed-light identities.
    std::vector<uint32_t> owners;
  };
  // CPU-only planning for the existing hardware bake transport. Throws on
  // invalid/folded charts or exhausted budgets rather than silently dropping
  // static surfaces. Does not allocate GPU resources or publish lighting.
  Layout plan(const scene::BakeScene &scene, Options options = {}, std::stop_token cancelled = {});
  // Find a fitting global density, refining the interval between exponential
  // budget probes. Returns a verified fit; shelf packing is not globally optimal.
  Layout fit(const scene::BakeScene &scene, Options options = {}, std::stop_token cancelled = {});
} // namespace remix::forward::lightmap

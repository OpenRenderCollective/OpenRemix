#include "forward/bake/job.h"
#include "forward/bake/cache.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace remix::forward::bake {
  void Controller::Job::prepareSurfaceStorage() {
    const char *override = std::getenv("REMIX_DEBUG_UNIFIED_MASKS");
    if (surfacePixels.empty()) return;
    const auto began = std::chrono::steady_clock::now();
    const uint64_t visibilityBytes = uint64_t(surfaceLayout.pageSize) * surfaceLayout.pageSize * surfaceLayout.pages * 8;
    const uint64_t budget = std::min<uint64_t>(mixed::Masks::maximumPublishedBytes,
        mixed::Masks::maximumBytes + visibilityBytes);
    if (!override || std::strcmp(override, "0") != 0)
      unifiedMasks = mixed::unifyMasks(*mixedLights, surfaceLayout, surfacePixels, budget, cancellation.get_token());
    std::fprintf(stderr, "Forward unified visibility preparation: %.3f ms, fits %u\n",
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count(),
        unsigned(bool(unifiedMasks)));
    const char *directional = std::getenv("REMIX_DEBUG_DIRECTIONAL_FIELDS");
    if (directional && std::strcmp(directional, "0") == 0) return;
    const auto directBegan = std::chrono::steady_clock::now();
    size_t owners = 0;
    try {
      const auto fields = mixed::retainedDirectionals(*mixedLights, surfaceLayout, surfacePixels,
          mixed::DirectionalFields::maximumBytes, cancellation.get_token());
      if (fields) {
        owners = fields->size();
        // An empty directory can also replace the otherwise unused nine
        // directional layers when no local light retains direct attribution.
        directionalWords = fields->words(cancellation.get_token());
      }
    } catch (const std::bad_alloc &) {
      // This is an optional, complete replacement. Memory pressure keeps the
      // existing atlas; cancellation and invalid input still propagate.
      directionalWords.clear();
    }
    std::fprintf(stderr, "Forward directional field preparation: %.3f ms, %zu owners, %zu encoded bytes\n",
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - directBegan).count(),
        owners, directionalWords.size() * sizeof(uint32_t));
  }
  std::unique_ptr<const cache::Snapshot> Controller::Job::takeCacheSnapshot() {
    auto snapshot = std::make_unique<cache::Snapshot>();
    snapshot->grid.header = grid.header;
    snapshot->grid.probes = std::move(grid.probes);
    snapshot->grid.tasks = std::move(grid.tasks);
    // Keep only serialized allocations/mappings, never transient sample arrays.
    snapshot->layout.pageSize = surfaceLayout.pageSize;
    snapshot->layout.pages = surfaceLayout.pages;
    snapshot->layout.luxelMetres = surfaceLayout.luxelMetres;
    snapshot->layout.seamSamples = surfaceLayout.seamSamples;
    snapshot->layout.curvedSeamSamples = surfaceLayout.curvedSeamSamples;
    snapshot->layout.charts = std::move(surfaceLayout.charts);
    snapshot->layout.mappings = std::move(surfaceLayout.mappings);
    snapshot->mixed.lights = std::move(mixedLights->lights);
    snapshot->mixed.charts = std::move(mixedLights->charts);
    snapshot->mixed.directCharts = std::move(mixedLights->directCharts);
    snapshot->mixed.overflow = mixedLights->overflow;
    snapshot->mixed.occlusion = std::move(mixedLights->occlusion);
    snapshot->mixed.masks = std::move(mixedLights->masks);
    snapshot->captures = std::move(captures);
    snapshot->specular = std::move(specular);
    snapshot->pixels = std::move(surfacePixels);
    snapshot->styles = std::move(styleWords);
    snapshot->localProbes = std::move(localProbes);
    snapshot->surfaceSamples = surfaceReceived;
    snapshot->directDiffuse = hasDirectDiffuse;
    snapshot->sun = hasSun;
    return snapshot;
  }
}

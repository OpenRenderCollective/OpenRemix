#include "forward/bake/fingerprint.h"
#include "forward/bake/job.h"
#include "forward/bake/receiver_report.h"
#include "forward/bake/storage_options.h"
#include "forward/bake/styles.h"
#include "forward/lightmap/data.h"
#include "forward/lightmap/denoise.h"
#include "forward/lightmap/upload.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace remix::forward::bake {
namespace {
template <typename T> void releaseStorage(std::vector<T> &values) {
  std::vector<T>().swap(values);
}
} // namespace
void Controller::Job::finishDiffuseAssets(
    VkPhysicalDevice physical, VkDevice device, const Config &config,
    std::span<const probes::Capture> diagnosticCaptures) {
  if (config.denoiseLightmaps && lightmap::denoiserVersion()) {
    finishStage.store(Job::FinishStage::Denoising, std::memory_order_relaxed);
    auto progress = [this](uint32_t completed, uint32_t total) {
      denoisedCharts.store(completed, std::memory_order_relaxed);
      denoiseChartCount.store(total, std::memory_order_relaxed);
    };
    const auto filtered = lightmap::denoise(
        surfaceLayout, surfaces, cancellation.get_token(), std::move(progress));
    std::fprintf(stderr,
                 "Forward lightmap OIDN: %u charts, %llu texels, device type "
                 "%d, %.3f s\n",
                 filtered.charts,
                 static_cast<unsigned long long>(filtered.texels),
                 filtered.deviceType, filtered.seconds);
    std::fprintf(stderr,
                 "Forward lightmap OIDN work: %u executions, %u preparations, "
                 "%u constant fields, %u monitored, %u batches, %u waits; %llu "
                 "host bytes, %llu image bytes, "
                 "%llu transfer bytes\n",
                 filtered.executions, filtered.preparations,
                 filtered.constantFields, filtered.monitoredExecutions,
                 filtered.batches, filtered.synchronizations,
                 static_cast<unsigned long long>(filtered.peakHostBytes),
                 static_cast<unsigned long long>(filtered.imageBufferBytes),
                 static_cast<unsigned long long>(filtered.transferBytes));
    std::fprintf(stderr,
                 "Forward lightmap OIDN graphs: %u captures, %u launches, %u "
                 "fallbacks\n",
                 filtered.graphCaptures, filtered.graphLaunches,
                 filtered.graphFallbacks);
    std::fprintf(
        stderr,
        "Forward lightmap OIDN CPU schedule: %u queues, %u threads per queue\n",
        filtered.cpuQueues, filtered.cpuThreadsPerQueue);
    std::fprintf(stderr,
                 "Forward lightmap OIDN summed worker time: initialize %.3f s, "
                 "prepare %.3f s, "
                 "upload %.3f s, execute %.3f s, readback %.3f s, wait %.3f s, "
                 "chart %.3f s\n",
                 filtered.initializationSeconds, filtered.preparationSeconds,
                 filtered.uploadSeconds, filtered.executionSeconds,
                 filtered.readbackSeconds, filtered.waitSeconds,
                 filtered.chartSeconds);
    std::fprintf(stderr,
                 "Forward lightmap OIDN staging: %llu pinned bytes, %.3f s "
                 "host setup/copies; "
                 "%llu GPU batch bytes\n",
                 static_cast<unsigned long long>(filtered.stagingBufferBytes),
                 filtered.stagingSeconds,
                 static_cast<unsigned long long>(filtered.batchBufferBytes));
  }
  dumpPhaseFingerprint("filtered", source->contentHash, grid, surfaceLayout,
                       surfaces, sun, diagnosticCaptures);
  finishStage.store(Job::FinishStage::Atlas, std::memory_order_relaxed);
  surfacePixels =
      lightmap::pixels(surfaceLayout, surfaces, cancellation.get_token(),
                       visibility, direct, sun);
  if (reference)
    reference->reportGrid(grid, config.aggregateDiffuse ||
                                    config.fullyBakedLights ||
                                    config.bakedGeometryLights);
  if (coarseTransferCpuReportsRequested() && coarseTransferWords) {
    auto candidate = grid;
    candidate.coarseTransferWords = coarseTransferWords;
    dumpReceiverReport(*source, surfaceLayout, surfacePixels, candidate,
                       cancellation.get_token());
  } else {
    dumpReceiverReport(*source, surfaceLayout, surfacePixels, grid,
                       cancellation.get_token());
  }
  // These transport arrays are no longer needed once encoded. Avoid
  // retaining them beside the much larger atlas and upload allocation.
  releaseStorage(visibility);
  releaseStorage(direct);
  releaseStorage(sun);
  const char *samples = std::getenv("REMIX_DEBUG_BAKE_SURFACE_SAMPLES");
  if (config.styledIndirect && !styleIds(*source).empty()) {
    VkPhysicalDeviceProperties limits;
    vkGetPhysicalDeviceProperties(physical, &limits);
    const uint64_t baseBytes = gridBytes(grid);
    const uint64_t localBytes = localProbes ? localProbes->gpuBytes() : 0;
    if (baseBytes + localBytes + 16 >= limits.limits.maxStorageBufferRange)
      throw std::length_error(
          "No device storage range for attributed lighting");
    const uint64_t maximum =
        std::min(uint64_t(config.styleStorageMiB) << 20,
                 uint64_t(limits.limits.maxStorageBufferRange) - baseBytes -
                     localBytes - 15);
    styles = std::make_unique<StyleWork>(*source, surfaceLayout, grid, maximum);
    styles->takeSurfaces(surfaces, surfaceLayout.points.size());
  } else if (!samples || std::strcmp(samples, "1") != 0) {
    releaseStorage(surfaces);
  }
  finishStage.store(Job::FinishStage::Geometry, std::memory_order_relaxed);
  if (!source->charts.empty())
    surfaceGeometry = std::make_shared<lightmap::Geometry>(
        physical, device, source, surfaceLayout, cancellation.get_token(),
        mixedLights->charts, mixedLights->directCharts);
  prepareSurfaceStorage();
  VkPhysicalDeviceProperties groupLimits;
  vkGetPhysicalDeviceProperties(physical, &groupLimits);
  if (!styles)
    prepareGroupedStyles(groupLimits.limits.maxStorageBufferRange, config);
  baseFinished = true;
  filtering = !styles;
}
} // namespace remix::forward::bake

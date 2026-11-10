#include "forward/bake/job.h"
#include "forward/bake/transport.h"
#include "forward/probes/placement.h"
#include "forward/probes/source2_reference.h"
#include <algorithm>
#include <stdexcept>

namespace remix::forward::bake {
  void Controller::Job::retainReflectionSeeds() {
    if (reflectionSeeds) return;
    // Reuse the placement provider's existing compact allocation when possible.
    // Diffuse-only bakes still retain their admitted points for a later enable.
    auto seeds = reflections.seeds.bounds.valid ? std::move(reflections.seeds)
        : probes::placementSeeds(*source, grid, cancellation.get_token());
    reflectionSeeds = std::make_shared<const probes::PlacementSeeds>(std::move(seeds));
  }
  void Controller::Job::prepareReflectionRefresh(VkPhysicalDevice physical, VkDevice device,
                                                  const Config &config, float cutoff, uint32_t family) {
    irradianceContents = config.irradianceContents;
    reflections.reference = probes::source2Reference(source->lightingHash(scene::BakeLightScope::Combined));
    if (!reflectionSeeds || !reflectionOwnership)
      throw std::logic_error("Reflection refresh requires published diffuse admission and ownership");
    // This job borrows immutable publication metadata, never its SH or atlas pixels.
    Options options;
    options.directLighting = captureDirectPolicy();
    options.lightingComponents = config.styledIndirect && !styleIds(*source).empty();
    options.batchPoints = Budget::maximumPoints;
    options.rayBudget = Budget::maximumWork;
    options.initialBatchPoints = 1024;
    options.queueFamily = family;
    options.textureResolution = config.textureResolution;
    transport = std::make_unique<Transport>();
    transport->initialize(physical, device, source, options);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    cacheDevice = {properties.vendorID, properties.deviceID, properties.driverVersion,
        properties.limits.maxImageDimension2D, properties.limits.maxImageArrayLayers,
        properties.limits.maxStorageBufferRange, {}};
    std::copy_n(properties.pipelineCacheUUID, cacheDevice.uuid.size(), cacheDevice.uuid.begin());
    if (config.diskCache) {
      cacheKey = cache::key(source->contentHash, config, cutoff, cacheDevice, options.directLighting,
                            !source->partition || !source->partition->closed, source.get());
      try { cacheFile = cache::path(cacheKey); } catch (const std::exception &) {}
    }
    reflectionRecovery = true; // use capture scheduling, skipping every diffuse stage
  }
}

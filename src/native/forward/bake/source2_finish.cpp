#include "forward/bake/job.h"
#include "forward/bake/fingerprint.h"
#include "forward/bake/visibility_report.h"
#include "forward/probes/capture_cache.h"

namespace remix::forward::bake {
  void Controller::Job::finishSource2Recipe(
      VkPhysicalDevice physical, VkDevice device, const Config &config) {
    // Preserve exact input provenance on the worker, then release ray
    // scratch before filtered cubemaps, denoising and atlas encoding.
    // Publication must not scan this potentially large buffer again.
    reflections.complete(cancellation.get_token());
    const auto reflectionFingerprint = reflectionRecovery ? ReflectionFingerprint{}
        : fingerprintCaptures(reflections.captureView());
    if (!reflectionRecovery) {
      dumpPhaseFingerprint("transport", source->contentHash, grid, surfaceLayout, surfaces,
                           sun, reflectionFingerprint);
      dumpVisibilityReport(*mixedLights, surfaceLayout,
                            Options{}.bias / source->sceneScale, cancellation.get_token());
    }
    finishStage.store(Job::FinishStage::Reflections, std::memory_order_relaxed);
    if (inMemoryDiffuse && reflectionCacheKey && !reflections.cachedRadiance) {
      auto payload = reflections.finishTransferredPublication(
          physical, device, cancellation.get_token());
      if (probes::capture_cache::retainedCharge(*payload) <= probes::capture_cache::maximumBytes)
        reflectionPersistence = std::move(payload);
    } else
      reflections.finishRetainedPublication(physical, device, cancellation.get_token());
    if (!reflectionRecovery) {
      finishDiffuseAssets(physical, device, config, reflectionFingerprint);
      // Source 2 recipe transition: capture admission and scheduling
      // flags are not outputs of the diffuse asset encoder.
      retainReflectionSeeds();
      baseFinished = true;
      filtering = !styles;
    }
  }
}

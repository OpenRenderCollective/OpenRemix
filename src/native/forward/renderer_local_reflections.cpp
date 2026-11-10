#include "forward/gpu.h"
#include "forward/probes/receiver_mask.h"
#include "raster/renderer.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace remix::forward {
  const probes::ImageAsset *Renderer::Gpu::reflectionImage() const {
    if (const auto selected = localSelections.active()) return selected->image.get();
    if (const auto captured = captureProviders.image(techniquePlan.capturePolicy.transport)) return captured;
    return baking->reflectionImage();
  }
  const probes::Header *Renderer::Gpu::reflectionHeader() const {
    const auto *image = reflectionImage();
    return image ? &image->header() : nullptr;
  }
  probes::Descriptors Renderer::Gpu::reflectionDescriptor() const {
    const auto *image = reflectionImage();
    if (!image) return baking->reflectionDescriptor();
    const auto &volume = baking->volume();
    return image->descriptors(volume ? volume->reflectionReference : probes::ReferenceProvenance{});
  }
  const probes::LayerIdentities *Renderer::Gpu::reflectionIdentities() const {
    if (const auto selected = localSelections.active()) return &selected->identities;
    return captureProviders.authored()->identities();
  }
  void Renderer::Gpu::prepareReflectionMasks(std::span<water::MaterialData> waterMaterials) {
    static const bool unculledWater = std::getenv("REMIX_DEBUG_FORWARD_WATER_UNCULLED_PROBES") != nullptr;
    const bool maskWater = !waterMaterials.empty() && !unculledWater;
    if (!techniquePlan.inlineReflections && !maskWater) return;
    const auto *header = reflectionHeader();
    if (!header) return;
    const float bias = options.reflectionSampleBias();
    // Layer order and projection are immutable within one publication. Compare
    // contents because the allocator can reuse the address of an old header.
    if (!reflectionMaskHeader || reflectionMaskBias != bias ||
        std::memcmp(&*reflectionMaskHeader, header, sizeof(*header)) != 0) {
      reflectionMaskHeader = *header;
      reflectionMaskBias = bias;
      ++reflectionMaskRevision;
    }
    const auto mask = [&](const Command &draw) {
      auto &geometry = *draw.geometry;
      const auto &bounds = draw.worldBounds;
      const auto &cached = geometry.reflectionMaskBounds;
      if (geometry.reflectionMaskRevision != reflectionMaskRevision || bounds.valid != cached.valid ||
          bounds.minimum != cached.minimum || bounds.maximum != cached.maximum) {
        geometry.reflectionMask = probes::receiverMask(*header, bounds, bias);
        geometry.reflectionMaskBounds = bounds;
        geometry.reflectionMaskRevision = reflectionMaskRevision;
      }
      return geometry.reflectionMask;
    };
    auto &frame = frames[slot];
    if (techniquePlan.inlineReflections) {
      for (auto &draw : frame.commands)
        if (draw.cameraVisible) draw.reflectionCandidates = mask(draw);
    }
    if (maskWater) {
      for (const auto &draw : frame.waterCommands) {
        if (!draw.cameraVisible) continue;
        const auto candidates = mask(draw);
        waterMaterials[draw.waterIndex].probeCandidates = {candidates[0], candidates[1], 0, 0};
      }
    }
  }
  uint32_t Renderer::Gpu::reflectionLayer(uint64_t sceneId, uint64_t volumeId) const {
    if (const auto selected = localSelections.active()) {
      if (sceneId != selected->context.sceneId || !volumeId) return 0;
      for (uint32_t i = 0; i < selected->image->header().parameters[0]; ++i)
        if (selected->identities[i] == volumeId) return i + 1;
      return 0;
    }
    return captureProviders.authored()->receiverLayer(sceneId, volumeId);
  }
  void Renderer::Gpu::prepareLocalReflections(VkCommandBuffer command, RasterRenderer &resources) {
    const char *request = std::getenv("REMIX_DEBUG_PROBE_LOCAL_SELECTION");
    const bool enabled = request && std::strcmp(request, "1") == 0;
    if (!enabled && !localSelections.needsContext() &&
        (!localSelectionPreparation || !localSelectionPreparation->pending())) return;
    const auto source = resources.bakeScene();
    const auto snapshot = captureProviders.authored()->snapshot(reflectionSource());
    const auto local = baking->localReflectionSource();
    // Disabled/author-only sets cannot accept automatic replacements. Retire
    // existing selections without scheduling a worker that can only return empty.
    const bool automaticAllowed = !snapshot.baseline.disabled && snapshot.baseline.automaticAllowed;
    const bool available = enabled && automaticAllowed && localSelectionPreparation && source && source->ready &&
        snapshot.image && local && local->image && local->ready && !local->ready->layers.empty() && local->evidence &&
        probes::matchesCoverageContext(local->ready->context, *source, local->ready->context.baseline);
    if (!available) {
      localSelections.invalidate();
      localSelectionAttempt.reset();
      if (localSelectionPreparation) localSelectionPreparation->cancel();
    } else if (!localSelectionAttempt ||
        !probes::matchesCoverageContext(*localSelectionAttempt, *source, snapshot.baseline) ||
        localSelectionBaseline.lock() != snapshot.image || localSelectionImage.lock() != local->image) {
      localSelections.invalidate();
      localSelectionAttempt = probes::coveragePlanContext(*source, snapshot.baseline);
      localSelectionBaseline = snapshot.image;
      localSelectionImage = local->image;
      try {
        localSelectionPreparation->start(source, snapshot, local->evidence, local->ready, local->image);
      } catch (const std::exception &error) {
        std::fprintf(stderr, "Local reflection preparation refused: %s\n", error.what());
      }
    }
    if (available) {
      try {
        if (auto upload = localSelectionPreparation->poll(*source, snapshot, local->image))
          localSelections.offer(std::move(upload));
      } catch (VkResult error) {
        std::fprintf(stderr, "Local reflection worker Vulkan failure: %d\n", int(error));
      } catch (const std::exception &error) {
        std::fprintf(stderr, "Local reflection worker refused: %s\n", error.what());
      }
    }
    // Context is supplied again after selectFrame observed the copy fence.
    // Descriptors, header and IDs all consult this one active publication.
    const scene::BakeScene empty;
    localSelections.record(command, source ? *source : empty, snapshot.baseline);
  }
}

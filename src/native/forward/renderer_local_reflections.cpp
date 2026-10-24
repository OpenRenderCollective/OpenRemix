#include "forward/gpu.h"
#include "raster/renderer.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace remix::forward {
  const probes::Header *Renderer::Gpu::reflectionHeader() const {
    if (const auto selected = localSelections.active()) return &selected->image->header();
    if (const auto authored = authoredReflections->header()) return authored;
    const auto &volume = baking->volume();
    return volume ? &volume->reflectionHeader : nullptr;
  }
  const probes::LayerIdentities *Renderer::Gpu::reflectionIdentities() const {
    if (const auto selected = localSelections.active()) return &selected->identities;
    return authoredReflections->identities();
  }
  uint32_t Renderer::Gpu::reflectionLayer(uint64_t sceneId, uint64_t volumeId) const {
    if (const auto selected = localSelections.active()) {
      if (sceneId != selected->context.sceneId || !volumeId) return 0;
      for (uint32_t i = 0; i < selected->image->header().parameters[0]; ++i)
        if (selected->identities[i] == volumeId) return i + 1;
      return 0;
    }
    return authoredReflections->receiverLayer(sceneId, volumeId);
  }
  void Renderer::Gpu::prepareLocalReflections(VkCommandBuffer command, RasterRenderer &resources) {
    const char *request = std::getenv("REMIX_DEBUG_PROBE_LOCAL_SELECTION");
    const bool enabled = request && std::strcmp(request, "1") == 0;
    if (!enabled && !localSelections.needsContext() &&
        (!localSelectionPreparation || !localSelectionPreparation->pending())) return;
    const auto source = resources.bakeScene();
    const auto &volume = baking->volume();
    const auto snapshot = authoredReflections->snapshot(volume);
    const auto local = volume ? volume->localReflections : nullptr;
    // Disabled/author-only sets cannot accept automatic replacements. Retire
    // existing selections without scheduling a worker that can only return empty.
    const bool automaticAllowed = !snapshot.baseline.disabled && snapshot.baseline.automaticAllowed;
    const bool available = enabled && automaticAllowed && localSelectionPreparation && source && source->ready &&
        snapshot.image && local && local->image && local->ready && !local->ready->layers.empty() && volume->reflectionEvidence &&
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
        localSelectionPreparation->start(source, snapshot, volume->reflectionEvidence, local->ready, local->image);
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

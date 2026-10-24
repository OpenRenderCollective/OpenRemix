#include "forward/gpu.h"
#include "raster/renderer.h"
#include <chrono>
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
    if (!enabled && !localSelections.needsContext()) return;
    const auto source = resources.bakeScene();
    const auto &volume = baking->volume();
    const auto snapshot = authoredReflections->snapshot(volume);
    const auto local = volume ? volume->localReflections : nullptr;
    const bool available = enabled && source && source->ready && snapshot.image && local && local->image &&
        local->ready && !local->ready->layers.empty() && volume->reflectionEvidence &&
        probes::matchesCoverageContext(local->ready->context, *source, local->ready->context.baseline);
    if (!available) {
      localSelections.invalidate();
      localSelectionAttempt.reset();
    } else if (!localSelectionAttempt ||
        !probes::matchesCoverageContext(*localSelectionAttempt, *source, snapshot.baseline) ||
        localSelectionBaseline.lock() != snapshot.image || localSelectionImage.lock() != local->image) {
      localSelections.invalidate();
      localSelectionAttempt = probes::coveragePlanContext(*source, snapshot.baseline);
      localSelectionBaseline = snapshot.image;
      localSelectionImage = local->image;
      try {
        const auto began = std::chrono::steady_clock::now();
        std::vector<uint32_t> eligible;
        for (const auto &layer : local->ready->layers) eligible.push_back(layer.candidate);
        // Restrict before greedy planning: dropping unavailable replacements
        // afterwards could invalidate another replacement's preservation proof.
        const auto plan = probes::planReceiverCoverage(*source, snapshot.baseline, *volume->reflectionEvidence, {}, eligible);
        if (!plan.replacements.empty()) {
          auto ready = *local->ready;
          ready.context = plan.context;
          // Radiance belongs to the same frozen scene. The builder also checks
          // every selected box/frame/origin against the freshly approved plan.
          const auto imagePlan = probes::buildCoverageImagePlan(*source, snapshot.baseline, plan,
              snapshot.image->header(), snapshot.identities, ready);
          localSelections.offer(std::make_unique<probes::SelectionUpload>(physical, device, imagePlan,
                                                                          snapshot.image, local->image));
          const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
          std::fprintf(stderr, "Local reflection selection: %zu replacements, %u output layers, %.3f ms preparation\n",
              plan.replacements.size(), imagePlan.header.parameters[0], ms);
        }
      } catch (const std::exception &error) {
        std::fprintf(stderr, "Local reflection selection refused: %s\n", error.what());
      }
    }
    // Context is supplied again after selectFrame observed the copy fence.
    // Descriptors, header and IDs all consult this one active publication.
    const scene::BakeScene empty;
    localSelections.record(command, source ? *source : empty, snapshot.baseline);
  }
}

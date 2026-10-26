#include "runtime/runtime.h"
#include "forward/bake/controller.h"
#include <algorithm>
#include <iomanip>
#include <locale>
#include <sstream>

namespace remix {
  forward::Statistics::CpuRecording RemixNativeRuntime::forwardCpuRecording() const {
    return forwardStatistics().cpuRecording;
  }
  forward::Statistics RemixNativeRuntime::forwardStatistics() const {
    if (const auto *busy = busyState())
      return busy->forward;
    auto statistics = m_forward ? m_forward->statistics() : forward::Statistics{};
    // A swapchain/attachment replacement can temporarily have no renderer,
    // while its lighting controller and published volume remain alive.
    if (!m_forward && m_forwardBaking) statistics.baking = m_forwardBaking->statistics();
    return statistics;
  }
  std::string RemixNativeRuntime::reflectionProbeReport() const {
    // Read the same published frame snapshot as the statistics API. Never inspect
    // a mutable fitting job or read GPU memory to produce diagnostics.
    const auto state = forwardStatistics();
    const auto &header = state.reflectionProbes;
    const auto count = std::min(header.parameters[0], forward::probes::maxProbes);
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(9) << "{\n  \"version\": 1,\n  \"activeHash\": \"" << state.baking.activeHash
        << "\",\n  \"contentHashes\": {\"grid\": \"" << state.bakeContent.grid << "\", \"reflections\": \""
        << state.bakeContent.reflections << "\", \"surfaces\": \"" << state.bakeContent.surfaces
        << "\", \"staticOcclusion\": \"" << state.bakeContent.staticOcclusion
        << "\", \"captureRadiance\": \"" << state.bakeContent.captureRadiance << "\", \"captureInputs\": \""
        << state.bakeContent.captureInputs << "\", \"styledIndirect\": \"" << state.bakeContent.styledIndirect << "\"}"
        << ",\n  \"rendering\": {\"lights\": " << state.lights
        << ", \"samples\": " << state.samples
        << ", \"debugView\": " << state.debugView
        << ", \"clusteredLights\": " << state.clusteredLights
        << ", \"clusterBuildLights\": " << state.clusterBuildLights
        << ", \"analyticLights\": " << state.analyticLights
        << ", \"aggregateLights\": " << state.aggregateLights
        << ", \"selectedAggregateLights\": " << state.selectedAggregateLights
        << ", \"selectedHighlightLights\": " << state.selectedHighlightLights
        << ", \"selectedMixedShadowLights\": " << state.selectedMixedShadowLights
        << ", \"requiredMixedLights\": " << state.requiredMixedLights
        << ", \"bakedFallbackLights\": " << state.fullyBakedLights << ", \"draws\": " << state.draws
        << ", \"clusters\": " << state.clusters << ", \"clusterCapacity\": " << state.clusterLightCapacity
        << ", \"staticShadowUpdates\": " << state.staticShadowUpdates
        << ", \"staticSunUpdates\": " << state.staticSunUpdates
        << ", \"capturedCasters\": " << state.capturedCasters
        << ", \"liveCasters\": " << state.liveCasters
        << ", \"emptyMovingShadowMaps\": " << state.emptyMovingShadowMaps
        << ", \"staticCasterPreparations\": " << state.staticCasterPreparations << '}'
        << ",\n  \"probeVisibility\": {\"lights\": " << state.probeVisibility.lights
        << ", \"groups\": " << state.probeVisibility.groups
        << ", \"fieldBytes\": " << state.probeVisibility.fieldBytes
        << ", \"residentBytes\": " << state.probeVisibility.residentBytes
        << ", \"peakResidentBytes\": " << state.probeVisibility.peakResidentBytes
        << ", \"selectionHash\": \"" << state.probeVisibility.selectionHash << '"'
        << ", \"recordedGathers\": " << state.probeVisibility.recordedGathers
        << ", \"completedReuses\": " << state.probeVisibility.completedReuses << '}'
        << ",\n  \"exposure\": {\"current\": " << state.postProcessing.exposure
        << ", \"target\": " << state.postProcessing.target << ", \"limit\": " << state.postProcessing.limit
        << ", \"milliseconds\": " << state.postProcessing.milliseconds
        << ", \"memoryBytes\": " << state.postProcessing.memoryBytes
        << ", \"meterFrames\": " << state.postProcessing.meterFrames
        << ", \"adapting\": " << (state.postProcessing.adapting ? "true" : "false") << '}'
        << ",\n  \"authoredReflections\": {\"revision\": " << state.authoredReflections.revision
        << ", \"publishedSceneId\": \"" << state.authoredReflections.publishedSceneId << '"'
        << ", \"publishedRevision\": " << state.authoredReflections.publishedRevision
        << ", \"pending\": " << (state.authoredReflections.pending ? "true" : "false")
        << ", \"active\": " << (state.authoredReflections.active ? "true" : "false")
        << ", \"rays\": " << state.authoredReflections.rays
        << ", \"captureScratchBytes\": " << state.authoredReflections.captureScratchBytes
        << ", \"completed\": " << state.authoredReflections.completed
        << ", \"reused\": " << state.authoredReflections.reused
        << ", \"automatic\": " << state.authoredReflections.automatic
        << ", \"covered\": " << state.authoredReflections.covered
        << ", \"omitted\": " << state.authoredReflections.omitted
        << ", \"sceneReused\": " << (state.authoredReflections.sceneReused ? "true" : "false") << '}'
        << ",\n  \"receiverReflections\": {\"boundDraws\": " << state.boundReflectionDraws
        << ", \"unresolvedDraws\": " << state.unresolvedReflectionDraws << '}'
        << ",\n  \"irradianceGrid\": {\"spacingMetres\": " << state.irradianceGrid.spacingMetres
        << ", \"dimensions\": [" << state.irradianceGrid.dimensions[0] << ", "
        << state.irradianceGrid.dimensions[1] << ", " << state.irradianceGrid.dimensions[2]
        << "], \"slots\": " << state.irradianceGrid.slots << ", \"usable\": "
        << state.irradianceGrid.usable
        << ", \"finestSpacingMetres\": " << state.irradianceGrid.finestSpacingMetres
        << ", \"coarseRecords\": " << state.irradianceGrid.coarseRecords
        << ", \"patchCounts2x4x8x\": [" << state.irradianceGrid.patchCounts[0] << ", "
        << state.irradianceGrid.patchCounts[1] << ", " << state.irradianceGrid.patchCounts[2] << "]}"
        << ",\n  \"metersPerUnit\": " << state.reflectionSceneScale
        << ",\n  \"resolution\": " << header.parameters[1] << ",\n  \"probes\": [";
    auto vector = [&](const auto &v) { out << '[' << v[0] << ", " << v[1] << ", " << v[2] << ']'; };
    for (uint32_t i = 0; i < count; ++i) {
      const auto &probe = header.probes[i];
      out << (i ? "," : "") << "\n    {\"authoredId\": \"" << state.reflectionIdentities[i] << "\", \"position\": ";
      vector(probe.position);
      out << ", \"minimum\": ";
      vector(probe.minimum);
      out << ", \"maximum\": ";
      vector(probe.maximum);
      out << ", \"influenceMinimum\": ";
      vector(probe.influenceMinimum);
      out << ", \"influenceMaximum\": ";
      vector(probe.influenceMaximum);
      out << ", \"featherDistance\": ";
      forward::Vec3 widths;
      const forward::Vec3 inverse{probe.maximum[3], probe.influenceMinimum[3],
                                  probe.influenceMaximum[3]};
      for (uint32_t c = 0; c < 3; ++c)
        widths[c] =
            inverse[c] > 0
                ? 1 / inverse[c]
                : (probe.influenceMaximum[c] - probe.influenceMinimum[c]) * .1f;
      vector(widths);
      out << ", \"projectionConfidence\": " << probe.position[3];
      out << ", \"priority\": " << int32_t(probe.minimum[3]) << ", \"worldToLocal\": [";
      for (uint32_t row = 0; row < 3; ++row) {
        const auto &r = probe.worldToLocal[row];
        out << (row ? "," : "") << '[' << r[0] << ',' << r[1] << ',' << r[2] << ',' << r[3] << ']';
      }
      out << "]}";
    }
    out << "\n  ],\n  \"localShadowDraws\": [";
    bool firstShadow = true;
    for (uint32_t slot = 0; slot < state.localShadowDraws.size(); ++slot) {
      const auto &shadow = state.localShadowDraws[slot];
      if (!shadow.active) continue;
      out << (firstShadow ? "" : ",") << "\n    {\"slot\": " << slot
          << ", \"lightId\": \"" << shadow.lightId << "\", \"frozen\": [";
      for (uint32_t face = 0; face < shadow.frozen.size(); ++face)
        out << (face ? ", " : "") << shadow.frozen[face];
      out << "], \"moving\": [";
      for (uint32_t face = 0; face < shadow.moving.size(); ++face)
        out << (face ? ", " : "") << shadow.moving[face];
      out << "]}";
      firstShadow = false;
    }
    out << "\n  ]\n}\n";
    return out.str();
  }
  void RemixNativeRuntime::retireForward(bool preserveBaking) {
    if (m_forwardPreparation) {
      m_pipelines->cancel(m_forwardPreparation->job);
      m_obsoleteForward.push_back(std::move(m_forwardPreparation));
    }
    if (m_forward) {
      if (!m_deviceLost) {
        const auto status = vkDeviceWaitIdle(m_device);
        if (status != VK_SUCCESS)
          throw status;
      }
      m_forward.reset();
    }
    if (!preserveBaking) {
      m_forwardBaking.reset();
      m_forwardReflections.reset();
      m_forwardExposure.reset();
    }
  }
  bool RemixNativeRuntime::prepareForwardSettings() {
    std::erase_if(m_obsoleteForward, [](const auto &entry) { return entry->job->ready(); });
    const auto &settings = m_profiles.current();
    if (settings.backend != config::Backend::ForwardPlus) {
      retireForward();
      m_forwardFailure.clear();
      return false;
    }
    const auto options = settings.forward;
    if (m_forwardAttempt != options) {
      m_forwardFailure.clear();
      m_forwardAttempt = options;
    }
    if (m_forwardPreparation && !m_forwardPreparation->options.sameTargets(options)) {
      m_pipelines->cancel(m_forwardPreparation->job);
      m_obsoleteForward.push_back(std::move(m_forwardPreparation));
    }
    if ((!m_forward || !m_forward->options().sameTargets(options)) && !m_forwardPreparation &&
        m_forwardFailure.empty() && m_forwardSupported && renderer().cameraValid()) {
      auto next = std::make_shared<PreparedForward>();
      next->options = options;
      const auto physical = m_physicalDevice;
      const auto device = m_device;
      const auto output = m_renderPass;
      const auto extent = m_swapchainExtent;
      auto *pipelines = m_pipelines.get();
      const bool hardwareRay = m_forwardRaySupported;
      const auto baking = m_forwardBaking;
      const auto reflections = m_forwardReflections;
      const auto exposure = m_forwardExposure;
      const auto queueFamily = m_queueFamily;
      const bool srgb =
          m_swapchainFormat == VK_FORMAT_R8G8B8A8_SRGB || m_swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB;
      const bool encode = m_srgb && !srgb;
      next->job = m_pipelines->prepare("forward-" + std::to_string(++m_preparationGeneration), [=] {
        auto replacement = std::make_unique<forward::Renderer>();
        replacement->initialize(physical, device, output, extent, options, encode, pipelines, hardwareRay,
                                baking, queueFamily, exposure, srgb, reflections);
        next->renderer = std::move(replacement);
      });
      m_forwardPreparation = std::move(next);
    }
    if (m_forwardPreparation && m_forwardPreparation->job->ready()) {
      try {
        m_forwardPreparation->job->check();
        const auto status = vkDeviceWaitIdle(m_device);
        if (status != VK_SUCCESS)
          throw status;
        m_forward = std::move(m_forwardPreparation->renderer);
        m_forwardBaking = m_forward->bakeController();
        m_forwardReflections = m_forward->reflectionAuthoring();
        m_forwardExposure = m_forward->exposureState();
        // Activation waited for the whole queue. Adopt the actual frame slot
        // before the new targets advance a retained bake or touch descriptors.
        m_forward->activate(m_frameIndex);
        m_forward->updateLighting(options);
        m_forwardPreparation.reset();
        m_forwardFailure.clear();
      } catch (VkResult status) {
        if (status == VK_ERROR_DEVICE_LOST)
          throw;
        m_forwardFailure = "Forward+ preparation failed (Vulkan " + std::to_string(status) + ")";
      } catch (const std::exception &error) {
        m_forwardFailure = error.what();
      }
      if (!m_forwardFailure.empty()) {
        SDL_Log("%s", m_forwardFailure.c_str());
        m_obsoleteForward.push_back(std::move(m_forwardPreparation));
      }
    }
    if (!m_forwardSupported)
      m_forwardFailure = "Forward+ requires a graphics/compute queue and sampled HDR/depth attachments";
    if (m_forward && m_forward->options().sameTargets(options))
      m_forward->updateLighting(options);
    m_backend = m_targetBackend = m_forward ? config::Backend::ForwardPlus : config::Backend::Raster;
    m_appliedSettings = settings;
    std::copy(settings.clearColor.begin(), settings.clearColor.end(), m_clearColor);
    m_loading = !m_forward && bool(m_forwardPreparation);
    m_worldCameraHold = false;
    return true;
  }
} // namespace remix

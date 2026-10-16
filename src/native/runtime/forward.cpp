#include "runtime/runtime.h"
#include <algorithm>
#include <iomanip>
#include <locale>
#include <sstream>

namespace remix {
  forward::Statistics RemixNativeRuntime::forwardStatistics() const {
    if (const auto *busy = busyState())
      return busy->forward;
    return m_forward ? m_forward->statistics() : forward::Statistics{};
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
        << state.bakeContent.captureInputs << "\"}"
        << ",\n  \"metersPerUnit\": " << state.reflectionSceneScale
        << ",\n  \"resolution\": " << header.parameters[1] << ",\n  \"probes\": [";
    auto vector = [&](const auto &v) { out << '[' << v[0] << ", " << v[1] << ", " << v[2] << ']'; };
    for (uint32_t i = 0; i < count; ++i) {
      const auto &probe = header.probes[i];
      out << (i ? "," : "") << "\n    {\"position\": ";
      vector(probe.position);
      out << ", \"minimum\": ";
      vector(probe.minimum);
      out << ", \"maximum\": ";
      vector(probe.maximum);
      out << ", \"influenceMinimum\": ";
      vector(probe.influenceMinimum);
      out << ", \"influenceMaximum\": ";
      vector(probe.influenceMaximum);
      out << '}';
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
    if (!preserveBaking)
      m_forwardBaking.reset();
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
      const auto queueFamily = m_queueFamily;
      const bool srgb =
          m_swapchainFormat == VK_FORMAT_R8G8B8A8_SRGB || m_swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB;
      const bool encode = m_srgb && !srgb;
      next->job = m_pipelines->prepare("forward-" + std::to_string(++m_preparationGeneration), [=] {
        auto replacement = std::make_unique<forward::Renderer>();
        replacement->initialize(physical, device, output, extent, options, encode, pipelines, hardwareRay,
                                baking, queueFamily);
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
        // Activation waited for the whole queue. Adopt the actual frame slot
        // before the new targets advance a retained bake or touch descriptors.
        m_forward->selectFrame(m_frameIndex);
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

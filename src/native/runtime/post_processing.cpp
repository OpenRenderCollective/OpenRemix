#include "runtime/runtime.h"
#include <algorithm>

namespace remix {
namespace {
void copyPost(TraceOptions &out, const TraceOptions &in) {
  out.autoExposure = in.autoExposure;
  out.exposure = in.exposure;
  out.autoExposureTarget = in.autoExposureTarget;
  out.autoExposureSpeed = in.autoExposureSpeed;
  out.autoExposureCentre = in.autoExposureCentre;
  out.autoExposureMinEv = in.autoExposureMinEv;
  out.autoExposureMaxEv = in.autoExposureMaxEv;
  out.bloom = in.bloom;
  out.bloomIntensity = in.bloomIntensity;
  out.bloomRadius = in.bloomRadius;
  out.contrast = in.contrast;
  out.contrastAmount = in.contrastAmount;
}
} // namespace
void RemixNativeRuntime::preparePostProcessing() {
  std::erase_if(m_obsoletePost, [](const auto &p) { return p->job->ready(); });
  auto cancel = [&] {
    if (!m_postPreparation)
      return;
    m_pipelines->cancel(m_postPreparation->job);
    m_obsoletePost.push_back(std::move(m_postPreparation));
  };
  if (!m_pathTracer) {
    cancel();
    if (m_postProcessing)
      m_postProcessing->freeze();
    return;
  }
  const auto display = m_swapchainExtent, render = m_pathTracer->renderExtent();
  const auto &requested = m_profiles.current().trace;
  const auto key =
      std::to_string(display.width) + ":" + std::to_string(display.height) +
      ":" + std::to_string(render.width) + ":" + std::to_string(render.height) +
      ":" + std::to_string(requested.autoExposure) + ":" +
      std::to_string(requested.bloom);
  if (key != m_failedPostKey)
    m_failedPostKey.clear();
  if (m_postPreparation && m_postPreparation->key != key)
    cancel();
  const bool ready =
      m_postProcessing &&
      m_postProcessing->supports(display, render, requested.autoExposure,
                                 requested.bloom);
  if (ready) {
    cancel();
    copyPost(m_postOptions, requested);
    m_postFailure.clear();
  } else if (!m_postPreparation && key != m_failedPostKey) {
    auto next = std::make_shared<PreparedPost>();
    next->key = key;
    const auto physical = m_physicalDevice;
    const auto device = m_device;
    const auto family = m_queueFamily;
    const auto state = m_exposureState;
    const bool meter = requested.autoExposure, bloom = requested.bloom;
    next->job = m_pipelines->prepare(
        "postfx-" + std::to_string(++m_postGeneration), [=] {
          auto renderer = std::make_unique<postfx::Renderer>();
          renderer->initialize(physical, device, family, display, render, meter,
                               bloom, state);
          next->renderer = std::move(renderer);
        });
    m_postPreparation = std::move(next);
    m_postFailure.clear();
  }
  if (m_postPreparation && m_postPreparation->job->ready()) {
    try {
      m_postPreparation->job->check();
      // Activation follows the existing renderer preparation boundary. The
      // readback path itself only uses completed frame slots and never waits.
      auto status = vkDeviceWaitIdle(m_device);
      if (status != VK_SUCCESS)
        throw status;
      m_postProcessing = std::move(m_postPreparation->renderer);
      m_postPreparation.reset();
      copyPost(m_postOptions, requested);
    } catch (VkResult result) {
      if (result == VK_ERROR_DEVICE_LOST)
        throw;
      m_postFailure = "Post-processing preparation failed (Vulkan " +
                      std::to_string(result) + ")";
    } catch (const std::exception &error) {
      m_postFailure = error.what();
    } catch (...) {
      m_postFailure = "Post-processing preparation failed";
    }
    if (!m_postFailure.empty()) {
      m_failedPostKey = key;
      cancel();
      SDL_Log("%s", m_postFailure.c_str());
    }
  }
  // Keep the previous working values on failure, even when the requested
  // profile has already been saved. During resize, the minimal compositor
  // retains manual/automatic exposure while new guide and bloom targets
  // prepare.
  const bool usable = m_postProcessing &&
                      m_postProcessing->supports(display, render, false, false);
  m_pathTracer->setPostProcessing(usable ? m_postProcessing.get() : nullptr,
                                  bool(m_postProcessing));
  if (m_postProcessing) {
    copyPost(m_traceOptions, m_postOptions);
    if (!usable)
      m_postProcessing->freeze();
  }
}
} // namespace remix

#include "forward/gpu.h"

namespace remix::forward {
  void Renderer::Gpu::recordExposure(VkCommandBuffer command, RasterRenderer &resources, float seconds) {
    TraceOptions post;
    post.autoExposure = options.autoExposure;
    post.exposure = options.exposure;
    post.autoExposureTarget = options.autoExposureTarget;
    post.autoExposureSpeed = options.autoExposureSpeed;
    post.autoExposureCentre = options.autoExposureCentre;
    post.autoExposureMinEv = options.autoExposureMinEv;
    post.autoExposureMaxEv = options.autoExposureMaxEv;
    post.bloom = false;
    const auto &frame = frames[slot];
    const auto &hdr = resolved ? resolved : color;
    // Late water has already incorporated screen reflections into HDR. Other
    // frames add the same separate reflection image used by the compositor.
    const auto reflected = reflections && !frame.waterComposed ? reflections->output() : VkDescriptorImageInfo{};
    auto epoch = resources.temporalEpoch();
    if (const auto &source = resources.bakeScene(); source && source->ready)
      epoch.world = scene::fastHash(&source->sceneId, sizeof(source->sceneId), epoch.world);
    postProcessing->record(command, {hdr->handle, hdr->view}, post, epoch, true,
                            options.debugView != 0, seconds, 0,
                            {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, reflected});
    statistics.postProcessing = postProcessing->statistics();
  }
}

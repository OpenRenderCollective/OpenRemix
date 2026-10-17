#include "forward/gpu.h"
#include "forward/image_dump.h"
#include "forward/probes/capture_dump.h"
#include <algorithm>

namespace remix::forward {
  void Renderer::Gpu::prepareSky(VkCommandBuffer command, RasterRenderer &resources) {
    auto &frame = frames[slot];
    const auto &dome = resources.dome();
    frame.skyEnabled = dome.enabled;
    SkyData data;
    data.inverseProjection = frame.data.inverseProjection;
    data.viewToDome = scene::multiply(dome.worldToLocal, scene::inverse(frame.data.view));
    // A uniform camera environment lights water even without a displayed dome.
    // Keep background ownership separate: only an authored dome enables it.
    const auto &radiance = dome.enabled ? dome.radiance : resources.skyRadiance();
    std::copy(radiance.begin(), radiance.end(), data.radiance.begin());
    data.radiance[3] = dome.enabled ? 1.f : 0.f;
    data.target = {float(extent.width), float(extent.height), 0, 0};
    probes::dumpFrameData(".sky-frame.bin", &data, sizeof(data));
    ensure(frame.skyConstants, sizeof(data),
           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
               (ImageDump::enabled(physical) ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : 0u));
    frame.skyConstants->write(&data, sizeof(data));
    // Upload/retain the texture through the shared resource service, while this
    // pass owns its panoramic sampler. Sky filtering needs no anisotropy feature.
    const auto material = resources.defaultMaterial();
    auto image = dome.texture ? resources.sampledTexture(material, dome.texture, command)
                              : resources.sampledMaterial(material, command);
    image.sampler = skySampler;
    frame.skyImage = image;
    const auto buffer = frame.skyConstants->descriptor();
    const VkWriteDescriptorSet writes[] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                            .dstSet = frame.composite,
                                            .dstBinding = 3,
                                            .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                            .pBufferInfo = &buffer},
                                           {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                            .dstSet = frame.composite,
                                            .dstBinding = 4,
                                            .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                            .pImageInfo = &image}};
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
  }
} // namespace remix::forward

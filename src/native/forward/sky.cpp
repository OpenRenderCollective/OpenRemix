#include "forward/gpu.h"
#include "forward/image_dump.h"
#include "forward/probes/capture_dump.h"
#include "forward/sky_model.h"
#include <algorithm>
#include <cmath>

namespace remix::forward {
  namespace {
    void transition(VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags source, VkAccessFlags sourceAccess, VkPipelineStageFlags target,
                    VkAccessFlags targetAccess) {
      const VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                         .srcAccessMask = sourceAccess,
                                         .dstAccessMask = targetAccess,
                                         .oldLayout = from,
                                         .newLayout = to,
                                         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .image = image,
                                         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      vkCmdPipelineBarrier(command, source, target, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
  } // namespace
  // The analytic sky for scenes without an authored dome. Each sun direction
  // gets its own immutable texture; frames retain theirs through their fence.
  const Renderer::Gpu::ProceduralSky &Renderer::Gpu::proceduralSkyTexture(VkCommandBuffer command,
                                                                         const Vec3 &towardSun) {
    if (proceduralSky) {
      const auto &previous = proceduralSky->towardSun;
      const float change = std::abs(previous[0] - towardSun[0]) + std::abs(previous[1] - towardSun[1]) +
                           std::abs(previous[2] - towardSun[2]);
      if (change < 1e-4f)
        return *proceduralSky;
    }
    auto next = std::make_shared<ProceduralSky>();
    next->towardSun = towardSun;
    const auto texels = sky_model::texels(towardSun);
    const VkDeviceSize bytes = texels.size() * sizeof(texels[0]);
    next->staging = std::make_unique<Buffer>();
    next->staging->create(physical, device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    next->staging->write(texels.data(), bytes);
    next->image = std::make_unique<Image>();
    next->image->create(physical, device, {sky_model::width, sky_model::height}, VK_FORMAT_R16G16B16A16_SFLOAT,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);
    transition(command, next->image->handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkBufferImageCopy region{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                   .imageExtent = {sky_model::width, sky_model::height, 1}};
    vkCmdCopyBufferToImage(command, next->staging->handle, next->image->handle,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transition(command, next->image->handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_ACCESS_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
               VK_ACCESS_SHADER_READ_BIT);
    proceduralSky = std::move(next);
    return *proceduralSky;
  }
  void Renderer::Gpu::prepareSky(VkCommandBuffer command, RasterRenderer &resources) {
    auto &frame = frames[slot];
    const auto &dome = resources.dome();
    // An authored dome always wins; the analytic sky replaces only a missing one.
    const bool procedural = !dome.enabled && options.proceduralSky;
    frame.skyEnabled = dome.enabled || procedural;
    SkyData data;
    data.inverseProjection = frame.data.inverseProjection;
    const auto worldToDome = dome.enabled ? dome.worldToLocal : sky_model::worldToDome(upAxis);
    data.viewToDome = scene::multiply(worldToDome, scene::inverse(frame.data.view));
    // A uniform camera environment lights water even without a displayed dome.
    // Keep background ownership separate: only a dome (authored or analytic) enables it.
    std::array<float, 3> radiance = dome.enabled ? dome.radiance : resources.skyRadiance();
    if (procedural)
      radiance.fill(sky_model::radianceScale(sun, options));
    std::copy(radiance.begin(), radiance.end(), data.radiance.begin());
    data.radiance[3] = frame.skyEnabled ? 1.f : 0.f;
    data.target = {float(extent.width), float(extent.height), .002f / options.sceneScale, 0};
    probes::dumpFrameData(".sky-frame.bin", &data, sizeof(data));
    ensure(frame.skyConstants, sizeof(data),
           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
               (ImageDump::enabled(physical) ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : 0u));
    frame.skyConstants->write(&data, sizeof(data));
    frame.skyWorldToDome = worldToDome;
    frame.skyRadiance = data.radiance;
    // Upload/retain the texture through the shared resource service, while this
    // pass owns its panoramic sampler. Sky filtering needs no anisotropy feature.
    VkDescriptorImageInfo image;
    if (procedural) {
      const Vec3 towardSun = sun.isDistant
          ? normalized(transformVector(worldToDome, {-sun.direction[0], -sun.direction[1], -sun.direction[2]}))
          : Vec3{.4f, .6f, .3f};
      const auto &texture = proceduralSkyTexture(command, towardSun);
      frame.proceduralSky = proceduralSky;
      image = {VK_NULL_HANDLE, texture.image->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    } else {
      const auto material = resources.defaultMaterial();
      image = dome.texture ? resources.sampledTexture(material, dome.texture, command)
                           : resources.sampledMaterial(material, command);
    }
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

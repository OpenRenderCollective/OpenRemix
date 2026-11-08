#include "pathtrace/gpu.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace remix {
  using namespace trace::detail;
  void PathTracer::Gpu::captureLinear(VkCommandBuffer command) {
    // Explicit diagnostic capture: linear floating-point transport before the
    // display transform, independent of the swapchain screenshot/JNI protocol.
    if (const char *path = std::getenv("REMIX_PATH_LINEAR_CAPTURE")) {
      auto source = world->handle;
      linearCapture.packedPbr = false;
      linearCapture.hitDistance = false;
      if (const auto *signal = std::getenv("REMIX_PATH_LINEAR_SIGNAL"); signal && denoiser) {
        if (!std::strcmp(signal, "diffuse") || !std::strcmp(signal, "specular") ||
            !std::strcmp(signal, "hit-distance")) {
          source = denoiser->image(!std::strcmp(signal, "diffuse") ? denoise::Sh0 : denoise::PbrSpecularInput).image;
          linearCapture.packedPbr = true;
          linearCapture.hitDistance = !std::strcmp(signal, "hit-distance");
        }
        if (denoiser->foregroundActive() && !std::strcmp(signal, "front-motion"))
          source = denoiser->image(denoise::FrontMotion).image;
        for (const auto &[name, slot] : std::initializer_list<std::pair<const char *, denoise::ImageSlot>>{
                 {"front-diffuse", denoise::FrontDiffuse},
                 {"front-specular", denoise::FrontSpecular},
                 {"front-diffuse-output", denoise::FrontDiffuseOutput},
                 {"front-specular-output", denoise::FrontSpecularOutput}})
          if (denoiser->foregroundActive() && !std::strcmp(signal, name)) {
            source = denoiser->image(slot).image;
            linearCapture.packedPbr = true;
          }
      }
      const VkDeviceSize size = VkDeviceSize(extent.width) * extent.height * (linearCapture.packedPbr ? 8 : 16);
      if (!linearCapture.readback || linearCapture.readback->size < size)
        linearCapture.readback = buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
      linearCapture.path = path;
      linearCapture.extent = extent;
      barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_ACCESS_TRANSFER_READ_BIT);
      const VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                   .imageExtent = {extent.width, extent.height, 1}};
      vkCmdCopyImageToBuffer(command, source, VK_IMAGE_LAYOUT_GENERAL, linearCapture.readback->handle, 1, &copy);
      barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
      barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
              VK_ACCESS_HOST_READ_BIT);
    }
  }
  void PathTracer::Gpu::completeLinearCapture() {
    if (!linearCapture.path.empty()) {
      void *mapped{};
      check(vkMapMemory(device, linearCapture.readback->memory, 0, VK_WHOLE_SIZE, 0, &mapped));
      const VkMappedMemoryRange range{.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                      .memory = linearCapture.readback->memory,
                                      .size = VK_WHOLE_SIZE};
      const auto result = vkInvalidateMappedMemoryRanges(device, 1, &range);
      if (result == VK_SUCCESS)
        if (auto *file = std::fopen(linearCapture.path.c_str(), "wb")) {
          std::fprintf(file, "PF\n%u %u\n-1.0\n", linearCapture.extent.width, linearCapture.extent.height);
          auto half = [](uint16_t bits) {
            const auto exponent = (bits >> 10) & 31u, mantissa = bits & 1023u;
            const float value =
                exponent == 31
                    ? (mantissa ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity())
                    : std::ldexp(float(mantissa + (exponent ? 1024 : 0)), exponent ? int(exponent) - 25 : -24);
            return bits & 0x8000 ? -value : value;
          };
          for (uint32_t y = linearCapture.extent.height; y-- > 0;)
            for (uint32_t x = 0; x < linearCapture.extent.width; ++x) {
              const auto offset = (VkDeviceSize(y) * linearCapture.extent.width + x) * 4;
              if (linearCapture.packedPbr) {
                const auto *pixels = static_cast<const uint16_t *>(mapped) + offset;
                if (linearCapture.hitDistance) {
                  const float distance = half(pixels[3]);
                  const float rgb[] = {distance, distance, distance};
                  std::fwrite(rgb, sizeof(float), 3, file);
                  continue;
                }
                const float Y = half(pixels[0]), Co = half(pixels[1]), Cg = half(pixels[2]);
                // NRD packed radiance uses YCoCg, and each lobe has its own material factor.
                const float rgb[] = {std::max(0.f, Y - Cg + Co), std::max(0.f, Y + Cg), std::max(0.f, Y - Cg - Co)};
                std::fwrite(rgb, sizeof(float), 3, file);
              } else {
                std::fwrite(static_cast<const float *>(mapped) + offset, sizeof(float), 3, file);
              }
            }
          std::fclose(file);
        }
      vkUnmapMemory(device, linearCapture.readback->memory);
      check(result);
      linearCapture.path.clear();
    }
  }
} // namespace remix

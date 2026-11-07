#pragma once
#include "denoise/renderer.h"
#include "platform/gpu_timers.h"
#include <array>
#include <string>
#include <vector>
#ifdef REMIX_HAS_NRD
#include <NRD.h>
#endif
namespace remix::denoise {
  inline void check(VkResult result) {
    if (result != VK_SUCCESS)
      throw result;
  }
  inline void synchronize(VkCommandBuffer command) {
    VkMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                      .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                       VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                       VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0,
                         nullptr, 0, nullptr);
  }
  struct Texture {
    VkDevice device{};
    VkImage image{};
    VkImageView view{};
    VkDeviceMemory memory{};
    VkDeviceSize bytes{};
    VkExtent2D extent{};
    ~Texture() {
      if (view)
        vkDestroyImageView(device, view, nullptr);
      if (image)
        vkDestroyImage(device, image, nullptr);
      if (memory)
        vkFreeMemory(device, memory, nullptr);
    }
  };
  struct Buffer {
    VkDevice device{};
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void *mapped{};
    VkDeviceSize size{};
    ~Buffer() {
      if (mapped)
        vkUnmapMemory(device, memory);
      if (buffer)
        vkDestroyBuffer(device, buffer, nullptr);
      if (memory)
        vkFreeMemory(device, memory, nullptr);
    }
  };
#ifdef REMIX_HAS_NRD
  // Each signal owns its dispatch descriptors, constants and temporal history.
  struct NrdPass {
    VkDevice device{};
    bool sigma{}, local{}, wasActive{};
    bool emissive{}, pbr{}, foreground{};
    nrd::Instance *instance{};
    std::vector<std::unique_ptr<Texture>> permanent, transient;
    struct Pipeline {
      VkDescriptorSetLayout resources{};
      VkPipelineLayout layout{};
      VkPipeline pipeline{};
    };
    std::vector<Pipeline> pipelines;
    VkDescriptorSetLayout commonLayout{};
    VkDescriptorPool nrdPool{};
    VkSampler samplers[2]{};
    std::unique_ptr<Buffer> constants;
    struct FrameResources {
      VkDescriptorPool pool{};
      std::unique_ptr<Buffer> constants;
    } frames[gpuFrameCount];
    VkDeviceSize constantStride{};
    const nrd::DispatchDesc *dispatches{};
    uint32_t dispatchCount{};
    ~NrdPass();
  };
#endif
  struct Renderer::Gpu {
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkExtent2D extent{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkPhysicalDeviceProperties properties{};
    std::array<std::unique_ptr<Texture>, ImageCount> images;
    std::unique_ptr<Buffer> uniform;
    VkDescriptorSetLayout inputLayout{}, emptyLayout{};
    VkDescriptorPool pool{};
    VkDescriptorSet inputs{};
    VkPipelineLayout layout{};
    VkPipeline post{};
    VkQueryPool timestamps{};
    uint32_t timestampBits{};
    bool timingPending{}, initialized{}, submittedFrame{};
    struct FrameResources {
      std::unique_ptr<Buffer> uniform;
      VkDescriptorSet inputs{};
      VkQueryPool timestamps{};
      bool timingPending{};
    } frames[gpuFrameCount];
    uint32_t frameIndex = 0;
    void swapFrame(FrameResources &frame, uint32_t slot);
    Frame frame{};
    bool localFireflyFilter = false;
    LocalTuning localTuning;
    bool sigma{}, local{}, split{}, validation{}, localReconstruction{};
    float sigmaMilliseconds{}, localMilliseconds{};
    remixapi_ShadowStatisticsEXT shadowStats{.structSize = sizeof(shadowStats)};
    uint32_t quietFrames = 0, warmupFrames = 32;
    float smoothedFrameMilliseconds = 0; // history lengths follow a smoothed frame rate
    remixapi_DenoisingStatisticsEXT stats{.structSize = sizeof(stats)};
#ifdef REMIX_HAS_NRD
    std::unique_ptr<NrdPass> indirect, shadow, localDirect, emissiveDirect, pbr, foreground;
    void initializeNrd(NrdPass &pass, bool sigma, bool local = false, bool pbr = false);
    void prepareNrd(NrdPass &pass, const scene::Matrix &projection, const scene::Matrix &previousProjection);
    void recordNrd(NrdPass &pass, VkCommandBuffer command);
#endif
    ~Gpu();
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags);
    std::unique_ptr<Texture> texture(VkFormat format, VkExtent2D size);
    std::unique_ptr<Buffer> buffer(VkDeviceSize size);
    VkPipeline pipeline(VkPipelineLayout layout, const uint32_t *code, size_t bytes);
    void pass(VkCommandBuffer command, uint32_t mode, uint32_t step = 0);
  };
} // namespace remix::denoise

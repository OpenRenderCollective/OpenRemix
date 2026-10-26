#pragma once
#include "platform/gpu_frames.h"
#include "platform/gpu_timers.h"
#include "sky/shader_code.h"
#include <numos/gpu.h>
#include <numos/shaders.h>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

namespace remix::sky {
  // numos::IGpu over the runtime's Vulkan device. Every image stays in
  // VK_IMAGE_LAYOUT_GENERAL; barrier() and every transfer are full
  // compute/transfer memory barriers. Constants and uploads go through rings
  // of the current frame slot, and destroyed resources are freed when their
  // slot comes round again (after its fence).
  class Bridge final : public numos::IGpu {
  public:
    // A pass of the host's own (an adapter), dispatched like NUMOS's: index
    // numos::passCount() + its position.
    struct HostPass {
      numos::PassInfo info;
      ShaderCode code;
    };
    Bridge(VkPhysicalDevice physical, VkDevice device, std::vector<HostPass> hostPasses);
    ~Bridge() override;
    Bridge(const Bridge &) = delete;
    Bridge &operator=(const Bridge &) = delete;

    // After the slot's fence: frees what the slot deferred and recycles its rings.
    void selectFrame(uint32_t slot);
    // Creates the pipelines of the passes `needed` selects, asking the
    // pipeline service to compile those it has not; true once all exist.
    // Render thread.
    bool preparePipelines(const std::function<bool(uint32_t pass)> &needed, uint32_t &ready, uint32_t &total);
    // Brackets the IGpu calls of a frame recorded into `command`; end() makes
    // the outputs visible to the host's compute and fragment shaders.
    void begin(VkCommandBuffer command);
    void end();

    // Host access to resources created or imported through this IGpu.
    VkImage image(numos::GpuHandle texture) const;
    VkImageView sampledView(numos::GpuHandle texture);
    VkSampler sampler(numos::GpuHandle sampler) const;
    // A host image NUMOS may bind; it must be in VK_IMAGE_LAYOUT_GENERAL
    // while NUMOS uses it. The view must cover what `desc` describes.
    numos::GpuHandle importTexture(VkImage image, VkImageView view, const numos::TextureDesc &desc);
    numos::GpuHandle importAccelerationStructure(VkAccelerationStructureKHR accel);
    // Forgets an import without destroying the host's object.
    void forget(numos::GpuHandle handle);

    numos::GpuHandle createTexture(const numos::TextureDesc &desc) override;
    numos::GpuHandle createBuffer(const numos::BufferDesc &desc) override;
    numos::GpuHandle createSampler(const numos::SamplerDesc &desc) override;
    void destroy(numos::GpuHandle resource) override;
    ConstantsRegion uploadConstants(const void *data, size_t size) override;
    void updateBuffer(numos::GpuHandle buffer, size_t offset, size_t size, const void *data) override;
    void copyBuffer(numos::GpuHandle dst, size_t dstOffset, numos::GpuHandle src, size_t srcOffset,
                    size_t size) override;
    void copyTexture(numos::GpuHandle dst, numos::GpuHandle src) override;
    void clearTexture(numos::GpuHandle texture, const float rgba[4]) override;
    void uploadTexture(numos::GpuHandle texture, uint32_t mip, const void *data, size_t size) override;
    void dispatch(const numos::DispatchDesc &desc) override;
    void barrier() override;
    bool readBuffer(numos::GpuHandle readback, void *dst, size_t size) override;
    void beginZone(const char *name) override;
    void endZone() override;

  private:
    struct Resource {
      enum class Kind : uint8_t { Texture, Buffer, Sampler, Accel } kind = Kind::Texture;
      bool imported = false;
      numos::TextureDesc texture;
      VkImage image = VK_NULL_HANDLE;
      VkImageView importedView = VK_NULL_HANDLE;
      std::map<uint64_t, VkImageView> views;
      numos::BufferDesc buffer;
      VkBuffer handle = VK_NULL_HANDLE;
      void *mapped = nullptr;
      uint64_t copyFrame = 0;
      VkSampler sampler = VK_NULL_HANDLE;
      VkAccelerationStructureKHR accel = VK_NULL_HANDLE;
      VkDeviceMemory memory = VK_NULL_HANDLE;
    };
    struct Staging {
      VkBuffer buffer = VK_NULL_HANDLE;
      VkDeviceMemory memory = VK_NULL_HANDLE;
      uint8_t *mapped = nullptr;
      size_t size = 0, used = 0;
    };
    struct Slot {
      numos::GpuHandle constants = 0;
      size_t constantsUsed = 0;
      std::vector<Staging> staging;
      std::vector<VkDescriptorPool> pools;
      size_t pool = 0;
      std::vector<Resource> retired;
    };
    struct Pass {
      numos::PassInfo info;
      ShaderCode code;
      VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
      VkPipelineLayout layout = VK_NULL_HANDLE;
      VkPipeline pipeline = VK_NULL_HANDLE;
      // Kept while the pipeline compiles: the service hashes the code on each request.
      VkShaderModule module = VK_NULL_HANDLE;
    };

    numos::GpuHandle add(Resource resource);
    Resource &get(numos::GpuHandle handle);
    const Resource &get(numos::GpuHandle handle) const;
    void release(Resource &resource);
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred = 0) const;
    VkBuffer makeBuffer(size_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required,
                        VkMemoryPropertyFlags preferred, VkDeviceMemory &memory, void **mapped);
    VkImageView view(Resource &texture, uint32_t mip, uint32_t count, bool storage);
    // Room in the slot's staging ring, growing it as needed.
    std::pair<Staging *, size_t> stage(const void *data, size_t size);
    VkDescriptorSet allocateSet(VkDescriptorSetLayout layout);
    numos::GpuHandle placeholder(const numos::PassBindingInfo &binding);
    void transferBarrier();

    VkDevice m_device;
    VkPhysicalDeviceProperties m_properties{};
    VkPhysicalDeviceMemoryProperties m_memory{};
    std::vector<Pass> m_passes;
    std::unordered_map<numos::GpuHandle, Resource> m_resources;
    numos::GpuHandle m_next = 1;
    std::array<Slot, gpuFrameCount> m_slots;
    uint32_t m_slot = 0;
    uint64_t m_frame = 0;
    VkCommandBuffer m_command = VK_NULL_HANDLE;
    std::map<uint32_t, numos::GpuHandle> m_placeholders;
    std::vector<std::unique_ptr<gpu::GpuScope>> m_zones;
  };
} // namespace remix::sky

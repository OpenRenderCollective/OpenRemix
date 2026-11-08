#pragma once
// Host passes (remixapi_HostPassEXT): the host's screen-space passes replayed
// on the composed frame, with its own shaders, before the UI. Targets the
// host names are images of this module (in GENERAL layout throughout); the
// frame is a raw copy of the swapchain image in its UNORM sibling format,
// copied back when a pass wrote it. Pipelines, layouts, samplers and shader
// modules go through the pipeline service (interned, compiled off the
// caller's thread). Render thread, except register/destroy/submit, which the
// API serializes with it through its lock. Metal builds share the API side
// (validation, submission, uploads) and replay the passes in
// metal/host_passes.mm.
#include "platform/gpu_frames.h"
#include <remix/remix_native_ext.h>
#include <cstdint>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#if !defined(REMIX_METAL)
#include <vulkan/vulkan.h>
#endif

namespace remix {
  class HostPasses {
  public:
#if defined(REMIX_METAL)
    void initialize(void *device); // an id<MTLDevice>
#else
    void initialize(VkPhysicalDevice physical, VkDevice device);
#endif
    void destroy(); // after the device is idle
    remixapi_ErrorCode registerShader(const remixapi_HostShaderInfoEXT &info, remixapi_HostShaderHandleEXT &out);
    remixapi_ErrorCode destroyShader(remixapi_HostShaderHandleEXT shader);
    remixapi_ErrorCode submit(const remixapi_HostPassListEXT &list);
    // `depth` above 1: a volume, sampled only.
    remixapi_ErrorCode updateTarget(remixapi_HostTargetHandleEXT target, uint32_t width, uint32_t height,
                                    uint32_t format, const void *pixels, uint64_t bytes, uint32_t depth = 1);
    bool pending() const;
    // The user's setting (rtx.native.hostPasses), from the frame's settings:
    // off, submissions keep only their REMIXAPI_HOST_PASS_ALWAYS_EXT passes
    // (what is pending keeps them likewise); uploads are kept for those.
    void setEnabled(bool enabled);
    // For remixapi_GetHostPassesEnabledEXT: the setting; the host asking is what
    // shows the setting in the UI (asked()).
    bool enabled() {
      m_asked = true;
      return m_enabled;
    }
    bool asked() const { return m_asked; }
    // Executes the submitted passes between the frame's render passes: the
    // swapchain `image` is in `layout` before and after. `viewDepth`: the
    // frame's REMIXAPI_HOST_TARGET_VIEW_DEPTH_EXT image (GENERAL layout), or
    // none (passes sampling it are skipped).
#if defined(REMIX_METAL)
    // Metal: `command` an id<MTLCommandBuffer>, `frame` the frame's texture
    // (an id<MTLTexture>, read and written), `viewDepth` an id<MTLTexture> or null.
    void record(void *command, uint32_t slot, void *frame, void *viewDepth);
#else
    void record(VkCommandBuffer command, uint32_t slot, VkImage image, VkFormat format, VkExtent2D extent,
                VkImageLayout imageLayout, VkImageView viewDepth);
#endif
    remixapi_HostPassStatisticsEXT statistics() const;

  private:
    // The target formats a host uploads (VkFormat values, as the API takes them).
    static constexpr uint32_t formatRgba8 = 37, formatBgra8 = 44; // VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM
    struct Shader {
      bool vertex = false; // else a fragment shader
      std::vector<remixapi_HostShaderBindingEXT> bindings;
      uint32_t pushBytes = 0;
      std::vector<uint32_t> code; // kept: the module is interned by its code
#if defined(REMIX_METAL)
      // metal/host_passes.mm: the translated function (a retained id<MTLFunction>) and the Metal
      // slot of each binding (buffers from bufferBase, textures and samplers from 0).
      std::shared_ptr<void> function;
      std::map<uint32_t, uint32_t> slots;
#else
      VkShaderModule module = VK_NULL_HANDLE;
      VkShaderStageFlagBits stage{};
#endif
    };
    // The shader's GPU form (module or translation); false when it cannot have one.
    bool createModule(Shader &shader);
    // A submitted pass with its data copied.
    struct Pass {
      remixapi_HostPassEXT info{};
      std::vector<remixapi_HostPassBufferEXT> buffers;
      std::vector<remixapi_HostPassSamplerEXT> samplers;
      std::vector<remixapi_HostPassVertexBindingEXT> vertexBindings;
      std::vector<remixapi_HostPassAttributeEXT> attributes;
      std::vector<uint8_t> data; // buffers', push constants' and vertices' bytes, in that order
      std::vector<uint32_t> indices;
      bool always = false; // REMIXAPI_HOST_PASS_ALWAYS_EXT: replayed while the setting is off
    };
    // A target's contents from the host, uploaded before the frame's passes.
    struct Upload {
      remixapi_HostTargetHandleEXT target;
      uint32_t format; // formatRgba8 or formatBgra8
      uint32_t width, height, depth;
      std::vector<uint8_t> pixels;
    };
#if defined(REMIX_METAL)
    struct Metal; // metal/host_passes.mm
    std::shared_ptr<Metal> m_metal;
#else
    struct Target {
      VkImage image = VK_NULL_HANDLE;
      VkDeviceMemory memory = VK_NULL_HANDLE;
      VkImageView view = VK_NULL_HANDLE;    // identity: the attachment
      VkImageView sampled = VK_NULL_HANDLE; // swizzled as `channels` says: what passes sample
      VkFramebuffer framebuffer = VK_NULL_HANDLE;
      VkRenderPass pass = VK_NULL_HANDLE;
      // The frame only: with the frame depth attached (draws that test depth).
      VkRenderPass depthPass = VK_NULL_HANDLE;
      VkFramebuffer depthFramebuffer = VK_NULL_HANDLE;
      VkFormat format{};
      VkExtent2D extent{};
      uint32_t channels = 0;    // REMIXAPI_HOST_CHANNELS_*
      uint32_t depth = 1;       // above 1: a 3D image, sampled only (no render pass)
      bool initialized = false; // in GENERAL layout
      uint64_t lastFrame = 0;
    };
    struct Layout {
      VkDescriptorSetLayout set = VK_NULL_HANDLE;
      VkPipelineLayout pipeline = VK_NULL_HANDLE;
      std::vector<remixapi_HostShaderBindingEXT> bindings;
      uint32_t pushBytes = 0;
    };
    struct Slot {
      VkBuffer buffer = VK_NULL_HANDLE; // host-visible: uniform, vertex and index data of the frame's passes
      VkDeviceMemory memory = VK_NULL_HANDLE;
      uint8_t *mapped = nullptr;
      VkDeviceSize size = 0, used = 0;
      VkDescriptorPool pool = VK_NULL_HANDLE;
    };
    Target &target(remixapi_HostTargetHandleEXT handle, VkFormat format, VkExtent2D extent, uint32_t channels,
                   uint32_t depth = 1);
    // REMIXAPI_HOST_TARGET_FRAME_DEPTH_EXT: a depth image of the frame's size with its depth-only pass.
    Target &frameDepth(VkCommandBuffer command, VkExtent2D extent);
    // The frame's pass and framebuffer with that depth attached.
    void attachDepth(Target &frame, const Target &depth);
    void destroyTarget(Target &target);
    const Layout &layout(const Shader &vertex, const Shader &fragment);
    VkPipeline pipeline(const Pass &pass, const Shader &vertex, const Shader &fragment, const Layout &layout,
                        VkRenderPass renderPass, bool depthOnly);
    VkDeviceSize stage(Slot &slot, const void *data, VkDeviceSize bytes, VkDeviceSize alignment);
    void reserve(Slot &slot, VkDeviceSize bytes);
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const;
    void barrier(VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags source,
                 VkAccessFlags destination, VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage);
    void blit(VkCommandBuffer command, VkImage source, VkExtent2D sourceExtent, const int32_t sourceRect[4],
              VkImage destination, VkExtent2D destinationExtent, const int32_t targetRect[4], bool linear);

    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties m_memory{};
    std::map<remixapi_HostTargetHandleEXT, Target> m_targets; // 0: the frame's copy
    std::map<std::pair<remixapi_HostShaderHandleEXT, remixapi_HostShaderHandleEXT>, Layout> m_layouts;
    std::map<std::string, VkPipeline> m_pipelines;
    Slot m_slots[gpuFrameCount];
#endif
    bool ready() const; // initialized, until destroy()
    uint64_t m_nextShader = 1;
    std::map<remixapi_HostShaderHandleEXT, std::shared_ptr<Shader>>
        m_shaders; // a shader outlives its handle while a frame uses it
    std::vector<Pass> m_pending;
    std::vector<Upload> m_uploads;
    uint64_t m_frame = 0;
    std::atomic<bool> m_enabled{false}, m_asked{false};
    mutable std::mutex m_mutex; // m_pending, m_uploads, m_shaders, m_stats (API thread vs render thread)
    remixapi_HostPassStatisticsEXT m_stats{.structSize = sizeof(remixapi_HostPassStatisticsEXT)};
  };
} // namespace remix

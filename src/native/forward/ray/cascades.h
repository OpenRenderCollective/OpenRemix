#pragma once
#include "forward/bindings.h"
#include "forward/options.h"
#include "forward/ray/live_scene.h"
#include <array>
#include <memory>
#include <vulkan/vulkan.h>

namespace remix::forward::ray {
  // Split Radiance Cascades (Freeman, Sannikov and Pitt-Francis): diffuse
  // irradiance from sparse world-space probes. Probes are hashed next to the
  // visible surfaces, one ray per sampled pixel is traced through the live
  // scene's transport and split into the cascades' distance intervals, and the
  // cascades merge down into per-probe irradiance.
  //
  // A build uses the completed frame's depth and normal guides, so receivers
  // read the build of the previous frame. Probes are world-space: there is no
  // reprojection, and a receiver no probe covers yet keeps its ambient fallback.
  // Every build traces from empty bins; its irradiance blends with the same
  // probe's irradiance from the build before (Options::cascadeAccumulation).
  class Cascades {
  public:
    // Mirrored by cascade_shaders/common.glsl.
    static constexpr uint32_t cascadeCount = 4, tableBits = 18, baseDirections = 32;
    // Probe pools per cascade, shared by every level of detail.
    static constexpr std::array<uint32_t, cascadeCount> capacities{32768, 8192, 2048, 1024};
    static constexpr uint32_t tile = 8, tileRow = 256;
    // Finest cells between the camera and the end of level of detail 0, which
    // sets how large a probe is on screen. Keys wrap cell coordinates at 256, so
    // a level's probes must span fewer cells than that. The build lowers the
    // requested value while a probe pool overflows.
    static constexpr float minimumLodCells = 12, maximumLodCells = 96;
    static constexpr float intervalCells = 1.6f; // cascade 0 interval end, in probe spacings
    static constexpr float radianceScale = 1024; // fixed-point steps per unit of radiance

    Cascades();
    ~Cascades();
    void initialize(VkPhysicalDevice physical, VkDevice device, LiveScene &scene);
    void selectFrame(uint32_t slot); // that slot's submission fence completed
    // The receivers' descriptor set (set 2 of the live-diffuse pipeline layout).
    // Caller owns the returned layout reference.
    static VkDescriptorSetLayout createQueryLayout(VkDevice device);
    // Describe the last published build to this frame's receivers. Outside a render pass.
    VkDescriptorSet prepareQuery(VkCommandBuffer command);
    struct Inputs {
      VkImageView normals, depth; // the frame's resolved reflection guides
      VkExtent2D extent;
    };
    // After every pass that reads the published build. `bounces` must match the
    // other device radiance batches recorded in this submission.
    void record(VkCommandBuffer command, Inputs inputs, const FrameData &frame, const Options &options,
                uint32_t bounces);
    const Statistics &statistics() const;
    using Memory = LiveScene::Memory;
    Memory memory() const;

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix::forward::ray

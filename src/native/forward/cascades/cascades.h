#pragma once
#include "forward/lighting.h"
#include <cstdint>
#include <memory>
#include <vulkan/vulkan.h>

namespace remix::forward {
  struct Options;
}
namespace remix::forward::cascades {
  struct Statistics {
    bool available = false, active = false;
    uint32_t cascades = 0, probes = 0; // probes of cascade 0
    uint64_t rays = 0;                 // texels traced over all cascades
    char status[128]{};
  };
  // Screen-space radiance cascades (Sannikov, radiance-cascades.com) for
  // Forward+ indirect diffuse. Cascade c places probes every s * 2^c pixels
  // on the depth buffer, each with (s * 2^c)^2 octahedral directions; every
  // cascade image is therefore about the size of the target. Cascade c traces
  // the world-space interval [l (4^c - 1) / 3, l (4^(c+1) - 1) / 3) against the
  // live ray scene, top cascade first; an unoccluded interval continues into
  // the four matching directions of the four nearest parent probes. The last
  // cascade sees the sky. Hits are lit by the sun and lights (with shadow rays)
  // plus, optionally, the previous frame's irradiance reprojected onto them.
  // Gathered irradiance / pi (rgb) and view depth (a) are read by the next
  // frame's shading through outputCamera(). One target-sized resource set is
  // shared by every frame slot on one queue.
  class RadianceCascades {
  public:
    RadianceCascades();
    ~RadianceCascades();
    // rayScene: bake::Transport::createSceneLayout; the caller keeps it alive.
    void initialize(VkPhysicalDevice physical, VkDevice device, VkExtent2D extent, VkSampleCountFlagBits samples,
                    const Options &options, VkDescriptorSetLayout rayScene);
    void selectFrame(uint32_t slot); // that slot's submission fence completed
    struct Inputs {
      VkImageView depth;              // world depth, DEPTH_STENCIL_READ_ONLY_OPTIMAL
      VkDescriptorSet rayScene;       // this frame's prepared transport set
      VkDescriptorImageInfo sky;      // equirectangular dome, SHADER_READ_ONLY_OPTIMAL
      Matrix camera, view, worldToDome;
      Vec4 skyRadiance;               // rgb scale; w zero without a sky image
      Vec3 eye;
    };
    // After depth preparation; leaves the output readable by later fragment shading.
    void record(VkCommandBuffer command, const Inputs &inputs, const Options &options);
    // The most recently recorded gather (SHADER_READ_ONLY_OPTIMAL after its
    // frame's record) and the camera it used; false before the first record.
    bool hasOutput() const;
    VkDescriptorImageInfo output() const;
    // Cascade-0 probes' luminance L1/L0 SH (GENERAL, bilinear), one texel per
    // probe every probeSpacing() pixels, for shading-normal response.
    VkDescriptorImageInfo directions() const;
    uint32_t probeSpacing() const;
    const Matrix &outputCamera() const;
    const Statistics &statistics() const;
    uint64_t targetBytes() const;

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix::forward::cascades

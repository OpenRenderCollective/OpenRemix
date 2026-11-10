#pragma once
#include "forward/bindings.h"
#include "forward/options.h"
#include "forward/ray/live_scene.h"
#include <memory>
#include <vulkan/vulkan.h>

namespace remix {
  class RasterRenderer;
}
namespace remix::forward::ray {
  // Optional hardware continuation of unresolved SSR radiance. The transport
  // and its scene owners remain alive through every querying frame's fence.
  class Reflections {
  public:
    Reflections();
    ~Reflections();
    void initialize(VkPhysicalDevice physical, VkDevice device, LiveScene &scene);
    void selectFrame(uint32_t slot); // that slot's submission fence completed
    struct Inputs {
      VkImageView normals, depth, radiance; // current resolved guides and SSR raw
      VkExtent2D extent, traceExtent;
    };
    void record(VkCommandBuffer command, Inputs inputs, const FrameData &frame, const Options &options);
    const Statistics &statistics() const;
    using Memory = LiveScene::Memory;
    Memory memory() const;

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix::forward::ray

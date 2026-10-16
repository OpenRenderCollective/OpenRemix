#pragma once
#include <cstdint>
#include <memory>
#include <vulkan/vulkan.h>

namespace remix::forward {
  struct Options;
}
namespace remix::forward::bake {
  class Transport;
}

namespace remix {
  class RasterRenderer;
}
namespace remix::forward::ray {
  struct Statistics {
    bool available = false, active = false;
    uint32_t rays = 0, sceneBuilds = 0;
    char status[128]{};
  };
  // One live opaque scene shared by optional GI and reflections. Preparation,
  // lighting streams and frame-fence ownership are independent of either effect.
  class LiveScene {
  public:
    LiveScene();
    ~LiveScene();
    void initialize(VkPhysicalDevice physical, VkDevice device);
    void selectFrame(uint32_t slot);
    void prepare(VkCommandBuffer command, RasterRenderer &resources, const Options &options, double seconds);
    bake::Transport *transport() const;
    const Statistics &statistics() const;
    struct Memory {
      uint64_t buffers = 0, images = 0, acceleration = 0;
    };
    Memory memory() const;
    static bool supportsFragmentGI(VkPhysicalDevice physical);

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix::forward::ray

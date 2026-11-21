// Stands in for the Forward+ backend in builds without it (-Dforward=false).
// The runtime never creates the renderer there (runtime/device.cpp finds the
// backend unsupported); these definitions only complete the link.
#include "forward/lightmap/denoise.h"
#include "forward/renderer.h"

namespace remix::forward {
  namespace lightmap {
    uint32_t denoiserVersion() { return 0; }
  } // namespace lightmap

  struct Renderer::Gpu {};
  Renderer::Renderer() = default;
  Renderer::~Renderer() = default;
  void Renderer::initialize(VkPhysicalDevice, VkDevice, VkRenderPass, VkExtent2D, Options, bool, pipeline::Service *,
                            bool, std::shared_ptr<bake::Controller>, uint32_t, std::shared_ptr<postfx::State>, bool,
                            std::shared_ptr<probes::Authoring>, std::shared_ptr<probes::StandaloneCapture>) {
    throw VK_ERROR_FEATURE_NOT_PRESENT;
  }
  std::shared_ptr<bake::Controller> Renderer::bakeController() const { return {}; }
  std::shared_ptr<postfx::State> Renderer::exposureState() const { return {}; }
  std::shared_ptr<probes::Authoring> Renderer::reflectionAuthoring() const { return {}; }
  std::shared_ptr<probes::StandaloneCapture> Renderer::standaloneCaptures() const { return {}; }
  void Renderer::activate(uint32_t) {}
  void Renderer::selectFrame(uint32_t) {}
  void Renderer::prepare(VkCommandBuffer, RasterRenderer &, float, double) {}
  void Renderer::setDisplayOutput(const scene::DisplayOutput &) {}
  void Renderer::composite(VkCommandBuffer) {}
  void Renderer::background(VkCommandBuffer) {}
  const Statistics &Renderer::statistics() const {
    static const Statistics none;
    return none;
  }
  const Options &Renderer::options() const {
    static const Options none;
    return none;
  }
  bool Renderer::owns(const scene::Draw &) const { return false; }
  void Renderer::updateLighting(const Options &) {}
} // namespace remix::forward

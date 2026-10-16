#pragma once
#include <array>
#include <memory>
#include <span>
#include <vulkan/vulkan.h>

namespace remix::forward {
  // Static world-space triangle acceleration, independent of a camera, shading
  // layout, descriptor indexing and the full path tracer. A bake can release
  // it afterwards; optional live RT effects may retain it separately.
  class RayScene {
  public:
    using Position = std::array<float, 3>;
    struct Geometry {
      std::span<const Position> positions;
      std::span<const uint32_t> indices; // triples; primitive IDs preserve this order
    };
    RayScene();
    ~RayScene();
    RayScene(const RayScene &) = delete;
    RayScene &operator=(const RayScene &) = delete;
    // Physical-device support only. The caller must enable accelerationStructure,
    // rayQuery and bufferDeviceAddress plus their three KHR extensions.
    static bool supported(VkPhysicalDevice physical);
    // Creates owned GPU inputs and acceleration storage; no commands are submitted.
    // Before replacing an initialized scene, complete its last query/build fence.
    void initialize(VkPhysicalDevice physical, VkDevice device, Geometry geometry);
    // Caller serializes builds/queries on its queue. Build inputs and the scene
    // must outlive the submission fence. Final barrier covers the requested query stages.
    void recordBuild(VkCommandBuffer command,
                     VkPipelineStageFlags queryStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    // Call only after the build fence; queries need only the retained structures.
    void releaseBuildInputs();
    VkAccelerationStructureKHR handle() const;
    uint64_t accelerationBytes() const;
    uint64_t buildBytes() const;

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix::forward

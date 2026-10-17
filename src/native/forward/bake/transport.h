#pragma once
#include "forward/bake/data.h"
#include <optional>
#include <span>
#include <vulkan/vulkan.h>

namespace remix {
  class RasterRenderer;
}
namespace remix::forward::bake {
  struct Options {
    uint32_t textureResolution = 256, batchPoints = 1024;
    uint32_t initialBatchPoints = 0, queueFamily = VK_QUEUE_FAMILY_IGNORED;
    bool parallel = true; // sample-parallel diffuse/visibility; radiance and site queries stay serial
    bool cpuReadback = true; // prefer CPU-cached results for baking; live GPU consumers keep host placement
    uint32_t rayBudget = 1u << 20; // point * sample * bounce ceiling per dispatch
    uint64_t atlasBudget = 256ull << 20;
    float distance = 100, bias = .002f; // metres
  };
  // One scene snapshot's hardware diffuse/radiance transport. The caller owns
  // scheduling, submission and fences; no queue waits or hidden work occur here.
  class Transport {
  public:
    Transport();
    ~Transport();
    // Compile immutable transport pipelines during renderer preparation, so a
    // live scene update never needs to compile them on the rendering thread.
    static void preparePipelines(VkDevice device);
    // Shared read-only scene contract for compute and optional fragment queries.
    // Caller owns the returned layout reference.
    static VkDescriptorSetLayout createSceneLayout(VkDevice device);
    // Pipeline creation runs through the runtime's preparation service. Complete
    // this instance's submitted work before replacement or destruction.
    void initialize(VkPhysicalDevice physical, VkDevice device, std::shared_ptr<const scene::BakeScene> scene,
                    Options options = {});
    // Reuse a caller's packed inputs (for example a live posed-scene cache).
    void initialize(VkPhysicalDevice physical, VkDevice device, std::shared_ptr<const scene::BakeScene> scene,
                    Inputs inputs, Options options);
    void recordPrepare(VkCommandBuffer command, RasterRenderer &textures);
    // The selected slot's prior fence has completed. Call after recordPrepare.
    void recordBatch(VkCommandBuffer command, uint32_t slot, std::span<const Point> points, uint32_t samples,
                     uint32_t bounces = 3, uint32_t firstSample = 0);
    // Directional radiance for reflection capture. Point.normal.xyz is a ray
    // direction; no surface-normal bias is applied. Result.irradiance[0] holds
    // RGB radiance and first-hit distance, without SH/cosine convolution.
    void recordRadiance(VkCommandBuffer command, uint32_t slot, std::span<const Point> rays,
                        uint32_t bounces = 3);
    // GPU producers may fill this slot's points directly, then dispatch radiance
    // without a CPU copy. Producer writes must be visible to COMPUTE shader reads;
    // the returned results become visible to COMPUTE reads after the dispatch.
    // Point.normal.w < 0 marks an inactive ray; no traversal is performed for it.
    struct DeviceBatch {
      VkDescriptorBufferInfo points, results;
      uint32_t capacity;
    };
    DeviceBatch deviceBatch(uint32_t slot) const;
    // Reacquire deviceBatch after a CPU-recorded batch: growing that completed
    // slot may replace its buffers. GPU producers must obey its actual capacity.
    // Separate fragment constants prevent later compute reflection dispatches
    // from overwriting the GI configuration in the same submission.
    VkDescriptorSet prepareFragment(uint32_t slot, uint32_t samples, uint32_t bounces, float distance);
    // Stream current analytic lighting into a completed frame slot without
    // rebuilding geometry. Dome texture membership still belongs to the atlas.
    void updateLighting(uint32_t slot, std::span<const LightData> lights, const scene::DistantLight &sun,
                        const scene::DomeLight &dome, float distance);
    // Repeated batches in one submission share slot constants: keep count and
    // bounces fixed, padding a short tail with inactive rays when necessary.
    void recordDeviceRadiance(VkCommandBuffer command, uint32_t slot, uint32_t count, uint32_t bounces = 3);
    // Soft static visibility toward one captured emitter, independent of
    // receiver BRDF/albedo and current light colour. Result.irradiance[0].x
    // is a shape-weighted visible fraction; statistics.z is the sample count.
    // With directDiffuse, irradiance[0..8].y additionally holds unit-radiance
    // cosine-convolved SH, including static visibility, for this same light.
    // Geometry-only spherical queries for origin validation. No lighting is evaluated.
    void recordSiteValidation(VkCommandBuffer command, uint32_t slot, std::span<const Point> points,
                              uint32_t samples = 64);
    void recordVisibility(VkCommandBuffer command, uint32_t slot, std::span<const VisibilityPoint> points,
                          uint32_t samples = 256, bool directDiffuse = false);
    // Unit-radiance static sunlight, independent of the captured sun's colour.
    // Same result layout as local visibility/direct SH, using the distant disc.
    void recordSun(VkCommandBuffer command, uint32_t slot, std::span<const Point> points,
                   uint32_t samples = 256, bool directDiffuse = false);
    // The selected slot's submission fence has completed.
    void read(uint32_t slot, std::span<Result> results) const;
    bool timingSupported() const;
    std::optional<double> elapsed(uint32_t slot) const; // owning slot fence completed
    void releaseBuildInputs();                          // after the preparation fence
    uint64_t memoryBytes() const;
    uint64_t imageBytes() const;
    uint64_t accelerationBytes() const;
    uint32_t textureResolution() const;

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m_gpu;
    enum class Mode : uint32_t {
      Irradiance,
      Radiance,
      Visibility,
      DirectVisibility,
      SiteValidation,
      SunVisibility,
      SunDirectVisibility
    };
    void record(VkCommandBuffer command, uint32_t slot, std::span<const Point> points, uint32_t samples,
                uint32_t bounces, uint32_t firstSample, Mode mode);
    void dispatch(VkCommandBuffer command, uint32_t slot, uint32_t count, uint32_t samples, uint32_t bounces,
                  uint32_t firstSample, Mode mode);
  };
} // namespace remix::forward::bake

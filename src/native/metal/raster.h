#pragma once
#include "scene/resources.h"
#include <memory>

namespace remix::metal {
  // The API's scene and the Metal raster renderer (raster/renderer.h's Metal
  // counterpart). GPU state is private to metal/*.mm (raster_gpu.h).
  class Raster : public NativeResources {
  public:
    Raster();
    ~Raster();
    // device: an id<MTLDevice>.
    void initializeGpu(void *device);
    void shutdownGpu();
    struct MemoryStatistics {
      uint64_t textureBytes = 0, textureCount = 0, compressedTextureBytes = 0;
      uint64_t geometryBytes = 0, pooledBytes = 0;
    };
    MemoryStatistics memoryStatistics() const;
    // Changes whenever a dynamic texture's new pixels were uploaded
    // (raster/renderer.h dynamicTextureGeneration).
    uint64_t dynamicTextureGeneration() const;
    struct Profile {
      double prepareMilliseconds = 0, skinningMilliseconds = 0;
      uint64_t draws = 0, skinnedVertices = 0, uploadBytes = 0;
      uint64_t culledDraws = 0, maskGroups = 0, maskDraws = 0, haloGroups = 0, haloDraws = 0;
    };
    const Profile &profile() const { return m_profile; }
    // The caller has completed this slot's submission.
    void selectFrame(uint32_t slot);
    // Uploads and the frame's draw list; tracing leaves traced draws out.
    void prepare(bool tracing = false);

    struct Gpu;
    Gpu &gpu() { return *m_gpu; }

  private:
    std::unique_ptr<Gpu> m_gpu;
    Profile m_profile;
  };
} // namespace remix::metal

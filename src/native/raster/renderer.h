#pragma once
#include <vulkan/vulkan.h>
#include "scene/resources.h"
#include "platform/gpu_frames.h"

namespace remix {
  class RasterRenderer : public NativeResources {
  public:
    RasterRenderer();
    ~RasterRenderer();
    // textureCompression: the device was created with textureCompressionBC, so
    // compressed textures upload as BC images instead of a decoded fallback.
    void initializeGpu(VkPhysicalDevice physical, VkDevice device, bool textureCompression = false,
                       bool memoryBudget = false);
    // Takes other's GPU state (caches, pipelines, target and raster profile),
    // leaving other without one. Asynchronous presentation renders from a
    // published frame (NativeResources::publishFrame) held by a second
    // renderer; the caller has completed the render thread's work.
    void adoptGpu(RasterRenderer &other);
    struct MemoryStatistics {
      uint64_t textureBytes = 0, textureCount = 0, compressedTextureBytes = 0;
      uint64_t geometryBytes = 0, pooledBytes = 0;
    };
    MemoryStatistics memoryStatistics() const; // Device allocations owned by this renderer.
    // CPU cost of the last prepare() (FrameBreakdownEXT).
    struct Profile {
      double prepareMilliseconds = 0, skinningMilliseconds = 0;
      uint64_t culledDraws = 0; // left out by frustum culling
      uint64_t unreadyDraws = 0; // left out while their pipeline variant compiles (path tracing)
      // Where prepare()'s time goes: retiring released resources, refreshing
      // dynamic textures, ordering the draws and building their commands
      // (a mesh's or material's first use uploads it).
      double retireMilliseconds = 0, refreshMilliseconds = 0, orderMilliseconds = 0, buildMilliseconds = 0;
      uint64_t meshesCreated = 0, materialsCreated = 0, texturesRefreshed = 0;
      // Staging uploads of this prepare(): buffers written, their bytes, the
      // time allocating and writing them, and the memory type's property flags.
      double stagingAllocateMilliseconds = 0, stagingWriteMilliseconds = 0, skinUploadMilliseconds = 0;
      uint64_t stagingBuffers = 0, stagingAllocated = 0;
      uint32_t stagingMemoryFlags = 0;
      // First uses: creating pipelines, meshes (upload), textures (image,
      // upload, mips) and materials (descriptor set; its texture's time is
      // counted with the textures).
      double createPipelineMilliseconds = 0, createMeshMilliseconds = 0, createTextureMilliseconds = 0;
      double createMaterialMilliseconds = 0;
      uint64_t pipelinesCreated = 0, texturesCreated = 0;
      // The retire phase in parts, and what it walks.
      double recycleMilliseconds = 0, retireMaterialsMilliseconds = 0, retireTexturesMilliseconds = 0;
      double retireMeshesMilliseconds = 0, retirePathsMilliseconds = 0;
      uint64_t materialCount = 0, textureCount = 0, meshCount = 0, pathCount = 0, recycledBuffers = 0;
      uint64_t draws = 0, skinnedVertices = 0, uploadBytes = 0; // draws include halo sources
      uint64_t haloGroups = 0, haloDraws = 0;
      uint64_t maskGroups = 0, maskDraws = 0; // groups with coverage, their source draws
      uint64_t digest = 0; // REMIX_FRAME_DIGEST only: commands and skinned vertices.
    };
    const Profile &profile() const { return m_profile; }
    // The last prepare() that finished, whole (profile() is rewritten while one runs).
    Profile completedProfile() const { return m_completedProfile; }
    // What creating textures' images cost since this was last asked: the
    // driver's budget query, creating the image, allocating and binding its
    // memory, its view, and writing the staging copy.
    struct TextureCosts {
      uint64_t images = 0, bytes = 0;
      double query = 0, create = 0, allocate = 0, bind = 0, view = 0, stage = 0, slowest = 0;
    };
    TextureCosts takeTextureCosts();
    // The slowest prepare() since this was last asked.
    Profile takeWorstProfile() {
      const auto worst = m_worstProfile;
      m_worstProfile = {};
      return worst;
    }
    // The target is UNORM: fixed-function blending operates on what is
    // written. encodeOutput: draws write display values (gamma 2.2, the
    // swapchain's); otherwise linear ones.
    void configureTarget(VkRenderPass pass, VkExtent2D extent, bool encodeOutput = false);
    void setSkyTarget(VkRenderPass pass);
    void destroyTarget(); // Caller has completed all submitted work.
    void shutdownGpu();
    void selectFrame(uint32_t slot); // Caller has completed this slot's submission.
    VkFormat depthFormat() const;
    VkImageView depthView() const;
    std::shared_ptr<void> depthLifetime() const;
    bool preparePipelines(bool tracing);
    void prepare(VkCommandBuffer command,
                 bool tracing = false); // After prior frame fence, outside render pass.
    // Layers: 0 sky, 1 world, 2 view model, 3 UI and overlays. part: with
    // masked overlays, the commands before (1) or from (2) the coverage passes.
    void record(VkCommandBuffer command, bool skyOnly = false, bool skipSky = false,
                const float *skyJitter = nullptr, VkExtent2D skyExtent = {}, int firstLayer = 0,
                int lastLayer = 3, int part = 0); // Inside render pass, before ImGui.

    // Masked overlays (remixapi_InstanceInfoMaskEXT, masked UI commands).
    // maskRuns: coverage passes the last prepare() made; zero when the frame
    // has none. Such a frame records layers 0-1 part 1 (every world draw that
    // is not masked), leaves the target pass (its depth, stored, is what
    // depth-tested sources test), recordMasks (own passes, one per run),
    // re-enters a pass that loads colour and depth and records layers 0-1
    // part 2 (the masked world overlays and the blended draws sorted after the
    // first of them); then halos, then layers 2-3.
    uint32_t maskRuns() const;
    void recordMasks(VkCommandBuffer command);
    // Role CUT (remixapi_InstanceInfoMaskEXT) with a traced scene: the
    // coverage of the groups that hide traced instances from the camera, as
    // the last prepare(command, true) planned it. count is zero when nothing
    // cuts. Otherwise recordCutCoverage (own passes, outside a render pass)
    // must precede the path tracer's passes, which sample images[slot] (zero
    // or one, target resolution, no depth test) for the surfaces whose group
    // has slots' nibble (group - 1) equal to slot + 1.
    struct CutCoverage {
      uint32_t count = 0;
      VkDescriptorImageInfo images[scene::Mask::maxCutGroups] = {};
      uint32_t slots[4] = {};
      uint64_t digest = 0; // changes whenever the coverage may have
    };
    CutCoverage cutCoverage() const;
    void recordCutCoverage(VkCommandBuffer command);

    // Halo outlines (remixapi_InstanceInfoHaloEXT). haloCount: groups the last
    // prepare() made ready, in composite order; zero when the frame has none.
    // A frame with halos records layers 0-1, then per group: leaves the target
    // pass (its depth attachment, stored, holds the scene's depth),
    // recordHaloMask (own passes: silhouette mask, blur), re-enters a pass on
    // the target that loads its colour and recordHaloComposite; then layers
    // 2-3. The view model and the UI therefore draw over halos.
    uint32_t haloCount() const;
    void recordHaloMask(VkCommandBuffer command, uint32_t group);
    void recordHaloComposite(VkCommandBuffer command, uint32_t group);

    // Sprite overlays with SOFT_DEPTH (remixapi_InstanceInfoSpriteEXT) sample
    // the frame's opaque depth. wantsSceneDepth: the last prepare() made such
    // draws. A frame that has them leaves the target pass once the scene's
    // depth is in the target's depth attachment (stored) and before record()
    // draws the world layer's overlays, calls captureSceneDepth (a copy of
    // the attachment, outside a render pass; the attachment is in its
    // attachment layout before and after) and re-enters a pass that loads
    // colour and depth. Without the capture those draws are hard-edged.
    bool wantsSceneDepth() const;
    void captureSceneDepth(VkCommandBuffer command);
    // Particles (particles/renderer.h) sample the same copy: after prepare(),
    // requestSceneDepth makes wantsSceneDepth true for the frame; once
    // captured, sceneDepth is the copy (shader read-only layout), else null.
    void requestSceneDepth();
    VkDescriptorImageInfo sceneDepth() const;

    // The NUMOS sky (sky::Renderer::recordRasterComposite) over the target,
    // inside its pass: the target times transmittance, plus addition (both
    // sampled, target resolution). False while its pipelines compile.
    bool recordSkyComposite(VkCommandBuffer command, const VkDescriptorImageInfo &transmittance,
                            const VkDescriptorImageInfo &addition);
    // The target's depth attachment (depthView's image).
    VkImage depthImage() const;

    VkDescriptorImageInfo sampledMaterial(const std::shared_ptr<scene::Material> &material,
                                          VkCommandBuffer command);
    VkDescriptorImageInfo sampledEmission(const std::shared_ptr<scene::Material> &material,
                                          VkCommandBuffer command);
    VkDescriptorImageInfo sampledTexture(const std::shared_ptr<scene::Material> &material,
                                         const std::shared_ptr<scene::Texture> &texture,
                                         VkCommandBuffer command);
    VkImageView textureView(const scene::Texture *texture) const;
    // The texture's image holds sRGB-encoded texels (stored UNORM): shaders
    // sampling it as colour decode them with pow 2.2.
    bool textureSrgb(const scene::Texture *texture) const;

    // Changes whenever prepare() uploaded new pixels of a dynamic texture
    // (remixapi_UpdateTextureEXT): the images are not what the last frame saw.
    uint64_t dynamicTextureGeneration() const;

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m_gpu;
    Profile m_profile, m_completedProfile, m_worstProfile;
  };
} // namespace remix

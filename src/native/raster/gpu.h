#pragma once
#include "platform/memory_release.h"
#include "raster/renderer.h"
#include "platform/timing.h"
#include "runtime/pipelines.h"

#include <optional>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <utility>

namespace remix::raster::detail {
  using namespace scene;

  inline void check(VkResult result) {
    if (result != VK_SUCCESS) {
      throw result;
    }
  }
  // One buffer and its memory that many small buffers are parts of. Creating
  // a buffer with memory of its own takes the driver milliseconds (and mapping
  // one about half of one), which a frame that first draws fifty small meshes
  // paid fifty times: 130 ms for 70 KiB of geometry. Parts of an arena cost a
  // search of its free list.
  struct Arena {
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    uint8_t *mapped = nullptr; // staging arenas: mapped for their lifetime
    bool coherent = true;
    std::map<VkDeviceSize, VkDeviceSize> free; // offset -> size, neighbours merged
    // The first free range that holds size bytes, or false.
    bool take(VkDeviceSize bytes, VkDeviceSize &offset) {
      for (auto it = free.begin(); it != free.end(); ++it)
        if (it->second >= bytes) {
          offset = it->first;
          const VkDeviceSize rest = it->second - bytes;
          free.erase(it);
          if (rest)
            free.emplace(offset + bytes, rest);
          return true;
        }
      return false;
    }
    void give(VkDeviceSize offset, VkDeviceSize bytes) {
      auto next = free.lower_bound(offset);
      if (next != free.begin()) {
        auto before = std::prev(next);
        if (before->first + before->second == offset) {
          offset = before->first;
          bytes += before->second;
          free.erase(before);
        }
      }
      if (next != free.end() && offset + bytes == next->first) {
        bytes += next->second;
        free.erase(next);
      }
      free.emplace(offset, bytes);
    }
    bool empty() const { return free.size() == 1 && free.begin()->second == size; }
    ~Arena() {
      if (buffer)
        vkDestroyBuffer(device, buffer, nullptr);
      MemoryRelease::free(device, memory); // (its mapping with it)
    }
  };
  struct Buffer {
    VkDevice device;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    size_t capacity = 0;
    VkDeviceSize allocationSize = 0;
    VkBufferUsageFlags usage = 0;
    VkMemoryPropertyFlags flags = 0;
    // A part of an arena: `buffer` is the arena's and the part begins at
    // `offset` (0 for a buffer of its own). Every use adds it.
    std::shared_ptr<Arena> arena;
    VkDeviceSize offset = 0;
    ~Buffer() {
      if (arena) {
        arena->give(offset, allocationSize);
        return;
      }
      if (buffer) {
        vkDestroyBuffer(device, buffer, nullptr);
      }
      MemoryRelease::free(device, memory);
    }
  };
  // Device memory that texture images are parts of. An allocation of its own
  // took the driver 10 ms an image on average and up to 130 (Garry's Mod,
  // RTX 2080 on Thunderbolt: 900 ms over 92 images in a 75 s session), in the
  // frame that first drew the texture.
  struct MemoryBlock {
    VkDevice device = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    uint32_t type = 0;
    std::map<VkDeviceSize, VkDeviceSize> free; // offset -> size, neighbours merged
    // The first free range that holds bytes at the alignment, or false.
    bool take(VkDeviceSize bytes, VkDeviceSize alignment, VkDeviceSize &offset) {
      for (auto it = free.begin(); it != free.end(); ++it) {
        const VkDeviceSize begin = it->first, end = it->first + it->second;
        const VkDeviceSize aligned = (begin + alignment - 1) / alignment * alignment;
        if (aligned + bytes > end)
          continue;
        free.erase(it);
        if (aligned > begin)
          free.emplace(begin, aligned - begin);
        if (aligned + bytes < end)
          free.emplace(aligned + bytes, end - aligned - bytes);
        offset = aligned;
        return true;
      }
      return false;
    }
    void give(VkDeviceSize offset, VkDeviceSize bytes) {
      auto next = free.lower_bound(offset);
      if (next != free.begin()) {
        auto before = std::prev(next);
        if (before->first + before->second == offset) {
          offset = before->first;
          bytes += before->second;
          free.erase(before);
        }
      }
      if (next != free.end() && offset + bytes == next->first) {
        bytes += next->second;
        free.erase(next);
      }
      free.emplace(offset, bytes);
    }
    ~MemoryBlock() { MemoryRelease::free(device, memory); }
  };
  struct Image {
    VkDevice device;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    // A part of a block (memory is then null): returned to it.
    std::shared_ptr<MemoryBlock> block;
    VkDeviceSize blockOffset = 0, blockBytes = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t levels = 0;
    VkComponentMapping components = {};
    VkDeviceSize bytes = 0; // Device allocation size, for VRAM statistics.
    uint32_t width = 0, height = 0;
    VkImageCreateFlags flags = 0;
    ~Image() {
      if (view) {
        vkDestroyImageView(device, view, nullptr);
      }
      if (image) {
        vkDestroyImage(device, image, nullptr);
      }
      MemoryRelease::free(device, memory);
      if (block)
        block->give(blockOffset, blockBytes);
    }
  };
  struct Push {
    Matrix mvp;
    float tint[4];
    float uv[4] = {1, 1, 0, 0};
    uint32_t state[4] = {};
    float factor[4] = {};
  };
  static_assert(sizeof(Push) == 128);

  // A display-encoded value (gamma 2.2) to linear.
  inline float linear(float c) { return std::pow(c, 2.2f); }
  template <typename Map, typename Predicate> void prune(Map &map, Predicate predicate) {
    for (auto it = map.begin(); it != map.end();) {
      if (predicate(*it)) {
        it = map.erase(it);
      } else {
        ++it;
      }
    }
  }
} // namespace remix::raster::detail

namespace remix {
  struct RasterRenderer::Gpu {
    using Buffer = raster::detail::Buffer;
    using Arena = raster::detail::Arena;
    using MemoryBlock = raster::detail::MemoryBlock;
    using Image = raster::detail::Image;
    using Push = raster::detail::Push;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory = {};
    VkPhysicalDeviceProperties properties = {};
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkRenderPass pass = VK_NULL_HANDLE, skyPass = VK_NULL_HANDLE;
    VkExtent2D extent = {};
    bool encodeOutput = false;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    std::shared_ptr<Image> depth;
    struct MeshGpu {
      std::weak_ptr<scene::Surface> owner;
      std::unique_ptr<Buffer> data;
      VkDeviceSize indexOffset;
      VkDeviceSize spriteOffset = 0; // scene::Surface::sprite, after the indices (when it has any)
      // Object-space bounds of the vertices, computed on first halo use.
      bool bounded = false;
      float low[3] = {}, high[3] = {};
      // Object-space bounds of the index ranges drawn from the mesh, computed
      // when a range is first drawn: what frustum culling tests. A host that
      // draws a large mesh in parts (a world split by visibility cluster)
      // has its parts culled one by one.
      struct Range {
        bool valid = false; // false: an index outside the vertices, never culled
        float low[3] = {}, high[3] = {};
      };
      struct RangeKey {
        uint32_t first, count;
        int32_t base;
        bool operator==(const RangeKey &) const = default;
      };
      struct RangeHash {
        size_t operator()(const RangeKey &k) const {
          return size_t((uint64_t(k.first) * 0x9e3779b97f4a7c15ull) ^ (uint64_t(k.count) << 32) ^ uint32_t(k.base));
        }
      };
      std::unordered_map<RangeKey, Range, RangeHash> ranges;
    };
    struct TextureGpu {
      std::weak_ptr<scene::Texture> owner;
      std::unique_ptr<Image> data; // Null for a linear alias of another entry's image.
      VkImageView view = VK_NULL_HANDLE; // Sampled view: data->view or the alias.
      bool srgb = false;                 // Encoded sRGB texels (UNORM): the shaders decode them, pow 2.2.
      uint64_t revision = 0;             // scene::TextureUpdates::revision uploaded
      bool updatable = false;            // owner has TextureUpdates: refreshed by prepare()
    };
    struct MaterialGpu {
      VkDevice device;
      VkDescriptorPool pool;
      std::weak_ptr<scene::Material> owner;
      VkSampler sampler = VK_NULL_HANDLE;
      VkDescriptorSet set = VK_NULL_HANDLE;
      ~MaterialGpu() {
        if (set) {
          vkFreeDescriptorSets(device, pool, 1, &set);
        }
        if (sampler) {
          pipeline::destroySampler(device, sampler, nullptr);
        }
      }
    };
    struct Command {
      Push push;
      VkPipeline pipeline;
      VkDescriptorSet set;
      VkBuffer vertices, indices;
      VkDeviceSize vertexOffset, indexOffset;
      uint32_t firstIndex, count;
      int32_t baseVertex;
      int layer;
      // A masked overlay (raster/mask.cpp): the group's coverage, bound as set 1
      // of mask.layout, and the pixels the draw is confined to.
      VkDescriptorSet coverage;
      VkRect2D area;
      // A sprite overlay (raster/sprite.cpp): the mesh's sprite vertices as
      // binding 1 (same buffer as its indices), and its parameters, which
      // replace push.factor (1 / softDistance, fade start, fade end, modulate k).
      bool sprite = false;
      VkBuffer spriteBuffer = VK_NULL_HANDLE;
      VkDeviceSize spriteOffset = 0;
      float spriteParameters[4] = {};
    };
    // Diagnostics of the frame's staging uploads (RasterRenderer::Profile).
    RasterRenderer::TextureCosts textureCosts;
    double stagingAllocateMs = 0, stagingWriteMs = 0;
    // First uses in the frame: what creating them took.
    double createPipelineMs = 0, createMeshMs = 0, createTextureMs = 0, createMaterialMs = 0;
    uint64_t pipelinesCreated = 0, texturesCreated = 0;
    uint64_t stagingBuffers = 0, stagingAllocated = 0;
    uint32_t stagingMemoryFlags = 0;
    std::unordered_map<const scene::Surface *, MeshGpu> meshes;
    std::unordered_map<const scene::Texture *, TextureGpu> textures;
    std::unordered_map<const scene::Material *, std::unique_ptr<MaterialGpu>> materials;
    std::unordered_map<uint64_t, VkPipeline> pipelines;
    std::vector<std::unique_ptr<Buffer>> uploads;
    std::vector<std::unique_ptr<Buffer>> reusableBuffers;
    VkDeviceSize reusableBytes = 0;
    void recycle(std::unique_ptr<Buffer> buffer);
    std::unique_ptr<Buffer> skin;
    // Arenas small geometry is part of, and the ones its staging copies are
    // part of: those per frame slot, since a slot's staging is released
    // together when the slot comes round again.
    // Blocks texture images are parts of, and the next one, allocated ahead
    // on another thread so that no frame waits for it.
    std::vector<std::shared_ptr<MemoryBlock>> imageBlocks;
    std::future<std::shared_ptr<MemoryBlock>> spareImageBlock;
    // The spare is not allocated the moment its predecessor is taken: the
    // driver serializes its memory calls, and the image that needed the block
    // then waited behind the allocation to be bound (9-15 ms). It is asked for
    // here and allocated in the first frame that follows one without a new
    // image (launchSpareImageBlock, once a frame).
    static constexpr VkDeviceSize imageBlockSize = 64u * 1024u * 1024u;
    std::optional<uint32_t> spareImageWanted;
    uint32_t spareImageFrames = 0;
    uint64_t imagesCreated = 0, imagesSeen = 0;
    void launchSpareImageBlock();
    // `warm`: also pays the driver's first-use cost (for a spare, off the render thread).
    static std::shared_ptr<MemoryBlock> allocateBlock(VkDevice device, VkDeviceSize size, uint32_t type, bool warm);
    std::vector<std::shared_ptr<Arena>> geometryArenas;
    std::vector<std::shared_ptr<Arena>> stagingArenas[gpuFrameCount];
    std::unique_ptr<Buffer> part(std::vector<std::shared_ptr<Arena>> &arenas, VkDeviceSize size,
                                 VkBufferUsageFlags usage, VkMemoryPropertyFlags flags, VkDeviceSize arenaSize);
    struct FrameResources {
      std::vector<std::unique_ptr<Buffer>> uploads, retiredBuffers;
      std::vector<std::unique_ptr<Image>> retiredImages;
      std::vector<std::unique_ptr<MaterialGpu>> retiredMaterials;
      std::unique_ptr<Buffer> skin;
    } frames[gpuFrameCount];
    uint32_t frameIndex = 0;
    uint64_t uploadBytes = 0; // Staged by buffer(), for RasterRenderer::profile().
    // Counts the uploads that changed a dynamic texture's image
    // (remixapi_UpdateTextureEXT): what the images show, for the traced
    // scene's signature.
    uint64_t dynamicTextureUploads = 0;
    std::vector<Command> commands;
    // Halo outlines (remixapi_InstanceInfoHaloEXT, raster/halo.cpp). Nothing
    // here exists until a frame submits a halo instance; the objects are
    // destroyed with the target.
    struct Halo {
      struct Target {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
      };
      // Coverage at target resolution: the silhouette mask and two blur targets.
      Target targets[3];
      // loadPass: maskPass keeping the target's contents. earlyPass,
      // earlyLoadPass: the same two before the frame has a depth (the depth
      // attachment is neither loaded nor stored; untested silhouettes only).
      VkRenderPass maskPass = VK_NULL_HANDLE, loadPass = VK_NULL_HANDLE, blurPass = VK_NULL_HANDLE;
      VkRenderPass earlyPass = VK_NULL_HANDLE, earlyLoadPass = VK_NULL_HANDLE;
      VkPipelineLayout layout = VK_NULL_HANDLE; // blur and composite
      VkShaderModule maskFragment = VK_NULL_HANDLE, vertex = VK_NULL_HANDLE, blurFragment = VK_NULL_HANDLE,
                     compositeFragment = VK_NULL_HANDLE;
      VkSampler sampler = VK_NULL_HANDLE;
      // mask: [depth tested]; composite: [additive].
      VkPipeline mask[2] = {}, blur = VK_NULL_HANDLE, composite[2] = {};
      bool ready = false;  // every pipeline is compiled
      bool coverageReady = false; // the silhouette passes and mask pipelines (prepareCoverage)
      bool failed = false; // creation failed: no halos until the target changes
      struct Group {
        scene::Halo halo;
        uint32_t first = 0, count = 0; // in commands
        VkRect2D area = {};            // pixels the group can change
        bool bounded = false;
        float low[2] = {}, high[2] = {}; // silhouette bounds while accumulating
      };
      std::vector<Group> groups; // this frame's, in composite order
      std::vector<Command> commands;
    } halo;
    // Requests the halo pipelines (creating the passes they need); false while
    // they compile.
    bool prepareHalo();
    void createHaloTargets();
    void destroyHaloTargets();
    // Sets the group's pixel area from its silhouette bounds and blur reach;
    // false when it reaches no pixel.
    bool haloArea(Halo::Group &group) const;
    void destroyHalo();
    // Silhouette coverage, shared by halo outlines and masked overlays: the
    // sampler, the silhouette passes (halo.maskPass, loadPass) and the
    // silhouette pipelines (halo.mask). False while they compile.
    bool prepareCoverage();
    // A silhouette pipeline for halo.maskPass: the raster vertex stage, the
    // given fragment stage and layout, both faces, optionally depth-tested
    // (biased) against the frame's depth. False while it compiles.
    bool coveragePipeline(VkPipeline &out, VkShaderModule fragmentModule, VkPipelineLayout pipelineLayout,
                          bool tested);
    // An R8 target at target resolution with its framebuffer (tested: with the
    // frame's depth attachment, for halo.maskPass/loadPass) and sampling set.
    void createCoverageTarget(Halo::Target &target, bool tested);
    void destroyCoverageTarget(Halo::Target &target);
    // Sets the group's pixel area from its silhouette bounds grown by margin
    // (pixels, x and y); false when it reaches no pixel.
    bool coverageArea(Halo::Group &group, const float *margin) const;
    // Masked overlays (remixapi_InstanceInfoMaskEXT and masked UI commands,
    // raster/mask.cpp). As for halos, nothing exists until a frame uses a mask.
    struct Mask {
      // One coverage target per group that has sources in a frame, in the order
      // the groups first appear; kept until the target changes.
      std::vector<Halo::Target> targets;
      VkPipelineLayout layout = VK_NULL_HANDLE; // the raster layout and, set 1, a coverage target
      VkShaderModule withinFragment = VK_NULL_HANDLE, overlayFragment = VK_NULL_HANDLE;
      VkPipeline within[2] = {}; // sources limited to another group: [depth tested]
      bool ready = false, failed = false;
      // This frame's coverage passes, in order: sources with equal parameters.
      struct Run {
        uint32_t target = 0;
        int32_t within = -1;           // the target limiting these sources
        bool clear = false;            // the target's first run
        uint32_t first = 0, count = 0; // in commands
        bool early = false;            // also drawn before the scene is traced (cut)
      };
      std::vector<Run> runs;
      std::vector<Command> commands;
      int32_t slot[scene::Mask::maxGroup + 1] = {}; // group -> target this frame, -1: no sources
      VkRect2D area[scene::Mask::maxGroup] = {};    // per target: pixels its coverage can be set in
      uint32_t used = 0;                            // targets holding this frame's coverage
      // Commands [0, split) are recorded before the coverage passes: every
      // world-layer draw that is not masked.
      size_t split = 0;
      // Role CUT with a traced scene: the path tracer resolves primary
      // visibility long before the coverage passes, so the runs of the groups
      // that cut (and of the groups those are `within`) are marked early and
      // drawn once more ahead of it, into the same targets, without the depth
      // test (there is no depth yet). The tracer samples the targets; the
      // coverage passes then draw every run again for the masked overlays.
      struct Cut {
        uint32_t count = 0;                             // groups that cut this frame
        uint32_t targets[scene::Mask::maxCutGroups] = {}; // their targets, in slot order
        // Four bits per group (1..32): its slot + 1, or 0 when it does not cut.
        uint32_t slots[4] = {};
        uint64_t digest = 0; // changes with the early sources
      } cut;
    } mask;
    // Sprite overlays (remixapi_InstanceInfoSpriteEXT, raster/sprite.cpp).
    struct Sprite {
      VkPipelineLayout layout = VK_NULL_HANDLE; // the raster layout and, set 1, the scene's depth
      VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
      std::unique_ptr<Image> depth; // copy of the target's depth attachment
      VkSampler sampler = VK_NULL_HANDLE;
      VkDescriptorSet set = VK_NULL_HANDLE;
      bool failed = false;
      uint32_t soft = 0;     // this frame's commands with SOFT_DEPTH
      bool requested = false; // particles sample the copy this frame
      bool captured = false; // this frame's depth is in the copy
    } sprite;
    // The layout and shader modules; false when they cannot be created.
    bool prepareSprite();
    void destroySprite();
    // The NUMOS sky over the target (raster/sky.cpp): fullscreen draws that
    // multiply it by the sky's transmittance, then add its light.
    struct SkyComposite {
      VkPipelineLayout layout = VK_NULL_HANDLE; // one sampled image (setLayout)
      VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
      VkPipeline multiply = VK_NULL_HANDLE, add = VK_NULL_HANDLE;
      // Per frame slot: the transmittance's and the addition's sets.
      VkDescriptorSet sets[gpuFrameCount][2] = {};
    } skyComposite;
    // False while the pipelines compile.
    bool prepareSkyComposite();
    void destroySkyComposite();
    bool prepareMask();
    // The coverage passes: every run, or (early) the runs marked early.
    void recordMaskRuns(VkCommandBuffer command, bool early);
    void createMaskTargets(uint32_t count);
    void destroyMask();
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();

    ~Gpu();
    void destroyTarget();
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags);
    std::unique_ptr<Buffer> allocateBuffer(size_t size, VkBufferUsageFlags usage,
                                           VkMemoryPropertyFlags flags);
    std::unique_ptr<Buffer> buffer(const void *data, size_t size, VkBufferUsageFlags usage);
    // Host-visible buffer whose contents write(mapped) fills directly.
    std::unique_ptr<Buffer> buffer(size_t size, VkBufferUsageFlags usage,
                                   const std::function<void(uint8_t *)> &write);
    // Large sampled images whose last use has completed, reused by image() for
    // an identical description (a per-frame screen overlay of the same size).
    std::vector<std::unique_ptr<Image>> reusableImages;
    void recycle(std::unique_ptr<Image> image);
    std::unique_ptr<Buffer> geometryBuffer(const void *data, size_t size, VkBufferUsageFlags usage,
                                           VkCommandBuffer command);
    std::unique_ptr<Image> image(uint32_t width, uint32_t height, uint32_t levels, VkFormat format,
                                 bool isDepth, VkImageCreateFlags flags = 0,
                                 VkComponentMapping components = {});
    MeshGpu &mesh(const std::shared_ptr<scene::Surface> &source, VkCommandBuffer command);
    TextureGpu &texture(const std::shared_ptr<scene::Texture> &source, VkCommandBuffer command);
    TextureGpu &createTexture(const std::shared_ptr<scene::Texture> &source, VkCommandBuffer command);
    // Uploads the dirty region of an updatable texture into its image.
    void refreshTexture(TextureGpu &value, const scene::Texture &source, VkCommandBuffer command);
    void refreshLevels(TextureGpu &value, const scene::Texture &source, VkCommandBuffer command);
    TextureGpu uploadPixels(const std::shared_ptr<scene::Texture> &owner, const scene::Pixels &p,
                            VkCommandBuffer command, VkImageCreateFlags flags);
    TextureGpu &compressedTexture(const std::shared_ptr<scene::Texture> &source, VkCommandBuffer command);
    void copyLevels(VkCommandBuffer command, VkImage image, uint32_t levels,
                    const std::vector<VkBufferImageCopy> &copies);
    bool textureCompression = false; // Device enabled textureCompressionBC.
    bool fillModeNonSolid = false;   // wireframe draws as lines (else filled)
    float maxAnisotropy = 0;         // Material sampler anisotropy (0: samplerAnisotropy unavailable).
    bool memoryBudget = false;       // VK_EXT_memory_budget queries available.
    // Top mip levels to leave out of a new texture: large textures give up
    // detail as device-local memory fills, instead of failing allocations.
    uint32_t textureMipSkip(uint32_t width, uint32_t height, uint32_t levels) const;
    VkDescriptorSet material(const std::shared_ptr<scene::Material> &source, VkCommandBuffer command);
    // masked: the variant tested against a group's coverage (mask.layout).
    // sprite: the sprite overlay variant (sprite.layout), not for masked draws.
    VkPipeline pipeline(const scene::Draw &draw, bool masked = false, bool sprite = false);
  };
} // namespace remix

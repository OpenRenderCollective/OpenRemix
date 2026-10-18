#pragma once
// Objective-C++ only: the Metal raster renderer's GPU state.
#include "metal/gpu.h"
#include "metal/raster.h"
#include <chrono>

namespace remix::metal {
  // raster.vert/raster.frag push constants (raster/gpu.h Push).
  struct RasterPush {
    scene::Matrix mvp;
    float tint[4];
    float uv[4] = {1, 1, 0, 0};
    uint32_t state[4] = {};
    float factor[4] = {};
  };
  static_assert(sizeof(RasterPush) == 128);
  // Color targets a draw list is recorded into.
  enum class Target : uint8_t { Display, Sky };
  struct SampledImage {
    id<MTLTexture> texture = nil;
    id<MTLSamplerState> sampler = nil;
  };

  struct Raster::Gpu {
    id<MTLDevice> device = nil;
    MTLPixelFormat displayFormat = MTLPixelFormatBGRA8Unorm_sRGB;
    static constexpr MTLPixelFormat depthFormat = MTLPixelFormatDepth32Float;
    static constexpr MTLPixelFormat skyFormat = MTLPixelFormatRGBA32Float;
    // UNORM display formats encode sRGB in the shader (raster.frag state.w bit 1).
    bool encodeOutput = false;
    id<MTLTexture> depth = nil; // display depth, shared with the tracer's sky pass
    uint32_t width = 0, height = 0;
    struct MeshGpu {
      std::weak_ptr<scene::Surface> owner;
      id<MTLBuffer> data = nil; // vertices, indices, then sprite vertices
      size_t indexOffset = 0;
      size_t spriteOffset = 0; // scene::Surface::sprite, after the indices (when it has any)
      // Object-space bounds of the vertices, computed on first silhouette use.
      bool bounded = false;
      float low[3] = {}, high[3] = {};
      // Object-space bounds of the index ranges drawn from the mesh: what
      // frustum culling tests (raster/gpu.h MeshGpu::ranges).
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
      id<MTLTexture> texture = nil;
      uint64_t bytes = 0;
      bool compressed = false;
      uint64_t revision = 0; // scene::TextureUpdates::revision uploaded
    };
    struct MaterialGpu {
      std::weak_ptr<scene::Material> owner;
      id<MTLSamplerState> sampler = nil;
      ArgumentSet set; // raster.frag set 0: albedo
    };
    struct Command {
      RasterPush push;
      uint64_t pipeline;    // key(draw, Target::Display)
      uint64_t skyPipeline; // key(draw, Target::Sky)
      id<MTLBuffer> material = nil;
      id<MTLTexture> texture = nil;
      id<MTLBuffer> vertices = nil, indices = nil;
      size_t vertexOffset = 0, indexOffset = 0;
      uint32_t firstIndex = 0, count = 0;
      int32_t baseVertex = 0;
      int layer = 0;
      bool doubleSided = false, depthBias = false;
      // A masked overlay (raster/mask.cpp): its group's coverage target, and
      // the pixels the draw is confined to.
      int32_t coverage = -1;
      MTLScissorRect area = {};
      // A sprite overlay (raster/sprite.cpp): the mesh's sprite vertices, and
      // the parameters that replace push.factor (1 / softDistance, fade start,
      // fade end, modulate k).
      bool sprite = false;
      size_t spriteOffset = 0;
      float spriteParameters[4] = {};
      // A silhouette (halo or mask source): depth-tested against the frame's depth.
      bool tested = false;
    };
    // An R8 coverage target at target resolution (halo outlines and masked
    // overlays) with the argument buffers that sample it.
    struct CoverageTarget {
      id<MTLTexture> texture = nil;
      ArgumentSet overlay, within;       // maskOverlay set 1, maskWithin set 1
      ArgumentSet blur, composite, mask; // haloBlur set 0, haloComposite sets 0 and 1
    };
    CoverageTarget coverageTarget(bool halo);
    static constexpr MTLPixelFormat coverageFormat = MTLPixelFormatR8Unorm;
    id<MTLSamplerState> coverageSampler = nil;
    // Silhouette pipelines: [fragment stage within][depth tested].
    id<MTLRenderPipelineState> silhouettes[2] = {};
    id<MTLDepthStencilState> silhouetteDepth[2] = {};
    id<MTLRenderPipelineState> silhouettePipeline(bool within);
    void prepareSilhouettes();
    id<MTLDepthStencilState> noDepth = nil; // halo composite
    // Silhouettes [first, first + count) of list; within: the target limiting
    // them (-1: none); early: ahead of the frame, untested.
    void drawSilhouettes(id<MTLRenderCommandEncoder> encoder, const std::vector<Command> &list, uint32_t first,
                         uint32_t count, int32_t within, bool early);
    // Halo outlines (remixapi_InstanceInfoHaloEXT, raster/halo.cpp).
    struct Halo {
      CoverageTarget targets[3]; // silhouette mask, two blur targets
      id<MTLRenderPipelineState> blur = nil, composite[2] = {}; // composite: [additive]
      struct Group {
        scene::Halo halo;
        uint32_t first = 0, count = 0; // in commands
        MTLScissorRect area = {};      // pixels the group can change
        bool bounded = false;
        float low[2] = {}, high[2] = {}; // silhouette bounds while accumulating
      };
      std::vector<Group> groups; // this frame's, in composite order
      std::vector<Command> commands;
    } halo;
    void prepareHalo();
    // Sets the group's pixel area from its silhouette bounds grown by margin
    // (pixels, x and y); false when it reaches no pixel.
    bool coverageArea(Halo::Group &group, const float *margin) const;
    bool haloArea(Halo::Group &group) const;
    uint32_t haloCount() const { return uint32_t(halo.groups.size()); }
    // Draws group index's silhouette and blurs it (own passes).
    void recordHaloMask(id<MTLCommandBuffer> command, uint32_t index);
    // Composites group index onto the frame (the frame's pass).
    void recordHaloComposite(id<MTLRenderCommandEncoder> encoder, uint32_t index);
    // Masked overlays (remixapi_InstanceInfoMaskEXT, raster/mask.cpp).
    struct Mask {
      std::vector<CoverageTarget> targets;
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
      MTLScissorRect area[scene::Mask::maxGroup] = {}; // per target: pixels its coverage can be set in
      uint32_t used = 0;
      // Commands [0, split) are recorded before the coverage passes.
      size_t split = 0;
      // Role CUT with a traced scene (raster/gpu.h Mask::Cut).
      struct Cut {
        uint32_t count = 0;
        uint32_t targets[scene::Mask::maxCutGroups] = {};
        uint32_t slots[4] = {};
        uint64_t digest = 0;
      } cut;
    } mask;
    uint32_t maskRuns() const { return uint32_t(mask.runs.size()); }
    // The coverage passes: every run, or (early) the runs marked early, ahead
    // of the path tracer and without the depth test.
    void recordMasks(id<MTLCommandBuffer> command, bool early);
    // Sprite overlays (remixapi_InstanceInfoSpriteEXT, raster/sprite.cpp).
    struct Sprite {
      id<MTLTexture> depth = nil; // copy of the target's depth attachment
      ArgumentSet set;            // spriteFrag set 1: the copy
      uint32_t soft = 0;          // this frame's commands with SOFT_DEPTH
      bool captured = false;      // this frame's depth is in the copy
    } sprite;
    static constexpr uint32_t spriteBufferIndex = 29;
    bool wantsSceneDepth() const { return sprite.soft != 0; }
    // Copies the frame's depth for soft sprites (between the frame's passes).
    void captureSceneDepth(id<MTLCommandBuffer> command);
    // The frame's passes are split (masks, halos or soft sprites): its depth
    // attachment is stored for the passes that resume it.
    bool splitsFrame() const { return wantsSceneDepth() || maskRuns() || haloCount(); }
    // This frame draws something tested against the traced scene's depth,
    // which the composite leaves in the depth attachment: a world-layer draw
    // (the view model and HUD layers clear depth first), a depth-tested halo
    // or mask source, or a soft sprite.
    bool testsSceneDepth() const;
    // The target blends linear values (sRGB or floating-point format).
    bool linearTarget = true;
    std::unordered_map<const scene::Surface *, MeshGpu> meshes;
    std::unordered_map<const scene::Texture *, TextureGpu> textures;
    std::unordered_map<const scene::Material *, std::unique_ptr<MaterialGpu>> materials;
    std::unordered_map<uint64_t, id<MTLRenderPipelineState>> pipelines;
    std::unordered_map<uint64_t, id<MTLDepthStencilState>> depthStates;
    id<MTLBuffer> skin = nil;
    // Retired GPU objects stay referenced until their slot's frame completes.
    struct FrameResources {
      std::vector<id> retired;
    } frames[gpuFrameCount];
    uint32_t frameIndex = 0;
    uint64_t uploadBytes = 0;
    // Uploads that changed a dynamic texture's image (remixapi_UpdateTextureEXT):
    // part of the traced scene's signature.
    uint64_t dynamicTextureUploads = 0;
    void refreshTexture(TextureGpu &value, const scene::Texture &source);
    std::vector<Command> commands;
    static constexpr uint32_t vertexBufferIndex = 30;
    id<MTLBuffer> clearGeometry = nil, clearMaterial = nil;
    void clearDepth(id<MTLRenderCommandEncoder> encoder);
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();

    void configureTarget(MTLPixelFormat format, uint32_t width, uint32_t height, bool encode);
    MeshGpu &mesh(const std::shared_ptr<scene::Surface> &source);
    TextureGpu &texture(const std::shared_ptr<scene::Texture> &source);
    TextureGpu uploadPixels(const std::shared_ptr<scene::Texture> &owner, const scene::Pixels &pixels);
    TextureGpu &compressedTexture(const std::shared_ptr<scene::Texture> &source);
    MaterialGpu &material(const std::shared_ptr<scene::Material> &source);
    // The pipeline key of a draw (raster/pipelines.cpp), with its target.
    static uint64_t key(const scene::Draw &draw, Target target);
    id<MTLRenderPipelineState> pipeline(uint64_t key);
    id<MTLDepthStencilState> depthState(uint64_t key);
    // Records the prepared commands. Display: everything (skipSky leaves the
    // sky layer out); Sky: only the sky layer into the tracer's sky image,
    // jittered like the primary rays.
    // Layers firstLayer..lastLayer; part 1: the commands before the mask
    // coverage passes, part 2: the rest, 0: all.
    void record(id<MTLRenderCommandEncoder> encoder, Target target, bool skipSky, const float *skyJitter,
                uint32_t width, uint32_t height, int firstLayer = 0, int lastLayer = 3, int part = 0);
    // Path tracer material descriptors (raster/renderer.h sampled*).
    SampledImage sampledMaterial(const std::shared_ptr<scene::Material> &source);
    SampledImage sampledTexture(const std::shared_ptr<scene::Material> &source,
                                const std::shared_ptr<scene::Texture> &image);
    void retire(id object) {
      if (object)
        frames[frameIndex].retired.push_back(object);
    }
  };
} // namespace remix::metal

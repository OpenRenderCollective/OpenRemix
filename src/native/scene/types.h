#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <remix/remix_c.h>
#include <unordered_map>
#include <vector>

namespace remix {
  namespace scene {
    using Matrix = std::array<float, 16>; // Column vectors, column-major storage.
    Matrix identity();
    Matrix inverse(const Matrix &matrix);
    uint64_t hashBytes(const void *bytes, size_t size, uint64_t seed = 1469598103934665603ull);
    // Bulk content hash (8 bytes per step) for change detection within a run.
    uint64_t fastHash(const void *bytes, size_t size, uint64_t seed = 0);
    Matrix multiply(const Matrix &a, const Matrix &b);
    Matrix transform(const remixapi_Transform &value);
    const void *extension(const void *next, remixapi_StructType type);

    // Block-compressed texture kept compressed on the GPU ("Keep BC textures
    // compressed"). Pixels::mips/hdrMips stay empty; CPU consumers read only the
    // small derived data below, never a full decoded copy.
    struct CompressedPixels {
      uint32_t format = 0;   // remixapi_Format as submitted (a BC format)
      uint32_t block = 0;    // 1 BC1, 3 BC3, 4 BC4, 5 BC5, 6 BC6H UF16, 16 BC6H SF16, 7 BC7
      bool bc1Alpha = false; // BC1 RGBA semantics; BC1 RGB samples alpha as one
      uint32_t levels = 0;   // supplied mip levels
      uint64_t hash = 0;     // content hash of every supplied block
      // Supplied blocks, kept only until the one-time GPU upload releases them.
      std::vector<uint8_t> upload;
      // One bit per mip-0 texel (row-major) whose decoded alpha is not 255.
      // Empty when every texel is opaque. Opacity partitions read only this.
      std::vector<uint64_t> transparent;
      bool transparentTexel(uint32_t x, uint32_t y, uint32_t width) const {
        if (transparent.empty())
          return false;
        const size_t i = size_t(y) * width + x;
        return (transparent[i >> 6] >> (i & 63)) & 1;
      }
      // Small decoded level (at most 64x64 texels) for CPU emission statistics:
      // RGBA8 like Pixels::mips, plus linear RGBA32F for BC6H.
      uint32_t previewWidth = 0, previewHeight = 0;
      std::vector<uint8_t> preview;
      std::vector<float> hdrPreview;
    };
    struct Pixels {
      uint32_t width = 0, height = 0;
      bool srgb = true;
      std::vector<std::vector<uint8_t>> mips;
      std::vector<std::vector<float>> hdrMips; // Linear RGBA32F, retained through upload.
      bool twoChannel = false; // BC5 stores normal X/Y; reconstruct Z when used as a normal map.
      std::shared_ptr<CompressedPixels> compressed; // Set only for GPU-compressed textures.
      uint32_t levels() const { return compressed ? compressed->levels : uint32_t(mips.size()); }
    };
    // In-place region updates of a texture (UI textures:
    // remixapi_UpdateUITextureRegionEXT). `revision` counts them; `dirty`
    // (x0, y0, x1, y1) is the union of regions not yet on the GPU. The lock
    // guards both and the texels while they are written or uploaded.
    struct TextureUpdates {
      std::mutex lock;
      uint64_t revision = 0;
      uint32_t dirty[4] = {};
    };
    // Texture and material turnover (remixapi_TextureStatisticsEXT), counted
    // wherever it happens: API calls, the rendering thread, scene preparation.
    struct TextureCounters {
      std::atomic<uint64_t> texturesCreated{0}, texturesDestroyed{0}, materialsCreated{0}, materialsDestroyed{0};
      std::atomic<uint64_t> updateCalls{0}, updates{0}, updateBytes{0}, updateTicks{0};
      std::atomic<uint64_t> uploads{0}, uploadBytes{0}, uploadTicks{0};
      std::atomic<uint64_t> gpuTexturesCreated{0}, gpuTexturesDestroyed{0}, gpuMaterialsCreated{0},
          gpuMaterialsDestroyed{0}, gpuCreateBytes{0}, gpuCreateTicks{0}, gpuDestroyTicks{0};
      std::atomic<uint64_t> emissionRefreshes{0}, emissionRefreshTicks{0};
    };
    inline TextureCounters textureCounters;
    // Adds the steady-clock ticks since construction to a counter.
    struct CounterTicks {
      std::atomic<uint64_t> &out;
      std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
      ~CounterTicks() {
        out.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - start)
                                   .count()),
                      std::memory_order_relaxed);
      }
    };
    struct Texture {
      Pixels pixels;
      std::shared_ptr<TextureUpdates> updates; // Set for updatable textures only.
      // remixapi_TextureInfoDynamicEXT: a scene texture rewritten in place
      // (NativeResources::updateTexture). One RGBA8 level; `updates` is set and
      // its lock guards the texels. Its fingerprint is its identity, not its
      // contents; CPU consumers of the texels follow the rules in the header.
      bool dynamic = false;
      mutable std::weak_ptr<Texture> linear;
      std::shared_ptr<Texture> encodedSource; // Keep a file-backed data view's weak path cache alive.
      uint64_t hash = 0;
      mutable uint64_t contentHash = 0;
      uint64_t fingerprint() const;
      uint64_t getHash() const {
        if (hash)
          return hash;
        if (contentHash)
          return contentHash;
        return fingerprint();
      }
    };
    struct Vertex {
      float position[3];
      float uv[2];
      float color[4];
      float normal[3] = {}; // Zero selects the geometric-normal fallback.
    };
    struct Material {
      uint64_t sourceHash = 0;
      std::shared_ptr<Texture> texture;
      std::shared_ptr<Texture> emissionTexture;
      std::shared_ptr<Texture> roughnessTexture, metallicTexture, normalTexture;
      float roughness = 1, metallic = 0;
      bool pbr = false;
      uint32_t normalEncoding = 0;
      // remixapi_MaterialPbrRemapEXT, per map (roughness, metallic).
      struct Remap {
        uint8_t channel = 0, curve = 0;
        bool invert = false;
        float exponent = 1, bias = 0, scale = 1;
        bool identity() const { return !channel && !curve && !invert && exponent == 1 && bias == 0 && scale == 1; }
        bool operator==(const Remap &) const = default;
      } remap[2];
      // remixapi_MaterialNormalScrollEXT (normal encoding 5): layers, then the
      // base, layer 1 and layer 2 rates.
      uint32_t scrollLayers = 1;
      float scroll[6] = {};
      // A translucent body (remixapi_MaterialInfoTranslucentEXT without a thin
      // wall): its refractive index and measurement distance (0: a thin sheet).
      float refractiveIndex = 1.5f, transmittanceDistance = 0;
      float emission[3] = {};
      bool suppressEmissionLight = false;
      // remixapi_MaterialInfoSelfIlluminationEXT: 0 off, else the mask + 1
      // (1 none, 2 albedo alpha, 3 emissive alpha), and the mask scale.
      uint8_t selfIllumination = 0;
      float selfIlluminationScale = 1;
      mutable std::unordered_map<uint64_t, std::array<float, 3>> emissionCache;
      // With a dynamic emission or self-illumination texture, emissionCache
      // holds the estimates of one image: the texture revisions it was filled
      // from, when, how long it is kept although they changed, and a count of
      // the times it was emptied for a newer image (scene/emission.cpp).
      mutable uint64_t emissionRevision = 0, emissionSerial = 0;
      mutable std::chrono::steady_clock::time_point emissionMade{};
      mutable double emissionKeepSeconds = 0;
      float tint[4] = {1, 1, 1, 1};
      uint32_t compare = 7;
      float reference = 0;
      bool drawAlpha = false, translucent = false, transmission = false;
      uint8_t filter = 0, wrapU = 1, wrapV = 1;
      uint8_t rows = 1, columns = 1, fps = 0;
      // Set once the handle stops resolving to this object (replaced or
      // destroyed), so retained instances know to resolve the handle again.
      bool retired = false;
    };
    // What a backend's shader decodes for a material beyond its maps.
    struct MaterialDecode {
      // Self-illumination: enabled, and its mask (0 none, 1 albedo alpha,
      // 2 emissive alpha) in the two bits above.
      static constexpr uint32_t selfIllumination = 32, selfIlluminationMaskShift = 6;
      uint32_t flags = 0;
      float selfIlluminationScale = 0;
    };
    MaterialDecode materialDecode(const Material &material);
    // remixapi_MeshInfoSpriteVerticesEXT: a vertex's sprite parameters.
    struct SpriteVertex {
      float frame[3];    // u1, v1, weight1
      float sequence[4]; // u2, v2, u3, v3
      float weight2;
    };
    struct Surface {
      std::vector<Vertex> vertices;
      std::vector<SpriteVertex> sprite; // empty, or one per vertex
      std::vector<uint32_t> indices;
      std::vector<float> weights;
      std::vector<uint32_t> bones;
      uint32_t bonesPerVertex = 0;
      remixapi_MaterialHandle material = nullptr;
      std::array<float, 3> center = {};
    };
    struct MeshUpdateStream {};
    // Counts destroyed Mesh objects: a consumer holding weak references to
    // meshes (the tracer's views) need not look for expired ones while it has
    // not changed.
    inline std::atomic<uint64_t> meshReleases{0};
    struct Mesh {
      std::shared_ptr<const MeshUpdateStream> updateStream;
      bool motionCorrespondence = false;
      std::vector<std::shared_ptr<Surface>> surfaces;
      bool retired = false; // See Material::retired.
      struct Release {
        // Release: the mesh expired first, for a reader on another thread.
        ~Release() { meshReleases.fetch_add(1, std::memory_order_release); }
      } release;
    };
    struct Blend {
      bool enabled = false;
      uint32_t src = 6, dst = 7, op = 0, srcAlpha = 1, dstAlpha = 7, alphaOp = 0;
      uint32_t mask = 15;
      // Supported legacy texture operations: select arg1/arg2, modulate.
      uint32_t colorOp = 3, color1 = 1, color2 = 2;
      uint32_t alphaOpTexture = 1, alpha1 = 1, alpha2 = 0;
      uint32_t factor = 0xffffffff;
    };
    struct SphereLight {
      uint64_t id = 0;
      float position[3] = {}, radius = 0;
      float radiance[3] = {};
      // Triangle area emitters share the light hierarchy and stable identity path.
      uint32_t triangle = 0; // 0 sphere, 1 one-sided, 2 two-sided
      float corners[3][3] = {};
      float direction[3] = {0, 0, -1}, coneCos = -1, softness = 0, focus = 0;
      // IES profile (NativeResources::iesProfiles) + 1; 0: none.
      uint32_t ies = 0;
      // Light cookie (NativeResources::cookieTextures) + 1, 0: none; its
      // projection's up and the tangent of half its width.
      uint32_t cookie = 0;
      float cookieUp[3] = {0, 0, 1}, cookieTan = 0;
      // Scale of its light in volumetric media (the light's volumetricRadianceScale).
      float volumetric = 1;
      // Emitter of an instance the host identified as dynamic
      // (remixapi_InstanceIdentityEXT): kept in the light tree's dynamic part.
      bool dynamic = false;
      // Triangle emitters: the surface draw they belong to (0 for sphere
      // lights) and their place in a spatial order of its emitters. The light
      // tree keeps a surface's emitters together in that order and makes
      // leaves of runs of them (LightBvh::setClusterSize).
      uint64_t group = 0;
      uint32_t rank = 0;
      bool operator==(const SphereLight &) const = default;
    };
    struct DomeLight {
      bool enabled = false;
      std::shared_ptr<Texture> texture;
      Matrix worldToLocal = identity();
      std::array<float, 3> radiance{};
    };
    struct DistantLight {
      float direction[3] = {0, -1, 0}; // Direction in which light travels.
      float radiance[3] = {};
      float angularDiameterDegrees = 0;
      // Scale of its light in volumetric media (the light's volumetricRadianceScale).
      float volumetric = 1;
      bool isDistant = false;
      bool operator==(const DistantLight &) const = default;
    };
    struct Draw {
      std::shared_ptr<Mesh> mesh;
      uint64_t instanceId = 0;
      uint32_t instanceIndex = 0, surfaceIndex = 0;
      uint32_t classification = 1; // dynamic for callers without the extension
      bool rasterVisible = true;
      bool traced() const {
        return !ui && camera == REMIXAPI_CAMERA_TYPE_WORLD && classification != 2 && blend.mask != 0 &&
               !maskGroup;
      }
      std::shared_ptr<Surface> surface;
      std::shared_ptr<Material> material;
      std::shared_ptr<std::vector<Matrix>> bones;
      Matrix model = identity();
      Blend blend;
      uint32_t camera = REMIXAPI_CAMERA_TYPE_WORLD;
      bool doubleSided = false, ui = false, depth = true, ignoreAlpha = false;
      bool depthWriteOverride = false, depthWrite = true, depthBias = false;
      uint32_t depthCompare = 3;
      bool decal = false;
      // remixapi_InstanceInfoWireframeEXT: drawn as its triangles' edges
      // (raster-only, line polygon mode).
      bool wireframe = false;
      // A view-model draw moved into the world camera's view
      // (NativeResources::applyViewModel): traced in front of the scene, seen
      // by no ray but the camera's and those of paths that start on it.
      bool viewModel = false;
      // THIRD_PERSON_PLAYER_MODEL / _BODY instances (dxvk-remix semantics): the
      // player's own body in a first-person game. Neither the camera nor the
      // raster passes see it, nor shadow and light-visibility rays (the eye and
      // the player's lights are inside it); reflection and bounce rays do.
      bool reflectionOnly = false;
      bool alphaOverride = false;
      uint32_t compare = 7;
      float reference = 0;
      uint32_t canvasWidth = 0, canvasHeight = 0;
      uint32_t firstIndex = 0, indexCount = 0;
      int32_t vertexOffset = 0;
      // Masked overlay (remixapi_InstanceInfoMaskEXT INSIDE/OUTSIDE, or a UI
      // command's mask flags): rasterized only where the group's coverage is
      // set (or, maskOutside, clear). Never traced. 0: unmasked.
      uint8_t maskGroup = 0;
      bool maskOutside = false;
      // remixapi_InstanceInfoMaskEXT CUT: an ordinary instance the camera does
      // not see where the group's coverage is set. Traced: primary visibility
      // skips it there. Rasterized: drawn only outside the coverage. 0: none.
      uint8_t cutGroup = 0;
      // remixapi_InstanceInfoCameraCullingEXT flags: what camera rays skip of
      // a traced instance (1 back faces, 2 clusters the camera cannot see).
      uint8_t cameraCull = 0;
      // remixapi_InstanceInfoRasterOrderEXT: the host's place for a blended
      // raster-only draw among its layer's (0: sorted by depth).
      uint32_t rasterOrder = 0;
      // remixapi_InstanceInfoSpriteEXT (0: none), on blended raster-only draws.
      uint32_t spriteFlags = 0;
      float spriteSoft[3] = {}; // softDistance, softFadeStart, softFadeEnd
    };
    // remixapi_InstanceInfoHaloEXT, validated and clamped. Instances whose
    // parameters compare equal share one group (one silhouette mask).
    struct Halo {
      float color[3] = {1, 1, 1}; // display-referred, 0..1
      float blurX = 2, blurY = 2; // pixels at 1080p output height, (0, maxBlur]
      uint32_t passes = 1;        // 1..maxPasses
      bool additive = true, ignoreZ = false;
      static constexpr float maxBlur = 64;
      static constexpr uint32_t maxPasses = 8;
      bool operator==(const Halo &) const = default;
    };
    // Halo sources of one frame. Their draws are in no other list: only the
    // rasterizer's halo passes read them (never traced, lit or drawn).
    struct HaloGroup {
      Halo halo;
      std::vector<Draw> draws;
    };
    // remixapi_InstanceInfoMaskEXT with role SOURCE, validated.
    struct Mask {
      static constexpr uint32_t maxGroup = 32;
      // Groups that can cut traced instances in one frame (role CUT).
      static constexpr uint32_t maxCutGroups = 8;
      uint32_t group = 0;  // 1..maxGroup
      uint32_t within = 0; // 0, or the group whose coverage (so far) limits these sources
      bool depthTest = false;
      bool operator==(const Mask &) const = default;
    };
    // Mask sources of one frame that are drawn together: a run of sources with
    // equal parameters. Runs are kept in an order that preserves what `within`
    // observes (see NativeResources::appendMask). Like halo sources, the draws
    // are in no other list.
    struct MaskRun {
      Mask mask;
      std::vector<Draw> draws;
    };
  } // namespace scene
} // namespace remix

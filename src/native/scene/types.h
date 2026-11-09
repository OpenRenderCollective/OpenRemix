#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
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
      // Mip 0's decoded alpha (min, max) per 4x4 block (row-major, texels
      // inside the image only), for micromap proofs. Empty with `transparent`.
      std::vector<std::array<uint8_t, 2>> alphaBlocks;
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
      std::vector<std::vector<float>> hdrMips;      // Linear RGBA32F, retained through upload.
      bool twoChannel = false;                      // BC5 stores normal X/Y; reconstruct Z when used as a normal map.
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
    // "rtx.native.capture.retainTextureData": GPU uploads keep
    // CompressedPixels::upload instead of releasing it, so USD captures
    // (usd/capture.cpp) can write the blocks. Applies to later uploads.
    inline std::atomic<bool> retainCompressedUploads{false};
    // The world's up axis (rtx.zUp: 2 Z, else 1 Y) where the host does not
    // say: USD mods are converted to it and captures declare it.
    inline std::atomic<uint32_t> sceneUpAxis{1};
    // Adds the steady-clock ticks since construction to a counter.
    struct CounterTicks {
      std::atomic<uint64_t> &out;
      std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
      ~CounterTicks() {
        out.fetch_add(
            uint64_t(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
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
    // The parts of a material a mod's partial override (an `over` of a
    // captured material, usd/loader.cpp) authors; the others keep the values
    // of the material it replaces (instances.cpp), as the game made it.
    struct Material;
    // remixapi_MaterialGradientMapEXT, validated: Left 4 Dead 2's Infected
    // composite of the material's albedo texture (read as stored). The
    // sampler materials only name how their texture is sampled (the gradient
    // bilinear and clamped, the detail and burn with the material's filter,
    // repeating): backends sample a texture through a material's sampler.
    struct GradientMap {
      // detail, burn: null skips its step.
      std::shared_ptr<Texture> gradient, detail, burn;
      std::shared_ptr<Material> gradientSampler, detailSampler, burnSampler;
      float skinRow = 0, clothRow = 0, spriteOffset[2] = {}, detailU[2] = {}, detailV[2] = {};
      float bloodColor[3] = {1, 1, 1}, bloodRange[2] = {0, 1}, burnStrength = 0;
      bool operator==(const GradientMap &o) const {
        return gradient == o.gradient && detail == o.detail && burn == o.burn && burnStrength == o.burnStrength &&
               skinRow == o.skinRow && clothRow == o.clothRow &&
               std::equal(spriteOffset, spriteOffset + 2, o.spriteOffset) &&
               std::equal(detailU, detailU + 2, o.detailU) && std::equal(detailV, detailV + 2, o.detailV) &&
               std::equal(bloodColor, bloodColor + 3, o.bloodColor) &&
               std::equal(bloodRange, bloodRange + 2, o.bloodRange);
      }
    };
    // remixapi_MaterialPortalRimEXT, validated: Portal 2's portal rim, whose
    // noise is the material's albedo texture (read as stored). The ramp is
    // read through rampSampler (bilinear, clamped).
    struct PortalRim {
      std::shared_ptr<Texture> ramp;
      std::shared_ptr<Material> rampSampler;
      float colorScale = 1;
      bool operator==(const PortalRim &o) const { return ramp == o.ramp && colorScale == o.colorScale; }
    };
    // remixapi_MaterialSolidEnergyEXT, validated: Portal 2's SolidEnergy
    // surface (a light bridge), whose base is the material's albedo texture.
    // The details are read through their samplers (bilinear, repeating).
    struct SolidEnergy {
      std::shared_ptr<Texture> detail1, detail2;
      std::shared_ptr<Material> detail1Sampler, detail2Sampler;
      float intensity = 1;
      float baseScroll[2] = {}, detail1Scroll[2] = {}, detail2Scroll[2] = {}, detail1Sway[2] = {};
      // Its glow averaged over the textures (the scrolls move them apart, so
      // over time every texel of each meets every other's), linear: the
      // light it gives per unit of the material's emission (scene/emission.cpp,
      // once per material).
      mutable std::once_flag meanOnce;
      mutable std::array<float, 3> mean{};
      bool operator==(const SolidEnergy &o) const {
        auto same = [](const float *a, const float *b) { return a[0] == b[0] && a[1] == b[1]; };
        return detail1 == o.detail1 && detail2 == o.detail2 && intensity == o.intensity &&
               same(baseScroll, o.baseScroll) && same(detail1Scroll, o.detail1Scroll) &&
               same(detail2Scroll, o.detail2Scroll) && same(detail1Sway, o.detail1Sway);
      }
    };
    // remixapi_MaterialBlendModulationEXT: the mask (a data texture: read as
    // stored) and its sampler material, and the mesh UV -> mask UV rows.
    struct BlendModulation {
      std::shared_ptr<Texture> mask;
      std::shared_ptr<Material> sampler;
      float transform[2][3] = {{1, 0, 0}, {0, 1, 0}};
      bool operator==(const BlendModulation &o) const {
        return mask == o.mask && std::equal(&transform[0][0], &transform[0][0] + 6, &o.transform[0][0]);
      }
    };
    // remixapi_InstanceInfoWoundCutEXT, validated. A description keeps the
    // texture's name (path); the draws built from it hold the texture and
    // its sampler material (bilinear, clipped: black outside 0..1).
    struct WoundCut {
      std::basic_string<std::remove_cv_t<std::remove_pointer_t<remixapi_Path>>> path;
      std::shared_ptr<Texture> texture;
      std::shared_ptr<Material> sampler;
      float ellipsoids[2][4][3] = {}; // per ellipsoid: center, axisX, axisY, axisZ
      float mappingScale = 1, secondBlend = 0, threshold = 0;
      float projectionScale[4] = {}, projectionBias[4] = {}, cutScaleBias[2][4] = {};
    };
    namespace ModParts {
      constexpr uint32_t color = 1, roughness = 2, metallic = 4, normal = 8, emission = 16, transmission = 32,
                         film = 64, alpha = 128, sampling = 256, subsurface = 512, all = ~0u;
    }
    struct Material {
      uint64_t sourceHash = 0;
      std::vector<uint64_t> replacementKeys;
      // A mod material whose textures stream (usd/submission.h): a draw that
      // resolves to it sets 1, or 2 when a scene instance keeps it. The mod
      // loader reads and clears it. Guarded by the API mutex.
      std::shared_ptr<uint8_t> streamUse;
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
      // remixapi_MaterialMetalColorEXT: the metal's colour (metalColored).
      bool metalColored = false;
      float metalColor[3] = {1, 1, 1};
      // remixapi_MaterialPbrBlurEXT: the roughness map's least mip.
      float pbrBlur = 0;
      // remixapi_MaterialGradientMapEXT (null: none); texture is then a data
      // texture (as stored).
      std::shared_ptr<const GradientMap> gradient;
      // remixapi_MaterialPortalRimEXT (null: none); texture is then its noise,
      // a data texture (as stored).
      std::shared_ptr<const PortalRim> portalRim;
      // remixapi_MaterialSolidEnergyEXT (null: none).
      std::shared_ptr<const SolidEnergy> solidEnergy;
      // remixapi_MaterialBlendModulationEXT (null: none).
      std::shared_ptr<const BlendModulation> blendModulation;
      // remixapi_MaterialNormalScrollEXT (normal encoding 5): layers, then the
      // base, layer 1 and layer 2 rates.
      uint32_t scrollLayers = 1;
      float scroll[6] = {};
      // Measured optical properties in scene units. Zero measurement distance
      // retains legacy tinted-sheet shading; positive thickness denotes a sheet.
      float refractiveIndex = 1.5f, transmittanceDistance = 0;
      float thinWallThickness = 0;       // positive: measured parallel sheet, not a containing volume
      float transmittanceScroll[2] = {}; // actual tint UV rates; bake-averaged axes ignore the clock
      uint32_t bakeTintAxes = 0;         // explicitly averaged transmission-tint UV axes; live shading ignores this
      bool bakeSmoothBoundary = false;   // explicit mean interface; live ripple normals are excluded
      // remixapi_MaterialTranslucentFogEXT: the body fades linearly over
      // transmittanceDistance (its fog end), untinted, instead of absorbing.
      bool fogBody = false;
      float refractTint[3] = {1, 1, 1}; // what passes, seen through it (fogBody)
      float fogStart = 0;               // seen through it (fogBody)
      // A thin-film coating (remixapi_MaterialIridescenceEXT, or the opaque
      // info's thinFilmThickness): strength 0 means none. Thicknesses in nm.
      struct Iridescence {
        float strength = 0, ior = 1.5f, thickness = 200, min = 100, max = 800;
        float flowAmplitude = 0, flowScale = 1, flowSpeed = .1f, structural = 0;
        bool fromAlpha = false; // albedo alpha times 1500 nm
        std::shared_ptr<Texture> thicknessTexture;
        bool operator==(const Iridescence &) const = default;
      } iridescence;
      // Subsurface scattering, RTX Remix's controls
      // (remixapi_MaterialInfoOpaqueSubsurfaceEXT): a diffusion profile, or
      // without one a thin opaque surface (measurementDistance > 0). Weight 0:
      // neither (remixapi_MaterialSubsurfaceWeightEXT).
      struct Subsurface {
        float weight = 0;
        bool diffusionProfile = false;
        float radius[3] = {.5f, .5f, .5f}, radiusScale = 1, maxSampleRadius = 1;
        float anisotropy = 0, transmittance[3] = {1, 1, 1};
        float measurementDistance = 0, singleScatteringAlbedo[3] = {.5f, .5f, .5f};
        std::shared_ptr<Texture> transmittanceTexture, thicknessTexture, singleScatteringAlbedoTexture, radiusTexture;
        bool operator==(const Subsurface &) const = default;
      } subsurface;
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
      // Light through it draws caustics: translucent, with a normal map.
      bool caustics = false;
      // remixapi_MaterialInfoScreenCameraEXT: the screen camera slot + 1 it
      // shows (0: none).
      uint8_t screenCamera = 0;
      // remixapi_MaterialInfoPortalEXT: the ray portal index + 1 its surface
      // opens (0: none; scene/portals.h).
      uint8_t rayPortal = 0;
      // Wrap modes in RTX Remix's MDL encoding: 0 clamp, 1 repeat, 2 mirrored
      // repeat, 3 clip.
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
      // A metal colour (remixapi_MaterialMetalColorEXT), as unorm8 RGB in
      // pbrWord's low three bytes; a blur (remixapi_MaterialPbrBlurEXT) of
      // the roughness map, in 1/16 mips in its high byte.
      static constexpr uint32_t metalColor = 256, pbrBlur = 512;
      // remixapi_MaterialGradientMapEXT (frame_tables.h materialFields: its
      // parameters take the thin film's fields, which such a material lacks).
      static constexpr uint32_t gradientMap = 1024;
      // remixapi_MaterialPortalRimEXT (frame_tables.h portalRimFields: its
      // ramp and colour scale in the film's fields, which it lacks too).
      static constexpr uint32_t portalRim = 2048;
      // remixapi_MaterialSolidEnergyEXT (frame_tables.h solidEnergyFields:
      // its details and rates in the film's fields, likewise).
      static constexpr uint32_t solidEnergy = 4096;
      // remixapi_MaterialBlendModulationEXT (frame_tables.h
      // blendModulationFields: the mask and its transform in the film's
      // fields, which such a material lacks as well).
      static constexpr uint32_t blendModulation = 8192;
      // A fog body's refraction tint and fog start
      // (remixapi_MaterialTranslucentFogEXT) in the film's fields, which such
      // a material lacks (frame_tables.h).
      static constexpr uint32_t refractTint = 16384;
      uint32_t flags = 0, pbrWord = 0;
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
      uint32_t style = 0;    // captured stationary style; excluded from shared indirect transport
      uint32_t bakeHint = 0; // optional engine-agnostic static representation preference
      // Triangle emitters: the surface draw they belong to (0 for sphere
      // lights) and their place in a spatial order of its emitters. The light
      // tree keeps a surface's emitters together in that order and makes
      // leaves of runs of them (LightBvh::setClusterSize).
      uint64_t group = 0;
      uint32_t rank = 0;
      float forwardCutoff = 0; // explicit Forward+ cutoff, in scene units
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
      uint64_t reflectionMaterial = 0; // original receiver material key, independent of shading replacements
      std::shared_ptr<Mesh> mesh;
      uint64_t instanceId = 0;
      uint32_t instanceIndex = 0, surfaceIndex = 0;
      uint32_t classification = 1; // dynamic for callers without the extension
      bool rasterVisible = true;
      bool traced() const {
        return !ui && camera == REMIXAPI_CAMERA_TYPE_WORLD && classification != 2 && blend.mask != 0 && !maskGroup;
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
      bool terrain = false; // existing TERRAIN category; enables lit static paint in Forward+
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
      // remixapi_InstanceInfoWoundCutEXT (null: none): cut away for every ray
      // and raster pass, evaluated at the mesh's own (bind pose) positions.
      std::shared_ptr<const WoundCut> wound;
      // remixapi_InstanceInfoRasterOrderEXT: the host's place for a blended
      // raster-only draw among its layer's (0: sorted by depth).
      uint32_t rasterOrder = 0;
      // remixapi_InstanceInfoSpriteEXT (0: none), on blended raster-only draws.
      uint32_t spriteFlags = 0;
      float spriteSoft[3] = {}; // softDistance, softFadeStart, softFadeEnd
      // remixapi_InstanceInfoTexcoordTransformEXT: scale, offset; velocity in
      // UV units per second (surfaceTexcoord).
      std::array<float, 4> texcoord{1, 1, 0, 0};
      std::array<float, 2> texcoordVelocity{};
      bool scrolling() const { return texcoordVelocity[0] != 0 || texcoordVelocity[1] != 0; }
      // An offset or a scroll: what the texels under a triangle are may change
      // from frame to frame (a host may give a new offset each frame), so
      // per-offset caches (opacity, emitters) take the draw as at rest.
      bool texcoordMoves() const { return scrolling() || texcoord[2] != 0 || texcoord[3] != 0; }
    };
    // The UV transform (scale, offset) a draw's maps are read through at
    // `seconds`: the instance's texcoord transform, scrolled, and then the
    // material's sprite sheet cell (animationFrame). The scroll is kept to
    // less than two repeats, which neither wrapping nor mirroring shows;
    // scroll false: at rest, without the instance's offset or scroll (the
    // emitters of a draw whose texcoords move, SurfaceEmissionCache).
    std::array<float, 4> surfaceTexcoord(const Draw &draw, double seconds, uint32_t &animationFrame,
                                         bool scroll = true);
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

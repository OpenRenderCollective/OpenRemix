#pragma once
#include "upscale/options.h"
#include <cstdint>

namespace remix {
  struct TraceOptions {
    uint32_t samples = 1, bounces = 16;
    bool russianRoulette = true;
    uint32_t rouletteMinBounces = 1;
    bool accumulate = false, forceRebuild = false;
    bool denoiser = true, taa = true, validation = false;
    upscale::Mode upscaler = upscale::Mode::Dlss;
    bool dlssRayReconstruction = true;
    // Ray Reconstruction's model: 0 transformer (DLSS 310.9.1, preset E), 1 CNN
    // (DLSS 310.2.1, preset C; about half the cost on Turing). With the
    // override, dlssRayReconstructionPreset (NGX's 1-6, A-F) instead; a preset
    // of the other model's library is replaced by the model's own.
    uint32_t dlssRayReconstructionModel = 0;
    bool dlssRayReconstructionPresetOverride = false;
    uint32_t dlssRayReconstructionPreset = 5;
    uint32_t dlssQuality = 3;
    bool autoExposure = true, bloom = true;
    float exposure = .98f, autoExposureTarget = .2f, autoExposureSpeed = 2;
    float autoExposureCentre = .75f, autoExposureMinEv = -16.3f, autoExposureMaxEv = 3.1f;
    float bloomIntensity = .01f, bloomRadius = .85f;
    // Per-channel contrast before the tonemapper, pivoting on middle grey:
    // c = (c / .18)^contrastAmount * .18 after exposure.
    bool contrast = false;
    float contrastAmount = 1.15f;
    uint32_t fsrQuality = 3;
    float fsrSharpness = 0.f;
    bool sigma = true;
    bool blueNoiseShadows = true;
    bool blueNoiseLocalLights = true;
    // Blue-noise directions for a path's first bounce and its sky rays
    // (skySamples): neighbouring pixels and frames probe the hemisphere
    // evenly, so the denoiser averages spread samples instead of clumps.
    bool blueNoiseSky = true;
    bool localLights = true, localDenoiser = true;
    bool torchLights = true, emissiveLights = true;
    // Emissive triangles of one surface a light-tree leaf holds at most (1: a
    // leaf per triangle). Sampling picks a leaf, then one of its triangles by
    // power: each keeps its own shape and color, and the tree to build and
    // descend is that many times smaller.
    uint32_t emissiveClusterSize = 16;
    uint32_t emissiveCandidates = 1;
    // RIS candidates in sets of more than four sphere lights without emissive
    // surfaces (1: plain BVH sampling). Candidates compare unshadowed
    // contributions, cone shaping included, before the one shadow ray.
    uint32_t sphereCandidates = 1;
    bool adaptiveLocalSampling = false;
    bool stabilizeEmissiveLighting = false, emissiveSolidAngleSampling = false;
    bool freezeEmissiveRadiance = false; // diagnostic: casting only
    uint32_t separateLocalLighting = 3;  // combined, shared rays, per-family rays, combined separate pass
    uint32_t localSamples = 1;
    float localIntensity = 1, localRange = 16;
    float localDirectClamp = 0, indirectClamp = 0;
    bool localFireflyFilter = true;
    bool localGuiding = true;
    // Visibility-guiding cell size in meters (see sceneScale).
    float localGuideCellSize = 2;
    // Local-light (torch) REBLUR: anti-lag variance scale and sensitivity
    // (NRD ReblurAntilagSettings; lower values react sooner) and history cap.
    float localAntilagSigma = 2, localAntilagSensitivity = 3;
    uint32_t localHistoryFrames = 16;
    // Drop local-light history where moved lights or moved occluders (a closing
    // door; ReSTIR history samples re-tested) changed the light.
    bool localHistoryConfidence = true;
    // Where a moving object's shadow just arrived or left (the denoiser
    // restarted the pixel), the share of the light distance the local
    // denoiser is told, rising to all of it over eight frames: lower keeps
    // the moving shadow sharper and grainier, higher smoother and paler.
    float movingShadowBlur = .125f;
    // Trace an indirect continuation path from each transparent sheet (glass,
    // blended surfaces). Off: a non-PBR sheet takes the denoised indirect light
    // of the surface behind it (much cheaper; transparent layers cost about a
    // third). PBR sheets (water) always trace: their reflections need it.
    bool translucentIndirect = true;
    // Shadow, light-visibility and bounce rays ignore blended layers (glass,
    // sprites): no transmission through them and no candidate alpha tests.
    // Camera and transparency rays still see them.
    bool skipBlendedSecondaryRays = true;
    // View models (REMIXAPI_CAMERA_TYPE_VIEW_MODEL instances): path-traced in
    // the world camera's view instead of rasterized with their own camera
    // (NativeResources::applyViewModel). The camera sees them in front of
    // everything; every other ray but those of paths that start on them
    // passes them (no shadows on the world, the player's light through them,
    // not in mirrors). viewModelScale shrinks them toward the eye (the same
    // image, nearer) so their own rays start inside the player's space and
    // not in a wall they reach into.
    bool viewModel = true;
    float viewModelScale = .5f;
    // Primary surface replacement: with raster primary visibility and a
    // denoiser, the surface a mirror (smooth metal or smooth black) reflects
    // replaces it as the pixel's primary surface, so it gets primary-quality
    // lighting and denoising at its mirrored position.
    bool primarySurfaceReplacement = true;
    // Without traced sheet indirect: PBR sheets (water) also sample local
    // lights at their own surface. Off: sun and sky only there (local light
    // still reaches them through their reflection paths).
    bool sheetLocalLights = true;
    // Layers behind the front translucent layer of a pixel take its light
    // (per unit material factor) instead of their own light samples and
    // paths: one lit layer per pixel, however many it passes. Off: every
    // layer is lit (PBR layers, or all with translucentIndirect).
    bool layerReuse = true;
    // A translucent layer's paths end at their first hit: the radiance
    // cache's light there (whatever the path's width), else the hit's direct
    // light. Off: they continue like an opaque surface's paths.
    bool shortSheetPaths = true;
    // Exclude local lights outside the shading point's potentially visible
    // clusters (remixapi_SetVisibilityPartitionEXT), when a partition is set.
    // ReSTIR DI for primary local lighting (implies separateLocalLighting 3):
    // per-pixel reservoirs of light samples, resampled from the light tree,
    // reused across frames and neighbors, occluded samples discarded.
    bool restir = false;
    // Presampled light pools per world cell (ReGIR): bounce vertices, the
    // SHARC update and ReSTIR's initial candidates take a light from their
    // cell's pool instead of descending the light tree per sample.
    bool regir = true;
    float regirCellSize = 1.05f; // meters (see sceneScale)
    // ORCA: a radiance cache rebuilt every frame from sparse full paths (one
    // per orcaTile x orcaTile pixels); other pixels read their first bounce
    // from it. No history: indirect light follows lighting changes at once.
    // Sparse rendering (after dxvk-remix's rtx.sparseRendering): each frame
    // lights only a blue-noise selection of the pixels, at 1 / rate times
    // their radiance, and DLSS Ray Reconstruction rebuilds the rest; guides,
    // emission, sky and transparent layers stay dense. Needs DLSS Ray
    // Reconstruction; off with ReSTIR, a separate local-lighting pass,
    // accumulation and debug views.
    // The rate (selected share of the pixels) is sparseSamplingRate as is,
    // or with sparseAdaptiveRate that rate at the reference render resolution
    // (2227x1238, 4K output with DLSS Auto) scaled by (reference pixels /
    // render pixels) ^ sparseResolutionExponent: lower render resolutions
    // leave Ray Reconstruction less to work with and take a higher rate.
    // sparseQuality then moves the scaled rate, in log space, towards 1/16
    // (0) or 1 (1); 0.5 leaves it. Above sparseMaxRate sparse rendering is
    // off: the gain no longer pays for the loss.
    bool sparseRendering = false, sparseAdaptiveRate = true;
    float sparseSamplingRate = .1f, sparseResolutionExponent = .91f;
    float sparseQuality = .3f, sparseMaxRate = .75f;
    // Volumetrics (volume.cpp, volume.glsl): a participating medium lit by
    // every light. Its density is the game's fog (remixapi_SetFogState), or
    // with volumetricOverride the one given here: volumetricDistance is how
    // far one sees through it (meters; 5% of the light is left there) and
    // volumetricColor what it scatters. volumetricHeightFalloff > 0 thins
    // either above volumetricBaseHeight along volumetricUpAxis (0 X, 1 Y,
    // 2 Z, 3 Auto: Y or Z from the camera; the NUMOS sky follows it too):
    // the height, in meters, over which the density falls to 37%.
    // volumetricScattering scales the scattered light; the medium fills at
    // most the first volumetricMaxDistance meters of a camera ray.
    bool volumetrics = false, volumetricOverride = true;
    float volumetricDistance = 2000, volumetricColor[3] = {1, 1, 1};
    float volumetricHeightFalloff = 0, volumetricBaseHeight = 0;
    uint32_t volumetricUpAxis = 3;
    float volumetricScattering = 1, volumetricMaxDistance = 250;
    // Each source's share of the scattered light, on top of a light's own
    // volumetricRadianceScale: the sun, lights (sphere, rect, disk, cylinder),
    // emissive surfaces, the sky.
    float volumetricSunScale = 1, volumetricLightScale = 1, volumetricEmissiveScale = 1, volumetricSkyScale = 1;
    // The scattered light is held in volumetricLevels nested grids of
    // volumetricCells^3 cells around the camera, the first of
    // volumetricCellSize meters and each next one twice as coarse. A cell is
    // relit every volumetricUpdateInterval frames and keeps the mean of its
    // last volumetricHistory samples; a camera ray takes volumetricSteps
    // samples of the grids.
    float volumetricCellSize = .2f;
    uint32_t volumetricCells = 64, volumetricLevels = 4;
    uint32_t volumetricUpdateInterval = 2, volumetricHistory = 4, volumetricSteps = 20;
    bool orca = false;
    uint32_t orcaTile = 4;
    float orcaCellSize = .0625f; // smallest voxel, meters; levels grow by 4x
    uint32_t orcaLevels = 6, orcaMinSamples = 4;
    // A voxel whose mean has a larger relative standard error than this is
    // passed over for the next larger one (then SHARC, then tracing on):
    // a few disagreeing samples flash as a patch. 0 accepts any voxel.
    float orcaMaxError = .3f;
    // Denoiser history of surfaces that move on their own (doors, characters,
    // the view model; camera motion excluded). With movingGradients the
    // indirect gradient pass follows such a surface along its motion vector
    // and estimates how much its light changes per frame, relative to its
    // level:
    // - local lights: the unshadowed light of every local light at the
    //   surface point's two poses (position and normal), exact up to shadows;
    // - indirect light: the pose change itself, displacement over
    //   movingLightScale (meters, see sceneScale: the distance over which
    //   indirect light changes completely) plus half the normal's rotation in
    //   radians; and the measured difference to last frame's sample (retraced
    //   with the same random sequence) where it exceeds movingGradientNoise.
    // History is kept as far as that change allows: confidence
    // 1 - change / movingLightTolerance, which leaves 1 / (1 - confidence)
    // frames, so the tolerance is about the relative lag error accepted on a
    // moving surface (lower: less lag, more noise; higher: the reverse).
    // On top of that a blanket rule can lower history with the surface's own
    // speed on screen: confidence falls from 1 at movingHistoryStart to the
    // values below at movingHistoryEnd (pixels per frame). 1 keeps the
    // history, 0 drops it: movingIndirectHistory for the indirect passes on
    // the moving surface, movingNeighborHistory for static surfaces within 16
    // pixels of it at its depth (its contact shadow), movingLocalHistory for
    // the local-light passes.
    bool movingGradients = true;
    float movingLightTolerance = .2f, movingLightScale = 3, movingGradientNoise = .5f;
    float movingHistoryStart = .5f, movingHistoryEnd = 3;
    float movingIndirectHistory = 1, movingNeighborHistory = 0, movingLocalHistory = 1;
    // Native TAA: weight of the current frame where the reprojected history
    // falls between texels (anything moving on screen). Resampling costs
    // detail every frame; a higher rate keeps moving surfaces sharper and
    // leaves fewer frames to anti-alias them (0.05 is the rate at rest).
    float taaMotionRate = .15f;
    // Indirect-light denoiser history in frames, 1-63 (0 = NRD's default,
    // 0.5 s of frames). Longer averages away more of the sparse bounce light
    // and follows lighting changes more slowly.
    uint32_t indirectHistoryFrames = 0;
    // Sky visibility rays at a path's first surface (1-16), each in its own
    // direction from the path's lobe. Indoors only a narrow set of directions
    // reaches an opening: one ray finds it rarely and brightly, which boils
    // after denoising. Each ray is a shadow query; the bounce path is unchanged.
    uint32_t skySamples = 2;
    // Sky rays at a translucent layer's surface (glass, water), at most
    // skySamples: each lit layer casts them on top of the surface behind it.
    uint32_t sheetSkySamples = 2;
    // Sky guiding: each world cell (skyGuideCellSize meters over sceneScale)
    // learns which directions its sky rays found the sky in, and the extra
    // sky rays (skySamples above 1) sample those directions with share
    // skyGuideShare, the rest by the BRDF. Diffuse and legacy surfaces only.
    bool skyGuiding = true;
    float skyGuideCellSize = 2, skyGuideShare = .5f;
    uint32_t restirCandidates = 32;     // initial light-tree candidates per pixel
    uint32_t restirSpatialSamples = 8; // neighbor reservoirs per pixel
    float restirSpatialRadius = 30;    // pixels
    // Temporal M cap in multiples of the current M. Longer histories correlate
    // the signal over time, which NRD cannot filter (frozen noise); dxvk-remix
    // uses 4.
    uint32_t restirTemporalHistory = 64;
    // RTXDI permutation sampling: the temporal lookup takes a per-frame
    // shuffled neighbor (2x2 block), decorrelating reused samples.
    bool restirPermutation = true;
    bool restirInitialVisibility = true; // shadow-test the initial winner before reuse
    bool lightVisibilityCulling = true; // opt-in: ~3 ms on L4D2 c1m1, no gain where the PVS is broad
    // remixapi_InstanceInfoCameraCullingEXT: honour the instances' flags.
    // Camera rays only; lighting is the same either way.
    bool cameraBackFaceCulling = true, cameraVisibilityCulling = true;
    // Meters per world unit. Scales meter-based defaults: denoiser hit
    // distances and guiding cells (Source engine inches: 0.0254).
    float sceneScale = 1;
    uint32_t shadowDebug = 0;
    uint32_t debugView = 0;        // display diagnostics; never selects an integrator
    float sunAngularDiameter = -1; // -1 inherits the distant light's diameter.
    bool rasterPrimary = true;
    bool waterIndirectReuse = true;
    // Caustics: light through translucent surfaces with normal maps (water,
    // bumpy glass) focused by their waves or bumps. Strength scales the
    // bending (.4: as the material bends it); contrast raises the result to a
    // power (past 1, brighter lines and darker gaps).
    bool caustics = true;
    float causticsStrength = .4f, causticsContrast = 1;
    // Caustics fade in over this depth (meters, see sceneScale) behind the
    // surface: shallow water and thin glass have none, as light has not yet
    // been focused. 0: at once.
    float causticsDepth = .5f;
    // Thin-film iridescence of materials that have it (remixapi_MaterialIridescenceEXT).
    bool iridescence = true;
    bool translucencyDenoiser = true, translucencyReuse = true;
    bool pbrLocalReuse = true; // Experimental previous-frame secondary local lighting.
    bool sky = true;
    bool sharc = true, sharcDebug = false;
    uint32_t sharcCapacityLog2 = 22, sharcDownscale = 5, sharcSceneScale = 15;
    uint32_t sharcAccumulationFrames = 32, sharcStaleFrames = 64;
    // The accumulation window while dynamic geometry moves (shadows cast into
    // the cache follow it); lighting changes use a shorter one of their own.
    uint32_t sharcMotionFrames = 10;
    // An update sample is limited to this multiple of what its cell already
    // holds (0: unlimited), as dxvk-remix's fireflyClampMultiplier.
    float sharcFireflyClamp = 10;
    // The most update paths a frame traces (0: no limit): above it the update
    // grid coarsens past sharcDownscale. The default is 1920x1080 at 5.
    uint32_t sharcMaxUpdatePaths = 82944;
    // Moving surfaces hold cache entries too, at the world position they are at.
    bool sharcDynamicSurfaces = true;
    // PBR surfaces smoother than REMIX_SHARC_PBR_MIN_ROUGHNESS hold cache
    // entries as if they were that rough, where otherwise paths trace on.
    bool sharcGlossySurfaces = true;
    // A path reads the cache only where its spread at the hit covers a cell
    // (footprint > cell diagonal). Off: every cacheable vertex reads it,
    // ~1.5 ms faster on df_range, but coarse far cells read too dark.
    bool sharcFootprintTest = true;
    uint64_t sharcReset = 0; // Runtime action sequence; excluded from profiles.
    bool overrideLight = false;
    float lightDirection[3] = {-0.4f, -0.8f, -0.3f};
    float lightColor[3] = {1, 1, 1};
    float lightIntensity = 3.14159265f;
    // GPU optimizations, each output-identical when on (docs in
    // ui/rendering.cpp). All apply at runtime without a restart.
    // One-sided surfaces become hardware-opaque in their BLAS and one-sided
    // instances reject back faces in traversal instead of in the shader.
    bool hardwareFaceCulling = true;
    // One TLAS over the static and dynamic instances: one traversal per ray.
    bool combinedTlas = true;
    // Boolean shadow and local-light rays stop at their first blocker.
    bool shadowEarlyOut = true;
    // DLSS 5 Neural Uplift (DLSS-NR)
    bool neuralUplift = false;
    uint32_t neuralUpliftPasses = 1;
    uint32_t neuralUpliftPreset = 0;
    uint32_t neuralUpliftStyle = 0;
    float neuralUpliftIntensity = 1.0f;
    float neuralUpliftStyleStrength = 1.0f;
    float neuralUpliftLocalStructureStrength = 1.0f;
    float neuralUpliftSkinStructureStrength = -1.0f;
    bool neuralUpliftAutoMask = true;
    bool neuralUpliftDepthInverted = false;
    float neuralUpliftMotionVectorScale = 1.0f;
    uint32_t neuralUpliftFeatureId = 18;
    bool neuralUpliftBypassCallerCheck = true;
    bool operator==(const TraceOptions &) const = default;
  };
  // Options as the tracer applies them: ReSTIR DI runs in the combined
  // separate local-lighting pass.
  inline TraceOptions normalized(TraceOptions options) {
    if (options.restir)
      options.separateLocalLighting = 3;
    return options;
  }
} // namespace remix

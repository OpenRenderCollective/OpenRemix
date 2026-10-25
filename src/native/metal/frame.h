#pragma once
// The shaders' std140 constants the Metal backend writes: the path tracing
// uniform (scene.glsl Frame; pathtrace/gpu.h Frame) and the temporal passes'
// image slots and constants (inputs.glsl; denoise/renderer.h). Copies of the
// Vulkan renderer's definitions, kept beside the Metal backend: their sizes
// are checked against the translated shaders' reflection (shaders/embedded.h
// layout constants) so a field added upstream fails the build here.
#include "metal/shaders/embedded.h"
#include "scene/types.h"
#include <cstddef>
#include <cstdint>

namespace remix::metal::trace {
  using scene::Matrix;
  struct Frame {
    Matrix inverseCamera, camera;
    float eye[4] = {}, direction[4] = {}, radiance[4] = {};
    uint32_t config[4] = {}, target[4] = {};
    float jitter[4] = {};      // primary offset; z water reuse, w blue-noise sun/local bits
    float skyRadiance[4] = {}; // linear uniform environment; w reserved
    uint64_t sharcHash = 0, sharcAccumulation = 0, sharcResolved = 0, sharcCounters = 0;
    uint32_t sharcConfig[4] = {};
    float sharcGrid[4] = {}, sharcPreviousCamera[4] = {};
    float sun[4] = {}; // tangent of angular radius, SIGMA active, debug view, raw local-distance diagnostic
    uint64_t lightNodes = 0, lightEmitters = 0;
    uint32_t localConfig[4] = {};
    float localScale[4] = {};
    uint64_t lightGuideRead = 0, lightGuideWrite = 0;
    uint32_t lightGuideConfig[4] = {}; // capacity, node count, inverse cell size (float bits), reserved
    uint64_t torchNodes = 0, torchEmitters = 0, emissiveNodes = 0, emissiveEmitters = 0;
    uint32_t splitLocal[4] = {}; // mode, torch count, emissive count, debug view
    uint64_t reuseRead = 0, reuseWrite = 0;
    uint32_t reuseConfig[4] = {}; // previous frame valid, diagnostics, ray counter address low/high
    uint64_t indirectResults = 0, blueNoise = 0;
    float firefly[4] = {};       // outgoing local direct / indirect luminance limits; reserved
    uint32_t pathConfig[4] = {}; // adaptive termination, minimum completed bounces, sheet flags (1 traced indirect, 2 local lights), secondary ray mask
    float domeTransform[3][4] = {};
    uint64_t visibilityNodes = 0, visibilityLeaves = 0, visibilityTable = 0;
    uint64_t movedLights = 0; // scene::MovedLight pairs, count in visibilityConfig[1]
    uint32_t visibilityConfig[4] = {}; // table row words, moved light count, light-tree nodes in the table, reserved
    float visibilityProbe[4] = {};     // world units: probe offset above the surface, decal attachment distance; texture footprint scale (render/output size)
    uint64_t restirCurrent = 0, restirHistoryRead = 0, restirHistoryWrite = 0, restirRemap = 0;
    uint32_t restirConfig[4] = {}; // candidates, spatial samples, temporal M cap, flags (1 initial visibility)
    float restirParams[4] = {};    // spatial radius (pixels), emitter count, reserved
    uint64_t regirRead = 0, regirWrite = 0, regirSlots = 0;
    uint64_t caustics = 0; // pathtrace/gpu.h: strength, contrast, half float focus depth; 0 off
    uint32_t regirConfig[4] = {}; // cell capacity, slots per cell, cells per world unit (float bits), active
    uint64_t orcaTable = 0, orcaReserved = 0;
    uint32_t orcaConfig[4] = {}; // entry capacity, sparse tile size, voxel levels, active
    float orcaParams[4] = {};    // smallest voxel (world units), level growth, minimum samples, maximum relative error
    // remixapi_InstanceInfoMaskEXT CUT. cutConfig: first coverage texture + 1
    // (0: nothing cuts), coverage textures. cutSlots: four bits per mask
    // group (1..32), its coverage texture (from the first) + 1 or 0.
    uint32_t cutConfig[4] = {}, cutSlots[4] = {};
    // remixapi_InstanceInfoCameraCullingEXT. cameraNodes: the visibility
    // partition's nodes; cameraLeaves: a bit per leaf, set where the camera
    // sees the leaf (both zero unless visibility culling is active).
    // cameraCull: active flags (1 back faces, 2 visibility), the distance
    // before a surface at which a camera ray is located (float bits).
    uint64_t cameraNodes = 0, cameraLeaves = 0;
    uint32_t cameraCull[4] = {};
    // Sparse rendering (sparse.cpp): the selected pixels, their count and
    // indirect dispatch. sparseConfig: active, threshold levels passed of 256.
    uint64_t sparseList = 0, sparseCount = 0;
    uint32_t sparseConfig[4] = {};
    // Volumetrics (volume.cpp, volume.glsl). volumeConfig: active, cells per
    // side, levels, update interval. volumeFrame: update counter, history
    // samples, march steps, reset. volumeParams: smallest cell (world units),
    // march distance, scattered light scale, base height. volumeMedium:
    // scattering albedo, extinction per world unit. volumeHeight: up axis,
    // density falloff per world unit. volumeEye/Previous: the windows' centre.
    uint64_t volumeGrid = 0;
    // Seconds of the runtime's clock (wrapped), for scrolling maps; the world's
    // up axis (0 x, 1 y, 2 z: rtx.native.pathtrace.volumetricUpAxis).
    float animationTime = 0, upAxis = 2;
    uint32_t volumeConfig[4] = {}, volumeFrame[4] = {};
    float volumeParams[4] = {}, volumeMedium[4] = {}, volumeHeight[4] = {};
    float volumeEye[4] = {}, volumePrevious[4] = {};
    // remixapi_CameraMediumEXT: transmittance colour, measurement distance (0: in air).
    float cameraMedium[4] = {};
    uint64_t sheetResults = 0; // SheetResults: the sheet lighting pass's transport
    // Sky guiding (sky_guiding.glsl): each world cell's learned sky directions,
    // last frame's (read, 0: none yet) and this frame's. skyGuideConfig: slot
    // capacity, cells per world unit (float bits), guided share (float bits), active.
    uint64_t skyGuideRead = 0;
    uint64_t skyGuideWrite = 0, skyGuideReserved = 0;
    uint32_t skyGuideConfig[4] = {};
    // Camera rays (pathtrace.comp ray()): inverseCamera relative to the eye,
    // which the shader adds back. In single precision the absolute matrix
    // holds the eye's translation scaled by the inverse near plane, and loses
    // the ray directions to rounding far from the origin (near geometry such
    // as a view model jitters as the camera moves).
    Matrix inverseCameraRelative{};
    // Local occlusion probes (pathtrace.comp, localOcclusionConfidence): per
    // pixel, the arrived share of its local light, last frame's (0: none)
    // and this frame's. localProbeParams: movingShadowBlur, reserved.
    uint64_t localProbeRead = 0, localProbeWrite = 0;
    float localProbeParams[4] = {};
    // Volumetrics: each source's share of the scattered light (sun, lights,
    // emissive surfaces, sky; the volumetric*Scale options).
    float volumeLightScales[4] = {1, 1, 1, 1};
    // The NUMOS sky's cloud shadow (unused here: zero turns it off).
    float cloudShadow[3][4] = {};
    uint32_t cloudShadowConfig[4] = {};
  };
  static_assert(sizeof(Frame) == 1280 && offsetof(Frame, localProbeRead) == 1168 &&
                offsetof(Frame, localProbeParams) == 1184 && offsetof(Frame, volumeLightScales) == 1200 &&
                offsetof(Frame, cloudShadow) == 1216 && offsetof(Frame, cloudShadowConfig) == 1264 &&
                offsetof(Frame, inverseCameraRelative) == 1104 &&
                offsetof(Frame, skyGuideRead) == 1064 && offsetof(Frame, skyGuideConfig) == 1088 && offsetof(Frame, volumeGrid) == 912 && offsetof(Frame, volumePrevious) == 1024 && offsetof(Frame, sparseList) == 880 && offsetof(Frame, sparseConfig) == 896 && offsetof(Frame, cameraCull) == 864 && offsetof(Frame, cutConfig) == 816 && offsetof(Frame, orcaParams) == 800 && offsetof(Frame, regirConfig) == 752 && offsetof(Frame, restirCurrent) == 656 && offsetof(Frame, visibilityNodes) == 592 && offsetof(Frame, pathConfig) == 528 &&
                offsetof(Frame, firefly) == 512 && offsetof(Frame, indirectResults) == 496 &&
                offsetof(Frame, lightGuideRead) == 384 && offsetof(Frame, lightGuideConfig) == 400 &&
                offsetof(Frame, lightNodes) == 336 && offsetof(Frame, localConfig) == 352 &&
                offsetof(Frame, localScale) == 368 && offsetof(Frame, skyRadiance) == 224 &&
                offsetof(Frame, sharcHash) == 240 && offsetof(Frame, sharcConfig) == 272 &&
                offsetof(Frame, sharcGrid) == 288 && offsetof(Frame, sharcPreviousCamera) == 304 &&
                offsetof(Frame, sun) == 320);
  static_assert(sizeof(Frame) == shaders::traceFrameBytes, "scene.glsl Frame changed: update metal/frame.h");
} // namespace remix::metal::trace

namespace remix::metal::denoise {
  enum ImageSlot : uint32_t {
    Sh0,
    Sh1,
    Normal,
    Depth,
    Motion,
    Albedo,
    Direct,
    Transparent,
    Transmission,
    Out0,
    Out1,
    Confidence,
    GradientA,
    GradientB,
    History0,
    History1,
    PreviousDepth,
    PreviousNormal,
    PreviousLighting,
    Lighting,
    Composed,
    Validation,
    BaseImageCount,
    Penumbra = BaseImageCount,
    ShadowInput,
    ShadowOutput,
    SigmaImageCount,
    LocalInput = SigmaImageCount,
    LocalOutput,
    LocalImageCount,
    EmissiveInput = LocalImageCount,
    EmissiveOutput,
    EmissiveImageCount,
    PbrSpecularInput = EmissiveImageCount,
    PbrDiffuseOutput,
    PbrSpecularOutput,
    PbrSpecularFactor,
    RrDiffuse, RrSpecular, RrNormal, RrHitDistance,
    FrontNormal, FrontDepth, FrontMotion, FrontAlbedo, FrontSpecularFactor,
    FrontDiffuse, FrontSpecular, FrontDiffuseOutput, FrontSpecularOutput, FrontConfidence,
    LocalConfidence, // local-light history confidence (lights that moved)
    PsrMirror,       // primary surface replacement: mirror plane and reflectance
    // SH directions of the PBR pass (REBLUR_DIFFUSE_SPECULAR_SH). Its SH0
    // images are Sh0, PbrSpecularInput, PbrDiffuseOutput and PbrSpecularOutput.
    PbrSpecularSh1, PbrDiffuseOutputSh1, PbrSpecularOutputSh1,
    // Gradient tiles of surfaces that move on their own (see inputs.glsl).
    MovingA, MovingB,
    ImageCount
  };
  static_assert(BaseImageCount == 22 && PbrSpecularInput == 29 && FrontNormal == 37 && PbrSpecularSh1 == 49 &&
                    MovingA == 52 && ImageCount == 54,
                "Update the matching inputs.glsl bindings when changing image slots");
  // Shared std140 constants. Matrices are nonjittered, in Vulkan framebuffer coordinates.
  struct Frame {
    scene::Matrix view{}, previousView{}, previousCamera{}, previousInverseCamera{}, inverseCamera{};
    float jitter[4] = {}; // current xy, previous zw, pixels
    float hitDistance[4] = {3.f, .1f, 20.f, 0};
    uint32_t flags[4] = {};  // reset, NRD, TAA, history parity
    uint32_t target[4] = {}; // width, height, previous seed, samples
    float timing[4] = {};    // frame milliseconds, validation, world changed, reserved
    float sun[4] = {};       // direction toward sun, SIGMA active
    uint32_t shadow[4] = {}; // SIGMA reset, debug view, PBR receivers, legacy receivers
    uint32_t local[4] = {}; // denoise, active lights, responsive frames, reserved
    // foreground NRD, opaque lighting reuse, PSR mirrors, PBR lobe selection (set by Renderer::begin)
    uint32_t translucency[4] = {};
    uint32_t families[4] = {}; // torch/emissive counts, torch/emissive response frames
    // History of surfaces that move on their own (TraceOptions::moving*):
    // ramp start and end in pixels per frame, then the confidence kept at the
    // ramp's end by the indirect passes on the moving surface itself, and by
    // the local-light passes.
    float motion[4] = {.5f, 3.f, 1, 1};
    // The same for static surfaces next to a moving one (indirect passes);
    // the gradient pass follows moving surfaces (0/1); the relative change of
    // their light per frame at which their history is dropped; the inverse
    // of the lighting's length scale in world units.
    float motionGradient[4] = {0, 1, .2f, 1 / 3.f};
    // A measured relative indirect change below this is sampling noise.
    // y: native TAA's current-frame weight where history is resampled half a texel off.
    float motionNoise[4] = {.5f, .15f, 0, 0};
  };
  static_assert(sizeof(Frame) == 528 && offsetof(Frame, families) == 464 && offsetof(Frame, local) == 432 && offsetof(Frame, sun) == 400 && offsetof(Frame, shadow) == 416);
  // Local-light pass tuning (see trace::Options::localAntilag*).
  struct LocalTuning {
    float antilagSigma = 2, antilagSensitivity = 3;
    uint32_t historyFrames = 16;
    uint32_t indirectHistoryFrames = 0; // indirect REBLUR history cap; 0 = NRD's default
  };
  static_assert(sizeof(Frame) == shaders::denoiseFrameBytes, "inputs.glsl Frame changed: update metal/frame.h");
} // namespace remix::metal::denoise

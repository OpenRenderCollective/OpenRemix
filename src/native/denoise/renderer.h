#pragma once
#include "scene/types.h"
#include "upscale/renderer.h"
#include <remix/remix_native_ext.h>
#include <vulkan/vulkan.h>
#include <memory>
#include "platform/gpu_frames.h"

namespace remix::denoise {
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
    uint32_t indirectHistoryFrames = 0; // indirect REBLUR history (1-63); 0 = NRD's default
  };
  remixapi_DenoisingCapabilitiesEXT capabilities(VkPhysicalDevice physical);
  bool sigmaSupported(VkPhysicalDevice physical);
  bool localSupported(VkPhysicalDevice physical, bool sigma, bool split = false);
  class Renderer {
  public:
    Renderer();
    ~Renderer();
    void initialize(VkPhysicalDevice physical, VkDevice device,
                    VkExtent2D extent, bool reblur, uint32_t queueFamily,
                    bool sigma = false, bool local = false,
                    bool localDenoise = false, bool split = false,
                    bool rayReconstruction = false, bool validation = false,
                    bool localReconstruction = false, bool foregroundDenoise = false);
    VkDescriptorSetLayout inputLayout() const;
    VkDescriptorSet inputs() const;
    void selectFrame(uint32_t slot);
    upscale::Input image(ImageSlot slot) const;
    void begin(VkCommandBuffer command, const Frame &frame, const scene::Matrix &projection,
               const scene::Matrix &previousProjection, VkImageView world, VkImageView sky, bool localFireflyFilter = false);
    void beginConfidence(VkCommandBuffer command);
    void setLocalTuning(const LocalTuning &tuning);
    void recordOpaque(VkCommandBuffer command);
    void record(VkCommandBuffer command);
    void submitted();
    remixapi_ShadowStatisticsEXT shadowStatistics() const;
    float localMilliseconds() const;
    bool foregroundActive() const;
    remixapi_DenoisingStatisticsEXT statistics() const;

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix::denoise

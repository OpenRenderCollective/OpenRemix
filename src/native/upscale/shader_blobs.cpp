#include "shaders/fsr31.h"
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_interface.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <cstring>
namespace {
  // The permutation bits of the two frame generation effects, as their
  // components compose them (FrameInterpolationShaderPermutationOptions and
  // OpticalflowShaderPermutationOptions in the components' private headers).
  enum : uint32_t {
    interpolationLowResMotion = 1,
    interpolationJitteredMotion = 2,
    interpolationDepthInverted = 4,
    interpolationWave64 = 8,
    interpolationFp16 = 16,
    opticalFlowWave64 = 1,
    opticalFlowFp16 = 2,
  };
} // namespace
extern "C" FfxErrorCode ffxGetPermutationBlobByIndex(FfxEffect effect, FfxPass pass, FfxBindStage, uint32_t flags,
                                                     FfxShaderBlob *out) {
  using namespace remix::upscale;
  if (!out)
    return FFX_ERROR_INVALID_ARGUMENT;
  uint32_t blob = 0;
  if (effect == FFX_EFFECT_FSR3UPSCALER) {
    // Our context uses HDR, inverted depth and unjittered render-resolution
    // motion. FP32 shaders also work on devices whose logical device did not
    // enable FP16.
    if (pass > 10 || (flags & (8 | 128)) || (flags & (6 | 16)) != (6 | 16) || (flags & ~255u))
      return FFX_ERROR_INVALID_ARGUMENT;
    blob = upscalerBlobs + pass + ((flags & 1) ? 11 : 0);
  } else if (effect == FFX_EFFECT_FRAMEINTERPOLATION) {
    // Unjittered render-resolution motion, either depth direction; FP32 and
    // the device's own subgroup size, as for the upscaler.
    const uint32_t required = interpolationLowResMotion, variable = interpolationDepthInverted;
    if (pass >= FFX_FRAMEINTERPOLATION_PASS_COUNT || (flags & ~variable) != required)
      return FFX_ERROR_INVALID_ARGUMENT;
    blob =
        frameInterpolationBlobs + pass + ((flags & interpolationDepthInverted) ? FFX_FRAMEINTERPOLATION_PASS_COUNT : 0);
  } else if (effect == FFX_EFFECT_OPTICALFLOW) {
    if (pass >= FFX_OPTICALFLOW_PASS_COUNT || flags)
      return FFX_ERROR_INVALID_ARGUMENT;
    blob = opticalFlowBlobs + pass;
  } else {
    return FFX_ERROR_INVALID_ARGUMENT;
  }
  std::memcpy(out, &blobs[blob], sizeof(*out));
  return FFX_OK;
}
extern "C" FfxErrorCode ffxIsWave64(FfxEffect effect, uint32_t flags, bool &wave64) {
  if (effect == FFX_EFFECT_FSR3UPSCALER)
    wave64 = (flags & 64) != 0;
  else if (effect == FFX_EFFECT_FRAMEINTERPOLATION)
    wave64 = (flags & interpolationWave64) != 0;
  else if (effect == FFX_EFFECT_OPTICALFLOW)
    wave64 = (flags & opticalFlowWave64) != 0;
  else
    return FFX_ERROR_INVALID_ARGUMENT;
  return FFX_OK;
}
// Neither upscaling nor frame generation creates or replaces the
// application's swapchain: the runtime presents the generated frames itself.
extern "C" FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(const FfxFrameGenerationConfig *) {
  return FFX_ERROR_INVALID_ARGUMENT;
}

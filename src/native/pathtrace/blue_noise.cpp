#include "pathtrace/gpu.h"
#include "pathtrace/shaders/blue_noise_data.h"

namespace remix {
using namespace trace::detail;

void PathTracer::Gpu::configureBlueNoise(VkCommandBuffer command, Frame &frame,
                                        const TraceOptions &options) {
  const bool sun = options.blueNoiseShadows && frame.sun[1] && frame.sun[0] > 0;
  const bool local = options.blueNoiseLocalLights && localIlluminationActive &&
                     denoiser && postReblur && postLocalDenoise && !options.accumulate;
  // First-surface bounce and sky directions: a repeating 32-frame pattern is
  // for the denoiser, never for a progressive reference.
  const bool sky = options.blueNoiseSky && denoiser && postReblur && !options.accumulate;
  // Sparse rendering selects its pixels from the same tables.
  const bool selection = options.sparseRendering && rrActive();
  if ((!sun && !local && !sky && !selection) || blueNoiseFailed) return;
  try {
    if (!blueNoise) {
      blueNoise = upload(command, trace::blueNoiseSamples, sizeof(trace::blueNoiseSamples),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    frame.blueNoise = blueNoise->address;
    frame.jitter[3] = float(uint32_t(sun) | uint32_t(local) << 1 | uint32_t(sky) << 2);
  } catch (VkResult error) {
    if (error == VK_ERROR_DEVICE_LOST) throw;
    blueNoiseFailed = true;
    std::fprintf(stderr, "Blue-noise sampling unavailable (%d); using hashed sampling\n", int(error));
  }
}
} // namespace remix

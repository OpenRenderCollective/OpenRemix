#pragma once
// The Metal path tracer's reconstruction statistics (upscale::Statistics'
// counterpart), shared by the tracer, the runtime and the developer UI.
#include "upscale/options.h"
#include <cstdint>
#include <string>

namespace remix::metal {
  // The reconstruction a frame was rendered with. Values are the public
  // numbering (remix_reconstruction_ext.h): MetalFX upscaling in the
  // upscaler's slot, MetalFX denoising in Ray Reconstruction's.
  enum class ReconstructionBackend : uint32_t { Native = 0, MetalFxUpscaling = 1, MetalFxDenoising = 3 };
  inline const char *name(ReconstructionBackend backend) {
    switch (backend) {
    case ReconstructionBackend::MetalFxUpscaling: return "MetalFX upscaling";
    case ReconstructionBackend::MetalFxDenoising: return "MetalFX denoising";
    default: return "Native";
    }
  }
  inline bool denoises(ReconstructionBackend backend) { return backend == ReconstructionBackend::MetalFxDenoising; }
  struct Extent {
    uint32_t width = 0, height = 0;
  };
  // upscale::Statistics' fields (upscale/renderer.h).
  struct ReconstructionStatistics {
    ReconstructionBackend backend{};
    upscale::Mode requested{};
    bool srSupported{}, rrSupported{}, sdkAvailable{}, failed{};
    uint64_t failures{}, memoryBytes{};
    bool active{};
    Extent render{}, display{};
    uint64_t frames{}, resets{};
    float milliseconds{};
    std::string failure;
  };
} // namespace remix::metal

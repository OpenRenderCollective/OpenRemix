#pragma once
#include <cstdint>

namespace remix::upscale {
  // Xess: XeSS Super Resolution, Windows only (upscale/xess.h).
  enum class Mode : uint32_t { Off, Auto, Fsr31, Dlss, Xess };
  // "rtx.native.frameGenerationBackend". Auto: DLSS Frame Generation where
  // NGX reports it available, FSR 3.1 otherwise.
  enum class FrameGenerationMode : uint32_t { Auto, Dlss, Fsr31 };
}

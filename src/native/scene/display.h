#pragma once

namespace remix::scene {
  // What the display is sent (displayColor in pathtrace/shaders/display.glsl):
  // SDR, or HDR10 with its peak and paper white nits (runtime/hdr_output.h).
  struct DisplayOutput {
    bool hdr10 = false;
    float peakNits = 1000, paperWhiteNits = 203;
  };
} // namespace remix::scene

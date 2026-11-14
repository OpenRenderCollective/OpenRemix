#pragma once
#include <cstdint>

namespace remix::scene {
  // What the display is sent (displayColor in pathtrace/shaders/display.glsl):
  // SDR, or HDR10 with its peak and paper white nits (runtime/hdr_output.h);
  // prism tonemapped, or clipped (rtx.native.pathtrace.prism off).
  struct DisplayOutput {
    bool hdr10 = false;
    float peakNits = 1000, paperWhiteNits = 203;
    bool prism = true;
    // displayColor's `display`: bit 0 HDR10, bit 2 no prism.
    uint32_t mode() const { return (hdr10 ? 1u : 0u) | (prism ? 0u : 4u); }
  };
} // namespace remix::scene

#pragma once
#include <algorithm>
#include <cmath>

namespace remix {
  // HDR10 output's brightness settings (runtime/hdr_output.h, shared with the
  // Metal backend's EDR output).
  struct HdrNits {
    float peak = 1000, paperWhite = 203, ui = 203;
    // The accepted ranges; the peak is at least paper white (the tone curve's
    // peak relative to it at least 1).
    HdrNits clamped() const {
      HdrNits c;
      c.paperWhite = std::clamp(paperWhite, 80.f, 1000.f);
      c.peak = std::clamp(peak, c.paperWhite, 10000.f);
      c.ui = std::clamp(ui, 80.f, 1000.f);
      return c;
    }
    // The scale of gamma 2.2 encoded UI colours that puts UI white at the UI
    // brightness: (UI / paper white nits)^(1 / 2.2).
    float uiScale() const {
      const auto c = clamped();
      return std::pow(c.ui / c.paperWhite, 1.f / 2.2f);
    }
  };
} // namespace remix

#pragma once
#include "runtime/settings.h"
#include <string>

namespace remix::ui {
  // What the backend reports about the sky for the Sky tab.
  struct SkyStatus {
    // The backend renders NUMOS (false: Metal, or no ray-query device).
    bool supported = false;
    bool active = false;
    // Why the sky is not active, or empty.
    std::string reason;
    float gpuMilliseconds = 0;
  };
  // The Sky tab: NUMOS controls over the profile's sky settings. Shared by the
  // Vulkan and Metal backends.
  void drawSkyTab(config::Settings &settings, const SkyStatus &status);
} // namespace remix::ui

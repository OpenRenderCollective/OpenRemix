#pragma once
#include "forward/options.h"
#include <string_view>

namespace remix::forward::presets {
  inline constexpr std::string_view source2Name = "Source 2";

  // Runtime default profile. Low-level Options retains explicit manual controls
  // for tools/tests; loading saved settings overrides these defaults normally.
  inline Options source2() {
    Options options;
    options.autoExposure = true;
    options.bakedLighting = true;
    options.rtGI = false;
    options.rtReflections = false;
    options.ssr = true;
    options.baking.enabled = true;
    options.baking.directDiffuse = true;
    options.baking.modelLightmaps = true;
    options.baking.styledIndirect = true;
    options.baking.styleStorageMiB = 256;
    return options;
  }
}

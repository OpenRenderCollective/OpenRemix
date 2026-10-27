#pragma once
#include "forward/options.h"
#include <string_view>

namespace remix::forward::presets {
  inline constexpr std::string_view source2Name = "Source 2";

  // Techniques only. Quality, display, cache and authored-volume preferences
  // survive preset application. The legacy zero capture budget also disables
  // cubemaps, so enabling them needs a default budget when no positive one exists.
  inline void applySource2(Options &options) {
    options.bakedLighting = true;
    options.rtGI = false;
    options.rtReflections = false;
    options.ssr = true;
    options.shadows = true;
    options.localShadows = true;
    options.baking.enabled = true;
    options.baking.directDiffuse = true;
    options.baking.modelLightmaps = true;
    options.baking.styledIndirect = true;
    if (!options.baking.reflectionProbes)
      options.baking.reflectionProbes = bake::Config{}.reflectionProbes;
  }

  inline bool isSource2(const Options &options) {
    auto selected = options;
    applySource2(selected);
    return selected == options;
  }

  // Runtime default profile. Low-level Options retains explicit manual controls
  // for tools/tests; loading saved settings overrides these defaults normally.
  inline Options source2() {
    Options options;
    applySource2(options);
    options.autoExposure = true;
    options.baking.styleStorageMiB = 256;
    return options;
  }
}

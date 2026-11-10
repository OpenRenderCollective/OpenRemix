#pragma once
#include "forward/techniques.h"
#include <string_view>

namespace remix::forward::presets {
  inline constexpr std::string_view source2Name = "Source 2";

  // Techniques only. Quality, display, cache and authored-volume preferences
  // survive preset application. The legacy zero capture budget also disables
  // cubemaps, so enabling them needs a default budget when no positive one exists.
  inline void applySource2(Options &options) {
    techniques::apply(options, techniques::Selection{});
    options.baking.styledIndirect = true;
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
    options.baking.modelLightmaps = true;
    options.baking.styleStorageMiB = 256;
    return options;
  }
}

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

  inline constexpr std::string_view dynamicName = "Dynamic";

  // No baked lighting: analytic direct light with shadow maps for the nearest
  // lights and ray-traced shadows for the rest, Split Radiance Cascades diffuse
  // and ray reflections over screen traces. The same
  // technique-only rule applies.
  // Without ray queries the resolved plan falls back per provider_plan.h; this
  // request does not change with the device.
  inline void applyDynamic(Options &options) {
    techniques::apply(options, techniques::Selection{
        .direct = techniques::Direct::Analytic,
        .diffuse = techniques::Diffuse::SplitCascades,
        .reflections = techniques::Reflections::RayQueries,
        .screenReflections = true,
        .rayLocalShadows = true,
        .raySunShadows = true});
  }

  inline bool isDynamic(const Options &options) {
    auto selected = options;
    applyDynamic(selected);
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

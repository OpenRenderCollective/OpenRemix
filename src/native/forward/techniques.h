#pragma once
#include "forward/options.h"

namespace remix::forward::techniques {
  enum class Direct { Analytic, AttributedBake, AggregateBake };
  enum class Diffuse { SurfaceAndProbes, RayQueries, Disabled };
  enum class Reflections { Disabled, Cubemaps, RayQueries, RayQueriesAndCubemaps };

  // Requested techniques, independent of sampling, atlas sizes and display.
  // Options remains the saved representation during migration. Do not persist
  // a second selector that could disagree with those existing settings.
  struct Selection {
    Direct direct = Direct::AggregateBake;
    Diffuse diffuse = Diffuse::SurfaceAndProbes;
    Reflections reflections = Reflections::Cubemaps;
    bool screenReflections = true;
    bool sunShadows = true, localShadows = true;
    bool operator==(const Selection &) const = default;
  };

  inline Selection requested(const Options &options) {
    Selection result;
    result.direct = !options.baking.directDiffuse ? Direct::Analytic
        : options.bakedLighting ? Direct::AggregateBake : Direct::AttributedBake;
    result.diffuse = options.rtGI ? Diffuse::RayQueries
        : options.baking.enabled ? Diffuse::SurfaceAndProbes : Diffuse::Disabled;
    result.reflections = options.baking.reflectionProbes
        ? options.rtReflections ? Reflections::RayQueriesAndCubemaps : Reflections::Cubemaps
        : options.rtReflections ? Reflections::RayQueries : Reflections::Disabled;
    result.screenReflections = options.ssr;
    result.sunShadows = options.shadows;
    result.localShadows = options.localShadows;
    return result;
  }

  inline void selectDirect(Options &options, Direct direct) {
    options.bakedLighting = direct == Direct::AggregateBake;
    options.baking.directDiffuse = direct != Direct::Analytic;
  }

  inline void selectDiffuse(Options &options, Diffuse diffuse) {
    options.rtGI = diffuse == Diffuse::RayQueries;
    options.baking.enabled = diffuse != Diffuse::Disabled;
  }

  inline void selectReflections(Options &options, Reflections reflections) {
    options.rtReflections = reflections == Reflections::RayQueries ||
                            reflections == Reflections::RayQueriesAndCubemaps;
    const bool cubemaps = reflections == Reflections::Cubemaps ||
                          reflections == Reflections::RayQueriesAndCubemaps;
    // Zero is the existing persisted off switch as well as a capture budget.
    if (!cubemaps) options.baking.reflectionProbes = 0;
    else if (!options.baking.reflectionProbes)
      options.baking.reflectionProbes = bake::Config{}.reflectionProbes;
  }

  inline void apply(Options &options, const Selection &selection) {
    selectDirect(options, selection.direct);
    selectDiffuse(options, selection.diffuse);
    selectReflections(options, selection.reflections);
    options.ssr = selection.screenReflections;
    options.shadows = selection.sunShadows;
    options.localShadows = selection.localShadows;
  }
}

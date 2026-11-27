#pragma once
#include "forward/options.h"

namespace remix::forward::techniques {
  enum class Direct { Analytic, AttributedBake, AggregateBake };
  enum class Diffuse { SurfaceAndProbes, RayQueries, Disabled, RayIncoming, SplitCascades };
  enum class Reflections { Disabled, Cubemaps, RayQueries, RayQueriesAndCubemaps };

  // Requested techniques, independent of sampling, atlas sizes and display.
  // Options remains the saved representation during migration. Do not persist
  // a second selector that could disagree with those existing settings.
  struct Selection {
    Direct direct = Direct::AggregateBake;
    Diffuse diffuse = Diffuse::SurfaceAndProbes;
    Reflections reflections = Reflections::Cubemaps;
    bool screenReflections = false;
    bool sunShadows = true, localShadows = true;
    bool rayLocalShadows = false; // ray-traced visibility for lights beyond the shadow-map budget
    bool raySunShadows = false;   // ray-traced sun visibility in place of its cascades
    bool operator==(const Selection &) const = default;
  };

  inline Selection requested(const Options &options) {
    Selection result;
    result.direct = !options.baking.directDiffuse ? Direct::Analytic
        : options.bakedLighting ? Direct::AggregateBake : Direct::AttributedBake;
    result.diffuse = options.splitCascades ? Diffuse::SplitCascades
        : options.rtGI ? options.bakedDirectWithLiveGI ? Diffuse::RayIncoming : Diffuse::RayQueries
        : options.baking.enabled ? Diffuse::SurfaceAndProbes : Diffuse::Disabled;
    result.reflections = options.baking.reflectionProbes
        ? options.rtReflections ? Reflections::RayQueriesAndCubemaps : Reflections::Cubemaps
        : options.rtReflections ? Reflections::RayQueries : Reflections::Disabled;
    result.screenReflections = options.ssr;
    result.sunShadows = options.shadows;
    result.localShadows = options.localShadows;
    result.rayLocalShadows = options.rtLocalShadows;
    result.raySunShadows = options.rtSunShadows;
    return result;
  }

  inline void selectDirect(Options &options, Direct direct) {
    options.bakedLighting = direct == Direct::AggregateBake;
    options.baking.directDiffuse = direct != Direct::Analytic;
  }

  inline void selectDiffuse(Options &options, Diffuse diffuse) {
    options.rtGI = diffuse == Diffuse::RayQueries || diffuse == Diffuse::RayIncoming;
    options.bakedDirectWithLiveGI = diffuse == Diffuse::RayIncoming;
    options.splitCascades = diffuse == Diffuse::SplitCascades;
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
    options.rtLocalShadows = selection.rayLocalShadows;
    options.rtSunShadows = selection.raySunShadows;
  }
}

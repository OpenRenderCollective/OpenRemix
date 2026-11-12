#pragma once

namespace remix::postfx {
  // Renderer-neutral inputs to the shared exposure/bloom/display preparation.
  // Meter inputs and their interpretation remain explicit at initialization.
  struct Options {
    bool autoExposure = false, bloom = false, contrast = false, chromaticAberration = false;
    float exposure = 1, autoExposureTarget = .18f, autoExposureSpeed = 2;
    float autoExposureCentre = .75f, autoExposureMinEv = -8, autoExposureMaxEv = 4;
    float bloomIntensity = .01f, bloomRadius = .85f, contrastAmount = 1.15f;
    // Lens chromatic aberration: the red and blue channels' displacement from
    // the green at the left and right edges of the image, a fraction of its
    // width, growing from none at the centre.
    float chromaticAberrationAmount = .002f;
  };
  // Both renderer option types use this mapping. Effects without renderer
  // controls stay disabled, rather than inheriting another renderer's defaults.
  template <class Source> Options settings(const Source &source) {
    Options result;
    result.autoExposure = source.autoExposure;
    result.exposure = source.exposure;
    result.autoExposureTarget = source.autoExposureTarget;
    result.autoExposureSpeed = source.autoExposureSpeed;
    result.autoExposureCentre = source.autoExposureCentre;
    result.autoExposureMinEv = source.autoExposureMinEv;
    result.autoExposureMaxEv = source.autoExposureMaxEv;
    if constexpr (requires { source.bloom; source.bloomIntensity; source.bloomRadius; }) {
      result.bloom = source.bloom;
      result.bloomIntensity = source.bloomIntensity;
      result.bloomRadius = source.bloomRadius;
    }
    if constexpr (requires { source.contrast; source.contrastAmount; }) {
      result.contrast = source.contrast;
      result.contrastAmount = source.contrastAmount;
    }
    if constexpr (requires { source.chromaticAberration; source.chromaticAberrationAmount; }) {
      result.chromaticAberration = source.chromaticAberration;
      result.chromaticAberrationAmount = source.chromaticAberrationAmount;
    }
    return result;
  }
}

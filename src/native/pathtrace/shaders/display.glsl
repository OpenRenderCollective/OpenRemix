#include "color.glsl"

const mat3 inset = mat3(
  .72634649, .13423698, .07263955,
  .16665483, .70013888, .06012051,
  .10699868, .16562414, .86723994);
const mat3 outset = mat3(
  1.45021560, -.25347057, -.10389769,
  -.33533138, 1.51071102, -.07664130,
  -.11488422, -.25724045, 1.18053899);

float reinhardPiecewiseSmooth(float x, float peak, float anchor){
    if (x <= anchor)
        return x;

    float range = peak - anchor;
    float t = (x - anchor) / range;
    float s = exp(-0.01 / (t * t));
    return anchor + range * t / (1.0 + t * s);
}

vec3 reinhardPiecewiseSmooth(vec3 color, float peak, float anchor){
    return vec3(
        reinhardPiecewiseSmooth(color.x, peak, anchor),
        reinhardPiecewiseSmooth(color.y, peak, anchor),
        reinhardPiecewiseSmooth(color.z, peak, anchor));
}

vec3 prismToneMap(vec3 color, float peak){
  color = inset * color;
  color = reinhardPiecewiseSmooth(color, peak, 0.18);
  return outset * color;
}

const uint displaySdr = 0u;
const uint displayHdr10 = 1u;
// Or'd into `display`: clipped at the peak rather than prism tonemapped
// (rtx.native.pathtrace.prism off).
const uint displayNoPrism = 4u;

vec3 displayColor(vec3 color, uint display, float peakNits, float paperWhiteNits){
  bool prism = (display & displayNoPrism) == 0u;
  display &= ~displayNoPrism;
  const float peak = display == displayHdr10 ? peakNits / paperWhiteNits : 1.0;
  color = prism ? prismToneMap(color, peak) : clamp(color, vec3(0), vec3(peak));

  return gammaEncodeSafe(color, 2.2);
}

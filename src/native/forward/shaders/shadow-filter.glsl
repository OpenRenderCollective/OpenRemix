#ifndef REMIX_SHADOW_FILTER_GLSL
#define REMIX_SHADOW_FILTER_GLSL
// Fractional translation of a normalized [1, 2, 1] tent. Four nearest
// comparisons preserve receiver-plane correction and interpolate continuously.
vec4 shadowTentWeights(float fraction) {
  return vec4(1 - fraction, 2 - fraction, 1 + fraction, fraction) * .25;
}
#endif

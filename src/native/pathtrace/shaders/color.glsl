#ifndef REMIX_COLOR_GLSL
#define REMIX_COLOR_GLSL

vec3 gammaEncodeSafe(vec3 color, float gamma){
  return sign(color) * pow(abs(color), vec3(1.0 / gamma));
}

vec3 gammaDecodeSafe(vec3 color, float gamma){
  return sign(color) * pow(abs(color), vec3(gamma));
}

const mat3 bt709To2020 = mat3(
  0.6274038960, 0.0690972894, 0.0163914388,
  0.3292830384, 0.9195403951, 0.0880133078,
  0.0433130657, 0.0113623156, 0.8955952532);

const mat3 bt2020To709 = mat3(
  1.6604910020, -0.1245504746, -0.0181507632,
  -0.5876411388, 1.1328998971, -0.1005788980,
  -0.0728498633, -0.0083494226, 1.1187296614);

// ST.2084 (PQ) constants.
const float pqM1 = 2610.0 / 16384.0;
const float pqM2 = 128.0 * (2523.0 / 4096.0);
const float pqC1 = 3424.0 / 4096.0;
const float pqC2 = 32.0 * (2413.0 / 4096.0);
const float pqC3 = 32.0 * (2392.0 / 4096.0);

vec3 pqEncode(vec3 color, float whiteNits){
  vec3 y = pow(clamp(color * (whiteNits / 10000.0), 0.0, 1.0), vec3(pqM1));
  return pow((pqC1 + pqC2 * y) / (1.0 + pqC3 * y), vec3(pqM2));
}

vec3 pqDecode(vec3 pq, float whiteNits){
  vec3 y = pow(clamp(pq, 0.0, 1.0), vec3(1.0 / pqM2));
  vec3 nits = 10000.0 * pow(max(y - pqC1, 0.0) / (pqC2 - pqC3 * y), vec3(1.0 / pqM1));
  return nits / whiteNits;
}
#endif

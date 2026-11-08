#ifndef OPENREMIX_STANDARD_BRDF
#define OPENREMIX_STANDARD_BRDF
#include "brdf-values.glsl"
BrdfLobes brdfLobes(vec3 base, float roughness, float metallic, vec3 n, vec3 v, vec3 l, vec3 dielectricF0) {
  float nl = max(dot(n, l), 0), nv = max(dot(n, v), 1e-4);
  vec3 halfVector = v + l;
  float halfLengthSquared = dot(halfVector, halfVector);
  bool degenerateHalfVector = halfLengthSquared < 1e-8;
  vec3 h = degenerateHalfVector ? vec3(0) : normalize(halfVector);
  vec3 f0 = mix(dielectricF0, base, metallic);
  float vh = halfLengthSquared > 1e-8 ? max(dot(v, h), 0) : 0;
  vec3 f = f0 + (1 - f0) * pow(1 - vh, 5);
  vec3 diffuse = (1 - f) * (1 - metallic) * base / Pi;
  if (nl <= 0 || degenerateHalfVector)
    return BrdfLobes(diffuse, vec3(0));
  float nh = max(dot(n, h), 0);
  float a = max(roughness * roughness, .0025), a2 = a * a;
  float d = a2 / (Pi * pow(nh * nh * (a2 - 1) + 1, 2));
  float visibility = .5 / max(nl * sqrt(nv * nv * (1 - a2) + a2) + nv * sqrt(nl * nl * (1 - a2) + a2), 1e-5);
  return BrdfLobes(diffuse, d * visibility * f * nl);
}
BrdfLobes brdfLobes(vec3 base, float roughness, float metallic, vec3 n, vec3 v, vec3 l) {
  return brdfLobes(base, roughness, metallic, n, v, l, vec3(.04));
}
vec3 brdf(vec3 base, float roughness, float metallic, vec3 n, vec3 v, vec3 l) {
  BrdfLobes response = brdfLobes(base, roughness, metallic, n, v, l);
  return response.diffuse * max(dot(n, l), 0) + response.specular;
}

#endif

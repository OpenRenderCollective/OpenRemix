#ifndef FORWARD_WATER_OPTICS_GLSL
#define FORWARD_WATER_OPTICS_GLSL
// Primary water boundary semantics from pathtrace/shaders/pathtrace.comp.
// The bake's smooth-interface transport intentionally uses exact Fresnel;
// the primary sheet uses Schlick, Snell refraction and measured absorption.
#include "../shaders/dielectric.glsl"
struct WaterBoundary {
  vec3 refracted, reflected;
  float reflectance;
  bool totalInternal;
};
WaterBoundary waterBoundary(vec3 incident, vec3 normal, float index, bool inside) {
  incident = normalize(incident);
  normal = normalize(normal);
  vec3 facing = dot(incident, normal) < 0 ? normal : -normal;
  float ratio = (index - 1) / (index + 1), f0 = ratio * ratio;
  float fresnel = f0 + (1 - f0) * pow(1 - clamp(abs(dot(normal, incident)), 0, 1), 5);
  vec3 bent = refract(incident, facing, inside ? index : 1 / index);
  bool total = dot(bent, bent) == 0;
  return WaterBoundary(total ? incident : normalize(bent), reflect(incident, facing),
                         total ? 1 : fresnel, total);
}
vec3 waterThrough(WaterBoundary boundary, vec3 tint, float measurement, float distance,
                  bool foundBehind, bool inside, out float surfaceWeight) {
  vec3 passed = vec3(1);
  if (!inside) {
    vec3 extinction = -log(clamp(tint, vec3(1e-4), vec3(1))) / max(measurement, 1e-6);
    passed = foundBehind ? beerLambert(extinction, distance) : vec3(0);
  }
  vec3 through = (1 - boundary.reflectance) * passed;
  surfaceWeight = 1 - dot(through, vec3(.2126, .7152, .0722));
  return through;
}
#endif

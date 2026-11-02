#ifndef OPENREMIX_SHADING_VALUES
#define OPENREMIX_SHADING_VALUES
#include "surface-values.glsl"
// Material shading output values, independent of receiver/source resources.
struct ShadingResult {
  vec4 color;
  vec3 normal;
  float roughness;
  vec3 reflectance;
  bool hasReflectance;
  float irradianceLuma; // irradiance / pi luminance; -1 means unavailable
};
// Safe defaults for a diffuse/custom model. Models opt into a reflection
// response/reference explicitly; no pass bindings or chart fields are needed.
ShadingResult initShadingResult(Surface surface, float opacity) {
  ShadingResult result;
  result.color = vec4(0, 0, 0, opacity);
  result.normal = surface.normal;
  result.roughness = surface.roughness;
  result.reflectance = vec3(0);
  result.hasReflectance = false;
  result.irradianceLuma = -1;
  return result;
}
#endif

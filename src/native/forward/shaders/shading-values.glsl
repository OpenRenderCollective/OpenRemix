#ifndef OPENREMIX_SHADING_VALUES
#define OPENREMIX_SHADING_VALUES
#include "surface-values.glsl"
// Material shading output values, independent of receiver/source resources.
struct ShadingResult {
  vec4 color;
  vec4 normalRoughness;
  vec4 reflectance;
  float irradianceLuma; // irradiance / pi luminance; -1 means unavailable
};
// Safe defaults for a diffuse/custom model. Models opt into a reflection
// response/reference explicitly; no pass bindings or chart fields are needed.
ShadingResult initShadingResult(Surface surface, float opacity) {
  ShadingResult result;
  result.color = vec4(0, 0, 0, opacity);
  result.normalRoughness = vec4(surface.normal, surface.roughness);
  result.reflectance = vec4(0);
  result.irradianceLuma = -1;
  return result;
}
#endif

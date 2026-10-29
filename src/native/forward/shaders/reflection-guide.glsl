#ifndef FORWARD_REFLECTION_GUIDE_GLSL
#define FORWARD_REFLECTION_GUIDE_GLSL
// RGBA16F: octahedral world normal XY, roughness Z, irradiance/pi luminance W.
// Negative roughness marks uncovered/invalid guides; negative luminance means
// unknown incident lighting. Picking one covered MSAA sample preserves all fields.
vec4 invalidReflectionGuide() { return vec4(0, 0, -1, -1); }
bool reflectionGuideValid(vec4 guide) { return guide.z >= 0 && guide.z <= 1; }
vec2 reflectionNormalSign(vec2 value) {
  return mix(vec2(-1), vec2(1), greaterThanEqual(value, vec2(0)));
}
vec4 packReflectionGuide(vec3 normal, float roughness, float irradianceLuma) {
  float magnitude = dot(abs(normal), vec3(1));
  if (magnitude <= 1e-10) return invalidReflectionGuide();
  normal /= magnitude;
  vec2 oct = normal.z >= 0 ? normal.xy : (1 - abs(normal.yx)) * reflectionNormalSign(normal.xy);
  return vec4(oct, roughness, irradianceLuma);
}
vec3 reflectionGuideNormal(vec4 guide) {
  if (!reflectionGuideValid(guide)) return vec3(0);
  vec3 normal = vec3(guide.xy, 1 - abs(guide.x) - abs(guide.y));
  normal.xy -= reflectionNormalSign(normal.xy) * max(-normal.z, 0);
  return normalize(normal);
}
#endif

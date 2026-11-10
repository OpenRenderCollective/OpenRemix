#ifndef OPENREMIX_SUN_VALUES
#define OPENREMIX_SUN_VALUES
// Sun transport is independent of the material response. Irradiance excludes
// RGB radiance; moving-caster visibility is already included in radiance.
struct SunLightSample {
  vec3 direction;
  vec3 radiance;
  float diffuseIrradiance;
  float staticVisibility;
};
#endif

#ifndef OPENREMIX_LOCAL_LIGHT_VALUES
#define OPENREMIX_LOCAL_LIGHT_VALUES
// Shader-facing lighting data, before material/BRDF response or exposure.
// Diffuse irradiance and analytic weights exclude RGB radiance. Aggregate diffuse represents
// already-baked energy and requires a moving-caster correction, not a second add.
struct LocalLightSample {
  bool aggregateDiffuse;
  bool selectedHighlight;
  vec3 direction;
  vec3 radiance;
  float diffuseIrradiance;
  float analyticWeight;
  float movingVisibility;
  float influence;
  float highlightWeight;
};
#endif

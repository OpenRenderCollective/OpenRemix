#include "diffuse-residual.glsl"
#include "light-sample.glsl"

// Standard PBR response is separate from light transport and visibility.
BrdfLobes shadeStandardLocalLight(LocalLightSample lighting, vec3 n, vec3 v,
                                  vec3 base, float roughness, float metallic, vec3 dielectricF0) {
  BrdfLobes response = brdfLobes(base, roughness, metallic, n, v, lighting.direction, dielectricF0);
  // The aggregate SH is irradiance, shaded by the same Lambertian material
  // weight as the ambient term. Keep its moving-caster correction in those
  // units instead of adding another view-dependent analytic diffuse lobe.
  vec3 diffuseWeight = lighting.aggregateDiffuse ? base * (1 - metallic) / Pi : response.diffuse;
  vec3 diffuse = diffuseResidual(diffuseWeight * lighting.diffuseIrradiance, lighting.movingVisibility, lighting.aggregateDiffuse);
  vec3 specular = response.specular * lighting.analyticWeight;
  // SelectedAggregate has a separate highlight weight. Its shadow-map strength
  // must remain independent. Uncovered receivers retain the analytic fallback.
  if (lighting.selectedHighlight)
    specular *= lighting.highlightWeight;
  // Aggregate transport integrates the physical emitter without the runtime
  // influence fade. Its subtraction must use those same radiometric units.
  return BrdfLobes(diffuse * lighting.radiance * (lighting.aggregateDiffuse ? 1 : lighting.influence),
                   specular * lighting.radiance * (lighting.influence * lighting.movingVisibility));
}

BrdfLobes localLightResponse(uint index, vec3 p, vec3 n, vec3 v, vec3 base, float roughness, float metallic,
                vec4 directIrradiance, BakedReceiver receiver, vec3 dielectricF0, vec3 receiverNormal,
                bool aggregateReceiver) {
  const bool frontFacingOnly = true;
#define LOCAL_LIGHT_MISS return BrdfLobes(vec3(0), vec3(0))
#include "local-light-query.glsl"
#undef LOCAL_LIGHT_MISS
  return shadeStandardLocalLight(lighting, n, v, base, roughness, metallic, dielectricF0);
}

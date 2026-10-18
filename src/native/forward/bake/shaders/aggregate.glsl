// Fully baked direct light joins the surface/grid irradiance, rather than
// requiring a separate visibility query for every light at every texel.
// The all-light mixture PDF stays valid when only a subset contributes.
void accumulateBakedDiffuse(inout RayResult value, vec3 origin, Medium medium, uint ordinal) {
  if (!lightingStyleSelected(0u) || (rayFrame.control.w & 4u) == 0u || rayFrame.counts.y == 0u)
    return;
  // Independent of the diffuse path's random stream and batch-local indices.
  uint seed = floatBitsToUint(origin.x) * 73856093u ^ floatBitsToUint(origin.y) * 19349663u ^
              floatBitsToUint(origin.z) * 83492791u ^ (ordinal + 1u) * 2654435761u ^ 0x8da6b343u;
  float pdf;
  // SH represents the incident sphere, including normals other than the bake
  // normal. A zero normal prevents hemisphere pruning in the light proposal.
  uint index = selectLight(origin, vec3(0), random(seed), pdf);
  RayLight light = rayLights[index];
  if (light.shaping.z < .5)
    return;
  vec3 direction;
  float distance, weight;
  if (!sampleEmitter(light, origin, seed, direction, distance, weight))
    return;
  vec3 passing = visibility(origin, direction, emitterShadowDistance(light, distance), seed, medium);
  // The common irradiance epilogue applies 4Pi/N and cosine convolution.
  // NEE already integrates emitter solid angle, so remove that extra 4Pi.
  vec3 incoming = light.radiance.rgb * passing * (weight / (pdf * 4 * RayPi));
  float sh[9];
  basis(direction, sh);
  for (uint coefficient = 0u; coefficient < 9u; ++coefficient)
    value.irradiance[coefficient].rgb += incoming * sh[coefficient];
}

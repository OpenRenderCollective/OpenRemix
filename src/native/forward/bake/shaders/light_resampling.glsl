// Resample unshadowed light proposals, then trace one selected connection.
// The reservoir normalization retains the original all-light expectation.
vec3 resampledDirectLight(vec3 p, vec3 n, inout uint seed, Medium medium) {
  const uint candidates = 4u;
  float totalWeight = 0, selectedWeight = 0, selectedDistance = 0;
  vec3 selectedDirection = vec3(0), selectedEnergy = vec3(0);
  uint selectedLight = 0u;
  for (uint candidate = 0u; candidate < candidates; ++candidate) {
    float selectionPdf;
    uint index = selectLight(p, n, random(seed), selectionPdf);
    // Every component uses the full captured proposal and the same reservoir.
    // Apply style selection only after the shared visibility query.
    vec3 direction;
    float distance, weight;
    if (!sampleEmitter(rayLights[index], p, seed, direction, distance, weight)) continue;
    vec3 energy = rayLights[index].radiance.rgb * (weight * max(dot(n, direction), 0) / selectionPdf);
    float importance = dot(energy, vec3(.2126, .7152, .0722));
    if (importance <= 0) continue;
    totalWeight += importance;
    if (selectedWeight == 0 || random(seed) * totalWeight < importance) {
      selectedWeight = importance;
      selectedEnergy = energy;
      selectedDirection = direction;
      selectedDistance = distance;
      selectedLight = index;
    }
  }
  if (selectedWeight > 0) {
    vec3 passing = visibility(p, selectedDirection,
                             emitterShadowDistance(rayLights[selectedLight], selectedDistance), seed, medium);
    if (lightingStyleSelected(uint(rayLights[selectedLight].shaping.w)))
      return selectedEnergy * (totalWeight / (float(candidates) * selectedWeight)) * passing;
  }
  return vec3(0);
}

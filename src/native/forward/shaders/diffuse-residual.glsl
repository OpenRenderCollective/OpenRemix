// Aggregate irradiance already contains this light's unoccluded-by-moving-
// objects diffuse contribution. Mixed shadows subtract only the newly blocked
// portion; ordinary analytic lighting adds its visible portion.
vec3 diffuseResidual(vec3 contribution, float movingVisibility, bool aggregateReceiver) {
  return contribution * (movingVisibility - (aggregateReceiver ? 1 : 0));
}

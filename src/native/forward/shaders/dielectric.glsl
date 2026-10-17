#ifndef FORWARD_DIELECTRIC_GLSL
#define FORWARD_DIELECTRIC_GLSL
// Shared smooth-interface optics for measured water and glass. Distances and
// extinction must use the same scene units. Ripple normals are a caller policy.
vec3 beerLambert(vec3 extinction, float distance) {
  return exp(-extinction * max(distance, 0));
}
float fresnelBoundary(float cosine, float incident, float transmitted) {
  cosine = clamp(cosine, 0, 1);
  float eta = incident / transmitted;
  float sineSquared = eta * eta * max(0, 1 - cosine * cosine);
  if (sineSquared >= 1)
    return 1;
  float other = sqrt(max(0, 1 - sineSquared));
  float parallel =
      (transmitted * cosine - incident * other) / max(transmitted * cosine + incident * other, 1e-8);
  float perpendicular =
      (incident * cosine - transmitted * other) / max(incident * cosine + transmitted * other, 1e-8);
  return clamp(.5 * (parallel * parallel + perpendicular * perpendicular), 0, 1);
}
#endif

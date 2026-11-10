#ifndef FORWARD_DIELECTRIC_GLSL
#define FORWARD_DIELECTRIC_GLSL
// Shared smooth-interface optics for measured water and glass. Distances and
// extinction must use the same scene units. Ripple normals are a caller policy.
vec3 beerLambert(vec3 extinction, float distance) {
  return exp(-extinction * max(distance, 0));
}
float fresnelBoundary(float cosine, float incident, float transmitted) {
  if (incident == transmitted)
    return 0; // No interface, including the grazing limit.
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
struct DielectricSheet {
  vec3 reflection, transmission;
};
struct DielectricSheetPath {
  vec3 exitOffset, reflected;
  bool totalInternal;
};
// Parallel interfaces restore the incident direction after transmission, but
// shift the ray sideways. Thickness is measured along the sheet normal.
DielectricSheetPath dielectricSheetPath(vec3 incident, vec3 normal, float thickness,
                                         float incidentIndex, float sheetIndex) {
  incident = normalize(incident);
  normal = normalize(normal);
  vec3 facing = dot(incident, normal) < 0 ? normal : -normal;
  vec3 inside = incidentIndex == sheetIndex ? incident : refract(incident, facing, incidentIndex / sheetIndex);
  bool total = dot(inside, inside) < 1e-12;
  // Extremely thick or grazing sheets leave the available scene. Bound the
  // offset so their screen projection stays finite and can reject that path.
  float distance = min(thickness / max(-dot(inside, facing), 1e-6), 1e30);
  vec3 offset = total ? vec3(0) : inside * distance;
  return DielectricSheetPath(offset, reflect(incident, facing), total);
}
// Incoherent multiple reflections in a parallel sheet. Outside directions and
// medium membership are unchanged after transmission through both interfaces.
DielectricSheet dielectricSheet(vec3 extinction, float thickness, float cosine,
                                 float incident, float sheetIndex) {
  cosine = clamp(cosine, 0, 1);
  float eta = incident / sheetIndex;
  float sineSquared = eta * eta * max(0, 1 - cosine * cosine);
  if (sineSquared >= 1 && incident != sheetIndex)
    return DielectricSheet(vec3(1), vec3(0));
  float f = fresnelBoundary(cosine, incident, sheetIndex);
  if (f >= 1)
    return DielectricSheet(vec3(1), vec3(0));
  float insideCosine = incident == sheetIndex ? cosine : sqrt(max(0, 1 - sineSquared));
  // Multiply before dividing: a clear, extremely thick sheet must not produce
  // zero times an overflowing path length. Optical depths above 80 are opaque.
  vec3 depth = min(extinction * thickness, vec3(80)) / max(insideCosine, 1e-6);
  vec3 absorbed = exp(-min(depth, vec3(80)));
  vec3 denominator = max(vec3(1) - f * f * absorbed * absorbed, vec3(1e-6));
  vec3 transmitted = (1 - f) * (1 - f) * absorbed / denominator;
  vec3 reflected = vec3(f) + (1 - f) * (1 - f) * f * absorbed * absorbed / denominator;
  return DielectricSheet(clamp(reflected, 0, 1), clamp(transmitted, 0, 1));
}
#endif

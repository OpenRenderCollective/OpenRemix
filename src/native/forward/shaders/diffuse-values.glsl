#ifndef OPENREMIX_DIFFUSE_VALUES
#define OPENREMIX_DIFFUSE_VALUES
// Incident diffuse irradiance / pi, before material response or exposure.
struct ReceiverLighting {
  vec3 diffuse;
  // Apply the receiver's per-light baked attribution policy. This does not mean
  // every light is present: individual source membership remains query-owned.
  bool usesDirectAttribution;
  // This field can supply the local incident-light reference used by the
  // selected reflection provider. Independent of direct-light membership.
  bool reflectionReference;
};
vec3 composeDiffuseLight(ReceiverLighting lighting, vec3 diffuseReflectance) {
  return diffuseReflectance * lighting.diffuse;
}
#endif

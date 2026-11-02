#ifndef OPENREMIX_LIGHTING_REFERENCE
#define OPENREMIX_LIGHTING_REFERENCE
// References compare scene-linear incident irradiance / pi, before material or
// exposure. A domain is a provider contract: matching units alone do not prove
// that two references contain the same lighting. Zero means unavailable.
const uint NoLightingReference = 0u;
const uint Source2StaticLightingReference = 1u;
struct LightingReference {
  float luminance;
  uint domain;
};
LightingReference unavailableLightingReference() {
  return LightingReference(-1, NoLightingReference);
}
bool compatibleLightingReferenceDomains(uint receiver, uint capture) {
  return receiver != NoLightingReference && receiver == capture;
}
bool compatibleLightingReferences(LightingReference receiver, LightingReference capture) {
  return compatibleLightingReferenceDomains(receiver.domain, capture.domain);
}
bool neutralReferenceLuminance(float receiver, float capture) {
  return receiver < 0 || capture <= 0;
}
// Call only after rejecting neutral references. Scalar helpers let pass adapters
// preserve their packed/vector representation without moving radiance through
// temporary aggregates; material/provider code can use the typed entry below.
float reflectionReferenceScale(float receiver, float capture) {
  return min(max(receiver, 0) / capture, 1);
}
// Bounded normalization: a compatible receiver can attenuate a frozen capture,
// never amplify it. A valid black receiver attenuates to black; unknown receiver
// lighting or a dark capture reference is neutral. Providers choose the domain.
vec3 normalizeReflectionReference(vec3 radiance, LightingReference receiver, LightingReference capture) {
  if (!compatibleLightingReferences(receiver, capture) || neutralReferenceLuminance(receiver.luminance, capture.luminance))
    return radiance;
  return radiance * reflectionReferenceScale(receiver.luminance, capture.luminance);
}
#endif

// An alternate source with no chart coordinates, shadow maps or frame bindings.
#include "sun-values.glsl"
SunLightSample sampleReceiverSunLight(ShadingReceiver receiver, vec3 normal, float unitIrradiance) {
  return SunLightSample(vec3(0, 0, 1), vec3(.2), max(normal.z, 0), 1);
}
SunLightSample sampleOpticalReceiverSunLight(ShadingReceiver receiver, vec3 normal) {
  return SunLightSample(vec3(0, 0, 1), vec3(.2), max(normal.z, 0), 1);
}

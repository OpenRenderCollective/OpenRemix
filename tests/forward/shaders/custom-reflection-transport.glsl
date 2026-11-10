// Deliberately independent of rawRadiance, frame and bilateral guide storage.
#include "reflection-sample.glsl"
ReflectionSample sampleTransportReflection(ivec2 pixel, vec4 guide, float centerDepth) {
  float confidence = guide.z < .5 ? .75 : .25;
  vec3 radiance = vec3(float(pixel.x & 1), float(pixel.y & 1), centerDepth * .001);
  return ReflectionSample(radiance * confidence, confidence);
}

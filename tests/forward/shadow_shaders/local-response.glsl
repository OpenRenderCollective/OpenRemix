// Exercise the production raw query and standard adapter against the same
// filtered moving shadow, including exact unshadowed receivers.
#include "../../../src/native/forward/shaders/receiver.glsl"
#include "../../../src/native/forward/shaders/emitter.glsl"
#include "../../../src/native/forward/shaders/local-light-response.glsl"
const bool FullyMappedReceiver = true;
Light lights[1];
bool fieldKnown;
float fieldValue;
bool indexedSurfaceDirect() { return true; }
uint staticLightVisibility(uint chart, uint id) { return 2u; }
uint lightChannel(uvec4 owners, uint id) { return 0xffffffffu; }
bool surfaceDirectionalField(uint chart, uint id, vec4 coordinate, vec3 normal, vec4 bounds, out float value) {
  value = fieldValue;
  return fieldKnown;
}
bool bakedLightVisibility(vec4 coordinate, vec4 bounds, uvec4 owners, uint id, out float value) {
  value = 1;
  return false;
}
bool overflowLightVisibility(uint chart, uint id, vec4 coordinate, vec4 bounds, out float value) {
  value = 1;
  return false;
}
bool batchedProbeVisibility(vec3 position, vec3 normal, uint id, out float value) {
  value = 1;
  return false;
}
bool bakedLocalProbeResponse(vec3 position, vec3 geometric, vec3 normal, uint id, bool correction,
                            out float visibility, out float diffuse) {
  visibility = 1;
  diffuse = -1;
  return false;
}
bool emitterAbovePlane(Light light, vec3 position, vec3 normal) {
  return dot(normal, light.positionRadius.xyz - position) + light.positionRadius.w > 0;
}
bool rawLocalLight(uint index, vec3 p, vec3 n, vec3 receiverNormal, BakedReceiver receiver,
                   vec4 directIrradiance, bool aggregateReceiver, out LocalLightSample lightSample) {
  const bool frontFacingOnly = true;
#define LOCAL_LIGHT_MISS(reason) return false
#include "../../../src/native/forward/shaders/local-light-query.glsl"
#undef LOCAL_LIGHT_MISS
  lightSample = lighting;
  return true;
}
// Supply the raw query above without importing the production storage backend.
#define OPENREMIX_LOCAL_LIGHT_SAMPLE
#include "../../../src/native/forward/shaders/direct-light.glsl"

vec4 compareLocalResponses(vec3 position, vec3 geometric) {
  Light light;
  light.positionRadius = vec4(frame.localPositions[0].xyz, .25);
  light.shaping = vec4(0, 0, 100, 1);
  light.radiance = vec4(1, .5, .25, 0);
  light.corners[0] = vec4(0, 0, 0, 1);
  light.corners[1] = vec4(0);
  light.corners[2] = vec4(0, 0, 0, 3);
  light.responseWeights = vec4(.5, 0, 0, 0);
  vec3 n = normalize(light.positionRadius.xyz - position);
  BakedReceiver receiver = BakedReceiver(vec4(0, 0, 0, 1), vec4(0, 0, 1, 1),
      uvec4(0xffffffffu), uvec4(0xffffffffu), 0u);
  float moving = localShadowVisibility(light, position, geometric, MovingLocalShadowLayer, true);
  vec4 result = vec4(0);
  // Positive, known-black and missing fields, both inside and outside the
  // analytic cone. Captured finite-emitter energy must survive cone rejection.
  for (uint cone = 0u; cone < 2u; ++cone) {
    light.directionCone = vec4(n, cone == 0u ? -1 : .95);
    lights[0] = light;
    for (uint field = 0u; field < 3u; ++field) {
      fieldKnown = field < 2u;
      fieldValue = field == 0u ? 2 : 0;
      LocalLightSample lightSample;
      bool found = rawLocalLight(0u, position, n, geometric, receiver, vec4(-1), true, lightSample);
      if (fieldKnown)
        result.y += found && lightSample.diffuseIrradiance == fieldValue ? 0 : 1;
      if (found)
        result.z += lightSample.movingVisibility == moving ? 0 : 1;
      LightContribution expected = LightContribution(vec3(0), vec3(0));
      if (found)
        expected = shadeStandardLocalLight(lightSample, n, n, vec3(.6), .5, 0, vec3(.04));
      uint miss;
      LightContribution actual = localLightResponse(0u, position, n, n, vec3(.6), .5, 0,
          vec4(-1), receiver, vec3(.04), geometric, true, miss);
      vec3 difference = max(abs(actual.diffuse - expected.diffuse), abs(actual.specular - expected.specular));
      result.x = max(result.x, max(difference.x, max(difference.y, difference.z)));
      result.w += moving == 1 ? 1 : 0;
    }
  }
  // A negative centre cosine can still retain baked light from a finite
  // emitter crossing the horizon. The same sphere farther back must miss.
  vec3 horizonNormal = vec3(1, 0, 0);
  light.directionCone.w = -1;
  fieldKnown = true;
  fieldValue = 2;
  light.positionRadius.xyz = position - .125 * horizonNormal;
  lights[0] = light;
  LocalLightSample horizonSample;
  bool found = rawLocalLight(0u, position, horizonNormal, geometric, receiver, vec4(-1), true, horizonSample);
  if (!found || horizonSample.diffuseIrradiance != 2) result.y += 1;
  light.positionRadius.xyz = position - .5 * horizonNormal;
  lights[0] = light;
  if (rawLocalLight(0u, position, horizonNormal, geometric, receiver, vec4(-1), true, horizonSample)) result.y += 1;
  return result;
}

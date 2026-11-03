#define PROBE_BINDING 12
#define PROBE_IMAGE_BINDING 19
#define PROBE_TILE_CANDIDATES
layout(set = 0, binding = 13, std140) uniform ViewModelSky {
  mat4 inverseProjection, viewToDome;
  vec4 radiance, target;
} sky;
layout(set = 0, binding = 14) uniform sampler2D skyMap;
#include "../../shaders/environment-sampling.glsl"
#include "../../shaders/reflection-lighting.glsl"
layout(set = 0, binding = 15, std140) uniform ViewModelScreen {
  mat4 inverseCamera;
  vec4 trace;
  uvec4 config;
  uvec4 probeCandidates;
} viewModelScreen;
layout(set = 0, binding = 16) uniform sampler2D depthPyramid;
layout(set = 0, binding = 17) uniform sampler2D normals;
layout(set = 0, binding = 18) uniform sampler2D opaqueColor;
#include "../../shaders/reflection_math.glsl"
#include "../../shaders/radiance-filter.glsl"
#include "../../shaders/screen-reflection.glsl"

// Scene-linear reflected radiance, before the material Fresnel response.
vec3 viewModelReflectionRadiance(vec3 position, vec3 n, vec3 v, float roughness, uint layer, LightingReference reference) {
  vec3 reflected = reflect(-v, n);
  ReflectionReceiver receiver = ReflectionReceiver(position, position + n * sky.target.z, n,
      reflected, roughness, reference, layer, viewModelScreen.probeCandidates.xy);
  vec3 radiance = sampleEnvironmentReflection(receiver);
  ScreenReflectionView screen = ScreenReflectionView(frame.camera, viewModelScreen.inverseCamera,
      frame.view, viewModelScreen.trace, viewModelScreen.config, frame.target.xy);
  vec4 hit = traceScreenReflection(screen, position, n, reflected, -v, roughness,
                                   gl_FragCoord.xy / vec2(frame.target.xy));
  radiance = resolveReflection(ReflectionSample(hit.rgb, hit.a), radiance);
  return radiance;
}
bool viewModelReflectionsDisabled() { return (frame.clusterInfo.w & 2u) == 0u; }
vec3 sampleViewModelReflection(vec3 position, vec3 n, vec3 v, float roughness, uint layer, LightingReference reference) {
  if (viewModelReflectionsDisabled()) return vec3(0);
  return viewModelReflectionRadiance(position, n, v, roughness, layer, reference);
}

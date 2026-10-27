#define PROBE_BINDING 12
#define PROBE_IMAGE_BINDING 19
#define PROBE_TILE_CANDIDATES
#include "../../shaders/probes.glsl"
layout(set = 0, binding = 13, std140) uniform ViewModelSky {
  mat4 inverseProjection, viewToDome;
  vec4 radiance, target;
} sky;
layout(set = 0, binding = 14) uniform sampler2D skyMap;
#include "../../shaders/environment-sampling.glsl"
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

vec3 viewModelReflection(vec3 position, vec3 n, vec3 v, vec3 base, float roughness, float metallic,
                         uint layer, float localLuma) {
  if ((frame.clusterInfo.w & 2u) == 0u) return vec3(0);
  vec3 reflected = reflect(-v, n), radiance;
  if (!boundProbeRadiance(layer, position + n * sky.target.z, reflected, roughness, n, localLuma, radiance) &&
      !probeRadianceMasked(position + n * sky.target.z, reflected, roughness,
                           viewModelScreen.probeCandidates.xy, n, localLuma, radiance))
    radiance = environmentReflection(position, n, reflected, roughness);
  ScreenReflectionView screen = ScreenReflectionView(frame.camera, viewModelScreen.inverseCamera,
      frame.view, viewModelScreen.trace, viewModelScreen.config, frame.target.xy);
  vec4 hit = traceScreenReflection(screen, position, n, reflected, -v, roughness,
                                   gl_FragCoord.xy / vec2(frame.target.xy));
  radiance = hit.rgb + (1 - hit.a) * radiance;
  vec3 f0 = mix(vec3(.04), base, metallic);
  vec3 fresnel = f0 + (max(vec3(1 - roughness), f0) - f0) * pow(1 - max(dot(n, v), 0), 5);
  return radiance * fresnel;
}

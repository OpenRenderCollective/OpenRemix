// Mirrors forward/lightmap/data.h. Each atlas page keeps indirect, visibility
// and per-light unit direct irradiance separate.
// GPU-only compaction can omit visibility and/or local direct coefficients.
// Retained direct coefficients start at (layer count - 10); sun is always last.
bool indexedSurfaceVisibility() { return (frame.occlusion.x & 0x80000000u) != 0u; }
bool indexedSurfaceDirect() { return frame.clusterInfo.z != 0u; }
#define SurfaceAtlasLayers (20.0 - (indexedSurfaceVisibility() ? 1.0 : 0.0) - (indexedSurfaceDirect() ? 9.0 : 0.0))
void surfaceBasis(vec3 normal, out float sh[9]) {
  sh = float[9](.2820947918, .4886025119 * normal.y, .4886025119 * normal.z, .4886025119 * normal.x,
                1.092548431 * normal.x * normal.y, 1.092548431 * normal.y * normal.z,
                .3153915653 * (3 * normal.z * normal.z - 1), 1.092548431 * normal.x * normal.z,
                .5462742153 * (normal.x * normal.x - normal.y * normal.y));
}
layout(set = 0, binding = 6) uniform sampler2DArray surfaceAtlas;
bool surfaceIrradiance(vec4 coordinate, vec3 normal, out vec4 result) {
  if (coordinate.w < .5)
    return false;
  float sh[9];
  surfaceBasis(normal, sh);
  result = vec4(0);
  for (uint coefficient = 0; coefficient < 9u; ++coefficient)
    result += textureLod(surfaceAtlas,
                         vec3(coordinate.xy, coordinate.z * SurfaceAtlasLayers + float(coefficient)), 0)
                  * sh[coefficient];
#ifdef STYLED_BAKE
  result.rgb += styledSurfaceIrradiance(coordinate, sh);
#endif
  result = max(result, vec4(0));
  // A valid black chart remains black; it must not fall through to probes.
  return true;
}

uint lightChannel(uvec4 owners, uint capturedLight) {
  if (capturedLight != 0u)
    for (uint channel = 0u; channel < 4u; ++channel)
      if (owners[channel] == capturedLight - 1u)
        return channel;
  return 0xffffffffu;
}
// Returning the ownership separately prevents sampling the cached static
// depth map on top of a baked soft visibility channel, including a fully lit one.
bool bakedLightVisibility(vec4 coordinate, uvec4 owners, uint capturedLight, out float visibility,
                          out uint selected) {
  visibility = 1;
  selected = 0xffffffffu;
  if (indexedSurfaceVisibility() || coordinate.w < .5 || capturedLight == 0u)
    return false;
  for (uint channel = 0; channel < 4u; ++channel)
    if (owners[channel] == capturedLight - 1u) {
      selected = channel;
      visibility = clamp(
          textureLod(surfaceAtlas, vec3(coordinate.xy, coordinate.z * SurfaceAtlasLayers + 9), 0)[channel], 0,
          1);
      return true;
    }
  return false;
}

vec4 surfaceDirectIrradiance(vec4 coordinate, vec3 normal) {
  if (indexedSurfaceDirect() || coordinate.w < .5 || frame.localSettings.w < .5)
    return vec4(-1);
  float sh[9];
  surfaceBasis(normal, sh);
  vec4 result = vec4(0);
  for (uint coefficient = 0; coefficient < 9u; ++coefficient)
    result +=
        textureLod(surfaceAtlas,
                   vec3(coordinate.xy, coordinate.z * SurfaceAtlasLayers + (SurfaceAtlasLayers - 10) + float(coefficient)), 0) *
        sh[coefficient];
  return max(result, vec4(0));
}

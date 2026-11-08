// Mirrors forward/lightmap/data.h. Each atlas page keeps indirect, visibility
// and per-light unit direct irradiance separate.
// GPU-only compaction can omit visibility and/or local direct coefficients.
// Retained direct coefficients start at (layer count - 10); sun is always last.
layout(constant_id = 17) const bool IndexedWorldLightmap = false;
bool indexedSurfaceVisibility() { return IndexedWorldLightmap || (frame.occlusion.x & 0x80000000u) != 0u; }
layout(constant_id = 2) const bool IndexedSurfaceDirectionals = false;
#include "receiver-coverage.glsl"
bool indexedSurfaceDirect() {
  return IndexedWorldLightmap || (IndexedSurfaceDirectionals && frame.clusterInfo.z != 0u);
}
const float SurfaceCoefficientCount = 9.0;
float surfaceAtlasLayers() {
  return SurfaceCoefficientCount + 1.0 +
      (indexedSurfaceVisibility() ? 0.0 : 1.0) +
      (indexedSurfaceDirect() ? 0.0 : SurfaceCoefficientCount);
}
float surfaceSunLayer(float page) {
  float layers = surfaceAtlasLayers();
  return page * layers + (layers - 1.0);
}
#include "irradiance-basis.glsl"
layout(set = 0, binding = 6) uniform sampler2DArray surfaceAtlas;
#include "lightmap-filter.glsl"
LightmapFilter surfaceFilter(vec4 coordinate, vec4 bounds) {
  return lightmapFilter(coordinate.xy, bounds, vec2(textureSize(surfaceAtlas, 0).xy));
}
vec4 sampleSurfaceAtlas(LightmapFilter stencil, float layer) {
  if (!BicubicLightmaps) return textureLod(surfaceAtlas, vec3(stencil.uv.xy, layer), 0);
  vec4 result = vec4(0);
  for (uint tap = 0u; tap < 4u; ++tap)
    result += lightmapWeight(stencil, tap) * textureLod(surfaceAtlas, vec3(lightmapTap(stencil, tap), layer), 0);
  return result;
}
#ifdef STYLED_BAKE
void addSparseStyleSurface(vec4 coordinate, LightmapFilter stencil, float sh[9],
                           inout vec3 irradiance) {
  StyledSurfaceLayout styles;
  if (!styledSurfaceLayout(coordinate, styles)) return;
  if (BicubicLightmaps) {
    for (uint tap = 0u; tap < 4u; ++tap)
      irradiance += lightmapWeight(stencil, tap) *
          styledSurfaceIrradiance(lightmapTap(stencil, tap), styles, sh);
  } else irradiance += styledSurfaceIrradiance(coordinate.xy, styles, sh);
}
#endif
vec4 sampleSurfaceCoefficients(LightmapFilter stencil, float firstLayer, float sh[9]) {
  vec4 result = vec4(0);
  for (uint coefficient = 0; coefficient < 9u; ++coefficient) {
    // Axis-aligned receivers have several exactly-zero SH weights. Their
    // finite atlas values cannot contribute; avoid four filtered fetches.
    // This is exact, not a threshold that discards weak lighting.
    if (sh[coefficient] == 0.0) continue;
    result += sampleSurfaceAtlas(stencil, firstLayer + float(coefficient)) * sh[coefficient];
  }
  return result;
}
bool hasSurfaceIrradiance(vec4 coordinate) {
  return !UnmappedReceiver && (FullyMappedReceiver || !(coordinate.w < .5));
}
bool surfaceIrradiance(vec4 coordinate, vec3 normal, vec4 bounds, out vec4 result) {
  if (!hasSurfaceIrradiance(coordinate)) return false;
  float sh[9];
  irradianceBasis(normal, sh);
  LightmapFilter stencil = surfaceFilter(coordinate, bounds);
  float firstLayer = coordinate.z * surfaceAtlasLayers();
  result = sampleSurfaceCoefficients(stencil, firstLayer, sh);
#ifdef STYLED_BAKE
  addSparseStyleSurface(coordinate, stencil, sh, result.rgb);
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
bool bakedLightVisibility(vec4 coordinate, vec4 bounds, uvec4 owners, uint capturedLight, out float visibility) {
  visibility = 1;
  if (indexedSurfaceVisibility() || coordinate.w < .5)
    return false;
  uint channel = lightChannel(owners, capturedLight);
  if (channel >= 4u)
    return false;
  float layer = coordinate.z * surfaceAtlasLayers() + SurfaceCoefficientCount;
  visibility = clamp(sampleSurfaceAtlas(surfaceFilter(coordinate, bounds), layer)[channel], 0, 1);
  return true;
}

vec4 surfaceDirectIrradiance(vec4 coordinate, vec3 normal, vec4 bounds) {
  if (indexedSurfaceDirect() || coordinate.w < .5 || frame.localSettings.w < .5)
    return vec4(-1);
  float sh[9];
  irradianceBasis(normal, sh);
  LightmapFilter stencil = surfaceFilter(coordinate, bounds);
  float layers = surfaceAtlasLayers();
  float firstLayer = coordinate.z * layers + (layers - (SurfaceCoefficientCount + 1.0));
  return max(sampleSurfaceCoefficients(stencil, firstLayer, sh), vec4(0));
}

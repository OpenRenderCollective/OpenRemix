#include "../../shaders/material-common.glsl"
int wrapTexel(int x, int size, uint mode) {
  if (mode == 3u)
    return clamp(x, 0, size - 1);
  if (mode == 4u)
    return x < 0 || x >= size ? -1 : x;
  if (mode == 2u) {
    int p = ((x % (2 * size)) + 2 * size) % (2 * size);
    return p < size ? p : 2 * size - 1 - p;
  }
  return ((x % size) + size) % size;
}
vec4 atlasTexel(uint layer, ivec2 pixel, uvec2 wrap) {
  ivec2 size = textureSize(rayAtlas, 0).xy;
  pixel = ivec2(wrapTexel(pixel.x, size.x, wrap.x), wrapTexel(pixel.y, size.y, wrap.y));
  if (any(lessThan(pixel, ivec2(0))))
    return vec4(0);
  return texelFetch(rayAtlas, ivec3(pixel, layer), 0);
}
vec4 sampleMap(uint layer, vec2 uv, uvec3 policy) {
  if (layer == 0u)
    return vec4(1);
  vec2 p = uv * vec2(textureSize(rayAtlas, 0).xy);
  if (policy.z == 0u)
    return atlasTexel(layer, ivec2(floor(p)), policy.xy);
  p -= .5;
  ivec2 base = ivec2(floor(p));
  vec2 f = fract(p);
  return mix(
      mix(atlasTexel(layer, base, policy.xy), atlasTexel(layer, base + ivec2(1, 0), policy.xy), f.x),
      mix(atlasTexel(layer, base + ivec2(0, 1), policy.xy), atlasTexel(layer, base + 1, policy.xy), f.x),
      f.y);
}
void attributes(uint primitive, vec2 bary, out vec2 uv, out vec4 color, out vec3 normal, out vec3 geometric) {
  uvec4 t = rayTriangles[primitive];
  RayVertex a = rayVertices[t.x], b = rayVertices[t.y], c = rayVertices[t.z];
  vec3 w = vec3(1 - bary.x - bary.y, bary);
  uv = a.uv.xy * w.x + b.uv.xy * w.y + c.uv.xy * w.z;
  color = a.color * w.x + b.color * w.y + c.color * w.z;
  geometric = normalize(cross(b.position.xyz - a.position.xyz, c.position.xyz - a.position.xyz));
  normal = a.normal.xyz * w.x + b.normal.xyz * w.y + c.normal.xyz * w.z;
  normal = dot(normal, normal) > 1e-10 ? normalize(normal) : geometric;
}
vec4 baseColor(RayMaterial m, vec2 uv, vec4 vertex) {
  return surfaceColorValues(m.shading.state, m.shading.factor, m.shading.tint,
                            sampleMap(m.textures.x, uv, m.flags.xyz), vertex);
}
float random(inout uint state);
bool covered(uint primitive, vec2 bary, bool front, uint seed) {
  RayMaterial m = rayMaterials[rayTriangles[primitive].w];
  if ((m.shading.state.w & 2u) == 0u && !front)
    return false;
  vec2 uv;
  vec4 vertex;
  vec3 n, g;
  attributes(primitive, bary, uv, vertex, n, g);
  const float alpha = baseColor(m, uv, vertex).a;
  if (!alphaPass(m.shading.state.y, uintBitsToFloat(m.shading.state.z), alpha))
    return false;
  // Thin source-over sheets represent coverage, not refraction. Independent
  // candidate decisions give proportional transmission through stacked sheets.
  // Candidate traversal order is unspecified. Derive a local decision from the
  // ray stream and primitive identity, without advancing the path's RNG for
  // whichever candidate happens to be visited first. Distinct sheets still use
  // independent coverage samples; opaque/cutout candidates consume none.
  seed ^= (primitive + 1u) * 2654435761u;
  return (m.flags.w & 2u) == 0u || random(seed) < clamp(alpha, 0, 1);
}

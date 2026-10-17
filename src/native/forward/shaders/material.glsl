layout(set = 1, binding = 0) uniform sampler2D albedoMap;
layout(set = 1, binding = 1) uniform sampler2D normalMap;
layout(set = 1, binding = 2) uniform sampler2D roughnessMap;
layout(set = 1, binding = 3) uniform sampler2D metallicMap;
layout(set = 1, binding = 4) uniform sampler2D emissionMap;
#include "material-common.glsl"
vec4 surfaceColor(Material m, vec2 uv, vec4 vertexColor) {
  return surfaceColorValues(m.state, m.factor, m.tint, texture(albedoMap, uv), vertexColor);
}
void alphaTest(Material m, float alpha) {
  if (!alphaPass(m.state.y, uintBitsToFloat(m.state.z), alpha)) discard;
}
vec3 tangentNormal(vec3 local, vec2 uv, vec3 p, vec3 n, vec3 v) {
  vec3 dx = dFdx(p), dy = dFdy(p);
  vec2 tx = dFdx(uv), ty = dFdy(uv);
  float det = tx.x * ty.y - tx.y * ty.x;
  if (abs(det) < 1e-10 || dot(local, local) < 1e-10)
    return n;
  vec3 t = (dx * ty.y - dy * tx.y) / det;
  t -= n * dot(n, t);
  if (dot(t, t) < 1e-10)
    return n;
  t = normalize(t);
  vec3 b = cross(n, t);
  if (dot(b, (dy * tx.x - dx * ty.x) / det) < 0)
    b = -b;
  vec3 result = normalize(mat3(t, b, n) * vec3(local.xy, max(local.z, .001)));
  return dot(result, v) > 1e-4 ? result : n;
}
vec3 mappedNormal(Material m, vec2 uv, vec3 p, vec3 n, vec3 v) {
  if ((m.maps.x & 4u) == 0u)
    return n;
  vec3 local = texture(normalMap, uv).xyz;
  if (m.maps.y == 4u)
    local =
        mat3(.81649658, 0, .57735027, -.40824829, .70710678, .57735027, -.40824829, -.70710678, .57735027) *
        local;
  else
    local = local * 2 - 1;
  if (m.maps.y == 1u) {
    local.xy = vec2(local.x + local.y, local.x - local.y) * .5;
    local.z = 1 - abs(local.x) - abs(local.y);
  }
  if ((m.maps.y == 2u) || (m.maps.y == 3u) || (m.pbr.z > 0))
    local.z = sqrt(max(0, 1 - dot(local.xy, local.xy)));
  if (m.maps.y == 3u)
    local.y = -local.y;
  return tangentNormal(local, uv, p, n, v);
}

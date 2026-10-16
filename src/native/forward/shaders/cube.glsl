#ifndef FORWARD_CUBE_GLSL
#define FORWARD_CUBE_GLSL
vec3 cubeDirection(uint face, vec2 uv) {
  if (face == 0u)
    return vec3(1, -uv.y, -uv.x);
  if (face == 1u)
    return vec3(-1, -uv.y, uv.x);
  if (face == 2u)
    return vec3(uv.x, 1, uv.y);
  if (face == 3u)
    return vec3(uv.x, -1, -uv.y);
  if (face == 4u)
    return vec3(uv.x, -uv.y, 1);
  return vec3(-uv.x, -uv.y, -1);
}
vec3 cubeProjection(vec3 d) {
  vec3 a = abs(d);
  if (a.x >= a.y && a.x >= a.z)
    return vec3(d.x >= 0 ? vec2(-d.z, -d.y) / a.x : vec2(d.z, -d.y) / a.x, d.x >= 0 ? 0 : 1);
  if (a.y >= a.z)
    return vec3(d.y >= 0 ? vec2(d.x, d.z) / a.y : vec2(d.x, -d.z) / a.y, d.y >= 0 ? 2 : 3);
  return vec3(d.z >= 0 ? vec2(d.x, -d.y) / a.z : vec2(-d.x, -d.y) / a.z, d.z >= 0 ? 4 : 5);
}
#endif

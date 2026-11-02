// remixapi_MaterialGradientMapEXT and remixapi_InstanceInfoWoundCutEXT in the
// raster stages (raster.vert / raster.frag with INFECTED; raster/infected.cpp
// packs the parameters). The same composite and cut as the path tracer's
// (pathtrace/shaders/scene.glsl gradientAlbedo, woundVertex, woundValue).
#ifndef INFECTED_VERTEX_ONLY
layout(set=1,binding=0) uniform sampler2D gradientMap;
layout(set=1,binding=1) uniform sampler2D detailMap;
layout(set=1,binding=2) uniform sampler2D cutMap;
#endif
layout(set=1,binding=3,std140) uniform Infected {
  vec4 ellipsoid[6]; // the first ellipsoid's rows (axis, -axis . center), then the second's
  vec4 projection[2]; // scale, bias
  vec4 cut[2];        // each half's scale-bias
  vec4 wound;         // mapping scale, second blend, threshold, flags (bits: 1 wound, 2 gradient, 4 detail, 8 sRGB gradient)
  vec4 rows;          // skin row, cloth row
  vec4 sprite;        // sprite offset, blood range
  vec4 detail;        // detail U, detail V
  vec4 blood;         // blood colour
} infected;
vec4 woundVertex(vec3 p, out float h, out uint missing) {
  vec4 x = vec4(p, 1.0);
  vec3 e1 = vec3(dot(infected.ellipsoid[0], x), dot(infected.ellipsoid[1], x), dot(infected.ellipsoid[2], x));
  vec3 e2 = vec3(dot(infected.ellipsoid[3], x), dot(infected.ellipsoid[4], x), dot(infected.ellipsoid[5], x));
  float m = infected.wound.x, k = infected.wound.y;
  vec3 el = mix(e1, e2, k);
  vec4 q = vec4(0);
  missing = 0u;
  if (abs(e1.z) < 1.0) q.xy = e1.xy / (sqrt(1.0 - e1.z * e1.z) * m); else missing |= 1u;
  if (abs(el.z) < 1.0) q.zw = el.xy / (sqrt(1.0 - el.z * el.z) * m); else missing |= 2u;
  h = length(e2) + k;
  return q * infected.projection[0] + infected.projection[1];
}
#ifndef INFECTED_VERTEX_ONLY
float woundValue(vec4 w, uint missing) {
  float value = 0.0;
  if ((missing & 1u) == 0u)
    value = textureLod(cutMap, clamp(w.xy, 0.0, 1.0) * infected.cut[0].xy + infected.cut[0].zw, 0.0).r;
  if ((missing & 2u) == 0u)
    value = max(value, textureLod(cutMap, clamp(w.zw, 0.0, 1.0) * infected.cut[1].xy + infected.cut[1].zw, 0.0).r);
  return value;
}
// uv: the transformed texture coordinate (uv x transform.xy + transform.zw);
// the albedo texture is read as stored, its sprite at the mesh's uv / 2 +
// offset through the same transform.
vec4 gradientAlbedo(vec2 uv, vec4 transform, float wound) {
  vec4 b = texture(albedo, uv);
  vec2 mesh = (uv - transform.zw) / transform.xy;
  vec4 sprite = texture(albedo, (mesh * 0.5 + infected.sprite.xy) * transform.xy + transform.zw);
  uint flags = floatBitsToUint(infected.wound.w);
  float l = b.a * b.a, height = float(textureSize(gradientMap, 0).y);
  vec3 skin = textureLod(gradientMap, vec2(b.a, (infected.rows.x + 0.5) / height), 0.0).rgb;
  vec3 cloth = textureLod(gradientMap, vec2(b.a, (infected.rows.y + 0.5) / height), 0.0).rgb;
  if ((flags & 8u) != 0u) {
    skin = pow(max(skin, vec3(0)), vec3(2.2));
    cloth = pow(max(cloth, vec3(0)), vec3(2.2));
  }
  vec3 c = mix(vec3(l), skin, clamp(2.0 * b.b - 1.0, 0.0, 1.0));
  c = mix(c, cloth, clamp(1.0 - 2.0 * b.b, 0.0, 1.0));
  if ((flags & 4u) != 0u) {
    vec3 d = texture(detailMap, vec2(dot(infected.detail.xy, mesh), dot(infected.detail.zw, mesh))).rgb;
    c = mix(c, 2.0 * l * d, clamp(2.0 * sprite.r - 1.0, 0.0, 1.0));
  }
  float s = smoothstep(infected.sprite.z, infected.sprite.w, clamp(2.0 * sprite.g - 1.0, 0.0, 1.0) + wound);
  return vec4(min(c * mix(vec3(1), infected.blood.rgb, s), vec3(1)), 1.0);
}
#endif

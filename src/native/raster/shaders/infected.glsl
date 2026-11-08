// remixapi_MaterialGradientMapEXT and remixapi_InstanceInfoWoundCutEXT in the
// raster stages (raster.vert / raster.frag with INFECTED; raster/infected.cpp
// packs the parameters). The same composite and cut as the path tracer's
// (pathtrace/shaders/scene.glsl gradientAlbedo, woundVertex, woundValue).
#ifndef INFECTED_VERTEX_ONLY
layout(set=1,binding=0) uniform sampler2D gradientMap;
layout(set=1,binding=1) uniform sampler2D detailMap;
layout(set=1,binding=2) uniform sampler2D cutMap;
layout(set=1,binding=4) uniform sampler2D burnMap;
#endif
layout(set=1,binding=3,std140) uniform Infected {
  vec4 ellipsoid[6]; // the first ellipsoid's rows (axis, -axis . center), then the second's
  vec4 projection[2]; // scale, bias
  vec4 cut[2];        // each half's scale-bias
  vec4 wound;         // mapping scale, second blend, threshold, flags (bits: 1 wound, 2 gradient, 4 detail,
                      // 8 sRGB gradient or ramp, 16 burn, 32 sRGB burn, 64 portal rim)
  vec4 rows;          // skin row, cloth row, burn strength; a portal rim's colour scale, noise scroll
  vec4 sprite;        // sprite offset, blood range
  vec4 detail;        // detail U, detail V
  vec4 blood;         // blood colour; a portal rim's emission
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
  c = min(c * mix(vec3(1), infected.blood.rgb, s), vec3(1));
  // The burn, at the detail coordinate swapped.
  if ((flags & 16u) != 0u) {
    vec4 burn = texture(burnMap, vec2(dot(infected.detail.zw, mesh), dot(infected.detail.xy, mesh)));
    if ((flags & 32u) != 0u)
      burn.rgb = pow(max(burn.rgb, vec3(0)), vec3(2.2));
    float t = clamp(infected.rows.z * (2.0 - clamp(2.0 * b.b - 1.0, 0.0, 1.0)) + burn.a - 1.0, 0.0, 1.0);
    c = mix(c, burn.rgb, t);
  }
  return vec4(c, 1.0);
}
#endif
#ifndef INFECTED_VERTEX_ONLY
// remixapi_MaterialPortalRimEXT (flags bit 64), as the path tracer's
// portalRim (pathtrace/shaders/scene.glsl): the noise is the draw's albedo
// texture (as stored), the ramp in gradientMap. mesh: the mesh's texture
// coordinate; vertex: the vertex colour as stored (the opening in red, the
// static in green). Colour and coverage (source alpha over).
vec4 portalRimColour(vec2 mesh, vec4 vertex) {
  float open = clamp(vertex.r, 0.0, 1.0), stat = clamp(vertex.g, 0.0, 1.0);
  vec2 c = mesh * 1.075 - 0.0375;
  float d = length(2.0 * c - 1.0), k = open * open * (3.0 - 2.0 * open), r = k * k;
  bool inside = d <= r;
  float e = inside ? clamp((d - r + 0.3) / 0.3, 0.0, 1.0) : 1.0 - clamp((d - r) * 13.3333, 0.0, 1.0);
  float edge = e * max(clamp(2.5 * k, 0.0, 1.0), stat);
  float scale = 0.3 / clamp(open + 0.001, 0.0, 1.0), shift = infected.rows.y;
  vec2 w = (mesh - 0.5) * scale + 0.15, a = w + vec2(shift, 0.0), b = w - vec2(shift, 0.0);
  vec2 n1 = texture(albedo, a).rg;
  vec2 n2 = texture(albedo, b.yx - 0.02 * n1).rg;
  vec2 n3 = texture(albedo, a - 0.02 * n2).rg;
  float h = 0.5 * (n2.g + n3.g);
  float p = clamp((h - edge + 0.875) / 1.75, 0.0, 1.0);
  p = 1.0 - p * p * (3.0 - 2.0 * p);
  float q = clamp((1.0 - stat) / max(h, 1e-6), 0.0, 1.0);
  q = 1.0 - q * q * (3.0 - 2.0 * q);
  float level = clamp(edge * p + (inside ? q : 0.0), 0.0, 1.0);
  float x = 1.5 * level * sqrt(p) * (0.8 * pow(abs(c.y), 1.5) + 0.2);
  vec3 colour = textureLod(gradientMap, vec2(x, 0.5), 0.0).rgb;
  if ((floatBitsToUint(infected.wound.w) & 8u) != 0u)
    colour = pow(max(colour, vec3(0)), vec3(2.2));
  return vec4(colour * infected.rows.x * infected.blood.rgb, clamp(1.5 * level, 0.0, 1.0));
}
#endif

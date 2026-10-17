vec3 waterNormal(Material m, WaterMaterial water, vec2 uv, vec3 p, vec3 n, vec3 v) {
  if (m.maps.y != 5u) return mappedNormal(m, uv, p, n, v);
  if ((m.maps.x & 4u) == 0u) return n;
  float seconds = water.optical.y;
  vec3 local = texture(normalMap, uv + water.scroll01.xy * seconds).xyz;
  if (water.optical.z > 1.5)
    local = (local + texture(normalMap, vec2(uv.x + uv.y, uv.y - uv.x) * .1 + water.scroll01.zw * seconds).xyz +
             texture(normalMap, uv.yx * .45 + water.scroll2.xy * seconds).xyz) / 3;
  local = local * 2 - 1;
  if (m.pbr.z > 0) local.z = sqrt(max(0, 1 - dot(local.xy, local.xy)));
  return tangentNormal(local, uv, p, n, v);
}

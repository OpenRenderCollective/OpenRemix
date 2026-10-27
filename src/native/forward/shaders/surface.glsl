// Evaluated material values. No lighting, exposure, receiver lookup or GPU
// material-table layout belongs in this contract.
struct Surface {
  vec4 color;
  vec3 emission;
  vec3 normal;
  float roughness;
  float metallic;
};

// Compatibility decoder for the standard PBR material. Alternative material
// implementations can produce Surface values without knowing lighting storage.
Surface evaluateStandardSurface(Material material, vec4 color, vec2 texcoord,
                                vec3 position, vec3 normal, vec3 geometric,
                                vec3 viewDirection, bool useNormalMap) {
  Surface surface;
  vec4 emissionTexel = (material.maps.x & 8u) != 0u ? texture(emissionMap, texcoord) : vec4(1);
  float reflected;
  emissionLobes(material.emission, material.state, color, texture(albedoMap, texcoord),
                emissionTexel, surface.emission, reflected);
  surface.color = vec4(color.rgb * reflected, color.a);
  surface.normal = dot(normal, normal) > 1e-10 ? normalize(normal) : normalize(geometric);
  if (dot(surface.normal, viewDirection) < 0) surface.normal = -surface.normal;
  if (useNormalMap)
    surface.normal = mappedNormal(material, texcoord, position, surface.normal, viewDirection);
  surface.roughness = (material.maps.x & 1u) != 0u
      ? mapValue(texture(roughnessMap, texcoord), material.remap[0], material.maps.z) : material.pbr.x;
  surface.metallic = (material.maps.x & 2u) != 0u
      ? mapValue(texture(metallicMap, texcoord), material.remap[1], material.maps.w) : material.pbr.y;
  // Filter specular at normal-map frequencies MSAA alone cannot sample.
  surface.roughness = clamp(sqrt(surface.roughness * surface.roughness + min(.25,
      dot(dFdx(surface.normal), dFdx(surface.normal)) + dot(dFdy(surface.normal), dFdy(surface.normal)))), .04, 1);
  return surface;
}

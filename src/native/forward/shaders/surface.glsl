#include "surface-values.glsl"
layout(constant_id = 19) const bool RoughDielectricMaterial = false;

// Compatibility decoder for the standard PBR material. Alternative material
// implementations can produce Surface values without knowing lighting storage.
Surface evaluateStandardSurface(Material material, vec4 color, vec4 albedo, vec2 texcoord,
                                vec3 positionDx, vec3 positionDy, vec3 normal, vec3 geometric,
                                vec3 viewDirection, bool useNormalMap) {
  Surface surface;
  surface.directSpecular = (material.maps.x & 128u) == 0u;
  vec4 emissionTexel = (material.maps.x & 8u) != 0u
      ? decodeColorTexel(texture(emissionMap, texcoord), (material.maps.x & 64u) != 0u) : vec4(1);
  float reflected;
  emissionLobes(material.emission, material.state, color, albedo,
                emissionTexel, surface.emission, reflected);
  surface.color = vec4(color.rgb * reflected, color.a);
  surface.normal = dot(normal, normal) > 1e-10 ? normalize(normal) : normalize(geometric);
  // Orient the smoothed normal with the visible side of the triangle. Its
  // own view dot can cross zero at grazing angles while the face stays visible.
  float faceSide = dot(geometric, viewDirection);
  bool flipNormal = faceSide == 0 ? dot(surface.normal, viewDirection) < 0
                                  : (dot(surface.normal, geometric) < 0) != (faceSide < 0);
  if (flipNormal) surface.normal = -surface.normal;
  if (useNormalMap)
    surface.normal = mappedNormal(material, texcoord, positionDx, positionDy, surface.normal, viewDirection);
  if (RoughDielectricMaterial) {
    // The selected draw has scalar roughness 1 and metallic 0. The
    // existing variance term can only increase roughness before clamping.
    surface.roughness = 1;
    surface.metallic = 0;
  } else {
    surface.roughness = (material.maps.x & 1u) != 0u
        ? mapValue(texture(roughnessMap, texcoord), material.remap[0], material.maps.z) : material.pbr.x;
    surface.metallic = (material.maps.x & 2u) != 0u
        ? mapValue(texture(metallicMap, texcoord), material.remap[1], material.maps.w) : material.pbr.y;
    // Filter specular at normal-map frequencies MSAA alone cannot sample.
    vec3 normalDx = dFdx(surface.normal), normalDy = dFdy(surface.normal);
    float normalVariance = min(.25, dot(normalDx, normalDx) + dot(normalDy, normalDy));
    surface.roughness = clamp(sqrt(surface.roughness * surface.roughness + normalVariance), .04, 1);
  }
  return surface;
}

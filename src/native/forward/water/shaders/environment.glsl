// Default environment source's water fallback. The optical material supplies
// unfiltered sky radiance for transmission too; baked reflection visibility
// belongs to this source and is not required by replacement environments.
#include "../../shaders/baked-storage.glsl"
vec3 environmentReflection(vec3 position, vec3 normal, vec3 reflected, float roughness) {
  return waterSkyRadiance(reflected) * bakedSkyVisibility(position, normal, reflected);
}

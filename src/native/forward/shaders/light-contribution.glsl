// Scene-linear radiance after source visibility and material response. Unlike
// BrdfLobes, these values can be accumulated directly into the shaded result.
// Diffuse can be a negative mixed-shadow correction; clamp only the final sum.
struct LightContribution {
  vec3 diffuse;
  vec3 specular;
};

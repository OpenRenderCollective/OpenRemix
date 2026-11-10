#ifndef OPENREMIX_SURFACE_VALUES
#define OPENREMIX_SURFACE_VALUES

// Evaluated material values. No lighting, exposure, receiver lookup or GPU
// material-table layout belongs in this contract.
struct Surface {
  vec4 color;
  vec3 emission;
  vec3 normal;
  float roughness;
  float metallic;
};

// Safe evaluated-material defaults, independent of pass resources. The caller
// supplies a normalized world shading normal; geometry/lighting stay in Receiver.
Surface initSurface(vec3 normal) {
  Surface surface;
  surface.color = vec4(1);
  surface.emission = vec3(0);
  surface.normal = normal;
  surface.roughness = 1;
  surface.metallic = 0;
  return surface;
}
Surface initSurface() { return initSurface(vec3(0, 0, 1)); }

#endif

#ifndef OPENREMIX_BRDF_VALUES
#define OPENREMIX_BRDF_VALUES
// Material response values, independent of any particular BRDF implementation.
const float Pi = 3.14159265359;
struct BrdfLobes {
  vec3 diffuse;  // BRDF weight; multiply by irradiance or analytic NdotL.
  vec3 specular; // Analytic GGX including NdotL.
};
#endif

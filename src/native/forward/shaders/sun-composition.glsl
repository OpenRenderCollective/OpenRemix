#ifndef OPENREMIX_SUN_COMPOSITION
#define OPENREMIX_SUN_COMPOSITION
#include "sun-values.glsl"
LightContribution composeSunLight(SunLightSample lighting, BrdfLobes response) {
  return LightContribution(response.diffuse * lighting.diffuseIrradiance * lighting.radiance,
                            response.specular * lighting.staticVisibility * lighting.radiance);
}
#endif

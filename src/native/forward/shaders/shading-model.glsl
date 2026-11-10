#ifndef OPENREMIX_SHADING_MODEL
#define OPENREMIX_SHADING_MODEL
// Compile-selected material response. A custom model supplies shadeSurface with
// the same Surface/Receiver/ShadingResult contract, leaving the pass unchanged.
// Bind the default name before compilation; no wrapper call is generated.
#include "standard-shading.glsl"
#define shadeSurface shadeStandardSurface
#endif

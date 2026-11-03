#ifndef OPENREMIX_BAKED_STORAGE
#define OPENREMIX_BAKED_STORAGE
// Shared Source 2 surface/probe storage. Only sources that query these fields
// import it; material passes and replacement providers need not declare it.
#define STYLED_BAKE
#include "irradiance.glsl"
#include "style-lighting.glsl"
#include "lightmap.glsl"
#endif

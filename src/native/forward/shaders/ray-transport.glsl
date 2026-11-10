#ifndef OPENREMIX_FRAGMENT_RAY_TRANSPORT
#define OPENREMIX_FRAGMENT_RAY_TRANSPORT
// Shared implementation for incoming diffuse and water reflection rays. Sources
// choose their descriptor set before importing; the material pass owns neither.
#ifndef RAY_SCENE_SET
#define RAY_SCENE_SET 2
#endif
#define RAY_FRAGMENT 1
#include "../bake/shaders/bindings.glsl"
#include "../bake/shaders/material.glsl"
#include "../bake/shaders/radiance.glsl"
#endif

#ifndef WATER_PARAMETERS_GLSL
#define WATER_PARAMETERS_GLSL
#ifndef WATER_PARAMETERS_SET
#define WATER_PARAMETERS_SET 2
#endif
#ifndef WATER_PARAMETERS_BINDING
#define WATER_PARAMETERS_BINDING 2
#endif
layout(set = WATER_PARAMETERS_SET, binding = WATER_PARAMETERS_BINDING, std140) uniform WaterFrame {
  mat4 projection;
  vec4 cameraMedium, target, settings, domeUp, sunFog;
} waterFrame;
#endif

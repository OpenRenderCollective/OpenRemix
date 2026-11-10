#include "shading-values.glsl"
// Receiver metadata is independent of material properties. These values are
// prepared by the pass adapter, not by user material code.
struct BakedReceiver {
  vec4 coordinate;
  vec4 bounds;
  uvec4 lights;
  uvec4 directLights;
  uint chart;
};
struct ShadingReceiver {
  vec3 position;
  vec3 geometric;
  vec3 viewDirection;
  vec2 screenPosition;
  float depth;
  BakedReceiver baked;
  bool directChannels;
  uint reflectionLayer;
#ifdef INLINE_WORLD_REFLECTIONS
  uvec2 reflectionCandidates;
#endif
  float opacity;
};

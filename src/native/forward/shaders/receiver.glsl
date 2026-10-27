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
  float opacity;
};
struct ShadingResult {
  vec4 color;
  vec4 normalRoughness;
  vec4 reflectance;
};

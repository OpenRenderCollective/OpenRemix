#include "light-list.glsl"

// Direct illumination provider shared by world and viewmodel materials.
// Baked visibility, mixed shadows and analytic specular remain owned by the
// existing sun/local implementations. No exposure or emission belongs here.
vec3 directLightComponent(BrdfLobes light, uint view) {
  if (view == 2u || view == 7u) return light.diffuse;
  if (view == 3u) return light.specular;
  return light.diffuse + light.specular;
}
void accumulateDirectLighting(inout vec3 color, Surface surface, ShadingReceiver receiver,
                            float bakedSunVisibility, bool aggregate, uint debugView) {
  vec3 n = surface.normal, v = receiver.viewDirection;
  vec4 coordinate = receiver.baked.coordinate, bounds = receiver.baked.bounds;
  BrdfLobes sun = sunLightResponse(coordinate, bounds, bakedSunVisibility, receiver.position, receiver.depth, n, v,
      surface.color.rgb, surface.roughness, surface.metallic, normalize(receiver.geometric));
  color += directLightComponent(sun, debugView);
  bool directChannels = receiver.directChannels && !indexedSurfaceDirect() && surface.metallic < 1 &&
      frame.target.z > 0u && any(notEqual(receiver.baked.directLights, uvec4(0xffffffffu)));
  vec4 directIrradiance = directChannels ? surfaceDirectIrradiance(coordinate, n, bounds) : vec4(-1);
  LocalLightList localLights = localLightsForReceiver(receiver, aggregate);
  for (uint i = 0u; i < localLights.count; ++i) {
    uint index = localLightIndex(localLights, i);
    BrdfLobes local = localLightResponse(index, receiver.position, n, v, surface.color.rgb,
        surface.roughness, surface.metallic, directIrradiance, receiver.baked, vec3(.04),
        normalize(receiver.geometric), aggregate);
    color += directLightComponent(local, debugView);
  }
}

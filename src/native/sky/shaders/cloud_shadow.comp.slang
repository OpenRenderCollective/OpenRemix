// The clouds' transmittance toward the sun around the camera, for the path
// tracer's sun (pathtrace/shaders/scene.glsl, cloudShadow): horizontal slices
// of a world-space box, side by side in one texture, each slice's texel the
// map NUMOS bakes for its own lights (sampleCloudGroundShadowFromMap).
[[vk::binding(0)]] Texture2D AtmosphereTransmittanceLut;
[[vk::binding(1)]] Texture2D AtmosphereMultiscatteringLut;
[[vk::binding(2)]] Texture2D AtmosphereSkyViewLut;
[[vk::binding(3)]] SamplerState AtmosphereSkyViewSampler;
[[vk::binding(4)]] Texture2D<float4> AtmosphereCloudSkyTransmittanceLut;
[[vk::binding(5)]] Texture3D<float2> AtmosphereCloudShadowMap;
[[vk::binding(6)]] SamplerState AtmosphereCloudNoise3DSampler;
[[vk::binding(7)]] Texture2D<float4> AtmosphereCloudSecondaryLut;
[[vk::binding(8)]] Texture2DArray BlueNoise;
#define ATMOSPHERE_AVAILABLE

#include "numos/host/numos_host.slangh"

[[vk::binding(9)]] ConstantBuffer<AtmosphereArgs> atmosphereArgs;
[[vk::binding(10)]] [[vk::image_format("r16f")]] RWTexture2D<float> cloudShadow;

struct CloudShadowPush
{
  // A texel's world position: origin + (u, v, slice) along the three axes,
  // u and v in [0, 1] across a slice and slice in [0, 1] from the lowest.
  float4 origin;
  float4 axisU;
  float4 axisV;
  float4 axisSlice;
  uint size;   // texels along a slice's side
  uint slices;
  uint zUp;
  uint reserved;
};
[[vk::push_constant]] ConstantBuffer<CloudShadowPush> push;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
  if (pixel.x >= push.size * push.slices || pixel.y >= push.size)
  {
    return;
  }
  const uint slice = pixel.x / push.size;
  const vec2 uv = (vec2(pixel.x % push.size, pixel.y) + 0.5f) / float(push.size);
  const float height = push.slices > 1u ? float(slice) / float(push.slices - 1u) : 0.0f;
  const vec3 world = push.origin.xyz + uv.x * push.axisU.xyz + uv.y * push.axisV.xyz + height * push.axisSlice.xyz;
  const AtmosphereArgs args = atmosphereArgs;
  cloudShadow[pixel] = sampleCloudGroundShadowFromMap(world, args.sunDirection, args, push.zUp != 0u,
                                                      AtmosphereCloudShadowMap, AtmosphereCloudNoise3DSampler);
}

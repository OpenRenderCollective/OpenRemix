// NUMOS's sky for rays that miss the scene, baked into the equirectangular
// environment the path tracer samples like a dome (pathtrace/shaders/scene.glsl,
// environmentRadiance): secondary rays, clouds from the dome LUT, and no sun or
// moon disks, since the sun is the scene's distant light.
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
[[vk::binding(22)]] StructuredBuffer<uint> NightSkyStarCells;
[[vk::binding(23)]] StructuredBuffer<float4> NightSkyStars;
[[vk::binding(24)]] StructuredBuffer<uint> NightSkyMilkyWay;
[[vk::binding(25)]] Texture2D<float4> NightSkyStarlightLow;
[[vk::binding(26)]] Texture2D<float4> NightSkyMoon;
#define NIGHT_SKY_AVAILABLE
// ATMOSPHERE_TRANSMITTANCE_LUT_AVAILABLE stays undefined, which leaves out the
// planet's ground on purpose: game terrain or nothing covers the lower half.

#include "numos/host/numos_host.slangh"

[[vk::binding(9)]] ConstantBuffer<AtmosphereArgs> atmosphereArgs;
[[vk::binding(10)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> environment;

struct EnvironmentPush
{
  // Rows of the dome frame's (Y up) rotation into the world: world = M * local.
  float4 localToWorld[3];
  uint width;
  uint height;
  uint frameIndex;
  uint zUp;
};
[[vk::push_constant]] ConstantBuffer<EnvironmentPush> push;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
  if (pixel.x >= push.width || pixel.y >= push.height)
  {
    return;
  }
  // The inverse of environmentRadiance's mapping:
  // uv = (atan(d.z, d.x) / 2pi + 0.5, acos(d.y) / pi) in the dome frame.
  const vec2 uv = (vec2(pixel) + 0.5f) / vec2(push.width, push.height);
  const float phi = (uv.x - 0.5f) * 2.0f * kPi;
  const float theta = uv.y * kPi;
  const vec3 local = vec3(sin(theta) * cos(phi), cos(theta), sin(theta) * sin(phi));
  const vec3 world = normalize(vec3(dot(push.localToWorld[0].xyz, local), dot(push.localToWorld[1].xyz, local),
                                    dot(push.localToWorld[2].xyz, local)));
  const vec3 radiance = evalSkyRadiance(world, atmosphereArgs, pixel, push.frameIndex, push.zUp != 0u,
                                        /*isPrimaryRay=*/false, /*applySkyIndirectRadianceScale=*/true,
                                        /*skyDisks=*/NUMOS_SKY_DISKS_NONE);
  environment[pixel] = vec4(max(radiance, vec3(0.0f)), 1.0f);
}

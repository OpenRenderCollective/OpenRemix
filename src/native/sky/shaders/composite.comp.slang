// NUMOS over the path tracer's composed frame (render resolution, before the
// upscaler), in place: the sky with its sun and moon disks where the camera
// ray left the scene, then the screen clouds and the air in front of every
// pixel. Scene radiance is NUMOS radiance times push.scale.
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
[[vk::binding(10)]] ConstantBuffer<Camera> camera;
[[vk::binding(11)]] Texture2D<float4> cloudRender;
[[vk::binding(12)]] Texture2D<float4> cloudDepth;
[[vk::binding(13)]] Texture3D<float4> apNearScatter;
[[vk::binding(14)]] Texture3D<float4> apFarScatter;
[[vk::binding(15)]] Texture3D<float4> apFarTransmittance;
[[vk::binding(16)]] Texture3D<float4> apFroxelAux;
[[vk::binding(17)]] StructuredBuffer<AerialPerspectiveLight> apLights;
[[vk::binding(18)]] StructuredBuffer<uint> apLightClusters;
[[vk::binding(19)]] SamplerState apSampler;
[[vk::binding(20)]] Texture2D<float> linearViewZ;
[[vk::binding(21)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> color;

// gameSky: 0 hidden (NUMOS's sky replaces the miss), 1 under (the game's sky
// stays behind the clouds and air), 2 over (sky pixels are left alone).
struct CompositePush
{
  uint width;
  uint height;
  uint frameIndex;
  uint zUp;
  float missDepth;
  float scale;
  uint gameSky;
  uint reserved;
};
[[vk::push_constant]] ConstantBuffer<CompositePush> push;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
  if (pixel.x >= push.width || pixel.y >= push.height)
  {
    return;
  }
  const AtmosphereArgs args = atmosphereArgs;
  const float viewZ = linearViewZ[pixel];
  const bool miss = viewZ >= push.missDepth;
  if (miss && push.gameSky == 2u)
  {
    return;
  }
  const vec4 input = color[pixel];
  const float scale = max(push.scale, 1e-30f);
  // The tracer's camera ray through this pixel, jittered as its sample was.
  const vec2 uv = cameraPixelCoordinateToScreenUV(camera, ivec2(pixel));
  const CameraDirections ray = cameraScreenUVToDirection(camera, uv, true);
  // The composite helpers turn the distance back into view depth along the
  // unjittered ray.
  const float forwardPerDistance = max(abs(cameraScreenUVToDirection(camera, uv, false).viewDirection.z), 1e-6f);
  const float viewDistance = miss ? 0.0f : viewZ / forwardPerDistance;
  vec3 radiance = input.rgb / scale;
  if (miss && push.gameSky == 0u)
  {
    // The cloud composite below covers the disks.
    radiance = evalSkyRadiance(ray.worldDirection, args, pixel, push.frameIndex, push.zUp != 0u,
                               /*isPrimaryRay=*/true, /*applySkyIndirectRadianceScale=*/false,
                               /*skyDisks=*/NUMOS_SKY_DISKS_ALL);
  }
  vec3 alphaBlend = vec3(0.0f), foreground = vec3(0.0f);
  vec4 cloudOverPixel;
  numosApplyCloudComposite(camera, args, cloudRender, cloudDepth, apFarScatter, apFarTransmittance, apSampler,
                           pixel, miss, viewDistance, 0.0f, vec3(1.0f), radiance, alphaBlend,
                           /*alphaBlendSurfaceValid=*/false, 0.0f, foreground, 0.0f, /*performedPSR=*/false,
                           cloudOverPixel);
  numosApplyAerialPerspective(camera, args, /*hostSkyOverride=*/false, apNearScatter, apFarScatter,
                              apFarTransmittance, apFroxelAux, apLights, apLightClusters, apSampler, pixel, miss,
                              viewDistance, vec3(1.0f), vec3(1.0f), cloudOverPixel, radiance);
  color[pixel] = vec4(max(radiance, vec3(0.0f)) * scale, input.a);
}

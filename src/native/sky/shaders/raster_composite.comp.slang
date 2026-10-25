// NUMOS over the raster renderer's frame, which is display-referred and
// cannot be written here: per pixel, what the target is multiplied by (the
// air's and clouds' transmittance) and what is then added (the sky, the
// clouds' light and the air's in-scatter, exposed and tone mapped). The
// composite is affine in the colour behind it, so its result over black is
// the addition and over white less that is the transmittance.
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
[[vk::binding(21)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> addition;
[[vk::binding(27)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> transmittance;

// gameSky: 0 hidden (NUMOS's sky replaces the miss), 1 under (the game's sky
// stays behind the clouds and air), 2 over (sky pixels are left alone).
// flags: 1 the target blends linear values (an sRGB format).
struct RasterCompositePush
{
  uint width;
  uint height;
  uint frameIndex;
  uint zUp;
  float missDepth;
  float exposure; // NUMOS radiance to the display's
  uint gameSky;
  uint flags;
};
[[vk::push_constant]] ConstantBuffer<RasterCompositePush> push;

vec3 composite(Camera view, AtmosphereArgs args, uint2 pixel, bool miss, float viewDistance, vec3 radiance)
{
  vec3 alphaBlend = vec3(0.0f), foreground = vec3(0.0f);
  vec4 cloudOverPixel;
  numosApplyCloudComposite(view, args, cloudRender, cloudDepth, apFarScatter, apFarTransmittance, apSampler,
                           pixel, miss, viewDistance, 0.0f, vec3(1.0f), radiance, alphaBlend,
                           /*alphaBlendSurfaceValid=*/false, 0.0f, foreground, 0.0f, /*performedPSR=*/false,
                           cloudOverPixel);
  numosApplyAerialPerspective(view, args, /*hostSkyOverride=*/false, apNearScatter, apFarScatter,
                              apFarTransmittance, apFroxelAux, apLights, apLightClusters, apSampler, pixel, miss,
                              viewDistance, vec3(1.0f), vec3(1.0f), cloudOverPixel, radiance);
  return radiance;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
  if (pixel.x >= push.width || pixel.y >= push.height)
  {
    return;
  }
  const AtmosphereArgs args = atmosphereArgs;
  const Camera view = camera;
  const float viewZ = linearViewZ[pixel];
  const bool miss = viewZ >= push.missDepth;
  if (miss && push.gameSky == 2u)
  {
    addition[pixel] = vec4(0.0f);
    transmittance[pixel] = vec4(1.0f);
    return;
  }
  const vec2 uv = cameraPixelCoordinateToScreenUV(view, ivec2(pixel));
  const CameraDirections ray = cameraScreenUVToDirection(view, uv, true);
  const float forwardPerDistance = max(abs(cameraScreenUVToDirection(view, uv, false).viewDirection.z), 1e-6f);
  const float viewDistance = miss ? 0.0f : viewZ / forwardPerDistance;
  vec3 added, kept;
  if (miss && push.gameSky == 0u)
  {
    const vec3 sky = evalSkyRadiance(ray.worldDirection, args, pixel, push.frameIndex, push.zUp != 0u,
                                     /*isPrimaryRay=*/true, /*applySkyIndirectRadianceScale=*/false,
                                     /*skyDisks=*/NUMOS_SKY_DISKS_ALL);
    added = composite(view, args, pixel, miss, viewDistance, sky);
    kept = vec3(0.0f);
  }
  else
  {
    added = composite(view, args, pixel, miss, viewDistance, vec3(0.0f));
    kept = saturate(composite(view, args, pixel, miss, viewDistance, vec3(1.0f)) - added);
  }
  // Exposed and tone mapped toward the display's range.
  added = max(added, vec3(0.0f)) * push.exposure;
  added = added / (1.0f + added);
  if ((push.flags & 1u) == 0u)
  {
    // A display-referred target blends encoded values.
    added = pow(added, vec3(1.0f / 2.2f));
    kept = pow(kept, vec3(1.0f / 2.2f));
  }
  addition[pixel] = vec4(added, 1.0f);
  transmittance[pixel] = vec4(kept, 1.0f);
}

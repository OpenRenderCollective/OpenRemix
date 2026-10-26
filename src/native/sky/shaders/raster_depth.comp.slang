// The raster target's depth as the linear view depth NUMOS takes (world
// units along the view axis), with missDepth where nothing was drawn.
[[vk::binding(0)]] Texture2D<float> depth;
[[vk::binding(1)]] [[vk::image_format("r32f")]] RWTexture2D<float> linearViewZ;

struct RasterDepthPush
{
  // The inverse projection's z and w rows: a point's view z over w.
  float4 inverseZ;
  float4 inverseW;
  uint width;
  uint height;
  float clearDepth; // the target's clear value: sky
  float missDepth;
};
[[vk::push_constant]] ConstantBuffer<RasterDepthPush> push;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
  if (pixel.x >= push.width || pixel.y >= push.height)
  {
    return;
  }
  const float d = depth[pixel];
  if (d >= push.clearDepth)
  {
    linearViewZ[pixel] = push.missDepth;
    return;
  }
  const float2 ndc = (float2(pixel) + 0.5f) / float2(push.width, push.height) * 2.0f - 1.0f;
  const float4 clip = float4(ndc, d, 1.0f);
  const float w = dot(push.inverseW, clip);
  linearViewZ[pixel] = abs(dot(push.inverseZ, clip) / w);
}

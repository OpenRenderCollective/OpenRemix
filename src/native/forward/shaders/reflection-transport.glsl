// Current-frame screen/ray radiance reconstruction. Providers return incident
// radiance weighted by confidence; material Fresnel and environment fallback
// belong to the caller. Guide storage and bilateral reconstruction stay here.
#include "reflection-sample.glsl"
ReflectionSample sampleTransportReflection(ivec2 pixel, vec4 guide, float centerDepth) {
  vec2 low = (vec2(pixel) - float(frame.config.y / 2u)) / float(frame.config.y);
  ivec2 base = ivec2(floor(low));
  vec4 radiance = vec4(0);
  float total = 0;
  // Facepunch s&box's 2x2 bilateral upscale; see SBOX-LICENSE.txt.
  // Roughness filtering happens at the hit in the colour mip chain. This pass
  // reconstructs the receiving surface without blurring mirrors at full scale.
  vec2 fractional = low - floor(low);
  for (int y = 0; y <= 1; ++y)
    for (int x = 0; x <= 1; ++x) {
      ivec2 samplePixel = clamp(base + ivec2(x, y), ivec2(0), ivec2(frame.target.zw) - 1);
      ivec2 surface = surfacePixel(samplePixel);
      vec4 sampleGuide = texelFetch(normals, surface, 0);
      float sampleDepth = texelFetch(depthPyramid, surface, 0).r;
      if (sampleDepth >= 1 || !reflectionGuideValid(sampleGuide))
        continue;
      float distance = viewDepth((vec2(surface) + .5) / vec2(frame.target.xy), sampleDepth);
      vec2 bilinear = mix(1 - fractional, fractional, vec2(x, y));
      float weight = bilinear.x * bilinear.y;
      weight *= exp(-abs(distance - centerDepth) / max(centerDepth, 1e-4) * 70);
      weight *= pow(max(reflectionDot(reflectionGuideNormal(guide), reflectionGuideNormal(sampleGuide)), 0), 6);
      weight *= exp(-abs(guide.z - sampleGuide.z) * 4);
      radiance += texelFetch(rawRadiance, samplePixel, 0) * weight;
      total += weight;
    }
  radiance /= max(total, 1e-6);
  return ReflectionSample(radiance.rgb, radiance.a);
}

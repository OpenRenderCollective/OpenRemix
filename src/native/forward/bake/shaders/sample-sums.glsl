// Parallel kernels assign one 64-lane workgroup to a point. The arrays use
// less than 16 KiB, including the shared containing-medium classification.
#ifdef PARALLEL_BAKE
shared vec4 sumIrradiance[64 * 9];
shared vec4 sumMoments[64 * 6];
shared float sumFirst[64], sumSecond[64];
shared vec4 containingMedium;
shared uint containingSeed;
Medium pointMedium(vec3 origin, inout uint seed) {
  if (gl_LocalInvocationIndex == 0u) {
    Medium medium = initialMedium(origin, seed);
    containingMedium = vec4(medium.extinction, medium.index);
    containingSeed = seed;
  }
  barrier();
  seed = containingSeed;
  return Medium(containingMedium.xyz, containingMedium.w);
}
void storeResult(uint index, RayResult value) {
  uint lane = gl_LocalInvocationIndex;
  for (uint i = 0u; i < 9u; ++i) sumIrradiance[lane * 9u + i] = value.irradiance[i];
  for (uint i = 0u; i < 6u; ++i) sumMoments[lane * 6u + i] = value.moments[i];
  sumFirst[lane] = value.statistics.x;
  sumSecond[lane] = value.statistics.y;
  barrier();
  for (uint stride = 32u; stride > 0u; stride >>= 1u) {
    if (lane < stride) {
      for (uint i = 0u; i < 9u; ++i)
        sumIrradiance[lane * 9u + i] += sumIrradiance[(lane + stride) * 9u + i];
      for (uint i = 0u; i < 6u; ++i)
        sumMoments[lane * 6u + i] += sumMoments[(lane + stride) * 6u + i];
      sumFirst[lane] += sumFirst[lane + stride];
      sumSecond[lane] += sumSecond[lane + stride];
    }
    barrier();
  }
  if (lane != 0u) return;
  for (uint i = 0u; i < 9u; ++i) value.irradiance[i] = sumIrradiance[i];
  for (uint i = 0u; i < 6u; ++i) value.moments[i] = sumMoments[i];
  value.statistics = vec4(sumFirst[0], sumSecond[0], float(rayFrame.counts.z), 0);
  if (rayFrame.control.z == 0u) {
    for (uint i = 0u; i < 6u; ++i)
      if (value.moments[i].w > 0) value.moments[i].xyz /= value.moments[i].w;
    value.statistics.xy /= float(rayFrame.counts.z);
  } else {
    float numerator = value.irradiance[0].x;
    float denominator = rayFrame.control.z >= 5u ? float(rayFrame.counts.z) : value.irradiance[0].z;
    float attenuated = value.irradiance[0].w;
    value.irradiance[0].x = attenuated == 0 || denominator <= 0
                              ? 1 : clamp(numerator / denominator, 0, uintBitsToFloat(0x3f7fffffu));
    value.irradiance[0].zw = vec2(0);
    value.statistics.xy = vec2(0);
  }
  rayResults[index] = value;
}
#else
Medium pointMedium(vec3 origin, inout uint seed) { return initialMedium(origin, seed); }
void storeResult(uint index, RayResult value) { rayResults[index] = value; }
#endif

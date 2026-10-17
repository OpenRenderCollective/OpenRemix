// Parallel kernels assign one 64-lane workgroup to a point. The arrays use
// less than 16 KiB, including the shared containing-medium classification.
#ifdef PARALLEL_BAKE
#ifdef COMPACT_VISIBILITY
shared vec4 sumIrradiance[64 * 3];
#else
shared vec4 sumIrradiance[64 * 9];
shared vec4 sumMoments[64 * 6];
shared float sumFirst[64], sumSecond[64];
#endif
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
#ifndef COMPACT_VISIBILITY
  if (rayFrame.control.z == 1u) {
    // Offline radiance needs one RGB reduction, not the nine irradiance SH
    // bands or visibility ratios. Only sample zero contributes hit metadata.
    sumIrradiance[lane] = value.irradiance[0];
    sumFirst[lane] = value.statistics.x;
    sumSecond[lane] = value.statistics.y;
    barrier();
    for (uint stride = 32u; stride > 0u; stride >>= 1u) {
      if (lane < stride) {
        sumIrradiance[lane] += sumIrradiance[lane + stride];
        sumFirst[lane] += sumFirst[lane + stride];
        sumSecond[lane] += sumSecond[lane + stride];
      }
      barrier();
    }
    if (lane == 0u) {
      value.irradiance[0] = sumIrradiance[0];
      value.statistics.xy = vec2(sumFirst[0], sumSecond[0]);
      rayResults[index] = value;
    }
    return;
  }
  if (rayFrame.control.z != 0u)
#endif
  {
    // Visibility needs nine scalar SH values and three reduction scalars.
    // Do not move/reduce the unused RGB coefficients, moments and statistics
    // of a full irradiance result for every chart/light connection.
    sumIrradiance[lane * 3u] = vec4(value.irradiance[0].xzw, value.irradiance[0].y);
    sumIrradiance[lane * 3u + 1u] = vec4(value.irradiance[1].y, value.irradiance[2].y,
                                         value.irradiance[3].y, value.irradiance[4].y);
    sumIrradiance[lane * 3u + 2u] = vec4(value.irradiance[5].y, value.irradiance[6].y,
                                         value.irradiance[7].y, value.irradiance[8].y);
    barrier();
    for (uint stride = 32u; stride > 0u; stride >>= 1u) {
      if (lane < stride)
        for (uint i = 0u; i < 3u; ++i)
          sumIrradiance[lane * 3u + i] += sumIrradiance[(lane + stride) * 3u + i];
      barrier();
    }
    if (lane != 0u) return;
    vec4 first = sumIrradiance[0];
    float denominator = rayFrame.control.z >= 5u ? float(rayFrame.counts.z) : first.y;
#ifdef COMPACT_VISIBILITY
    float visible = first.z == 0 || denominator <= 0
                        ? 1 : clamp(first.x / denominator, 0, uintBitsToFloat(0x3f7fffffu));
    visibilityResults[index * 3u] = vec4(visible, first.w, sumIrradiance[1].xy);
    visibilityResults[index * 3u + 1u] = vec4(sumIrradiance[1].zw, sumIrradiance[2].xy);
    visibilityResults[index * 3u + 2u] = vec4(sumIrradiance[2].zw, 0, 0);
#else
    value.irradiance[0] = vec4(first.z == 0 || denominator <= 0
                                 ? 1 : clamp(first.x / denominator, 0, uintBitsToFloat(0x3f7fffffu)),
                               first.w, 0, 0);
    for (uint i = 1u; i < 9u; ++i)
      value.irradiance[i] = vec4(0, sumIrradiance[1u + (i - 1u) / 4u][(i - 1u) % 4u], 0, 0);
    value.statistics = vec4(0, 0, float(rayFrame.counts.z), 0);
    rayResults[index] = value;
#endif
    return;
  }
#ifndef COMPACT_VISIBILITY
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
  for (uint i = 0u; i < 6u; ++i)
    if (value.moments[i].w > 0) value.moments[i].xyz /= value.moments[i].w;
  value.statistics.xy /= float(rayFrame.counts.z);
  rayResults[index] = value;
#endif
}
#else
Medium pointMedium(vec3 origin, inout uint seed) { return initialMedium(origin, seed); }
void storeResult(uint index, RayResult value) { rayResults[index] = value; }
#endif

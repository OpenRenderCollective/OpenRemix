// Receiver positions for one 8x8 reflection workgroup. No persistent GPU list
// or additional pass is needed. All invocations participate before early exits.
// One bit per probe matches the current 32-record storage contract.
shared vec4 probeTilePoints[64];
shared vec4 probeTileMinimum;
shared vec3 probeTileMaximum;
shared uint probeTileMask;
uint probeTileCandidates(vec3 position, bool valid) {
  if (probeParameters.x == 0u) return 0u; // uniform across the workgroup
  uint lane = gl_LocalInvocationIndex;
  bool finitePosition = !any(isnan(position)) && !any(isinf(position));
  probeTilePoints[lane] = vec4(valid && finitePosition ? position : vec3(0),
                             !valid ? 0 : finitePosition ? 1 : -1);
  barrier();
  if (lane == 0u) {
    vec3 low = vec3(3.402823e38), high = -low;
    float status = 0;
    for (uint i = 0u; i < 64u; ++i) {
      vec4 point = probeTilePoints[i];
      if (point.w < 0) { status = -1; break; }
      if (point.w == 0) continue;
      status = 1;
      low = min(low, point.xyz);
      high = max(high, point.xyz);
    }
    probeTileMinimum = vec4(low, status);
    probeTileMaximum = high;
    probeTileMask = status < 0 ? 0xffffffffu : 0u;
  }
  barrier();
  if (probeTileMinimum.w > 0 && lane < probeParameters.x) {
    ReflectionProbe record = probes[lane];
    vec3 center = probeTileMinimum.xyz * .5 + probeTileMaximum * .5;
    vec3 halfSize = probeTileMaximum * .5 - probeTileMinimum.xyz * .5;
    vec3 magnitude = max(abs(probeTileMinimum.xyz), abs(probeTileMaximum));
    bool intersects = true;
    for (uint c = 0u; c < 3u; ++c) {
      vec4 row = record.worldToLocal[c];
      float localCenter = dot(row, vec4(center, 1));
      float radius = dot(abs(row.xyz), halfSize);
      // Conservative float error guard for the affine support calculation.
      float guard = 16 * 1.192092896e-7 * (dot(abs(row.xyz), magnitude) + abs(row.w) + 1);
      // Reject only proven separation. Unordered arithmetic fails open.
      intersects = intersects && !(localCenter + radius + guard < record.influenceMinimum[c] ||
                                   localCenter - radius - guard > record.influenceMaximum[c]);
    }
    if (intersects) atomicOr(probeTileMask, 1u << lane);
  }
  barrier();
  return probeTileMask;
}

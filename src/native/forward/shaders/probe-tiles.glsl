// Receiver positions for one 8x8 reflection workgroup. No persistent GPU list
// or additional pass is needed. All invocations participate before early exits.
// One bit per probe matches the current 64-record storage contract.
shared vec4 probeTilePoints[64];
shared vec4 probeTileRowMinimum[8];
shared vec3 probeTileRowMaximum[8];
shared vec4 probeTileMinimum;
shared vec3 probeTileMaximum;
shared uint probeTileMask[2];
uvec2 probeTileCandidates(vec3 position, bool valid) {
  // Uniform across the workgroup. With one probe, let the receiver's exact
  // influence test reject it without building tile bounds or synchronizing.
  if (probeParameters.x <= 1u)
    return uvec2(probeParameters.x, 0u);
  uint lane = gl_LocalInvocationIndex;
  bool finitePosition = !any(isnan(position)) && !any(isinf(position));
  probeTilePoints[lane] = vec4(valid && finitePosition ? position : vec3(0),
                             !valid ? 0 : finitePosition ? 1 : -1);
  barrier();
  // Eight lanes reduce rows in parallel. A separate summary array keeps
  // these writes independent of the positions other lanes are still reading.
  if (lane < 8u) {
    vec3 low = vec3(3.402823e38), high = -low;
    float status = 0;
    for (uint x = 0u; x < 8u; ++x) {
      vec4 point = probeTilePoints[lane * 8u + x];
      if (point.w < 0) { status = -1; break; }
      if (point.w == 0) continue;
      status = 1;
      low = min(low, point.xyz);
      high = max(high, point.xyz);
    }
    probeTileRowMinimum[lane] = vec4(low, status);
    probeTileRowMaximum[lane] = high;
  }
  barrier();
  if (lane == 0u) {
    vec3 low = vec3(3.402823e38), high = -low;
    float status = 0;
    for (uint row = 0u; row < 8u; ++row) {
      vec4 minimum = probeTileRowMinimum[row];
      if (minimum.w < 0) { status = -1; break; }
      if (minimum.w == 0) continue;
      status = 1;
      low = min(low, minimum.xyz);
      high = max(high, probeTileRowMaximum[row]);
    }
    probeTileMinimum = vec4(low, status);
    probeTileMaximum = high;
    probeTileMask[0] = status < 0 ? 0xffffffffu : 0u;
    probeTileMask[1] = probeTileMask[0];
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
    if (intersects) atomicOr(probeTileMask[lane / 32u], 1u << (lane % 32u));
  }
  barrier();
  return uvec2(probeTileMask[0], probeTileMask[1]);
}

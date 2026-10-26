// Invocation-private stencil shared by diffuse, sky, sun and local lighting.
// Legacy fields retain eight raw weights. Refined cells blend two normalized
// stencils, fading to the coarse field over one fine cell at the patch edge.
bool localStencilInitialized = false;
bool localStencilValid = false;
vec3 localStencilPosition, localStencilNormal;
uint localStencilIndices[16];
float localStencilWeights[16];
uint localStencilFineNodes[8];
uint localStencilCount = 0u;
uint localStencilRevision = 0u;

float bakeCellStencil(vec3 position, vec3 normal, uvec3 dimensions, uvec3 start,
                      uint divisor, uint directory, uint first, out float confidence) {
  confidence = 0;
  for (uint corner = 0u; corner < 8u; ++corner) {
    localStencilIndices[first + corner] = 0u;
    localStencilWeights[first + corner] = 0;
  }
  float ratio = 1.0 / float(divisor), spacing = bakeOriginSpacing.w * ratio;
  float minimumVariance = bakeSettings.y * ratio * ratio;
  position += normal * (bakeSettings.x * ratio);
  vec3 origin = bakeOriginSpacing.xyz + vec3(start) * spacing;
  vec3 q = (position - origin) / spacing;
  if (any(isnan(q)) || any(isinf(q)) || any(lessThan(q, vec3(0))) ||
      any(greaterThan(q, vec3(dimensions) - 1))) return 0;
  uvec3 base = min(uvec3(q), dimensions - 2u);
  vec3 fraction = q - vec3(base);
  float total = 0, support = 0;
  for (uint corner = 0u; corner < 8u; ++corner) {
    uvec3 upper = uvec3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
    uvec3 coordinate = base + upper;
    vec3 factors = mix(1 - fraction, fraction, vec3(upper));
    float weight = factors.x * factors.y * factors.z;
    uint index = (coordinate.z * dimensions.y + coordinate.y) * dimensions.x + coordinate.x;
    if (first == 8u) localStencilFineNodes[corner] = index;
    if (bakeAddressingMode() >= 2u) {
      uint record = bakeLookupWord(directory + index);
      if (record == 0u) continue;
      index = record - 1u;
    }
    localStencilIndices[first + corner] = index;
    if (bakeProbeStatistics(index).w < .5 || weight <= 0) continue;
    vec3 delta = position - (bakeOriginSpacing.xyz + vec3(start + coordinate) * spacing);
    if (bakeAddressingMode() != 0u)
      delta -= vec3(bakeProbeCoefficient(index, 0u).w, bakeProbeCoefficient(index, 1u).w,
                    bakeProbeCoefficient(index, 2u).w);
    float distance = length(delta);
    float visibility = 1;
    if (!bakedProbeDepthVisibility(index, delta, minimumVariance, visibility)) {
      vec3 absolute = abs(delta);
      uint axis = absolute.x >= absolute.y && absolute.x >= absolute.z ? 0u
                  : absolute.y >= absolute.z ? 1u : 2u;
      vec4 moments = bakeProbeMoment(index, axis * 2u + (delta[axis] < 0 ? 1u : 0u));
      if (moments.w <= 0) continue;
      if (distance > moments.x) {
        float variance = max(minimumVariance, moments.y - moments.x * moments.x);
        float difference = distance - moments.x;
        visibility = pow(variance / (variance + difference * difference), 3);
      }
    }
    float facing = distance > 1e-6 ? dot(normal, delta) / distance : -1;
    float facingWeight = pow(max(.05, .5 - .5 * facing), 2);
    support += weight * facingWeight;
    weight *= visibility;
    weight *= facingWeight;
    localStencilWeights[first + corner] = weight;
    total += weight;
  }
  confidence = support > 1e-6 ? total / support : 0;
  return total;
}
bool localProbeStencil(vec3 position, vec3 normal) {
  if (localStencilInitialized && all(equal(position, localStencilPosition)) &&
      all(equal(normal, localStencilNormal))) return localStencilValid;
  localStencilInitialized = true;
  ++localStencilRevision;
  localStencilValid = false;
  localStencilPosition = position;
  localStencilNormal = normal;
  localStencilCount = 8u;
  if (bakeDimensions.w == 0u) return false;
  float coarseConfidence, fineConfidence;
  float coarseTotal = bakeCellStencil(position, normal, bakeDimensions.xyz, uvec3(0), 1u, 0u, 0u, coarseConfidence);
  localStencilValid = coarseTotal >= 1e-4;
  if (bakeAddressingMode() < 4u) return localStencilValid;
  vec3 q = (position - bakeOriginSpacing.xyz) / bakeOriginSpacing.w;
  if (any(isnan(q)) || any(isinf(q)) || any(lessThan(q, vec3(0))) ||
      any(greaterThanEqual(q, vec3(bakeDimensions.xyz) - 1))) return localStencilValid;
  uvec3 parent = uvec3(q);
  uint nodes = bakeDimensions.x * bakeDimensions.y * bakeDimensions.z;
  uint cell = (parent.z * bakeDimensions.y + parent.y) * bakeDimensions.x + parent.x;
  uint reference = bakeLookupWord(nodes + 4u + cell);
  uint level = reference & 3u;
  uint factor = level == 0u ? bakeLookupWord(nodes + 1u) : 1u << level;
  uvec3 strides = uvec3(1u, bakeDimensions.x, bakeDimensions.x * bakeDimensions.y);
  vec3 biased = (position + normal * (bakeSettings.x / float(factor)) - bakeOriginSpacing.xyz) / bakeOriginSpacing.w;
  for (uint axis = 0u; axis < 3u; ++axis) {
    uint faces = reference >> 26u;
    if (biased[axis] < float(parent[axis]) && (faces & (1u << (axis * 2u))) != 0u) {
      parent[axis]--; cell -= strides[axis];
    } else if (biased[axis] >= float(parent[axis] + 1u) && (faces & (1u << (axis * 2u + 1u))) != 0u) {
      parent[axis]++; cell += strides[axis];
    } else continue;
    reference = bakeLookupWord(nodes + 4u + cell);
  }
  uint block = reference & 0x03fffffcu, faces = reference >> 26u;
  vec3 fraction = q - vec3(parent), edge = vec3(1);
  for (uint axis = 0u; axis < 3u; ++axis) {
    if ((faces & (1u << (axis * 2u))) == 0u) edge[axis] = min(edge[axis], fraction[axis]);
    if ((faces & (1u << (axis * 2u + 1u))) == 0u) edge[axis] = min(edge[axis], 1 - fraction[axis]);
  }
  float blend = clamp(min(edge.x, min(edge.y, edge.z)) * float(factor), 0, 1);
  blend = blend * blend * (3 - 2 * blend);
  if (block == 0u || blend <= 0) return localStencilValid;
  float fineTotal = bakeCellStencil(position, normal, uvec3(factor + 1u), parent * factor,
                                    factor, block, 8u, fineConfidence);
  if (fineTotal < 1e-4) return localStencilValid;
  if (coarseTotal < 1e-4) blend = 1;
  else {
    // Broad moments may suppress weak coarse fallback, never promote it
    // over the nominal fine share. Keep directional confidence symmetric.
    float fine = blend * fineConfidence, coarse = (1 - blend) * coarseConfidence;
    if (fine + coarse > 1e-6) {
      float trusted = fine / (fine + coarse);
      blend = directionalProbeDepth() ? trusted : max(blend, trusted);
    }
  }
  for (uint corner = 0u; corner < 8u; ++corner) {
    localStencilWeights[corner] *= coarseTotal >= 1e-4 ? (1 - blend) / coarseTotal : 0;
    localStencilWeights[8u + corner] *= blend / fineTotal;
  }
  applyCoarseTransfer(cell, factor, block, localStencilIndices, localStencilFineNodes, localStencilWeights);
  localStencilCount = 16u;
  localStencilValid = true;
  return true;
}

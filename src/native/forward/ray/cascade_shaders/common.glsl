#ifndef OPENREMIX_CASCADE_COMMON_GLSL
#define OPENREMIX_CASCADE_COMMON_GLSL
// Split Radiance Cascades (Freeman, Sannikov and Pitt-Francis): sparse world-space
// probes in a spatial hash, shared by the compute passes that build them and the
// receiver query. Mirrors forward/ray/cascades.h; the importer selects
// CASCADE_SET and CASCADE_TABLE_ACCESS before including this file.
const uint CascadeCount = 4u;
const uint CascadeTableBits = 18u;
const uint CascadeTableSize = 1u << CascadeTableBits;
// Probe pools, shared by every level of detail. A cascade has an eighth of the
// cells of the one below in open space and a quarter on a surface; the last
// cascade is the one every other merges from, so it keeps more than a quarter.
const uint CascadeCapacities[4] = uint[4](32768u, 8192u, 2048u, 1024u);
const uint CascadeBaseProbes = CascadeCapacities[0];
const uint CascadeBaseDirections = 32u; // cascade 0; each higher cascade has four times as many
const uint CascadeInvalid = 0xffffffffu;
const uint CascadeMaxLod = 15u;
// Insertion and lookup share this bound: a key placed further away would never be found.
const uint CascadeProbeSteps = 16u;
const float CascadeLodOverlap = .9;
const float CascadePi = 3.141592653589793;
// Irradiance tiles: 6x6 octahedral texels and a one-texel border, 256 tiles per row.
const uint CascadeTile = 8u, CascadeTileRow = 256u;
const vec2 CascadeAtlasExtent = vec2(2048, 1024);

layout(set = CASCADE_SET, binding = 0, std140) uniform CascadeFrameBlock {
  mat4 inverseCamera;
  vec4 cameraSpacing; // xyz: camera of the build, w: finest probe spacing (scene units)
  vec4 ranges;        // LOD 0 radius, cascade 0 interval end at LOD 0, ray origin bias, radiance fixed-point scale
  uvec4 target;       // guide extent, ray grid extent
  uvec4 config;       // ray stride, batch capacity, a build is published, reserved
  uvec4 sequence;     // direction sequence offset, sampled pixel within a stride block (xy), history is usable
} cascadeFrame;
// Keys in the first half, each key's probe index in the second.
layout(set = CASCADE_SET, binding = 1, std430) CASCADE_TABLE_ACCESS buffer CascadeTableBuffer {
  uint cascadeTable[];
};

uint cascadeCapacity(uint cascade) { return CascadeCapacities[cascade]; }
uint cascadeProbeBase(uint cascade) {
  uint base = 0u;
  for (uint i = 0u; i < cascade; ++i) base += cascadeCapacity(i);
  return base;
}
uint cascadeDirections(uint cascade) { return CascadeBaseDirections << (2u * cascade); }
uint cascadeBinBase(uint cascade) {
  uint base = 0u;
  for (uint i = 0u; i < cascade; ++i) base += cascadeCapacity(i) * cascadeDirections(i);
  return base;
}

// Probes keep a constant size on screen: the level of detail follows the
// Chebyshev distance to the camera of the build. `blend` is the weight of the
// next level where the two overlap, zero elsewhere.
uint cascadeLod(vec3 position, out float blend) {
  vec3 offset = abs(position - cascadeFrame.cameraSpacing.xyz);
  float distance = max(offset.x, max(offset.y, offset.z));
  float radius = cascadeFrame.ranges.x;
  uint lod = distance <= radius ? 0u : min(uint(ceil(log2(distance / radius))), CascadeMaxLod);
  float outer = ldexp(radius, int(lod));
  // log2 can round across a boundary; keep the level's own range exact.
  if (lod < CascadeMaxLod && distance > outer) { ++lod; outer *= 2; }
  else if (lod > 0u && distance <= outer * .5) { --lod; outer *= .5; }
  float inner = outer * CascadeLodOverlap;
  blend = lod < CascadeMaxLod && distance > inner ? clamp((distance - inner) / (outer - inner), 0, 1) : 0;
  return lod;
}
float cascadeSpacing(uint lod, uint cascade) { return ldexp(cascadeFrame.cameraSpacing.w, int(lod + cascade)); }
// Probes sit at cell centres, so each cascade is offset by half its spacing.
ivec3 cascadeCell(vec3 position, uint lod, uint cascade) {
  return ivec3(floor(position / cascadeSpacing(lod, cascade)));
}
// Where a surface point deposits and reads its probes: off the surface along
// its normal. A cell wider than a wall would otherwise serve both of its
// faces, and light from one side would reach the other. Three quarters of a
// spacing puts the cells of two opposite faces more than one spacing apart,
// outside each other's trilinear neighbourhood, however thin the wall.
const float CascadeAnchorOffset = .75;
vec3 cascadeAnchor(vec3 position, vec3 normal, uint lod, uint cascade) {
  return position + normal * (CascadeAnchorOffset * cascadeSpacing(lod, cascade));
}
// A level's probes span fewer than 256 cells per axis around the camera, so
// wrapped coordinates identify them. Bit 31 distinguishes a key from an empty slot.
uint cascadeKey(ivec3 cell, uint lod, uint cascade) {
  uvec3 wrapped = uvec3(cell) & 255u;
  return 0x80000000u | cascade << 28u | lod << 24u | wrapped.z << 16u | wrapped.y << 8u | wrapped.x;
}
uint cascadeSlot(uint key) {
  key ^= key >> 16u;
  key *= 0x7feb352du;
  key ^= key >> 15u;
  key *= 0x846ca68bu;
  key ^= key >> 16u;
  return key & (CascadeTableSize - 1u);
}
// Index into the unified probe pool, or CascadeInvalid.
uint cascadeFind(uint key) {
  uint slot = cascadeSlot(key);
  for (uint step = 0u; step < CascadeProbeSteps; ++step) {
    uint stored = cascadeTable[slot];
    if (stored == key) return cascadeTable[CascadeTableSize + slot];
    if (stored == 0u) return CascadeInvalid;
    slot = (slot + 1u) & (CascadeTableSize - 1u);
  }
  return CascadeInvalid;
}

// Equal-area mapping between the unit square and the sphere.
vec3 cascadeDirection(vec2 uv) {
  float phi = 2 * CascadePi * uv.x, z = 2 * uv.y - 1, radius = sqrt(max(1 - z * z, 0));
  return vec3(radius * cos(phi), radius * sin(phi), z);
}
vec2 cascadeDirectionCoordinate(vec3 direction) {
  return vec2(fract(atan(direction.y, direction.x) / (2 * CascadePi)), clamp(direction.z * .5 + .5, 0, 1));
}
// Directions form a 2n by n grid; four of a cascade's directions share one in the cascade below.
uint cascadeDirectionRows(uint cascade) { return 4u << cascade; }
uint cascadeDirectionIndex(vec3 direction, uint cascade) {
  uint rows = cascadeDirectionRows(cascade);
  vec2 uv = cascadeDirectionCoordinate(direction);
  uvec2 cell = min(uvec2(uv * vec2(2u * rows, rows)), uvec2(2u * rows - 1u, rows - 1u));
  return cell.y * 2u * rows + cell.x;
}

vec2 cascadeOctEncode(vec3 normal) {
  normal /= max(abs(normal.x) + abs(normal.y) + abs(normal.z), 1e-10);
  vec2 side = mix(vec2(-1), vec2(1), greaterThanEqual(normal.xy, vec2(0)));
  return normal.z >= 0 ? normal.xy : (1 - abs(normal.yx)) * side;
}
vec3 cascadeOctDecode(vec2 value) {
  vec3 normal = vec3(value, 1 - abs(value.x) - abs(value.y));
  vec2 side = mix(vec2(-1), vec2(1), greaterThanEqual(normal.xy, vec2(0)));
  normal.xy -= side * max(-normal.z, 0);
  return normalize(normal);
}
#endif

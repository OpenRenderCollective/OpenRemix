// Scalar visibility and directional ownership share the same sparse entry ID.
// This decoder is also used by the selected-light GPU gather.
uint localProbeWord(uint base, uint word) {
  return bakeRecords[base + word / 4u][word % 4u];
}
bool localProbeMask(uint base, uint probe, uint light, out float visibility, out uint entry) {
  entry = 0u;
  if (localProbeWord(base, 1u) == 3u) {
    uint count = localProbeWord(base, 3u), rowWords = localProbeWord(base, 4u + count);
    uint rows = 5u + count, row = rows + (probe * rowWords + light / 32u) * 2u;
    uint bits = localProbeWord(base, row), bit = light % 32u;
    if ((bits & (1u << bit)) == 0u) return false;
    uint rank = localProbeWord(base, row + 1u) + uint(bitCount(bits & ((1u << bit) - 1u)));
    uint offsets = rows + bakeDimensions.w * rowWords * 2u;
    uint index = localProbeWord(base, offsets + probe) + rank;
    entry = index;
    visibility = uintBitsToFloat(localProbeWord(base, offsets + bakeDimensions.w + 1u + index));
    return true;
  }
  uint offsets = 4u + localProbeWord(base, 3u);
  uint low = localProbeWord(base, offsets + probe), end = localProbeWord(base, offsets + 1u + probe), high = end;
  uint entries = offsets + 1u + bakeDimensions.w;
  while (low < high) {
    uint middle = low + (high - low) / 2u;
    if (localProbeWord(base, entries + middle * 2u) < light) low = middle + 1u;
    else high = middle;
  }
  if (low >= end || localProbeWord(base, entries + low * 2u) != light) return false;
  entry = low;
  visibility = uintBitsToFloat(localProbeWord(base, entries + low * 2u + 1u));
  return true;
}
float localProbeDirect(uint indices, uint records, uint entry, float sh[9]) {
  uint field = localProbeWord(0u, indices + entry);
  if (field == 0u) return 0;
  uint record = records + (field - 1u) * 6u;
  float value = 0;
  for (uint pair = 0u; pair < 4u; ++pair)
    value += dot(unpackHalf2x16(localProbeWord(0u, record + pair)),
                 vec2(sh[pair * 2u], sh[pair * 2u + 1u]));
  value += unpackHalf2x16(localProbeWord(0u, record + 4u)).x * sh[8];
  return value * uintBitsToFloat(localProbeWord(0u, record + 5u));
}
struct ProbeLightReference {
  uint table, light, direct, records;
};
bool resolveProbeLight(uint capturedLight, out ProbeLightReference reference) {
  uint table = (floatBitsToUint(bakeSettings.w) & 0x3fffffffu) / 2u;
  if (capturedLight == 0u || table == 0u || localProbeWord(table, 0u) != 0x56504c52u ||
      localProbeWord(table, 2u) != bakeDimensions.w) return false;
  uint direct = 0u;
  uint records = 0u;
  if (localProbeWord(table, 1u) == 7u) {
    direct = (table + 2u) * 4u + localProbeWord(table, 3u);
    records = direct + localProbeWord(table, 4u);
    table += 2u; // wrapper and scalar payload are both vec4 aligned
  }
  uint light = capturedLight - 1u, version = localProbeWord(table, 1u), count = localProbeWord(table, 3u);
  if (version == 3u) {
    if (light >= count) return false;
    light = localProbeWord(table, 4u + light);
    if (light == 0xffffffffu) return false;
  } else if (version == 2u) {
    if (light / 32u >= count ||
        (localProbeWord(table, 4u + light / 32u) & (1u << (light % 32u))) == 0u) return false;
  } else return false;
  reference = ProbeLightReference(table, light, direct, records);
  return true;
}

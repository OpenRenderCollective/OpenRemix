// Optional immutable geometry transfer follows the directory and directional
// depth records, before styled SH. All offsets below are relative uint words.
uint bakeTransferBase() {
  return bakeDimensions.w * 16u + bakeDirectoryRecords() + bakeDepthRecords();
}
uint bakeTransferWord(uint word) {
  return bakeRecords[bakeTransferBase() + word / 4u][word % 4u];
}
uint bakeTransferRecords() {
  if ((floatBitsToUint(bakeSettings.w) & 0x40000000u) == 0u) return 0u;
  uint base = bakeTransferBase(), available = uint(bakeRecords.length());
  if (base >= available) return 0u;
  uint words = bakeTransferWord(3u);
  return words >= 4u && (words & 3u) == 0u && words / 4u <= available - base ? words / 4u : 0u;
}
void applyCoarseTransfer(uint cell, uint factor, uint block, uint indices[16],
                         uint fineNodes[8], inout float weights[16]) {
  uint records = bakeTransferRecords();
  if (records == 0u || bakeTransferWord(0u) != 0x43544652u) return;
  uint words = records * 4u, nodes = bakeDimensions.x * bakeDimensions.y * bakeDimensions.z;
  if (bakeTransferWord(1u) != nodes || words < 4u || nodes > words - 4u || cell >= nodes) return;
  uint patchOffset = bakeTransferWord(4u + cell);
  if (patchOffset == 0u || patchOffset > words || words - patchOffset < 12u) return;
  if (factor != 2u && factor != 4u && factor != 8u) return;
  uint side = factor + 1u, count = side * side * side, fields = bakeTransferWord(patchOffset + 2u);
  if (bakeTransferWord(patchOffset) != factor || bakeTransferWord(patchOffset + 1u) != count ||
      bakeTransferWord(patchOffset + 3u) != 0u || fields > words || count * 3u > words - fields ||
      block > bakeDirectoryRecords() * 4u || count > bakeDirectoryRecords() * 4u - block) return;
  float coarse = 0, fine = 0;
  for (uint i = 0u; i < 16u; ++i) {
    if (isnan(weights[i]) || isinf(weights[i]) || weights[i] < 0) return;
    if (i < 8u) coarse += weights[i]; else fine += weights[i];
  }
  if (isinf(coarse + fine) || coarse <= 1e-6 || fine <= 1e-6) return;
  float share = fine / (coarse + fine), total = fine;
  float corrected[8];
  bool occluded = false;
  for (uint c = 0u; c < 8u; ++c) {
    corrected[c] = weights[c];
    if (weights[c] <= 0) continue;
    uint corner = 8u;
    if (indices[c] < bakeDimensions.w)
      for (uint i = 0u; i < 8u; ++i)
        if (bakeTransferWord(patchOffset + 4u + i) == indices[c] + 1u) { corner = i; break; }
    float visibility = fine;
    if (corner < 8u) {
      visibility = 0;
      for (uint f = 0u; f < 8u; ++f) {
        float weight = weights[8u + f];
        if (weight <= 0) continue;
        uint node = fineNodes[f];
        float visible = 1;
        if (node < count && indices[8u + f] < bakeDimensions.w &&
            bakeLookupWord(block + node) == indices[8u + f] + 1u) {
          uint address = fields + node * 3u;
          if ((bakeTransferWord(address + 2u) & (1u << corner)) != 0u) {
            uint value = (bakeTransferWord(address + corner / 4u) >> ((corner % 4u) * 8u)) & 255u;
            visible = float(value) / 255.0;
            occluded = occluded || value < 255u;
          }
        }
        visibility += weight * visible;
      }
    }
    corrected[c] *= 1 - share + share * visibility / fine;
    total += corrected[c];
  }
  if (!occluded || isinf(total) || isnan(total) || total < 1e-4) return;
  for (uint i = 0u; i < 16u; ++i) weights[i] = (i < 8u ? corrected[i] : weights[i]) / total;
}

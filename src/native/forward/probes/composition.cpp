#include "forward/probes/composition.h"
#include "forward/probes/priority_order.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace remix::forward::probes {
  namespace {
    bool contains(const Record &outer, const Record &inner) {
      if (!validProjectionSpace(outer.worldToLocal) || !validProjectionSpace(inner.worldToLocal)) return false;
      for (const auto *record : {&outer, &inner})
        for (uint32_t c = 0; c < 3; ++c)
          if (!std::isfinite(record->influenceMinimum[c]) || !std::isfinite(record->influenceMaximum[c]) ||
              record->influenceMinimum[c] >= record->influenceMaximum[c]) return false;
      if (outer.worldToLocal == inner.worldToLocal) {
        for (uint32_t c = 0; c < 3; ++c)
          if (inner.influenceMinimum[c] < outer.influenceMinimum[c] ||
              inner.influenceMaximum[c] > outer.influenceMaximum[c]) return false;
        return true;
      }
      // Rigid inverse: world = transpose(R) * (local - translation).
      // All corners inside a convex outer box prove containment. A roundoff
      // guard retains uncertain boundary cases rather than losing fallback.
      for (uint32_t corner = 0; corner < 8; ++corner) {
        Vec3 world{};
        for (uint32_t axis = 0; axis < 3; ++axis) {
          const float local = (corner & (1u << axis)) ? inner.influenceMaximum[axis] : inner.influenceMinimum[axis];
          for (uint32_t c = 0; c < 3; ++c)
            world[c] += inner.worldToLocal[axis][c] * (local - inner.worldToLocal[axis][3]);
        }
        const auto local = projectionPoint(outer.worldToLocal, world);
        for (uint32_t c = 0; c < 3; ++c) {
          float magnitude = std::abs(outer.worldToLocal[c][3]) + 1;
          for (uint32_t axis = 0; axis < 3; ++axis)
            magnitude += std::abs(outer.worldToLocal[c][axis] * world[axis]);
          const float guard = 32 * std::numeric_limits<float>::epsilon() * magnitude;
          if (!std::isfinite(local[c]) || local[c] - guard < outer.influenceMinimum[c] ||
              local[c] + guard > outer.influenceMaximum[c]) return false;
        }
      }
      return true;
    }
  }
  Composition compose(const Header &authored, const Header &automatic, uint32_t capacity) {
    const uint32_t count = authored.parameters[0], available = automatic.parameters[0];
    if (capacity > maxProbes || count > capacity || available > maxProbes)
      throw std::invalid_argument("Reflection composition exceeds metadata capacity");
    Composition result;
    if (!count) return result; // An explicit empty authored set still disables local probes.
    priorityOrder(authored);
    if (available) {
      priorityOrder(automatic);
      for (uint32_t c = 1; c < 4; ++c)
        if (authored.parameters[c] != automatic.parameters[c])
          throw std::invalid_argument("Reflection composition requires matching image dimensions and mips");
    }
    result.header = authored;
    float minimumPriority = authored.probes[0].minimum[3];
    for (uint32_t i = 0; i < count; ++i) {
      if (authored.probes[i].minimum[3] < -32768 || authored.probes[i].minimum[3] > 32767)
        throw std::invalid_argument("Authored reflection priority exceeds signed 16-bit range");
      result.layers.push_back({false, i});
      minimumPriority = std::min(minimumPriority, authored.probes[i].minimum[3]);
    }
    for (uint32_t i = 0; i < available; ++i) {
      bool covered = false;
      for (uint32_t j = 0; j < count && !covered; ++j) covered = contains(authored.probes[j], automatic.probes[i]);
      if (covered) { ++result.covered; continue; }
      if (result.layers.size() == capacity) { ++result.omitted; continue; }
      const auto destination = result.layers.size();
      result.header.probes[destination] = automatic.probes[i];
      // Internal metadata can use -32769; public authored priorities remain
      // unchanged and valid throughout their full signed 16-bit range.
      result.header.probes[destination].minimum[3] = minimumPriority - 1;
      result.layers.push_back({true, i});
      ++result.automatic;
    }
    result.header.parameters[0] = uint32_t(result.layers.size());
    const auto order = priorityOrder(result.header);
    const auto original = result;
    for (uint32_t i = 0; i < result.layers.size(); ++i) {
      result.header.probes[i] = original.header.probes[order[i]];
      result.layers[i] = original.layers[order[i]];
    }
    return result;
  }
}

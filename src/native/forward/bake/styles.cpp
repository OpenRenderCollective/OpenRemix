#include "forward/bake/styles.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace remix::forward::bake {
  namespace {
    bool sameShape(scene::SphereLight a, scene::SphereLight b) {
      a.id = b.id = 0;
      a.style = b.style = 0;
      a.dynamic = b.dynamic = false;
      std::fill_n(a.radiance, 3, 0.f);
      std::fill_n(b.radiance, 3, 0.f);
      return a == b;
    }
    bool close(float a, float b) {
      return std::abs(a - b) <= 1e-5f * std::max(std::abs(a), std::abs(b));
    }
    struct Group {
      StyleScale result;
      std::optional<float> minimum, maximum;
    };
  } // namespace
  std::vector<StyleScale> styleScales(const scene::BakeScene &captured,
                                     std::span<const scene::SphereLight> current) {
    std::unordered_multimap<uint64_t, const scene::SphereLight *> lights;
    for (const auto &light : current) lights.emplace(light.id, &light);
    std::map<uint32_t, Group> groups;
    for (const auto &source : captured.lights) {
      if (!source.style) continue;
      if (source.style > 127 || source.dynamic)
        throw std::invalid_argument("Invalid captured stationary style group");
      auto [entry, inserted] = groups.try_emplace(source.style);
      auto &group = entry->second;
      if (inserted) group.result = {source.style, 0, StyleStatus::Ready};
      if (group.result.status != StyleStatus::Ready) continue;
      const auto [first, last] = lights.equal_range(source.id);
      if (first == last) { group.result.status = StyleStatus::Missing; continue; }
      auto next = first;
      if (++next != last) { group.result.status = StyleStatus::Ambiguous; continue; }
      const auto &live = *first->second;
      if (live.dynamic || !sameShape(source, live)) {
        group.result.status = StyleStatus::GeometryChanged;
        continue;
      }
      for (uint32_t c = 0; c < 3; ++c) {
        const float a = source.radiance[c], b = live.radiance[c];
        if (!std::isfinite(a) || !std::isfinite(b) || a < 0 || b < 0) {
          group.result.status = StyleStatus::Invalid;
          break;
        }
        if (a == 0) {
          if (b != 0) group.result.status = StyleStatus::NonScalar;
        } else {
          const float ratio = b / a;
          if (!std::isfinite(ratio)) group.result.status = StyleStatus::Invalid;
          else {
            group.minimum = group.minimum ? std::min(*group.minimum, ratio) : ratio;
            group.maximum = group.maximum ? std::max(*group.maximum, ratio) : ratio;
            if (!close(*group.minimum, *group.maximum)) group.result.status = StyleStatus::NonScalar;
          }
        }
        if (group.result.status != StyleStatus::Ready) break;
      }
    }
    std::vector<StyleScale> results;
    results.reserve(groups.size());
    for (const auto &[style, group] : groups) {
      auto value = group.result;
      if (value.status == StyleStatus::Ready) value.value = group.minimum.value_or(0);
      results.push_back(value);
    }
    return results;
  }
} // namespace remix::forward::bake

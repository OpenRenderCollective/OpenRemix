#include "forward/mixed/directional_coverage.h"
#include "forward/mixed/directional_links.h"
#include "forward/mixed/data.h"
#include <stdexcept>

namespace remix::forward::mixed {
  void validateDirectionalCoverage(std::span<const uint32_t> words, const Plan &plan,
      const lightmap::Layout &layout, std::stop_token cancelled) {
    const auto require = [](bool value) {
      if (!value) throw std::invalid_argument("Incomplete directional surface coverage");
    };
    require(plan.lights.source && plan.charts.size() == layout.charts.size());
    DirectionalFields::validate(words, uint32_t(layout.charts.size()),
        uint32_t(plan.lights.source->lights.size()), DirectionalFields::maximumBytes);
    const auto links = positiveDirectionalLinks(plan, cancelled);
    require(words[4] == links.size());
    for (size_t i = 0; i < links.size(); ++i) {
      if ((i & 1023u) == 0 && cancelled.stop_requested())
        throw std::runtime_error("Directional coverage validation cancelled");
      const auto &link = links[i];
      const auto &r = layout.charts[link.chart];
      const auto record = words.subspan(words[6] + i * DirectionalFields::entryWords,
                                        DirectionalFields::entryWords);
      require(i >= words[words[5] + link.chart] && i < words[words[5] + link.chart + 1] &&
          record[0] == link.light && record[1] == r.x && record[2] == r.y &&
          record[3] == r.width && record[4] == r.height && r.page < layout.pages &&
          r.width <= layout.pageSize && r.height <= layout.pageSize &&
          r.x <= layout.pageSize - r.width && r.y <= layout.pageSize - r.height);
    }
  }
}

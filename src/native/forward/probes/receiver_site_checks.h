#pragma once
#include "forward/probes/receiver_sites.h"

namespace remix::forward::probes {
  // Seven bounded geometry queries per admitted site: the receiver connection
  // followed by +/-X, +/-Y, +/-Z clearance segments. This is sampled support,
  // not certification of an empty box or a usable parallax projection.
  class ReceiverSiteChecks {
  public:
    ReceiverSiteChecks(const ReceiverSiteAudit &admission, std::span<const ReceiverSample> receivers, float scale)
        : m_queries(plan(admission, receivers, scale), .001f / scale), m_slots(admission.sites().size()) {
      for (auto &slots : m_slots) slots.fill(UINT32_MAX);
      for (uint32_t i = 0; i < m_queries.pairs().size(); ++i) {
        const auto &pair = m_queries.pairs()[i];
        m_slots.at(pair.receiver).at(pair.capture) = i;
      }
    }
    std::span<const bake::Point> rays() const { return m_queries.rays(); }
    bool observe(uint32_t first, std::span<const bake::Result> values) { return m_queries.observe(first, values); }
    bool complete() const { return m_queries.complete(); }
    ReceiverVisibility connection(size_t site) const { return state(site, 0); }
    std::optional<Vec3> connectionBlock(size_t site) const {
      if (connection(site) != ReceiverVisibility::Blocked) return std::nullopt;
      const auto index = m_slots.at(site)[0];
      const auto &ray = m_queries.pairs()[index].ray.point;
      const float distance = m_queries.distance(index);
      Vec3 point;
      for (uint32_t c = 0; c < 3; ++c) point[c] = ray.position[c] + distance * ray.normal[c];
      return point;
    }
    ReceiverVisibility clearance(size_t site) const {
      bool unknown = false;
      for (uint32_t direction = 1; direction < 7; ++direction) {
        const auto value = state(site, direction);
        if (value == ReceiverVisibility::Blocked) return value;
        unknown |= value == ReceiverVisibility::Unknown;
      }
      return unknown ? ReceiverVisibility::Unknown : ReceiverVisibility::Visible;
    }
    bool supported(size_t site) const {
      return connection(site) == ReceiverVisibility::Visible && clearance(site) == ReceiverVisibility::Visible;
    }
  private:
    static std::vector<ReceiverPair> plan(const ReceiverSiteAudit &admission,
        std::span<const ReceiverSample> receivers, float scale) {
      if (!admission.complete() || receivers.size() > 8192 || !std::isfinite(scale) || scale <= 0)
        throw std::invalid_argument("Invalid local site check inputs");
      std::vector<ReceiverPair> queries;
      queries.reserve(admission.sites().size() * 7);
      for (uint32_t i = 0; i < admission.sites().size(); ++i) {
        if (!admission.sampledFree(i)) continue;
        const auto &site = admission.sites()[i];
        if (site.receiver >= receivers.size()) throw std::invalid_argument("Invalid local site receiver");
        const Vec3 position{site.point.position[0], site.point.position[1], site.point.position[2]};
        const auto connection = receiverRay(receivers[site.receiver], position, .002f / scale);
        if (!connection) continue;
        // Here the pair's capture field is a local query slot, not an image layer.
        queries.push_back({i, 0, *connection});
        for (uint32_t direction = 0; direction < 6; ++direction) {
          ReceiverRay ray{};
          ray.point.position = site.point.position;
          ray.point.normal[direction / 2] = direction & 1u ? -1 : 1;
          ray.distance = .1f / scale;
          queries.push_back({i, direction + 1, ray});
        }
      }
      return queries;
    }
    ReceiverVisibility state(size_t site, uint32_t slot) const {
      const auto index = m_slots.at(site).at(slot);
      return index == UINT32_MAX ? ReceiverVisibility::Unknown : m_queries.visibility(index);
    }
    ReceiverAudit m_queries;
    std::vector<std::array<uint32_t, 7>> m_slots;
  };
} // namespace remix::forward::probes

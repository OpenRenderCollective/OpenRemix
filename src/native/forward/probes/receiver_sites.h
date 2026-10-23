#pragma once
#include "forward/probes/receiver_audit.h"

namespace remix::forward::probes {
  struct ReceiverSite {
    uint32_t receiver = 0;
    bake::Point point{};
    bool partitionDisagreement = false;
  };
  // Proposals only: a normal offset and partition disagreement do not prove
  // empty space. Hardware admission and receiver connectivity remain separate.
  std::vector<ReceiverSite> receiverSites(const scene::BakeScene &source,
      std::span<const ReceiverSample> receivers, const ReceiverAudit &audit,
      uint32_t budget = 512, std::stop_token cancelled = {});

  class ReceiverSiteAudit {
  public:
    static constexpr uint32_t samples = 64;
    explicit ReceiverSiteAudit(std::vector<ReceiverSite> sites) : m_sites(std::move(sites)) {
      if (m_sites.size() > 512) throw std::invalid_argument("Receiver site budget exceeded");
      m_points.reserve(m_sites.size());
      for (const auto &site : m_sites) {
        for (uint32_t c = 0; c < 3; ++c)
          if (!std::isfinite(site.point.position[c])) throw std::invalid_argument("Nonfinite receiver site");
        m_points.push_back(site.point);
      }
      m_seen.resize(m_sites.size());
      m_back.resize(m_sites.size());
      m_valid.resize(m_sites.size());
    }
    std::span<const bake::Point> points() const { return m_points; }
    std::span<const ReceiverSite> sites() const { return m_sites; }
    bool observe(uint32_t first, std::span<const bake::Result> values) {
      if (first > m_sites.size() || values.size() > m_sites.size() - first) return false;
      for (size_t i = 0; i < values.size(); ++i) if (m_seen[first + i]) return false;
      for (size_t i = 0; i < values.size(); ++i) {
        const auto &s = values[i].statistics;
        const size_t at = first + i;
        m_back[at] = s[0];
        m_valid[at] = s[2] == samples && std::isfinite(s[0]) && std::isfinite(s[1]) &&
            s[0] >= 0 && s[0] <= 1 && s[1] >= 0 && s[1] <= 1 && s[0] + s[1] <= 1.00001f;
        m_seen[at] = true;
      }
      m_completed += values.size();
      return true;
    }
    bool complete() const { return m_completed == m_sites.size(); }
    bool known(size_t at) const { return m_seen.at(at) && m_valid.at(at); }
    bool sampledFree(size_t at) const { return known(at) && m_back.at(at) < .5f; }
    float backface(size_t at) const { return m_back.at(at); }
  private:
    std::vector<ReceiverSite> m_sites;
    std::vector<bake::Point> m_points;
    std::vector<uint8_t> m_seen, m_valid;
    std::vector<float> m_back;
    size_t m_completed = 0;
  };
} // namespace remix::forward::probes

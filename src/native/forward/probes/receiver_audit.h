#pragma once
#include "forward/probes/receiver_pairs.h"
#include <utility>
#include <unordered_set>

namespace remix::forward::probes {
  // One bounded set of geometry queries. The controller owns submission and
  // fences; batches may retire out of order, but never twice.
  class ReceiverAudit {
  public:
    ReceiverAudit(std::vector<ReceiverPair> pairs, float tolerance)
        : m_pairs(std::move(pairs)), m_tolerance(tolerance) {
      if (m_pairs.size() > 32768 || !std::isfinite(tolerance) || tolerance < 0)
        throw std::invalid_argument("Invalid receiver audit budget or tolerance");
      m_rays.reserve(m_pairs.size());
      for (const auto &pair : m_pairs) {
        if (!std::isfinite(pair.ray.distance) || pair.ray.distance <= 0)
          throw std::invalid_argument("Invalid receiver audit segment distance");
        double length = 0;
        for (uint32_t c = 0; c < 3; ++c) {
          if (!std::isfinite(pair.ray.point.position[c]) || !std::isfinite(pair.ray.point.normal[c]))
            throw std::invalid_argument("Nonfinite receiver audit ray");
          length += double(pair.ray.point.normal[c]) * pair.ray.point.normal[c];
        }
        if (std::abs(length - 1) > 1e-4) throw std::invalid_argument("Receiver audit direction is not normalized");
        m_rays.push_back(pair.ray.point);
      }
      m_seen.resize(m_pairs.size());
      m_visibility.resize(m_pairs.size(), ReceiverVisibility::Unknown);
      m_distances.resize(m_pairs.size());
    }
    std::span<const bake::Point> rays() const { return m_rays; }
    std::span<const ReceiverPair> pairs() const { return m_pairs; }
    std::span<const ReceiverVisibility> visibility() const { return m_visibility; }
    // Only extend after every old slot has retired. Existing evidence and seen
    // bits stay intact; retry pairs must not resubmit an earlier owner.
    void append(std::vector<ReceiverPair> pairs) {
      if (!complete() || pairs.size() > 32768 - m_pairs.size())
        throw std::logic_error("Receiver audit cannot extend pending work or exceed its budget");
      ReceiverAudit extra(std::move(pairs), m_tolerance);
      std::unordered_set<uint64_t> owners;
      for (const auto &pair : m_pairs) owners.insert((uint64_t(pair.receiver) << 32) | pair.capture);
      for (const auto &pair : extra.m_pairs)
        if (!owners.insert((uint64_t(pair.receiver) << 32) | pair.capture).second)
          throw std::invalid_argument("Receiver audit retry repeats a capture owner");
      m_pairs.insert(m_pairs.end(), extra.m_pairs.begin(), extra.m_pairs.end());
      m_rays.insert(m_rays.end(), extra.m_rays.begin(), extra.m_rays.end());
      m_seen.resize(m_pairs.size());
      m_visibility.resize(m_pairs.size(), ReceiverVisibility::Unknown);
      m_distances.resize(m_pairs.size());
    }
    bool observe(uint32_t first, std::span<const bake::Result> results) {
      if (first > m_pairs.size() || results.size() > m_pairs.size() - first) return false;
      for (size_t i = 0; i < results.size(); ++i) if (m_seen[first + i]) return false;
      for (size_t i = 0; i < results.size(); ++i) {
        const size_t at = first + i;
        m_visibility[at] = receiverVisibility(m_pairs[at].ray, results[i], m_tolerance);
        m_distances[at] = results[i].irradiance[0][3];
        m_seen[at] = true;
      }
      m_completed += results.size();
      return true;
    }
    bool complete() const { return m_completed == m_pairs.size(); }
    ReceiverVisibility visibility(size_t index) const { return m_visibility.at(index); }
    float distance(size_t index) const { return m_distances.at(index); }
  private:
    std::vector<ReceiverPair> m_pairs;
    std::vector<bake::Point> m_rays;
    std::vector<uint8_t> m_seen;
    std::vector<ReceiverVisibility> m_visibility;
    std::vector<float> m_distances;
    size_t m_completed = 0;
    float m_tolerance;
  };
} // namespace remix::forward::probes

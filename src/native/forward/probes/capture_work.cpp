#include "forward/probes/capture_work.h"
#include <stdexcept>

namespace remix::forward::probes {
  namespace {
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Reflection capture planning superseded");
    }
  } // namespace
  CaptureWork::CaptureWork(std::span<const Capture> captures, std::stop_token cancelled) {
    check(cancelled);
    if (captures.size() > maxProbes)
      throw std::invalid_argument("Reflection capture budget exceeded");
    if (captures.empty()) return;
    validateCapture(captures.front());
    m_resolution = captures.front().resolution;
    m_texels = 6 * m_resolution * m_resolution;
    for (uint32_t i = 0; i < captures.size(); ++i) {
      check(cancelled);
      const auto &capture = captures[i];
      validateCapture(capture);
      if (capture.resolution != captures.front().resolution || capture.radiance.size() != m_texels)
        throw std::invalid_argument("Inconsistent reflection capture faces");
      uint32_t source = i;
      for (const auto owner : m_owners)
        if (sameCaptureRays(capture, captures[owner])) {
          source = owner;
          break;
        }
      if (source == i) m_owners.push_back(i);
      m_sources.push_back(source);
      m_positions.push_back(capture.position);
    }
    m_rays.reserve(m_owners.size() * m_texels);
    for (const auto owner : m_owners) {
      check(cancelled);
      appendRays(captures[owner], m_rays, cancelled);
    }
  }
  std::pair<uint32_t, uint32_t> CaptureWork::destination(uint32_t ray) const {
    if (ray >= m_rays.size()) throw std::out_of_range("Reflection capture readback ray");
    return {m_owners[ray / m_texels], ray % m_texels};
  }
  void CaptureWork::complete(std::span<Capture> captures, std::stop_token cancelled) const {
    check(cancelled);
    if (captures.size() != m_sources.size())
      throw std::invalid_argument("Reflection capture set changed during transport");
    for (uint32_t i = 0; i < captures.size(); ++i) {
      check(cancelled);
      const auto source = m_sources[i];
      Capture expected;
      expected.position = m_positions[i];
      expected.resolution = m_resolution;
      if (!sameCaptureRays(captures[i], expected) || !sameCaptureRays(captures[i], captures[source]) ||
          captures[source].radiance.size() != m_texels)
        throw std::invalid_argument("Reflection capture alias changed during transport");
    }
    for (uint32_t i = 0; i < captures.size(); ++i) {
      check(cancelled);
      if (m_sources[i] != i) captures[i].radiance = captures[m_sources[i]].radiance;
    }
  }
} // namespace remix::forward::probes

#include "forward/probes/local_capture_work.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace remix::forward::probes {
  LocalCaptureWork::LocalCaptureWork(const scene::BakeScene &source, const CoverageBaselineSet &baseline,
      const ReceiverCoveragePlan &plan, uint32_t resolution, float blendDistance, std::stop_token cancelled)
      : m_context(plan.context) {
    if (!source.ready || !matchesCoverageContext(plan.context, source, baseline) ||
        baseline.disabled || plan.disabledBaseline || !baseline.automaticAllowed ||
        plan.replacements.empty() || plan.replacements.size() > maximumCaptures ||
        plan.replacements.size() > plan.maximumChanges ||
        plan.candidateRecords.size() > 32 * ReceiverSiteFit::maximumVariants || !std::isfinite(plan.maximumError) || plan.maximumError < 0 ||
        plan.candidateRecords.size() != plan.errors.size() || plan.candidateRecords.size() != plan.owners.size())
      throw std::invalid_argument("Invalid or stale local reflection capture plan");
    std::set<uint32_t> selected, destinations;
    for (const auto &replacement : plan.replacements) {
      if (cancelled.stop_requested()) throw std::runtime_error("Local reflection preparation superseded");
      const auto candidate = replacement.candidate;
      if (candidate >= plan.candidateRecords.size() || !selected.insert(candidate).second ||
          !std::isfinite(plan.errors[candidate]) || plan.errors[candidate] < 0 || plan.errors[candidate] > plan.maximumError ||
          !std::isfinite(replacement.gainedArea) || replacement.gainedArea <= 0)
        throw std::invalid_argument("Invalid selected local reflection candidate");
      if (replacement.baseline != UINT32_MAX &&
          (replacement.baseline >= baseline.volumes.size() || !destinations.insert(replacement.baseline).second ||
           baseline.volumes[replacement.baseline].protectedCapture ||
           (baseline.composed && !baseline.volumes[replacement.baseline].source.automatic)))
        throw std::invalid_argument("Local reflection candidate replaces a protected or repeated row");
      const auto &record = plan.candidateRecords[candidate];
      Capture capture;
      capture.resolution = resolution;
      capture.blendDistance = blendDistance;
      capture.worldToLocal = record.worldToLocal;
      capture.bounds.valid = capture.influence.valid = true;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        capture.bounds.minimum[axis] = record.minimum[axis];
        capture.bounds.maximum[axis] = record.maximum[axis];
        capture.influence.minimum[axis] = record.influenceMinimum[axis];
        capture.influence.maximum[axis] = record.influenceMaximum[axis];
        capture.position[axis] = record.position[axis];
      }
      // Priority is assigned by final composition, never narrowed from the
      // internal automatic fallback sentinel into the public Capture range.
      validateCapture(capture);
      m_captures.push_back(std::move(capture));
      m_layers.push_back({candidate, uint32_t(m_captures.size() - 1)});
    }
    const auto free = std::count_if(plan.replacements.begin(), plan.replacements.end(),
        [](const auto &replacement) { return replacement.baseline == UINT32_MAX; });
    if (baseline.volumes.size() + free > maxProbes ||
        (baseline.parameters[0] && baseline.parameters[1] != resolution))
      throw std::invalid_argument("Local reflection captures exceed image capacity or differ in resolution");
    for (auto &capture : m_captures) capture.radiance.resize(6ull * resolution * resolution);
    m_work = CaptureWork(m_captures, cancelled);
    m_seen.resize(m_work.rays().size());
  }
  bool LocalCaptureWork::observe(uint32_t first, std::span<const bake::Result> results) {
    if (m_finished || first > m_seen.size() || results.size() > m_seen.size() - first) return false;
    // Validate the whole readback before changing either seen bits or radiance.
    for (size_t i = 0; i < results.size(); ++i) {
      if (m_seen[first + i]) return false;
      for (float value : results[i].irradiance[0]) if (!std::isfinite(value)) return false;
      if (results[i].irradiance[0][3] < 0) return false;
    }
    for (uint32_t i = 0; i < results.size(); ++i) {
      const auto [capture, texel] = m_work.destination(first + i);
      auto color = results[i].irradiance[0];
      for (uint32_t c = 0; c < 3; ++c) color[c] = std::clamp(color[c], 0.f, 65000.f);
      m_captures[capture].radiance[texel] = color;
      m_seen[first + i] = 1;
    }
    m_received += results.size();
    return true;
  }
  LocalCaptureImage LocalCaptureWork::finish(uint32_t filterSamples, std::stop_token cancelled, WorkerPool *workers) {
    if (m_finished || !complete()) throw std::logic_error("Local reflection capture has pending or consumed readbacks");
    m_work.complete(m_captures, cancelled);
    LocalCaptureImage result;
    result.data = filter(m_captures, filterSamples, cancelled, workers);
    result.selection = {result.data.header, m_context, m_layers};
    m_finished = true;
    m_work = {};
    std::vector<uint8_t>().swap(m_seen);
    std::vector<Capture>().swap(m_captures);
    return result;
  }
}

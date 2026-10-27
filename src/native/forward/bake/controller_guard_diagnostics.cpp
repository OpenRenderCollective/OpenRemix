#include "forward/bake/job.h"
#include "forward/probes/coverage_guards.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace remix::forward::bake {
  bool Controller::recordCoverageGuardDiagnostics(VkCommandBuffer command, const Config &config) {
    const char *request = std::getenv("REMIX_DEBUG_PROBE_GUARD_VISIBILITY");
    if (!request || !*request) return false;
    auto &job = *m_job;
    static std::atomic_uint64_t generation = 0;
    if (!job.receiverGuardsPlanned) {
      job.receiverGuardsPlanned = true;
      const auto owner = m_job;
      job.ticket = m_pipelines->prepare("forward-guard-plan-" + std::to_string(++generation),
          [owner, path = std::string(request)] {
        try {
          if (!owner->receiverFits || std::filesystem::exists(path)) return;
          const auto baseline = probes::automaticCoverageBaseline(owner->captures);
          auto guards = probes::coverageGuardPoints(baseline.volumes, owner->cancellation.get_token());
          const auto fits = owner->receiverFits->finish(*owner->source, owner->cancellation.get_token());
          auto pairs = probes::coverageGuardPairs(guards, fits, .002f / owner->source->sceneScale,
                                                32768, owner->cancellation.get_token());
          owner->receiverGuardCoverage = std::make_unique<probes::ReceiverAudit>(
              std::move(pairs), .001f / owner->source->sceneScale);
          owner->receiverGuardPoints = std::move(guards);
        } catch (const std::exception &error) {
          std::fprintf(stderr, "Reflection guard audit unavailable: %s\n", error.what());
        }
      });
      return true;
    }
    if (!job.receiverGuardCoverage || job.receiverGuardsFinished) return false;
    const auto rays = job.receiverGuardCoverage->rays();
    if (job.receiverGuardReceived < rays.size()) {
      if (job.receiverGuardNext < rays.size()) {
        auto &submission = m_submissions[m_slot];
        if (submission.job) throw std::logic_error("Reflection guard slot reused before its fence");
        const uint32_t count = job.count(Work::Geometry, config, uint32_t(rays.size() - job.receiverGuardNext), 1);
        job.transport->recordGeometry(command, m_slot, rays.subspan(job.receiverGuardNext, count));
        submission.job = m_job;
        submission.receiverGuardCoverage = true;
        submission.first = job.receiverGuardNext;
        submission.count = count;
        job.receiverGuardNext += count;
      }
      status(REMIXAPI_BAKE_TRACING_EXT, "Checking optional reflection preservation guards");
      return true;
    }
    job.receiverGuardsFinished = true;
    const auto owner = m_job;
    job.ticket = m_pipelines->prepare("forward-guard-report-" + std::to_string(++generation),
        [owner, path = std::string(request)] {
      try {
        if (!owner->receiverGuardCoverage->complete() || std::filesystem::exists(path))
          throw std::runtime_error("Incomplete guard audit or report already exists");
        std::ofstream output(path);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.precision(9);
        output << "{\"version\":1,\"sceneHash\":\"" << owner->source->contentHash
               << "\",\"scope\":\"Sampled geometry only; no replacement authorization\",\"guards\":[";
        for (size_t i = 0; i < owner->receiverGuardPoints.size(); ++i) {
          if (owner->cancellation.stop_requested()) throw std::runtime_error("Guard report superseded");
          const auto &p = owner->receiverGuardPoints[i];
          output << (i ? "," : "") << '[' << p[0] << ',' << p[1] << ',' << p[2] << ']';
        }
        output << "],\"pairs\":[";
        const auto &audit = *owner->receiverGuardCoverage;
        for (size_t i = 0; i < audit.pairs().size(); ++i) {
          if (owner->cancellation.stop_requested()) throw std::runtime_error("Guard report superseded");
          const auto &pair = audit.pairs()[i];
          const auto visibility = audit.visibility(i);
          const char *state = visibility == probes::ReceiverVisibility::Visible ? "visible" :
                              visibility == probes::ReceiverVisibility::Blocked ? "blocked" : "unknown";
          output << (i ? "," : "") << "{\"guard\":" << pair.receiver << ",\"attempt\":" << pair.capture
                 << ",\"visibility\":\"" << state << "\",\"distance\":";
          if (std::isfinite(audit.distance(i))) output << audit.distance(i);
          else output << "null";
          output << ",\"targetDistance\":" << pair.ray.distance << '}';
        }
        output << "]}\n";
      } catch (const std::exception &error) {
        std::fprintf(stderr, "Reflection guard report unavailable: %s\n", error.what());
      }
      owner->receiverGuardCoverage.reset();
      owner->receiverGuardPoints.clear();
    });
    return true;
  }
}

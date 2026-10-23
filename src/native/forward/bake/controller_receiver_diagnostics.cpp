#include "forward/bake/job.h"
#include "forward/probes/capture_dump.h"
#include "runtime/pipelines.h"
#include <atomic>
#include <stdexcept>

namespace remix::forward::bake {
  bool Controller::recordReceiverDiagnostics(VkCommandBuffer command, const Config &config) {
    static std::atomic<uint64_t> receiverDiagnosticGeneration{0};
    auto &job = *m_job;
    if (job.receiverAudit) {
      const auto rays = job.receiverAudit->rays();
      if (job.receiverReceived == rays.size()) {
        const auto owner = m_job;
        const bool retry = !job.receiverRetried;
        job.receiverRetried = true;
        job.ticket = m_pipelines->prepare("forward-receiver-report-" + std::to_string(++receiverDiagnosticGeneration), [owner, retry] {
          if (retry && probes::retryReceiverAudit(*owner->source, owner->captures, *owner->receiverAudit,
                                                 owner->cancellation.get_token())) return;
          probes::dumpReceiverAudit(*owner->source, owner->captures, *owner->receiverAudit,
                                   owner->cancellation.get_token());
          owner->receiverSites = probes::prepareReceiverSites(*owner->source, *owner->receiverAudit,
                                                             owner->cancellation.get_token());
          owner->receiverAudit.reset();
        });
      } else if (job.receiverNext < rays.size()) {
        auto &submission = m_submissions[m_slot];
        if (submission.job) throw std::logic_error("Receiver audit slot reused before its fence");
        const uint32_t count = job.count(Work::Geometry, config, uint32_t(rays.size() - job.receiverNext), 1);
        job.transport->recordGeometry(command, m_slot, rays.subspan(job.receiverNext, count));
        submission.job = m_job;
        submission.reflectionReceiver = true;
        submission.first = job.receiverNext;
        submission.count = count;
        job.receiverNext += count;
      }
      status(REMIXAPI_BAKE_TRACING_EXT, "Checking optional receiver-to-capture visibility");
      return true;
    }
    if (job.receiverSites) {
      const auto points = job.receiverSites->points();
      if (job.receiverSiteReceived == points.size()) {
        if (!job.receiverChecksPlanned) {
          job.receiverChecksPlanned = true;
          const auto owner = m_job;
          job.ticket = m_pipelines->prepare("forward-receiver-check-plan-" + std::to_string(++receiverDiagnosticGeneration), [owner] {
            owner->receiverSiteChecks = probes::prepareReceiverSiteChecks(*owner->source, *owner->receiverSites,
                                                                         owner->cancellation.get_token());
          });
          return true;
        }
        if (job.receiverSiteChecks && job.receiverCheckReceived < job.receiverSiteChecks->rays().size()) {
          const auto rays = job.receiverSiteChecks->rays();
          if (job.receiverCheckNext < rays.size()) {
            auto &submission = m_submissions[m_slot];
            if (submission.job) throw std::logic_error("Receiver check slot reused before its fence");
            const uint32_t count = job.count(Work::Geometry, config, uint32_t(rays.size() - job.receiverCheckNext), 1);
            job.transport->recordGeometry(command, m_slot, rays.subspan(job.receiverCheckNext, count));
            submission.job = m_job;
            submission.receiverSiteCheck = true;
            submission.first = job.receiverCheckNext;
            submission.count = count;
            job.receiverCheckNext += count;
          }
          status(REMIXAPI_BAKE_TRACING_EXT, "Checking local reflection site connectivity and clearance");
          return true;
        }
        const auto owner = m_job;
        job.ticket = m_pipelines->prepare("forward-receiver-sites-" + std::to_string(++receiverDiagnosticGeneration), [owner] {
          probes::dumpReceiverSites(*owner->source, *owner->receiverSites, owner->receiverSiteChecks.get(),
                                   owner->cancellation.get_token());
          owner->receiverSiteChecks.reset();
          owner->receiverSites.reset();
        });
      } else if (job.receiverSiteNext < points.size()) {
        auto &submission = m_submissions[m_slot];
        if (submission.job) throw std::logic_error("Receiver site slot reused before its fence");
        const uint32_t count = job.count(Work::Site, config, uint32_t(points.size() - job.receiverSiteNext),
                                        probes::ReceiverSiteAudit::samples);
        job.transport->recordSiteValidation(command, m_slot, points.subspan(job.receiverSiteNext, count),
                                            probes::ReceiverSiteAudit::samples);
        submission.job = m_job;
        submission.receiverSite = true;
        submission.first = job.receiverSiteNext;
        submission.count = count;
        job.receiverSiteNext += count;
      }
      status(REMIXAPI_BAKE_TRACING_EXT, "Validating optional local reflection sites");
      return true;
    }
    return false;
  }
} // namespace remix::forward::bake

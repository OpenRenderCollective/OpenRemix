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
        if (!job.receiverFitsPlanned) {
          job.receiverFitsPlanned = true;
          const auto owner = m_job;
          job.ticket = m_pipelines->prepare("forward-receiver-fit-plan-" + std::to_string(++receiverDiagnosticGeneration), [owner, detailTolerance = config.reflectionDetailTolerance] {
            if (owner->receiverSiteChecks)
              owner->receiverFits = probes::prepareReceiverFits(*owner->source, *owner->receiverSites,
                  *owner->receiverSiteChecks, detailTolerance, owner->cancellation.get_token());
          });
          return true;
        }
        if (job.receiverFits && job.receiverFitReceived < job.receiverFits->rays().size()) {
          const auto rays = job.receiverFits->rays();
          if (job.receiverFitNext < rays.size()) {
            auto &submission = m_submissions[m_slot];
            if (submission.job) throw std::logic_error("Local projection slot reused before its fence");
            const uint32_t count = job.count(Work::Geometry, config, uint32_t(rays.size() - job.receiverFitNext), 1);
            job.transport->recordGeometry(command, m_slot, rays.subspan(job.receiverFitNext, count));
            submission.job = m_job;
            submission.receiverFit = true;
            submission.first = job.receiverFitNext;
            submission.count = count;
            job.receiverFitNext += count;
          }
          status(REMIXAPI_BAKE_TRACING_EXT, "Assessing optional local reflection projections");
          return true;
        }
        if (!job.receiverCoveragePlanned) {
          job.receiverCoveragePlanned = true;
          const auto owner = m_job;
          job.ticket = m_pipelines->prepare("forward-local-coverage-plan-" + std::to_string(++receiverDiagnosticGeneration), [owner] {
            if (owner->receiverFits)
              owner->receiverFitCoverage = probes::prepareReceiverFitCoverage(*owner->source, owner->captures,
                  *owner->receiverFits, owner->cancellation.get_token());
          });
          return true;
        }
        if (job.receiverFitCoverage && job.receiverCoverageReceived < job.receiverFitCoverage->rays().size()) {
          const auto rays = job.receiverFitCoverage->rays();
          if (job.receiverCoverageNext < rays.size()) {
            auto &submission = m_submissions[m_slot];
            if (submission.job) throw std::logic_error("Local coverage slot reused before its fence");
            const uint32_t count = job.count(Work::Geometry, config, uint32_t(rays.size() - job.receiverCoverageNext), 1);
            job.transport->recordGeometry(command, m_slot, rays.subspan(job.receiverCoverageNext, count));
            submission.job = m_job;
            submission.receiverFitCoverage = true;
            submission.first = job.receiverCoverageNext;
            submission.count = count;
            job.receiverCoverageNext += count;
          }
          status(REMIXAPI_BAKE_TRACING_EXT, "Checking optional local reflection receiver coverage");
          return true;
        }
        const auto owner = m_job;
        job.ticket = m_pipelines->prepare("forward-receiver-sites-" + std::to_string(++receiverDiagnosticGeneration), [owner] {
          probes::dumpReceiverSites(*owner->source, *owner->receiverSites, owner->receiverSiteChecks.get(), owner->receiverFits.get(), owner->receiverFitCoverage.get(),
                                   owner->cancellation.get_token());
          owner->receiverFitCoverage.reset();
          owner->receiverFits.reset();
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

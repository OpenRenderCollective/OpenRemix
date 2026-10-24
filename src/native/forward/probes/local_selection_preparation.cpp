#include "forward/probes/local_selection_preparation.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace remix::forward::probes {
  struct LocalSelectionPreparation::Job {
    pipeline::PreparationQueue::Ticket ticket;
    std::stop_source cancellation;
    CoveragePlanContext context;
    std::shared_ptr<const ImageAsset> baseline, local;
    std::unique_ptr<SelectionUpload> upload;
    std::array<uint64_t, 2> memory{};
  };
  LocalSelectionPreparation::LocalSelectionPreparation(VkPhysicalDevice physical, VkDevice device, pipeline::Service &service)
      : m_physical(physical), m_device(device), m_service(service) {}
  LocalSelectionPreparation::~LocalSelectionPreparation() { cancel(); }
  void LocalSelectionPreparation::cancel() {
    if (!m_job) return;
    m_job->cancellation.request_stop();
    m_service.cancel(m_job->ticket);
    m_job.reset();
  }
  void LocalSelectionPreparation::start(std::shared_ptr<const scene::BakeScene> source, ReflectionSnapshot baseline,
      std::shared_ptr<const ReceiverCoverageEvidence> evidence, std::shared_ptr<const CoverageReadyImage> ready,
      std::shared_ptr<const ImageAsset> local) {
    cancel();
    if (!source || !baseline.image || !evidence || !ready || !local || ready->layers.empty())
      throw std::invalid_argument("Local selection preparation requires immutable input owners");
    auto job = std::make_shared<Job>();
    job->context = coveragePlanContext(*source, baseline.baseline);
    job->baseline = baseline.image;
    job->local = local;
    const auto physical = m_physical;
    const auto device = m_device;
    static std::atomic_uint64_t generation = 0;
    job->ticket = m_service.prepare("forward-local-selection-" + std::to_string(++generation),
        [job, source, baseline = std::move(baseline), evidence, ready, physical, device] {
      const auto began = std::chrono::steady_clock::now();
      const auto token = job->cancellation.get_token();
      if (token.stop_requested()) throw std::runtime_error("Local selection superseded");
      if (!matchesCoverageContext(ready->context, *source, ready->context.baseline))
        throw std::invalid_argument("Prepared local radiance belongs to another frozen scene");
      std::vector<uint32_t> eligible;
      for (const auto &layer : ready->layers) eligible.push_back(layer.candidate);
      // Restrict before greedy selection; dropping replacements afterwards can
      // invalidate another replacement's sampled preservation proof.
      const auto plan = planReceiverCoverage(*source, baseline.baseline, *evidence, token, eligible);
      if (plan.replacements.empty()) return;
      auto rebound = *ready;
      rebound.context = plan.context;
      const auto imagePlan = buildCoverageImagePlan(*source, baseline.baseline, plan,
          baseline.image->header(), baseline.identities, rebound);
      if (token.stop_requested()) throw std::runtime_error("Local selection superseded");
      job->upload = std::make_unique<SelectionUpload>(physical, device, imagePlan, job->baseline, job->local);
      const auto bytes = job->upload->memory();
      job->memory = bytes;
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
      std::fprintf(stderr, "Local reflection worker selection: %zu replacements, %u output layers, %.3f ms preparation\n",
          plan.replacements.size(), imagePlan.header.parameters[0], ms);
    });
    m_job = std::move(job);
  }
  std::unique_ptr<SelectionUpload> LocalSelectionPreparation::poll(const scene::BakeScene &source,
      const ReflectionSnapshot &baseline, const std::shared_ptr<const ImageAsset> &local) {
    if (!m_job) return {};
    if (!matchesCoverageContext(m_job->context, source, baseline.baseline) ||
        m_job->baseline != baseline.image || m_job->local != local) {
      cancel();
      return {};
    }
    if (!m_job->ticket->ready()) return {};
    auto job = std::move(m_job);
    job->ticket->check();
    return std::move(job->upload);
  }
  std::array<uint64_t, 2> LocalSelectionPreparation::memory() const {
    return m_job && m_job->ticket->ready() ? m_job->memory : std::array<uint64_t, 2>{};
  }
}

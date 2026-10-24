#pragma once
#include "forward/probes/reflection_snapshot.h"
#include "forward/probes/selection_upload.h"
#include "runtime/pipelines.h"

namespace remix::forward::probes {
  // One optional local-selection request. Workers own immutable input snapshots;
  // only poll transfers completed GPU preparation back to the render thread.
  class LocalSelectionPreparation {
  public:
    LocalSelectionPreparation(VkPhysicalDevice physical, VkDevice device, pipeline::Service &service);
    ~LocalSelectionPreparation();
    LocalSelectionPreparation(const LocalSelectionPreparation &) = delete;
    LocalSelectionPreparation &operator=(const LocalSelectionPreparation &) = delete;
    void start(std::shared_ptr<const scene::BakeScene> source, ReflectionSnapshot baseline,
        std::shared_ptr<const ReceiverCoverageEvidence> evidence, std::shared_ptr<const CoverageReadyImage> ready,
        std::shared_ptr<const ImageAsset> local);
    std::unique_ptr<SelectionUpload> poll(const scene::BakeScene &source, const ReflectionSnapshot &baseline,
                                         const std::shared_ptr<const ImageAsset> &local);
    void cancel();
    bool pending() const { return bool(m_job); }
    std::array<uint64_t, 2> memory() const;
  private:
    struct Job;
    VkPhysicalDevice m_physical;
    VkDevice m_device;
    pipeline::Service &m_service;
    std::shared_ptr<Job> m_job;
  };
}

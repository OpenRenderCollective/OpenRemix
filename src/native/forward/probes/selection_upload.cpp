#include "forward/probes/selection_upload.h"
#include <stdexcept>

namespace remix::forward::probes {
  SelectionUpload::SelectionUpload(VkPhysicalDevice physical, VkDevice device, const CoverageImagePlan &plan,
      std::shared_ptr<const ImageAsset> baseline, std::shared_ptr<const ImageAsset> local)
      : m_baseline(std::move(baseline)), m_local(std::move(local)) {
    if (!m_baseline || !m_local || !plan.context.valid)
      throw std::invalid_argument("Reflection selection requires retained sources and context");
    m_output = std::make_shared<ImageAsset>(physical, device, plan);
    m_publication = {m_output, plan.identities, plan.context};
  }
  bool SelectionUpload::record(VkCommandBuffer command, const scene::BakeScene &source,
                               const CoverageBaselineSet &baseline) {
    if (m_state != State::Prepared) throw std::logic_error("Reflection selection is not pending recording");
    if (!matchesCoverageContext(m_publication.context, source, baseline)) {
      m_state = State::Cancelled;
      m_baseline.reset();
      m_local.reset();
      m_output.reset();
      m_publication.image.reset();
      return false;
    }
    m_output->recordSelection(command, *m_baseline, *m_local);
    m_state = State::Recorded;
    return true;
  }
  std::optional<SelectionPublication> SelectionUpload::retire(const scene::BakeScene &source,
                                                              const CoverageBaselineSet &baseline) {
    if (m_state != State::Recorded) throw std::logic_error("Reflection selection has no submitted copy to retire");
    const bool current = matchesCoverageContext(m_publication.context, source, baseline);
    m_output->releaseUpload();
    m_baseline.reset();
    m_local.reset();
    m_output.reset();
    m_state = State::Retired;
    if (current) return std::move(m_publication);
    m_publication.image.reset();
    return std::nullopt;
  }
  void SelectionFrames::offer(std::unique_ptr<SelectionUpload> upload) {
    if (!upload) throw std::invalid_argument("Missing reflection selection upload");
    m_pending = {std::move(upload), ++m_generation};
  }
  void SelectionFrames::selectFrame(uint32_t slot) {
    auto &frame = m_frames.at(slot);
    m_slot = slot;
    if (frame.copy.upload) m_completed.push_back(std::move(frame.copy));
    frame = {};
  }
  bool SelectionFrames::needsContext() const {
    if (m_pending.upload || m_active || !m_completed.empty()) return true;
    for (const auto &frame : m_frames) if (frame.copy.upload) return true;
    return false;
  }
  void SelectionFrames::record(VkCommandBuffer command, const scene::BakeScene &source,
                               const CoverageBaselineSet &baseline) {
    auto &frame = m_frames.at(m_slot);
    if (frame.recorded) throw std::logic_error("Reflection selection slot has not retired its fence");
    if (m_active && !matchesCoverageContext(m_active->context, source, baseline)) m_active.reset();
    for (auto &copy : m_completed) {
      auto publication = copy.upload->retire(source, baseline);
      if (publication && copy.generation == m_generation)
        m_active = std::make_shared<SelectionPublication>(std::move(*publication));
    }
    m_completed.clear();
    if (m_pending.upload) {
      if (m_pending.upload->record(command, source, baseline)) frame.copy = std::move(m_pending);
      m_pending = {};
    }
    frame.sampled = m_active;
    frame.recorded = true;
  }
}

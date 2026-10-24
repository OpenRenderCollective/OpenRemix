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
}

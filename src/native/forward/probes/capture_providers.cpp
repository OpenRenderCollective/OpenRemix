#include "forward/probes/capture_providers.h"
#include "forward/bake/controller.h"
#include "raster/renderer.h"

namespace remix::forward::probes {
  CaptureProviders::CaptureProviders(std::shared_ptr<Authoring> authored,
                                     std::shared_ptr<StandaloneCapture> standalone)
      : m_authored(std::move(authored)), m_standalone(std::move(standalone)),
        m_standaloneUsed(m_standalone->wasUsed()) {}
  void CaptureProviders::activate() { m_standaloneUsed |= m_standalone->wasUsed(); }
  void CaptureProviders::selectFrame(uint32_t slot) {
    m_slot = slot;
    m_authored->selectFrame(slot);
    if (m_standaloneUsed) m_standalone->selectFrame(slot);
  }
  void CaptureProviders::record(VkCommandBuffer command, RasterRenderer &resources, const Options &options,
                                const techniques::ResourcePlan &plan, const CaptureConfig &authoredConfig,
                                bake::SceneRequest *sceneRequest, CaptureSource captures, bool sharedTransportIdle) {
    const bool capturesWithBake = plan.capturesWithBake();
    if (!capturesWithBake || m_standaloneUsed) {
      if (!m_standaloneUsed) {
        m_standaloneUsed = true;
        m_standalone->selectFrame(m_slot);
      }
      auto automaticConfig = authoredConfig;
      automaticConfig.enabled = automaticConfig.enabled && !capturesWithBake;
      m_standalone->record(command, resources, automaticConfig,
          options.baking.reflectionProbes, options.baking.reflectionDetailTolerance, sceneRequest);
    }
    // Image availability is not transport admission. Keep one default work
    // budget until diffuse/style work retires, even after early image publication.
    const bool capturesReady = capturesWithBake
        ? sharedTransportIdle : !m_standalone->pending();
    if (!capturesWithBake) {
      captures = m_standalone->source();
      const auto scene = resources.bakeScene();
      captures.sceneHash = scene && scene->ready ? scene->contentHash : 0;
    }
    m_authored->record(command, resources, authoredConfig, captures,
                        options.authoredReflectionFallback, capturesReady);
  }
  CaptureSource CaptureProviders::source(const techniques::ResourcePlan &plan,
                                         const bake::Controller &baking) const {
    if (m_standaloneUsed && !plan.capturesWithBake()) return m_standalone->source();
    return baking.reflectionSource();
  }
  Descriptors CaptureProviders::descriptor(const techniques::ResourcePlan &plan,
                                           const bake::Controller &baking) const {
    auto automatic = baking.reflectionDescriptor();
    if (m_standaloneUsed && !plan.capturesWithBake()) {
      const auto &standalone = m_standalone->source();
      if (standalone.image) automatic = standalone.image->descriptors();
    }
    return m_authored->descriptor(automatic);
  }
  const Header *CaptureProviders::header(const techniques::ResourcePlan &plan,
                                        const bake::Controller &baking) const {
    if (const auto authored = m_authored->header()) return authored;
    if (m_standaloneUsed && !plan.capturesWithBake()) {
      const auto &standalone = m_standalone->source();
      if (standalone.image) return &standalone.image->header();
    }
    return baking.reflectionHeader();
  }
  float CaptureProviders::sceneScale(const techniques::ResourcePlan &plan,
                                     const bake::Controller &baking) const {
    if (const auto &volume = baking.volume()) return volume->sceneScale;
    return m_standaloneUsed && !plan.capturesWithBake() ? m_standalone->sceneScale()
                                                      : baking.reflectionSceneScale();
  }
}

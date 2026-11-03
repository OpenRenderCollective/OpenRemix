#include "forward/probes/capture_providers.h"
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
  void CaptureProviders::record(VkCommandBuffer command, RasterRenderer &resources, const CapturePolicy &policy,
                                const CaptureConfig &authoredConfig,
                                bake::SceneRequest *sceneRequest, CaptureSource captures, bool sharedTransportIdle) {
    const bool sharedCaptures = policy.transport == CaptureTransport::SharedScene;
    if (!sharedCaptures || m_standaloneUsed) {
      if (!m_standaloneUsed) {
        m_standaloneUsed = true;
        m_standalone->selectFrame(m_slot);
      }
      auto automaticConfig = authoredConfig;
      automaticConfig.enabled = automaticConfig.enabled && !sharedCaptures;
      m_standalone->record(command, resources, automaticConfig,
          policy.automaticCount, policy.detailTolerance, sceneRequest);
    }
    // Image availability is not transport admission. Keep one default work
    // budget until diffuse/style work retires, even after early image publication.
    const bool capturesReady = sharedCaptures
        ? sharedTransportIdle : !m_standalone->pending();
    if (!sharedCaptures) {
      captures = m_standalone->source();
      const auto scene = resources.bakeScene();
      captures.sceneHash = scene && scene->ready ? scene->contentHash : 0;
    }
    m_authored->record(command, resources, authoredConfig, captures,
                        policy.authoredFallback, capturesReady);
  }
}

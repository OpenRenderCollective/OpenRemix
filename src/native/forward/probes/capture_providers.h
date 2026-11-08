#pragma once
#include "forward/probes/authoring.h"
#include "forward/probes/capture_config.h"
#include "forward/probes/standalone_capture.h"

namespace remix::forward::probes {
  // Captured reflection scheduling and publication selection. The Source 2
  // adapter lends its diffuse transport budget; providers retain their own jobs.
  class CaptureProviders {
  public:
    CaptureProviders() = default;
    CaptureProviders(std::shared_ptr<Authoring> authored, std::shared_ptr<StandaloneCapture> standalone);
    const std::shared_ptr<Authoring> &authored() const { return m_authored; }
    const std::shared_ptr<StandaloneCapture> &standalone() const { return m_standalone; }
    bool standaloneUsed() const { return m_standaloneUsed; }
    // Replacement activation follows queue idle. The old renderer may have used
    // a shared provider after the replacement worker took its initial snapshot.
    void activate();
    void selectFrame(uint32_t slot);
    // Request sharing before the selected diffuse provider records work. The
    // caller then supplies its publication and transport admission explicitly.
    bake::SceneRequest *requestScene(RasterRenderer &resources, const CaptureConfig &config) {
      return m_authored->requestScene(resources, config);
    }
    void record(VkCommandBuffer command, RasterRenderer &resources, const CapturePolicy &policy,
                const CaptureConfig &config, bake::SceneRequest *sceneRequest,
                CaptureSource captures, bool sharedTransportIdle);
    // Borrowed publication view; the provider/frame owners retain the image.
    const CaptureSource *standaloneSource(CaptureTransport transport) const {
      return m_standaloneUsed && transport != CaptureTransport::SharedScene ? &m_standalone->source() : nullptr;
    }
    // Select before binding so replaced providers do not prepare unused descriptors.
    // The provider and frame owners retain this borrowed immutable image.
    const ImageAsset *image(CaptureTransport transport) const {
      if (const auto authored = m_authored->image()) return authored;
      if (const auto source = standaloneSource(transport)) return source->image.get();
      return nullptr;
    }
    float standaloneSceneScale() const { return m_standalone->sceneScale(); }
    // Preserve renderer teardown order: authored workers retire before bake
    // ownership; standalone frame owners remain retained until normal teardown.
    void releaseAuthoring() { m_authored.reset(); }
  private:
    std::shared_ptr<Authoring> m_authored;
    std::shared_ptr<StandaloneCapture> m_standalone;
    bool m_standaloneUsed = false;
    uint32_t m_slot = 0;
  };
}

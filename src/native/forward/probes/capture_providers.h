#pragma once
#include "forward/probes/authoring.h"
#include "forward/probes/standalone_capture.h"
#include "forward/provider_plan.h"

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
    void record(VkCommandBuffer command, RasterRenderer &resources, const Options &options,
                const techniques::ResourcePlan &plan, const CaptureConfig &config,
                bake::SceneRequest *sceneRequest, CaptureSource captures, bool sharedTransportIdle);
    // Borrowed publication view; the provider/frame owners retain the image.
    const CaptureSource *standaloneSource(const techniques::ResourcePlan &plan) const {
      return m_standaloneUsed && !plan.capturesWithBake() ? &m_standalone->source() : nullptr;
    }
    Descriptors descriptor(const techniques::ResourcePlan &plan, Descriptors fallback) const {
      if (const auto source = standaloneSource(plan); source && source->image)
        fallback = source->image->descriptors();
      return m_authored->descriptor(fallback);
    }
    const Header *header(const techniques::ResourcePlan &plan) const {
      if (const auto authored = m_authored->header()) return authored;
      if (const auto source = standaloneSource(plan); source && source->image)
        return &source->image->header();
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

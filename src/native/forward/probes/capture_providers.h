#pragma once
#include "forward/probes/authoring.h"
#include "forward/probes/standalone_capture.h"
#include "forward/provider_plan.h"

namespace remix::forward::bake { class Controller; }
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
    void record(VkCommandBuffer command, RasterRenderer &resources, const Options &options,
                const techniques::ResourcePlan &plan, bake::Controller &baking);
    CaptureSource source(const techniques::ResourcePlan &plan, const bake::Controller &baking) const;
    Descriptors descriptor(const techniques::ResourcePlan &plan, const bake::Controller &baking) const;
    const Header *header(const techniques::ResourcePlan &plan, const bake::Controller &baking) const;
    float sceneScale(const techniques::ResourcePlan &plan, const bake::Controller &baking) const;
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

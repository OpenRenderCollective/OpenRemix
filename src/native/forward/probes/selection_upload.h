#pragma once
#include "forward/probes/coverage_image_plan.h"
#include "forward/probes/image_asset.h"
#include <optional>
#include "platform/gpu_frames.h"

namespace remix::forward::probes {
  struct SelectionPublication {
    std::shared_ptr<const ImageAsset> image;
    LayerIdentities identities{};
    CoveragePlanContext context;
  };
  // Render-thread copy transaction. Retain this object in the submitted frame
  // slot until its fence completes, including when an edit supersedes the plan.
  // Aliasing shared_ptr owners may retain an enclosing bake or authored job.
  class SelectionUpload {
  public:
    SelectionUpload(VkPhysicalDevice physical, VkDevice device, const CoverageImagePlan &plan,
                    std::shared_ptr<const ImageAsset> baseline, std::shared_ptr<const ImageAsset> local);
    SelectionUpload(const SelectionUpload &) = delete;
    SelectionUpload &operator=(const SelectionUpload &) = delete;
    // False means stale before submission; no commands were recorded.
    bool record(VkCommandBuffer command, const scene::BakeScene &source, const CoverageBaselineSet &baseline);
    // Call only after the copy submission's fence. Stale work releases its
    // retained sources but cannot produce a publication. This does not wait.
    std::optional<SelectionPublication> retire(const scene::BakeScene &source, const CoverageBaselineSet &baseline);
  private:
    enum class State { Prepared, Recorded, Retired, Cancelled };
    State m_state = State::Prepared;
    SelectionPublication m_publication;
    std::shared_ptr<ImageAsset> m_output;
    std::shared_ptr<const ImageAsset> m_baseline, m_local;
  };
  // One render queue and the renderer's existing fenced frame slots. Call
  // selectFrame only after that slot's fence; record then supplies current scene
  // metadata before any completed copy can replace the active publication.
  class SelectionFrames {
  public:
    void offer(std::unique_ptr<SelectionUpload> upload);
    void selectFrame(uint32_t slot);
    void record(VkCommandBuffer command, const scene::BakeScene &source, const CoverageBaselineSet &baseline);
    const SelectionPublication *active() const { return m_active.get(); }
    bool needsContext() const;
  private:
    struct Copy {
      std::unique_ptr<SelectionUpload> upload;
      uint64_t generation = 0;
    };
    struct Frame {
      Copy copy;
      std::shared_ptr<const SelectionPublication> sampled;
      bool recorded = false;
    };
    uint32_t m_slot = 0;
    uint64_t m_generation = 0;
    Copy m_pending;
    std::array<Frame, gpuFrameCount> m_frames;
    std::vector<Copy> m_completed;
    std::shared_ptr<const SelectionPublication> m_active;
  };
}

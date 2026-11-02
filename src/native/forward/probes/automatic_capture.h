#pragma once
#include "forward/probes/capture_work.h"
#include "forward/probes/capture_config.h"
#include "forward/probes/capture_payload.h"
#include "forward/probes/placement_seeds.h"
#include "forward/probes/evaluation.h"
#include "forward/probes/image_asset.h"
#include <stdexcept>
#include <atomic>
#include <optional>

namespace remix::forward::bake { class Transport; class Budget; }

namespace remix::forward::probes {
  // Automatic reflection work, independent of diffuse atlases and probe values.
  // Workers advance placement/filtering; the scheduler advances submission and
  // readback counters only after the owning frame fence. No copied ray arrays.
  struct AutomaticCapture {
    enum class Step { PlanSites, ValidateSites, Fit, CheckGeometry, Select, Capture, Complete };
    // Inspect only after the owning preparation ticket or submission fence.
    Step nextStep() const {
      if (!sitesPlanned) return Step::PlanSites;
      if (siteReceived < reflectionSites.size()) return Step::ValidateSites;
      if (!candidatesPlanned) return Step::Fit;
      if (projectionAudit && geometryReceived < projectionAudit->rays().size()) return Step::CheckGeometry;
      if (!capturePlanned) return Step::Select;
      return captureReceived < work.rays().size() ? Step::Capture : Step::Complete;
    }
    struct Dispatch {
      Step step;
      uint32_t first = 0, count = 0;
    };
    // Borrows transport and adaptive budget; creates no second scene or ray list.
    // The caller retains this owner and consumes readback after the slot fence.
    Dispatch record(VkCommandBuffer command, uint32_t slot, bake::Transport &transport,
                     bake::Budget &budget, const CaptureConfig &config);
    // Consume each submitted range exactly once, after its fence. Ranges may
    // finish out of order. Completion state belongs to the
    // provider, so another scheduler need not understand capture storage.
    bool observe(Step step, uint32_t first, std::span<const bake::Result> results);
    struct Placement {
      uint32_t requested = 0, capacity = 0, resolution = 128;
      float detailTolerance = .5f;
    };
    // Worker-only CPU transition. GPU admission/projection stages must retire
    // before preparation can continue. Admitted seeds are borrowed from another
    // provider only at PlanSites; otherwise geometry candidates are generated.
    Step prepareNext(const scene::BakeScene &source, const Placement &placement,
                     std::stop_token cancelled,
                     std::optional<PlacementSeeds> admittedSeeds = std::nullopt);
    static constexpr uint32_t siteSamples = 64;
    void planSites(const scene::BakeScene &source, PlacementSeeds seeds,
                    uint32_t count, std::stop_token cancelled);
    // Standalone planning uses geometry candidates, then admits them with the
    // same hardware site queries. It never needs diffuse probes or SH results.
    void planSites(const scene::BakeScene &source, uint32_t count, std::stop_token cancelled);
    void fit(const scene::BakeScene &source, uint32_t count,
              float detailTolerance, std::stop_token cancelled);
    void select(const scene::BakeScene &source, uint32_t count, uint32_t resolution,
                 float detailTolerance, std::stop_token cancelled);
    void restoreProjection(const scene::BakeScene &source, float detailTolerance,
                            std::stop_token cancelled);
    void prepareRays(std::stop_token cancelled);
    void restoreCapture(std::vector<Capture> cached, Data data, std::stop_token cancelled = {});
    const std::vector<Capture> &captureView() const { return m_payload ? m_payload->captures : captures; }
    const Data &filteredView() const { return m_payload ? m_payload->data : filtered; }
    // Worker-only completion boundary. Moves vector storage once; subsequent
    // readers/persistence retain the immutable payload without copying arrays.
    const std::shared_ptr<const CapturePayload> &retainPayload() {
      if (!m_payload) m_payload = std::make_shared<CapturePayload>(CapturePayload{std::move(captures), std::move(filtered)});
      return m_payload;
    }
    bool cachedRadiance = false;
    void complete(std::stop_token cancelled); // retain provenance once, then release ray scratch
    void filter(VkPhysicalDevice physical, VkDevice device, std::stop_token cancelled);
    PlacementSeeds seeds;
    std::vector<bake::Point> reflectionSites;
    std::vector<uint8_t> usableReflectionSites;
    uint32_t siteNext = 0, siteReceived = 0;
    bool sitesPlanned = false;
    bool validateSeeds = false;
    uint32_t seedSiteOffset = 0;
    std::vector<FittedRegion> candidateRegions;
    std::unique_ptr<ProjectionAudit> projectionAudit;
    uint32_t geometryNext = 0, geometryReceived = 0;
    bool candidatesPlanned = false;
    std::vector<Capture> captures;
    CaptureWork work;
    uint64_t inputHash = 0;
    Data filtered;
    std::shared_ptr<ImageAsset> image;
    // Worker completion handoff. After sealing, the worker must not change the
    // image. CPU payload ownership is independent and may already be retained.
    void sealImage();
    // Render-thread only; returns an owner only for the first upload. The caller
    // retains it in the submitted frame until that frame's fence completes.
    std::shared_ptr<ImageAsset> recordImageUpload(VkCommandBuffer command);

    uint32_t captureNext = 0, captureReceived = 0;
    bool capturePlanned = false;
  private:
    std::shared_ptr<const CapturePayload> m_payload;
    std::atomic_bool m_imageReady = false;
    bool m_imageUploaded = false;
    void requirePreparing() const {
      if (m_payload || m_imageReady.load(std::memory_order_relaxed))
        throw std::logic_error("Completed reflection capture is immutable");
    }
  };
}

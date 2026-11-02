#pragma once
#include "forward/probes/capture_work.h"
#include "forward/probes/readback_receipts.h"
#include "forward/probes/capture_config.h"
#include "forward/probes/capture_step.h"
#include "forward/probes/capture_content.h"
#include "forward/probes/capture_payload.h"
#include "forward/probes/placement_seeds.h"
#include "forward/probes/evaluation.h"
#include "forward/probes/image_asset.h"
#include "runtime/pipelines.h"
#include <stdexcept>
#include <atomic>
#include <optional>

namespace remix::forward::bake { class Transport; class Budget; enum class Work : uint32_t; }

namespace remix::forward::probes::capture_cache { struct Planned; }

namespace remix::forward::probes {
  // Automatic reflection work, independent of diffuse atlases and probe values.
  // Workers advance placement/filtering; the scheduler advances submission and
  // readback counters only after the owning frame fence. No copied ray arrays.
  struct AutomaticCapture {
    using Step = CaptureStep;
    // Render-thread worker ownership. The preset lends transport/admission,
    // not its diffuse preparation ticket. Submitted callbacks retain the owner.
    void prepare(pipeline::Service &service, const std::string &key, std::function<void()> work);
    bool preparationReady() const { return !m_preparation || m_preparation->ready(); }
    void checkPreparation() const { if (m_preparation) m_preparation->check(); }
    void cancelPreparation(pipeline::Service &service) { if (m_preparation) service.cancel(m_preparation); }
    // Inspect only after the owning preparation ticket or submission fence.
    Step nextStep() const {
      if (!sitesPlanned) return Step::PlanSites;
      if (siteReceived < reflectionSites.size()) return Step::ValidateSites;
      if (!candidatesPlanned) return Step::Fit;
      if (projectionAudit && geometryReceived < projectionAudit->rays().size()) return Step::CheckGeometry;
      if (!capturePlanned) return Step::Select;
      return captureReceived < work.rays().size() ? Step::Capture : Step::Complete;
    }
    struct Progress {
      uint32_t completed = 0;
      size_t total = 0;
    };
    // Scheduler diagnostics inspect work units, not placement/ray storage.
    // As with nextStep(), call only after the preparation/readback handoff.
    Progress progress(Step step) const {
      if (step == Step::ValidateSites) return {siteReceived, reflectionSites.size()};
      if (step == Step::CheckGeometry)
        return {geometryReceived, projectionAudit ? projectionAudit->rays().size() : 0};
      if (step == Step::Capture) return {captureReceived, work.rays().size()};
      return {};
    }
    static bake::Work workType(Step step);
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
    bool observe(Dispatch dispatch, std::span<const bake::Result> results) {
      return dispatch.count && results.size() == dispatch.count &&
             observe(dispatch.step, dispatch.first, results);
    }
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
    // Worker-only selected-capture cache lookup, then ray generation on a miss.
    // Returns the independent cache identity, or zero when persistence is absent.
    uint64_t prepareRadiance(const CaptureConfig &config, const capture_cache::Planned &cache,
                              std::stop_token cancelled);
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
    void prepareImage(VkPhysicalDevice physical, VkDevice device, std::stop_token cancelled);
    void prepareContent(std::stop_token cancelled); // optional worker diagnostics
    // Worker-only finishing after complete(): both publish the image once.
    // The default preset retains immutable CPU data for diffuse provenance/cache;
    // standalone capture transfers its nonempty payload without copying arrays
    // or calculating unused diffuse provenance. Same allocation count as before.
    void finishRetainedPublication(VkPhysicalDevice physical, VkDevice device, std::stop_token cancelled);
    // Nonempty cached/nonpersistent capture: release CPU arrays before handoff,
    // without allocating a persistence payload or computing content diagnostics.
    void finishImageOnlyPublication(VkPhysicalDevice physical, VkDevice device, std::stop_token cancelled);
    std::unique_ptr<const CapturePayload> finishTransferredPublication(
        VkPhysicalDevice physical, VkDevice device, std::stop_token cancelled);
    CaptureContent content; // published by the same release/acquire image handoff
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
    // image. A retained empty payload completes without allocating an image.
    // CPU payload ownership is independent and may already be retained.
    void sealPublication();
    // Render-thread only; uploads a nonempty image once. Empty completion records
    // no command and returns no owner. Retain a returned owner until its fence.
    std::shared_ptr<ImageAsset> recordImageUpload(VkCommandBuffer command);

    uint32_t captureNext = 0, captureReceived = 0;
    bool capturePlanned = false;
  private:
    ReadbackReceipts m_siteReceipts, m_captureReceipts;
    pipeline::PreparationQueue::Ticket m_preparation;
    std::shared_ptr<const CapturePayload> m_payload;
    std::atomic_bool m_publicationReady = false;
    bool m_publicationConsumed = false;
    bool emptyRadiance() const {
      return captureView().empty() && filteredView().header.parameters[0] == 0 && filteredView().texels.empty();
    }
    void requirePreparing() const {
      if (m_payload || m_publicationReady.load(std::memory_order_relaxed))
        throw std::logic_error("Completed reflection capture is immutable");
    }
  };
}

#pragma once
#include "forward/bake/controller.h"
#include "forward/bake/adaptive_grid.h"
#include "forward/bake/budget.h"
#include "forward/bake/coarse_transfer.h"
#include "forward/bake/transport.h"
#include "forward/bake/reference_capture.h"
#include "forward/bake/style_work.h"
#include "forward/bake/local_reflection_cache.h"
#include "forward/bake/styled_direct.h"
#include "forward/bake/probe_visibility.h"
#include "forward/bake/sun_antialiasing.h"
#include "forward/mixed/overflow.h"
#include "forward/mixed/direct_work.h"
#include "forward/mixed/unified_masks.h"
#include "forward/mixed/directional_fields.h"
#include "forward/mixed/directional_capture.h"
#include "forward/probes/evaluation.h"
#include "forward/probes/capture_work.h"
#include "forward/probes/local_capture_work.h"
#include "forward/probes/receiver_audit.h"
#include "forward/probes/receiver_sites.h"
#include "forward/probes/receiver_site_checks.h"
#include "forward/probes/receiver_fits.h"
#include "forward/probes/receiver_coverage_plan.h"
#include <atomic>
#include <chrono>
#include <filesystem>

namespace remix::forward::bake::cache { struct Snapshot; }

namespace remix::forward::bake {
  struct Controller::Job {
    std::shared_ptr<const scene::BakeScene> source;
    pipeline::PreparationQueue::Ticket ticket;
    std::unique_ptr<Transport> transport;
    std::optional<ReferenceCapture> reference;
    Budget budget;
    Grid grid;
    std::shared_ptr<SunDepth> sunDepth;
    uint32_t sunDepthNext = 0, sunDepthReceived = 0;
    bool sunDepthLogged = false;
    bool sunDepthCacheSubmitted = false, sunDepthCacheReady = false, sunDepthCacheWritten = false;
    double sunDepthGpuMs = 0;
    uint32_t probeDepthNext = 0, probeDepthReceived = 0;
    std::optional<CoarseTransfer> coarseTransfer;
    std::shared_ptr<const std::vector<uint32_t>> coarseTransferWords;
    struct TransferSlot { std::unique_ptr<Buffer> output, readback; };
    std::array<TransferSlot, gpuFrameCount> coarseTransferSlots;
    bool coarseTransferPlanned = false, coarseTransferFinished = false;
    uint32_t coarseTransferNext = 0, coarseTransferReceived = 0;
    std::chrono::steady_clock::time_point coarseTransferBegan;
    double coarseTransferPlanningMs = 0, coarseTransferReadbackMs = 0, coarseTransferGpuMs = 0;
    Refinement refinement;
    bool refinementPlanned = false;
    bool finalProbePass = false; // a separate quality pass must not drive adaptive placement
    void refineProbes(const Config &config);
    lightmap::Layout surfaceLayout;
    std::vector<Result> surfaces;
    bool surfaceDonorsResolved = false;
    std::unique_ptr<StyleWork> styles;
    std::vector<uint32_t> styleWords; // original indirect-only cache data
    StyledDirectGroups groupedStyles; // disposable derived publication data
    void prepareGroupedStyles(uint64_t storageRange, const Config &config);
    std::optional<ProbeVisibility> localProbes;
    std::vector<VisibilityTask> localProbeTasks;
    bool localProbesPlanned = false;
    void prepareProbeVisibility(uint64_t storageRange, float cutoff, bool groupedDirect);
    uint32_t localProbeNext = 0, localProbeReceived = 0;
    bool baseFinished = false;
    std::vector<Result> readback; // reused between completed frame slots; avoids clearing every batch
    std::vector<VisibilityResult> visibilityReadback;
    std::vector<float> overflowReadback;
    double readbackMs = 0;
    struct OverflowTiming {
      double roundsMs = 0, submissionMs = 0, readbackMs = 0, gpuMs = 0;
      uint64_t rounds = 0, batches = 0, points = 0;
    } overflowTiming;
    uint64_t readbackBytes = 0;
    std::shared_ptr<mixed::Plan> mixedLights;
    std::optional<mixed::UnifiedMasks> unifiedMasks;
    std::vector<uint32_t> directionalWords;
    std::unique_ptr<mixed::DirectionalCapture> directionalCapture;
    std::vector<uint32_t> directionalPending; // local sample indices not already supplied by retained fields
    bool directionalAttempted = false, completeDirectionals = false;
    uint32_t directionalNext = 0, directionalReceived = 0, directionalCount = 0;
    uint32_t directionalField = 0, directionalFields = 0;
    uint32_t directionalReusedFields = 0;
    uint64_t directionalTracedPoints = 0;
    uint64_t directionalBudget = 0;
    struct DirectionalTiming {
      std::chrono::steady_clock::time_point began;
      double workerMs = 0, submissionMs = 0, readbackMs = 0, gpuMs = 0;
      uint64_t batches = 0;
    } directionalTiming;
    void logDirectionalTiming() const;
    void advanceDirectionals(bool finish);
    void prepareSurfaceStorage();
    std::vector<Vec4> visibility;
    std::vector<lightmap::Direct> direct;
    std::optional<mixed::DirectWork> directWork;
    uint64_t directWorkBytes = 0;
    bool directPlanned = false, directFinished = false;
    uint32_t directNext = 0, directReceived = 0;
    std::vector<lightmap::Sun> sun;
    bool hasDirectDiffuse = false, hasSun = false; // survive release of the completed CPU arrays
    std::filesystem::path cacheFile;
    uint64_t cacheKey = 0;
    std::unique_ptr<const cache::Snapshot> takeCacheSnapshot();
    bool cacheHit = false;
    uint32_t sunNext = 0, sunReceived = 0;
    std::unique_ptr<SunAntialiasing> sunAntialiasing;
    bool sunAntialiasingPlanned = false, sunAntialiasingFinished = false;
    uint32_t sunAntialiasingNext = 0, sunAntialiasingReceived = 0;
    double sunAntialiasingCpuMs = 0; // edge planning and submission; readback is included in readbackMs
    double sunAntialiasingGpuMs = 0;
    std::vector<uint32_t> probeSunTasks;
    bool probeSunPlanned = false;
    uint32_t probeSunNext = 0, probeSunReceived = 0;
    uint32_t visibilityNext = 0, visibilityReceived = 0;
    uint64_t visibilityPoints = 0;
    bool visibilityFinished = false;
    std::unique_ptr<mixed::OverflowAudit> overflowAudit;
    uint32_t overflowNext = 0, overflowReceived = 0;
    bool overflowFinished = false, overflowPreparing = false;
    std::vector<uint16_t> surfacePixels;
    std::shared_ptr<lightmap::Geometry> surfaceGeometry;
    uint32_t surfaceNext = 0, surfaceReceived = 0;
    std::vector<Point> reflectionSites;
    std::vector<uint8_t> usableReflectionSites;
    uint32_t siteNext = 0, siteReceived = 0;
    bool sitesPlanned = false;
    std::vector<probes::FittedRegion> candidateRegions;
    std::unique_ptr<probes::ProjectionAudit> projectionAudit;
    uint32_t geometryNext = 0, geometryReceived = 0;
    bool candidatesPlanned = false;
    std::vector<probes::Capture> captures;
    std::unique_ptr<probes::ReceiverAudit> receiverAudit;
    uint32_t receiverNext = 0, receiverReceived = 0;
    bool receiverRetried = false;
    std::unique_ptr<probes::ReceiverSiteAudit> receiverSites;
    uint32_t receiverSiteNext = 0, receiverSiteReceived = 0;
    std::unique_ptr<probes::ReceiverSiteChecks> receiverSiteChecks;
    uint32_t receiverCheckNext = 0, receiverCheckReceived = 0;
    bool receiverChecksPlanned = false, receiverFitsPlanned = false;
    std::unique_ptr<probes::ReceiverFitAudit> receiverFits;
    uint32_t receiverFitNext = 0, receiverFitReceived = 0;
    bool receiverCoveragePlanned = false;
    std::unique_ptr<probes::ReceiverAudit> receiverFitCoverage;
    uint32_t receiverCoverageNext = 0, receiverCoverageReceived = 0;
    bool receiverGuardsPlanned = false, receiverGuardsFinished = false;
    std::unique_ptr<probes::ReceiverAudit> receiverGuardCoverage;
    std::vector<Vec3> receiverGuardPoints;
    uint32_t receiverGuardNext = 0, receiverGuardReceived = 0;
    std::shared_ptr<const probes::ReceiverCoverageEvidence> reflectionEvidence;
    std::unique_ptr<probes::LocalCaptureWork> localCaptureWork;
    std::shared_ptr<LocalReflections> localReflections;
    std::shared_ptr<const local_cache::Snapshot> localReflectionCache;
    void restoreLocalReflections(VkPhysicalDevice physical, VkDevice device, const Config &config);
    void prepareLocalReflectionUpgrade(VkPhysicalDevice physical, VkDevice device, const Config &config, uint32_t family);
    bool localReflectionUpgrade = false;
    uint32_t localCaptureNext = 0, localCaptureReceived = 0;
    bool localCapturePlanned = false, localCaptureFiltered = false;
    probes::CaptureWork captureWork;
    uint64_t captureInputHash = 0;
    probes::Data specular;
    std::unique_ptr<probes::ImageAsset> reflectionImage;
    std::stop_source cancellation;
    uint32_t captureNext = 0, captureReceived = 0;
    bool capturePlanned = false, filtering = false;
    enum class FinishStage { None, ProbeRefinement, CoarseTransfer, Reflections, Denoising, Atlas, Geometry, Styles };
    std::atomic<FinishStage> finishStage{FinishStage::None};
    std::atomic<uint32_t> denoisedCharts{0}, denoiseChartCount{0};
    uint32_t next = 0, received = 0;
    bool initialized = false, prepared = false, failed = false;
    void prepareResources(VkPhysicalDevice physical, VkDevice device, const Config &config, float lightCutoff,
                          uint32_t queueFamily);
    void finishCaptureInputs(); // worker-only: retain provenance, release ray scratch
    enum class Visibility {
      Local, Direct, DirectionalField, Overflow, SurfaceSun, SurfaceSunAntialiasing, ProbeSun, ProbeLocal
    };
    bool consumeVisibility(uint32_t slot, uint32_t first, uint32_t count, Visibility kind);
    uint32_t count(Work work, const Config &config, uint32_t remaining, uint32_t samples,
                   uint32_t bounces = 1) {
      return budget.count(work, config.batch, transport->timingSupported() ? config.gpuBudgetMs : 0, samples,
                          bounces, remaining);
    }
  };
} // namespace remix::forward::bake

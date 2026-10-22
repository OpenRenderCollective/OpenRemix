#pragma once
#include "forward/bake/controller.h"
#include "forward/bake/adaptive_grid.h"
#include "forward/bake/budget.h"
#include "forward/bake/transport.h"
#include "forward/bake/reference_capture.h"
#include "forward/bake/style_work.h"
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
    uint32_t probeDepthNext = 0, probeDepthReceived = 0;
    Refinement refinement;
    bool refinementPlanned = false;
    void refineProbes(const Config &config);
    lightmap::Layout surfaceLayout;
    std::vector<Result> surfaces;
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
    probes::CaptureWork captureWork;
    probes::Data specular;
    std::stop_source cancellation;
    uint32_t captureNext = 0, captureReceived = 0;
    bool capturePlanned = false, filtering = false;
    enum class FinishStage { None, ProbeRefinement, Reflections, Denoising, Atlas, Geometry, Styles };
    std::atomic<FinishStage> finishStage{FinishStage::None};
    std::atomic<uint32_t> denoisedCharts{0}, denoiseChartCount{0};
    uint32_t next = 0, received = 0;
    bool initialized = false, prepared = false, failed = false;
    void prepareResources(VkPhysicalDevice physical, VkDevice device, const Config &config, float lightCutoff,
                          uint32_t queueFamily);
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

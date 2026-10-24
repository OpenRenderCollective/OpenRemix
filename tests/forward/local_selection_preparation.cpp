#include "forward/probes/local_selection_preparation.h"
#include "gpu_fixture.h"
#include <algorithm>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

using namespace remix::forward;
using namespace remix::forward::test;

namespace {
  void queuedCancellationCase() {
    Fixture fixture(false);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(fixture.physical, &properties);
    remix::pipeline::Service service(fixture.device, properties, false);
    probes::Capture capture;
    capture.bounds.include({-1,-1,-1}); capture.bounds.include({1,1,1}); capture.resolution = 8;
    probes::Data data;
    data.header.parameters = {1,8,4,510}; data.header.probes[0] = probes::projectionRecord(capture);
    data.texels.resize(510, {1,2,3,1});
    probes::ReflectionSnapshot baseline;
    baseline.image = std::make_shared<probes::ImageAsset>(fixture.physical, fixture.device, data);
    baseline.baseline = probes::automaticCoverageBaseline(baseline.image->header());
    auto source = std::make_shared<remix::scene::BakeScene>();
    source->sceneId = 100; source->revision = 1; source->ready = true;
    auto ready = std::make_shared<probes::CoverageReadyImage>();
    ready->header = data.header; ready->context = probes::coveragePlanContext(*source, baseline.baseline); ready->layers = {{0,0}};
    auto evidence = std::make_shared<probes::ReceiverCoverageEvidence>();
    evidence->sourceContext = probes::coveragePlanContext(*source, {});
    auto local = std::make_shared<probes::ImageAsset>(fixture.physical, fixture.device, data);
    // Serial preparation behind a held gate makes cancellation deterministic.
    // Reaching release proves that start/poll/cancel never wait for this worker.
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto releaseFuture = release.get_future();
    service.prepare("local-selection-test-gate", [&] { entered.set_value(); releaseFuture.wait(); });
    require(enteredFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "Preparation test gate starts");
    {
      probes::LocalSelectionPreparation preparation(fixture.physical, fixture.device, service);
      preparation.start(source, baseline, evidence, ready, local);
      require(preparation.pending() && preparation.memory() == std::array<uint64_t, 2>{},
              "Queued local selection is pending without blocking for worker allocation");
      auto changed = *source; ++changed.revision;
      require(!preparation.poll(changed, baseline, local) && !preparation.pending(),
              "Changed source cancels queued selection without waiting for the worker");
      preparation.start(source, baseline, evidence, ready, local);
      auto replacedBaseline = baseline;
      replacedBaseline.image = local;
      require(!preparation.poll(*source, replacedBaseline, local) && !preparation.pending(),
              "Same-metadata baseline image replacement cancels queued preparation");
      preparation.start(source, baseline, evidence, ready, local);
      require(!preparation.poll(*source, baseline, baseline.image) && !preparation.pending(),
              "Same-metadata local image replacement cancels queued preparation");
      preparation.start(source, baseline, evidence, ready, local);
      preparation.cancel();
      require(!preparation.pending(), "Explicit cancellation discards queued selection");
      preparation.start(source, baseline, evidence, ready, local);
    }
    release.set_value();
    service.stop();
    require(validationErrors == 0, "Preparation cancellation and shutdown are validation clean");
  }

  enum class Change { None, Scene, Map, SubmittedMap, Authored, BaselineImage, LocalImage, Cancel, InvalidImageMetadata };
  void completedPreparationCase(Change change) {
    Fixture fixture(false);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(fixture.physical, &properties);
    remix::pipeline::Service service(fixture.device, properties, false);
    auto source = std::make_shared<remix::scene::BakeScene>();
    source->sceneId = 100; source->revision = 1; source->contentHash = 7; source->ready = true;
    auto surface = std::make_shared<remix::scene::Surface>();
    for (const Vec3 point : {Vec3{0,0,0}, Vec3{1,0,0}, Vec3{0,1,0}}) {
      remix::scene::Vertex vertex{};
      std::copy_n(point.begin(), 3, vertex.position);
      vertex.normal[2] = 1;
      surface->vertices.push_back(vertex);
    }
    surface->indices = {0,1,2};
    remix::scene::Draw draw;
    draw.surface = surface;
    source->draws.push_back(draw);
    source->charts.emplace_back().texcoords.resize(3);

    probes::Capture capture;
    capture.bounds.include({-1,-1,-1}); capture.bounds.include({1,1,1});
    capture.position = {.25f,.25f,.5f}; capture.resolution = 8;
    probes::Data data;
    data.header.parameters = {1,8,4,510};
    data.header.probes[0] = probes::projectionRecord(capture);
    data.texels.resize(510, {1,2,3,0});
    auto baselineImage = std::make_shared<probes::ImageAsset>(fixture.physical, fixture.device, probes::Data{});
    auto local = std::make_shared<probes::ImageAsset>(fixture.physical, fixture.device, data);
    fixture.begin();
    baselineImage->recordUpload(fixture.command);
    local->recordUpload(fixture.command);
    fixture.finish();
    baselineImage->releaseUpload(); local->releaseUpload();
    probes::ReflectionSnapshot baseline;
    baseline.image = baselineImage;
    baseline.baseline = probes::automaticCoverageBaseline(baselineImage->header());

    auto evidence = std::make_shared<probes::ReceiverCoverageEvidence>();
    evidence->sourceContext = probes::coveragePlanContext(*source, {});
    evidence->fits.available = true;
    probes::FittedRegion geometry;
    geometry.bounds = capture.bounds; geometry.site.position = capture.position; geometry.geometryEvaluated = true;
    for (auto &face : geometry.faces) face.solidAngleFraction = 1.f / 6;
    probes::ReceiverProjection variant;
    variant.projection = variant.influence = capture.bounds;
    variant.coversReceiver = true; variant.geometry = geometry;
    evidence->fits.attempts.push_back({0, {variant}});
    const auto receivers = probes::sampleReceivers(*source, 8192);
    require(!receivers.empty(), "Completed preparation fixture has uncovered receiver area");
    for (uint32_t i = 0; i < receivers.size(); ++i)
      evidence->observations.push_back({i, 0, probes::ReceiverVisibility::Visible});
    auto ready = std::make_shared<probes::CoverageReadyImage>();
    ready->header = data.header;
    ready->context = probes::coveragePlanContext(*source, baseline.baseline);
    ready->layers = {{0,0}};
    if (change == Change::InvalidImageMetadata) ++ready->header.parameters[0];

    probes::LocalSelectionPreparation preparation(fixture.physical, fixture.device, service);
    preparation.start(source, baseline, evidence, ready, local);
    // This serial marker follows the worker: it proves preparation has finished
    // before an edit arrives, without consuming the prepared upload via poll.
    auto after = service.prepare("after-local-selection", [] {});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!after->ready() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(after->ready(), "Local selection worker reaches its completion marker");
    after->check();
    if (change == Change::InvalidImageMetadata) {
      bool refused = false;
      try { (void)preparation.poll(*source, baseline, local); }
      catch (const std::invalid_argument &) { refused = true; }
      require(refused && !preparation.pending() && preparation.memory() == std::array<uint64_t,2>{},
              "Worker rejects radiance metadata that does not belong to its retained image");
      service.stop();
      return;
    }
    require(preparation.memory()[1] > 0, "Completed worker prepared an actual replacement image");
    if (change == Change::Scene) ++source->revision;
    if (change == Change::Map) ++source->sceneId;
    if (change == Change::Authored) {
      baseline.baseline.composed = true;
      baseline.baseline.authoredSceneId = source->sceneId;
      baseline.baseline.authoredRevision = 1;
    }
    if (change == Change::BaselineImage)
      baseline.image = std::make_shared<probes::ImageAsset>(fixture.physical, fixture.device, probes::Data{});
    if (change == Change::LocalImage)
      local = std::make_shared<probes::ImageAsset>(fixture.physical, fixture.device, data);
    if (change == Change::Cancel) preparation.cancel();
    auto upload = preparation.poll(*source, baseline, local);
    require(bool(upload) == (change == Change::None || change == Change::SubmittedMap) && !preparation.pending(),
            "Only current completed work transfers a prepared upload to the renderer");
    require(preparation.memory() == std::array<uint64_t,2>{}, "Preparation releases transferred or superseded memory");
    if (upload) {
      std::weak_ptr<const probes::ImageAsset> oldBaseline = baselineImage, oldLocal = local;
      probes::SelectionFrames frames;
      frames.offer(std::move(upload));
      baselineImage.reset(); baseline.image.reset(); local.reset();
      frames.selectFrame(0);
      fixture.begin(); frames.record(fixture.command, *source, baseline.baseline); fixture.finish();
      require(!frames.active() && !oldBaseline.expired() && !oldLocal.expired(),
              "Worker-produced copy retains both source images until its submission retires");
      if (change == Change::SubmittedMap) ++source->sceneId;
      frames.selectFrame(0);
      fixture.begin(); frames.record(fixture.command, *source, baseline.baseline); fixture.finish();
      require(bool(frames.active()) == (change != Change::SubmittedMap) &&
                  (!frames.active() || frames.active()->image->header().parameters[0] == 1) &&
                  oldBaseline.expired() && oldLocal.expired(),
              "Retirement publishes current copies, discards old-map copies and releases their source owners");
    }
    service.stop();
    require(validationErrors == 0, "Completed preparation and edit lifecycle are validation clean");
  }
}
void testLocalSelectionPreparation() {
  queuedCancellationCase();
  for (auto change : {Change::None, Change::Scene, Change::Map, Change::SubmittedMap, Change::Authored, Change::BaselineImage,
                      Change::LocalImage, Change::Cancel, Change::InvalidImageMetadata})
    completedPreparationCase(change);
}

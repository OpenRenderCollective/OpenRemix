#include "forward/bake/controller.h"
#include "forward/probes/automatic_capture.h"
#include "forward/probes/cache_index.h"
#include "forward/probes/authoring.h"
#include "forward/bake/cache.h"
#include "forward/bake/transport.h"
#include "forward/bake/budget.h"
#include "forward/bake/cache_persistence.h"
#include <cstdlib>
#include <optional>
#include <string_view>
#include <fstream>
#include "forward/lightmap/denoise.h"
#include "gpu_fixture.h"
#include "raster/renderer.h"
#include "runtime/pipelines.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace remix;
using namespace remix::forward;
using test::require;
namespace {
std::mutex mutex;
std::condition_variable wake;
bool entered = false, released = false;
std::atomic_uint32_t denoiseCalls = 0;
struct RecoveryDirectory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("remix-reflection-recovery-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::optional<std::string> previous;
  RecoveryDirectory() {
    if (const char *value = std::getenv("REMIX_BAKE_CACHE_DIR")) previous = value;
    if (!std::filesystem::create_directory(path))
      throw std::runtime_error("Recovery fixture directory already exists");
    set(path.string().c_str());
  }
  static void set(const char *value) {
#ifdef _WIN32
    _putenv_s("REMIX_BAKE_CACHE_DIR", value ? value : "");
#else
    if (value) setenv("REMIX_BAKE_CACHE_DIR", value, 1);
    else unsetenv("REMIX_BAKE_CACHE_DIR");
#endif
  }
  ~RecoveryDirectory() {
    bake::cache::persistence().waitIdle();
    set(previous ? previous->c_str() : nullptr);
    // This exact, newly created fixture directory is the only cleanup target.
    if (path.parent_path() == std::filesystem::temp_directory_path() &&
        path.filename().string().starts_with("remix-reflection-recovery-")) {
      std::error_code ignored; std::filesystem::remove_all(path, ignored);
    }
  }
};
void resetGate() {
  std::lock_guard lock(mutex);
  entered = released = false;
}
void releaseGate() {
  {
    std::lock_guard lock(mutex);
    released = true;
  }
  wake.notify_all();
}
} // namespace
// Test double only: pause diffuse finishing without changing any production
// scheduling code. Actual denoiser correctness has separate GPU/CPU tests.
namespace remix::forward::lightmap {
uint32_t denoiserVersion() { return 0x7fffffffu; }
DenoiseStatistics denoise(const Layout &, std::span<bake::Result>,
                          std::stop_token cancelled,
                          std::function<void(uint32_t, uint32_t)>,
                          HostTransfers, FilterDispatch) {
  ++denoiseCalls;
  std::unique_lock lock(mutex);
  entered = true;
  wake.notify_all();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (!released && !cancelled.stop_requested() &&
         std::chrono::steady_clock::now() < deadline)
    wake.wait_for(lock, std::chrono::milliseconds(5));
  if (!released)
    throw std::runtime_error("Diffuse test worker cancelled or timed out");
  return {};
}
} // namespace remix::forward::lightmap
int main() try {
  {
    test::Fixture fixture;
    using CaptureStep = probes::AutomaticCapture::Step;
    require(probes::AutomaticCapture::workType(CaptureStep::ValidateSites) == bake::Work::Site &&
                probes::AutomaticCapture::workType(CaptureStep::CheckGeometry) == bake::Work::Geometry &&
                probes::AutomaticCapture::workType(CaptureStep::Capture) == bake::Work::Reflection,
            "Provider query stages changed adaptive timing categories");
    bool invalidTiming = false;
    try { (void)probes::AutomaticCapture::workType(CaptureStep::Fit); }
    catch (const std::logic_error &) { invalidTiming = true; }
    require(invalidTiming, "CPU preparation stage was accepted as GPU timing work");
    {
      probes::AutomaticCapture capture;
      require(!capture.recordImageUpload(VK_NULL_HANDLE), "Unready image was published");
      bool rejected = false;
      try { capture.sealPublication(); } catch (const std::logic_error &) { rejected = true; }
      require(rejected, "Missing image was sealed");
      capture.image = std::make_shared<probes::ImageAsset>(fixture.physical, fixture.device, probes::Data{});
      capture.sealPublication();
      rejected = false;
      try { capture.sealPublication(); } catch (const std::logic_error &) { rejected = true; }
      require(rejected, "Image completion was repeated");
      fixture.begin();
      const auto upload = capture.recordImageUpload(fixture.command);
      require(upload == capture.image && !capture.recordImageUpload(fixture.command),
              "Provider did not upload its image exactly once");
      fixture.finish();
    }
    {
      probes::AutomaticCapture empty;
      // An empty provider must not require a device or allocate fallback GPU data.
      empty.finishRetainedPublication(VK_NULL_HANDLE, VK_NULL_HANDLE, {});
      require(!empty.image, "Empty captures allocated a reflection image");
      require(!empty.recordImageUpload(VK_NULL_HANDLE) && !empty.recordImageUpload(VK_NULL_HANDLE),
              "Empty completion attempted a GPU upload");
      bool rejected = false;
      try { empty.sealPublication(); } catch (const std::logic_error &) { rejected = true; }
      require(rejected, "Empty reflection completion was repeated");
      probes::AutomaticCapture emptyTransfer;
      rejected = false;
      try { emptyTransfer.finishTransferredPublication(VK_NULL_HANDLE, VK_NULL_HANDLE, {}); }
      catch (const std::logic_error &) { rejected = true; }
      require(rejected && !emptyTransfer.image, "Empty transfer acquired GPU resources instead of requiring retained completion");
      probes::AutomaticCapture incomplete;
      incomplete.captures.emplace_back();
      incomplete.retainPayload();
      rejected = false;
      try { incomplete.sealPublication(); } catch (const std::logic_error &) { rejected = true; }
      require(rejected, "Nonempty captures completed without an image");
      std::stop_source cancelled;
      cancelled.request_stop();
      probes::AutomaticCapture superseded;
      rejected = false;
      try { superseded.prepareImage(VK_NULL_HANDLE, VK_NULL_HANDLE, cancelled.get_token()); }
      catch (const std::runtime_error &) { rejected = true; }
      require(rejected, "Cancelled empty publication was accepted");
    }
    {
      namespace storage = probes::capture_cache;
      probes::Capture source;
      source.bounds.include({-1,-1,-1}); source.bounds.include({1,1,1});
      source.influence = source.bounds; source.resolution = 8;
      source.radiance.resize(384, {1,2,3,4});
      {
        probes::AutomaticCapture transfer;
        transfer.captures.push_back(source);
        transfer.filtered = probes::filter(transfer.captures, 8, {});
        transfer.cachedRadiance = true;
        transfer.complete({});
        const auto captures = transfer.captures.data();
        const auto rays = transfer.captures.front().radiance.data();
        const auto texels = transfer.filtered.texels.data();
        auto payload = transfer.finishTransferredPublication(fixture.physical, fixture.device, {});
        require(payload && payload->captures.data() == captures && payload->captures.front().radiance.data() == rays &&
                    payload->data.texels.data() == texels && transfer.captureView().empty() &&
                    transfer.filteredView().texels.empty(),
                "Transferred publication copied or retained CPU capture/filter arrays");
        payload.reset(); // Upload ownership must not depend on the persistence payload.
        fixture.begin();
        const auto image = transfer.recordImageUpload(fixture.command);
        require(image && !transfer.recordImageUpload(fixture.command),
                "Transferred reflection publication did not upload exactly once after CPU release");
        fixture.finish();
      }
      const auto directory = std::filesystem::temp_directory_path() /
          ("remix-provider-cache-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      probes::CaptureConfig config; config.reflectionResolution = 8;
      storage::Planned request{directory, 123, 456, {1,2,3,{1,8,4096,256,128u << 20}}, false, {}};
      const auto plan = probes::CaptureWork::planHash(std::span(&source, 1));
      const bake::cache::Device legacyDevice{1,2,3,4096,256,128u << 20,{}};
      require(bake::cache::reflectionKey(123, plan, std::span(&source, 1), config, legacyDevice, false) ==
                  storage::plannedKey(123, bake::Transport::shaderFingerprint(), plan,
                                      std::span(&source, 1), config, request.device, false),
              "Provider transport fingerprint changed legacy planned cache identity");
      probes::AutomaticCapture first;
      first.sitesPlanned = first.candidatesPlanned = first.capturePlanned = true; first.captures.push_back(source);
      const auto miss = first.prepareRadiance(config, request, {});
      require(miss && !first.cachedRadiance && first.work.rays().size() == 384,
              "Cache miss did not prepare the selected capture rays");
      bool repeated = false;
      try { first.prepareRadiance(config, request, {}); }
      catch (const std::logic_error &) { repeated = true; }
      require(repeated, "Active capture rays were prepared twice");
      const auto data = probes::filter(std::span(&source, 1), 8);
      const auto blob = storage::storeShared(directory, std::span(&source, 1), data);
      require(blob && probes::cache_index::writeShared(directory, miss, blob), "Provider cache fixture failed");
      probes::AutomaticCapture warm;
      warm.sitesPlanned = warm.candidatesPlanned = warm.capturePlanned = true; warm.captures.push_back(source);
      require(warm.prepareRadiance(config, request, {}) == miss && warm.cachedRadiance &&
                  warm.work.rays().empty() && warm.filteredView().texels == data.texels,
              "Warm provider lookup retraced rays or lost filtered content");
      probes::AutomaticCapture changed;
      changed.sitesPlanned = changed.candidatesPlanned = changed.capturePlanned = true; changed.captures.push_back(source);
      ++request.shaders;
      require(changed.prepareRadiance(config, request, {}) != miss && !changed.cachedRadiance &&
                  changed.work.rays().size() == 384, "Changed transport shader reused stale captures");
      std::filesystem::remove(probes::cache_index::path(directory, miss));
      std::filesystem::remove(storage::sharedPath(directory, blob));
      std::filesystem::remove(directory / "reflections" / "shared");
      std::filesystem::remove(directory / "reflections"); std::filesystem::remove(directory);
    }
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(fixture.physical, &properties);
    pipeline::Service pipelines(fixture.device, properties, false);
    {
      struct Gate { std::atomic_bool entered = false, released = false; std::atomic_uint32_t cancelledRuns = 0; };
      auto gate = std::make_shared<Gate>();
      struct Release { std::shared_ptr<Gate> gate; ~Release() { gate->released = true; } } release{gate};
      probes::AutomaticCapture running, queued;
      running.prepare(pipelines, "reflection-worker-ownership-running", [gate] {
        gate->entered = true;
        while (!gate->released) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      });
      const auto enteredDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!gate->entered && std::chrono::steady_clock::now() < enteredDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      require(gate->entered && !running.preparationReady(), "Reflection provider lost its running worker ownership");
      bool rejected = false;
      try { running.prepare(pipelines, "reflection-worker-ownership-overlap", [] {}); }
      catch (const std::logic_error &) { rejected = true; }
      require(rejected, "Reflection worker ticket replaced while its callback was running");
      auto retained = std::make_shared<int>(1);
      std::weak_ptr<int> retired = retained;
      queued.prepare(pipelines, "reflection-worker-ownership-cancelled", [gate, retained] { ++gate->cancelledRuns; });
      retained.reset();
      require(!queued.preparationReady() && !retired.expired(), "Queued reflection callback lost its retained inputs");
      queued.cancelPreparation(pipelines);
      require(queued.preparationReady() && retired.expired() && gate->cancelledRuns == 0,
              "Cancelling queued reflection work retained inputs or executed the callback");
      rejected = false;
      try { queued.checkPreparation(); } catch (const std::runtime_error &) { rejected = true; }
      require(rejected, "Cancelled reflection preparation published successful readiness");
      gate->released = true;
      const auto finishedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!running.preparationReady() && std::chrono::steady_clock::now() < finishedDeadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      require(running.preparationReady(), "Reflection worker completion did not release readiness");
      running.checkPreparation();
    }
    RasterRenderer resources;
    resources.initializeGpu(fixture.physical, fixture.device);
    remixapi_BakeSceneBoundsInfoEXT request{};
    request.scene.structSize = sizeof(request);
    request.scene.version = REMIXAPI_BAKE_SCENE_BOUNDS_VERSION_EXT;
    request.scene.sceneId = request.scene.revision = 1;
    request.scene.sceneScale = .5f;
    request.scene.state = REMIXAPI_BAKE_SCENE_READY_EXT;
    for (uint32_t c = 0; c < 3; ++c) {
      request.minimum[c] = -2;
      request.maximum[c] = 2;
    }
    resources.setBakeScene(&request.scene);
    {
      remixapi_ReflectionVolumeEXT volume{};
      volume.id = 77;
      for (uint32_t c = 0; c < 3; ++c) {
        volume.minimum[c] = -1; volume.maximum[c] = 1; volume.worldToLocal[c][c] = 1;
      }
      remixapi_ReflectionVolumesInfoEXT volumes{sizeof(volumes), REMIXAPI_REFLECTION_VOLUMES_VERSION_EXT,
          request.scene.sceneId, 1, 1, 0, &volume};
      resources.setReflectionVolumes(&volumes);
      probes::Authoring authored(fixture.physical, fixture.device, &pipelines, true, fixture.family);
      probes::CaptureConfig captureConfig; captureConfig.reflectionResolution = 8;
      captureConfig.samples = captureConfig.bounces = 1;
      const probes::CaptureSource inputs{resources.bakeScene()->contentHash, {}};
      for (uint32_t frame = 0; frame < gpuFrameCount + 1; ++frame) {
        authored.selectFrame(frame % gpuFrameCount);
        fixture.begin(); authored.record(fixture.command, resources, captureConfig, inputs, false, false); fixture.finish();
      }
      require(authored.statistics().pending && authored.statistics().rays == 0 &&
                  authored.memory().buffers == 0 && authored.memory().acceleration == 0,
              "Deferred authored captures consumed a second transport budget");
      const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      uint32_t frame = 0;
      while (!authored.statistics().active && std::chrono::steady_clock::now() < until) {
        authored.selectFrame(frame++ % gpuFrameCount);
        fixture.begin(); authored.record(fixture.command, resources, captureConfig, inputs, false, true); fixture.finish();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      require(authored.statistics().active && authored.statistics().completed == 384,
              "Authored capture did not resume when granted its work budget");
      for (uint32_t slot = 0; slot < gpuFrameCount; ++slot) authored.selectFrame(slot);
      resources.setReflectionVolumes(nullptr);
    }

    bake::Config config;
    config.diskCache = false;
    config.maxProbes = 64;
    config.samples = 1;
    config.bounces = 1;
    config.reflectionProbes = 1;
    config.reflectionResolution = 8;
    config.textureResolution = 16;
    config.batch = 1024;
    config.gpuBudgetMs = 0;
    config.denoiseLightmaps = true;
    {
      RecoveryDirectory directory;
      auto diffuseOnly = config;
      diffuseOnly.reflectionProbes = 0;
      diffuseOnly.diskCache = true;
      diffuseOnly.denoiseLightmaps = false;
      bake::ContentHashes previous{};
      for (uint32_t pass = 0; pass < 2; ++pass) {
        bake::Controller controller(fixture.physical, fixture.device, &pipelines, true, fixture.family);
        uint32_t frame = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do {
          controller.selectFrame(frame++ % gpuFrameCount);
          fixture.begin(); controller.record(fixture.command, resources, diffuseOnly); fixture.finish();
          require(!controller.reflectionSource().image && !controller.reflectionHeader(),
                  "Diffuse-only bake published an unused reflection image");
          require(controller.statistics().state != REMIXAPI_BAKE_FAILED_EXT, "Diffuse-only publication failed");
          if (controller.statistics().state == REMIXAPI_BAKE_READY_EXT) break;
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        require(controller.volume() && controller.statistics().state == REMIXAPI_BAKE_READY_EXT &&
                    controller.reflectionSource().sceneHash == resources.bakeScene()->contentHash,
                "Diffuse publication depended on a reflection image");
        if (pass == 0) previous = controller.content();
        else require(std::string_view(controller.statistics().status).starts_with("Cached lighting active") &&
                         controller.content().grid == previous.grid &&
                         controller.content().surfaces == previous.surfaces,
                     "Warm diffuse-only cache changed lighting or missed persistence");
        for (uint32_t slot = 0; slot < gpuFrameCount; ++slot) controller.selectFrame(slot);
        bake::cache::persistence().waitIdle();
      }
    }
    for (bool cancel : {false, true}) {
      resetGate();
      bake::Controller controller(fixture.physical, fixture.device, &pipelines,
                                  true, fixture.family);
      uint32_t frame = 0;
      auto tick = [&](const bake::Config &settings) {
        controller.selectFrame(frame++ % gpuFrameCount);
        fixture.begin();
        controller.record(fixture.command, resources, settings);
        fixture.finish();
        (void)
            controller.memory(); // pending worker outputs must not be inspected
      };
      const auto until =
          std::chrono::steady_clock::now() + std::chrono::seconds(30);
      while (std::chrono::steady_clock::now() < until) {
        tick(config);
        bool workerEntered;
        {
          std::lock_guard lock(mutex);
          workerEntered = entered;
        }
        if (controller.reflectionSource().image && workerEntered)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      bool paused;
      {
        std::lock_guard lock(mutex);
        paused = entered && !released;
      }
      const auto early = controller.reflectionSource();
      require(
          paused && early.image && !controller.volume() &&
              early.sceneHash == resources.bakeScene()->contentHash &&
              controller.reflectionSceneScale() == .5f,
          "Reflections did not publish independently while diffuse finishing "
          "was paused");
      if (cancel) {
        auto disabled = config;
        disabled.enabled = false;
        tick(disabled);
        require(!controller.reflectionSource().image && !controller.volume() &&
                    early.image->header().parameters[0] == 1,
                "Cancellation invalidated an independently retained reflection "
                "image");
      }
      releaseGate();
      if (!cancel) {
        while (!controller.volume() &&
               std::chrono::steady_clock::now() < until) {
          tick(config);
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(controller.volume() &&
                    controller.reflectionSource().image == early.image,
                "Diffuse activation replaced or reuploaded the independently "
                "published image");
      }
      for (uint32_t slot = 0; slot < gpuFrameCount; ++slot)
        controller.selectFrame(slot);
    }
    {
      RecoveryDirectory directory;
      releaseGate();
      config.diskCache = true;
      bake::ContentHashes original;
      uint32_t coldDenoises = 0;
      for (uint32_t pass = 0; pass < 9; ++pass) {
        if (pass == 1) {
          size_t removed = 0;
          for (const auto &entry : std::filesystem::directory_iterator(directory.path / "reflections" / "shared"))
            if (entry.path().extension() == ".reflection") { std::filesystem::remove(entry.path()); ++removed; }
          require(removed != 0, "Cold run did not persist shared reflection data");
        }
        if (pass == 3) config.reflectionResolution = 16;
        if (pass == 5) config.reflectionProbes = 0;
        if (pass == 6) config.reflectionProbes = 2;
        if (pass == 7) config.reflectionDetailTolerance = .75f;
        bake::Controller controller(fixture.physical, fixture.device, &pipelines, true, fixture.family);
        uint32_t frame = 0;
        std::shared_ptr<bake::Volume> earlyDiffuse, cancelledDiffuse;
        bake::DiffuseContent earlyContent;
        bool cancelledRecovery = false;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (controller.statistics().state != REMIXAPI_BAKE_READY_EXT && std::chrono::steady_clock::now() < until) {
          controller.selectFrame(frame++ % gpuFrameCount);
          fixture.begin(); controller.record(fixture.command, resources, config); fixture.finish();
          if (pass == 1 && controller.volume() && !controller.reflectionSource().image) {
            earlyDiffuse = controller.volume();
            earlyContent = earlyDiffuse->content;
            if (!cancelledRecovery) {
              cancelledDiffuse = earlyDiffuse;
              auto disabled = config; disabled.enabled = false;
              controller.selectFrame(frame++ % gpuFrameCount);
              fixture.begin(); controller.record(fixture.command, resources, disabled); fixture.finish();
              require(!controller.volume() && cancelledDiffuse->data->handle,
                      "Cancelling reflection recovery invalidated retained diffuse assets");
              cancelledRecovery = true;
              earlyDiffuse.reset();
            }
          }
          (void)controller.memory();
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (pass == 1)
          require(cancelledRecovery && earlyDiffuse && controller.volume() == earlyDiffuse &&
                      earlyDiffuse->content == earlyContent &&
                      cancelledDiffuse->data->handle,
                  "Diffuse recovery did not survive cancellation or rebuilt its volume for reflections");
        require(controller.volume() && bool(controller.reflectionSource().image) == bool(config.reflectionProbes) &&
                    controller.statistics().state == REMIXAPI_BAKE_READY_EXT,
                "Cold/recovery/reflection-edit/warm cache sequence failed to finish lighting");
        const auto content = controller.content();
        if (pass == 0) { original = content; coldDenoises = denoiseCalls.load(); }
        else require(content.grid == original.grid && content.surfaces == original.surfaces &&
                         content.staticOcclusion == original.staticOcclusion &&
                         content.styledIndirect == original.styledIndirect && denoiseCalls.load() == coldDenoises,
                     "Reflection-only recovery retraced/denoised or changed cached diffuse lighting");
        for (uint32_t slot = 0; slot < gpuFrameCount; ++slot) controller.selectFrame(slot);
        bake::cache::persistence().waitIdle();
        bake::cache::Device cacheDevice{properties.vendorID, properties.deviceID, properties.driverVersion,
            properties.limits.maxImageDimension2D, properties.limits.maxImageArrayLayers,
            properties.limits.maxStorageBufferRange, {}};
        std::copy_n(properties.pipelineCacheUUID, cacheDevice.uuid.size(), cacheDevice.uuid.begin());
        const auto scene = resources.bakeScene();
        const auto expectedKey = bake::cache::key(scene->contentHash, config, .001f, cacheDevice, {},
            !scene->partition || !scene->partition->closed);
        auto restored = bake::cache::read(directory.path / bake::cache::path(expectedKey).filename(),
            expectedKey, scene, config, cacheDevice);
        const bool completeCache = restored && !restored->missingReflections &&
            bool(restored->captures.size()) == bool(config.reflectionProbes);
        require(completeCache, "Reflection recovery did not persist a strict, complete cache for the next load");
      }
    }
  }
  require(test::validationErrors == 0,
          "Reflection handoff emitted Vulkan validation errors");
} catch (VkResult result) {
  releaseGate();
  std::fprintf(stderr, "Vulkan failure %d\n", int(result));
  return 1;
} catch (const std::exception &error) {
  releaseGate();
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}

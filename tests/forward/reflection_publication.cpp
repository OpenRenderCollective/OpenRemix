#include "forward/bake/controller.h"
#include "forward/probes/automatic_capture.h"
#include "forward/probes/cache_index.h"
#include "forward/bake/cache.h"
#include "forward/bake/transport.h"
#include "forward/bake/budget.h"
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
      try { capture.sealImage(); } catch (const std::logic_error &) { rejected = true; }
      require(rejected, "Missing image was sealed");
      capture.image = std::make_shared<probes::ImageAsset>(fixture.physical, fixture.device, probes::Data{});
      capture.sealImage();
      rejected = false;
      try { capture.sealImage(); } catch (const std::logic_error &) { rejected = true; }
      require(rejected, "Image completion was repeated");
      fixture.begin();
      const auto upload = capture.recordImageUpload(fixture.command);
      require(upload == capture.image && !capture.recordImageUpload(fixture.command),
              "Provider did not upload its image exactly once");
      fixture.finish();
    }
    {
      namespace storage = probes::capture_cache;
      probes::Capture source;
      source.bounds.include({-1,-1,-1}); source.bounds.include({1,1,1});
      source.influence = source.bounds; source.resolution = 8;
      source.radiance.resize(384, {1,2,3,4});
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

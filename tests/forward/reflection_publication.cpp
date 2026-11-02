#include "forward/bake/controller.h"
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

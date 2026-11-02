#include "forward/ray/provider.h"
#include "forward/bake/transport.h"
#include "forward/water/pass.h"
#include "platform/gpu_timers.h"

namespace remix::forward::ray {
  void Provider::initialize(VkPhysicalDevice physical, VkDevice device,
                            const Options &options, bool hardwareRay) {
    reset();
    m_diffuseStatistics = {};
    m_reflectionStatistics = {};
    m_diffuseStatistics.available = hardwareRay && LiveScene::supportsFragmentGI(physical);
    m_reflectionStatistics.available = hardwareRay;
    m_diffuse = options.rtGI && m_diffuseStatistics.available;
    m_water = options.water && options.rtReflections && hardwareRay &&
              water::Pass::supportsRayReflections(physical);
    if ((options.rtReflections && hardwareRay) || m_diffuse) {
      m_scene = std::make_unique<LiveScene>();
      m_scene->initialize(physical, device);
    }
    if (options.rtReflections && hardwareRay) {
      m_reflections = std::make_unique<Reflections>();
      m_reflections->initialize(physical, device, *m_scene);
    }
  }
  void Provider::reset() {
    m_reflections.reset();
    m_scene.reset();
    m_diffuse = m_water = false;
  }
  Provider::FrameInputs Provider::prepare(VkCommandBuffer command, RasterRenderer &resources,
                                          const Options &options, double animationSeconds, uint32_t slot) {
    FrameInputs inputs;
    if (!m_scene) return inputs;
    {
      gpu::GpuScope timing(command, "Forward+ live ray scene");
      m_scene->prepare(command, resources, options, animationSeconds);
    }
    if (m_diffuse) m_diffuseStatistics = m_scene->statistics();
    const auto transport = m_scene->transport();
    if (!transport) return inputs;
    if (m_diffuse)
      inputs.diffuse = transport->prepareFragment(slot, options.rtGISamples, options.rtGIBounces,
                                                  options.rtGIDistance);
    if (m_water)
      inputs.water = transport->prepareFragment(slot, 1, options.rtReflectionBounces,
          options.rtReflectionDistance, bake::Transport::FragmentUse::Reflection);
    return inputs;
  }
}

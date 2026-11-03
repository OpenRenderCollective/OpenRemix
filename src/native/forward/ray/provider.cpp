#include "forward/ray/provider.h"
#include "forward/bake/transport.h"
#include "forward/water/pass.h"
#include "platform/gpu_timers.h"
#include "runtime/pipelines.h"
#include <cstdio>

namespace remix::forward::ray {
  techniques::Capabilities Provider::capabilities(VkPhysicalDevice physical, bool hardwareRay) {
    return {hardwareRay,
            hardwareRay && LiveScene::supportsFragmentGI(physical),
            hardwareRay && water::Pass::supportsRayReflections(physical)};
  }
  void Provider::initialize(VkPhysicalDevice physical, VkDevice device,
                            const techniques::ResourcePlan &plan, techniques::Capabilities capabilities) {
    reset();
    m_physical = physical;
    m_device = device;
    m_diffuseStatistics = {};
    m_reflectionStatistics = {};
    m_cascadeStatistics = {};
    m_diffuseStatistics.available = capabilities.rayQueries && capabilities.fragmentGI;
    m_reflectionStatistics.available = capabilities.rayQueries;
    m_cascadeStatistics.available = capabilities.rayQueries;
    m_diffuse = plan.rayDiffuse();
    m_cascadeDiffuse = plan.cascadeDiffuse();
    m_water = plan.rayWater;
    if (plan.rayScene) {
      m_scene = std::make_unique<LiveScene>();
      m_scene->initialize(physical, device, plan.rayReflections);
    }
    if (plan.rayReflections) {
      m_reflections = std::make_unique<Reflections>();
      m_reflections->initialize(physical, device, *m_scene);
    }
  }
  void Provider::initializeCascades(VkExtent2D extent, VkSampleCountFlagBits samples, const Options &options) {
    if (!m_cascadeDiffuse || m_cascades) return;
    m_cascadeSceneLayout = bake::Transport::createSceneLayout(m_device);
    m_cascades = std::make_unique<cascades::RadianceCascades>();
    m_cascades->initialize(m_physical, m_device, extent, samples, options, m_cascadeSceneLayout);
    m_cascadeStatistics = m_cascades->statistics();
  }
  void Provider::reset() {
    m_cascades.reset();
    m_reflections.reset();
    m_scene.reset();
    if (m_cascadeSceneLayout) pipeline::destroyDescriptorSetLayout(m_device, m_cascadeSceneLayout, nullptr);
    m_cascadeSceneLayout = VK_NULL_HANDLE;
    m_diffuse = m_water = m_cascadeDiffuse = false;
  }
  Provider::FrameInputs Provider::prepare(VkCommandBuffer command, RasterRenderer &resources, const Options &options,
                                          const scene::DistantLight &sun, double animationSeconds, uint32_t slot) {
    FrameInputs inputs;
    if (!m_scene) return inputs;
    {
      gpu::GpuScope timing(command, "Forward+ live ray scene");
      m_scene->prepare(command, resources, options, sun, animationSeconds);
    }
    if (m_diffuse) m_diffuseStatistics = m_scene->statistics();
    const auto transport = m_scene->transport();
    if (m_cascades && !transport) {
      m_cascadeStatistics.active = false;
      const auto &scene = m_scene->statistics();
      std::snprintf(m_cascadeStatistics.status, sizeof(m_cascadeStatistics.status), "%s",
                    scene.status[0] ? scene.status : "Waiting for the live ray scene");
    }
    if (!transport) return inputs;
    if (m_diffuse)
      inputs.diffuse = transport->prepareFragment(slot, options.rtGISamples, options.rtGIBounces,
                                                  options.rtGIDistance);
    if (m_cascades)
      inputs.cascades = transport->prepareFragment(slot, 1, 1, options.cascadeDistance,
                                                   bake::Transport::FragmentUse::Cascades);
    if (m_water) {
      if (inputs.diffuse) {
        const auto query = transport->fragmentQuery(1, options.rtReflectionBounces, options.rtReflectionDistance);
        inputs.reflectionDistance = query.distance;
        inputs.reflectionBounces = query.bounces;
        // Share immutable inputs and the scene set; water supplies its own limits.
        inputs.water = inputs.diffuse;
      } else {
        inputs.water = transport->prepareFragment(slot, 1, options.rtReflectionBounces,
            options.rtReflectionDistance, bake::Transport::FragmentUse::Reflection);
      }
    }
    return inputs;
  }
  void Provider::recordCascades(VkCommandBuffer command, const cascades::RadianceCascades::Inputs &inputs,
                                const Options &options) {
    if (!m_cascades || !inputs.rayScene) return;
    m_cascades->record(command, inputs, options);
    m_cascadeStatistics = m_cascades->statistics();
  }
}

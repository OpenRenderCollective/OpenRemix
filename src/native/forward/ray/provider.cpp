#include "forward/ray/provider.h"
#include "forward/bake/transport.h"
#include "forward/water/pass.h"
#include "platform/gpu_timers.h"
#include <algorithm>

namespace remix::forward::ray {
  techniques::Capabilities Provider::capabilities(VkPhysicalDevice physical, bool hardwareRay) {
    return {hardwareRay,
            hardwareRay && LiveScene::supportsFragmentGI(physical),
            hardwareRay && water::Pass::supportsRayReflections(physical)};
  }
  void Provider::initialize(VkPhysicalDevice physical, VkDevice device,
                            const techniques::ResourcePlan &plan, techniques::Capabilities capabilities) {
    reset();
    m_diffuseStatistics = {};
    m_reflectionStatistics = {};
    m_diffuseStatistics.available = capabilities.rayQueries && capabilities.fragmentGI;
    m_reflectionStatistics.available = capabilities.rayQueries;
    m_diffuse = plan.rayDiffuse();
    m_water = plan.rayWater;
    m_rayShadows = plan.rayShadows;
    if (plan.rayScene) {
      m_scene = std::make_unique<LiveScene>();
      // Reflections and cascades both launch compute ray batches.
      m_scene->initialize(physical, device, plan.rayReflections || plan.cascadeDiffuse());
    }
    if (plan.cascadeDiffuse()) {
      m_cascades = std::make_unique<Cascades>();
      m_cascades->initialize(physical, device, *m_scene);
    }
    if (plan.rayReflections) {
      m_reflections = std::make_unique<Reflections>();
      m_reflections->initialize(physical, device, *m_scene);
    }
  }
  void Provider::reset() {
    m_cascades.reset();
    m_reflections.reset();
    m_scene.reset();
    m_diffuse = m_water = m_rayShadows = false;
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
    // Receivers read the last published build even while the scene is unavailable.
    if (m_cascades) inputs.diffuse = m_cascades->prepareQuery(command);
    const auto transport = m_scene->transport();
    if (!transport) {
      // The shadowed receivers need the ray scene bound: without one, this
      // frame uses the ordinary receivers.
      if (m_rayShadows) inputs.diffuse = VK_NULL_HANDLE;
      return inputs;
    }
    // The cascades leave the diffuse fragment query free: its sample count
    // carries the shadow ray budget, its bounce count (less one) what is traced
    // (ray-local-shadows.glsl), and its range how far a sun ray looks for a
    // caster. Local light rays end at their light. The sun's range is the
    // cascades' own shadow distance: a host can place a backdrop around the
    // world (a 3D skybox at world scale reaches kilometres), and a ray across
    // all of it finds a caster for every point of the map.
    if (m_rayShadows) {
      const uint32_t mode = (options.rtLocalShadows && options.localShadows ? 1u : 0u) |
                            (options.rtSunShadows && options.shadows ? 2u : 0u);
      inputs.shadows =
          transport->prepareFragment(slot, std::clamp(options.rtShadowLights, 1u, 64u), 1 + mode,
                                     std::max(options.shadowDistance, 1.f));
    }
    if (m_diffuse)
      inputs.diffuse = transport->prepareFragment(slot, options.rtGISamples, options.rtGIBounces,
                                                  options.rtGIDistance);
    if (m_water) {
      // Only the fragment ray-query set can be shared with water; the cascades' set holds probes.
      if (m_diffuse && inputs.diffuse) {
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
  void Provider::recordCascades(VkCommandBuffer command, Cascades::Inputs inputs, const FrameData &frame,
                                const Options &options) {
    if (!m_cascades) return;
    // Device radiance batches in one submission share the transport's slot
    // constants, so follow the reflection batches when both are recorded.
    m_cascades->record(command, inputs, frame, options,
                       m_reflections ? options.rtReflectionBounces : options.rtGIBounces);
  }
}

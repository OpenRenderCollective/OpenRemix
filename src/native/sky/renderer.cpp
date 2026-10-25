#include "sky/renderer.h"
#include "sky/bridge.h"
#include "upscale/camera.h"
#include <numos/renderer.h>
#include <numos/settings.h>
#include <numos/shaders.h>
#include <numos/weather.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <string_view>
#include <utility>

namespace remix::sky {
  namespace {
    // The adapters' binding table (scripts/shaders/sky.py), one entry per Stage.
    const numos::PassBindingInfo adapterBindings[] = {
#define NUMOS_PASS_BINDING(slot, set, kind, dim, format, variable)                                            \
  {slot, set, numos::BindingKind::kind, numos::BindingDim::dim, format, variable},
#include "sky/shaders/adapter_passes.inc"
    };
    const numos::PassInfo adapterPasses[] = {
#define NUMOS_PASS(name, file, hash, x, y, z, push, first, count)                                             \
  {#name, file, hash, {x, y, z}, push, adapterBindings + first, count},
#include "sky/shaders/adapter_passes.inc"
    };
    static_assert(std::size(adapterPasses) == size_t(Stage::Count));

    constexpr uint32_t environmentWidth = 512, environmentHeight = 256;
    // A linear view depth at or beyond this is sky.
    constexpr float missDepth = 1e10f;

    struct EnvironmentPush {
      float localToWorld[3][4];
      uint32_t width, height, frameIndex, zUp;
    };
    struct CompositePush {
      uint32_t width, height, frameIndex, zUp;
      float missDepth, scale;
      uint32_t gameSky, reserved;
    };
    // The cloud shadow box around the camera: 4 km across in 8 m texels, and
    // 512 m high in slices 1/3 of that apart (shadows change slowly with
    // height below the clouds).
    constexpr uint32_t cloudShadowSize = 512;
    constexpr float cloudShadowWidthMeters = 4096, cloudShadowHeightMeters = 512;
    struct CloudShadowPush {
      float origin[4], axisU[4], axisV[4], axisSlice[4];
      uint32_t size, slices, zUp, reserved;
    };
    struct RasterDepthPush {
      float inverseZ[4], inverseW[4];
      uint32_t width, height;
      float clearDepth, missDepth;
    };
    struct RasterCompositePush {
      uint32_t width, height, frameIndex, zUp;
      float missDepth, exposure;
      uint32_t gameSky, flags;
    };

    std::vector<Bridge::HostPass> hostPasses() {
      std::vector<Bridge::HostPass> passes;
      for (uint32_t i = 0; i < uint32_t(Stage::Count); ++i)
        passes.push_back({adapterPasses[i], shader(Stage(i))});
      return passes;
    }

    numos::Vector3 vector(const float *v) { return {v[0], v[1], v[2]}; }
  } // namespace

  struct Renderer::State {
    Bridge bridge;
    numos::Renderer numos;
    std::map<std::string, std::string> atmosphere;
    bool configured = false;
    // The game's control (remix_sky_ext.h) and what of it is applied.
    config::SkyControl control;
    bool game = true, timeOverride = false;
    uint64_t timeGeneration = 0, weatherGeneration = 0, lightningGeneration = 0;
    numos::WeatherBlender weather;
    bool weatherActive = false, weatherWanted = false;
    float weatherElapsed = 0, weatherDuration = 0;
    config::SkyState state;
    numos::GpuHandle environment = 0, cloudShadow = 0, missDepth = 0, depthImport = 0, sceneImport = 0;
    // The raster renderer's linear depth and composite images.
    numos::GpuHandle rasterDepth = 0, rasterAddition = 0, rasterTransmittance = 0;
    uint32_t rasterWidth = 0, rasterHeight = 0;
    VkImageView depthView = VK_NULL_HANDLE;
    VkAccelerationStructureKHR scene = VK_NULL_HANDLE;
    uint32_t renderWidth = 0, renderHeight = 0, missWidth = 0, missHeight = 0;
    uint32_t frameIndex = 0, lastCut = 0;
    numos::Camera camera{};
    bool zUp = true, clouds = false, rayQuery = true;
    std::chrono::steady_clock::time_point last{};
    bool begun = false;
    State(VkPhysicalDevice physical, VkDevice device) : bridge(physical, device, hostPasses()) {}

    // Dispatches an adapter, binding NUMOS outputs and the host's resources by
    // the shader variables' names.
    void dispatch(Stage stage, const std::map<std::string_view, numos::Binding> &named, const void *push,
                  uint32_t pushSize, uint32_t groupsX, uint32_t groupsY) {
      const auto &info = adapterPasses[size_t(stage)];
      std::vector<numos::Binding> bindings(info.bindingCount);
      for (uint32_t i = 0; i < info.bindingCount; ++i) {
        if (auto found = named.find(info.bindings[i].shaderVariable); found != named.end())
          bindings[i] = found->second;
        bindings[i].slot = info.bindings[i].slot;
        bindings[i].kind = info.bindings[i].kind;
      }
      bridge.dispatch({.pass = numos::passCount() + uint32_t(stage),
                       .bindings = bindings.data(),
                       .bindingCount = uint32_t(bindings.size()),
                       .pushConstants = push,
                       .pushConstantSize = pushSize,
                       .groupsX = groupsX,
                       .groupsY = groupsY});
    }

    std::map<std::string_view, numos::Binding> outputs() {
      const auto &out = numos.outputs();
      const auto texture = [](numos::GpuHandle handle) { return numos::Binding{.resource = handle}; };
      const auto args = bridge.uploadConstants(&out.args, sizeof(out.args));
      return {{"AtmosphereTransmittanceLut", texture(out.transmittanceLut)},
              {"AtmosphereMultiscatteringLut", texture(out.multiscatteringLut)},
              {"AtmosphereSkyViewLut", texture(out.skyViewLut)},
              {"AtmosphereSkyViewSampler", texture(out.skyViewSampler)},
              {"AtmosphereCloudSkyTransmittanceLut", texture(out.cloudSkyTransmittanceLut)},
              {"AtmosphereCloudShadowMap", texture(out.cloudShadowMap)},
              {"AtmosphereCloudNoise3DSampler", texture(out.cloudNoiseSampler)},
              {"AtmosphereCloudSecondaryLut", texture(out.cloudSecondaryLut)},
              {"NightSkyStarCells", texture(out.nightSkyStarCells)},
              {"NightSkyStars", texture(out.nightSkyStars)},
              {"NightSkyMilkyWay", texture(out.nightSkyMilkyWay)},
              {"NightSkyStarlightLow", texture(out.nightSkyStarlightLow)},
              {"NightSkyMoon", texture(out.nightSkyMoon)},
              {"atmosphereArgs", {.resource = args.buffer, .offset = args.offset, .size = args.size}}};
    }
  };

  Renderer::Renderer(VkPhysicalDevice physical, VkDevice device, bool rayQuery)
      : m(std::make_unique<State>(physical, device)) {
    m->rayQuery = rayQuery;
    // NUMOS copies the night sky and streams it to the GPU over the next frames.
    std::vector<uint8_t> data[4];
    for (size_t i = 0; i < std::size(data); ++i)
      data[i] = nightSkyAsset(i);
    m->numos.setNightSkyAssets({.stars = data[0].data(),
                                .starsSize = data[0].size(),
                                .milkyWay = data[1].data(),
                                .milkyWaySize = data[1].size(),
                                .starlightLow = data[2].data(),
                                .starlightLowSize = data[2].size(),
                                .moon = data[3].data(),
                                .moonSize = data[3].size()});
  }

  Renderer::~Renderer() {
    // The device is idle: NUMOS's resources go through the bridge's deferral
    // and are freed with it.
    m->numos.releaseGpuResources(m->bridge);
  }

  void Renderer::selectFrame(uint32_t slot) { m->bridge.selectFrame(slot); }

  void Renderer::setSettings(const config::SkySettings &settings, const config::SkyControl &control) {
    m->game = !settings.ignoreGameControl;
    m->control = control;
    // The game's time of day replaces the profile's; a new one moves the
    // running clock (timeCycleEnable) to it.
    const bool time = m->game && control.time;
    if (m->configured && settings.atmosphere == m->atmosphere && time == m->timeOverride &&
        (!time || control.timeGeneration == m->timeGeneration))
      return;
    numos::Settings values;
    for (const auto &[key, value] : settings.atmosphere)
      numos::setSetting(values, key, value);
    if (time) {
      values.timeOfDayHours = std::clamp(control.timeOfDayHours, 0.f, 24.f);
      if (control.dayOfYear > 0)
        values.dayOfYear = std::clamp(control.dayOfYear, 1, 365);
      values.timeCycleEnable = control.dayLengthMinutes > 0;
      if (values.timeCycleEnable)
        values.dayLengthMinutes = std::max(control.dayLengthMinutes, .01f);
    }
    m->numos.setSettings(values);
    m->atmosphere = settings.atmosphere;
    m->timeOverride = time;
    m->timeGeneration = control.timeGeneration;
    m->configured = true;
  }

  bool Renderer::record(VkCommandBuffer command, const FrameDescription &frame) {
    m->begun = false;
    if (frame.upAxis > 2 || frame.upAxis == 0) {
      m_status = "The sky needs a Y-up or Z-up world (Lighting: volumetric up axis).";
      return false;
    }
    uint32_t ready = 0, total = 0;
    // Of the screen cloud pass's variants, only the one the settings select.
    const auto &settings = m->numos.settings();
    const auto cloud = settings.cloudProfilingMode == 9           ? numos::PassId::cloud_render_reference
                       : settings.cloudPerformancePreset == 1 ? numos::PassId::cloud_render_fast
                                                              : numos::PassId::cloud_render;
    const auto needed = [&](uint32_t pass) {
      const auto id = numos::PassId(pass);
      if (pass < numos::passCount() && !m->rayQuery) {
        // Without ray queries the scene-traced passes never run (no TLAS).
        const auto &info = numos::passInfo(pass);
        for (uint32_t i = 0; i < info.bindingCount; ++i)
          if (info.bindings[i].kind == numos::BindingKind::AccelerationStructure)
            return false;
      }
      return pass >= numos::passCount() ||
             (id != numos::PassId::cloud_render && id != numos::PassId::cloud_render_fast &&
              id != numos::PassId::cloud_render_reference) ||
             id == cloud;
    };
    if (!m->bridge.preparePipelines(needed, ready, total)) {
      m_status = "Compiling sky shaders (" + std::to_string(ready) + " of " + std::to_string(total) + ")";
      return false;
    }
    m_status.clear();
    const bool zUp = frame.upAxis == 2;
    const auto now = std::chrono::steady_clock::now();
    const float delta = m->frameIndex ? std::chrono::duration<float>(now - m->last).count() : 0.f;
    m->last = now;

    numos::FrameInputs inputs;
    inputs.frameIndex = ++m->frameIndex;
    inputs.deltaSeconds = std::clamp(delta, 0.f, .25f);
    inputs.cameraCut = frame.cameraCut || inputs.frameIndex == 1;
    if (inputs.cameraCut)
      m->lastCut = inputs.frameIndex;
    inputs.lastCameraCutFrame = m->lastCut;
    const auto range = upscale::depthRange(frame.projection);
    const bool rightHanded = frame.projection[11] < 0;
    const float width = float(std::max(frame.renderWidth, 1u)), height = float(std::max(frame.renderHeight, 1u));
    // NUMOS's clip space has +y up the screen (Remix's); the renderer's is Vulkan's, +y down.
    const auto flipY = [](scene::Matrix matrix) {
      for (int column = 0; column < 4; ++column)
        matrix[column * 4 + 1] = -matrix[column * 4 + 1];
      return matrix;
    };
    const auto projection = flipY(frame.projection), previousProjection = flipY(frame.previousProjection);
    // NUMOS's jitter is a clip-space translation after the projection, so a
    // pixel sees the direction projected to its centre minus the jitter. The
    // tracer's pixel samples its centre plus the jitter, y down.
    inputs.camera = numos::makeCamera(frame.view.data(), projection.data(), frame.previousView.data(),
                                      previousProjection.data(), -2 * frame.jitter[0] / width,
                                      2 * frame.jitter[1] / height, frame.renderWidth, frame.renderHeight,
                                      range.nearPlane, rightHanded);
    const auto viewToWorld = scene::inverse(frame.view), projectionToView = scene::inverse(projection);
    std::copy_n(viewToWorld.data(), 16, inputs.viewToWorld);
    std::copy_n(projectionToView.data(), 16, inputs.projectionToView);
    inputs.cameraPosition = inputs.cameraPositionGame = vector(viewToWorld.data() + 12);
    // The view's forward axis in the world: +z for a left-handed view, -z for a right-handed one.
    const float sign = rightHanded ? -1.f : 1.f;
    numos::Vector3 forward{sign * viewToWorld[8], sign * viewToWorld[9], sign * viewToWorld[10]};
    const float length = std::max(std::sqrt(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z), 1e-6f);
    inputs.cameraForward = {forward.x / length, forward.y / length, forward.z / length};
    inputs.zUp = zUp;
    inputs.hostUnitsPerMeter = 1.f / std::max(frame.metersPerUnit, 1e-6f);
    inputs.renderWidth = frame.renderWidth;
    inputs.renderHeight = frame.renderHeight;
    inputs.outputWidth = frame.outputWidth;
    inputs.outputHeight = frame.outputHeight;
    if (frame.scene != m->scene) {
      if (m->sceneImport)
        m->bridge.forget(m->sceneImport);
      m->sceneImport = frame.scene ? m->bridge.importAccelerationStructure(frame.scene) : 0;
      m->scene = frame.scene;
    }
    inputs.opaqueTlas = m->sceneImport;

    // The game's weather eases in from the weather in force; cleared, it
    // eases back to the profile's and then lets go.
    const auto &control = m->control;
    const bool wantWeather = m->game && control.weather;
    if (wantWeather != m->weatherWanted || (wantWeather && control.weatherGeneration != m->weatherGeneration)) {
      const auto &base = m->numos.settings();
      auto target = numos::weatherFromSettings(base);
      numos::WeatherPreset preset;
      if (wantWeather && !control.preset.empty() && numos::weatherPresetFromName(control.preset.c_str(), preset))
        target = numos::weatherPreset(preset, base);
      size_t count = 0;
      const auto *fields = numos::weatherFields(count);
      for (size_t i = 0; wantWeather && i < count; ++i)
        if (const auto found = control.fields.find(fields[i].name); found != control.fields.end())
          target.*fields[i].member = found->second;
      if (!m->weatherActive)
        m->weather = numos::WeatherBlender(numos::weatherFromSettings(base));
      m->weather.setTarget(target, control.transitionSeconds);
      m->weatherActive = true;
      m->weatherWanted = wantWeather;
      m->weatherGeneration = control.weatherGeneration;
      m->weatherElapsed = 0;
      m->weatherDuration = std::max(control.transitionSeconds, 0.f);
    }
    if (m->weatherActive) {
      m->weather.update(inputs.deltaSeconds);
      m->weatherElapsed += inputs.deltaSeconds;
      inputs.weather = &m->weather.current();
      if (!m->weatherWanted && !m->weather.transitioning())
        m->weatherActive = false;
    }
    if (control.lightningGeneration != m->lightningGeneration) {
      m->lightningGeneration = control.lightningGeneration;
      inputs.lightningStrike = m->game;
      inputs.lightningStrikeAtPosition = control.lightningAtPosition;
      inputs.lightningStrikePosition = {control.lightningPosition[0], control.lightningPosition[1],
                                        control.lightningPosition[2]};
    }

    m->bridge.begin(command);
    m->numos.beginFrame(m->bridge, inputs);
    m->begun = true;
    m->clouds = false;
    m->camera = inputs.camera;
    m->zUp = zUp;
    m->renderWidth = frame.renderWidth;
    m->renderHeight = frame.renderHeight;
    if (!m->environment)
      m->environment = m->bridge.createTexture({.type = numos::TextureType::Tex2D,
                                                .format = numos::Format::RGBA16Float,
                                                .width = environmentWidth,
                                                .height = environmentHeight,
                                                .debugName = "NUMOS environment"});
    // The map's frame is Y up; a Z-up world maps (x, y, z) to (x, z, -y).
    const float zUpRows[3][3] = {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}};
    const float yUpRows[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const auto &rows = zUp ? zUpRows : yUpRows;
    EnvironmentPush push{.width = environmentWidth, .height = environmentHeight, .frameIndex = m->frameIndex,
                         .zUp = zUp};
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) {
        m_domeTransform[r][c] = rows[r][c];
        push.localToWorld[r][c] = rows[c][r];
      }
    m->bridge.barrier();
    auto named = m->outputs();
    named["environment"] = {.resource = m->environment};
    m->dispatch(Stage::environment, named, &push, sizeof(push), (environmentWidth + 7) / 8,
                (environmentHeight + 7) / 8);

    if (!m->cloudShadow)
      m->cloudShadow = m->bridge.createTexture({.type = numos::TextureType::Tex2D,
                                                .format = numos::Format::R16Float,
                                                .width = cloudShadowSize * cloudShadowSlices,
                                                .height = cloudShadowSize,
                                                .debugName = "NUMOS cloud shadow"});
    // The box's horizontal axes and up axis in the world, its corner snapped
    // to whole texels so the shadows hold still as the camera moves.
    const float unitsPerMeter = inputs.hostUnitsPerMeter;
    const float boxWidth = cloudShadowWidthMeters * unitsPerMeter, boxHeight = cloudShadowHeightMeters * unitsPerMeter;
    const float texel = boxWidth / cloudShadowSize, step = boxHeight / (cloudShadowSlices - 1);
    const float axes[3][3] = {{1, 0, 0}, {0, float(zUp), float(!zUp)}, {0, float(!zUp), float(zUp)}};
    const float eye[3] = {inputs.cameraPosition.x, inputs.cameraPosition.y, inputs.cameraPosition.z};
    const auto along = [&](int axis) { return axes[axis][0] * eye[0] + axes[axis][1] * eye[1] + axes[axis][2] * eye[2]; };
    const float corner[3] = {std::floor((along(0) - boxWidth / 2) / texel) * texel,
                             std::floor((along(1) - boxWidth / 2) / texel) * texel,
                             std::floor((along(2) - boxHeight / 2) / step) * step};
    const float extent[3] = {boxWidth, boxWidth, boxHeight};
    CloudShadowPush shadowPush{.size = cloudShadowSize, .slices = cloudShadowSlices, .zUp = zUp};
    float *pushAxes[3] = {shadowPush.axisU, shadowPush.axisV, shadowPush.axisSlice};
    for (int a = 0; a < 3; ++a) {
      for (int c = 0; c < 3; ++c) {
        shadowPush.origin[c] += axes[a][c] * corner[a];
        pushAxes[a][c] = axes[a][c] * extent[a];
        m_cloudShadowTransform[a][c] = axes[a][c] / extent[a];
      }
      m_cloudShadowTransform[a][3] = -corner[a] / extent[a];
    }
    named.erase("environment");
    named["cloudShadow"] = {.resource = m->cloudShadow};
    m->dispatch(Stage::cloud_shadow, named, &shadowPush, sizeof(shadowPush),
                (cloudShadowSize * cloudShadowSlices + 7) / 8, (cloudShadowSize + 7) / 8);
    m->bridge.end();

    const auto &out = m->numos.outputs();
    const float scale = 1.f / std::max(out.radianceScale, 1e-30f);
    m_environmentScale = scale;
    // The scene has one distant light: the brightest of NUMOS's sun, moons and
    // lightning, by illuminance (radiance over the disc's solid angle). A
    // strike kilometres away lights the scene as a disc of its sphere's
    // radiance and angular size would.
    m_sun = {};
    float brightest = 0;
    uint32_t kind = 0;
    for (uint32_t index = 0; index < std::size(out.lights); ++index) {
      const auto &light = out.lights[index];
      if (!light.active)
        continue;
      Sun candidate{.active = true, .cloudShadowed = light.cloudShadowed};
      float halfAngle = light.halfAngle;
      numos::Vector3 direction = light.direction;
      if (light.type == numos::EmittedLight::Type::Sphere) {
        const numos::Vector3 offset{inputs.cameraPosition.x - light.position.x,
                                    inputs.cameraPosition.y - light.position.y,
                                    inputs.cameraPosition.z - light.position.z};
        const float distance = std::sqrt(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
        if (distance <= light.radius)
          continue;
        direction = {offset.x / distance, offset.y / distance, offset.z / distance};
        halfAngle = std::asin(light.radius / distance);
      }
      candidate.direction[0] = direction.x;
      candidate.direction[1] = direction.y;
      candidate.direction[2] = direction.z;
      candidate.radiance[0] = light.radiance.x * scale;
      candidate.radiance[1] = light.radiance.y * scale;
      candidate.radiance[2] = light.radiance.z * scale;
      candidate.angularDiameterDegrees = halfAngle * 2 * 57.2957795f;
      const float sine = std::sin(halfAngle);
      const float illuminance = (.2126f * candidate.radiance[0] + .7152f * candidate.radiance[1] +
                                 .0722f * candidate.radiance[2]) * sine * sine;
      if (illuminance > brightest) {
        brightest = illuminance;
        m_sun = candidate;
        kind = index == numos::FrameOutputs::kSunLight         ? 1
               : index == numos::FrameOutputs::kLightningLight ? 3
                                                               : 2;
      }
    }

    auto &state = m->state;
    state.active = true;
    state.light = kind;
    std::copy_n(m_sun.direction, 3, state.lightDirection);
    state.timeOfDayHours = m->numos.simulationState().timeOfDayHours;
    state.weatherTransition =
        m->weatherActive && m->weatherDuration > 0 ? std::min(m->weatherElapsed / m->weatherDuration, 1.f) : 1.f;
    state.cloudCoverage = inputs.weather ? inputs.weather->cloudCoverageMean : m->numos.settings().cloudCoverageMean;
    state.preset = m->weatherWanted ? control.preset : std::string();
    // Rain falls from the stormy presets unless the game says otherwise.
    float precipitation = 0;
    if (m->weatherWanted && control.precipitation >= 0)
      precipitation = control.precipitation;
    else if (numos::WeatherPreset preset;
             m->weatherWanted && numos::weatherPresetFromName(control.preset.c_str(), preset))
      precipitation = preset == numos::WeatherPreset::Storm ? 1.f : preset == numos::WeatherPreset::Drizzle ? .35f : 0.f;
    state.precipitation = precipitation;
    return true;
  }

  void Renderer::recordPostDepth(VkCommandBuffer command, VkImage depthImage, VkImageView depthView) {
    if (!m->begun)
      return;
    m->begun = false;
    m->bridge.begin(command);
    if (depthView != m->depthView) {
      if (m->depthImport)
        m->bridge.forget(m->depthImport);
      m->depthImport = depthView ? m->bridge.importTexture(depthImage, depthView,
                                                           {.format = numos::Format::R32Float,
                                                            .width = m->renderWidth,
                                                            .height = m->renderHeight})
                                 : 0;
      m->depthView = depthView;
    }
    auto depth = m->depthImport;
    if (!depth) {
      // Without the tracer's depth every pixel is sky.
      if (m->missDepth && (m->missWidth != m->renderWidth || m->missHeight != m->renderHeight)) {
        m->bridge.destroy(m->missDepth);
        m->missDepth = 0;
      }
      if (!m->missDepth) {
        m->missDepth = m->bridge.createTexture(
            {.format = numos::Format::R32Float, .width = m->renderWidth, .height = m->renderHeight});
        const float sky[4] = {missDepth, missDepth, missDepth, missDepth};
        m->bridge.clearTexture(m->missDepth, sky);
        m->missWidth = m->renderWidth;
        m->missHeight = m->renderHeight;
      }
      depth = m->missDepth;
    }
    m->numos.dispatchPostDepth(m->bridge, depth, missDepth);
    m->bridge.end();
    m->clouds = true;
  }

  void Renderer::recordComposite(VkCommandBuffer command, VkImage colorImage, VkImageView colorView,
                                 VkImage depthImage, VkImageView depthView, float scale,
                                 config::SkySettings::GameSky gameSky) {
    if (!m->clouds || !colorView || !depthView)
      return;
    m->clouds = false;
    m->bridge.begin(command);
    const numos::TextureDesc colorDesc{.format = numos::Format::RGBA16Float,
                                       .width = m->renderWidth,
                                       .height = m->renderHeight};
    const auto color = m->bridge.importTexture(colorImage, colorView, colorDesc);
    const auto depth = m->depthView == depthView
                           ? m->depthImport
                           : m->bridge.importTexture(depthImage, depthView,
                                                     {.format = numos::Format::R32Float,
                                                      .width = m->renderWidth,
                                                      .height = m->renderHeight});
    const auto &out = m->numos.outputs();
    auto named = m->outputs();
    const auto camera = m->bridge.uploadConstants(&m->camera, sizeof(m->camera));
    const auto texture = [](numos::GpuHandle handle) { return numos::Binding{.resource = handle}; };
    named["camera"] = {.resource = camera.buffer, .offset = camera.offset, .size = camera.size};
    named["cloudRender"] = texture(out.cloudRender);
    named["cloudDepth"] = texture(out.cloudDepth);
    named["apNearScatter"] = texture(out.apNearScatter);
    named["apFarScatter"] = texture(out.apFarScatter);
    named["apFarTransmittance"] = texture(out.apFarTransmittance);
    named["apFroxelAux"] = texture(out.apFroxelAux);
    named["apLights"] = texture(out.apLights);
    named["apLightClusters"] = texture(out.apLightClusters);
    named["apSampler"] = texture(out.linearClampSampler);
    named["linearViewZ"] = texture(depth);
    named["color"] = texture(color);
    const CompositePush push{.width = m->renderWidth,
                             .height = m->renderHeight,
                             .frameIndex = m->frameIndex,
                             .zUp = m->zUp,
                             .missDepth = missDepth,
                             .scale = scale,
                             .gameSky = uint32_t(gameSky)};
    m->dispatch(Stage::composite, named, &push, sizeof(push), (m->renderWidth + 7) / 8, (m->renderHeight + 7) / 8);
    m->bridge.end();
    m->bridge.forget(color);
    if (depth != m->depthImport)
      m->bridge.forget(depth);
  }

  const config::SkyState &Renderer::state() const { return m->state; }

  void Renderer::recordRasterPostDepth(VkCommandBuffer command, VkImage depthImage, VkImageView depthView,
                                       const scene::Matrix &projection, float clearDepth) {
    if (!m->begun)
      return;
    m->begun = false;
    m->bridge.begin(command);
    if (m->rasterWidth != m->renderWidth || m->rasterHeight != m->renderHeight) {
      for (auto *texture : {&m->rasterDepth, &m->rasterAddition, &m->rasterTransmittance})
        if (*texture)
          m->bridge.destroy(std::exchange(*texture, 0));
      m->rasterWidth = m->renderWidth;
      m->rasterHeight = m->renderHeight;
    }
    const auto create = [&](numos::Format format, const char *name) {
      return m->bridge.createTexture(
          {.format = format, .width = m->renderWidth, .height = m->renderHeight, .debugName = name});
    };
    if (!m->rasterDepth) {
      m->rasterDepth = create(numos::Format::R32Float, "NUMOS raster depth");
      m->rasterAddition = create(numos::Format::RGBA16Float, "NUMOS raster addition");
      m->rasterTransmittance = create(numos::Format::RGBA16Float, "NUMOS raster transmittance");
    }
    const auto depth = m->bridge.importTexture(
        depthImage, depthView, {.format = numos::Format::R32Float, .width = m->renderWidth, .height = m->renderHeight});
    // The inverse projection's z and w rows (column-major).
    const auto inverse = scene::inverse(projection);
    RasterDepthPush push{.width = m->renderWidth, .height = m->renderHeight, .clearDepth = clearDepth,
                         .missDepth = missDepth};
    for (int c = 0; c < 4; ++c) {
      push.inverseZ[c] = inverse[c * 4 + 2];
      push.inverseW[c] = inverse[c * 4 + 3];
    }
    m->dispatch(Stage::raster_depth, {{"depth", {.resource = depth}}, {"linearViewZ", {.resource = m->rasterDepth}}},
                &push, sizeof(push), (m->renderWidth + 7) / 8, (m->renderHeight + 7) / 8);
    m->bridge.barrier();
    m->numos.dispatchPostDepth(m->bridge, m->rasterDepth, missDepth);
    m->bridge.end();
    m->bridge.forget(depth);
    m->clouds = true;
  }

  bool Renderer::recordRasterComposite(VkCommandBuffer command, float exposure,
                                       config::SkySettings::GameSky gameSky, bool linearTarget) {
    if (!m->clouds || !m->rasterDepth)
      return false;
    m->clouds = false;
    m->bridge.begin(command);
    const auto &out = m->numos.outputs();
    auto named = m->outputs();
    const auto camera = m->bridge.uploadConstants(&m->camera, sizeof(m->camera));
    const auto texture = [](numos::GpuHandle handle) { return numos::Binding{.resource = handle}; };
    named["camera"] = {.resource = camera.buffer, .offset = camera.offset, .size = camera.size};
    named["cloudRender"] = texture(out.cloudRender);
    named["cloudDepth"] = texture(out.cloudDepth);
    named["apNearScatter"] = texture(out.apNearScatter);
    named["apFarScatter"] = texture(out.apFarScatter);
    named["apFarTransmittance"] = texture(out.apFarTransmittance);
    named["apFroxelAux"] = texture(out.apFroxelAux);
    named["apLights"] = texture(out.apLights);
    named["apLightClusters"] = texture(out.apLightClusters);
    named["apSampler"] = texture(out.linearClampSampler);
    named["linearViewZ"] = texture(m->rasterDepth);
    named["addition"] = texture(m->rasterAddition);
    named["transmittance"] = texture(m->rasterTransmittance);
    const RasterCompositePush push{.width = m->renderWidth,
                                   .height = m->renderHeight,
                                   .frameIndex = m->frameIndex,
                                   .zUp = m->zUp,
                                   .missDepth = missDepth,
                                   .exposure = exposure / std::max(out.radianceScale, 1e-30f),
                                   .gameSky = uint32_t(gameSky),
                                   .flags = linearTarget ? 1u : 0u};
    m->bridge.barrier();
    m->dispatch(Stage::raster_composite, named, &push, sizeof(push), (m->renderWidth + 7) / 8,
                (m->renderHeight + 7) / 8);
    m->bridge.end();
    return true;
  }

  VkDescriptorImageInfo Renderer::rasterAddition() const {
    if (!m->rasterAddition)
      return {};
    return {m->bridge.sampler(m->numos.outputs().linearClampSampler), m->bridge.sampledView(m->rasterAddition),
            VK_IMAGE_LAYOUT_GENERAL};
  }

  VkDescriptorImageInfo Renderer::rasterTransmittance() const {
    if (!m->rasterTransmittance)
      return {};
    return {m->bridge.sampler(m->numos.outputs().linearClampSampler), m->bridge.sampledView(m->rasterTransmittance),
            VK_IMAGE_LAYOUT_GENERAL};
  }

  VkDescriptorImageInfo Renderer::cloudShadow() const {
    if (!m->cloudShadow)
      return {};
    return {m->bridge.sampler(m->numos.outputs().linearClampSampler), m->bridge.sampledView(m->cloudShadow),
            VK_IMAGE_LAYOUT_GENERAL};
  }

  VkDescriptorImageInfo Renderer::environment() const {
    if (!m->environment)
      return {};
    return {m->bridge.sampler(m->numos.outputs().linearClampSampler), m->bridge.sampledView(m->environment),
            VK_IMAGE_LAYOUT_GENERAL};
  }
} // namespace remix::sky

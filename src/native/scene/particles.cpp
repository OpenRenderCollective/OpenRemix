#include "scene/particles.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace remix::scene {
  namespace {
    constexpr float pi = 3.14159265358979f;
    bool finite(float value) { return std::isfinite(value); }
    bool finite(const remixapi_Float3D &v) { return finite(v.x) && finite(v.y) && finite(v.z); }
    bool finite(const remixapi_Float4D &v) { return finite(v.x) && finite(v.y) && finite(v.z) && finite(v.w); }
    bool nonNegative(const remixapi_Float3D &v) { return v.x >= 0 && v.y >= 0 && v.z >= 0; }
    bool nonNegative(const remixapi_Float4D &v) { return v.x >= 0 && v.y >= 0 && v.z >= 0 && v.w >= 0; }
    constexpr uint32_t knownFlags = REMIXAPI_PARTICLE_FOLLOW_WEATHER_EXT | REMIXAPI_PARTICLE_SHELTER_EXT |
                                    REMIXAPI_PARTICLE_CAMERA_SPACE_EXT | REMIXAPI_PARTICLE_UNLIT_EXT;
    constexpr uint32_t presetParticles = 32768;

    float presetRate(uint32_t preset) {
      return preset == REMIXAPI_PARTICLE_PRESET_RAIN_EXT ? 30000.f
             : preset == REMIXAPI_PARTICLE_PRESET_SNOW_EXT ? 4000.f
                                                            : 0.f;
    }
    uint32_t wantedCapacity(const remixapi_ParticleEmitterInfoEXT &info) {
      if (info.maxParticles)
        return info.maxParticles;
      return info.preset != REMIXAPI_PARTICLE_PRESET_NONE_EXT ? presetParticles : defaultParticlesPerEmitter;
    }
    void normalize(float (&v)[3]) {
      const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      if (length > 1e-20f)
        for (float &c : v)
          c /= length;
      else
        v[0] = v[1] = 0, v[2] = 1;
    }
  } // namespace

  const char *validateParticleEmitter(const remixapi_ParticleEmitterInfoEXT &info) {
    if (info.sType != REMIXAPI_STRUCT_TYPE_PARTICLE_EMITTER_INFO_EXT)
      return "wrong sType";
    if (info.shape > REMIXAPI_PARTICLE_SHAPE_CAMERA_VOLUME_EXT)
      return "unknown shape";
    if (info.preset > REMIXAPI_PARTICLE_PRESET_SNOW_EXT)
      return "unknown preset";
    if (info.flags & ~knownFlags)
      return "unknown flag";
    if (info.blend > REMIXAPI_PARTICLE_BLEND_PREMULTIPLIED_EXT)
      return "unknown blend";
    if (info.facing > REMIXAPI_PARTICLE_FACING_HORIZONTAL_EXT)
      return "unknown facing";
    if (info.maxParticles > maxParticlesPerEmitter)
      return "maxParticles above 65536";
    for (const auto &row : info.transform.matrix)
      for (float value : row)
        if (!finite(value))
          return "transform not finite";
    const float scalars[] = {info.spawnRate, info.lifetimeMin,   info.lifetimeMax, info.speed,
                             info.coneAngleDegrees, info.drag, info.turbulence,   info.sizeStart,
                             info.sizeEnd,   info.sizeJitter,  info.stretch,      info.softDistance};
    for (float value : scalars)
      if (!finite(value))
        return "value not finite";
    if (!finite(info.extent) || !finite(info.velocity) || !finite(info.acceleration) || !finite(info.emission) ||
        !finite(info.colorStart) || !finite(info.colorEnd))
      return "value not finite";
    if (!nonNegative(info.extent) || info.spawnRate < 0)
      return "negative extent or spawnRate";
    if (info.preset != REMIXAPI_PARTICLE_PRESET_NONE_EXT)
      return nullptr;
    if (!(info.lifetimeMin > 0) || info.lifetimeMax < info.lifetimeMin)
      return "lifetime outside 0 < min <= max";
    if (info.coneAngleDegrees < 0 || info.coneAngleDegrees > 180)
      return "coneAngleDegrees outside 0..180";
    if (info.speed < 0 || info.drag < 0 || info.turbulence < 0 || info.sizeStart < 0 || info.sizeEnd < 0 ||
        info.stretch < 0 || info.softDistance < 0)
      return "negative value";
    if (info.sizeJitter < 0 || info.sizeJitter > 1)
      return "sizeJitter outside 0..1";
    if (!nonNegative(info.colorStart) || !nonNegative(info.colorEnd) || !nonNegative(info.emission))
      return "negative colour";
    return nullptr;
  }

  ResolvedEmitter resolveParticleEmitter(const remixapi_ParticleEmitterInfoEXT &source, float metersPerUnit,
                                         uint32_t upAxis) {
    auto info = source;
    ResolvedEmitter out;
    const float unit = 1 / std::max(metersPerUnit, 1e-6f); // units per metre
    upAxis = std::min(upAxis, 2u);
    // World axes with +Z along the up axis: a cyclic permutation, so right-handed.
    float upBasis[3][3] = {};
    for (uint32_t k = 0; k < 3; ++k)
      upBasis[(upAxis + 1 + k) % 3][k] = 1;
    float worldAcceleration[3] = {info.acceleration.x, info.acceleration.y, info.acceleration.z};
    if (info.preset != REMIXAPI_PARTICLE_PRESET_NONE_EXT) {
      const bool rain = info.preset == REMIXAPI_PARTICLE_PRESET_RAIN_EXT;
      const bool defaultExtent = info.extent.x == 0 && info.extent.y == 0 && info.extent.z == 0;
      info.shape = REMIXAPI_PARTICLE_SHAPE_CAMERA_VOLUME_EXT;
      if (defaultExtent)
        info.extent = rain ? remixapi_Float3D{14 * unit, 14 * unit, 8 * unit}
                           : remixapi_Float3D{12 * unit, 12 * unit, 6 * unit};
      if (info.spawnRate == 0)
        info.spawnRate = presetRate(info.preset);
      info.lifetimeMin = info.lifetimeMax = rain ? 3.f : 14.f;
      info.velocity = {0, 0, (rain ? -9.f : -1.f) * unit};
      info.speed = (rain ? .6f : .2f) * unit;
      info.coneAngleDegrees = 180;
      const float fall = rain ? 0.f : -.6f * unit;
      for (int i = 0; i < 3; ++i)
        worldAcceleration[i] = upBasis[i][2] * fall;
      info.drag = rain ? 0.f : .6f;
      info.turbulence = rain ? 0.f : .8f * unit;
      info.facing = rain ? REMIXAPI_PARTICLE_FACING_VELOCITY_EXT : REMIXAPI_PARTICLE_FACING_CAMERA_EXT;
      info.blend = REMIXAPI_PARTICLE_BLEND_PREMULTIPLIED_EXT;
      info.sizeStart = info.sizeEnd = (rain ? .012f : .03f) * unit;
      info.sizeJitter = rain ? .3f : .5f;
      info.stretch = rain ? .025f : 0.f;
      info.colorStart = info.colorEnd = rain ? remixapi_Float4D{.75f, .78f, .82f, .3f}
                                             : remixapi_Float4D{.92f, .93f, .96f, .9f};
      info.emission = {};
      info.texture = nullptr;
      info.softDistance = .2f * unit;
    }
    out.shape = info.shape;
    out.facing = info.facing;
    out.blend = info.blend;
    out.flags = info.flags;
    // Emitter space to world: a camera volume's is the up basis (its centre
    // follows the camera on the GPU).
    float rotation[3][3];
    if (info.shape == REMIXAPI_PARTICLE_SHAPE_CAMERA_VOLUME_EXT) {
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
          out.transform[r][c] = rotation[r][c] = upBasis[r][c];
        out.transform[r][3] = 0;
      }
    } else {
      std::memcpy(out.transform, info.transform.matrix, sizeof(out.transform));
      // The rotation alone, for directions.
      for (int c = 0; c < 3; ++c) {
        float column[3] = {info.transform.matrix[0][c], info.transform.matrix[1][c], info.transform.matrix[2][c]};
        normalize(column);
        for (int r = 0; r < 3; ++r)
          rotation[r][c] = column[r];
      }
    }
    out.extent[0] = info.extent.x;
    out.extent[1] = info.extent.y;
    out.extent[2] = info.extent.z;
    out.lifetime[0] = info.lifetimeMin;
    out.lifetime[1] = info.lifetimeMax;
    const float local[3] = {info.velocity.x, info.velocity.y, info.velocity.z};
    for (int r = 0; r < 3; ++r) {
      out.velocity[r] = rotation[r][0] * local[0] + rotation[r][1] * local[1] + rotation[r][2] * local[2];
      out.axis[r] = rotation[r][2];
      out.acceleration[r] = worldAcceleration[r];
    }
    normalize(out.axis);
    out.speed = info.speed;
    out.cosCone = std::cos(std::clamp(info.coneAngleDegrees, 0.f, 180.f) * (pi / 180));
    out.drag = info.drag;
    out.turbulence = info.turbulence;
    out.size[0] = info.sizeStart;
    out.size[1] = info.sizeEnd;
    out.size[2] = std::clamp(info.sizeJitter, 0.f, 1.f);
    out.stretch = info.stretch;
    const remixapi_Float4D *colors[] = {&info.colorStart, &info.colorEnd};
    float *outColors[] = {out.colorStart, out.colorEnd};
    for (int i = 0; i < 2; ++i) {
      outColors[i][0] = colors[i]->x;
      outColors[i][1] = colors[i]->y;
      outColors[i][2] = colors[i]->z;
      outColors[i][3] = colors[i]->w;
    }
    out.emission[0] = info.emission.x;
    out.emission[1] = info.emission.y;
    out.emission[2] = info.emission.z;
    out.softDistance = info.softDistance;
    out.seed = info.seed;
    out.texture = uint64_t(uintptr_t(info.texture));
    return out;
  }

  void RangeAllocator::reset(uint32_t size) {
    m_size = size;
    m_used = 0;
    m_free.clear();
    if (size)
      m_free.push_back({0, size});
  }

  uint32_t RangeAllocator::allocate(uint32_t count) {
    if (!count)
      return UINT32_MAX;
    for (auto it = m_free.begin(); it != m_free.end(); ++it)
      if (it->count >= count) {
        const uint32_t offset = it->offset;
        it->offset += count;
        it->count -= count;
        if (!it->count)
          m_free.erase(it);
        m_used += count;
        return offset;
      }
    return UINT32_MAX;
  }

  void RangeAllocator::release(uint32_t offset, uint32_t count) {
    if (!count)
      return;
    auto it = std::lower_bound(m_free.begin(), m_free.end(), offset,
                               [](const Range &range, uint32_t value) { return range.offset < value; });
    it = m_free.insert(it, {offset, count});
    m_used -= count;
    // Coalesce with the next range, then with the previous one.
    if (auto next = it + 1; next != m_free.end() && it->offset + it->count == next->offset) {
      it->count += next->count;
      m_free.erase(next);
    }
    if (it != m_free.begin()) {
      auto previous = it - 1;
      if (previous->offset + previous->count == it->offset) {
        previous->count += it->count;
        m_free.erase(it);
      }
    }
  }

  ParticleRegistry::Emitter *ParticleRegistry::find(remixapi_ParticleEmitterHandleEXT handle) {
    for (auto &emitter : m_emitters)
      if (emitter.handle == handle)
        return &emitter;
    return nullptr;
  }

  void ParticleRegistry::releaseSlice(Emitter &emitter) {
    if (emitter.capacity)
      m_pool.release(emitter.offset, emitter.capacity);
    emitter.offset = emitter.capacity = 0;
    emitter.pendingReset = true;
  }

  uint32_t ParticleRegistry::freeSlot() const {
    for (uint32_t slot = 0;; ++slot)
      if (std::none_of(m_emitters.begin(), m_emitters.end(),
                       [&](const Emitter &emitter) { return emitter.slot == slot; }))
        return slot;
  }

  remixapi_ErrorCode ParticleRegistry::create(const remixapi_ParticleEmitterInfoEXT &info,
                                              remixapi_ParticleEmitterHandleEXT &out, bool internal) {
    if (validateParticleEmitter(info))
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    std::lock_guard lock(m_mutex);
    if (m_emitters.size() >= maxParticleEmitters)
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    Emitter emitter;
    emitter.handle = m_next++;
    emitter.slot = freeSlot();
    emitter.info = info;
    emitter.info.pNext = nullptr;
    emitter.internal = internal;
    m_emitters.push_back(emitter);
    out = emitter.handle;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode ParticleRegistry::update(remixapi_ParticleEmitterHandleEXT handle,
                                              const remixapi_ParticleEmitterInfoEXT &info) {
    if (validateParticleEmitter(info))
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    std::lock_guard lock(m_mutex);
    auto *emitter = find(handle);
    if (!emitter)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    // A new size takes a new slice (publish).
    if (wantedCapacity(info) != wantedCapacity(emitter->info))
      releaseSlice(*emitter);
    emitter->info = info;
    emitter->info.pNext = nullptr;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode ParticleRegistry::setTransform(remixapi_ParticleEmitterHandleEXT handle,
                                                    const remixapi_Transform &transform) {
    for (const auto &row : transform.matrix)
      for (float value : row)
        if (!finite(value))
          return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    std::lock_guard lock(m_mutex);
    auto *emitter = find(handle);
    if (!emitter)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    emitter->info.transform = transform;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode ParticleRegistry::setEnabled(remixapi_ParticleEmitterHandleEXT handle, bool enabled) {
    std::lock_guard lock(m_mutex);
    auto *emitter = find(handle);
    if (!emitter)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    emitter->enabled = enabled;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode ParticleRegistry::burst(remixapi_ParticleEmitterHandleEXT handle, uint32_t count) {
    std::lock_guard lock(m_mutex);
    auto *emitter = find(handle);
    if (!emitter)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    emitter->burst = uint32_t(std::min<uint64_t>(uint64_t(emitter->burst) + count, maxParticlesPerEmitter));
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode ParticleRegistry::clear(remixapi_ParticleEmitterHandleEXT handle) {
    std::lock_guard lock(m_mutex);
    auto *emitter = find(handle);
    if (!emitter)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    emitter->pendingReset = true;
    emitter->burst = 0;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode ParticleRegistry::destroy(remixapi_ParticleEmitterHandleEXT handle) {
    std::lock_guard lock(m_mutex);
    auto it = std::find_if(m_emitters.begin(), m_emitters.end(),
                           [&](const Emitter &emitter) { return emitter.handle == handle; });
    if (it == m_emitters.end() || it->internal)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    releaseSlice(*it);
    m_emitters.erase(it);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  void ParticleRegistry::reset() {
    std::lock_guard lock(m_mutex);
    m_emitters.clear();
    m_rainTest = m_snowTest = 0;
    m_pool.reset(0);
    m_frame = 0;
    m_wasEnabled = false;
    m_statistics = {};
  }

  void ParticleRegistry::syncTestEmitter(remixapi_ParticleEmitterHandleEXT &handle, uint32_t preset, bool wanted,
                                         const PublishSettings &settings) {
    auto it = std::find_if(m_emitters.begin(), m_emitters.end(),
                           [&](const Emitter &emitter) { return handle && emitter.handle == handle; });
    if (!wanted) {
      if (it != m_emitters.end()) {
        releaseSlice(*it);
        m_emitters.erase(it);
      }
      handle = 0;
      return;
    }
    remixapi_ParticleEmitterInfoEXT info{};
    info.sType = REMIXAPI_STRUCT_TYPE_PARTICLE_EMITTER_INFO_EXT;
    info.preset = preset;
    info.flags = (settings.shelter ? REMIXAPI_PARTICLE_SHELTER_EXT : 0u) |
                 (settings.followWeather ? REMIXAPI_PARTICLE_FOLLOW_WEATHER_EXT : 0u);
    const float intensity = std::clamp(settings.intensity, 0.f, 1.f);
    info.spawnRate = presetRate(preset) * std::max(intensity, 1e-3f);
    info.seed = preset * 7919u;
    info.transform = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    if (it == m_emitters.end()) {
      if (m_emitters.size() >= maxParticleEmitters)
        return;
      Emitter emitter;
      emitter.handle = handle = m_next++;
      emitter.slot = freeSlot();
      emitter.internal = true;
      m_emitters.push_back(emitter);
      it = m_emitters.end() - 1;
    }
    it->info = info;
    it->enabled = intensity > 0;
  }

  ParticleFrame ParticleRegistry::publish(const PublishSettings &settings) {
    std::lock_guard lock(m_mutex);
    ParticleFrame frame;
    frame.frame = ++m_frame;
    frame.enabled = settings.enabled;
    frame.seconds = float(std::clamp(settings.seconds, 0.0, .25));
    frame.budget = settings.budget;
    syncTestEmitter(m_rainTest, REMIXAPI_PARTICLE_PRESET_RAIN_EXT, settings.rain, settings);
    syncTestEmitter(m_snowTest, REMIXAPI_PARTICLE_PRESET_SNOW_EXT, settings.snow, settings);
    // A disabled system keeps nothing: it starts empty when enabled again.
    const bool restart = settings.enabled && !m_wasEnabled && m_frame > 1;
    m_wasEnabled = settings.enabled;
    if (m_pool.size() != settings.budget || restart) {
      m_pool.reset(settings.budget);
      for (auto &emitter : m_emitters) {
        emitter.offset = emitter.capacity = 0;
        emitter.pendingReset = true;
      }
      frame.resetAll = true;
    }
    auto &s = m_statistics;
    s.enabled = settings.enabled && m_rendered;
    s.budget = settings.budget;
    s.spawnBudget = settings.spawnBudget;
    s.emitters = uint32_t(m_emitters.size());
    s.spawnsRequested = s.spawnsDropped = s.activeEmitters = 0;
    if (!settings.enabled) {
      for (auto &emitter : m_emitters) {
        emitter.burst = 0;
        emitter.carry = 0;
      }
      s.liveParticles = 0;
      s.allocatedParticles = m_pool.used();
      return frame;
    }
    uint32_t requested = 0, dropped = 0;
    for (auto &emitter : m_emitters) {
      const uint32_t wanted = std::min(wantedCapacity(emitter.info), settings.budget);
      if (emitter.capacity && emitter.capacity != wanted)
        releaseSlice(emitter);
      if (!emitter.capacity) {
        const uint32_t offset = m_pool.allocate(wanted);
        if (offset != UINT32_MAX) {
          emitter.offset = offset;
          emitter.capacity = wanted;
          emitter.pendingReset = true;
        }
      }
      auto resolved = resolveParticleEmitter(emitter.info, settings.metersPerUnit, settings.upAxis);
      double rate = emitter.enabled ? emitter.info.spawnRate : 0;
      if (emitter.info.preset != REMIXAPI_PARTICLE_PRESET_NONE_EXT && rate == 0 && emitter.enabled)
        rate = presetRate(emitter.info.preset);
      if (emitter.info.flags & REMIXAPI_PARTICLE_FOLLOW_WEATHER_EXT)
        rate *= std::clamp(settings.precipitation, 0.f, 1.f);
      emitter.carry += rate * frame.seconds;
      const double whole = std::floor(emitter.carry);
      emitter.carry -= whole;
      uint64_t spawn = uint64_t(whole) + emitter.burst;
      emitter.burst = 0;
      if (!emitter.capacity) {
        dropped += uint32_t(std::min<uint64_t>(spawn, UINT32_MAX));
        continue;
      }
      // More than a slice a frame would only replace itself.
      if (spawn > emitter.capacity) {
        dropped += uint32_t(spawn - emitter.capacity);
        spawn = emitter.capacity;
      }
      resolved.handle = emitter.handle;
      resolved.offset = emitter.offset;
      resolved.capacity = emitter.capacity;
      resolved.slot = emitter.slot;
      resolved.spawn = uint32_t(spawn);
      resolved.reset = emitter.pendingReset;
      emitter.pendingReset = false;
      requested += resolved.spawn;
      s.activeEmitters += emitter.enabled;
      frame.emitters.push_back(resolved);
    }
    // The spawn budget, shared in proportion to what each emitter asked for.
    if (requested > settings.spawnBudget) {
      const double scale = double(settings.spawnBudget) / requested;
      uint32_t kept = 0;
      for (auto &emitter : frame.emitters) {
        const uint32_t take = uint32_t(emitter.spawn * scale);
        dropped += emitter.spawn - take;
        emitter.spawn = take;
        kept += take;
      }
      requested = kept;
    }
    frame.spawns = requested;
    s.spawnsRequested = requested;
    s.spawnsDropped = dropped;
    s.allocatedParticles = m_pool.used();
    ++s.simulatedFrames;
    return frame;
  }

  void ParticleRegistry::report(const GpuReport &report) {
    std::lock_guard lock(m_mutex);
    m_statistics.liveParticles = report.live;
    m_statistics.spawnsDropped += report.dropped;
    m_statistics.spawnsSheltered = report.sheltered;
    m_statistics.gpuMilliseconds = report.milliseconds;
  }

  void ParticleRegistry::setRendered(bool rendered) {
    std::lock_guard lock(m_mutex);
    m_rendered = rendered;
  }

  remixapi_ParticleStatisticsEXT ParticleRegistry::statistics() const {
    std::lock_guard lock(m_mutex);
    auto out = m_statistics;
    out.structSize = sizeof(out);
    out.emitters = uint32_t(m_emitters.size());
    return out;
  }

  ParticleRegistry &particles() {
    static ParticleRegistry registry;
    return registry;
  }
} // namespace remix::scene

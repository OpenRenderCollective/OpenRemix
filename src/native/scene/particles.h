#pragma once
#include <remix/remix_particles_ext.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Particle emitters (remixapi_ParticleEmitterInfoEXT): the CPU registry the
// API, the developer UI and the renderer share. Emitters live here under a
// lock; each Present publishes a ParticleFrame (presets resolved, spawn counts
// of the frame, slices of the GPU pool) that the GPU simulation consumes.
namespace remix::scene {
  inline constexpr uint32_t maxParticleEmitters = 256;
  inline constexpr uint32_t maxParticlesPerEmitter = 65536;
  inline constexpr uint32_t defaultParticlesPerEmitter = 4096;

  // A null message: valid.
  const char *validateParticleEmitter(const remixapi_ParticleEmitterInfoEXT &info);

  // An emitter as the GPU takes it: presets applied, lengths in world units,
  // camera volumes in world axes.
  struct ResolvedEmitter {
    uint64_t handle = 0;
    uint32_t shape = 0, facing = 0, blend = 0, flags = 0;
    float transform[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}; // emitter to world, rows
    float extent[3] = {};
    float lifetime[2] = {1, 1};
    float velocity[3] = {}; // world
    float axis[3] = {0, 0, 1};
    float speed = 0, cosCone = 1;
    float acceleration[3] = {};
    float drag = 0, turbulence = 0;
    float size[3] = {}; // start, end, jitter
    float stretch = 0;
    float colorStart[4] = {1, 1, 1, 1}, colorEnd[4] = {1, 1, 1, 1};
    float emission[3] = {};
    float softDistance = 0;
    uint32_t seed = 0;
    uint64_t texture = 0; // remixapi_TextureHandle value
    // The emitter's slice of the GPU pool, its counter (0..255, kept for its
    // life) and this frame's work.
    uint32_t offset = 0, capacity = 0, slot = 0;
    uint32_t spawn = 0;
    bool reset = false; // its live particles go before this frame's spawns
  };

  // Fills a preset's fields and turns a camera volume's emitter space into
  // world axes (upAxis: 0 x, 1 y, 2 z).
  ResolvedEmitter resolveParticleEmitter(const remixapi_ParticleEmitterInfoEXT &info, float metersPerUnit,
                                         uint32_t upAxis);

  struct ParticleFrame {
    bool enabled = false;
    float seconds = 0; // since the previous frame
    uint32_t budget = 0;
    uint64_t frame = 0;
    // The budget changed: every slice moved.
    bool resetAll = false;
    std::vector<ResolvedEmitter> emitters; // those with a slice
    uint32_t spawns = 0;                   // of all emitters, within the spawn budget
  };

  // First-fit ranges of [0, size).
  class RangeAllocator {
  public:
    void reset(uint32_t size);
    // UINT32_MAX when no range of count is free.
    uint32_t allocate(uint32_t count);
    void release(uint32_t offset, uint32_t count);
    uint32_t size() const { return m_size; }
    uint32_t used() const { return m_used; }

  private:
    struct Range {
      uint32_t offset, count;
    };
    std::vector<Range> m_free; // sorted by offset, coalesced
    uint32_t m_size = 0, m_used = 0;
  };

  class ParticleRegistry {
  public:
    remixapi_ErrorCode create(const remixapi_ParticleEmitterInfoEXT &info, remixapi_ParticleEmitterHandleEXT &out,
                              bool internal = false);
    remixapi_ErrorCode update(remixapi_ParticleEmitterHandleEXT emitter, const remixapi_ParticleEmitterInfoEXT &info);
    remixapi_ErrorCode setTransform(remixapi_ParticleEmitterHandleEXT emitter, const remixapi_Transform &transform);
    remixapi_ErrorCode setEnabled(remixapi_ParticleEmitterHandleEXT emitter, bool enabled);
    remixapi_ErrorCode burst(remixapi_ParticleEmitterHandleEXT emitter, uint32_t count);
    remixapi_ErrorCode clear(remixapi_ParticleEmitterHandleEXT emitter);
    remixapi_ErrorCode destroy(remixapi_ParticleEmitterHandleEXT emitter);
    // Every emitter and statistic gone (Shutdown).
    void reset();

    struct PublishSettings {
      bool enabled = true;
      double seconds = 0;
      uint32_t budget = 65536, spawnBudget = 8192;
      float metersPerUnit = 1;
      uint32_t upAxis = 2;
      float precipitation = 1; // FOLLOW_WEATHER's scale
      // The developer UI's test emitters: a preset camera volume each, its
      // rate scaled by intensity.
      bool rain = false, snow = false, shelter = true, followWeather = false;
      float intensity = .5f;
    };
    // The frame's emitters: takes pending bursts and clears.
    ParticleFrame publish(const PublishSettings &settings);

    // What the GPU did with a completed frame.
    struct GpuReport {
      uint32_t live = 0, dropped = 0, sheltered = 0;
      float milliseconds = 0;
    };
    void report(const GpuReport &report);
    // Whether the backend draws particles (the Metal backend does not).
    void setRendered(bool rendered);
    remixapi_ParticleStatisticsEXT statistics() const;

  private:
    struct Emitter {
      remixapi_ParticleEmitterHandleEXT handle = 0;
      remixapi_ParticleEmitterInfoEXT info{};
      bool enabled = true, internal = false;
      bool pendingReset = true; // new, cleared or resized
      uint32_t burst = 0;
      double carry = 0;          // fractional spawns
      uint32_t offset = 0, capacity = 0;
      uint32_t slot = 0;
    };
    Emitter *find(remixapi_ParticleEmitterHandleEXT handle);
    void releaseSlice(Emitter &emitter);
    uint32_t freeSlot() const;
    void syncTestEmitter(remixapi_ParticleEmitterHandleEXT &handle, uint32_t preset, bool wanted,
                         const PublishSettings &settings);

    mutable std::mutex m_mutex;
    std::vector<Emitter> m_emitters;
    remixapi_ParticleEmitterHandleEXT m_next = 1;
    remixapi_ParticleEmitterHandleEXT m_rainTest = 0, m_snowTest = 0;
    RangeAllocator m_pool;
    uint64_t m_frame = 0;
    bool m_rendered = false, m_wasEnabled = false;
    remixapi_ParticleStatisticsEXT m_statistics{};
  };

  // The process's registry: the API, the UI and the runtime of either backend.
  ParticleRegistry &particles();
} // namespace remix::scene

#pragma once
#include "scene/types.h"
#include <algorithm>
#include <tuple>
#include <utility>
#include <vector>

namespace remix::scene {
  struct TemporalEpoch {
    uint64_t world = 0, reset = 0;
    bool operator==(const TemporalEpoch &) const = default;
  };
  using TemporalKey = std::tuple<uint64_t, uint32_t, uint32_t>;
  // Per-frame keyed table: dense entries plus an open-addressing index. clear()
  // keeps both allocations, so rebuilding it every frame allocates nothing.
  // Inserting an existing key replaces its value, as std::map::operator[] does.
  template <class Value> class TemporalTable {
  public:
    Value *find(const TemporalKey &key) {
      const uint32_t slot = locate(key);
      return slot != missing && m_slots[slot] ? &m_entries[m_slots[slot] - 1].second : nullptr;
    }
    const Value *find(const TemporalKey &key) const { return const_cast<TemporalTable *>(this)->find(key); }
    // Constructs key's value in place, or replaces an existing one.
    template <class... Arguments> Value &emplace(const TemporalKey &key, Arguments &&...arguments) {
      if ((m_entries.size() + 1) * 2 > m_slots.size())
        grow();
      const uint32_t slot = locate(key);
      if (m_slots[slot]) {
        auto &value = m_entries[m_slots[slot] - 1].second;
        value = Value(std::forward<Arguments>(arguments)...);
        return value;
      }
      m_entries.emplace_back(std::piecewise_construct, std::forward_as_tuple(key),
                             std::forward_as_tuple(std::forward<Arguments>(arguments)...));
      m_slots[slot] = uint32_t(m_entries.size());
      return m_entries.back().second;
    }
    Value &operator[](const TemporalKey &key) {
      if ((m_entries.size() + 1) * 2 > m_slots.size())
        grow();
      const uint32_t slot = locate(key);
      if (!m_slots[slot]) {
        m_entries.emplace_back(key, Value{});
        m_slots[slot] = uint32_t(m_entries.size());
      }
      return m_entries[m_slots[slot] - 1].second;
    }
    void clear() {
      if (m_entries.empty())
        return;
      m_entries.clear();
      std::fill(m_slots.begin(), m_slots.end(), 0u);
    }
    void reserve(size_t count) {
      m_entries.reserve(count);
      if (count * 2 > m_slots.size()) {
        size_t slots = 64;
        while (slots < count * 2)
          slots *= 2;
        m_slots.assign(slots / 2, 0u); // grow() doubles and reinserts.
        grow();
      }
    }
    size_t size() const { return m_entries.size(); }
    bool empty() const { return m_entries.empty(); }
    auto begin() { return m_entries.begin(); }
    auto end() { return m_entries.end(); }
    auto begin() const { return m_entries.begin(); }
    auto end() const { return m_entries.end(); }

  private:
    static constexpr uint32_t missing = ~0u;
    static uint64_t hash(const TemporalKey &key) {
      uint64_t h = std::get<0>(key) ^ (uint64_t(std::get<1>(key)) << 32 | std::get<2>(key)) * 0x9E3779B97F4A7C15ull;
      h ^= h >> 31;
      h *= 0xBF58476D1CE4E5B9ull;
      return h ^ (h >> 29);
    }
    // The slot holding key, or the empty slot where it belongs.
    uint32_t locate(const TemporalKey &key) const {
      if (m_slots.empty())
        return missing;
      const uint32_t mask = uint32_t(m_slots.size() - 1);
      for (uint32_t slot = uint32_t(hash(key)) & mask;; slot = (slot + 1) & mask)
        if (!m_slots[slot] || m_entries[m_slots[slot] - 1].first == key)
          return slot;
    }
    void grow() {
      m_slots.assign(std::max<size_t>(64, m_slots.size() * 2), 0u);
      const uint32_t mask = uint32_t(m_slots.size() - 1);
      for (uint32_t i = 0; i < m_entries.size(); ++i) {
        uint32_t slot = uint32_t(hash(m_entries[i].first)) & mask;
        while (m_slots[slot])
          slot = (slot + 1) & mask;
        m_slots[slot] = i + 1;
      }
    }
    std::vector<std::pair<TemporalKey, Value>> m_entries;
    std::vector<uint32_t> m_slots; // entry index + 1; zero is empty
  };

  // A transaction is committed only after vkQueueSubmit succeeds. API submissions
  // and discarded/minimized frames never become motion-vector history.
  class TemporalHistory {
  public:
    struct SurfaceHistory {
      SurfaceHistory() = default;
      SurfaceHistory(Draw &&draw, uint32_t textureFrame, const std::array<float, 4> &uv = {1, 1, 0, 0})
          : draw(std::move(draw)), textureFrame(textureFrame), uv(uv) {}
      Draw draw;
      uint32_t textureFrame = 0;
      std::array<float, 4> uv{1, 1, 0, 0}; // the UV transform it was traced with (surfaceTexcoord)
    };
    using Key = TemporalKey;
    void begin(TemporalEpoch epoch, uint64_t integrator, uint32_t width, uint32_t height);
    // Geometry correspondence and an unchanged material signature.
    const SurfaceHistory *previous(const Draw &draw) const;
    // Motion correspondence survives shading changes. Consumers must validate
    // reprojected shading separately (depth/normal, radiance and color clipping).
    const SurfaceHistory *previousGeometry(const Draw &draw) const { return previousGeometry(draw, draw.instanceId); }
    // As above, with the draw's temporal identity replaced by instanceId.
    const SurfaceHistory *previousGeometry(const Draw &draw, uint64_t instanceId) const;
    void stage(const Draw &draw, uint32_t textureFrame = 0) { stage(Draw(draw), textureFrame); }
    // Stores the draw under instanceId (default: its own), consuming it.
    void stage(Draw &&draw, uint32_t textureFrame = 0);
    void stage(Draw &&draw, uint64_t instanceId, uint32_t textureFrame, const std::array<float, 4> &uv = {1, 1, 0, 0});
    void commit();
    void clear();
    bool reset() const { return m_reset; }
    uint64_t frames() const { return m_frames; }
    static uint64_t materialSignature(const Draw &draw);
    // Surfaces the last committed frame traced without staging them here (the
    // persistent static set keeps their history itself). Consulted for keys
    // this table does not hold; a key is never in both.
    class Fallback {
    public:
      virtual const SurfaceHistory *previous(const TemporalKey &key) const = 0;

    protected:
      ~Fallback() = default;
    };
    void setFallback(const Fallback *fallback) { m_fallback = fallback; }

  private:
    const Fallback *m_fallback = nullptr;
    TemporalTable<SurfaceHistory> m_previous, m_pending;
    TemporalEpoch m_epoch{}, m_nextEpoch{};
    uint64_t m_integrator = 0, m_nextIntegrator = 0, m_frames = 0;
    uint32_t m_width = 0, m_height = 0, m_nextWidth = 0, m_nextHeight = 0;
    bool m_reset = true;
  };
} // namespace remix::scene

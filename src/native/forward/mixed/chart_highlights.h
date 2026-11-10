#pragma once
#include "forward/bake/data.h"
#include <chrono>
#include <future>
#include <optional>
#include <span>
#include <stop_token>

namespace remix::forward::lightmap { struct Layout; }

namespace remix::forward::mixed {
  struct Plan;
  struct ChartHighlightLight;
  class Occlusion;
  bool chartHighlightsEnabled();

  // One blocked bit per captured light. Clear and unknown both remain eligible.
  class BlockedChartLights {
  public:
    BlockedChartLights() = default;
    explicit BlockedChartLights(const Occlusion &occlusion);
    uint32_t chartCount() const { return m_charts; }
    size_t bytes() const { return (m_words.size() + m_anyBlocked.size()) * sizeof(uint16_t); }
    bool mayBlock(uint32_t light) const;
    // An empty result means the ordinary runtime list needs no chart mask.
    std::vector<uint32_t> visibleMasks(std::span<const ChartHighlightLight> selected, uint32_t runtimeMask) const;
    std::vector<uint32_t> wideVisibleMasks(std::span<const ChartHighlightLight> selected, uint32_t rowWords) const;

  private:
    uint32_t m_charts = 0, m_stride = 0;
    // Each column contains the same 16 captured lights across every chart.
    std::vector<uint16_t> m_words;
    std::vector<uint16_t> m_anyBlocked;
  };

  struct ChartHighlightLight {
    uint32_t id;
    Vec3 radiance;
    float range;
    uint32_t runtimeIndex = 0;
    bool operator==(const ChartHighlightLight &) const = default;
  };

  // Built once from existing lightmap and mask data. The frame worker only
  // needs these representatives and the lights in its settled selection.
  struct ChartHighlightField {
    static constexpr uint32_t blockSide = 8, fallback = ~0u;
    static constexpr uint8_t unknown = 255;
    static constexpr uint32_t clusterMembershipFlag = 1u << 31;
    static constexpr uint32_t exactVisibilityFlag = 1u << 30;
    std::shared_ptr<const scene::BakeScene> source;
    std::vector<uint32_t> chartOffsets;
    std::vector<bake::Point> points;
    std::vector<uint8_t> visibility;
    std::array<uint8_t, 254> columns{};
    uint32_t stride = 0;
    // Immutable proofs used for exact-only tables and unknown ranked blocks.
    BlockedChartLights exactOcclusion;

    bool supports(uint32_t id) const { return id < columns.size() && columns[id] != unknown; }
    bool exactVisibility() const { return chartOffsets.empty() && exactOcclusion.chartCount() != 0; }

    std::vector<uint32_t> rank(std::span<const ChartHighlightLight> selected,
                               std::optional<uint32_t> required = {}) const;

    static std::shared_ptr<const ChartHighlightField> prepare(const Plan &plan, const lightmap::Layout &layout,
        std::span<const uint16_t> pixels, std::span<const uint32_t> completeStyles, std::stop_token cancelled);
  };

  class ChartHighlightWork {
  public:
    struct Table {
      std::shared_ptr<const std::vector<uint32_t>> words;
      uint32_t flags = 0;
    };
    Table prepare(std::shared_ptr<const ChartHighlightField> field, std::span<const LightData> runtimeLights);

  private:
    std::shared_ptr<const std::vector<uint32_t>> update(std::shared_ptr<const ChartHighlightField> field,
        std::span<const ChartHighlightLight> lights, std::optional<uint32_t> required, uint32_t exactRowWords);

    struct Key {
      std::shared_ptr<const ChartHighlightField> field;
      std::vector<ChartHighlightLight> lights;
      std::optional<uint32_t> required;
      uint32_t exactRowWords = 0;
      bool operator==(const Key &) const = default;
    };
    Key m_requested, m_pending, m_finished;
    std::vector<ChartHighlightLight> m_candidates;
    std::chrono::steady_clock::time_point m_readyAt{};
    std::shared_ptr<const std::vector<uint32_t>> m_result;
    std::future<std::shared_ptr<const std::vector<uint32_t>>> m_worker;
  };
}

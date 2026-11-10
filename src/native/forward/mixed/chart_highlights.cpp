#include "forward/mixed/chart_highlights.h"
#include "forward/half.h"
#include "forward/light_response.h"
#include "forward/lighting.h"
#include "forward/lightmap/receiver_sampler.h"
#include "forward/mixed/data.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <new>
#include <string_view>

namespace remix::forward::mixed {
  BlockedChartLights::BlockedChartLights(const Occlusion &occlusion)
      : m_charts(occlusion.chartCount()), m_stride(occlusion.wordsPerChart()), m_anyBlocked(m_stride) {
    m_words.resize(occlusion.words().size());
    for (uint32_t chart = 0; chart < m_charts; ++chart) {
      const auto row = occlusion.words().subspan(size_t(chart) * m_stride, m_stride);
      for (uint32_t column = 0; column < m_stride; ++column) {
        // Keep the low bit of each two-bit state, then close the gaps between bits.
        uint32_t word = row[column] & 0x55555555u;
        word = (word | (word >> 1)) & 0x33333333u;
        word = (word | (word >> 2)) & 0x0f0f0f0fu;
        word = (word | (word >> 4)) & 0x00ff00ffu;
        const auto blocked = uint16_t(word | (word >> 8));
        m_words[size_t(column) * m_charts + chart] = blocked;
        m_anyBlocked[column] |= blocked;
      }
    }
  }

  bool BlockedChartLights::mayBlock(uint32_t light) const {
    const uint32_t word = light / 16;
    return word < m_stride && (m_anyBlocked[word] & (1u << (light % 16))) != 0;
  }

  std::vector<uint32_t> BlockedChartLights::visibleMasks(std::span<const ChartHighlightLight> selected,
                                                        uint32_t runtimeMask) const {
    struct Projection {
      std::span<const uint16_t> column;
      uint32_t blockedBit, runtimeIndex;
    };
    std::vector<Projection> projections;
    projections.reserve(selected.size());
    for (const auto &light : selected) {
      const uint32_t word = light.id / 16;
      if (!mayBlock(light.id) || light.runtimeIndex >= 32) continue;
      const uint32_t runtimeBit = 1u << light.runtimeIndex;
      if (runtimeMask & runtimeBit)
        projections.push_back({std::span(m_words).subspan(size_t(word) * m_charts, m_charts),
                               1u << (light.id % 16), light.runtimeIndex});
    }
    if (projections.empty()) return {};

    // Sweep contiguous chart proofs for each selected light. The loop can
    // clear many chart masks at once without touching unrelated proof words.
    std::vector<uint32_t> masks(m_charts, runtimeMask);
    for (const auto &light : projections) {
      for (uint32_t chart = 0; chart < m_charts; ++chart) {
        const uint32_t blocked = (light.column[chart] & light.blockedBit) != 0;
        masks[chart] &= ~(blocked << light.runtimeIndex);
      }
    }
    return masks;
  }

  bool chartHighlightsEnabled() {
    static const bool enabled = [] {
      const char *override = std::getenv("REMIX_DEBUG_CHART_HIGHLIGHTS");
      return !override || std::string_view(override) != "0";
    }();
    return enabled;
  }

  std::vector<uint32_t> BlockedChartLights::wideVisibleMasks(std::span<const ChartHighlightLight> selected,
                                                            uint32_t rowWords) const {
    constexpr size_t maxWords = (8u << 20) / sizeof(uint32_t);
    if (selected.empty() || rowWords == 0 || m_charts > maxWords / rowWords) return {};
    std::vector<uint32_t> masks(size_t(m_charts) * rowWords, ~0u);
    for (const auto &light : selected) {
      const uint32_t runtimeWord = light.runtimeIndex / 32;
      if (runtimeWord >= rowWords || !mayBlock(light.id)) continue;
      const auto column = std::span(m_words).subspan(size_t(light.id / 16) * m_charts, m_charts);
      const uint32_t capturedBit = 1u << (light.id % 16);
      for (uint32_t chart = 0; chart < m_charts; ++chart) {
        const uint32_t blocked = (column[chart] & capturedBit) != 0;
        masks[size_t(chart) * rowWords + runtimeWord] &= ~(blocked << (light.runtimeIndex % 32));
      }
    }
    return masks;
  }

  std::vector<uint32_t> ChartHighlightField::rank(std::span<const ChartHighlightLight> selected,
                                                  std::optional<uint32_t> required) const {
    if (exactVisibility())
      return required ? exactOcclusion.visibleMasks(selected, *required) : std::vector<uint32_t>{};
    auto words = chartOffsets;
    words.reserve(words.size() + points.size());
    bool hasShortlist = false;
    bool hasFallback = false;
    for (size_t block = 0; block < points.size(); ++block) {
      const auto &sample = points[block];
      if (!sample.normal[3]) {
        words.push_back(fallback);
        hasFallback = true;
        continue;
      }
      std::array<std::pair<float, uint32_t>, 4> best{};
      bool complete = true;
      for (const auto &candidate : selected) {
        const auto value = visibility[block * stride + columns[candidate.id]];
        if (value == 0) continue;
        const auto &light = source->lights[candidate.id];
        Vec3 delta{light.position[0] - sample.position[0], light.position[1] - sample.position[1],
                   light.position[2] - sample.position[2]};
        const float distance2 = std::max(dot(delta, delta), 1e-8f);
        const float distance = std::sqrt(distance2);
        if (distance >= candidate.range) continue;
        for (auto &axis : delta) axis /= distance;
        const float cosine = std::max(0.f, dot(delta, {sample.normal[0], sample.normal[1], sample.normal[2]}));
        const float axisCos = -dot(delta, {light.direction[0], light.direction[1], light.direction[2]});
        if (cosine <= 0 || (light.coneCos > -1.f && axisCos <= light.coneCos)) continue;
        float cone = light.focus > 0 ? std::pow(std::max(axisCos, 0.f), light.focus) : 1.f;
        const float inner = coneInnerCosine(light.coneCos, light.softness);
        if (light.coneCos > -1.f && inner > light.coneCos) {
          const float t = std::clamp((axisCos - light.coneCos) / (inner - light.coneCos), 0.f, 1.f);
          cone *= t * t * (3.f - 2.f * t);
        }
        if (cone <= 0) continue;
        if (value == unknown) {
          complete = false;
          break;
        }
        const float radius2 = light.radius * light.radius;
        const float sine2 = std::min(radius2 / distance2, 1.f);
        const float solidAngle = distance2 <= radius2 ? 2.f : sine2 / (1.f + std::sqrt(1.f - sine2));
        const float fade = 1.f - std::pow(distance / candidate.range, 4.f);
        const float luminance = dot(candidate.radiance, {.2126f, .7152f, .0722f});
        const uint32_t id = required ? candidate.runtimeIndex : candidate.id;
        std::pair ranked{luminance * cosine * solidAngle * cone * fade * fade * value, id + 1};
        for (auto &entry : best)
          if (ranked.first > entry.first) std::swap(ranked, entry);
      }
      uint32_t packed = required.value_or(0);
      for (uint32_t i = 0; i < best.size(); ++i) {
        const uint32_t id = best[i].second;
        if (required) {
          if (id) packed |= 1u << (id - 1);
        } else packed |= id << (8 * i);
      }
      words.push_back(complete ? packed : fallback);
      hasShortlist |= complete;
      hasFallback |= !complete;
    }
    if (required && hasFallback && exactOcclusion.chartCount() == chartOffsets.size()) {
      // Ranking can fail when a tile lacks filtered visibility. A whole-chart
      // blocked proof is still sufficient to remove that light. Keep every
      // other runtime slot, including lights that aren't ranked.
      const auto masks = exactOcclusion.visibleMasks(selected, ~0u);
      for (size_t chart = 0; chart < masks.size(); ++chart) {
        if (masks[chart] == fallback) continue;
        const size_t end = chart + 1 < chartOffsets.size() ? size_t(chartOffsets[chart + 1]) : words.size();
        for (size_t block = chartOffsets[chart]; block < end; ++block) {
          if (words[block] != fallback) continue;
          words[block] = masks[chart];
          hasShortlist = true;
        }
      }
    }
    // A table made entirely of fallback entries cannot remove any light work.
    if (!hasShortlist) return {};
    return words;
  }

  std::shared_ptr<const ChartHighlightField> ChartHighlightField::prepare(const Plan &plan, const lightmap::Layout &layout,
      std::span<const uint16_t> pixels, std::span<const uint32_t> completeStyles, std::stop_token cancelled) {
    if (cancelled.stop_requested() || !plan.lights.source || layout.mappings.size() != layout.charts.size() ||
        plan.charts.size() != layout.charts.size() || pixels.empty() ||
        pixels.size() != uint64_t(layout.pageSize) * layout.pageSize * layout.pages * lightmap::atlasLayers * 4)
      return {};

    auto field = std::make_shared<ChartHighlightField>();
    field->source = plan.lights.source;
    field->columns.fill(unknown);
    std::vector<uint32_t> candidates;
    for (uint32_t id = 0; id < std::min(field->columns.size(), field->source->lights.size()); ++id) {
      const auto &light = field->source->lights[id];
      const bool grouped = light.style &&
          std::find(completeStyles.begin(), completeStyles.end(), light.style) != completeStyles.end();
      if (plan.lights.retainsVisibility(id) && (plan.lights.bakedDiffuse(id) || grouped) &&
          !light.dynamic && !light.triangle && !light.ies && !light.cookie && light.forwardCutoff == 0) {
        field->columns[id] = uint8_t(candidates.size());
        candidates.push_back(id);
      }
    }
    field->stride = uint32_t(candidates.size());
    size_t blocks = 0;
    for (const auto &chart : layout.charts)
      blocks += size_t((chart.width + 7) / blockSide) * ((chart.height + 7) / blockSide);
    constexpr size_t budget = 32u << 20;
    constexpr size_t visibilityBudget = 8u << 20;
    const bool canKeepProofs = plan.occlusion.count() != 0 &&
        plan.occlusion.words().size_bytes() <= visibilityBudget;
    if (candidates.size() <= 4 || blocks > budget / (sizeof(bake::Point) + field->stride)) {
      if (!canKeepProofs) return {};
      field->exactOcclusion = BlockedChartLights(plan.occlusion);
      return field;
    }

    lightmap::ReceiverSampler sampler(*field->source, layout);
    std::vector<std::vector<const Masks::Entry *>> masks(layout.charts.size());
    for (const auto &entry : plan.masks.entries())
      masks[entry.chart].push_back(&entry);
    field->points.reserve(blocks);
    field->visibility.resize(blocks * field->stride, unknown);
    bool needsFallbackProofs = false;
    for (uint32_t chart = 0; chart < layout.charts.size(); ++chart) {
      if (cancelled.stop_requested()) return {};
      const auto &allocation = layout.charts[chart];
      const auto &owners = plan.charts[chart];
      const auto layer = size_t(allocation.page) * lightmap::atlasLayers + lightmap::visibilityLayer;
      field->chartOffsets.push_back(uint32_t(layout.charts.size() + field->points.size()));
      for (uint32_t y = 0; y < allocation.height; y += blockSide)
        for (uint32_t x = 0; x < allocation.width; x += blockSide) {
          uint32_t u = std::min(x + blockSide / 2, allocation.width - 1);
          uint32_t v = std::min(y + blockSide / 2, allocation.height - 1);
          auto sample = sampler.pointAt(chart, {float(u), float(v)});
          if (!sample) {
            const std::array us{x, u, std::min(x + blockSide - 1, allocation.width - 1)};
            const std::array vs{y, v, std::min(y + blockSide - 1, allocation.height - 1)};
            // Prefer edge midpoints to corners. Use the same texel for the
            // position and for any sampled visibility in the ranking.
            constexpr std::array<std::array<uint32_t, 2>, 8> nearby{{
                {0, 1}, {2, 1}, {1, 0}, {1, 2}, {0, 0}, {2, 0}, {0, 2}, {2, 2}}};
            for (const auto &[column, row] : nearby) {
              if (us[column] == u && vs[row] == v) continue;
              sample = sampler.pointAt(chart, {float(us[column]), float(vs[row])});
              if (!sample) continue;
              u = us[column];
              v = vs[row];
              break;
            }
          }
          if (!sample) {
            // Thin triangles can pass between the midpoint and edge samples.
            const uint32_t endX = std::min(x + blockSide, allocation.width);
            const uint32_t endY = std::min(y + blockSide, allocation.height);
            for (uint32_t row = y; row < endY && !sample; ++row)
              for (uint32_t column = x; column < endX; ++column) {
                sample = sampler.pointAt(chart, {float(column), float(row)});
                if (!sample) continue;
                u = column;
                v = row;
                break;
              }
          }
          const auto base = field->points.size() * field->stride;
          if (sample) sample->normal[3] = 1;
          field->points.push_back(sample.value_or(bake::Point{}));
          if (!sample) {
            needsFallbackProofs = true;
            continue;
          }
          for (const auto id : candidates) {
            auto &value = field->visibility[base + field->columns[id]];
            const auto state = plan.occlusion.state(chart, id);
            if (state != Visibility::Unknown) {
              value = state == Visibility::Clear ? 254 : 0;
              continue;
            }
            const auto owner = std::find(owners.begin(), owners.end(), id);
            if (owner != owners.end()) {
              const auto texel = ((layer * layout.pageSize + allocation.y + v) *
                                  layout.pageSize + allocation.x + u) * 4;
              value = uint8_t(std::lround(half::decode(pixels[texel + (owner - owners.begin())]) * 254));
              continue;
            }
            const auto &entries = masks[chart];
            const auto found = std::find_if(entries.begin(), entries.end(),
                [id](const Masks::Entry *entry) { return entry->light == id; });
            if (found != entries.end()) {
              const auto &entry = **found;
              const auto encoded = mask::texel(plan.masks.bytes().subspan(entry.offset), entry.width,
                                               entry.encoding, u, v);
              value = uint8_t((uint32_t(encoded) * 254 + 127) / 255);
            } else {
              needsFallbackProofs = true;
            }
          }
        }
    }
    if (cancelled.stop_requested()) return {};
    if (canKeepProofs && needsFallbackProofs) {
      try {
        field->exactOcclusion = BlockedChartLights(plan.occlusion);
      } catch (const std::bad_alloc &) {
        // Ranked tiles remain usable without the optional fallback proofs.
      }
    }
    return field;
  }

  ChartHighlightWork::Table ChartHighlightWork::prepare(std::shared_ptr<const ChartHighlightField> field,
                                                        std::span<const LightData> runtimeLights) {
    m_candidates.clear();
    std::optional<uint32_t> required;
    const bool exact = field && field->exactVisibility();
    const uint32_t exactRowWords = exact && runtimeLights.size() > 32
        ? uint32_t((runtimeLights.size() + 31) / 32) : 0;
    if (field) {
      if ((exact && exactRowWords == 0) || runtimeLights.size() == 32)
        required = ~0u; // Exact masks are clipped to the current prefix in the shader.
      else if (runtimeLights.size() < 32)
        required = (1u << runtimeLights.size()) - 1u;
      for (uint32_t index = 0; index < runtimeLights.size(); ++index) {
        const auto &light = runtimeLights[index];
        const uint32_t id = uint32_t(light.shaping[3]) - 1u;
        if (exact) {
          if (field->exactOcclusion.mayBlock(id)) m_candidates.push_back({id, {}, 0, index});
          continue;
        }
        if (light.corners[2][3] != float(LightResponse::SelectedAggregate) ||
            light.corners[0][3] != 0 || light.responseWeights[0] <= 0) continue;
        if (!field->supports(id) || light.responseWeights[0] != 1) {
          m_candidates.clear();
          break;
        }
        m_candidates.push_back({id, {light.radiance[0], light.radiance[1], light.radiance[2]},
                                light.shaping[2], required ? index : 0});
        if (required) *required &= ~(1u << index);
      }
      if (!exact) {
        std::ranges::sort(m_candidates, {}, &ChartHighlightLight::id);
        if (std::ranges::adjacent_find(m_candidates, {}, &ChartHighlightLight::id) != m_candidates.end())
          m_candidates.clear();
      }
    }
    const uint32_t flags = (required ? ChartHighlightField::clusterMembershipFlag : 0u) |
                           (exact ? ChartHighlightField::exactVisibilityFlag : 0u);
    return {update(std::move(field), m_candidates, required, exactRowWords), flags};
  }

  std::shared_ptr<const std::vector<uint32_t>> ChartHighlightWork::update(std::shared_ptr<const ChartHighlightField> field,
      std::span<const ChartHighlightLight> lights, std::optional<uint32_t> required, uint32_t exactRowWords) {
    using namespace std::chrono;
    constexpr auto settleDelay = milliseconds(150);
    if (field != m_requested.field || required != m_requested.required || exactRowWords != m_requested.exactRowWords ||
        !std::ranges::equal(lights, m_requested.lights)) {
      m_requested.lights.assign(lights.begin(), lights.end());
      m_requested.field = std::move(field);
      m_requested.required = required;
      m_requested.exactRowWords = exactRowWords;
      // Exact proofs only project bitsets. Ranked lists rescore baked blocks
      // and still wait for the light selection to settle.
      m_readyAt = m_requested.field && !m_requested.field->exactVisibility()
          ? steady_clock::now() + settleDelay : steady_clock::time_point::min();
    }
    if (m_requested.field != m_finished.field) {
      m_finished = {};
      m_result.reset();
    }
    if (m_worker.valid() && m_worker.wait_for(seconds(0)) == std::future_status::ready) {
      const bool current = m_pending == m_requested;
      try {
        auto result = m_worker.get();
        if (current) {
          m_finished = std::move(m_pending);
          m_result = std::move(result);
        }
      } catch (const std::exception &) {
        if (current) m_readyAt = steady_clock::now() + settleDelay;
      }
      m_pending = {};
    }
    if (!m_requested.field || m_requested.lights.empty() ||
        (!m_requested.field->exactVisibility() && m_requested.lights.size() <= 4))
      return {};
    if (m_requested == m_finished)
      return m_result;
    if (!m_worker.valid() && steady_clock::now() >= m_readyAt) {
      try {
        m_pending = m_requested;
        m_worker = std::async(std::launch::async, [key = m_pending]()
            -> std::shared_ptr<const std::vector<uint32_t>> {
          auto words = key.exactRowWords
              ? key.field->exactOcclusion.wideVisibleMasks(key.lights, key.exactRowWords)
              : key.field->rank(key.lights, key.required);
          if (words.empty()) return {};
          return std::make_shared<const std::vector<uint32_t>>(std::move(words));
        });
      } catch (const std::exception &) {
        m_pending = {};
        m_readyAt = steady_clock::now() + settleDelay;
      }
    }
    return {};
  }
} // namespace remix::forward::mixed

#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace remix::forward {
  // Stored exactly as floats in LightData::corners[2].w; shared with direct-light.glsl.
  enum class LightResponse { Analytic = 0, Aggregate = 1, BakedFallback = 2, SelectedAggregate = 3 };

  struct LightResponseInput {
    bool aggregate = false;
    bool attributed = false;
    bool selectedOnly = false;
    bool requireMixed = false;
    bool shadowSelected = false;
    float shadowWeight = 0;
    bool independentHighlights = false;
    // Negative means no highlight slot was assigned. Zero is an
    // assigned slot starting its fade and must retain clustered membership.
    float highlightWeight = -1;
  };
  struct RuntimeLightResponse {
    LightResponse mode = LightResponse::Analytic;
    float highlightWeight = 1;
  };
  // Aggregate highlights do not consume unit-direct SH without a moving
  // correction. Analytic/styled responses still need their attributed diffuse.
  inline bool needsUnitDirect(LightResponse mode, bool shadowSelected, float shadowWeight) {
    if (mode == LightResponse::Analytic) return true;
    if (mode == LightResponse::BakedFallback) return false;
    return shadowSelected && (!std::isfinite(shadowWeight) || shadowWeight > 0);
  }
  inline bool drawNeedsUnitDirect(std::span<const uint32_t> owners, std::span<const uint8_t> needed) {
    return std::any_of(owners.begin(), owners.end(), [&](uint32_t owner) {
      return owner >= needed.size() || needed[owner];
    });
  }
  inline RuntimeLightResponse lightResponse(const LightResponseInput &input) {
    if (!input.aggregate) return {};
    if (!input.attributed) return {LightResponse::BakedFallback};
    if (!input.selectedOnly || input.requireMixed) return {LightResponse::Aggregate};
    const bool highlightSelected = input.independentHighlights &&
        std::isfinite(input.highlightWeight) && input.highlightWeight >= 0;
    if (!input.shadowSelected && !highlightSelected) return {LightResponse::BakedFallback};
    // Independent highlight selection may deliberately assign zero to a light
    // that still owns a shadow slot. Shadow strength never changes this weight.
    const float weight = input.independentHighlights ? std::max(input.highlightWeight, 0.f) : input.shadowWeight;
    return {LightResponse::SelectedAggregate, std::isfinite(weight) ? std::clamp(weight, 0.f, 1.f) : 0.f};
  }
} // namespace remix::forward

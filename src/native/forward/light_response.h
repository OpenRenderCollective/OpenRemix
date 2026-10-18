#pragma once

namespace remix::forward {
  // Stored exactly as floats in LightData::corners[2].w; shared with direct-light.glsl.
  enum class LightResponse { Analytic = 0, Aggregate = 1, BakedFallback = 2, SelectedAggregate = 3 };

  inline LightResponse lightResponse(bool aggregate, bool attributed, bool selectedOnly,
                                     bool selected, bool requireMixed) {
    if (!aggregate) return LightResponse::Analytic;
    if (!attributed) return LightResponse::BakedFallback;
    if (!selectedOnly || requireMixed) return LightResponse::Aggregate;
    return selected ? LightResponse::SelectedAggregate : LightResponse::BakedFallback;
  }
} // namespace remix::forward

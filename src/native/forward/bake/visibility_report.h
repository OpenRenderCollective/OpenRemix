#pragma once
#include <stop_token>

namespace remix::forward::lightmap { struct Layout; }
namespace remix::forward::mixed { struct Plan; }
namespace remix::forward::bake {
  // Optional frozen inputs and completed hardware classifications for an
  // offline visibility-partition audit. Never culls work or changes lighting.
  void dumpVisibilityReport(const mixed::Plan &plan, const lightmap::Layout &layout,
                            float receiverBias, std::stop_token cancelled = {}) noexcept;
}

#pragma once
#include "forward/probes/receiver_fits.h"

namespace remix::forward::probes {
  // Proposed marginal coverage only: receivers outside the existing automatic
  // set and inside at least one assessed local influence. Each receiver/origin
  // is traced once regardless of alternate boxes. Unknown/unqueried stays unknown.
  // Pair capture indices refer to ReceiverFits::attempts, not GPU image layers.
  std::vector<ReceiverPair> receiverFitPairs(std::span<const ReceiverSample> receivers,
      std::span<const Capture> baseline, const ReceiverFits &fits, float bias,
      uint32_t budget = 32768, std::stop_token cancelled = {});
}

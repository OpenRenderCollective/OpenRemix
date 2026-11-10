#pragma once
#include "forward/probes/data.h"
#include "forward/probes/receiver_visibility.h"

namespace remix::forward::probes {
  struct ReceiverPair {
    uint32_t receiver = 0, capture = 0;
    ReceiverRay ray;
    uint32_t rank = 0; // position in the stable nearest front-facing shortlist
  };
  // Worker-only planning: at most 8192 receivers, 64 captures and 4 nearest
  // front-facing captures per uncovered receiver. The ray budget is independent
  // of map light count; covered receivers do not consume geometry queries.
  // Budgeted receivers are spread through the input sample sequence, rather
  // than selecting only the first charts in a map. Indices retain input order.
  std::vector<ReceiverPair> receiverPairs(std::span<const ReceiverSample> receivers,
      std::span<const Capture> captures, float bias, uint32_t rayBudget = 4096,
      uint32_t nearest = 4, std::stop_token cancelled = {}, uint32_t skipNearest = 0);
  std::vector<ReceiverPair> receiverRetryPairs(std::span<const ReceiverSample> receivers,
      std::span<const Capture> captures, std::span<const ReceiverPair> previous,
      std::span<const ReceiverVisibility> visibility, float bias, uint32_t rayBudget,
      std::stop_token cancelled = {});
} // namespace remix::forward::probes

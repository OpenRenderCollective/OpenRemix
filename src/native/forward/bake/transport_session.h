#pragma once
#include "forward/bake/budget.h"
#include "forward/bake/transport.h"
#include <memory>
#include <stop_token>

namespace remix::forward::bake {
// Execution ownership shared by baked-lighting and reflection-only jobs. Embed
// in the concrete job: no separate allocation, virtual call or shared session
// pointer. Frame/worker owners retain that concrete job through its fences.
// A preset can sequence several techniques through this one consumer/budget;
// independent consumers can use Transport::initializeShared after preparation.
struct TransportSession {
  std::shared_ptr<const scene::BakeScene> source;
  std::unique_ptr<Transport> transport;
  Budget budget;
  std::stop_source cancellation;
};
}

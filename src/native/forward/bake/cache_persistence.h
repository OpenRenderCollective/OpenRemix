#pragma once
#include "forward/bake/cache.h"
#include "forward/cache_persistence.h"

namespace remix::forward::bake::cache {
class Persistence : public cache_io::Persistence<Snapshot> {
public:
  using Writer = std::function<bool(const std::filesystem::path &, uint64_t,
                                    const View &)>;
  explicit Persistence(uint64_t byteBudget = 4ull << 30, Writer writer = {});
  static uint64_t retainedCharge(const Snapshot &snapshot);
};
Persistence &persistence();
} // namespace remix::forward::bake::cache

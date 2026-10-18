#include "forward/bake/probe_visibility.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

using namespace remix::forward::bake;
namespace {
void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}
template <typename Function> void rejects(Function function) {
  bool rejected = false;
  try {
    function();
  } catch (const std::exception &) {
    rejected = true;
  }
  require(rejected, "Invalid input must be rejected");
}
} // namespace
int main() {
  const std::array<VisibilityTask, 6> pairs{
      {{2, 9, 0}, {0, 5, 0}, {0, 1, 0}, {0, 8, 0}, {0, 3, 0}, {0, 7, 0}}};
  constexpr uint64_t bytes =
      4 * sizeof(uint32_t) + pairs.size() * sizeof(ProbeVisibility::Entry);
  ProbeVisibility table(3, 10, pairs, bytes);
  require(table.bytes() == bytes && table.offsets()[1] == 5 &&
              table.offsets()[2] == 5,
          "Five overlapping lights and empty probes use only sparse allocated "
          "entries");
  require(table.entries()[0].light == 1 && table.entries()[4].light == 8,
          "Captured indices are sorted without renumbering");
  require(!table.sample(0, 1) && !table.sample(1, 1) &&
              !table.sample(UINT32_MAX, 1),
          "Untraced, unallocated and invalid coverage remains unknown");
  const std::array<float, 6> values{0, 1, .25f, .5f, .75f, 0};
  table.observe(pairs, values);
  require(table.sample(2, 9) == 0 && table.sample(0, 5) == 1 &&
              table.sample(0, 1) == .25f,
          "Readback preserves blocked, clear and partial visibility under "
          "original light ownership");
  rejects([&] { table.observe(pairs, values); });
  rejects([&] { ProbeVisibility undersized(3, 10, pairs, bytes - 1); });
  rejects([&] { ProbeVisibility invalid(2, 10, pairs, bytes); });
  rejects([&] { ProbeVisibility invalid(3, 9, pairs, bytes); });
  const std::array<VisibilityTask, 2> duplicate{{pairs[0], pairs[0]}};
  rejects([&] { ProbeVisibility invalid(3, 10, duplicate, bytes); });
  ProbeVisibility pending(3, 10, pairs, bytes);
  auto bad = values;
  bad.back() = std::numeric_limits<float>::quiet_NaN();
  rejects([&] { pending.observe(pairs, bad); });
  require(!pending.sample(2, 9) && !pending.sample(0, 5),
          "Failed readback publishes no partial coverage");
  bad.back() = 1.01f;
  rejects([&] { pending.observe(pairs, bad); });
  bad.back() = -1;
  rejects([&] { pending.observe(pairs, bad); });
  const std::array<VisibilityTask, 1> unallocated{{{1, 1, 0}}};
  rejects([&] { pending.observe(unallocated, std::span(values).first(1)); });
  rejects([&] { pending.observe(duplicate, std::span(values).first(2)); });
  require(!pending.sample(2, 9), "Duplicate readback remains unknown");
  rejects([&] { pending.observe(pairs, std::span(values).first(2)); });
  pending.observe(pairs, values);
  require(pending.sample(0, 3) == .75f,
          "Valid retry can publish after rejected readback");
  ProbeVisibility empty(0, 0, {}, sizeof(uint32_t));
  require(empty.offsets().size() == 1 && empty.entries().empty(),
          "Empty table retains a valid offset sentinel");
  std::puts("Sparse probe visibility ownership, overlap, budgets and atomic "
            "readback passed");
}

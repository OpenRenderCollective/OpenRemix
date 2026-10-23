#include "forward/probes/receiver_pairs.h"
#include <algorithm>

namespace remix::forward::probes {
  namespace {
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested()) throw std::runtime_error("Receiver pair planning superseded");
    }
    bool contains(const Capture &capture, Vec3 position) {
      const auto local = projectionPoint(capture.worldToLocal, position);
      const auto &bounds = capture.influence.valid ? capture.influence : capture.bounds;
      for (uint32_t c = 0; c < 3; ++c)
        if (local[c] < bounds.minimum[c] || local[c] > bounds.maximum[c]) return false;
      return true;
    }
  }
  std::vector<ReceiverPair> receiverPairs(std::span<const ReceiverSample> receivers,
      std::span<const Capture> captures, float bias, uint32_t rayBudget,
      uint32_t nearest, std::stop_token cancelled) {
    check(cancelled);
    if (receivers.size() > 8192 || captures.size() > maxProbes || !rayBudget || rayBudget > 32768 ||
        !nearest || nearest > 4 || !std::isfinite(bias) || bias <= 0)
      throw std::invalid_argument("Invalid receiver pair budget or bias");
    for (const auto &capture : captures) validateCapture(capture);
    std::vector<uint32_t> uncovered;
    for (uint32_t i = 0; i < receivers.size(); ++i) {
      check(cancelled);
      for (uint32_t c = 0; c < 3; ++c)
        if (!std::isfinite(receivers[i].position[c]) || !std::isfinite(receivers[i].normal[c]))
          throw std::invalid_argument("Nonfinite receiver pair input");
      if (std::none_of(captures.begin(), captures.end(), [&](const auto &capture) {
            return contains(capture, receivers[i].position);
          })) uncovered.push_back(i);
    }
    if (captures.empty()) return {};
    const uint32_t perReceiver = std::min({nearest, rayBudget, uint32_t(captures.size())});
    const size_t count = std::min(uncovered.size(), size_t(rayBudget / perReceiver));
    std::vector<ReceiverPair> result, candidates;
    result.reserve(count * perReceiver);
    candidates.reserve(captures.size());
    for (size_t i = 0; i < count; ++i) {
      check(cancelled);
      const auto receiver = uncovered[((2 * i + 1) * uncovered.size()) / (2 * count)];
      candidates.clear();
      for (uint32_t capture = 0; capture < captures.size(); ++capture)
        if (const auto ray = receiverRay(receivers[receiver], captures[capture].position, bias))
          candidates.push_back({receiver, capture, *ray});
      std::stable_sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
        return a.ray.distance < b.ray.distance;
      });
      const size_t take = std::min(size_t(perReceiver), candidates.size());
      result.insert(result.end(), candidates.begin(), candidates.begin() + take);
    }
    return result;
  }
} // namespace remix::forward::probes

#include "forward/probes/receiver_visibility.h"
#include <limits>
#include <stdexcept>

namespace p = remix::forward::probes;
using remix::forward::bake::Result;
void require(bool value) { if (!value) throw std::runtime_error("Receiver visibility invariant failed"); }
Result geometry(float distance, bool missed = false, bool back = false) {
  Result value{};
  value.irradiance[0][3] = distance;
  value.statistics = {float(back), float(missed), 1, 0};
  return value;
}
int main() {
  p::ReceiverSample receiver;
  receiver.position = {10, 20, -144}; receiver.normal = {0, 0, 2};
  const auto ray = p::receiverRay(receiver, {10, 20, -140}, .05f);
  require(ray && std::abs(ray->point.position[2] + 143.95f) < 1e-5f &&
              std::abs(ray->distance - 3.95f) < 1e-5f && ray->point.normal[2] == 1);
  require(!p::receiverRay(receiver, {10, 20, -145}, .05f));
  require(!p::receiverRay(receiver, {10, 20, -143.95f}, .05f));
  using V = p::ReceiverVisibility;
  require(p::receiverVisibility(*ray, geometry(2), .01f) == V::Blocked);
  require(p::receiverVisibility(*ray, geometry(2, false, true), .01f) == V::Blocked);
  require(p::receiverVisibility(*ray, geometry(5), .01f) == V::Visible);
  require(p::receiverVisibility(*ray, geometry(5, true), .01f) == V::Visible);
  require(p::receiverVisibility(*ray, geometry(2, true), .01f) == V::Unknown);
  require(p::receiverVisibility(*ray, geometry(ray->distance), .01f) == V::Unknown);
  auto invalid = geometry(5);
  invalid.statistics[2] = 0;
  require(p::receiverVisibility(*ray, invalid, .01f) == V::Unknown);
  require(p::receiverVisibility(*ray, geometry(5, true, true), .01f) == V::Unknown);
  require(p::receiverVisibility(*ray, geometry(std::numeric_limits<float>::infinity()), .01f) == V::Unknown);
  auto scaled = receiver;
  for (auto &coordinate : scaled.position) coordinate *= 40;
  const auto scaledRay = p::receiverRay(scaled, {400, 800, -5600}, 2);
  require(scaledRay && std::abs(scaledRay->distance - ray->distance * 40) < .001f &&
              p::receiverVisibility(*scaledRay, geometry(80), .4f) == V::Blocked &&
              p::receiverVisibility(*scaledRay, geometry(200, true), .4f) == V::Visible);
  receiver.normal = {};
  require(!p::receiverRay(receiver, {10, 20, -140}, .05f));
  bool rejected = false;
  try { p::receiverRay(receiver, {0, 0, 0}, 0); } catch (const std::invalid_argument &) { rejected = true; }
  require(rejected);
}

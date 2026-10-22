#include "forward/probes/capture_work.h"
#include "scene/hash_stream.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace p = remix::forward::probes;
namespace {
  void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
  template <class F> void invalid(F call) {
    try { call(); } catch (const std::invalid_argument &) { return; }
    require(false, "Invalid capture work accepted");
  }
  p::Capture capture(uint32_t resolution = 8) {
    p::Capture result;
    result.bounds.include({-1,-1,-1});
    result.bounds.include({1,1,1});
    result.resolution = resolution;
    result.radiance.resize(6u * resolution * resolution);
    return result;
  }
}
void testCaptureWork() {
  auto base = capture(), alias = base, other = base;
  alias.bounds.include({-2,-2,-2});
  alias.bounds.include({2,2,2});
  alias.influence.include({-3,-3,-3});
  alias.influence.include({3,3,3});
  alias.priority = 2;
  alias.worldToLocal = {{{0,-1,0,0}, {1,0,0,0}, {0,0,1,0}}};
  other.position[0] = .25f;
  auto appended = p::rays(base);
  const auto prefix = appended;
  p::appendRays(other, appended);
  const auto suffix = p::rays(other);
  require(appended.size() == prefix.size() + suffix.size() &&
              std::memcmp(appended.data(), prefix.data(), prefix.size() * sizeof(prefix[0])) == 0 &&
              std::memcmp(appended.data() + prefix.size(), suffix.data(), suffix.size() * sizeof(suffix[0])) == 0,
          "Direct ray append preserves prior captures and the following capture's ray bits");
  std::stop_source stopped;
  stopped.request_stop();
  bool appendCancelled = false;
  try { p::appendRays(base, appended, stopped.get_token()); }
  catch (const std::runtime_error &) { appendCancelled = true; }
  require(appendCancelled && appended.size() == prefix.size() + suffix.size(),
          "Cancelled direct ray append leaves the existing work buffer intact");
  std::vector<p::Capture> captures{base, alias, other, alias, other};
  p::CaptureWork work(captures);
  require(p::CaptureWork::inputHash(captures) ==
              (remix::scene::fastHash(work.rays().data(), work.rays().size_bytes()) | 1),
          "Bounded cached provenance matches complete deduplicated capture ray bytes");
  for (uint64_t seed : {0ull, 1ull, UINT64_MAX}) {
    std::array<uint8_t, 513> bytes;
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(i * 37);
    for (size_t size = 0; size <= bytes.size(); ++size)
      for (size_t chunk : {1u, 3u, 8u, 31u, 32u, 33u, 127u}) {
        remix::scene::HashStream stream(seed);
        stream.append(nullptr, 0);
        for (size_t at = 0; at < size; at += chunk)
          stream.append(bytes.data() + at, std::min(chunk, size - at));
        require(stream.digest() == remix::scene::fastHash(bytes.data(), size, seed),
                "Streaming hash preserves one-shot bytes at every tail and block boundary");
      }
  }
  require(work.rays().size() == 2u * 384 &&
              work.destination(0) == std::pair<uint32_t,uint32_t>{0,0} &&
              work.destination(384) == std::pair<uint32_t,uint32_t>{2,0} &&
              work.destination(767) == std::pair<uint32_t,uint32_t>{2,383},
          "Nonadjacent aliases changed unique-ray ordering or readback ownership");
  for (uint32_t ray = 0; ray < work.rays().size(); ++ray) {
    const auto [owner, texel] = work.destination(ray);
    captures[owner].radiance[texel] = {float(texel % 7), float(owner), float(texel % 3), 2};
  }
  work.complete(captures);
  require(captures[0].radiance == captures[1].radiance && captures[0].radiance == captures[3].radiance &&
              captures[2].radiance == captures[4].radiance,
          "Completed aliases did not receive the owning capture's texels");
  const auto filtered = p::filter(captures, 32);
  const auto stride = filtered.header.parameters[3];
  for (size_t i = 0; i < captures.size(); ++i) {
    const auto independent = p::filter(std::span(captures).subspan(i, 1), 32);
    require(std::memcmp(&filtered.header.probes[i], &independent.header.probes[0], sizeof(p::Record)) == 0 &&
                std::memcmp(filtered.texels.data() + i * stride, independent.texels.data(),
                            stride * sizeof(remix::forward::Vec4)) == 0,
            "Shared filtering changed independent projection records or any mip texel bits");
  }
  p::Capture zeros[] = {base, base};
  zeros[1].position[0] = -0.f;
  require(!p::sameCaptureRays(zeros[0], zeros[1]) && p::CaptureWork(zeros).rays().size() == 768,
          "Signed-zero origins with different shader seeds must not share lighting queries");
  zeros[1].position[0] = 0.f;
  zeros[1].radiance[0][0] = -0.f;
  const auto signedData = p::filter(zeros, 32);
  require(std::memcmp(&signedData.texels[0][0], &signedData.texels[stride][0], sizeof(float)) != 0,
          "Value-equal signed-zero radiance must retain distinct payload bits");
  require(p::CaptureWork(std::vector<p::Capture>(64, base)).rays().size() == 384,
          "Maximum same-origin record set increased ray work");
  invalid([&] { p::CaptureWork excessive(std::vector<p::Capture>(65, base)); });
  auto incomplete = base;
  incomplete.radiance.pop_back();
  invalid([&] { p::CaptureWork bad(std::span(&incomplete, 1)); });
  p::Capture mixed[] = {base, capture(16)};
  invalid([&] { p::CaptureWork bad(mixed); });
  captures[0].position[0] = .5f;
  invalid([&] { work.complete(captures); });
  p::CaptureWork empty;
  std::vector<p::Capture> none;
  empty.complete(none);
  require(empty.rays().empty(), "Empty capture set generated work");
  bool outside = false;
  try { empty.destination(0); }
  catch (const std::out_of_range &) { outside = true; }
  require(outside, "Empty capture work accepted a readback destination");
  std::stop_source stop;
  stop.request_stop();
  bool cancelled = false;
  try { p::CaptureWork stopped(std::span(&base, 1), stop.get_token()); }
  catch (const std::runtime_error &) { cancelled = true; }
  require(cancelled, "Capture planning ignored cancellation");
  cancelled = false;
  try { work.complete(captures, stop.get_token()); }
  catch (const std::runtime_error &) { cancelled = true; }
  require(cancelled, "Capture completion ignored cancellation");
}

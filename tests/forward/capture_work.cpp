#include "forward/probes/capture_work.h"
#include "forward/probes/local_capture_work.h"
#include <limits>
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
void testLocalCaptureWork() {
  auto base = capture(), alias = base;
  alias.bounds.minimum[0] = -2;
  alias.worldToLocal = {{{0,-1,0,0}, {1,0,0,0}, {0,0,1,0}}};
  p::Header published;
  published.parameters = {1,8,4,510}; published.probes[0] = p::projectionRecord(base);
  const auto baseline = p::automaticCoverageBaseline(published);
  remix::scene::BakeScene scene;
  scene.sceneId = 100; scene.revision = 1; scene.ready = true;
  p::ReceiverCoveragePlan plan;
  plan.context = p::coveragePlanContext(scene, baseline);
  plan.candidateRecords = {p::projectionRecord(base), p::projectionRecord(alias)};
  plan.owners = {{0,0},{0,1}}; plan.errors = {0,0};
  plan.replacements = {{1,0,1},{0,UINT32_MAX,2}};
  p::LocalCaptureWork work(scene, baseline, plan, 8);
  require(work.rays().size() == 384, "Selected same-origin local boxes share one six-face capture");
  bool pending = false;
  try { (void)work.finish(32); } catch (const std::logic_error &) { pending = true; }
  require(pending, "Local image cannot filter before every readback retires");
  std::vector<remix::forward::bake::Result> results(384);
  for (uint32_t i = 0; i < results.size(); ++i) results[i].irradiance[0] = {float(i % 11), .5f, 2, 3};
  auto broken = results;
  broken.back().irradiance[0][0] = std::numeric_limits<float>::quiet_NaN();
  require(!work.observe(0, broken), "Invalid local readback is rejected atomically");
  require(work.observe(192, std::span(results).subspan(192)), "Out-of-order local tail retires");
  require(!work.observe(192, std::span(results).subspan(192)), "Duplicate local readback is refused");
  require(work.observe(0, std::span(results).first(192)) && work.complete(), "Every unique local readback completes");
  const auto image = work.finish(32);
  for (uint32_t i = 0; i < results.size(); ++i) base.radiance[i] = alias.radiance[i] = results[i].irradiance[0];
  const p::Capture independent[]{alias,base};
  require(p::serialize(image.data) == p::serialize(p::filter(independent, 32)),
          "Prepared local image matches independent filtering with distinct projection records");
  const auto composed = p::buildCoverageImagePlan(scene, baseline, plan, published, {}, image.selection);
  require(composed.header.parameters[0] == 2 && composed.copies[0].source == p::CoverageImageSource::Local &&
          composed.copies[0].layer == 0 && composed.copies[1].layer == 1,
          "Selected candidate IDs map to the correct prepared image layers");
  require(work.complete() && work.rays().empty() && !work.observe(0, {}),
          "Finished local capture releases query work and rejects further readbacks");
  auto changed = scene; ++changed.reflectionRevision;
  invalid([&] { p::LocalCaptureWork stale(changed, baseline, plan, 8); });
  invalid([&] { p::LocalCaptureWork wrongSize(scene, baseline, plan, 16); });
  plan.replacements[1].candidate = 1;
  invalid([&] { p::LocalCaptureWork duplicate(scene, baseline, plan, 8); });
}
void testCaptureWork() {
  testLocalCaptureWork();
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

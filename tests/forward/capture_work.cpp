#include "forward/probes/capture_work.h"
#include "forward/probes/automatic_capture.h"
#include "forward/probes/local_capture_work.h"
#include <limits>
#include "scene/hash_stream.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

// CPU preparation tests omit optional filesystem diagnostics; native/GPU tests
// still link the production candidate reporter.
namespace remix::forward::probes {
  void dumpCandidates(const scene::BakeScene &, std::span<const FittedRegion>,
                      std::span<const Capture>, std::stop_token) noexcept {}
}
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
  {
    std::shared_ptr<const p::CapturePayload> retained;
    {
      p::AutomaticCapture owner;
      owner.captures.push_back(capture());
      owner.filtered.texels.resize(16, {1,2,3,4});
      const auto *raw = owner.captures[0].radiance.data();
      const auto *filtered = owner.filtered.texels.data();
      retained = owner.retainPayload();
      require(owner.captures.empty() && owner.filtered.texels.empty() &&
                  owner.captureView()[0].radiance.data() == raw && owner.filteredView().texels.data() == filtered,
              "Payload retention copied arrays or kept duplicate preparing storage");
      bool immutable = false;
      try { owner.fit(remix::scene::BakeScene{}, 0, .5f, {}); } catch (const std::logic_error &) { immutable = true; }
      require(immutable && !owner.observe(p::AutomaticCapture::Step::Capture, 0, {}),
              "Completed reflection payload accepted mutation/readback");
    }
    require(retained->captures.size() == 1 && retained->data.texels.size() == 16,
            "Retained payload depended on capture scheduler lifetime");
  }
  {
    remix::scene::BakeScene source;
    source.hasBounds = true;
    source.minimum = {-500,-1,-1}; source.maximum = {500,1,1};
    const auto candidates = p::geometricSeedCandidates(source, 64);
    require(candidates.points.size() == 64 && candidates.points.front().position[0] < -480 &&
            candidates.points.back().position[0] > 480, "Geometry seeds lost bounded corridor coverage");
    invalid([&] { p::geometricSeedCandidates(source, 0); });
    invalid([&] { p::geometricSeedCandidates(source, 4097); });
    auto malformed = source; malformed.minimum[0] = 1000;
    invalid([&] { p::geometricSeedCandidates(malformed, 64); });
    std::stop_source stop; stop.request_stop();
    bool cancelled = false;
    try { p::geometricSeedCandidates(source, 64, stop.get_token()); }
    catch (const std::runtime_error &) { cancelled = true; }
    require(cancelled, "Cancelled geometry planning continued");
    p::AutomaticCapture owner;
    const p::AutomaticCapture::Placement placement{4, 4, 8, .5f};
    require(owner.prepareNext(source, placement, {}) == p::AutomaticCapture::Step::PlanSites,
            "Provider did not plan its first CPU stage");
    bool pending = false;
    try { owner.prepareNext(source, placement, {}); }
    catch (const std::logic_error &) { pending = true; }
    require(pending && owner.siteReceived == 0, "Preparation skipped pending GPU admission");
    require(owner.nextStep() == p::AutomaticCapture::Step::ValidateSites, "Standalone seeds bypassed admission");
    std::vector<remix::forward::bake::Result> results(owner.reflectionSites.size());
    for (auto &result : results) result.statistics = {1,0,float(p::AutomaticCapture::siteSamples),0};
    owner.siteNext = uint32_t(results.size());
    require(!owner.observe({p::AutomaticCapture::Step::ValidateSites, 0, uint32_t(results.size() + 1)}, results) &&
                owner.siteReceived == 0, "Truncated fenced batch advanced capture ownership");
    require(owner.observe(p::AutomaticCapture::Step::ValidateSites, 0, results), "Standalone admission readback refused");
    require(owner.prepareNext(source, placement, {}) == p::AutomaticCapture::Step::Fit,
            "Provider did not fit after retired admission");
    require(owner.seeds.points.empty() && owner.candidateRegions.empty(), "Rejected geometry seeds reached fitting");
    require(owner.prepareNext(source, placement, {}) == p::AutomaticCapture::Step::Select &&
                owner.captureView().empty() && owner.nextStep() == p::AutomaticCapture::Step::Complete,
            "Rejected candidates generated captures or failed to complete preparation");
    pending = false;
    try { owner.prepareNext(source, placement, {}); }
    catch (const std::logic_error &) { pending = true; }
    require(pending, "Completed placement ran another CPU stage");
    auto room = source;
    room.minimum = {-4,-3,-2}; room.maximum = {4,3,2};
    auto partition = std::make_shared<remix::scene::VisibilityPartition>();
    partition->closed = true; partition->clusterCount = 1; partition->leafClusters = {-1,0};
    partition->nodes.resize(6);
    for (uint32_t i = 0; i < 6; ++i) {
      auto &node = partition->nodes[i];
      const uint32_t axis = i / 2;
      node.plane[axis] = (i & 1u) ? -1.f : 1.f;
      node.plane[3] = (i & 1u) ? -room.maximum[axis] : room.minimum[axis];
      node.children[0] = i == 5 ? -2 : int32_t(i + 1);
      node.children[1] = -1;
    }
    room.partition = partition;
    p::AutomaticCapture enclosed;
    enclosed.planSites(room, 4, {});
    std::vector<remix::forward::bake::Result> clear(enclosed.reflectionSites.size());
    for (auto &result : clear) result.statistics = {0,0,float(p::AutomaticCapture::siteSamples),0};
    enclosed.siteNext = uint32_t(clear.size());
    require(enclosed.observe(p::AutomaticCapture::Step::ValidateSites, 0, clear), "Room admission refused");
    enclosed.fit(room, 4, 0, {});
    require(enclosed.nextStep() == p::AutomaticCapture::Step::CheckGeometry &&
                enclosed.projectionAudit && !enclosed.projectionAudit->rays().empty(),
            "Standalone room planning skipped projection geometry assessment");
  }
  {
    p::AutomaticCapture capture;
    capture.reflectionSites.resize(2);
    capture.usableReflectionSites.resize(2);
    capture.siteNext = 2;
    remix::forward::bake::Result valid{};
    valid.statistics = {.1f, 0, float(p::AutomaticCapture::siteSamples), 0};
    auto bad = valid;
    bad.irradiance[1][2] = std::numeric_limits<float>::quiet_NaN();
    const std::array batch{valid, bad};
    require(!capture.observe(p::AutomaticCapture::Step::ValidateSites, 0, batch),
            "Invalid site batch accepted");
    require(!capture.siteReceived && !capture.usableReflectionSites[0], "Partial site batch published");
    require(capture.observe(p::AutomaticCapture::Step::ValidateSites, 1, {&valid, 1}),
            "Out-of-order site readback rejected");
    require(capture.siteReceived == 1 && capture.usableReflectionSites[1], "Site completion not owned");
    require(!capture.observe(p::AutomaticCapture::Step::Capture, 0, {&valid, 1}), "Unsubmitted capture accepted");
    require(!capture.observe(p::AutomaticCapture::Step::Fit, 0, {}), "Non-query phase accepted readback");
  }
  {
    p::AutomaticCapture owner;
    owner.captures.push_back(capture());
    owner.work = p::CaptureWork(owner.captures);
    owner.captureNext = uint32_t(owner.work.rays().size());
    remix::forward::bake::Result result{};
    result.irradiance[0] = {-1, 70000, 2, 4};
    require(owner.observe(p::AutomaticCapture::Step::Capture, 0, {&result, 1}), "Capture readback rejected");
    require(owner.captureReceived == 1 && owner.captures[0].radiance[0][0] == 0 && owner.captures[0].radiance[0][1] == 65000 &&
            owner.captures[0].radiance[0][2] == 2 && owner.captures[0].radiance[0][3] == 4,
            "Capture readback changed radiance clamp or completion");
  }
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
  const auto signature = p::CaptureWork::planHash(captures);
  const p::Capture unique[]{base, other};
  require(signature == p::CaptureWork::planHash(unique),
          "Procedural capture signature follows unique-ray owner order, not projection aliases");
  const p::Capture reordered[]{other, base};
  require(signature != p::CaptureWork::planHash(reordered), "Procedural signature preserves ray order");
  auto resized = capture(16);
  require(p::CaptureWork::planHash(std::span(&base, 1)) != p::CaptureWork::planHash(std::span(&resized, 1)),
          "Procedural signature includes capture resolution");
  auto signedOrigin = base;
  signedOrigin.position[0] = -0.f;
  require(p::CaptureWork::planHash(std::span(&base, 1)) != p::CaptureWork::planHash(std::span(&signedOrigin, 1)),
          "Procedural signature retains signed-zero seed inputs");
  bool planCancelled = false;
  try { (void)p::CaptureWork::planHash(captures, stopped.get_token()); }
  catch (const std::runtime_error &) { planCancelled = true; }
  require(planCancelled && p::CaptureWork::planHash({}) == 0,
          "Procedural signature handles cancellation and empty capture sets");
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
  const auto diagnostic = p::captureContent(captures, filtered, 123);
  const auto joined = p::serialize(filtered);
  uint64_t legacyRadiance = 1469598103934665603ull;
  for (const auto &capture : captures)
    for (const auto &color : capture.radiance)
      legacyRadiance = remix::scene::hashBytes(color.data(), 3 * sizeof(float), legacyRadiance);
  require(diagnostic.filtered == (remix::scene::fastHash(joined.data(), joined.size()) | 1) &&
              diagnostic.radiance == (legacyRadiance | 1) && diagnostic.inputs == 123,
          "Worker reflection diagnostics changed legacy image/RGB hash values");
  auto large = filtered;
  large.texels.resize((1u << 20) / sizeof(remix::forward::Vec4) + 3, {1,2,3,4});
  const auto largeBytes = p::serialize(large);
  require(p::captureContent({}, large, 0).filtered ==
              (remix::scene::fastHash(largeBytes.data(), largeBytes.size()) | 1),
          "Reflection image fingerprint changed across streaming chunk boundary");
  std::stop_source stoppedContent; stoppedContent.request_stop();
  bool contentCancelled = false;
  try { (void)p::captureContent(captures, filtered, 123, stoppedContent.get_token()); }
  catch (const std::runtime_error &) { contentCancelled = true; }
  require(contentCancelled, "Reflection content hashing ignored cancellation");
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

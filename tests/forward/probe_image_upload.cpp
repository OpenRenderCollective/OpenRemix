#include "forward/probes/image_asset.h"
#include "forward/probes/coverage_image_plan.h"
#include "forward/probes/selection_upload.h"
#include "gpu_fixture.h"
#include <algorithm>
#include <bit>
#include <stdexcept>

using namespace remix::forward;
using namespace remix::forward::test;

void uploadCase(uint32_t count, uint32_t size, bool composition = false, bool selection = false) {
  Fixture fixture(false); // No optional cube-array or descriptor-indexing features.
  probes::Data source;
  const uint32_t levels = std::bit_width(size), stride = probes::levelOffset(size, levels);
  source.header.parameters = {count, size, levels, stride};
  source.texels.resize(size_t(count) * stride);
  for (size_t i = 0; i < source.texels.size(); ++i)
    source.texels[i] = {float(i % 1000) / 16, float(i % 31), .001f, 1000000};
  probes::ImageAsset asset(fixture.physical, fixture.device, source);
  std::unique_ptr<probes::ImageAsset> second, combined;
  if (composition) {
    require(count > 0 && count <= 32, "Composition fixture fits both sources within 64 layers");
    auto other = source;
    if (selection) other.header.probes[0].position[0] = 42;
    for (size_t i = 0; i < other.texels.size(); ++i)
      other.texels[i] = {32 + float(i % 1000) / 32, 8, .5f, 1};
    second = std::make_unique<probes::ImageAsset>(fixture.physical, fixture.device, other);
    probes::Data expected;
    expected.header = source.header;
    expected.header.parameters[0] = 2 * count;
    probes::Composition plan{.header = expected.header, .automatic = count};
    for (uint32_t layer = 0; layer < count; ++layer) {
      // Interleave reversed authored layers with forward automatic layers.
      // Distinct HDR patterns expose source/layer/mip addressing mistakes.
      const uint32_t reverse = count - layer - 1;
      plan.layers.push_back({false, reverse});
      plan.layers.push_back({true, layer});
      expected.texels.insert(expected.texels.end(), source.texels.begin() + size_t(reverse)*stride,
                             source.texels.begin() + size_t(reverse+1)*stride);
      expected.texels.insert(expected.texels.end(), other.texels.begin() + size_t(layer)*stride,
                             other.texels.begin() + size_t(layer+1)*stride);
    }
    source = std::move(expected);
    count *= 2;
    if (selection) {
      probes::CoverageImagePlan selected;
      selected.header = plan.header;
      selected.sources = {asset.header(), second->header()};
      for (const auto &copy : plan.layers)
        selected.copies.push_back({copy.automatic ? probes::CoverageImageSource::Local : probes::CoverageImageSource::Baseline, copy.layer});
      combined = std::make_unique<probes::ImageAsset>(fixture.physical, fixture.device, selected);
    } else combined = std::make_unique<probes::ImageAsset>(fixture.physical, fixture.device, plan);
  }
  auto &output = combined ? *combined : asset;
  auto packed = probes::prepareImage(source);
  if (!count) packed.pixels.resize(4);
  const auto bytes = packed.pixels.size() * sizeof(uint16_t);
  Buffer readback;
  readback.create(fixture.physical, fixture.device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer::Memory::Readback);
  const auto &image = output.image();
  require(asset.metadata().range == sizeof(probes::Header), "Projection descriptor excludes radiance payload");
  require(output.radiance().sampler && output.radiance().imageView == image.view,
          "Image asset owns a complete combined sampler descriptor");
  bool earlyRelease = false;
  try { asset.releaseUpload(); } catch (const std::logic_error &) { earlyRelease = true; }
  require(earlyRelease, "Staging cannot be released before the upload is recorded");
  std::vector<VkBufferImageCopy> copies;
  for (uint32_t level = 0; level < levels; ++level)
    copies.push_back({.bufferOffset = packed.levels[level].byteOffset,
                      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, count},
                      .imageExtent = {size >> level, 6*(size >> level), 1}});
  if (!count) copies.push_back({.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageExtent = {1, 1, 1}});
  fixture.begin();
  asset.recordUpload(fixture.command);
  if (composition) {
    second->recordUpload(fixture.command);
    if (selection) {
      bool wrongSource = false;
      try { combined->recordSelection(fixture.command, *second, asset); }
      catch (const std::invalid_argument &) { wrongSource = true; }
      // The image headers need distinct metadata to detect swapped source owners.
      require(wrongSource, "Selection rejects changed source metadata before recording");
      combined->recordSelection(fixture.command, asset, *second);
      bool repeated = false;
      try { combined->recordSelection(fixture.command, asset, *second); }
      catch (const std::logic_error &) { repeated = true; }
      require(repeated, "Selection records exactly once");
    } else combined->recordCompose(fixture.command, asset, *second);
  }
  bool duplicate = false;
  try { asset.recordUpload(fixture.command); } catch (const std::logic_error &) { duplicate = true; }
  require(duplicate, "Image asset refuses recording its one-time upload twice");
  VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_READ_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image.handle,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, std::max(levels, 1u), 0, std::max(count, 1u)}};
  vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &barrier);
  vkCmdCopyImageToBuffer(fixture.command, image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         readback.handle, uint32_t(copies.size()), copies.data());
  const VkMemoryBarrier host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
  vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       0, 1, &host, 0, nullptr, 0, nullptr);
  fixture.finish();
  const auto beforeRelease = asset.bufferBytes();
  asset.releaseUpload();
  if (composition) {
    second->releaseUpload();
    combined->releaseUpload();
  }
  require(asset.bufferBytes() < beforeRelease && asset.metadata().buffer,
          "Completed upload releases staging while retaining sampled metadata");
  std::vector<uint16_t> actual(packed.pixels.size());
  readback.read(actual.data(), bytes);
  require(actual == packed.pixels, "Every reflection layer, face and mip survives GPU image upload/readback");
  require(validationErrors == 0, "Reflection image upload is Vulkan validation clean");
  std::printf("Reflection image: %u layers, %u-square faces, %u mips, %zu bytes passed\n",
              count, size, levels, bytes);
}
void selectionLifetimeCase(uint32_t stale) {
  Fixture fixture(false);
  probes::Capture capture;
  capture.bounds.include({-1,-1,-1}); capture.bounds.include({1,1,1}); capture.resolution = 8;
  probes::Data data;
  data.header.parameters = {1,8,4,510};
  data.header.probes[0] = probes::projectionRecord(capture);
  data.texels.resize(510, {1,2,3,1});
  auto baselineImage = std::make_shared<probes::ImageAsset>(fixture.physical, fixture.device, data);
  auto localImage = std::make_shared<probes::ImageAsset>(fixture.physical, fixture.device, data);
  fixture.begin();
  baselineImage->recordUpload(fixture.command); localImage->recordUpload(fixture.command);
  fixture.finish();
  baselineImage->releaseUpload(); localImage->releaseUpload();
  auto baseline = probes::automaticCoverageBaseline(baselineImage->header());
  baseline.composed = true; baseline.authoredSceneId = 100; baseline.authoredRevision = 1;
  remix::scene::BakeScene scene;
  scene.sceneId = 100; scene.revision = 1; scene.ready = true;
  auto authoredOnly = baseline; authoredOnly.automaticAllowed = false;
  const auto noAutomatic = probes::planReceiverCoverage(scene, authoredOnly, {}, probes::ReceiverAudit({}, .1f));
  require(noAutomatic.replacements.empty() && !noAutomatic.disabledBaseline &&
          probes::matchesCoverageContext(noAutomatic.context, scene, authoredOnly),
          "Authored-only policy preserves active volumes without automatic work");
  probes::ReceiverFits retainedFits;
  retainedFits.attempts.resize(1);
  retainedFits.attempts[0].site = 17;
  probes::ReceiverPair pair;
  pair.ray.distance = 1; pair.ray.point.normal[0] = 1;
  probes::ReceiverAudit audit({pair}, .01f);
  bool pendingRefused = false;
  try { (void)probes::retainReceiverCoverage(scene, retainedFits, audit); }
  catch (const std::invalid_argument &) { pendingRefused = true; }
  require(pendingRefused, "Pending visibility cannot become retained evidence");
  remix::forward::bake::Result hit;
  hit.irradiance[0][3] = 2; hit.statistics = {0,1,1,0};
  require(audit.observe(0, std::span(&hit, 1)), "Completed visibility evidence is observed");
  const auto evidence = probes::retainReceiverCoverage(scene, retainedFits, audit);
  retainedFits.attempts[0].site = 99;
  require(evidence.fits.attempts[0].site == 17 && evidence.observations.size() == 1 &&
          evidence.observations[0].visibility == probes::ReceiverVisibility::Visible,
          "Retained evidence owns fit metadata and classified visibility without query rays");
  require(probes::planReceiverCoverage(scene, authoredOnly, evidence).replacements.empty(),
          "Retained evidence replans against current fallback policy");
  auto changedSource = scene; ++changedSource.chartRevision;
  bool staleEvidence = false;
  try { (void)probes::planReceiverCoverage(changedSource, authoredOnly, evidence); }
  catch (const std::invalid_argument &) { staleEvidence = true; }
  require(staleEvidence, "Changed frozen scene rejects retained receiver evidence");
  probes::ReceiverCoveragePlan selection;
  selection.context = probes::coveragePlanContext(scene, baseline);
  selection.owners = {{0,0}}; selection.errors = {0}; selection.candidateRecords = {localImage->header().probes[0]};
  selection.replacements = {{0,0,1}};
  probes::CoverageReadyImage local{localImage->header(), selection.context, {{0,0}}};
  const auto plan = probes::buildCoverageImagePlan(scene, baseline, selection, baselineImage->header(), {}, local);
  auto upload = std::make_unique<probes::SelectionUpload>(fixture.physical, fixture.device, plan, baselineImage, localImage);
  std::weak_ptr<const probes::ImageAsset> oldBaseline = baselineImage, oldLocal = localImage;
  if (stale >= 4) {
    probes::SelectionFrames frames;
    frames.offer(std::move(upload));
    baselineImage.reset(); localImage.reset();
    frames.selectFrame(0);
    fixture.begin(); frames.record(fixture.command, scene, baseline); fixture.finish();
    require(!frames.active(), "Copy is not published before its frame fence retires");
    bool reusedSlot = false;
    try { frames.record(VK_NULL_HANDLE, scene, baseline); } catch (const std::logic_error &) { reusedSlot = true; }
    require(reusedSlot, "A submitted frame slot cannot record again before fence retirement");
    if (stale == 5)
      frames.offer(std::make_unique<probes::SelectionUpload>(fixture.physical, fixture.device, plan,
                                                           oldBaseline.lock(), oldLocal.lock()));
    frames.selectFrame(0);
    if (stale == 6) ++scene.revision;
    if (stale == 7) frames.invalidate();
    fixture.begin(); frames.record(fixture.command, scene, baseline); fixture.finish();
    if (stale == 5) {
      require(!frames.active(), "A newer offer prevents an older completed copy from publishing");
      require(!oldBaseline.expired(), "Superseding copy retains its sources through its own fence");
      frames.selectFrame(0);
      fixture.begin(); frames.record(fixture.command, scene, baseline); fixture.finish();
    }
    require(bool(frames.active()) == (stale != 6 && stale != 7), "Frame activation checks current context");
    require(oldBaseline.expired() && oldLocal.expired(), "Frame retirement releases copy sources");
    if (frames.active()) {
      const auto memory = frames.memory();
      require(memory[0] == frames.active()->image->bufferBytes() && memory[1] == frames.active()->image->imageBytes(),
              "Active and sampled reflection output memory is counted once");
      std::weak_ptr<const probes::ImageAsset> sampled = frames.active()->image;
      ++scene.revision;
      frames.selectFrame(1);
      fixture.begin(); frames.record(fixture.command, scene, baseline); fixture.finish();
      require(!frames.active() && !sampled.expired(), "Map change invalidates active output but retains sampled frames");
      frames.selectFrame(0);
      require(sampled.expired(), "Sampling owner retires only with its own frame slot");
      require(frames.memory() == std::array<uint64_t, 2>{}, "Retired reflection output releases memory accounting");
    }
    require(validationErrors == 0, "Fenced frame publication is validation clean");
    return;
  }
  baselineImage.reset(); localImage.reset();
  require(!oldBaseline.expired() && !oldLocal.expired(), "Selection retains both source image owners");
  if (stale == 1) {
    ++scene.revision;
    require(!upload->record(VK_NULL_HANDLE, scene, baseline), "Stale selection records no GPU commands");
    require(oldBaseline.expired() && oldLocal.expired(), "Unsubmitted cancellation releases source owners");
    return;
  }
  fixture.begin();
  require(upload->record(fixture.command, scene, baseline), "Current selection records its copy");
  require(!oldBaseline.expired() && !oldLocal.expired(), "Submitted sources survive until their fence");
  fixture.finish();
  if (stale == 2) ++scene.revision;
  if (stale == 3) ++baseline.authoredRevision;
  const auto published = upload->retire(scene, baseline);
  require(bool(published) == !stale, "Fence retirement rejects stale scene or authored revision");
  require(oldBaseline.expired() && oldLocal.expired(), "Completed copy releases retained source images");
  if (published) require(published->image->header().parameters == plan.header.parameters &&
                         published->identities == plan.identities, "Publication retains image and aligned identities");
  bool repeated = false;
  try { (void)upload->retire(scene, baseline); } catch (const std::logic_error &) { repeated = true; }
  require(repeated, "A copy cannot retire twice");
  require(validationErrors == 0, "Selection ownership and retirement are validation clean");
}
int main() try {
  for (uint32_t stale = 0; stale < 8; ++stale) selectionLifetimeCase(stale);
  uploadCase(0, 0);
  uploadCase(64, 8);
  uploadCase(1, 256);
  uploadCase(1, 512);
  uploadCase(1, 8, true);
  uploadCase(32, 8, true);
  uploadCase(1, 8, true, true);
  uploadCase(32, 8, true, true);
} catch (VkResult error) {
  std::fprintf(stderr, "Reflection image upload failed: %d\n", int(error));
  return 1;
}

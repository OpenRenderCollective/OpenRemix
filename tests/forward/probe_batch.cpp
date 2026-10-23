#include "forward/bake/grid.h"
#include "forward/bake/controller.h"
#include "forward/bake/probe_visibility.h"
#include "forward/probe_batch/batch.h"
#include "gpu_fixture.h"
#include "probe_batch_query.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <exception>
#include <thread>
#include <vector>

using namespace remix::forward;
using namespace remix::forward::test;
namespace {
  struct Source {
    std::shared_ptr<bake::Volume> volume;
    bake::ProbeVisibility masks;
    uint32_t last;
  };
  Source source(Fixture &fixture, bool sorted, bool directional, uint32_t seed = 0) {
    const uint32_t lights = sorted ? 2097153u : 66u;
    std::vector<bake::VisibilityTask> pairs;
    std::vector<bake::VisibilityResult> values;
    for (uint32_t probe = 0; probe < 8; ++probe)
      for (uint32_t light : {0u, 1u, 64u, lights - 1}) {
        if (probe == 0 && light == 1) continue;
        pairs.push_back({probe, light, 0});
        bake::VisibilityResult value;
        value.visibility = float((probe + light % 5 + seed) % 7) / 6;
        value.irradiance[0] = 1;
        value.irradiance[2] = -.25f;
        values.push_back(value);
      }
    bake::ProbeVisibility masks(8, lights, pairs, 8192, directional);
    if (directional) masks.observe(pairs, values);
    else {
      std::vector<float> scalars;
      for (const auto &value : values) scalars.push_back(value.visibility);
      masks.observe(pairs, scalars);
    }
    const auto words = masks.words();
    require((directional ? words[9] : words[1]) == (sorted ? 2u : 3u),
            "Batch fixture exercises both sparse GPU encodings and the directional wrapper");
    auto volume = std::make_shared<bake::Volume>();
    auto &header = volume->irradianceHeader;
    header.originSpacing = {0, 0, 0, 1};
    header.dimensions = {2, 2, 2, 8};
    volume->data = std::make_unique<Buffer>();
    // Real published buffers carry lattice/depth/style data before local masks.
    // Deliberate aligned gaps catch accidental assumptions about that offset.
    const size_t offset = sizeof(header) + 8 * sizeof(bake::Result) +
                            (directional ? 64 : 0) + (sorted ? 80 : 0) + seed * 16;
    header.settings[3] = bake::publicationSettings((offset - sizeof(header)) / 16, false);
    volume->data->create(fixture.physical, fixture.device, offset + words.size() * 4,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    volume->data->write(&header, sizeof(header));
    std::array<bake::Result, 8> probes{};
    volume->data->write(probes.data(), sizeof(probes), sizeof(header));
    volume->data->write(words.data(), words.size() * 4, offset);
    return {std::move(volume), std::move(masks), lights - 1};
  }
  void readback(Fixture &fixture, const probe_batch::Field &field, const bake::ProbeVisibility &masks) {
    Buffer host;
    const auto descriptor = field.descriptor();
    host.create(fixture.physical, fixture.device, descriptor.range,
                  VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer::Memory::Readback);
    const VkMemoryBarrier copyReady{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &copyReady, 0, nullptr, 0, nullptr);
    const VkBufferCopy copy{0, 0, descriptor.range};
    vkCmdCopyBuffer(fixture.command, descriptor.buffer, host.handle, 1, &copy);
    const VkMemoryBarrier hostReady{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hostReady, 0, nullptr, 0, nullptr);
    fixture.finish();
    std::vector<uint32_t> words(size_t(descriptor.range / 4));
    host.read(words.data(), words.size() * 4);
    require(words[0] == field.probes() && words[1] == field.lights().size() &&
                words[2] == field.groups() && words[3] == 0x56504252u,
            "GPU batch header describes exact immutable ownership");
    for (uint32_t probe = 0; probe < field.probes(); ++probe)
      for (uint32_t slot = 0; slot < field.groups() * 4; ++slot) {
        const auto captured = slot < field.lights().size() ? field.lights()[slot] : 0;
        const auto expected = captured ? masks.sample(probe, captured - 1) : std::optional<float>{};
        const auto actual = words[4 + probe * field.groups() * 4 + slot];
        require(actual == std::bit_cast<uint32_t>(expected.value_or(-1)),
                "Vector gather preserves exact floats, holes, black masks, duplicate IDs and padding");
      }
  }
}
int main() try {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  Fixture fixture(false, true);
  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties(fixture.physical, &properties);
  remix::pipeline::Service pipelines(fixture.device, properties, false);
  uint32_t preparation = 0;
  auto prepare = [&](probe_batch::Cache &cache) {
    auto job = pipelines.prepare("probe-batch-" + std::to_string(preparation++), [&] { cache.prepare(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!job->ready() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(job->ready(), "Batch pipeline preparation completed outside frame recording");
    job->check();
  };
  for (bool sorted : {false, true}) for (bool directional : {false, true}) {
    auto input = source(fixture, sorted, directional);
    const std::array<uint32_t, 7> lights{input.last + 1, 1, 0, 2, 65, UINT32_MAX, 1};
    probe_batch::Cache cache(fixture.physical, fixture.device);
    fixture.begin();
    require(!cache.record(fixture.command, input.volume, lights),
            "An unprepared optional gather keeps its existing sparse fallback");
    fixture.finish();
    prepare(cache);
    fixture.begin();
    auto abandoned = cache.record(fixture.command, input.volume, lights);
    require(bool(abandoned), "Valid selected-light field fits its bounded storage");
    require(cache.bufferBytes() == abandoned->bufferBytes() &&
                abandoned->bufferBytes() >= abandoned->descriptor().range + 272,
            "Resident accounting includes parameter storage and device allocation alignment");
    check(vkEndCommandBuffer(fixture.command)); // Deliberately never submitted.
    fixture.begin();
    auto field = cache.record(fixture.command, input.volume, lights);
    require(field && field != abandoned, "Recording alone cannot publish an unwritten reusable field");
    require(cache.recordedGathers() == 2 && cache.completedReuses() == 0,
            "Recorded preparation counts abandoned producers without claiming completion");
    readback(fixture, *field, input.masks);
    queryField(fixture, *input.volume, input.masks, *field);
    std::weak_ptr<const probe_batch::Field> discarded = abandoned;
    abandoned.reset();
    fixture.begin();
    auto reused = cache.record(fixture.command, input.volume, lights);
    require(reused == field, "A device-completed immutable field is reused without another gather");
    require(cache.recordedGathers() == 2 && cache.completedReuses() == 1,
            "Completed reuse is distinguished from another recorded producer");
    require(discarded.expired(), "Completed reuse releases abandoned cache-only producers");
    require(cache.bufferBytes() == field->bufferBytes(), "Abandoned resident storage is reclaimed after completed reuse");
    require(cache.peakBufferBytes() == 2 * field->bufferBytes(),
            "Peak residency includes abandoned and submitted owners after current storage shrinks");
    readback(fixture, *reused, input.masks);
    auto next = source(fixture, sorted, directional, 1);
    fixture.begin();
    auto replacement = cache.record(fixture.command, next.volume, lights);
    require(replacement && replacement != field, "Replacing the source never aliases the old field");
    readback(fixture, *replacement, next.masks);
    require(cache.bufferBytes() == field->bufferBytes() + replacement->bufferBytes(),
            "Resident accounting retains older submitted consumers across a source change");
    auto changedLights = lights;
    std::swap(changedLights[0], changedLights[3]);
    fixture.begin();
    auto changed = cache.record(fixture.command, next.volume, changedLights);
    require(changed && changed != replacement, "Light permutations retain their new selected-slot ownership");
    readback(fixture, *changed, next.masks);
    std::array<uint32_t, probe_batch::Cache::maximumLights> full{};
    for (uint32_t i = 0; i < full.size(); ++i) full[i] = lights[i % lights.size()];
    fixture.begin();
    auto largest = cache.record(fixture.command, next.volume, full);
    require(largest && largest->groups() == 16, "The full supported selection preserves every vector group");
    readback(fixture, *largest, next.masks);
    queryField(fixture, *next.volume, next.masks, *largest);
    cache.reset();
    fixture.begin();
    readback(fixture, *field, input.masks); // Old submitted consumers remain valid.
    const uint64_t bytes = field->descriptor().range;
    probe_batch::Cache limited(fixture.physical, fixture.device, bytes - 1);
    fixture.begin();
    require(!limited.record(fixture.command, input.volume, lights),
            "An oversized optional field declines without truncating light coverage");
    std::array<uint32_t, probe_batch::Cache::maximumLights + 1> excessive{};
    require(!cache.record(fixture.command, input.volume, excessive) &&
                !cache.record(fixture.command, input.volume, {}),
            "Empty and over-cap selections keep the sparse fallback available");
    fixture.finish();
    probe_batch::Cache exact(fixture.physical, fixture.device, bytes);
    prepare(exact);
    fixture.begin();
    auto exactField = exact.record(fixture.command, input.volume, lights);
    require(exactField && exactField->descriptor().range == bytes, "An exact payload budget retains complete coverage");
    readback(fixture, *exactField, input.masks);

    auto temporarySource = source(fixture, sorted, directional, 2);
    std::weak_ptr<const bake::Volume> retained = temporarySource.volume;
    auto temporaryCache = std::make_unique<probe_batch::Cache>(fixture.physical, fixture.device);
    prepare(*temporaryCache);
    fixture.begin();
    auto submittedOwner = temporaryCache->record(fixture.command, temporarySource.volume, lights);
    temporarySource.volume.reset();
    temporaryCache.reset();
    require(!retained.expired(), "A recorded consumer retains source and pipeline owners after cache destruction");
    readback(fixture, *submittedOwner, temporarySource.masks);
    submittedOwner.reset();
    require(retained.expired(), "Completed consumers release obsolete source volumes without a cache leak");
  }
  require(validationErrors == 0, "Gather, cancellation, reuse and replacement are Vulkan validation clean");
  std::puts("Selected probe visibility: exact gather, weighted receiver coverage, group reuse and owners passed");
} catch (VkResult error) {
  std::fprintf(stderr, "Probe visibility batch test failed: %d\n", int(error));
  return 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "Probe visibility batch fixture failed: %s\n", error.what());
  return 1;
}

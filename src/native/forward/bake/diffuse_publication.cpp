#include "forward/bake/job.h"
#include "forward/bake/storage_options.h"
#include "forward/bake/sun_depth.h"
#include "forward/image_dump.h"
#include "forward/lightmap/upload.h"
#include "forward/mixed/ownership.h"
#include "forward/mixed/publication.h"
#include "forward/probes/capture_dump.h"
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace remix::forward::bake {
  std::shared_ptr<Volume> Controller::prepareVolume(VkCommandBuffer command) {
    auto &grid = m_job->grid;
    if (std::none_of(grid.probes.begin(), grid.probes.end(),
                     [](const auto &p) { return p.statistics[3] > .5f; }) &&
        m_job->surfaceReceived == 0)
      throw std::runtime_error("Bake contains no usable irradiance probes");
    auto volume = std::make_shared<Volume>();
    volume->irradianceContents = m_job->irradianceContents;
    volume->reflectionSeeds = m_job->reflectionSeeds;
    volume->directDiffuse = m_job->hasDirectDiffuse;
    if (m_job->hasSun || std::any_of(grid.probes.begin(), grid.probes.end(),
                                    [](const auto &p) { return p.irradiance[7][3] > .5f; }))
      volume->sun = m_job->source->sun;
    volume->sunDepth = m_job->sunDepth;
    if (volume->sunDepth) volume->sunDepth->recordUpload(command);
    volume->hash = m_hash;
    volume->sceneScale = m_source->sceneScale;
    volume->irradianceHeader = grid.header;
    volume->coarseTransferWords = m_job->coarseTransferWords;
    volume->usableIrradianceProbes = uint32_t(std::count_if(
        grid.probes.begin(), grid.probes.end(), [](const Result &probe) { return probe.statistics[3] >= .5f; }));
    volume->coarseIrradianceProbes = uint32_t(grid.probes.size());
    volume->finestIrradianceSpacing = grid.header.originSpacing[3];
    if (adaptive(grid.header)) {
      const auto nodes = gridNodes(grid.header);
      volume->coarseIrradianceProbes = grid.lookup[nodes + 3];
      for (uint32_t cell = 0; cell < nodes; ++cell) {
        const auto reference = grid.lookup[nodes + 4 + cell];
        if (!patchOffset(reference)) continue;
        const auto factor = patchFactor(reference, grid.lookup[nodes + 1]);
        ++volume->irradiancePatchCounts[std::countr_zero(factor) - 1];
        volume->finestIrradianceSpacing = std::min(volume->finestIrradianceSpacing,
                                                  grid.header.originSpacing[3] / factor);
      }
    }
    volume->mixedLights = std::make_shared<mixed::Bindings>(m_job->mixedLights->lights);
    mixed::dumpOwnership(*m_job->mixedLights, m_job->surfaceLayout);
    const size_t legacyBytes = gridBytes(grid);
    const auto &publishedStyles = m_job->groupedStyles.styles.empty() ? m_job->styleWords : m_job->groupedStyles.words;
    const size_t styleBytes = publishedStyles.size() * sizeof(uint32_t);
    volume->styledLighting = styleBytes != 0;
    volume->groupedStyles = m_job->groupedStyles.styles;
    const auto localWords = m_job->localProbes ? m_job->localProbes->words() : std::vector<uint32_t>{};
    const size_t localBytes = localWords.size() * sizeof(uint32_t);
    auto transfer = coarseTransferSamplingRequested() ? volume->coarseTransferWords : nullptr;
    if (transfer) {
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(m_physical, &properties);
      const size_t combined = ((legacyBytes + styleBytes + transfer->size() * sizeof(uint32_t) + 15) & ~size_t(15)) + localBytes;
      if (combined > properties.limits.maxStorageBufferRange) {
        std::fprintf(stderr, "Forward coarse transfer sampling declined: combined buffer exceeds device storage range\n");
        transfer.reset();
      }
    }
    const size_t transferBytes = transfer ? transfer->size() * sizeof(uint32_t) : 0;
    const size_t baseBytes = legacyBytes + transferBytes;
    const size_t localOffset = (baseBytes + styleBytes + 15) & ~size_t(15);
    const size_t bytes = localOffset + localBytes;
    probes::dumpPublishedData(".surfaces-published.bin", m_job->surfacePixels.data(), m_job->surfacePixels.size() * sizeof(uint16_t));
    probes::dumpPublishedData(".surface-charts.bin", m_job->surfaceLayout.charts.data(),
                              m_job->surfaceLayout.charts.size() * sizeof(lightmap::Allocation));
    const char *surfaceSamples = std::getenv("REMIX_DEBUG_BAKE_SURFACE_SAMPLES");
    if (surfaceSamples && std::strcmp(surfaceSamples, "1") == 0) {
      probes::dumpPublishedData(".surface-points.bin", m_job->surfaceLayout.points.data(),
                                m_job->surfaceLayout.points.size() * sizeof(Point));
      probes::dumpPublishedData(".surface-results.bin", m_job->surfaces.data(),
                                m_job->surfaces.size() * sizeof(Result));
      probes::dumpPublishedData(".surface-owners.bin", m_job->surfaceLayout.owners.data(),
                                m_job->surfaceLayout.owners.size() * sizeof(uint32_t));
    }
    const char *pageOverride = std::getenv("REMIX_DEBUG_MASK_PAGES");
    auto visibility = mixed::prepareVisibility(m_physical, m_device, *m_job->mixedLights,
        m_job->surfaceLayout, m_job->surfacePixels,
        !pageOverride || std::strcmp(pageOverride, "0") != 0,
        m_job->unifiedMasks ? &*m_job->unifiedMasks : nullptr,
        directionalFieldsRequested() ? std::span<const uint32_t>(m_job->directionalWords) : std::span<const uint32_t>{},
        directionalFieldsRequested() && m_job->completeDirectionals);
    auto &visibilityPages = visibility.pages;
    auto &occlusion = visibility.lookup;
    const auto compressedAllocation = visibility.compressedAllocation;
    volume->occlusion = std::move(visibility.lookupImage);
    volume->visibilityPages = std::move(visibility.pageImage);
    volume->surfaceDirectOffset = visibility.directionalOffset;
    const uint32_t surfaceLayers = lightmap::uploadedLayers(visibility.indexed, visibility.indexedDirect);
    const auto &surfaceLayout = m_job->surfaceLayout;
    const auto segments = lightmap::uploadSegments(surfaceLayout, visibility.indexed, visibility.indexedDirect);
    const size_t surfaceBytes = m_job->surfacePixels.empty() ? 0 : size_t(surfaceLayout.pageSize) *
        surfaceLayout.pageSize * surfaceLayout.pages * surfaceLayers * 4 * sizeof(uint16_t);
    std::fprintf(stderr, "Forward indexed visibility: %u, %llu -> %llu GPU bytes, %llu primary texels, max error %.9g\n",
        unsigned(visibility.indexed), static_cast<unsigned long long>(visibility.legacyAllocation),
        static_cast<unsigned long long>(visibility.totalAllocation),
        static_cast<unsigned long long>(visibility.convertedTexels), visibility.maximumError);
    std::fprintf(stderr, "Forward indexed directionality: %u, %zu bytes, table word %u, %u atlas layers, coverage=%s\n",
        unsigned(visibility.indexedDirect), visibility.indexedDirect ? m_job->directionalWords.size() * sizeof(uint32_t) : 0,
        visibility.directionalOffset, surfaceLayers,
        visibility.indexedDirect ? (m_job->completeDirectionals ? "complete" : "retained") : "legacy");
    const auto &maskCosts = m_job->mixedLights->masks.candidateCosts();
    if (maskCosts.candidates)
      probes::dumpPublishedData(".overflow-mask-costs.bin", &maskCosts, sizeof(maskCosts));
    probes::dumpPublishedData(".overflow-mask-entries.bin", m_job->mixedLights->masks.entries().data(),
                              m_job->mixedLights->masks.entries().size_bytes());
    probes::dumpPublishedData(".overflow-mask-bytes.bin", m_job->mixedLights->masks.bytes().data(),
                              m_job->mixedLights->masks.bytes().size_bytes());
    volume->occlusionParameters = {m_job->mixedLights->occlusion.wordsPerChart() |
                                       (visibility.indexed ? lightmap::indexedVisibilityFlag : 0u),
                                   m_job->mixedLights->occlusion.chartCount(), occlusion.width,
                                   visibility.hasMasks ? uint32_t(2 + m_job->mixedLights->occlusion.words().size()) : 0u};
    const size_t occlusionBytes = occlusion.words.size() * sizeof(uint32_t);
    volume->content.staticOcclusion = scene::fastHash(occlusion.words.data(), occlusionBytes) | 1;
    const size_t occlusionOffset = bytes + surfaceBytes;
    const size_t visibilityOffset = occlusionOffset + occlusionBytes;
    const size_t visibilityBytes = visibilityPages ? visibilityPages->texels.size() : 0;
    if (visibilityPages) {
      volume->content.staticOcclusion = scene::hashBytes(visibilityPages->texels.data(), visibilityBytes,
                                                        volume->content.staticOcclusion) | 1;
    }
    const uint64_t pageStatistics[] = {visibilityPages ? visibilityPages->size : 0,
        visibilityPages ? visibilityPages->layers : 0, visibilityBytes, occlusionBytes,
        compressedAllocation, volume->occlusion->bytes() +
            (volume->visibilityPages ? volume->visibilityPages->bytes() : 0)};
    probes::dumpPublishedData(".visibility-pages.bin", pageStatistics, sizeof(pageStatistics));
    std::vector<std::byte> data(bytes);
    auto gpuHeader = publicationHeader(grid.header, grid.lookup.size());
    gpuHeader.settings[3] = publicationSettings(0, false, !grid.depth.empty(), transferBytes != 0);
    std::memcpy(data.data(), &gpuHeader, sizeof(GridHeader));
    std::memcpy(data.data() + sizeof(GridHeader), grid.probes.data(), grid.probes.size() * sizeof(Result));
    if (!grid.lookup.empty())
      std::memcpy(data.data() + sizeof(GridHeader) + grid.probes.size() * sizeof(Result),
                  grid.lookup.data(), grid.lookup.size() * sizeof(uint32_t));
    if (!grid.depth.empty()) {
      grid.depth.validate(uint32_t(grid.probes.size()));
      const size_t offset = sizeof(GridHeader) + grid.probes.size() * sizeof(Result) + lookupBytes(grid);
      grid.depth.write(std::span(data).subspan(offset, grid.depth.bytes()));
    }
    if (transferBytes) {
      std::memcpy(data.data() + legacyBytes, transfer->data(), transferBytes);
      std::fprintf(stderr, "Forward coarse transfer sampling enabled: %zu GPU bytes\n", transferBytes);
    }
    // Hash once at publication, before releasing staging data. Diagnostics can
    // then distinguish input identity from changes in the actual baked result
    // without a GPU readback or a mutable worker-state dependency.
    probes::dumpPublishedData(".irradiance-published.bin", data.data(), baseBytes);
    volume->content.grid = scene::fastHash(data.data(), baseBytes) | 1;
    if (styleBytes) {
      volume->content.styledIndirect = scene::fastHash(publishedStyles.data(), styleBytes) | 1;
      std::memcpy(data.data() + baseBytes, publishedStyles.data(), styleBytes);
      auto header = gpuHeader;
      header.settings[3] = publicationSettings(0, true, !grid.depth.empty(), transferBytes != 0);
      std::memcpy(data.data(), &header, sizeof(header));
      probes::dumpPublishedData(".styled-irradiance.bin", publishedStyles.data(), styleBytes);
    }
    if (localBytes) {
      std::memcpy(data.data() + localOffset, localWords.data(), localBytes);
      auto header = gpuHeader;
      header.settings[3] = publicationSettings((localOffset - sizeof(GridHeader)) / 16, styleBytes != 0, !grid.depth.empty(), transferBytes != 0);
      std::memcpy(data.data(), &header, sizeof(header));
      volume->content.grid = scene::fastHash(localWords.data(), localBytes, volume->content.grid) | 1;
      probes::dumpPublishedData(".local-probe-visibility.bin", localWords.data(), localBytes);
    }
    if (surfaceBytes) {
      if (!visibility.indexed) volume->content.surfaces = scene::fastHash(m_job->surfacePixels.data(), surfaceBytes) | 1;
      else {
        uint64_t hash = 0;
        for (const auto &segment : segments)
          hash = scene::fastHash(m_job->surfacePixels.data() + segment.source,
                                segment.values * sizeof(uint16_t), hash);
        volume->content.surfaces = hash | 1;
      }
    }
    if (surfaceBytes) {
      const auto &atlas = m_job->surfaceLayout;
      volume->surfaceAtlas = std::make_unique<Image>();
      volume->surfaceAtlas->create(
          m_physical, m_device, {atlas.pageSize, atlas.pageSize}, VK_FORMAT_R16G16B16A16_SFLOAT,
          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT,
          VK_IMAGE_ASPECT_COLOR_BIT, atlas.pages * surfaceLayers);
      volume->surfaces = m_job->surfaceGeometry;
    }
    volume->staging = std::make_unique<Buffer>();
    volume->staging->create(m_physical, m_device, visibilityOffset + visibilityBytes,
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    volume->staging->write(data.data(), data.size());
    if (surfaceBytes)
      for (const auto &segment : segments)
        volume->staging->write(m_job->surfacePixels.data() + segment.source,
            segment.values * sizeof(uint16_t), bytes + segment.destination * sizeof(uint16_t));
    volume->staging->write(occlusion.words.data(), occlusionBytes, occlusionOffset);
    if (visibilityBytes)
      volume->staging->write(visibilityPages->texels.data(), visibilityBytes, visibilityOffset);
    volume->data = std::make_unique<Buffer>();
    volume->data->create(m_physical, m_device, bytes,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                             (ImageDump::enabled(m_physical) ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : 0u),
                         Buffer::Memory::Device);
    const VkBufferCopy copy{0, 0, bytes};
    vkCmdCopyBuffer(command, volume->staging->handle, volume->data->handle, 1, &copy);
    VkImageMemoryBarrier surfaceReady{};
    volume->occlusion->upload(command, volume->staging->handle, occlusionOffset);
    if (volume->visibilityPages)
      volume->visibilityPages->upload(command, volume->staging->handle, visibilityOffset);
    if (volume->surfaceAtlas) {
      const auto &atlas = m_job->surfaceLayout;
      const VkImageMemoryBarrier transfer{
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = volume->surfaceAtlas->handle,
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, atlas.pages * surfaceLayers}};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &transfer);
      const VkBufferImageCopy imageCopy{
          .bufferOffset = bytes,
          .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, atlas.pages * surfaceLayers},
          .imageExtent = {atlas.pageSize, atlas.pageSize, 1}};
      vkCmdCopyBufferToImage(command, volume->staging->handle, volume->surfaceAtlas->handle,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &imageCopy);
      surfaceReady = transfer;
      surfaceReady.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      surfaceReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      surfaceReady.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      surfaceReady.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    const VkMemoryBarrier ready{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &ready, 0, nullptr, volume->surfaceAtlas ? 1u : 0u,
                         volume->surfaceAtlas ? &surfaceReady : nullptr);
    return volume;
  }
}

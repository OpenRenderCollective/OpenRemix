#include "runtime/runtime.h"
#if REMIX_HAS_NUMOS
#include "sky/renderer.h"
#endif
#include <cstdio>
#include <exception>
#include <string>

// The raster backend's NUMOS sky. Before the target pass NUMOS records its
// frame for the world camera; after the world's layers the pass is left, the
// target's depth goes to the screen clouds and the composite, and the pass is
// re-entered to blend the sky over the frame (RasterRenderer::recordSkyComposite).
// No scene TLAS: the haze is unshadowed and the ground level is configured.
namespace remix {
  bool RemixNativeRuntime::recordRasterSky(VkCommandBuffer buffer, RasterRenderer &raster) {
    m_rasterSkyActive = false;
#if REMIX_HAS_NUMOS
    const auto &settings = m_profiles.current();
    if (m_pathTracer || !settings.sky.enabled || !m_skyUnavailable.empty() || !m_rasterSkyFailure.empty()) {
      m_rasterSkyReady = false;
      return false;
    }
    try {
      if (!m_rasterSky)
        m_rasterSky = std::make_shared<sky::Renderer>(m_physicalDevice, m_device, m_traceSupported);
      // This slot's last frame has completed.
      m_rasterSky->selectFrame(m_frameIndex);
      m_rasterSky->setSettings(settings.sky, m_skyControl);
      const auto &view = raster.view(), &projection = raster.projection();
      m_rasterUpAxis.observe(scene::inverse(view));
      const sky::FrameDescription description{
          .view = view,
          .projection = projection,
          .previousView = m_rasterSkyReady ? m_rasterSkyView : view,
          .previousProjection = m_rasterSkyReady ? m_rasterSkyProjection : projection,
          .renderWidth = m_swapchainExtent.width,
          .renderHeight = m_swapchainExtent.height,
          .outputWidth = m_swapchainExtent.width,
          .outputHeight = m_swapchainExtent.height,
          .metersPerUnit = settings.trace.sceneScale,
          .upAxis = m_rasterUpAxis.resolve(settings.trace.volumetricUpAxis),
          .cameraCut = !m_rasterSkyReady};
      gpu::GpuScope scope(buffer, "NUMOS sky");
      m_rasterSkyActive = m_rasterSky->record(buffer, description);
      m_rasterSkyReady |= m_rasterSkyActive;
      m_rasterSkyView = view;
      m_rasterSkyProjection = projection;
    } catch (const std::exception &exception) {
      m_rasterSkyFailure = exception.what();
    } catch (VkResult result) {
      m_rasterSkyFailure = "Vulkan error " + std::to_string(int(result));
    }
    if (!m_rasterSkyFailure.empty()) {
      std::fprintf(stderr, "NUMOS sky stopped: %s\n", m_rasterSkyFailure.c_str());
      m_rasterSkyActive = m_rasterSkyReady = false;
    }
#else
    (void)buffer;
    (void)raster;
#endif
    return m_rasterSkyActive;
  }

  void RemixNativeRuntime::compositeRasterSky(VkCommandBuffer buffer, RasterRenderer &raster,
                                              VkRenderPassBeginInfo &pass) {
#if REMIX_HAS_NUMOS
    if (!m_rasterSkyActive || !raster.depthImage())
      return;
    const auto &settings = m_profiles.current();
    gpu::GpuScope scope(buffer, "NUMOS composite", m_swapchainExtent);
    // The scene's depth stays in the target's depth attachment.
    vkCmdEndRenderPass(buffer);
    const auto depthBarrier = [&](VkImageLayout from, VkImageLayout to, VkPipelineStageFlags source,
                                  VkAccessFlags sourceAccess, VkPipelineStageFlags target, VkAccessFlags targetAccess) {
      const VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                            .srcAccessMask = sourceAccess,
                                            .dstAccessMask = targetAccess,
                                            .oldLayout = from,
                                            .newLayout = to,
                                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                            .image = raster.depthImage(),
                                            .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
      vkCmdPipelineBarrier(buffer, source, target, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    };
    depthBarrier(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    bool composed = false;
    try {
      m_rasterSky->recordRasterPostDepth(buffer, raster.depthImage(), raster.depthView(), raster.projection(), 1.f);
      composed = m_rasterSky->recordRasterComposite(buffer, settings.sky.rasterExposure * settings.sky.radianceScale,
                                                    settings.sky.gameSky, raster.linearTarget());
    } catch (const std::exception &exception) {
      m_rasterSkyFailure = exception.what();
    } catch (VkResult result) {
      m_rasterSkyFailure = "Vulkan error " + std::to_string(int(result));
    }
    depthBarrier(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    // The composite's images, written by compute, are read by the blend.
    const VkMemoryBarrier written = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                     .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                     .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                         &written, 0, nullptr, 0, nullptr);
    pass.renderPass = m_resumePass;
    vkCmdBeginRenderPass(buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
    if (composed)
      raster.recordSkyComposite(buffer, m_rasterSky->rasterTransmittance(), m_rasterSky->rasterAddition());
    if (!m_rasterSkyFailure.empty()) {
      std::fprintf(stderr, "NUMOS sky stopped: %s\n", m_rasterSkyFailure.c_str());
      m_rasterSkyActive = m_rasterSkyReady = false;
    }
#else
    (void)buffer;
    (void)raster;
    (void)pass;
#endif
  }
} // namespace remix

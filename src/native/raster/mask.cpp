#include "raster/gpu.h"
#include "raster/shaders/embedded.h"
#include "runtime/pipelines.h"

// Masked overlays (remixapi_InstanceInfoMaskEXT, masked UI commands): what a
// host did with the stencil buffer. A group's sources are drawn as coverage
// into an R8 target at target resolution, with the silhouette machinery of the
// halo outlines (raster/halo.cpp): one target per group, one pass per run of
// sources, depth-tested against the frame's depth attachment when asked. A
// source limited to another group (`within`) samples that group's target and
// discards outside it. Masked draws are ordinary raster draws whose fragment
// stage samples their group's target at the fragment and discards outside (or
// inside) the coverage, so blending, alpha test and depth state are untouched.
// Role CUT hides a scene instance from the camera inside a group's coverage.
// Where the instance is rasterized here it is a masked draw (outside). Where
// it is traced, the path tracer samples the group's target itself, before the
// frame has a depth: recordCutCoverage draws those groups' runs ahead of it.
namespace remix {
  using namespace scene;
  using namespace raster::detail;
  using namespace raster;

  void RasterRenderer::Gpu::destroyMask() {
    auto &m = mask;
    for (auto &target : m.targets)
      destroyCoverageTarget(target);
    m.targets.clear();
    for (auto &pipeline : m.within) {
      if (pipeline)
        pipeline::destroyPipeline(device, pipeline, nullptr);
      pipeline = VK_NULL_HANDLE;
    }
    for (auto *shader : {&m.withinFragment, &m.overlayFragment}) {
      if (*shader)
        pipeline::destroyShaderModule(device, *shader, nullptr);
      *shader = VK_NULL_HANDLE;
    }
    if (m.layout)
      pipeline::destroyPipelineLayout(device, m.layout, nullptr);
    m.layout = VK_NULL_HANDLE;
    m.ready = m.failed = false;
    m.runs.clear();
    m.commands.clear();
    m.used = 0;
    m.split = 0;
    m.cut = {};
  }

  bool RasterRenderer::Gpu::prepareMask() {
    auto &m = mask;
    if (m.ready)
      return true;
    if (!pass || m.failed || halo.failed)
      return false;
    bool ready = prepareCoverage();
    if (!m.layout) {
      // Set 0 and the push constants as the raster layout, so both layouts are
      // compatible for a draw's material; set 1: a coverage target.
      const VkDescriptorSetLayout sets[] = {setLayout, setLayout};
      const VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
      const VkPipelineLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                               .setLayoutCount = 2,
                                               .pSetLayouts = sets,
                                               .pushConstantRangeCount = 1,
                                               .pPushConstantRanges = &push};
      check(pipeline::createPipelineLayout(device, &info, nullptr, &m.layout));
    }
    const auto module = [&](VkShaderModule &out, const uint32_t *code, size_t bytes) {
      if (out)
        return;
      const VkShaderModuleCreateInfo info = {
          .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
      check(pipeline::createShaderModule(device, &info, nullptr, &out));
    };
    module(m.withinFragment, maskWithinShader, sizeof(maskWithinShader));
    module(m.overlayFragment, maskOverlayShader, sizeof(maskOverlayShader));
    for (int tested = 0; tested < 2; ++tested)
      ready &= coveragePipeline(m.within[tested], m.withinFragment, m.layout, tested != 0);
    m.ready = ready;
    return ready;
  }

  void RasterRenderer::Gpu::createMaskTargets(uint32_t count) {
    auto &m = mask;
    while (m.targets.size() < count) {
      Halo::Target target;
      try {
        createCoverageTarget(target, true);
      } catch (...) {
        destroyCoverageTarget(target);
        throw;
      }
      m.targets.push_back(target);
    }
  }

  uint32_t RasterRenderer::maskRuns() const { return m_gpu ? uint32_t(m_gpu->mask.runs.size()) : 0; }

  RasterRenderer::CutCoverage RasterRenderer::cutCoverage() const {
    CutCoverage out;
    if (!m_gpu)
      return out;
    const auto &m = m_gpu->mask;
    out.count = m.cut.count;
    for (uint32_t i = 0; i < out.count; ++i)
      out.images[i] = {m_gpu->halo.sampler, m.targets[m.cut.targets[i]].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    std::copy(std::begin(m.cut.slots), std::end(m.cut.slots), out.slots);
    out.digest = m.cut.digest;
    return out;
  }

  void RasterRenderer::recordMasks(VkCommandBuffer command) { m_gpu->recordMaskRuns(command, false); }

  void RasterRenderer::recordCutCoverage(VkCommandBuffer command) {
    if (m_gpu->mask.cut.count)
      m_gpu->recordMaskRuns(command, true);
  }

  void RasterRenderer::Gpu::recordMaskRuns(VkCommandBuffer command, bool early) {
    auto &g = *this;
    auto &m = g.mask;
    const VkViewport viewport = {0, 0, float(g.extent.width), float(g.extent.height), 0, 1};
    const VkRect2D whole = {{0, 0}, g.extent};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &whole);
    for (const auto &run : m.runs) {
      if (early && !run.early)
        continue;
      // The whole target is cleared: masked draws sample it anywhere.
      const VkClearValue clear = {};
      const VkRenderPass passes[2][2] = {{g.halo.loadPass, g.halo.maskPass}, {g.halo.earlyLoadPass, g.halo.earlyPass}};
      const VkRenderPassBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                           .renderPass = passes[early][run.clear],
                                           .framebuffer = m.targets[run.target].framebuffer,
                                           .renderArea = whole,
                                           .clearValueCount = run.clear ? 1u : 0u,
                                           .pClearValues = &clear};
      vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
      const auto layout = run.within >= 0 ? m.layout : g.layout;
      if (run.within >= 0)
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &m.targets[run.within].set, 0,
                                nullptr);
      // Ahead of the scene nothing is depth-tested: the untested silhouette.
      const VkPipeline untested = run.within >= 0 ? m.within[0] : g.halo.mask[0];
      for (uint32_t i = run.first; i < run.first + run.count; ++i) {
        const auto &draw = m.commands[i];
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, early ? untested : draw.pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &draw.set, 0, nullptr);
        vkCmdBindVertexBuffers(command, 0, 1, &draw.vertices, &draw.vertexOffset);
        vkCmdBindIndexBuffer(command, draw.indices, draw.indexOffset, VK_INDEX_TYPE_UINT32);
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push),
                           &draw.push);
        vkCmdDrawIndexed(command, draw.count, 1, draw.firstIndex, draw.baseVertex, 0);
      }
      vkCmdEndRenderPass(command);
    }
  }
} // namespace remix

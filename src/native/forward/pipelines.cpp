#include "forward/probes/cube_storage.h"
#include "forward/mixed/chart_highlights.h"
#include "forward/mixed/publication.h"
#include "runtime/pipelines.h"
#include "forward/gpu.h"
#include "forward/bake/storage_options.h"
#include "forward/probe_batch/endpoint_cells.h"
#include "forward/ray/gi_shaders/embedded.h"
#include "forward/shaders/embedded.h"
#include "forward/water/shaders/embedded.h"
#include <cstdlib>

namespace remix::forward {
  void Renderer::Gpu::createPipelines(VkRenderPass output) {
    pipeline::nameCode(vertShader, "forward.vertex");
    pipeline::nameCode(lightmapVertShader, "forward.lightmap-vertex");
    pipeline::nameCode(fragShader, "forward.fragment");
    pipeline::nameCode(ssrFragShader, "forward.reflection-guides");
    pipeline::nameCode(prepassedFragShader, "forward.prepassed-fragment");
    pipeline::nameCode(prepassedSsrFragShader, "forward.prepassed-reflection-guides");
    pipeline::nameCode(viewModelFragShader, "forward.viewmodel-fragment");
    pipeline::nameCode(viewModelEarlyFragShader, "forward.viewmodel-early-fragment");
    pipeline::nameCode(opaqueDepthFragShader, "forward.opaque-depth");
    pipeline::nameCode(ray::gi::fragShader, "forward.live-gi");
    pipeline::nameCode(ray::gi::guidesShader, "forward.live-gi-guides");
    struct Modules {
      VkDevice device;
      std::vector<VkShaderModule> handles;
      VkShaderModule add(const uint32_t *code, size_t bytes) {
        auto module = shaderModule(device, code, bytes);
        handles.push_back(module);
        return module;
      }
      ~Modules() {
        for (auto module : handles)
          pipeline::destroyShaderModule(device, module, nullptr);
      }
    } modules{device, {}};
    const bool nativeCube = probes::nativeCubeStorage(physical);
    auto reflectionModule = [&](const auto &stacked, const auto &cube) {
      return nativeCube ? modules.add(cube, sizeof(cube)) : modules.add(stacked, sizeof(stacked));
    };
    const auto vertex = modules.add(vertShader, sizeof(vertShader));
    const auto lightmapVertex = modules.add(lightmapVertShader, sizeof(lightmapVertShader));
    const auto fragment = [&] {
      if (techniquePlan.inlineReflections)
        return options.depthPrepass ? reflectionModule(inlinePrepassedFragShader, inlinePrepassedFragCubeShader)
                                    : reflectionModule(inlineFragShader, inlineFragCubeShader);
      if (techniquePlan.reflectionResolve)
        return options.depthPrepass ? modules.add(prepassedSsrFragShader, sizeof(prepassedSsrFragShader))
                                    : modules.add(ssrFragShader, sizeof(ssrFragShader));
      return options.depthPrepass ? modules.add(prepassedFragShader, sizeof(prepassedFragShader))
                                  : modules.add(fragShader, sizeof(fragShader));
    }();
    VkShaderModule giFragment = VK_NULL_HANDLE;
    const auto terrainFragment = modules.add(fragShader, sizeof(fragShader));
    const auto depthFragment = modules.add(opaqueDepthFragShader, sizeof(opaqueDepthFragShader));
    VkShaderModule giTerrainFragment = VK_NULL_HANDLE;
    VkShaderModule giViewModelFragment = VK_NULL_HANDLE, giViewModelEarlyFragment = VK_NULL_HANDLE;
    if (techniquePlan.rayDiffuse()) {
      const bool composed = techniquePlan.composesBakedDirect();
      giViewModelFragment = composed
          ? reflectionModule(ray::gi::composedViewModelShader, ray::gi::composedViewModelCubeShader)
          : reflectionModule(ray::gi::viewModelShader, ray::gi::viewModelCubeShader);
      giViewModelEarlyFragment = composed
          ? reflectionModule(ray::gi::composedViewModelEarlyShader, ray::gi::composedViewModelEarlyCubeShader)
          : reflectionModule(ray::gi::viewModelEarlyShader, ray::gi::viewModelEarlyCubeShader);
      giTerrainFragment = composed ? modules.add(ray::gi::composedFragShader, sizeof(ray::gi::composedFragShader))
                                   : modules.add(ray::gi::fragShader, sizeof(ray::gi::fragShader));
      giFragment = techniquePlan.reflectionResolve
          ? composed ? modules.add(ray::gi::composedGuidesShader, sizeof(ray::gi::composedGuidesShader))
                     : modules.add(ray::gi::guidesShader, sizeof(ray::gi::guidesShader))
          : giTerrainFragment;
    }
    const auto shadowVertex = modules.add(shadowVertShader, sizeof(shadowVertShader));
    const auto shadowFragment = modules.add(shadowFragShader, sizeof(shadowFragShader));
    const auto localVertex = modules.add(localShadowVertShader, sizeof(localShadowVertShader));
    const auto localFragment = modules.add(localShadowFragShader, sizeof(localShadowFragShader));
    const auto compositeVertex = modules.add(compositeVertShader, sizeof(compositeVertShader));
    const auto compositeFragment = options.water ? (samples == VK_SAMPLE_COUNT_1_BIT
                                       ? modules.add(water::compositeSingleShader, sizeof(water::compositeSingleShader))
                                       : modules.add(water::compositeFragShader, sizeof(water::compositeFragShader)))
                                   : samples == VK_SAMPLE_COUNT_1_BIT
                                       ? modules.add(compositeSingleShader, sizeof(compositeSingleShader))
                                       : modules.add(compositeFragShader, sizeof(compositeFragShader));
    const auto compute = modules.add(clusterShader, sizeof(clusterShader));
    const VkVertexInputBindingDescription binding{0, sizeof(scene::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(scene::Vertex, uv)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(scene::Vertex, color)},
        {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, normal)}};
    const VkPipelineVertexInputStateCreateInfo vertexInput{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 4,
        .pVertexAttributeDescriptions = attributes};
    const VkVertexInputBindingDescription mappedBinding{0, sizeof(lightmap::Vertex),
                                                        VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription mappedAttributes[] = {
        attributes[0],
        attributes[1],
        attributes[2],
        attributes[3],
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(lightmap::Vertex, coordinate)},
        {5, 0, VK_FORMAT_R32G32B32A32_UINT, offsetof(lightmap::Vertex, lights)},
        {6, 0, VK_FORMAT_R32G32B32A32_UINT, offsetof(lightmap::Vertex, directLights)},
        {7, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(lightmap::Vertex, atlasBounds)}};
    const VkPipelineVertexInputStateCreateInfo mappedInput{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &mappedBinding,
        .vertexAttributeDescriptionCount = 8,
        .pVertexAttributeDescriptions = mappedAttributes};
    const VkPipelineVertexInputStateCreateInfo fullscreenInput{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    const VkPipelineInputAssemblyStateCreateInfo assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1};
    const VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{.sType =
                                                       VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                   .dynamicStateCount = 2,
                                                   .pDynamicStates = states};
    enum class Pass { Opaque, Shadow, Composite, Terrain, Depth, ViewModel, DepthBatch };
    using Coverage = lightmap::ReceiverCoverage;
    const auto *localLightWorkView = std::getenv("REMIX_DEBUG_LOCAL_LIGHT_WORK_VIEW");
    const uint32_t localLightView = !localLightWorkView ? 0u :
        std::strcmp(localLightWorkView, "early") == 0 ? 9u :
        std::strcmp(localLightWorkView, "late") == 0 ? 10u :
        std::strcmp(localLightWorkView, "members") == 0 ? 11u :
        std::strcmp(localLightWorkView, "ownership") == 0 ? 12u :
        std::strcmp(localLightWorkView, "uncovered-clusters") == 0 ? 13u : 8u;
    const bool directionalOwnershipMask = mixed::bakedLightMasksEnabled() ||
        std::getenv("REMIX_DEBUG_DIRECTIONAL_OWNERSHIP_MASK") != nullptr;
    auto graphics = [&](VkShaderModule vs, VkShaderModule fs, VkRenderPass pass, Pass mode = Pass::Opaque,
                         Coverage coverage = Coverage::Mixed, bool indexedLightmap = false,
                         bool roughDielectric = false) {
      const bool shadow = mode == Pass::Shadow, composite = mode == Pass::Composite;
      const bool terrain = mode == Pass::Terrain;
      const bool batchedDepth = mode == Pass::DepthBatch;
      const bool depthOnly = mode == Pass::Depth || batchedDepth;
      const VkVertexInputBindingDescription batchBindings[] = {
          {0, sizeof(depth_batch::Position), VK_VERTEX_INPUT_RATE_VERTEX},
          {1, sizeof(uint32_t), VK_VERTEX_INPUT_RATE_VERTEX}};
      const VkVertexInputAttributeDescription batchAttributes[] = {
          {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 1, VK_FORMAT_R32_UINT, 0}};
      const VkPipelineVertexInputStateCreateInfo batchInput{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
          .vertexBindingDescriptionCount = 2, .pVertexBindingDescriptions = batchBindings,
          .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = batchAttributes};
      const bool viewModel = mode == Pass::ViewModel;
      const bool chartHighlights = mixed::chartHighlightsEnabled() && !techniquePlan.rayDiffuse() &&
          !depthOnly && vs == lightmapVertex && coverage != Coverage::Unmapped;
      // Ordinary vertices have no chart coordinates. Lightmap vertex ranges
      // can specialize either coverage without changing their buffer stride.
      // Mixed triangles retain the generic lightmap/probe selection.
      const uint32_t view = localLightView && mode == Pass::Opaque ? localLightView : options.debugView;
      const uint32_t values[] = {view, uint32_t(bake::directionalFieldsRequested()),
                                  uint32_t(coverage == Coverage::Mapped),
                                  uint32_t(coverage == Coverage::Unmapped || vs == vertex),
                                  uint32_t(probeVisibility != nullptr), uint32_t(options.bicubicLightmaps),
                                  uint32_t(chartHighlights),
                                  uint32_t(probe_batch::endpointCellsRequested()),
                                  uint32_t(mode == Pass::Opaque),
                                  uint32_t(mode == Pass::Opaque && directionalOwnershipMask), uint32_t(indexedLightmap),
                                  uint32_t(mode == Pass::Opaque && !std::getenv("REMIX_DEBUG_DISABLE_REFLECTION_SPATIAL_MASK")),
                                  uint32_t(roughDielectric)};
      const VkSpecializationMapEntry entries[] = {{1, 0, sizeof(uint32_t)},
                                                  {2, sizeof(uint32_t), sizeof(uint32_t)},
                                                  {3, 2 * sizeof(uint32_t), sizeof(uint32_t)},
                                                  {4, 3 * sizeof(uint32_t), sizeof(uint32_t)},
                                                  {5, 4 * sizeof(uint32_t), sizeof(uint32_t)},
                                                  {6, 5 * sizeof(uint32_t), sizeof(uint32_t)},
                                                  {7, 6 * sizeof(uint32_t), sizeof(uint32_t)},
                                                  {8, 7 * sizeof(uint32_t), sizeof(uint32_t)},
                                                  {15, 8 * sizeof(uint32_t), sizeof(uint32_t)},
                                                  {16, 9 * sizeof(uint32_t), sizeof(uint32_t)},
                                                  {17, 10 * sizeof(uint32_t), sizeof(uint32_t)},
                                                  {18, 11 * sizeof(uint32_t), sizeof(uint32_t)},
                                                  {19, 12 * sizeof(uint32_t), sizeof(uint32_t)}};
      const VkSpecializationInfo specialization{uint32_t(std::size(entries)), entries, sizeof(values), values};
      const VkPipelineShaderStageCreateInfo stages[] = {
          {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
           .stage = VK_SHADER_STAGE_VERTEX_BIT,
           .module = vs,
           .pName = "main"},
          {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
           .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
           .module = fs,
           .pName = "main",
           .pSpecializationInfo = !shadow && !composite ? &specialization : nullptr}};
      const VkPipelineRasterizationStateCreateInfo raster{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
          .polygonMode = VK_POLYGON_MODE_FILL,
          .cullMode = VK_CULL_MODE_NONE,
          .frontFace = VK_FRONT_FACE_CLOCKWISE,
          .depthBiasEnable = shadow && vs != localVertex,
          .depthBiasConstantFactor = 1.5f,
          .depthBiasSlopeFactor = 2.f,
          .lineWidth = 1};
      const VkPipelineMultisampleStateCreateInfo multisample{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
          .rasterizationSamples = shadow || composite ? VK_SAMPLE_COUNT_1_BIT : samples,
          .alphaToCoverageEnable = VK_FALSE};
      const VkPipelineDepthStencilStateCreateInfo depthState{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
          .depthTestEnable = VK_TRUE,
          // A prepassed world draw only shades existing coverage. In particular,
          // forced early tests must not write depth before its cutout discard.
          // Other cameras and shadow/depth passes retain their own depth writes.
          .depthWriteEnable = !terrain && !(mode == Pass::Opaque && options.depthPrepass),
          .depthCompareOp = composite ? VK_COMPARE_OP_ALWAYS : VK_COMPARE_OP_LESS_OR_EQUAL};
      const VkPipelineColorBlendAttachmentState attachment{
          .blendEnable = composite || terrain,
          .srcColorBlendFactor = terrain ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ONE,
          .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
          .colorBlendOp = VK_BLEND_OP_ADD,
          .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
          .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
          .alphaBlendOp = VK_BLEND_OP_ADD,
          .colorWriteMask = depthOnly ? 0u : 15u};
      const VkPipelineColorBlendAttachmentState attachments[] = {attachment, attachment, attachment, attachment};
      const VkPipelineColorBlendStateCreateInfo blend{
          .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
          .attachmentCount = shadow                                ? 0u
                             : !composite && !viewModel && options.autoExposure ? 4u
                             : composite || terrain || viewModel || !techniquePlan.reflectionResolve ? 1u
                                                                   : 3u,
          .pAttachments = shadow ? nullptr : attachments};
      const VkGraphicsPipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                              .stageCount = 2,
                                              .pStages = stages,
                                              .pVertexInputState = batchedDepth ? &batchInput : composite ? &fullscreenInput
                                                                   : vs == lightmapVertex ? &mappedInput
                                                                                          : &vertexInput,
                                              .pInputAssemblyState = &assembly,
                                              .pViewportState = &viewport,
                                              .pRasterizationState = &raster,
                                              .pMultisampleState = &multisample,
                                              .pDepthStencilState = &depthState,
                                              .pColorBlendState = &blend,
                                              .pDynamicState = &dynamic,
                                              .layout = composite          ? compositeLayout
                                                        : fs == giFragment || fs == giTerrainFragment ||
                                                          fs == giViewModelFragment || fs == giViewModelEarlyFragment ? giLayout
                                                                           : layout,
                                              .renderPass = pass,
                                              .subpass = terrain ? 1u : 0u};
      VkPipeline result;
      check(pipeline::createGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &result));
      return result;
    };
    opaquePipeline = graphics(vertex, fragment, worldPass);
    const auto viewModelFragment = reflectionModule(viewModelFragShader, viewModelFragCubeShader);
    viewModelPipeline = graphics(vertex, viewModelFragment, viewModelTarget->pass(), Pass::ViewModel);
    if (giViewModelFragment)
      giViewModelPipeline = graphics(vertex, giViewModelFragment, viewModelTarget->pass(), Pass::ViewModel);
    if (!std::getenv("REMIX_DEBUG_DISABLE_VIEWMODEL_EARLY_DEPTH")) {
      const auto early = reflectionModule(viewModelEarlyFragShader, viewModelEarlyFragCubeShader);
      viewModelEarlyPipeline = graphics(vertex, early, viewModelTarget->pass(), Pass::ViewModel);
      if (giViewModelEarlyFragment)
        giViewModelEarlyPipeline = graphics(vertex, giViewModelEarlyFragment, viewModelTarget->pass(), Pass::ViewModel);
    }
    lightmapPipeline = graphics(lightmapVertex, fragment, worldPass, Pass::Opaque, Coverage::Mapped);
    if (!techniquePlan.rayDiffuse() && bake::directionalFieldsRequested() &&
        !std::getenv("REMIX_DEBUG_DISABLE_INDEXED_WORLD_PIPELINE"))
      indexedLightmapPipeline = graphics(lightmapVertex, fragment, worldPass, Pass::Opaque, Coverage::Mapped, true);
    if (indexedLightmapPipeline && largeOpaqueTarget() &&
        !std::getenv("REMIX_DEBUG_DISABLE_INDEXED_ROUGH_DIELECTRIC"))
      indexedRoughDielectricPipeline =
          graphics(lightmapVertex, fragment, worldPass, Pass::Opaque, Coverage::Mapped, true, true);
    partialLightmapPipeline = graphics(lightmapVertex, fragment, worldPass);
    unmappedLightmapPipeline = graphics(lightmapVertex, fragment, worldPass, Pass::Opaque, Coverage::Unmapped);
    depthPipeline = graphics(vertex, depthFragment, worldPass, Pass::Depth);
    depthLightmapPipeline = graphics(lightmapVertex, depthFragment, worldPass, Pass::Depth);
    if (options.staticDepthBatch) {
      const auto batchVertex = modules.add(depthBatchVertShader, sizeof(depthBatchVertShader));
      const auto batchFragment = modules.add(depthBatchFragShader, sizeof(depthBatchFragShader));
      depthBatchPipeline = graphics(batchVertex, batchFragment, worldPass, Pass::DepthBatch);
    }
    terrainPipeline = graphics(vertex, terrainFragment, worldPass, Pass::Terrain);
    terrainLightmapPipeline = graphics(lightmapVertex, terrainFragment, worldPass, Pass::Terrain, Coverage::Mapped);
    terrainPartialLightmapPipeline = graphics(lightmapVertex, terrainFragment, worldPass, Pass::Terrain);
    if (techniquePlan.rayDiffuse()) {
      giOpaquePipeline = graphics(vertex, giFragment, worldPass);
      giLightmapPipeline = graphics(lightmapVertex, giFragment, worldPass);
      giTerrainPipeline = graphics(vertex, giTerrainFragment, worldPass, Pass::Terrain);
      giTerrainLightmapPipeline = graphics(lightmapVertex, giTerrainFragment, worldPass, Pass::Terrain);
    }
    shadowPipeline = graphics(shadowVertex, shadowFragment, shadowPass, Pass::Shadow);
    localShadowPipeline = graphics(localVertex, localFragment, shadowPass, Pass::Shadow);
    compositePipeline = graphics(compositeVertex, compositeFragment, output, Pass::Composite);
    const VkComputePipelineCreateInfo computeInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = compute,
                  .pName = "main"},
        .layout = layout};
    check(
        pipeline::createComputePipelines(device, VK_NULL_HANDLE, 1, &computeInfo, nullptr, &clusterPipeline));
  }
} // namespace remix::forward

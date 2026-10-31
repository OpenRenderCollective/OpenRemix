#include "pathtrace/gpu.h"
#include "runtime/pipelines.h"
#include <algorithm>

namespace remix {
using namespace trace::detail;

// Variants specialize split lighting, a dispatch's role (separate indirect
// pass, sparse launches, ray profile) and the shadow early-out setting: each
// is a 20-45 s compile, so only those stay variants, and the ones the
// settings select are prewarmed (prewarmLightingVariants). The light
// sampler's structure (local lights, guiding, the small set, resampling) and
// the remaining switches travel as push constants (bindTrace): as variants
// they trimmed code, not registers or spills, and a scene's lights recompiled
// every stage. Variants are lazy, cached, and share the source pipeline's
// fence/layout lifetime.
VkPipeline PathTracer::Gpu::selectLightingPipeline(VkPipeline pipeline, bool combinedDirect, bool separateIndirect, uint32_t sparse) {
  selectedLightingKey = UINT32_MAX;
  selectedFeatures = defaultFeatures;
  selectedLayout = layout;
  auto found = lightingPipelines.find(pipeline);
  if (found == lightingPipelines.end())
    return separateIndirect || sparse ? VK_NULL_HANDLE : pipeline;
  auto &source = found->second;
  selectedLayout = source.layout;
  // The small set's sampler costs a transparency pass 255 registers: only
  // the opaque (guiding-capable) passes take it.
  const bool smallSet = localIlluminationActive && source.supportsGuiding && lightBvh.nodes().size() >= 3 && lightBvh.nodes().size() <= 7;
  const bool guided = localGuidingActive && source.supportsGuiding;
  const bool resampling = localResamplingActive && source.supportsGuiding;
  const bool split = splitLightingActive && source.supportsGuiding && !combinedDirect;
  const bool angular = localIlluminationActive && localAngularActive && source.supportsGuiding;
  const bool firstHit = localFirstHitActive && source.supportsGuiding && source.preferFirstHit;
  const bool roulette = rouletteActive && source.supportsRoulette;
  // rtx.native.pathtrace.shadowEarlyOut; off keeps the existing variants.
  const bool earlyOut = shadowEarlyOutActive;
  selectedFeatures = (decalMaterialsActive ? featureDecals : 0u) | (roulette ? featureRoulette : 0u) |
                     (pbrReuse.active ? featurePbrReuse : 0u) | (angular ? featureAngularEmitters : 0u) |
                     (firstHit ? featureFirstOpaqueHit : 0u) | (localIlluminationActive ? featureLocalLights : 0u) |
                     (guided ? featureLocalGuiding : 0u) | (smallSet ? featureSmallSet : 0u) |
                     (resampling ? featureResampling : 0u);
  // Combined mode also uses the split opaque pipeline in raw rendering.
  const uint32_t key = (split ? variantSplit : 0u) | (combinedDirect ? variantCombinedDirect : 0u) |
                       (rayProfile.path ? variantProfile : 0u) | (separateIndirect ? variantSeparateIndirect : 0u) |
                       (earlyOut ? variantEarlyOut : 0u) | sparse << variantSparseShift;
  selectedLightingKey = key;
  // The source itself: its own early-out and indirect role.
  if (key == ((source.baseEarlyOut ? variantEarlyOut : 0u) | (source.baseSeparateIndirect ? variantSeparateIndirect : 0u)))
    return pipeline;
  auto &[variant, attempted] = source.specialized[key];
  if (!attempted) {
    attempted = true;
    const auto result = requestLightingVariant(source, key, &variant, false);
    if (result == VK_NOT_READY) {
      attempted = false;
      // A consumer source stands in for its own variants (plain sampling);
      // a producer-dependent role without one has no fallback.
      return sparse || (separateIndirect && !source.baseSeparateIndirect) ? VK_NULL_HANDLE : pipeline;
    }
    // A failed specialization keeps the source pipeline (below).
    if (result == VK_ERROR_DEVICE_LOST)
      throw result;
  }
  // A split consumer must never silently execute the inline fallback after
  // its producer has run (duplicate tracing and cache/profile side effects).
  if ((sparse || (separateIndirect && !source.baseSeparateIndirect)) && !variant) return VK_NULL_HANDLE;
  return variant ? variant : pipeline;
}

VkResult PathTracer::Gpu::requestLightingVariant(LightingPipeline &source, uint32_t key, VkPipeline *out,
                                                 bool prefetch) {
  VkShaderModuleCreateInfo ci{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                              .codeSize = source.bytes,
                              .pCode = source.code};
  VkShaderModule module{};
  auto result = pipeline::createShaderModule(device, &ci, nullptr, &module);
  if (result != VK_SUCCESS)
    return result;
  const auto bit = [key](uint32_t mask) { return VkBool32((key & mask) != 0); };
  const VkBool32 enabled[] = {VkBool32(source.supportsGuiding), bit(variantSplit), bit(variantProfile),
                              bit(variantSeparateIndirect), bit(variantEarlyOut), key >> variantSparseShift};
  const VkSpecializationMapEntry entries[] = {{1, 0, sizeof(VkBool32)}, {4, sizeof(VkBool32), sizeof(VkBool32)},
                                             {7, 2 * sizeof(VkBool32), sizeof(VkBool32)},
                                             {9, 3 * sizeof(VkBool32), sizeof(VkBool32)},
                                             {12, 4 * sizeof(VkBool32), sizeof(VkBool32)},
                                             {13, 5 * sizeof(VkBool32), sizeof(uint32_t)}};
  const VkSpecializationInfo specialization{6, entries, sizeof(enabled), enabled};
  const VkComputePipelineCreateInfo create{
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = module,
                .pName = "main",
                .pSpecializationInfo = &specialization},
      .layout = source.layout};
  if (prefetch)
    pipeline::prefetchComputePipelines(device, 1, &create, true);
  else
    result = pipeline::createComputePipelines(device, VK_NULL_HANDLE, 1, &create, nullptr, out);
  pipeline::destroyShaderModule(device, module, nullptr);
  return result;
}

void PathTracer::Gpu::prewarmLightingVariants(LightingPipeline &source, bool everyRole) {
  const auto &o = lightingOptions;
  const uint32_t base = (source.baseEarlyOut ? variantEarlyOut : 0u) |
                        (source.baseSeparateIndirect ? variantSeparateIndirect : 0u);
  const uint32_t earlyOut = (o.shadowEarlyOut ? variantEarlyOut : 0u) |
                            (source.baseSeparateIndirect ? variantSeparateIndirect : 0u);
  // The split direct pass runs split, or combined in mode 3; the opaque
  // passes split when the scene has emissive lights. Another configuration's
  // pipelines take every role: these options are the current configuration's
  // effective ones, and ray reconstruction clears modes 1 and 3.
  std::vector<uint32_t> roles{0u};
  if (everyRole && source.supportsGuiding)
    roles = source.splitDirect ? std::vector<uint32_t>{variantSplit, variantCombinedDirect}
                               : std::vector<uint32_t>{0u, variantSplit};
  else if (source.supportsGuiding && o.separateLocalLighting != 0)
    roles = source.splitDirect ? std::vector<uint32_t>{o.separateLocalLighting == 3 ? variantCombinedDirect : variantSplit}
                               : std::vector<uint32_t>{0u, variantSplit};
  for (const uint32_t role : roles) {
    const uint32_t key = earlyOut | role;
    if (key == base)
      continue;
    auto &[variant, attempted] = source.specialized[key];
    if (variant || attempted)
      continue;
    VkPipeline none{};
    requestLightingVariant(source, key, &none, true);
  }
}

void PathTracer::Gpu::bindTrace(VkCommandBuffer command, VkPipeline pipeline, VkPipelineLayout pipelineLayout,
                                uint32_t features) {
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, featuresOffset, sizeof(features), &features);
}

void PathTracer::Gpu::forgetLightingPipeline(VkPipeline pipeline) {
  const auto found = lightingPipelines.find(pipeline);
  if (found == lightingPipelines.end())
    return;
  for (const auto &[key, variant] : found->second.specialized)
    if (variant.first)
      pipeline::destroyPipeline(device, variant.first, nullptr);
  lightingPipelines.erase(found);
}
} // namespace remix

#include "denoise/gpu.h"
#include "denoise/pipeline_selection.h"
#include "runtime/pipelines.h"
#ifdef REMIX_HAS_NRD
#include "denoise/shaders/nrd_embedded.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace remix::denoise {
  namespace {
    void require(nrd::Result r) {
      if (r != nrd::Result::SUCCESS)
        throw std::runtime_error("NRD API rejected denoiser configuration");
    }
    VkFormat format(nrd::Format value) {
      switch (value) {
      case nrd::Format::R8_UNORM:
        return VK_FORMAT_R8_UNORM;
      case nrd::Format::R8_UINT:
        return VK_FORMAT_R8_UINT;
      case nrd::Format::R16_UINT:
        return VK_FORMAT_R16_UINT;
      case nrd::Format::R16_SFLOAT:
        return VK_FORMAT_R16_SFLOAT;
      case nrd::Format::R32_UINT:
        return VK_FORMAT_R32_UINT;
      case nrd::Format::RG8_UNORM:
        return VK_FORMAT_R8G8_UNORM;
      case nrd::Format::RGBA8_UNORM:
        return VK_FORMAT_R8G8B8A8_UNORM;
      case nrd::Format::R32_SFLOAT:
        return VK_FORMAT_R32_SFLOAT;
      case nrd::Format::RGBA16_SFLOAT:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
      case nrd::Format::R10_G10_B10_A2_UNORM:
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
      default:
        throw std::runtime_error("Unexpected NRD texture format");
      }
    }
  } // namespace
  void Renderer::Gpu::initializeNrd(NrdPass &pass, bool sigma, bool local, bool pbr) {
    pass.device = device;
    pass.sigma = sigma;
    pass.local = local;
    pass.pbr = pbr;
    // Indirect light is denoised as SH and resolved per pixel (denoise.comp):
    // legacy diffuse, and both lobes of PBR surfaces (with the direct local
    // light routed into them). The translucent front layer and the separate
    // direct local-light passes stay plain radiance.
    const nrd::DenoiserDesc denoiser{
        0, sigma ? nrd::Denoiser::SIGMA_SHADOW_TRANSLUCENCY
                 : pbr ? (pass.foreground ? nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR
                                          : nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR_SH)
                       : local ? nrd::Denoiser::REBLUR_DIFFUSE : nrd::Denoiser::REBLUR_DIFFUSE_SH};
    nrd::InstanceCreationDesc create{};
    create.denoisers = &denoiser;
    create.denoisersNum = 1;
    require(nrd::CreateInstance(create, pass.instance));
    const auto &desc = *nrd::GetInstanceDesc(*pass.instance);
    const auto &offset = nrd::GetLibraryDesc()->spirvBindingOffsets;
    if (desc.resourcesSpaceIndex != 0 || desc.constantBufferAndSamplersSpaceIndex != 1)
      throw std::runtime_error("Unexpected NRD register spaces");
    for (uint32_t i = 0; i < 2; ++i) {
      VkSamplerCreateInfo ci{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                             .magFilter = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                             .minFilter = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                             .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                             .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                             .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                             .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
      check(pipeline::createSampler(device, &ci, nullptr, &pass.samplers[i]));
    }
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (uint32_t i = 0; i < desc.samplersNum; ++i)
      bindings.push_back({offset.samplerOffset + desc.samplersBaseRegisterIndex + i,
                          VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &pass.samplers[i]});
    bindings.push_back({offset.constantBufferOffset + desc.constantBufferRegisterIndex,
                        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                       .bindingCount = uint32_t(bindings.size()),
                                       .pBindings = bindings.data()};
    check(pipeline::createDescriptorSetLayout(device, &ci, nullptr,
                                              &pass.commonLayout));
    pass.pipelines.resize(desc.pipelinesNum);
    for (uint32_t i = 0; i < desc.pipelinesNum; ++i) {
      const auto &source = desc.pipelines[i];
      auto &p = pass.pipelines[i];
      if (!pipelineRequired(source.shaderIdentifier, sigma, local, pbr,
                            validation, localReconstruction))
        continue;
      const Shader *shader = nullptr;
      for (const auto &s : shaders)
        if (!std::strcmp(source.shaderIdentifier, s.identifier))
          shader = &s;
      if (!shader)
        continue;
      bindings.clear();
      uint32_t t = 0, u = 0;
      for (uint32_t r = 0; r < source.resourceRangesNum; ++r) {
        const auto &range = source.resourceRanges[r];
        const bool storage = range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
        for (uint32_t k = 0; k < range.descriptorsNum; ++k)
          bindings.push_back(
              {desc.resourcesBaseRegisterIndex +
                   (storage ? offset.storageTextureAndBufferOffset + u++ : offset.textureOffset + t++),
               storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
               VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
      }
      ci.bindingCount = uint32_t(bindings.size());
      ci.pBindings = bindings.data();
      check(pipeline::createDescriptorSetLayout(device, &ci, nullptr,
                                                &p.resources));
      VkDescriptorSetLayout layouts[] = {p.resources, pass.commonLayout};
      VkPipelineLayoutCreateInfo li{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                    .setLayoutCount = 2,
                                    .pSetLayouts = layouts};
      check(pipeline::createPipelineLayout(device, &li, nullptr, &p.layout));
      p.pipeline = pipeline(p.layout, shader->code, shader->size);
    }
    const auto &pd = desc.descriptorPoolDesc;
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, pd.totalTexturesNum},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, pd.totalStorageTexturesNum},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, pd.setsMaxNum},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER, pd.setsMaxNum * 2}};
    VkDescriptorPoolCreateInfo pc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                  .maxSets = pd.setsMaxNum * 2,
                                  .poolSizeCount = 4,
                                  .pPoolSizes = sizes};
    check(vkCreateDescriptorPool(device, &pc, nullptr, &pass.nrdPool));
    for (unsigned i = 1; i < gpuFrameCount; ++i)
      check(vkCreateDescriptorPool(device, &pc, nullptr, &pass.frames[i].pool));
    const VkDeviceSize a = properties.limits.minUniformBufferOffsetAlignment;
    pass.constantStride = (desc.constantBufferMaxDataSize + a - 1) & ~(a - 1);
    pass.constants = buffer(pass.constantStride * pd.setsMaxNum);
    for (unsigned i = 1; i < gpuFrameCount; ++i)
      pass.frames[i].constants = buffer(pass.constantStride * pd.setsMaxNum);
    auto allocate = [&](auto &pool, const nrd::TextureDesc *textures, uint32_t count) {
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t d = textures[i].downsampleFactor;
        pool.push_back(
            texture(format(textures[i].format), {(extent.width + d - 1) / d, (extent.height + d - 1) / d}));
      }
    };
    allocate(pass.permanent, desc.permanentPool, desc.permanentPoolSize);
    allocate(pass.transient, desc.transientPool, desc.transientPoolSize);
  }
  void Renderer::Gpu::prepareNrd(NrdPass &pass, const scene::Matrix &projection, const scene::Matrix &previousProjection) {
    nrd::CommonSettings common{};
    std::copy(projection.begin(), projection.end(), common.viewToClipMatrix);
    std::copy(previousProjection.begin(), previousProjection.end(), common.viewToClipMatrixPrev);
    // Our projections include Vulkan's framebuffer Y flip. NRD reconstructs
    // positions and projects history using UV Y opposite to clip-space Y,
    // so undo that flip at this boundary for both frames. Motion and jitter
    // already use top-left image coordinates and must keep their signs.
    for (int column = 0; column < 4; ++column) {
      common.viewToClipMatrix[column * 4 + 1] *= -1;
      common.viewToClipMatrixPrev[column * 4 + 1] *= -1;
    }
    std::copy(frame.view.begin(), frame.view.end(), common.worldToViewMatrix);
    std::copy(frame.previousView.begin(), frame.previousView.end(), common.worldToViewMatrixPrev);
    for (int i = 0; i < 2; ++i) {
      common.cameraJitter[i] = frame.jitter[i];
      common.cameraJitterPrev[i] = frame.jitter[i + 2];
      uint16_t size = uint16_t(i ? extent.height : extent.width);
      common.resourceSize[i] = common.resourceSizePrev[i] = common.rectSize[i] = common.rectSizePrev[i] =
          size;
      common.motionVectorScale[i] = 1.f / size;
    }
    common.motionVectorScale[2] = 1;
    common.timeDeltaBetweenFrames = frame.timing[0];
    common.frameIndex = uint32_t(stats.renderedFrames);
    common.accumulationMode =
        (frame.flags[0] || ((pass.sigma || pass.foreground) && frame.shadow[0]) || !pass.wasActive)
            ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
    common.enableValidation = frame.timing[1] != 0;
    common.isHistoryConfidenceAvailable = !pass.sigma;
    common.enableValidation &= !pass.sigma && !pass.local && !pass.foreground;
    require(nrd::SetCommonSettings(*pass.instance, common));
    pass.wasActive = true;
    if (pass.sigma) {
      nrd::SigmaSettings settings{};
      std::copy_n(frame.sun, 3, settings.lightDirection);
      settings.maxStabilizedFrameNum = std::clamp(
          nrd::GetMaxAccumulatedFrameNum(nrd::SIGMA_DEFAULT_ACCUMULATION_TIME,
                                        1000.f / std::max(smoothedFrameMilliseconds, 1.f)),
          1u, nrd::SIGMA_MAX_HISTORY_FRAME_NUM);
      require(nrd::SetDenoiserSettings(*pass.instance, 0, &settings));
    } else {
      nrd::ReblurSettings settings{};
      // Indirect light: the legacy diffuse SH pass and the PBR SH pass. Their
      // settings follow NRD-Sample (GetDefaultReblurSettings and its per-frame
      // history lengths) unless a comment below says why not.
      const bool indirect = !pass.local && !pass.foreground;
      // Guide material IDs (pathtrace.comp): PBR dielectric 0, PBR metal 1,
      // legacy 2. NRD compares max(id, minMaterial): diffuse never mixes across
      // them (metals have none), specular mixes dielectrics with metals and
      // keeps legacy surfaces, which have no specular signal, out. The
      // translucent front layer has its own IDs (translucency.glsl), all apart.
      settings.minMaterialForDiffuse = 0;
      settings.minMaterialForSpecular = pass.foreground ? 0.f : 1.f;
      settings.fastHistoryClampingSigmaScale = 1.5f;
      if (indirect) {
        // The SG resolve regains normal detail, which allows more relaxed
        // normal weights in the filter.
        settings.lobeAngleFraction *= 1.333f;
      }
      if (frame.local[1]) {
        // Sparse torch samples need stronger outlier rejection in both the
        // direct signal and bounced SH, without increasing spatial blur.
        settings.fireflySuppressorMinRelativeScale = 1.5f;
        // Inverse-PDF light selection can produce isolated bright samples in
        // both direct radiance and bounced SH. NRD's prepass must be disabled
        // for this probabilistic signal: it otherwise expands outliers into
        // blobs before temporal accumulation and anti-firefly processing.
        // (Legacy SH and local passes; the PBR pass sets its own below.)
        settings.diffusePrepassBlurRadius = 0.f;
      }
      settings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::OFF;
      settings.hitDistanceParameters = {frame.hitDistance[0], frame.hitDistance[1], frame.hitDistance[2]};
      // Indirect light can take a set history (rtx.native.pathtrace.indirectHistoryFrames):
      // shorter follows lighting changes faster, longer hides more noise.
      settings.maxAccumulatedFrameNum =
          indirect && localTuning.indirectHistoryFrames
              ? std::min(localTuning.indirectHistoryFrames, nrd::REBLUR_MAX_HISTORY_FRAME_NUM)
              : std::clamp(nrd::GetMaxAccumulatedFrameNum(nrd::REBLUR_DEFAULT_ACCUMULATION_TIME,
                                                          1000.f / std::max(smoothedFrameMilliseconds, 1.f)),
                           1u, nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
      if (pass.pbr) {
        // PBR paths select one lobe per pixel (pathtrace.comp, primaryLobe):
        // the skipped lobe has hit distance 0, which REBLUR reconstructs from
        // its 3x3 neighbours, and NRD requires a prepass for such signals.
        // The radii stay below NRD's GI defaults (30 and 50 pixels): this
        // signal also carries direct local light (addPbrLocal), whose cast
        // shadows a wide prepass spreads.
        settings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_3X3;
        settings.diffusePrepassBlurRadius = 4.f;
        settings.specularPrepassBlurRadius = 8.f;
        if (localFireflyFilter && frame.local[1]) {
          // Local firefly filter: temporal accumulation and anti-firefly
          // receive the original radiance; the specular prepass only supplies
          // the hit distance used for motion tracking. Without a prepass the
          // signal must not be probabilistic, so lobe selection is off in this
          // mode (Renderer::begin, Frame::translucency[3]): every pixel then
          // has both lobes from one BRDF-mixture sample.
          settings.diffusePrepassBlurRadius = 0.f;
          settings.usePrepassOnlyForSpecularMotionEstimation = true;
        }
        settings.maxBlurRadius = 16.f;
        if ((frame.local[0] || pass.foreground) && frame.local[2])
          settings.maxAccumulatedFrameNum = std::min(settings.maxAccumulatedFrameNum, 8u);
      }
      if (pass.foreground) {
        // Direct light and the BRDF-mixture continuation produce both lobes at
        // every pixel; zero radiance is an estimator result, not a skipped lobe.
        settings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::OFF;
        // A small prepass averages valid light-selection estimates before
        // temporal outlier rejection. The wider acceptance range preserves
        // bright direct samples mixed into this surface's indirect signal.
        settings.diffusePrepassBlurRadius = 2.f;
        settings.specularPrepassBlurRadius = 4.f;
        settings.usePrepassOnlyForSpecularMotionEstimation = false;
        settings.fireflySuppressorMinRelativeScale = 3.f;
        settings.fastHistoryClampingSigmaScale = 3.f;
      }
      if (pass.local) {
        const bool responsive = split ? frame.families[pass.emissive ? 3 : 2] != 0 : frame.local[2] != 0;
        // Direct-light discontinuities are sharper than diffuse GI. Large GI
        // prepasses spread light across cast shadows on a single flat surface.
        settings.diffusePrepassBlurRadius = 0.f;
        settings.minBlurRadius = .5f;
        settings.maxBlurRadius = 8.f;
        settings.historyFixBasePixelStride = 2;
        settings.historyFixFrameNum = 1;
        settings.maxAccumulatedFrameNum =
            std::min(settings.maxAccumulatedFrameNum, responsive ? 2u : localTuning.historyFrames);
        settings.antilagSettings.luminanceSigmaScale = localTuning.antilagSigma;
        settings.antilagSettings.luminanceSensitivity = localTuning.antilagSensitivity;
        settings.maxFastAccumulatedFrameNum = 2;
        settings.maxStabilizedFrameNum = responsive ? 1u : 8u;
        if (split && frame.local[3] == 1) {
          // Complementary family selection leaves unsampled pixels. NRD's
          // probabilistic-sampling contract requires distance reconstruction
          // and a prepass; keep its radius small to protect cast shadow edges.
          settings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_3X3;
          settings.diffusePrepassBlurRadius = 4.f;
        }
      } else
        warmupFrames = std::max(32u, settings.maxAccumulatedFrameNum);
      if (indirect) {
        // A fast history five times shorter than the main one, stabilized
        // over the main history's length.
        settings.maxFastAccumulatedFrameNum = settings.maxAccumulatedFrameNum / 5;
        settings.maxStabilizedFrameNum = settings.maxAccumulatedFrameNum;
      }
      settings.maxFastAccumulatedFrameNum =
          std::min(settings.maxFastAccumulatedFrameNum, settings.maxAccumulatedFrameNum);
      require(nrd::SetDenoiserSettings(*pass.instance, 0, &settings));
    }
    const nrd::Identifier id = 0;
    require(nrd::GetComputeDispatches(*pass.instance, &id, 1, pass.dispatches, pass.dispatchCount));
    check(vkResetDescriptorPool(device, pass.nrdPool, 0));
  }
  void Renderer::Gpu::recordNrd(NrdPass &pass, VkCommandBuffer command) {
    const auto &desc = *nrd::GetInstanceDesc(*pass.instance);
    const auto &offset = nrd::GetLibraryDesc()->spirvBindingOffsets;
    if (pass.dispatchCount > desc.descriptorPoolDesc.setsMaxNum)
      throw std::runtime_error("NRD dispatch limit exceeded");
    auto resolve = [&](const nrd::ResourceDesc &r) -> Texture & {
      using R = nrd::ResourceType;
      switch (r.type) {
      case R::PERMANENT_POOL:
        return *pass.permanent.at(r.indexInPool);
      case R::TRANSIENT_POOL:
        return *pass.transient.at(r.indexInPool);
      case R::IN_PENUMBRA:
        return *images[Penumbra];
      case R::IN_TRANSLUCENCY:
        return *images[ShadowInput];
      case R::OUT_SHADOW_TRANSLUCENCY:
        return *images[ShadowOutput];
      case R::IN_MV:
        return *images[pass.foreground ? FrontMotion : Motion];
      case R::IN_NORMAL_ROUGHNESS:
        return *images[pass.foreground ? FrontNormal : Normal];
      case R::IN_VIEWZ:
        return *images[pass.foreground ? FrontDepth : Depth];
      case R::IN_DIFF_RADIANCE_HITDIST:
        return *images[pass.foreground ? FrontDiffuse : pass.pbr ? Sh0 : pass.emissive ? EmissiveInput : LocalInput];
      case R::OUT_DIFF_RADIANCE_HITDIST:
        return *images[pass.foreground ? FrontDiffuseOutput : pass.pbr ? PbrDiffuseOutput : pass.emissive ? EmissiveOutput : LocalOutput];
      case R::IN_SPEC_RADIANCE_HITDIST:
        return *images[pass.foreground ? FrontSpecular : PbrSpecularInput];
      case R::OUT_SPEC_RADIANCE_HITDIST:
        return *images[pass.foreground ? FrontSpecularOutput : PbrSpecularOutput];
      case R::IN_DIFF_SH0:
        return *images[Sh0];
      case R::IN_DIFF_SH1:
        return *images[Sh1];
      case R::IN_SPEC_SH0:
        return *images[PbrSpecularInput];
      case R::IN_SPEC_SH1:
        return *images[PbrSpecularSh1];
      case R::IN_DIFF_CONFIDENCE:
      case R::IN_SPEC_CONFIDENCE:
        return *images[pass.foreground ? FrontConfidence : pass.local ? LocalConfidence : Confidence];
      case R::OUT_DIFF_SH0:
        return *images[pass.pbr ? PbrDiffuseOutput : Out0];
      case R::OUT_DIFF_SH1:
        return *images[pass.pbr ? PbrDiffuseOutputSh1 : Out1];
      case R::OUT_SPEC_SH0:
        return *images[PbrSpecularOutput];
      case R::OUT_SPEC_SH1:
        return *images[PbrSpecularOutputSh1];
      case R::OUT_VALIDATION:
        return *images[Validation];
      default:
        throw std::runtime_error("Unbound NRD resource");
      }
    };
    for (uint32_t i = 0; i < pass.dispatchCount; ++i) {
      const auto &d = pass.dispatches[i];
      const auto &p = pass.pipelines.at(d.pipelineIndex);
      if (!p.pipeline)
        throw std::runtime_error("Uncompiled NRD shader requested");
      VkDescriptorSetLayout layouts[] = {p.resources, pass.commonLayout};
      VkDescriptorSet sets[2];
      VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                     .descriptorPool = pass.nrdPool,
                                     .descriptorSetCount = 2,
                                     .pSetLayouts = layouts};
      check(vkAllocateDescriptorSets(device, &ai, sets));
      std::vector<VkDescriptorImageInfo> imagesInfo(d.resourcesNum);
      std::vector<VkWriteDescriptorSet> writes;
      uint32_t t = 0, u = 0;
      for (uint32_t r = 0; r < d.resourcesNum; ++r) {
        const auto &resource = d.resources[r];
        const bool storage = resource.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
        imagesInfo[r] = {VK_NULL_HANDLE, resolve(resource).view, VK_IMAGE_LAYOUT_GENERAL};
        writes.push_back(
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = sets[0],
             .dstBinding =
                 desc.resourcesBaseRegisterIndex +
                 (storage ? offset.storageTextureAndBufferOffset + u++ : offset.textureOffset + t++),
             .descriptorCount = 1,
             .descriptorType = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
             .pImageInfo = &imagesInfo[r]});
      }
      if (d.constantBufferDataSize > pass.constantStride)
        throw std::runtime_error("NRD constant limit exceeded");
      const auto position = pass.constantStride * i;
      // Every dispatch has its own descriptors and constants until the frame fence.
      if (d.constantBufferDataSize)
        std::memcpy(static_cast<char *>(pass.constants->mapped) + position, d.constantBufferData,
                    d.constantBufferDataSize);
      VkDescriptorBufferInfo bi{pass.constants->buffer, position, desc.constantBufferMaxDataSize};
      writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = sets[1],
                        .dstBinding = offset.constantBufferOffset + desc.constantBufferRegisterIndex,
                        .descriptorCount = 1,
                        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                        .pBufferInfo = &bi});
      vkUpdateDescriptorSets(device, uint32_t(writes.size()), writes.data(), 0, nullptr);
      synchronize(command);
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 2, sets, 0, nullptr);
      vkCmdDispatch(command, d.gridWidth, d.gridHeight, 1);
    }
  }
} // namespace remix::denoise
#endif

#include "upscale/neural_uplift.h"
#include "upscale/nvsdk_ngx_defs_dlssnr.h"
#include "upscale/shaders/neural_uplift_prep.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <SDL3/SDL_log.h>

#if REMIX_HAS_DLSS5
#include <nvsdk_ngx_helpers_vk.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace remix::upscale {

  namespace {
#if REMIX_HAS_DLSS5
    static const wchar_t *const kNeuralUpliftLibraryPaths[] = {
      L"openremix\\dlss\\nvngx_dlssnr.dll",
      L".trex\\nvngx_dlssnr.dll",
      L"bin\\.trex\\nvngx_dlssnr.dll",
      L".trex\\bin\\nvngx_dlssnr.dll",
      L"dlss\\310.9.1\\nvngx_dlssnr.dll",
      L"nvngx_dlssnr.dll",
    };

    std::filesystem::path moduleDirectory() {
#ifdef _WIN32
      HMODULE module{};
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&moduleDirectory), &module))
        return {};
      std::wstring path(32768, L'\0');
      const auto size = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
      if (!size || size == path.size())
        return {};
      path.resize(size);
      return std::filesystem::path(path).parent_path();
#else
      return {};
#endif
    }

#ifdef _WIN32
    using PFN_GetModuleFileNameW = DWORD (WINAPI*)(HMODULE, LPWSTR, DWORD);
    PFN_GetModuleFileNameW g_realGetModuleFileNameW = nullptr;
    HMODULE g_spoofedModule = nullptr;
    constexpr DWORD kNvngxFileNameLength = 9;

    DWORD WINAPI neuralUpliftGetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize) {
      if (nSize != 0 && lpFilename != nullptr && hModule == g_spoofedModule && g_spoofedModule != nullptr) {
        if (nSize <= kNvngxFileNameLength) {
          lpFilename[0] = L'\0';
          SetLastError(ERROR_INSUFFICIENT_BUFFER);
          return nSize;
        }
        memcpy(lpFilename, L"nvngx.dll", (kNvngxFileNameLength + 1) * sizeof(wchar_t));
        return kNvngxFileNameLength;
      }
      return g_realGetModuleFileNameW(hModule, lpFilename, nSize);
    }

    void **hookNeuralUpliftGetModuleFileName(HMODULE snippet) {
      auto *const base = reinterpret_cast<uint8_t *>(snippet);
      const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
      if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
      }

      constexpr LONG kHeaderWindow = 0x1000;
      if (dos->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)) ||
          dos->e_lfanew > kHeaderWindow - static_cast<LONG>(sizeof(IMAGE_NT_HEADERS))) {
        return nullptr;
      }

      const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
      if (nt->Signature != IMAGE_NT_SIGNATURE ||
          nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC) {
        return nullptr;
      }

      const uint32_t imageSize = nt->OptionalHeader.SizeOfImage;
      const auto rvaFits = [imageSize](uint32_t rva, size_t size) {
        return rva != 0 && size <= imageSize && rva <= imageSize - size;
      };

      if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) {
        return nullptr;
      }

      const IMAGE_DATA_DIRECTORY &importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
      if (!rvaFits(importDir.VirtualAddress, sizeof(IMAGE_IMPORT_DESCRIPTOR))) {
        return nullptr;
      }

      for (uint32_t descriptorRva = importDir.VirtualAddress;
           rvaFits(descriptorRva, sizeof(IMAGE_IMPORT_DESCRIPTOR));
           descriptorRva += static_cast<uint32_t>(sizeof(IMAGE_IMPORT_DESCRIPTOR))) {
        const auto *descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base + descriptorRva);
        if (descriptor->FirstThunk == 0 && descriptor->OriginalFirstThunk == 0) {
          break;
        }
        if (descriptor->OriginalFirstThunk == 0 || descriptor->FirstThunk == 0) {
          continue;
        }

        for (uint64_t thunkOffset = 0;; thunkOffset += sizeof(IMAGE_THUNK_DATA)) {
          const uint64_t nameThunkRva = descriptor->OriginalFirstThunk + thunkOffset;
          if (nameThunkRva > imageSize || !rvaFits(static_cast<uint32_t>(nameThunkRva), sizeof(IMAGE_THUNK_DATA))) {
            break;
          }

          const auto *nameThunk = reinterpret_cast<const IMAGE_THUNK_DATA *>(base + nameThunkRva);
          if (nameThunk->u1.AddressOfData == 0) {
            break;
          }

          if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal)) {
            continue;
          }

          static constexpr char kImportName[] = "GetModuleFileNameW";
          const uint64_t nameRva = nameThunk->u1.AddressOfData + offsetof(IMAGE_IMPORT_BY_NAME, Name);
          if (nameRva + sizeof(kImportName) > imageSize) {
            continue;
          }

          if (memcmp(base + nameRva, kImportName, sizeof(kImportName)) != 0) {
            continue;
          }

          const uint64_t slotRva = descriptor->FirstThunk + thunkOffset;
          if (slotRva > imageSize || !rvaFits(static_cast<uint32_t>(slotRva), sizeof(void *))) {
            return nullptr;
          }

          auto **slot = reinterpret_cast<void **>(base + slotRva);
          HMODULE ourModule = nullptr;
          if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(&neuralUpliftGetModuleFileNameW),
                                  &ourModule) || ourModule == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[DLSS-NR] Could not identify this module; caller check left in place");
            return nullptr;
          }

          if (*slot == reinterpret_cast<void *>(&neuralUpliftGetModuleFileNameW)) {
            return nullptr;
          }

          DWORD oldProtect = 0;
          if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &oldProtect)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[DLSS-NR] Could not unprotect snippet IAT; caller check left in place");
            return nullptr;
          }

          g_realGetModuleFileNameW = reinterpret_cast<PFN_GetModuleFileNameW>(*slot);
          g_spoofedModule = ourModule;
          *slot = reinterpret_cast<void *>(&neuralUpliftGetModuleFileNameW);

          VirtualProtect(slot, sizeof(void *), oldProtect, &oldProtect);
          FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));

          SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[DLSS-NR] Caller check bypassed: GetModuleFileNameW hooked");
          return slot;
        }
      }

      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[DLSS-NR] The snippet does not import GetModuleFileNameW");
      return nullptr;
    }

    void unhookNeuralUpliftGetModuleFileName(void **slot) {
      if (slot == nullptr || g_realGetModuleFileNameW == nullptr) {
        return;
      }
      DWORD oldProtect = 0;
      if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &oldProtect)) {
        return;
      }
      *slot = reinterpret_cast<void *>(g_realGetModuleFileNameW);
      VirtualProtect(slot, sizeof(void *), oldProtect, &oldProtect);
      FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
      g_realGetModuleFileNameW = nullptr;
      g_spoofedModule = nullptr;
    }
#endif // _WIN32
#endif // REMIX_HAS_DLSS5
  } // namespace

  struct NeuralUplift::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkExtent2D displayExtent = {};
    VkExtent2D createdExtent = {};
    uint32_t createdPasses = 1;

    NeuralUpliftSettings currentSettings;
    NeuralUpliftStatistics stats;

    bool initialized = false;
    bool snippetInitialized = false;
    bool forceHistoryReset = false;

    // Intermediate color image for staging in-place evaluation
    VkImage stagingImage = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkImageView stagingView = VK_NULL_HANDLE;
    VkExtent2D stagingExtent = {};

#if REMIX_HAS_DLSS5
    void *module = nullptr;
    void *callerCheckHookSlot = nullptr;

    using PFN_CreateFeature1 = NVSDK_NGX_Result (NVSDK_CONV *)(VkDevice, VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    using PFN_EvaluateFeature = NVSDK_NGX_Result (NVSDK_CONV *)(VkCommandBuffer, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);
    using PFN_ReleaseFeature = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Handle*);
    using PFN_Init_Ext2 = NVSDK_NGX_Result (NVSDK_CONV *)(unsigned long long, const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice, PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, const NVSDK_NGX_FeatureCommonInfo*, NVSDK_NGX_Version);
    using PFN_Shutdown1 = NVSDK_NGX_Result (NVSDK_CONV *)(VkDevice);

    PFN_CreateFeature1 pfnCreateFeature1 = nullptr;
    PFN_EvaluateFeature pfnEvaluateFeature = nullptr;
    PFN_ReleaseFeature pfnReleaseFeature = nullptr;
    PFN_Init_Ext2 pfnInit_Ext2 = nullptr;
    PFN_Shutdown1 pfnShutdown1 = nullptr;

    NVSDK_NGX_Parameter *parameters = nullptr;
    std::vector<NVSDK_NGX_Handle *> features;

    NVSDK_NGX_Handle *featureForPass(uint32_t passIndex) const {
      if (features.empty())
        return nullptr;
      return features[std::min<size_t>(passIndex, features.size() - 1)];
    }

    void destroyStaging() {
      if (device) {
        if (stagingView) {
          vkDestroyImageView(device, stagingView, nullptr);
          stagingView = VK_NULL_HANDLE;
        }
        if (stagingImage) {
          vkDestroyImage(device, stagingImage, nullptr);
          stagingImage = VK_NULL_HANDLE;
        }
        if (stagingMemory) {
          vkFreeMemory(device, stagingMemory, nullptr);
          stagingMemory = VK_NULL_HANDLE;
        }
      }
      stagingExtent = {};
    }

    static constexpr uint32_t kFrames = 2;
    uint32_t frameSlot = 0;

    VkImage preparedDepthImages[kFrames] = {};
    VkDeviceMemory preparedDepthMemories[kFrames] = {};
    VkImageView preparedDepthViews[kFrames] = {};
    VkExtent2D preparedDepthExtent = {};

    VkImage preparedMotionImages[kFrames] = {};
    VkDeviceMemory preparedMotionMemories[kFrames] = {};
    VkImageView preparedMotionViews[kFrames] = {};
    VkExtent2D preparedMotionExtent = {};

    VkDescriptorSetLayout prepSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool prepPool = VK_NULL_HANDLE;
    VkDescriptorSet prepSets[kFrames] = {};
    VkPipelineLayout prepPipelineLayout = VK_NULL_HANDLE;
    VkPipeline prepPipeline = VK_NULL_HANDLE;
    VkShaderModule prepShaderModule = VK_NULL_HANDLE;

    void destroyPrepared() {
      if (device) {
        for (uint32_t f = 0; f < kFrames; ++f) {
          if (preparedDepthViews[f]) {
            vkDestroyImageView(device, preparedDepthViews[f], nullptr);
            preparedDepthViews[f] = VK_NULL_HANDLE;
          }
          if (preparedDepthImages[f]) {
            vkDestroyImage(device, preparedDepthImages[f], nullptr);
            preparedDepthImages[f] = VK_NULL_HANDLE;
          }
          if (preparedDepthMemories[f]) {
            vkFreeMemory(device, preparedDepthMemories[f], nullptr);
            preparedDepthMemories[f] = VK_NULL_HANDLE;
          }
          if (preparedMotionViews[f]) {
            vkDestroyImageView(device, preparedMotionViews[f], nullptr);
            preparedMotionViews[f] = VK_NULL_HANDLE;
          }
          if (preparedMotionImages[f]) {
            vkDestroyImage(device, preparedMotionImages[f], nullptr);
            preparedMotionImages[f] = VK_NULL_HANDLE;
          }
          if (preparedMotionMemories[f]) {
            vkFreeMemory(device, preparedMotionMemories[f], nullptr);
            preparedMotionMemories[f] = VK_NULL_HANDLE;
          }
        }
      }
      preparedDepthExtent = {};
      preparedMotionExtent = {};
    }

    void destroyPrepPipeline() {
      if (device) {
        if (prepPipeline) {
          vkDestroyPipeline(device, prepPipeline, nullptr);
          prepPipeline = VK_NULL_HANDLE;
        }
        if (prepPipelineLayout) {
          vkDestroyPipelineLayout(device, prepPipelineLayout, nullptr);
          prepPipelineLayout = VK_NULL_HANDLE;
        }
        if (prepShaderModule) {
          vkDestroyShaderModule(device, prepShaderModule, nullptr);
          prepShaderModule = VK_NULL_HANDLE;
        }
        if (prepPool) {
          vkDestroyDescriptorPool(device, prepPool, nullptr);
          prepPool = VK_NULL_HANDLE;
        }
        if (prepSetLayout) {
          vkDestroyDescriptorSetLayout(device, prepSetLayout, nullptr);
          prepSetLayout = VK_NULL_HANDLE;
        }
        for (uint32_t f = 0; f < kFrames; ++f) {
          prepSets[f] = VK_NULL_HANDLE;
        }
      }
    }

    void createPreparedImages(VkExtent2D extent) {
      if (preparedDepthImages[0] && preparedDepthExtent.width == extent.width && preparedDepthExtent.height == extent.height)
        return;
      destroyPrepared();

      VkPhysicalDeviceMemoryProperties memProps;
      vkGetPhysicalDeviceMemoryProperties(physical, &memProps);

      for (uint32_t f = 0; f < kFrames; ++f) {
        VkImageCreateInfo depthInfo{
          .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
          .imageType = VK_IMAGE_TYPE_2D,
          .format = VK_FORMAT_R32_SFLOAT,
          .extent = {extent.width, extent.height, 1},
          .mipLevels = 1,
          .arrayLayers = 1,
          .samples = VK_SAMPLE_COUNT_1_BIT,
          .tiling = VK_IMAGE_TILING_OPTIMAL,
          .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
          .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        if (vkCreateImage(device, &depthInfo, nullptr, &preparedDepthImages[f]) != VK_SUCCESS)
          return;

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, preparedDepthImages[f], &req);
        uint32_t typeIndex = UINT32_MAX;
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
          if ((req.memoryTypeBits & (1u << i)) &&
              (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            typeIndex = i;
            break;
          }
        }
        if (typeIndex == UINT32_MAX) {
          destroyPrepared();
          return;
        }

        VkMemoryAllocateInfo allocInfo{
          .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
          .allocationSize = req.size,
          .memoryTypeIndex = typeIndex,
        };
        if (vkAllocateMemory(device, &allocInfo, nullptr, &preparedDepthMemories[f]) != VK_SUCCESS) {
          destroyPrepared();
          return;
        }
        vkBindImageMemory(device, preparedDepthImages[f], preparedDepthMemories[f], 0);

        VkImageViewCreateInfo depthViewInfo{
          .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
          .image = preparedDepthImages[f],
          .viewType = VK_IMAGE_VIEW_TYPE_2D,
          .format = VK_FORMAT_R32_SFLOAT,
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        if (vkCreateImageView(device, &depthViewInfo, nullptr, &preparedDepthViews[f]) != VK_SUCCESS) {
          destroyPrepared();
          return;
        }

        VkImageCreateInfo motionInfo{
          .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
          .imageType = VK_IMAGE_TYPE_2D,
          .format = VK_FORMAT_R32G32_SFLOAT,
          .extent = {extent.width, extent.height, 1},
          .mipLevels = 1,
          .arrayLayers = 1,
          .samples = VK_SAMPLE_COUNT_1_BIT,
          .tiling = VK_IMAGE_TILING_OPTIMAL,
          .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
          .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        if (vkCreateImage(device, &motionInfo, nullptr, &preparedMotionImages[f]) != VK_SUCCESS) {
          destroyPrepared();
          return;
        }
        vkGetImageMemoryRequirements(device, preparedMotionImages[f], &req);
        typeIndex = UINT32_MAX;
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
          if ((req.memoryTypeBits & (1u << i)) &&
              (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            typeIndex = i;
            break;
          }
        }
        if (typeIndex == UINT32_MAX) {
          destroyPrepared();
          return;
        }
        allocInfo.allocationSize = req.size;
        allocInfo.memoryTypeIndex = typeIndex;
        if (vkAllocateMemory(device, &allocInfo, nullptr, &preparedMotionMemories[f]) != VK_SUCCESS) {
          destroyPrepared();
          return;
        }
        vkBindImageMemory(device, preparedMotionImages[f], preparedMotionMemories[f], 0);

        VkImageViewCreateInfo motionViewInfo{
          .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
          .image = preparedMotionImages[f],
          .viewType = VK_IMAGE_VIEW_TYPE_2D,
          .format = VK_FORMAT_R32G32_SFLOAT,
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        if (vkCreateImageView(device, &motionViewInfo, nullptr, &preparedMotionViews[f]) != VK_SUCCESS) {
          destroyPrepared();
          return;
        }
      }

      preparedDepthExtent = extent;
      preparedMotionExtent = extent;
    }

    void createPrepPipeline() {
      if (prepPipeline)
        return;

      VkDescriptorSetLayoutBinding bindings[4]{};
      for (uint32_t i = 0; i < 4; ++i) {
        bindings[i] = {
          .binding = i,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
          .pImmutableSamplers = nullptr,
        };
      }
      VkDescriptorSetLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 4,
        .pBindings = bindings,
      };
      if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &prepSetLayout) != VK_SUCCESS)
        return;

      VkDescriptorPoolSize poolSize{
        .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .descriptorCount = 4 * kFrames,
      };
      VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = kFrames,
        .poolSizeCount = 1,
        .pPoolSizes = &poolSize,
      };
      if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &prepPool) != VK_SUCCESS)
        return;

      VkDescriptorSetLayout layouts[kFrames] = {prepSetLayout, prepSetLayout};
      VkDescriptorSetAllocateInfo setInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = prepPool,
        .descriptorSetCount = kFrames,
        .pSetLayouts = layouts,
      };
      if (vkAllocateDescriptorSets(device, &setInfo, prepSets) != VK_SUCCESS)
        return;

      VkPushConstantRange pushRange{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(float) * 16 + sizeof(uint32_t),
      };
      VkPipelineLayoutCreateInfo pipeLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &prepSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushRange,
      };
      if (vkCreatePipelineLayout(device, &pipeLayoutInfo, nullptr, &prepPipelineLayout) != VK_SUCCESS)
        return;

      VkShaderModuleCreateInfo moduleInfo{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(prepCompShader),
        .pCode = prepCompShader,
      };
      if (vkCreateShaderModule(device, &moduleInfo, nullptr, &prepShaderModule) != VK_SUCCESS)
        return;

      VkComputePipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_COMPUTE_BIT,
          .module = prepShaderModule,
          .pName = "main",
        },
        .layout = prepPipelineLayout,
      };
      vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &prepPipeline);
    }

    int32_t prepareInputs(VkCommandBuffer command,
                          VkImage linearDepth, VkImageView linearDepthView,
                          VkImage guideMotion, VkImageView guideMotionView,
                          VkExtent2D extent, const float *projection,
                          bool depthInverted) {
      if (!device || !linearDepth || !linearDepthView || !guideMotion || !guideMotionView || !projection ||
          extent.width == 0 || extent.height == 0) {
        return -1;
      }

      createPreparedImages(extent);
      if (!preparedDepthImages[0] || !preparedMotionImages[0])
        return -1;

      createPrepPipeline();
      if (!prepPipeline)
        return -1;

      const uint32_t slot = frameSlot;
      frameSlot = (frameSlot + 1) % kFrames;

      // Layout transition for prepared images to GENERAL
      VkImageMemoryBarrier initBarriers[2] = {
        {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .srcAccessMask = 0,
          .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = preparedDepthImages[slot],
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        },
        {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .srcAccessMask = 0,
          .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = preparedMotionImages[slot],
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        }
      };
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                           0, nullptr, 0, nullptr, 2, initBarriers);

      // Input barriers: make sure previous writes to linearDepth and guideMotion are visible
      VkImageMemoryBarrier inBarriers[2] = {
        {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = linearDepth,
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        },
        {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = guideMotion,
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        }
      };
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                           0, nullptr, 0, nullptr, 2, inBarriers);

      VkDescriptorImageInfo imageInfos[4] = {
        { VK_NULL_HANDLE, linearDepthView, VK_IMAGE_LAYOUT_GENERAL },
        { VK_NULL_HANDLE, guideMotionView, VK_IMAGE_LAYOUT_GENERAL },
        { VK_NULL_HANDLE, preparedDepthViews[slot], VK_IMAGE_LAYOUT_GENERAL },
        { VK_NULL_HANDLE, preparedMotionViews[slot], VK_IMAGE_LAYOUT_GENERAL },
      };
      VkWriteDescriptorSet writes[4]{};
      for (uint32_t i = 0; i < 4; ++i) {
        writes[i] = {
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = prepSets[slot],
          .dstBinding = i,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .pImageInfo = &imageInfos[i],
        };
      }
      vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);

      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, prepPipeline);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, prepPipelineLayout, 0, 1, &prepSets[slot], 0, nullptr);

      struct PushData {
        float proj[16];
        uint32_t depthInverted;
      } push;
      memcpy(push.proj, projection, sizeof(float) * 16);
      if (depthInverted) {
        for (unsigned column = 0; column < 4; ++column)
          push.proj[column * 4 + 2] = projection[column * 4 + 3] - projection[column * 4 + 2];
      }
      push.depthInverted = depthInverted ? 1u : 0u;

      vkCmdPushConstants(command, prepPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
      vkCmdDispatch(command, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);

      // Barrier: prepared images write -> read (GENERAL for NGX compute evaluation)
      VkImageMemoryBarrier postBarriers[2] = {
        {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = preparedDepthImages[slot],
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        },
        {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = preparedMotionImages[slot],
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        }
      };
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                           0, nullptr, 0, nullptr, 2, postBarriers);
      return static_cast<int32_t>(slot);
    }

    void createStaging(VkExtent2D extent) {
      if (stagingImage && stagingExtent.width == extent.width && stagingExtent.height == extent.height)
        return;
      destroyStaging();

      VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      if (vkCreateImage(device, &info, nullptr, &stagingImage) != VK_SUCCESS)
        return;

      VkMemoryRequirements req;
      vkGetImageMemoryRequirements(device, stagingImage, &req);

      VkPhysicalDeviceMemoryProperties memProps;
      vkGetPhysicalDeviceMemoryProperties(physical, &memProps);
      uint32_t typeIndex = UINT32_MAX;
      for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
          typeIndex = i;
          break;
        }
      }
      if (typeIndex == UINT32_MAX) {
        destroyStaging();
        return;
      }

      VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = typeIndex,
      };
      if (vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory) != VK_SUCCESS) {
        destroyStaging();
        return;
      }
      vkBindImageMemory(device, stagingImage, stagingMemory, 0);

      VkImageViewCreateInfo viewInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = stagingImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };
      if (vkCreateImageView(device, &viewInfo, nullptr, &stagingView) != VK_SUCCESS) {
        destroyStaging();
        return;
      }
      stagingExtent = extent;
    }

    void loadLibrary(bool bypassCallerCheck) {
      if (module)
        return;

#ifdef _WIN32
      HMODULE hMod = nullptr;
      std::vector<std::filesystem::path> searchCandidates;
      const auto modDir = moduleDirectory();
      for (const wchar_t *p : kNeuralUpliftLibraryPaths) {
        searchCandidates.push_back(std::filesystem::path(p));
        if (!modDir.empty()) {
          searchCandidates.push_back(modDir / p);
        }
      }

      for (const auto &cand : searchCandidates) {
        hMod = LoadLibraryW(cand.c_str());
        if (hMod) {
          SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[DLSS-NR] Loaded %ls", cand.c_str());
          break;
        }
      }

      module = hMod;
      if (!hMod) {
        stats.notLoadedReason = "nvngx_dlssnr.dll could not be loaded";
        return;
      }

      pfnInit_Ext2 = reinterpret_cast<PFN_Init_Ext2>(GetProcAddress(hMod, "NVSDK_NGX_VULKAN_Init_Ext2"));
      pfnCreateFeature1 = reinterpret_cast<PFN_CreateFeature1>(GetProcAddress(hMod, "NVSDK_NGX_VULKAN_CreateFeature1"));
      pfnEvaluateFeature = reinterpret_cast<PFN_EvaluateFeature>(GetProcAddress(hMod, "NVSDK_NGX_VULKAN_EvaluateFeature"));
      pfnReleaseFeature = reinterpret_cast<PFN_ReleaseFeature>(GetProcAddress(hMod, "NVSDK_NGX_VULKAN_ReleaseFeature"));
      pfnShutdown1 = reinterpret_cast<PFN_Shutdown1>(GetProcAddress(hMod, "NVSDK_NGX_VULKAN_Shutdown1"));

      if (!pfnInit_Ext2 || !pfnCreateFeature1 || !pfnEvaluateFeature) {
        stats.notLoadedReason = "nvngx_dlssnr.dll missing Vulkan entry points";
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[DLSS-NR] %s", stats.notLoadedReason.c_str());
        return;
      }

      if (bypassCallerCheck) {
        callerCheckHookSlot = hookNeuralUpliftGetModuleFileName(hMod);
      }

      const auto initResult = pfnInit_Ext2(
        0x1c6bfafe, // Application / project identifier
        L".",
        instance, physical, device,
        vkGetInstanceProcAddr, vkGetDeviceProcAddr,
        nullptr,
        NVSDK_NGX_Version_API);

      if (NVSDK_NGX_FAILED(initResult)) {
        stats.notLoadedReason = "NVSDK_NGX_VULKAN_Init_Ext2 failed (" + std::to_string(uint32_t(initResult)) + ")";
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[DLSS-NR] %s", stats.notLoadedReason.c_str());
        return;
      }

      snippetInitialized = true;
      SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[DLSS-NR] Snippet initialized successfully");

      NVSDK_NGX_VULKAN_AllocateParameters(&parameters);
#endif
    }

    void unloadLibrary() {
#ifdef _WIN32
      releaseFeature();
      if (parameters) {
        NVSDK_NGX_VULKAN_DestroyParameters(parameters);
        parameters = nullptr;
      }
      if (module) {
        if (snippetInitialized && pfnShutdown1) {
          pfnShutdown1(device);
          snippetInitialized = false;
        }
        if (callerCheckHookSlot) {
          unhookNeuralUpliftGetModuleFileName(static_cast<void **>(callerCheckHookSlot));
          callerCheckHookSlot = nullptr;
        }
        FreeLibrary(static_cast<HMODULE>(module));
        module = nullptr;
      }
#endif
    }

    void releaseFeature() {
      if (device && !features.empty()) {
        vkDeviceWaitIdle(device);
      }
      for (auto *feat : features) {
        if (feat && pfnReleaseFeature) {
          pfnReleaseFeature(feat);
        }
      }
      features.clear();
      createdExtent = {};
      createdPasses = 1;
      initialized = false;
    }

    void createFeature(VkCommandBuffer command, const NeuralUpliftSettings &settings, VkExtent2D extent) {
      if (!snippetInitialized || !pfnCreateFeature1 || !parameters) {
        return;
      }

      releaseFeature();

      const uint32_t targetPasses = std::clamp(settings.passes, 1u, 7u);
      for (uint32_t p = 0; p < targetPasses; ++p) {
        parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Width, extent.width);
        parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Height, extent.height);
        parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Hint_Render_Preset, static_cast<int>(std::clamp(settings.preset, 0u, 7u)));
        parameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthInverted, settings.depthInverted ? 1 : 0);
        parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Enabled, 1);
        parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
        parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);

        NVSDK_NGX_Handle *feat = nullptr;
        const NVSDK_NGX_Result res = pfnCreateFeature1(
          device, command,
          static_cast<NVSDK_NGX_Feature>(settings.featureId),
          parameters,
          &feat);

        if (NVSDK_NGX_SUCCEED(res) && feat) {
          features.push_back(feat);
        } else {
          if (p == 0) {
            stats.statusReason = "feature creation failed (" + std::to_string(uint32_t(res)) + ")";
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[DLSS-NR] CreateFeature1 failed: 0x%08x", uint32_t(res));
            return;
          } else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[DLSS-NR] Multi-pass feature count capped at %u (code 0x%08x)", p, uint32_t(res));
            break;
          }
        }
      }

      if (features.empty()) {
        return;
      }

      initialized = true;
      forceHistoryReset = true;
      createdExtent = extent;
      createdPasses = targetPasses;
      currentSettings = settings;
      stats.initCount++;
      stats.statusReason = "active";
      SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[DLSS-NR] Created %zu feature(s) at %ux%u (preset %u, id %u, passes %u)",
                  features.size(), extent.width, extent.height, settings.preset, settings.featureId, targetPasses);
    }
#endif // REMIX_HAS_DLSS5
  };

  NeuralUplift::NeuralUplift() : m(std::make_unique<Impl>()) {}
  NeuralUplift::~NeuralUplift() { release(); }

  bool NeuralUplift::isSupported() {
#if REMIX_HAS_DLSS5
#ifdef _WIN32
    const auto modDir = moduleDirectory();
    for (const wchar_t *path : kNeuralUpliftLibraryPaths) {
      const DWORD attr1 = GetFileAttributesW(path);
      if (attr1 != INVALID_FILE_ATTRIBUTES && !(attr1 & FILE_ATTRIBUTE_DIRECTORY))
        return true;
      if (!modDir.empty()) {
        const auto p = modDir / path;
        const DWORD attr2 = GetFileAttributesW(p.c_str());
        if (attr2 != INVALID_FILE_ATTRIBUTES && !(attr2 & FILE_ATTRIBUTE_DIRECTORY))
          return true;
      }
    }
#endif
#endif
    return false;
  }

  const std::string &NeuralUplift::notSupportedReason() {
    static const std::string reason = "NVIDIA DLSS-NR (Neural Uplift) unavailable: build with -Ddlss5=true and provide nvngx_dlssnr.dll.";
    return reason;
  }

  void NeuralUplift::initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                                [[maybe_unused]] uint32_t queueFamily, VkExtent2D displayExtent,
                                const NeuralUpliftSettings &settings) {
    m->instance = instance;
    m->physical = physical;
    m->device = device;
    m->displayExtent = displayExtent;

#if REMIX_HAS_DLSS5
    if (m->callerCheckHookSlot && !settings.bypassCallerCheck) {
      m->unloadLibrary();
    }
    m->loadLibrary(settings.bypassCallerCheck);
    m->createStaging(displayExtent);
#endif
  }

  void NeuralUplift::release() {
#if REMIX_HAS_DLSS5
    m->destroyPrepared();
    m->destroyPrepPipeline();
    m->destroyStaging();
    m->unloadLibrary();
#endif
    m->initialized = false;
  }

  void NeuralUplift::record(VkCommandBuffer command,
                            VkImage inOutColor, VkImageView inOutColorView,
                            VkImage linearDepth, VkImageView linearDepthView,
                            VkImage guideMotion, VkImageView guideMotionView,
                            VkExtent2D gbufferExtent, const float *projection,
                            const NeuralUpliftSettings &settings,
                            bool resetHistory) {
#if REMIX_HAS_DLSS5
    if (!settings.enable) {
      m->stats.active = false;
      m->stats.evaluating = false;
      m->stats.lastHadDepth = false;
      m->stats.lastHadMotionVectors = false;
      return;
    }

    if (!m->snippetInitialized) {
      if (m->callerCheckHookSlot && !settings.bypassCallerCheck) {
        m->unloadLibrary();
      }
      m->loadLibrary(settings.bypassCallerCheck);
      if (!m->snippetInitialized) {
        m->stats.active = false;
        m->stats.evaluating = false;
        m->stats.statusReason = m->stats.notLoadedReason.empty() ? "snippet unavailable" : m->stats.notLoadedReason;
        m->stats.lastHadDepth = false;
        m->stats.lastHadMotionVectors = false;
        return;
      }
    }

    m->createStaging(m->displayExtent);
    if (!m->stagingImage) {
      m->stats.statusReason = "staging allocation failed";
      m->stats.lastHadDepth = false;
      m->stats.lastHadMotionVectors = false;
      return;
    }

    const uint32_t passCount = std::clamp(settings.passes, 1u, 7u);
    const bool recreate = !m->initialized || m->features.empty() ||
                          (m->createdExtent.width != m->displayExtent.width) ||
                          (m->createdExtent.height != m->displayExtent.height) ||
                          (m->createdPasses != passCount) ||
                          (m->currentSettings.preset != settings.preset) ||
                          (m->currentSettings.featureId != settings.featureId) ||
                          (m->currentSettings.depthInverted != settings.depthInverted);

    if (recreate) {
      m->createFeature(command, settings, m->displayExtent);
      if (!m->initialized || m->features.empty()) {
        m->stats.evaluating = false;
        m->stats.lastHadDepth = false;
        m->stats.lastHadMotionVectors = false;
        return;
      }
    } else {
      m->currentSettings = settings;
    }

    const int32_t prepSlot = m->prepareInputs(command, linearDepth, linearDepthView,
                                              guideMotion, guideMotionView,
                                              gbufferExtent, projection, settings.depthInverted);

    m->stats.lastHadDepth = (prepSlot >= 0);
    m->stats.lastHadMotionVectors = (prepSlot >= 0);

    const VkImageCopy copyRegion{
      .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .srcOffset = {0, 0, 0},
      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .dstOffset = {0, 0, 0},
      .extent = {m->displayExtent.width, m->displayExtent.height, 1},
    };

    NVSDK_NGX_Resource_VK colorRes = NVSDK_NGX_Create_ImageView_Resource_VK(
      m->stagingView, m->stagingImage, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      VK_FORMAT_R16G16B16A16_SFLOAT, m->displayExtent.width, m->displayExtent.height, false);

    NVSDK_NGX_Resource_VK outputRes = NVSDK_NGX_Create_ImageView_Resource_VK(
      inOutColorView, inOutColor, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      VK_FORMAT_R16G16B16A16_SFLOAT, m->displayExtent.width, m->displayExtent.height, true);

    NVSDK_NGX_Resource_VK depthRes{};
    if (prepSlot >= 0) {
      depthRes = NVSDK_NGX_Create_ImageView_Resource_VK(
        m->preparedDepthViews[prepSlot], m->preparedDepthImages[prepSlot],
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        VK_FORMAT_R32_SFLOAT, gbufferExtent.width, gbufferExtent.height, false);
    }

    NVSDK_NGX_Resource_VK motionRes{};
    if (prepSlot >= 0) {
      motionRes = NVSDK_NGX_Create_ImageView_Resource_VK(
        m->preparedMotionViews[prepSlot], m->preparedMotionImages[prepSlot],
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        VK_FORMAT_R32G32_SFLOAT, gbufferExtent.width, gbufferExtent.height, false);
    }

    bool allEvaluated = true;
    NVSDK_NGX_Result lastEvalRes = NVSDK_NGX_Result_Success;

    for (uint32_t pass = 0; pass < passCount; ++pass) {
      // Step 1: Copy inOutColor -> stagingImage
      const VkAccessFlags inSrcAccess = (pass == 0)
        ? static_cast<VkAccessFlags>(VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT)
        : static_cast<VkAccessFlags>(VK_ACCESS_SHADER_WRITE_BIT);
      const VkAccessFlags stgSrcAccess = (pass == 0)
        ? static_cast<VkAccessFlags>(0)
        : static_cast<VkAccessFlags>(VK_ACCESS_SHADER_READ_BIT);

      VkImageMemoryBarrier copyBarriers[2] = {
        {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .srcAccessMask = inSrcAccess,
          .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
          .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = inOutColor,
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        },
        {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .srcAccessMask = stgSrcAccess,
          .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
          .oldLayout = (pass == 0) ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
          .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = m->stagingImage,
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        }
      };
      vkCmdPipelineBarrier(command, (pass == 0) ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           0, nullptr, 0, nullptr, 2, copyBarriers);

      vkCmdCopyImage(command, inOutColor, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     m->stagingImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

      // Step 2: Transition stagingImage -> GENERAL (read), inOutColor -> GENERAL (read/write)
      VkImageMemoryBarrier evalBarriers[2] = {
        {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = m->stagingImage,
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        },
        {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = inOutColor,
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        }
      };
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                           0, nullptr, 0, nullptr, 2, evalBarriers);

      // Step 3: Configure NGX parameters and evaluate
      m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Color, &colorRes);
      m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Output, &outputRes);

      m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseX, 0);
      m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectBaseY, 0);
      m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectWidth, static_cast<int>(m->displayExtent.width));
      m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_ColorSubrectHeight, static_cast<int>(m->displayExtent.height));

      m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseX, 0);
      m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectBaseY, 0);
      m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectWidth, static_cast<int>(m->displayExtent.width));
      m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_OutputSubrectHeight, static_cast<int>(m->displayExtent.height));

      if (prepSlot >= 0) {
        m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Depth, &depthRes);
        m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseX, 0);
        m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectBaseY, 0);
        m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectWidth, static_cast<int>(gbufferExtent.width));
        m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthSubrectHeight, static_cast<int>(gbufferExtent.height));

        m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVec, &motionRes);
        m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseX, 0);
        m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectBaseY, 0);
        m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectWidth, static_cast<int>(gbufferExtent.width));
        m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecSubrectHeight, static_cast<int>(gbufferExtent.height));
        const float mvScale = settings.motionVectorScale != 0.0f ? settings.motionVectorScale : 1.0f;
        m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecScaleX, mvScale);
        m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVecScaleY, mvScale);
      } else {
        m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Depth, static_cast<void*>(nullptr));
        m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_MVec, static_cast<void*>(nullptr));
      }

      m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Enabled, 1);
      m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_DepthInverted, settings.depthInverted ? 1 : 0);
      m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Reset, (pass == 0 && (resetHistory || m->forceHistoryReset)) ? 1 : 0);
      m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_UseAutoMask, settings.autoMask ? 1 : 0);
      m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Style, std::clamp(settings.style, 0u, 2u));
      m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_Intensity, settings.intensity);
      m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_LocalToneStrength, std::clamp(settings.styleStrength, 0.0f, 1.0f));
      m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_LocalStructureStrength, settings.localStructureStrength);
      m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_SkinStructureStrength, settings.skinStructureStrength);
      m->parameters->Set(NVSDK_NGX_Parameter_DLSSNR_UICorrection, 0);

      auto *featureToUse = m->featureForPass(pass);
      const NVSDK_NGX_Result evalRes = m->pfnEvaluateFeature(command, featureToUse, m->parameters, nullptr);
      lastEvalRes = evalRes;
      if (!NVSDK_NGX_SUCCEED(evalRes)) {
        allEvaluated = false;
        break;
      }
    }

    m->forceHistoryReset = !allEvaluated;
    m->stats.evaluating = allEvaluated;
    m->stats.active = true;
    m->stats.statusReason = allEvaluated ? "active" : "evaluate failed (" + std::to_string(uint32_t(lastEvalRes)) + ")";

    // Barrier: inOutColor back to GENERAL / read/write after compute
    VkImageMemoryBarrier postBarrier{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = inOutColor,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &postBarrier);
#else
    m->stats.statusReason = "DLSS disabled in build";
    m->stats.active = false;
    m->stats.evaluating = false;
#endif
  }

  NeuralUpliftStatistics NeuralUplift::statistics() const {
    auto s = m->stats;
    s.supported = isSupported();
    return s;
  }

} // namespace remix::upscale

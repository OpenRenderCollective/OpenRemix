#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include "upscale/nvsdk_ngx_defs_dlssnr.h"

namespace remix::upscale {

  struct NeuralUpliftSettings {
    bool enable = false;
    uint32_t passes = 1;              // 1..7 (number of sequential enhancement passes)
    uint32_t preset = 0;              // 0..7 (NVSDK_NGX_DLSSNR_Hint_Render_Preset)
    uint32_t style = 0;               // 0..2
    float intensity = 1.0f;           // 0..1
    float styleStrength = 1.0f;       // 0..1 (DLSSNR.LocalToneStrength)
    float localStructureStrength = 1.0f; // 0..2
    float skinStructureStrength = -1.0f; // -1 sentinel, or 0..2
    bool autoMask = true;
    bool depthInverted = false;
    float motionVectorScale = 1.0f;
    uint32_t featureId = kNgxFeatureDlssNrDefault; // 18
    bool bypassCallerCheck = true;
  };

  struct NeuralUpliftStatistics {
    bool supported = false;
    bool active = false;
    bool evaluating = false;
    std::string statusReason;
    std::string notLoadedReason;
    uint32_t initCount = 0;
    bool lastHadDepth = false;
    bool lastHadMotionVectors = false;
  };

  class NeuralUplift {
  public:
    NeuralUplift();
    ~NeuralUplift();

    NeuralUplift(const NeuralUplift &) = delete;
    NeuralUplift &operator=(const NeuralUplift &) = delete;

    // Checks whether nvngx_dlssnr.dll exists in candidate locations.
    static bool isSupported();
    static const std::string &notSupportedReason();

    void initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                    uint32_t queueFamily, VkExtent2D displayExtent,
                    const NeuralUpliftSettings &settings);

    void release();

    // Enhances color in-place (or through staging). Evaluates intermediate -> inOutColor.
    // colorImage must be VK_FORMAT_R16G16B16A16_SFLOAT in GENERAL layout.
    // linearDepth is VK_FORMAT_R32_SFLOAT, guideMotion is VK_FORMAT_R32G32B32A32_SFLOAT.
    // gbufferExtent is the extent of linearDepth and guideMotion.
    // projection is the camera projection matrix (16 floats).
    void record(VkCommandBuffer command,
                VkImage inOutColor, VkImageView inOutColorView,
                VkImage linearDepth, VkImageView linearDepthView,
                VkImage guideMotion, VkImageView guideMotionView,
                VkExtent2D gbufferExtent, const float *projection,
                const NeuralUpliftSettings &settings,
                bool resetHistory);

    NeuralUpliftStatistics statistics() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m;
  };

} // namespace remix::upscale

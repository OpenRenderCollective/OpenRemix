#pragma once
#include "pathtrace/options.h"
#include "forward/options.h"
#include "runtime/backend.h"
#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace remix::config {
  struct Settings {
    TraceOptions trace;
    forward::Options forward{.autoExposure = true, .bakedLighting = true,
                             .baking = {.modelLightmaps = true, .styledIndirect = true, .styleStorageMiB = 256}};
    uint32_t backend = Backend::PathTrace;
    bool vsync = false;
    std::array<float, 4> clearColor = {0.025f, 0.035f, 0.055f, 1.f};
    // Upload BC textures as GPU BC images instead of decoding them on the CPU.
    // Applies to textures created after the change; no restart needed.
    bool compressedTextures = true;
    // Fill GetVramStats from OpenRemix's allocations and the driver budget.
    bool vramStatistics = false;
    // Honour remixapi_MeshInfoDeformEXT: a deformed mesh keeps its update
    // lineage, so the tracer refits its BLAS instead of rebuilding.
    bool refreshGeometry = true;
    // Trace retained static instances from the path tracer's own copy of
    // them, updated only where they change, instead of rebuilding their
    // frame tables every frame. Output-identical.
    bool persistentStatic = true;
    // Diagnostic: rebuild the kept static tables every frame and compare;
    // the first difference is logged and shown in Diagnostics.
    bool persistentStaticValidation = false;
    // Asynchronous presentation: Present captures the frame and returns; a
    // dedicated render thread prepares, records, submits and presents it while
    // the host continues. Toggling drains the render thread and applies at
    // the next Present.
    bool asyncPresent = true;
    // Frame generation: one generated frame between each pair of rendered
    // frames, presented half a frame ahead of the rendered one. Needs the path
    // tracer and an upscaler (its depth and motion vectors), and for DLSS
    // Frame Generation an RTX GPU the feature supports. Adds half a frame of
    // latency. Off by default.
    bool frameGeneration = false;
    // Which feature generates: DLSS Frame Generation, FSR 3.1 frame generation
    // (any GPU, one generated frame), or DLSS where available and FSR
    // otherwise.
    upscale::FrameGenerationMode frameGenerationBackend = upscale::FrameGenerationMode::Fsr31;
    // NVIDIA Reflex (VK_NV_low_latency2): the driver paces the host so that
    // frames are not queued ahead of the GPU. Always on with frame generation.
    bool lowLatency = true;
    // Generated frames per rendered frame: 1 (2x) to 3 (4x), as far as the
    // GPU's feature goes (DLSS 4 multi frame generation). FSR generates one.
    uint32_t frameGenerationFrames = 1;
    // Diagnostic: timestamp every GPU pass of a frame and show the costs in
    // Diagnostics (and GetGpuPassTimingsEXT). Off by default; off records no
    // additional queries.
    bool gpuPassTimers = false;
    // The host's screen-space passes with its own shaders
    // (remixapi_SubmitHostPassesEXT): off, submissions are dropped and the
    // frame is composed as without them. Experimental, so off by default.
    bool hostPasses = false;
    // Hidden textures
    std::vector<uint64_t> hideTextures;
    bool operator==(const Settings &) const = default;
  };
  using UnknownSettings = std::map<std::string, std::string>;
  enum class SetResult { Success, Unknown, Invalid };
  SetResult set(Settings &settings, std::string_view key, std::string_view value);
  bool validate(const Settings &settings, std::string &error);
  std::string serialize(const Settings &settings, const UnknownSettings &unknown = {});
  // Transactional: all output arguments stay untouched on failure.
  bool parse(std::string_view text, Settings &settings, UnknownSettings &unknown,
             std::vector<std::string> &warnings, std::string &error);
} // namespace remix::config

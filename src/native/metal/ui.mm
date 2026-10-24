// The developer UI on Metal: Dear ImGui's SDL3 platform backend with a Metal
// renderer, and the shared settings panel with the Metal backend's tabs.
#include "api/imgui_compat.h"
#include "metal/runtime_gpu.h"
#include "ui/context.h"
#include "ui/widgets.h"
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

namespace remix {
  namespace {
    // Dear ImGui's draw data (imgui_impl_metal's shader).
    const char *uiShader = R"(
#include <metal_stdlib>
using namespace metal;
struct Uniforms { float4x4 projection; };
struct VertexIn {
  float2 position [[attribute(0)]];
  float2 uv [[attribute(1)]];
  float4 color [[attribute(2)]];
};
struct VertexOut {
  float4 position [[position]];
  float2 uv;
  float4 color;
};
vertex VertexOut uiVertex(VertexIn in [[stage_in]], constant Uniforms &uniforms [[buffer(1)]]) {
  VertexOut out;
  out.position = uniforms.projection * float4(in.position, 0, 1);
  out.uv = in.uv;
  out.color = in.color;
  return out;
}
fragment half4 uiFragment(VertexOut in [[stage_in]], texture2d<half, access::sample> texture [[texture(0)]],
                          sampler textureSampler [[sampler(0)]]) {
  return half4(in.color) * texture.sample(textureSampler, in.uv);
}
)";

    // The runtime whose ImGui renderer the shared UI code (ui/context.h) uses.
    RemixNativeRuntime::Gpu *s_uiGpu = nullptr;
    // Shows a renderer image (an id<MTLTexture>) as an ImGui texture.
    ImTextureID addUiTexture(RemixNativeRuntime::Gpu &g, const void *view) {
      const auto key = g.ui.nextTexture++;
      g.ui.textures[key] = (__bridge id<MTLTexture>)view;
      return ImTextureID(key);
    }

    void updateTexture(RemixNativeRuntime::Gpu &g, ImTextureData *texture) {
      auto &ui = g.ui;
      if (texture->Status == ImTextureStatus_WantDestroy) {
        ui.textures.erase(uint64_t(texture->GetTexID()));
        texture->SetTexID(ImTextureID_Invalid);
        texture->SetStatus(ImTextureStatus_Destroyed);
        return;
      }
      if (texture->Status == ImTextureStatus_WantCreate) {
        IM_ASSERT(texture->Format == ImTextureFormat_RGBA32);
        const auto id = ui.nextTexture++;
        ui.textures[id] = metal::texture2d(g.device, MTLPixelFormatRGBA8Unorm, texture->Width, texture->Height,
                                           MTLTextureUsageShaderRead, 1, MTLStorageModeShared);
        texture->SetTexID(ImTextureID(id));
      }
      if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates) {
        // Shared textures are written directly; an update rewrites whole rows
        // of its rectangle, which the frames in flight do not read concurrently
        // in a way that matters for the font atlas's appended glyphs.
        id<MTLTexture> target = ui.textures.at(uint64_t(texture->GetTexID()));
        const bool whole = texture->Status == ImTextureStatus_WantCreate;
        const int x = whole ? 0 : texture->UpdateRect.x, y = whole ? 0 : texture->UpdateRect.y;
        const int w = whole ? texture->Width : texture->UpdateRect.w, h = whole ? texture->Height : texture->UpdateRect.h;
        [target replaceRegion:MTLRegionMake2D(x, y, w, h)
                  mipmapLevel:0
                    withBytes:texture->GetPixelsAt(x, y)
                  bytesPerRow:texture->GetPitch()];
        texture->SetStatus(ImTextureStatus_OK);
      }
    }
  } // namespace

  bool RemixNativeRuntime::initializeUi() {
    ui::UiScope scope(ImGui::GetCurrentContext());
    IMGUI_CHECKVERSION();
    m_gui = ImGui::CreateContext();
    if (!m_gui)
      return false;
    ImGui::SetCurrentContext(m_gui);
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendRendererName = "openremix_metal";
    s_uiGpu = m_gpu.get();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    ui::toolkitStyle();
    ui::toolkitColors(m_gpu->format == MTLPixelFormatBGRA8Unorm_sRGB);
    if (!ImGui_ImplSDL3_InitForMetal(m_window.handle()))
      return false;
    const float scale = SDL_GetWindowDisplayScale(m_window.handle());
    ImGui::GetStyle().ScaleAllSizes(scale > 0 ? scale : 1.0f);
    ImGui::GetStyle().FontScaleDpi = scale > 0 ? scale : 1.0f;
    auto &g = *m_gpu;
    NSError *error = nil;
    id<MTLLibrary> library = [g.device newLibraryWithSource:@(uiShader) options:nil error:&error];
    if (!library) {
      SDL_Log("Remix Metal UI shader: %s", error.localizedDescription.UTF8String);
      return false;
    }
    auto *vertex = [MTLVertexDescriptor vertexDescriptor];
    vertex.attributes[0].format = MTLVertexFormatFloat2;
    vertex.attributes[0].offset = offsetof(ImDrawVert, pos);
    vertex.attributes[1].format = MTLVertexFormatFloat2;
    vertex.attributes[1].offset = offsetof(ImDrawVert, uv);
    vertex.attributes[2].format = MTLVertexFormatUChar4Normalized;
    vertex.attributes[2].offset = offsetof(ImDrawVert, col);
    vertex.layouts[0].stride = sizeof(ImDrawVert);
    auto *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.label = @"Developer UI";
    descriptor.vertexFunction = [library newFunctionWithName:@"uiVertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"uiFragment"];
    descriptor.vertexDescriptor = vertex;
    auto *color = descriptor.colorAttachments[0];
    color.pixelFormat = g.format;
    color.blendingEnabled = YES;
    color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    color.sourceAlphaBlendFactor = MTLBlendFactorOne;
    color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    descriptor.depthAttachmentPixelFormat = metal::Raster::Gpu::depthFormat;
    g.ui.pipeline = [g.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!g.ui.pipeline) {
      SDL_Log("Remix Metal UI pipeline: %s", error.localizedDescription.UTF8String);
      return false;
    }
    auto *depth = [MTLDepthStencilDescriptor new];
    depth.depthCompareFunction = MTLCompareFunctionAlways;
    depth.depthWriteEnabled = NO;
    g.ui.depth = [g.device newDepthStencilStateWithDescriptor:depth];
    auto *sampler = [MTLSamplerDescriptor new];
    sampler.minFilter = sampler.magFilter = MTLSamplerMinMagFilterLinear;
    sampler.sAddressMode = sampler.tAddressMode = MTLSamplerAddressModeClampToEdge;
    g.ui.sampler = [g.device newSamplerStateWithDescriptor:sampler];
    m_window.setEventHandler(processEvent, this);
    return true;
  }

  void RemixNativeRuntime::shutdownUi() {
    if (!m_gui)
      return;
    ui::UiScope scope(ImGui::GetCurrentContext() == m_gui ? nullptr : ImGui::GetCurrentContext());
    ImGui::SetCurrentContext(m_gui);
    ui::resetTextureUiCache();
    for (auto *texture : ImGui::GetPlatformIO().Textures)
      if (texture->RefCount == 1) {
        texture->SetTexID(ImTextureID_Invalid);
        texture->SetStatus(ImTextureStatus_Destroyed);
      }
    auto &io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    if (io.BackendPlatformUserData)
      ImGui_ImplSDL3_Shutdown();
    SDL_StopTextInput(m_window.handle());
    ImGui::DestroyContext(m_gui);
    m_gui = nullptr;
    if (m_gpu)
      m_gpu->ui = {};
    m_window.setEventHandler(nullptr, nullptr);
    m_window.setGuiCapture(false, false);
  }

  bool RemixNativeRuntime::processEvent(const SDL_Event *event, void *userData) {
    auto &runtime = *static_cast<RemixNativeRuntime *>(userData);
    ui::UiScope scope(runtime.m_gui);
    return ImGui_ImplSDL3_ProcessEvent(event);
  }

  // ui/debug_ui.cpp drawUi (building the frame).
  void RemixNativeRuntime::buildUi() {
    if (!m_gui)
      return;
    ui::UiScope scope(m_gui);
    ImGui_ImplSDL3_NewFrame();
    ImGui::GetIO().MouseDrawCursor = m_window.embedded() && m_uiState != REMIXAPI_UI_STATE_NONE;
    ImGui::NewFrame();
    // ui/debug_ui.cpp's preparation notice.
    if (m_preparation) {
      const auto &io = ImGui::GetIO();
      ImGui::SetNextWindowPos(m_loading ? ImVec2(io.DisplaySize.x * .5f, io.DisplaySize.y * .5f) : ImVec2(16, 16),
                              ImGuiCond_Always, m_loading ? ImVec2(.5f, .5f) : ImVec2(0, 0));
      ImGui::SetNextWindowBgAlpha(.94f);
      ImGui::Begin("Renderer preparation", nullptr,
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoSavedSettings);
      ImGui::TextUnformatted(m_loading ? "Preparing renderer" : "Preparing requested settings");
      // The compiles so far as a bar; the scene variants compiled in the
      // background keep it going after activation.
      const auto stats = metal::compileProgress();
      char progress[48];
      std::snprintf(progress, sizeof(progress), "%llu / %llu", (unsigned long long)stats.completed,
                    (unsigned long long)stats.requested);
      ImGui::TextUnformatted("Compiling shaders");
      ImGui::ProgressBar(stats.requested ? float(double(stats.completed) / double(stats.requested)) : 0.f,
                         ImVec2(280, 0), progress);
      ImGui::Text("Elapsed: %.1f s", preparationSeconds());
      ImGui::Text("%s: settings and diagnostics", m_profileUi.menuShortcut.c_str());
      ImGui::End();
    }
    if (m_uiState != REMIXAPI_UI_STATE_NONE) {
      const bool open =
          ui::drawPanel(m_profiles, m_profileUi, this, [](void *user, int tab, config::Settings &settings) {
            auto &runtime = *static_cast<RemixNativeRuntime *>(user);
            switch (tab) {
            case 0:
              runtime.drawRenderingTab(settings);
              break;
            case 1:
              runtime.drawLightingTab(settings);
              break;
            case 2:
              runtime.drawSkyTab(settings);
              break;
            case 3:
              runtime.drawPostProcessingTab(settings);
              break;
            case 4:
              runtime.drawDiagnosticsTab(settings);
              break;
            case 5:
              runtime.drawTexturesTab(settings);
              break;
            }
          });
      if (!open)
        setUiState(REMIXAPI_UI_STATE_NONE);
    }
    remixapi_imgui_InvokeOverlayCallback();
    ImGui::Render();
    const auto &io = ImGui::GetIO();
    m_window.setGuiCapture(m_uiState != REMIXAPI_UI_STATE_NONE && io.WantCaptureKeyboard,
                           m_uiState != REMIXAPI_UI_STATE_NONE && io.WantCaptureMouse);
    m_window.syncTextInput(m_uiState != REMIXAPI_UI_STATE_NONE && io.WantTextInput);
    for (auto *texture : ImGui::GetPlatformIO().Textures)
      if (texture->Status != ImTextureStatus_OK)
        updateTexture(*m_gpu, texture);
  }

  void RemixNativeRuntime::renderUi(void *encoderPointer) {
    if (!m_gui)
      return;
    ui::UiScope scope(m_gui);
    const auto *data = ImGui::GetDrawData();
    if (!data || !data->TotalVtxCount)
      return;
    id<MTLRenderCommandEncoder> encoder = (__bridge id<MTLRenderCommandEncoder>)encoderPointer;
    auto &g = *m_gpu;
    auto &ui = g.ui;
    const float width = data->DisplaySize.x * data->FramebufferScale.x;
    const float height = data->DisplaySize.y * data->FramebufferScale.y;
    if (width <= 0 || height <= 0)
      return;
    // Per-slot buffers: the previous use of this slot has completed.
    const size_t vertexBytes = size_t(data->TotalVtxCount) * sizeof(ImDrawVert);
    const size_t indexBytes = size_t(data->TotalIdxCount) * sizeof(ImDrawIdx);
    auto &vertices = ui.vertices[g.frameIndex], &indices = ui.indices[g.frameIndex];
    if (!vertices || vertices.length < vertexBytes)
      vertices = metal::buffer(g.device, nullptr, vertexBytes * 2);
    if (!indices || indices.length < indexBytes)
      indices = metal::buffer(g.device, nullptr, indexBytes * 2);
    auto *vertexOut = static_cast<ImDrawVert *>(vertices.contents);
    auto *indexOut = static_cast<ImDrawIdx *>(indices.contents);
    for (const auto *list : data->CmdLists) {
      std::memcpy(vertexOut, list->VtxBuffer.Data, list->VtxBuffer.Size * sizeof(ImDrawVert));
      std::memcpy(indexOut, list->IdxBuffer.Data, list->IdxBuffer.Size * sizeof(ImDrawIdx));
      vertexOut += list->VtxBuffer.Size;
      indexOut += list->IdxBuffer.Size;
    }
    const float l = data->DisplayPos.x, r = data->DisplayPos.x + data->DisplaySize.x;
    const float t = data->DisplayPos.y, b = data->DisplayPos.y + data->DisplaySize.y;
    const float projection[16] = {2 / (r - l), 0, 0, 0, 0, 2 / (t - b), 0, 0, 0, 0, 1, 0,
                                  (r + l) / (l - r), (t + b) / (b - t), 0, 1};
    [encoder setViewport:{0, 0, double(width), double(height), 0, 1}];
    [encoder setRenderPipelineState:ui.pipeline];
    [encoder setDepthStencilState:ui.depth];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setVertexBytes:projection length:sizeof(projection) atIndex:1];
    [encoder setFragmentSamplerState:ui.sampler atIndex:0];
    size_t vertexOffset = 0, indexOffset = 0;
    const ImVec2 clipOffset = data->DisplayPos, clipScale = data->FramebufferScale;
    for (const auto *list : data->CmdLists) {
      for (const auto &command : list->CmdBuffer) {
        if (command.UserCallback) {
          if (command.UserCallback != ImDrawCallback_ResetRenderState)
            command.UserCallback(list, &command);
          continue;
        }
        const float x0 = std::max(0.f, (command.ClipRect.x - clipOffset.x) * clipScale.x);
        const float y0 = std::max(0.f, (command.ClipRect.y - clipOffset.y) * clipScale.y);
        const float x1 = std::min(width, (command.ClipRect.z - clipOffset.x) * clipScale.x);
        const float y1 = std::min(height, (command.ClipRect.w - clipOffset.y) * clipScale.y);
        if (x1 <= x0 || y1 <= y0)
          continue;
        [encoder setScissorRect:{NSUInteger(x0), NSUInteger(y0), NSUInteger(x1 - x0), NSUInteger(y1 - y0)}];
        const auto texture = ui.textures.find(uint64_t(command.GetTexID()));
        if (texture == ui.textures.end())
          continue;
        [encoder setFragmentTexture:texture->second atIndex:0];
        [encoder setVertexBuffer:vertices
                          offset:(vertexOffset + command.VtxOffset) * sizeof(ImDrawVert)
                         atIndex:0];
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:command.ElemCount
                             indexType:sizeof(ImDrawIdx) == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                           indexBuffer:indices
                     indexBufferOffset:(indexOffset + command.IdxOffset) * sizeof(ImDrawIdx)];
      }
      vertexOffset += list->VtxBuffer.Size;
      indexOffset += list->IdxBuffer.Size;
    }
    [encoder setScissorRect:{0, 0, NSUInteger(width), NSUInteger(height)}];
  }

  // The developer UI's view of the Metal runtime (metal/ui/status.h); the
  // tabs themselves are ui/rendering.cpp, lighting.cpp, post_processing.cpp
  // and diagnostics.cpp, as in Vulkan builds.
  ui::DeviceStatus RemixNativeRuntime::uiDevice() const {
    if (!m_gpu || !m_gpu->device)
      return {"", "Metal"};
    std::string api = "Metal";
    bool metal4 = false;
    if (@available(macOS 26.0, *))
      metal4 = [m_gpu->device supportsFamily:MTLGPUFamilyMetal4];
    if (metal4)
      api += " 4";
    else if ([m_gpu->device supportsFamily:MTLGPUFamilyMetal3])
      api += " 3";
    if (@available(macOS 14.0, *))
      api += std::string(" (") + m_gpu->device.architecture.name.UTF8String + ")";
    return {m_gpu->device.name.UTF8String, api};
  }

  ui::PreparationStatus RemixNativeRuntime::uiPreparation() const {
    ui::PreparationStatus result;
    result.available = true;
    if (m_preparation) {
      result.pending = true;
      result.generation = m_preparationGeneration;
      result.elapsedSeconds = preparationSeconds();
    }
    result.failure = m_traceUnavailable;
    return result;
  }

  ui::FrameGenerationStatus RemixNativeRuntime::uiFrameGeneration() const {
    ui::FrameGenerationStatus result;
    if (m_gpu) {
      const auto &f = m_gpu->interpolation;
      result.status = f.interpolator ? "MetalFX frame interpolation" : "";
      result.unavailable = f.failure;
      result.generated = f.generated;
      result.renderedInterval = double(f.interval) * 1000;
      result.presentImages = m_gpu->layer ? m_gpu->layer.maximumDrawableCount : 0;
      result.presentMode = m_vsync ? "display sync" : "immediate";
    }
    result.lowLatencyUnavailable = "NVIDIA Reflex is not available with the Metal backend.";
    return result;
  }

  ui::PostStatus RemixNativeRuntime::uiPost() const {
    ui::PostStatus result;
    if (m_pathTracer && m_backend) {
      const auto s = m_pathTracer->postStatistics();
      result.exposure = s.exposure;
      result.target = s.target;
      result.limit = s.limit < 0 ? -1 : s.limit > 0 ? 1 : 0;
      result.milliseconds = s.milliseconds;
      result.memoryBytes = s.memoryBytes;
    }
    result.activeExposure = m_traceOptions.exposure * (m_traceOptions.autoExposure ? result.exposure : 1.f);
    return result;
  }

  ui::PresentStatus RemixNativeRuntime::uiPresent() const {
    ui::PresentStatus result;
    result.performance = m_previousPerformance;
    result.breakdown = m_previousBreakdown;
    return result;
  }

  ui::ReconstructionChoices RemixNativeRuntime::uiReconstructionChoices() const {
    // MetalFX takes FSR's and DLSS's places: every upscaler mode is MetalFX.
    ui::ReconstructionChoices result;
    result.upscalers = "Off\0MetalFX\0";
    result.vendorModes = false;
    result.denoiser = "MetalFX denoising";
    result.sharpening = false;
    result.frameGenerationBackends = nullptr;
    result.multiFrameGeneration = false;
    result.frameGenerationTooltip = "MetalFX frame interpolation: presents a generated frame between\n"
                                    "rendered ones. Needs the path tracer without accumulation. Adds latency.";
    return result;
  }

  const void *RemixNativeRuntime::uiTextureView(const scene::Texture *texture) const {
    const auto &textures = const_cast<metal::Raster &>(m_raster).gpu().textures;
    const auto found = textures.find(texture);
    return found != textures.end() ? (__bridge const void *)found->second.texture : nullptr;
  }

  bool RemixNativeRuntime::gpuTimersSupported() const { return m_gpu && m_gpu->timers.supported(); }

  bool RemixNativeRuntime::textureCompressionSupported() const {
    return m_gpu && m_gpu->device && [m_gpu->device supportsBCTextureCompression];
  }
} // namespace remix

// ui/context.h: the Metal ImGui renderer's textures.
namespace remix::ui {
  void updateTexture(ImTextureData *texture) {
    if (s_uiGpu)
      remix::updateTexture(*s_uiGpu, texture); // the anonymous namespace's
  }
  ImTextureID addTexture(const void *view) {
    if (!s_uiGpu || !view)
      return ImTextureID_Invalid;
    return addUiTexture(*s_uiGpu, view);
  }
  void removeTexture(ImTextureID texture) {
    if (s_uiGpu)
      s_uiGpu->ui.textures.erase(uint64_t(texture));
  }
} // namespace remix::ui

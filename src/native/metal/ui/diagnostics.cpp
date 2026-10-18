// Metal builds' copy of ui/diagnostics.cpp: the same tab, with what is Vulkan's read through
// the runtime's ui* queries (metal/ui/status.h). Port upstream changes to ui/diagnostics.cpp here.
#include "metal/runtime.h"
#include "ui/widgets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <imgui.h>
#include <string>
#include <utility>
#include <vector>

namespace remix {
  void RemixNativeRuntime::drawGpuPassTimings(bool enabled) {
    if (!enabled)
      return;
    if (!gpuTimersSupported()) {
      ImGui::TextWrapped("Unavailable: the queue family has no timestamp support.");
      return;
    }
    const auto &t = gpuPassTimings();
    if (!t.available) {
      ImGui::TextWrapped("Waiting for a completed frame.");
      return;
    }
    ImGui::TextWrapped("GPU frame %.3f ms = passes %.3f + between passes %.3f (frame %llu, queue family %u)",
                       t.frameMilliseconds, t.passMilliseconds, t.untimedMilliseconds, (unsigned long long)t.frame,
                       t.queueFamily);
    ImGui::TextWrapped("Render %ux%u, output %ux%u", t.renderWidth, t.renderHeight, t.outputWidth, t.outputHeight);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Each pass is timed from the end of the work before it to its own end (bottom of\n"
                        "the pipe), so passes do not overlap and their own times add up to the frame.\n"
                        "Own time excludes nested passes. Passes that wait for the swapchain image or\n"
                        "for presentation include that wait.");
    if (t.truncated)
      ImGui::TextWrapped("%u passes not timed (query limit)", t.truncated);
    // Sorted by own time; the time between passes is a row of its own, so the
    // column adds up to the frame.
    const auto order = gpu::byCost(t);
    const uint32_t untimedRow = uint32_t(t.passes.size());
    std::vector<uint32_t> rows;
    rows.reserve(order.size() + 1);
    bool untimedPlaced = false;
    for (const auto index : order) {
      if (!untimedPlaced && t.untimedMilliseconds >= t.passes[index].selfMilliseconds) {
        rows.push_back(untimedRow);
        untimedPlaced = true;
      }
      rows.push_back(index);
    }
    if (!untimedPlaced)
      rows.push_back(untimedRow);
    const double frame = std::max(t.frameMilliseconds, 1e-9);
    auto size = [](const gpu::PassTiming &pass, char *out, size_t bytes) {
      if (pass.width && pass.height)
        std::snprintf(out, bytes, "%ux%u", pass.width, pass.height);
      else
        std::snprintf(out, bytes, "-");
    };
    if (ImGui::SmallButton("Copy as text")) {
      std::string text;
      char line[160], dims[32];
      std::snprintf(line, sizeof(line),
                    "GPU frame %.3f ms (passes %.3f, between passes %.3f); render %ux%u, output %ux%u\n",
                    t.frameMilliseconds, t.passMilliseconds, t.untimedMilliseconds, t.renderWidth, t.renderHeight,
                    t.outputWidth, t.outputHeight);
      text += line;
      for (const auto row : rows) {
        if (row == untimedRow) {
          std::snprintf(line, sizeof(line), "%-56s %11s %8.3f %8.3f %5.1f%%\n", "Between passes (untimed)", "-",
                        t.untimedMilliseconds, t.untimedMilliseconds, 100 * t.untimedMilliseconds / frame);
        } else {
          const auto &pass = t.passes[row];
          size(pass, dims, sizeof(dims));
          std::snprintf(line, sizeof(line), "%-56s %11s %8.3f %8.3f %5.1f%%\n", pass.name, dims,
                        pass.selfMilliseconds, pass.milliseconds, 100 * pass.selfMilliseconds / frame);
        }
        text += line;
      }
      ImGui::SetClipboardText(text.c_str());
    }
    if (!ImGui::BeginTable("gpuPasses", 5,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit))
      return;
    ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Size");
    ImGui::TableSetupColumn("Own ms");
    ImGui::TableSetupColumn("Incl. ms");
    ImGui::TableSetupColumn("%");
    ImGui::TableHeadersRow();
    auto cells = [&](const char *name, const char *dims, double self, double inclusive, bool nested) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (nested)
        ImGui::TextDisabled("%s", name);
      else
        ImGui::TextUnformatted(name);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(dims);
      ImGui::TableNextColumn();
      ImGui::Text("%7.3f", self);
      ImGui::TableNextColumn();
      ImGui::Text("%7.3f", inclusive);
      ImGui::TableNextColumn();
      ImGui::Text("%5.1f", 100 * self / frame);
    };
    char dims[32];
    for (const auto row : rows) {
      if (row == untimedRow) {
        cells("Between passes (untimed)", "-", t.untimedMilliseconds, t.untimedMilliseconds, false);
        continue;
      }
      const auto &pass = t.passes[row];
      size(pass, dims, sizeof(dims));
      // Containers (own time is what their nested passes leave out) are dimmed.
      cells(pass.name, dims, pass.selfMilliseconds, pass.milliseconds,
            pass.selfMilliseconds + 1e-9 < pass.milliseconds);
    }
    cells("Total", "-", t.frameMilliseconds, t.frameMilliseconds, false);
    ImGui::EndTable();
  }

  void RemixNativeRuntime::drawDiagnosticsTab(config::Settings &settings) {
    if (const auto s = uiPreparation(); s.available) {
      ImGui::Text("Pipeline preparation: %llu / %llu completed",
                  (unsigned long long)s.completed,
                  (unsigned long long)s.requested);
      ImGui::Text("Created %llu, reused %llu, failed %llu; %.1f ms compiling",
                  (unsigned long long)s.created, (unsigned long long)s.reused,
                  (unsigned long long)s.failed, s.milliseconds);
      ImGui::Text("Pending requests deduplicated: %llu",
                  (unsigned long long)s.deduplicated);
      ImGui::Text("Driver cache: %s; feedback: %s; hits: %llu",
                  s.cacheLoaded ? "loaded" : "cold",
                  s.feedback ? "supported" : "unavailable",
                  (unsigned long long)s.cacheHits);
      if (s.feedback)
        ImGui::Text("Driver creation time: %.1f ms", s.driverMilliseconds);
      if (s.cacheWriteFailed)
        ImGui::TextUnformatted(
            "Pipeline cache could not be saved; rendering remains available.");
      if (s.pending) {
        ImGui::Text("Pending configuration #%llu (profile %s)",
                    (unsigned long long)s.generation,
                    m_profiles.active().c_str());
        ImGui::Text(
            "Elapsed %.1f s; totals include jobs discovered during preparation",
            s.elapsedSeconds);
      }
      if (!s.failure.empty())
        ImGui::TextWrapped("%s", s.failure.c_str());
      ImGui::Separator();
      ImGui::Text(
          "Longest presentation gap: %.1f ms (includes application idle time)",
          m_longestPresentationGap);
    }
    auto &options = settings.trace;
    const bool rrActive = m_pathTracer && metal::denoises(m_pathTracer->upscalingStatistics().backend);
    if (ImGui::CollapsingHeader("Debug views", ImGuiTreeNodeFlags_DefaultOpen)) {
      int view = int(options.debugView);
      if (ui::Combo(
              "Debug view", &view,
              "Final lighting\0Albedo\0Shading normals\0Triangle IDs\0Depth bands\0World position grid\0"
              "Raw local lighting\0Raw indirect lighting\0Motion vectors\0Motion validity\0"
              "Indirect lighting confidence\0TAA current-frame weight\0Roughness and metalness\0Local history confidence\0"))
        options.debugView = uint32_t(view);
      static const char *help[] = {
          "Start with Triangle IDs and Shading normals at the problem angle, then compare Raw local "
          "lighting. Compare both Primary visibility options above.",
          "Unlit base color of the first visible surface. Flicker here points to visibility or material "
          "data.",
          "World-space shading normals: RGB = XYZ mapped to 0..1. Includes authored vertex normals and "
          "material normal maps; missing vertex normals fall back to flat faces.",
          "Colors identify submitted triangles. Coplanar surfaces swapping visibility change color. "
          "Remeshing can also change IDs; palette collisions are possible.",
          "Distance from the camera, repeating black to white every 4 world units. Sudden patches can reveal "
          "visibility or position jumps.",
          "World position as RGB, repeating every world unit. Look for moving discontinuities inside a flat "
          "face.",
          "Unfiltered direct local illumination on opaque receivers, including torches and emissives, before "
          "albedo and transparent tint. Uses the current display tonemapper.",
          "Unfiltered indirect illumination before albedo and transparent tint. Includes SHARC when enabled; "
          "zero bounces is black.",
          "Opaque motion guides: red/green = horizontal/vertical pixels, gray = zero, full range = +/-16 "
          "pixels. Magenta = invalid motion.",
          "Opaque motion correspondence: green = available, red = invalid/reset. This precedes per-pixel "
          "depth/normal history rejection.",
          "REBLUR indirect confidence: white = high trust, black = low. This is not the local-light "
          "denoiser's history.",
          "Actual native TAA blend weight: white = current frame only; darker = more history. Unavailable "
          "with FSR or TAA off.",
          "PBR material of the first visible surface: red = roughness, green = metalness. Blue marks "
          "non-PBR materials.",
          "Local-light denoiser history confidence: white = history kept, black = dropped (moved lights, "
          "moved occluders, moving surfaces). Indexed as NRD reads it."};
      ImGui::TextWrapped("%s", help[options.debugView]);
      if (options.debugView) {
        ImGui::TextWrapped(
            "Diagnostics display live internal pixels without temporal filtering. Lighting histories and "
            "requested settings keep running. FSR pixels are enlarged without filtering.");
        ImGui::TextWrapped("Purple checkerboard means the selected signal is unavailable. Normal subpixel "
                           "jitter remains visible at edges.");
      }
    }
    int shadowView = int(options.shadowDebug);
    if (ui::Combo("Shadow view", &shadowView, "Final lighting\0Raw visibility\0SIGMA visibility\0"))
      options.shadowDebug = uint32_t(shadowView);
    ImGui::BeginDisabled(rrActive);
    ui::Checkbox("NRD validation", &options.validation);
    ImGui::EndDisabled();
    ui::Checkbox("Progressive accumulation", &options.accumulate);
    if (options.accumulate)
      ImGui::TextWrapped("%s", "Raw reference: requested denoiser and TAA resume afterward.");
    ui::Checkbox("Force dynamic TLAS rebuild", &options.forceRebuild);
    ui::Checkbox("Freeze emissive radiance", &options.freezeEmissiveRadiance);
    if (ImGui::CollapsingHeader("Free Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
      ui::Checkbox("Enable Free Camera", &m_freeCamera.enabled);
      if (m_freeCamera.enabled) {
        ImGui::Indent();
        ui::Checkbox("Lock Free Camera", &m_freeCamera.locked);
        ui::Checkbox("Invert Y (Pitch)", &m_freeCamera.invertY);
        ui::DragFloat3("Position offset", m_freeCamera.positionOffset.data(), 0.1f);
        ui::DragFloat("Yaw", &m_freeCamera.yaw, 0.01f, -3.14159f, 3.14159f, "%.3f rad");
        ui::DragFloat("Pitch", &m_freeCamera.pitch, 0.01f, -1.55f, 1.55f, "%.3f rad");
        ui::DragFloat("Movement Speed", &m_freeCamera.speed, 0.5f, 0.1f, 500.0f, "%.1f");
        ui::DragFloat("Turning Speed", &m_freeCamera.turningSpeed, 0.05f, 0.1f, 10.0f, "%.2f");
        if (ImGui::Button("Reset Camera")) {
          m_freeCamera.reset();
        }
        if (ImGui::CollapsingHeader("Controls Guide")) {
          ImGui::BulletText("W / S: Move Forward / Backward");
          ImGui::BulletText("A / D: Move Left / Right");
          ImGui::BulletText("E / Q: Move Up / Down");
          ImGui::BulletText("Shift: Speed boost (3x) | Ctrl: Slow precision (0.25x)");
          ImGui::BulletText("Mouse Left-Drag: Look around (yaw & pitch)");
          ImGui::BulletText("J / L: Turn Yaw | I / K: Tilt Pitch");
          ImGui::BulletText("Mouse Right-Click: Reset position & angles");
        }
        ImGui::Unindent();
      }
    }
    if (ImGui::CollapsingHeader("Performance statistics", ImGuiTreeNodeFlags_DefaultOpen)) {
      const auto device = uiDevice();
      ImGui::TextWrapped("%s", device.name.c_str());
      ImGui::TextWrapped("%s", device.api.c_str());
      const auto &io = ImGui::GetIO();
      ImGui::TextWrapped("Presentation: %.2f ms / %.1f FPS", io.Framerate > 0 ? 1000.0f / io.Framerate : 0.0f,
                         io.Framerate);
      ImGui::TextWrapped("Scene: %zu surface draws", pendingDrawCount());
      auto s = tracingStatistics();
      ImGui::TextWrapped("Accumulated frames: %llu", (unsigned long long)s.accumulationCount);
      ImGui::TextWrapped("BLAS builds / refits / compactions: %llu / %llu / %llu",
                         (unsigned long long)s.blasBuilds, (unsigned long long)s.blasRefits,
                         (unsigned long long)s.blasCompactions);
      ImGui::TextWrapped("Static TLAS builds: %llu", (unsigned long long)s.staticTlasBuilds);
      ImGui::TextWrapped("Dynamic TLAS builds / refits: %llu / %llu", (unsigned long long)s.dynamicTlasBuilds,
                         (unsigned long long)s.dynamicTlasRefits);
      ImGui::TextWrapped("Rebuild reason: %u; refits since build: %u", s.dynamicRebuildReason,
                         s.dynamicRefitCount);
      ImGui::TextWrapped("Acceleration memory: %.1f MiB", double(s.accelerationBytes) / (1024 * 1024));
      if (s.gpuTimingsAvailable) {
        ImGui::TextWrapped("GPU builds %.3f ms; tracing %.3f ms", s.gpuBuildMilliseconds,
                           s.gpuTraceMilliseconds);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Tracing includes sun and local shadows, indirect rays, and transparent "
                            "sheets.\nDenoiser timings are reported separately.");
      }
    }
    if (ImGui::CollapsingHeader("GPU pass timings", ImGuiTreeNodeFlags_DefaultOpen)) {
      ui::Checkbox("GPU pass timers", &settings.gpuPassTimers);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Timestamps around every GPU pass of the frame (rtx.native.gpuPassTimers).\n"
                          "Off records no extra queries. Results are read after the frame's fence,\n"
                          "without waiting, so they describe the frame completed two frames ago.");
      drawGpuPassTimings(settings.gpuPassTimers);
    }
    if (ImGui::CollapsingHeader("Present CPU breakdown")) {
      // Previous Present; this panel is drawn while the current one records.
      const auto present = uiPresent();
      const auto &p = present.performance;
      const auto &b = present.breakdown;
      ImGui::TextWrapped("Present %.2f ms: scene %.2f, fence %.2f, acquire %.2f, record %.2f, submit %.2f, "
                         "queue present %.2f",
                         p.presentMilliseconds, p.sceneMilliseconds, p.fenceMilliseconds, p.acquireMilliseconds,
                         p.recordMilliseconds, p.submitMilliseconds, p.queuePresentMilliseconds);
      if (present.async) {
        // Asynchronous presentation: the frame above ran on the render thread;
        // its Present on the caller's thread is split out here. Present, setup
        // and the rows below include the caller's setup and capture, not its
        // wait for the handoff.
        const auto &c = present.caller;
        ImGui::TextWrapped("Caller Present %.2f ms: handoff wait %.2f, setup %.2f (developer UI %.2f), capture "
                           "%.2f | render thread %.2f ms",
                           c.callMilliseconds, c.waitMilliseconds, c.setupMilliseconds, c.uiMilliseconds,
                           c.captureMilliseconds, present.render.renderMilliseconds);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Handoff wait: time Present waited for the render thread to finish the previous\n"
                            "frame (the render thread is the bottleneck when it is large). Capture: publishing\n"
                            "the frame's draw lists, lights and static-set changes to the render thread.\n"
                            "Render thread: scene preparation through queue present, excluding the caller.");
        ImGui::TextWrapped("This Present waited %.2f ms for that frame; handed off %llu, presented %llu",
                           present.current.waitMilliseconds, (unsigned long long)present.current.handedOff,
                           (unsigned long long)present.render.rendered);
      }
      struct Row {
        const char *name;
        double value;
      };
      const Row rows[] = {{"Raster prepare", b.rasterPrepareMilliseconds},
                          {"  raster skinning", b.rasterSkinningMilliseconds},
                          {"Tracer begin", b.traceBeginMilliseconds},
                          {"Tracer setup", b.traceSetupMilliseconds},
                          {"Trace surfaces", b.traceSurfacesMilliseconds},
                          {"  geometry", p.geometryMilliseconds},
                          {"  materials", b.materialMilliseconds},
                          {"  temporal", b.temporalMilliseconds},
                          {"Retire", b.retireMilliseconds},
                          {"Acceleration", b.traceAccelerationMilliseconds},
                          {"Surface table", b.tableMilliseconds},
                          {"Lighting", b.lightingMilliseconds},
                          {"SHARC", b.sharcMilliseconds},
                          {"Scene signature", b.signatureMilliseconds},
                          {"Descriptors", b.descriptorMilliseconds},
                          {"Primary G-buffer", b.primaryMilliseconds},
                          {"Passes", b.passesMilliseconds},
                          {"Post-processing", b.postMilliseconds},
                          {"Composite", b.compositeMilliseconds},
                          {present.async ? "Developer UI draw" : "Developer UI", b.uiMilliseconds},
                          {"Snapshot", b.snapshotMilliseconds},
                          {"Other", b.otherMilliseconds},
                          {"Present: setup", b.presentSetupMilliseconds},
                          {"Present: release", b.presentReleaseMilliseconds}};
      for (const auto &row : rows)
        ImGui::Text("%-18s %7.3f ms", row.name, row.value);
      // The persistent static set: its sync is part of scene, its tables part
      // of trace surfaces.
      const auto &st = present.statics;
      ImGui::Text("%-18s %7.3f ms", "Static sync", st.syncMilliseconds);
      ImGui::Text("%-18s %7.3f ms", "Static tables", st.tableMilliseconds);
      if (st.active)
        ImGui::TextWrapped("Static set: %llu instances, surfaces reused %llu, rebuilt %llu (%llu instances); "
                           "dynamic surfaces %llu; changes %llu, layouts %llu, temporal %llu",
                           (unsigned long long)st.units, (unsigned long long)st.reusedSurfaces,
                           (unsigned long long)st.rebuiltSurfaces, (unsigned long long)st.rebuiltUnits,
                           (unsigned long long)st.dynamicSurfaces, (unsigned long long)st.changes,
                           (unsigned long long)st.relayouts, (unsigned long long)st.temporalSurfaces);
      else
        ImGui::TextWrapped("Static set: not in use (dynamic surfaces %llu)", (unsigned long long)st.dynamicSurfaces);
      if (st.validations)
        ImGui::TextWrapped("Static validation: %llu mismatches%s%s", (unsigned long long)st.mismatches,
                           st.mismatches ? ": " : "", st.mismatches ? st.mismatch.c_str() : "");
      ImGui::TextWrapped("Instances %llu, surfaces %llu, primary draws %llu, raster draws %llu, textures %llu",
                         (unsigned long long)b.traceInstances, (unsigned long long)b.traceSurfaces,
                         (unsigned long long)b.primaryDraws, (unsigned long long)b.rasterDraws,
                         (unsigned long long)b.textureDescriptors);
      ImGui::TextWrapped("Geometry conversions %llu, uploads %.2f MiB (raster %.2f MiB), raster skinned "
                         "vertices %llu",
                         (unsigned long long)p.geometryConversions, double(p.uploadBytes) / (1024 * 1024),
                         double(b.rasterUploadBytes) / (1024 * 1024),
                         (unsigned long long)b.rasterSkinnedVertices);
    }
    if (ImGui::CollapsingHeader("Input and clear color", ImGuiTreeNodeFlags_DefaultOpen)) {
      ui::ColorEdit3("Clear color", settings.clearColor.data());
      ui::InputText("Input", uiInput(), uiInputSize());
    }
  }
} // namespace remix

#include "raster/gpu.h"
#include <remix/remix_native_ext.h>
#include <deque>
#include <limits>

namespace remix {
  using namespace scene;
  using namespace raster::detail;

  void RasterRenderer::prepare(VkCommandBuffer command, bool tracing) {
    auto &g = *m_gpu;
    m_profile = {};
    g.uploadBytes = 0;
    struct Finish {
      Profile &profile;
      const uint64_t &uploads;
      Profile &completed, &worst;
      std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
      uint64_t skinning = 0;
      ~Finish() {
        profile.prepareMilliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        profile.skinningMilliseconds = tickMilliseconds(skinning);
        profile.uploadBytes = uploads;
        completed = profile;
        if (profile.prepareMilliseconds > worst.prepareMilliseconds)
          worst = profile;
      }
    } finish{m_profile, g.uploadBytes, m_completedProfile, m_worstProfile};
    using Clock = std::chrono::steady_clock;
    const auto since = [](Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); };
    auto phase = Clock::now();
    g.stagingAllocateMs = g.stagingWriteMs = 0;
    g.createPipelineMs = g.createMeshMs = g.createTextureMs = g.createMaterialMs = 0;
    g.pipelinesCreated = g.texturesCreated = 0;
    g.stagingBuffers = g.stagingAllocated = 0;
    g.commands.clear();
    g.sprite.soft = 0;
    g.sprite.requested = false;
    g.sprite.captured = false;
    m_profile.recycledBuffers = g.uploads.size();
    for (auto &buffer : g.uploads)
      g.recycle(std::move(buffer));
    g.uploads.clear();
    g.recycle(std::move(g.skin));
    m_profile.recycleMilliseconds = since(phase);
    auto part = Clock::now();
    m_profile.materialCount = g.materials.size();
    m_profile.textureCount = g.textures.size();
    m_profile.meshCount = g.meshes.size();
    m_profile.pathCount = m_paths.size();
    auto &retired = g.frames[g.frameIndex];
    g.launchSpareImageBlock();
    for (auto it = g.materials.begin(); it != g.materials.end();) {
      if (it->second->owner.expired()) {
        retired.retiredMaterials.push_back(std::move(it->second));
        it = g.materials.erase(it);
      } else
        ++it;
    }
    m_profile.retireMaterialsMilliseconds = since(part);
    part = Clock::now();
    for (auto it = g.textures.begin(); it != g.textures.end();) {
      if (it->second.owner.expired()) {
        retired.retiredImages.push_back(std::move(it->second.data));
        it = g.textures.erase(it);
      } else {
        // Textures rewritten in place: their changed regions reach the image
        // here, ahead of everything this frame records (raster and traced),
        // whichever of its tables holds the texture's descriptor.
        if (it->second.updatable)
          if (const auto owner = it->second.owner.lock()) {
            const auto begin = Clock::now();
            g.refreshTexture(it->second, *owner, command);
            m_profile.refreshMilliseconds += since(begin);
            ++m_profile.texturesRefreshed;
          }
        ++it;
      }
    }
    m_profile.retireTexturesMilliseconds = since(part);
    part = Clock::now();
    for (auto it = g.meshes.begin(); it != g.meshes.end();) {
      if (it->second.owner.expired()) {
        retired.retiredBuffers.push_back(std::move(it->second.data));
        it = g.meshes.erase(it);
      } else
        ++it;
    }
    m_profile.retireMeshesMilliseconds = since(part);
    part = Clock::now();
    prune(m_paths, [](const auto &entry) { return entry.second.expired(); });
    m_profile.retirePathsMilliseconds = since(part);
    m_profile.retireMilliseconds = since(phase) - m_profile.refreshMilliseconds;
    phase = Clock::now();
    auto layer = [](const Draw &d) {
      return d.ui                                          ? 3
             : d.camera == REMIXAPI_CAMERA_TYPE_SKY        ? 0
             : d.camera == REMIXAPI_CAMERA_TYPE_VIEW_MODEL ? 2
                                                           : 1;
    };
    auto distance = [&](const Draw &d) {
      const auto mvp = multiply(m_cameras[d.camera], d.model);
      const auto &c = d.surface->center;
      const float w = mvp[3] * c[0] + mvp[7] * c[1] + mvp[11] * c[2] + mvp[15];
      const float z = mvp[2] * c[0] + mvp[6] * c[1] + mvp[10] * c[2] + mvp[14];
      const float depth = z / w;
      return std::isfinite(depth) ? depth : 0.0f;
    };
    // Sort references with keys evaluated once; draws themselves hold shared owners.
    // `order` (remixapi_InstanceInfoRasterOrderEXT): a blended draw the host
    // placed itself, after the layer's depth-sorted ones.
    struct Entry {
      int layer;
      bool blended;
      float distance;
      const Draw *draw;
      uint32_t order = 0;
    };
    std::vector<Entry> ordered;
    ordered.reserve(m_draws.size() + m_overlay.size() + m_uiDraws.size() + 1);
    std::deque<Draw> adjusted;
    for (const auto &draw : m_draws) {
      if (draw.rasterVisible && !draw.reflectionOnly && (!tracing || !draw.traced())) {
        const Draw *source = &draw;
        // Traced barycentric depth and raster interpolation can differ by ULPs.
        // Bias coplanar EQUAL feedback toward the camera while preserving occlusion.
        if (tracing && draw.depthCompare == 2) {
          auto &copy = adjusted.emplace_back(draw);
          copy.depthCompare = 3;
          copy.depthBias = true;
          source = &copy;
        }
        // A masked overlay is drawn after its layer's opaque draws, which
        // precede the coverage passes (their depth is what sources test). So
        // is a rasterized CUT instance, drawn outside its group's coverage.
        const bool late = draw.blend.enabled || draw.maskGroup || draw.cutGroup;
        const uint32_t order = draw.blend.enabled && !draw.maskGroup && !draw.cutGroup ? draw.rasterOrder : 0;
        ordered.push_back({layer(draw), late, late && !order ? distance(draw) : 0.f, source, order});
      }
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const Entry &a, const Entry &b) {
      if (a.layer != b.layer) {
        return a.layer < b.layer;
      }
      if (a.blended != b.blended) {
        return !a.blended;
      }
      if (a.order || b.order) {
        return a.order && b.order ? a.order < b.order : !a.order;
      }
      return a.blended && a.distance > b.distance;
    });
    if (m_tint[3] > 0) {
      Draw tint;
      tint.ui = true;
      tint.depth = false;
      tint.doubleSided = true;
      tint.blend.enabled = true;
      tint.canvasWidth = tint.canvasHeight = 1;
      tint.material = std::make_shared<Material>();
      tint.material->texture = texturePath(nullptr);
      std::copy_n(m_tint, 4, tint.material->tint);
      tint.surface = std::make_shared<Surface>();
      tint.surface->vertices = {{{0, 0, 0}, {0, 0}, {1, 1, 1, 1}},
                                {{1, 0, 0}, {0, 0}, {1, 1, 1, 1}},
                                {{1, 1, 0}, {0, 0}, {1, 1, 1, 1}},
                                {{0, 1, 0}, {0, 0}, {1, 1, 1, 1}}};
      tint.surface->indices = {0, 1, 2, 0, 2, 3};
      ordered.push_back({0, false, 0, &adjusted.emplace_back(std::move(tint))});
    }
    for (const auto *list : {&m_overlay, &m_uiDraws})
      for (const auto &draw : *list)
        ordered.push_back({0, false, 0, &draw});
    m_profile.draws = ordered.size();
    m_profile.orderMilliseconds = since(phase);
    std::vector<Vertex> skinned;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g.epoch).count();
    // A halo or mask source: drawn as a silhouette into a coverage target
    // instead of the frame.
    struct Silhouette {
      Gpu::Halo::Group *group;     // counts the commands and bounds them on screen
      const VkPipeline *pipelines; // [depth tested]
      bool depthTest;              // honoured by WORLD instances only
      std::vector<Gpu::Command> *commands;
    };
    auto &masks = g.mask;
    const VkRect2D whole = {{0, 0}, g.extent};
    // Coverage exists for this frame's masked draws (set below).
    bool masking = false;
    // Frustum culling (REMIX_RASTER_FRUSTUM_CULL=0 turns it off): a draw whose
    // index range lies outside the view is left out before anything is looked
    // up or recorded for it. The test is conservative: the range's object-space
    // box against the four side planes, with a margin for the sky's sub-pixel
    // jitter, and against the eye plane; near and far are left to the clipper.
    // Not for draws whose vertex positions are not the mesh's (skinned, sprite
    // overlays), UI, or silhouettes, whose bounds size the coverage targets.
    static const bool cullEnabled = [] {
      const char *value = std::getenv("REMIX_RASTER_FRUSTUM_CULL");
      return !value || value[0] != '0';
    }();
    const auto outsideView = [&](const Draw &draw, Gpu::MeshGpu &mesh, const Matrix &m) {
      const Gpu::MeshGpu::RangeKey key = {draw.firstIndex,
                                          draw.indexCount ? draw.indexCount : uint32_t(draw.surface->indices.size()),
                                          draw.vertexOffset};
      auto found = mesh.ranges.find(key);
      if (found == mesh.ranges.end()) {
        Gpu::MeshGpu::Range range;
        const auto &indices = draw.surface->indices;
        const auto &vertices = draw.surface->vertices;
        range.valid = key.count && uint64_t(key.first) + key.count <= indices.size();
        std::fill_n(range.low, 3, std::numeric_limits<float>::max());
        std::fill_n(range.high, 3, std::numeric_limits<float>::lowest());
        for (uint32_t i = 0; i < key.count && range.valid; ++i) {
          const int64_t vertex = int64_t(indices[key.first + i]) + key.base;
          if (vertex < 0 || vertex >= int64_t(vertices.size())) {
            range.valid = false;
            break;
          }
          for (int c = 0; c < 3; ++c) {
            const float p = vertices[size_t(vertex)].position[c];
            range.valid = range.valid && std::isfinite(p);
            range.low[c] = std::min(range.low[c], p);
            range.high[c] = std::max(range.high[c], p);
          }
        }
        found = mesh.ranges.emplace(key, range).first;
      }
      const auto &range = found->second;
      if (!range.valid)
        return false;
      // Bits: outside left, right, bottom, top, behind the eye. A plane every
      // corner is outside of hides the box.
      uint32_t outside = 31;
      for (int corner = 0; corner < 8 && outside; ++corner) {
        const float p[3] = {corner & 1 ? range.high[0] : range.low[0], corner & 2 ? range.high[1] : range.low[1],
                            corner & 4 ? range.high[2] : range.low[2]};
        const float x = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
        const float y = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
        const float w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w))
          return false;
        const float edge = w + .01f * std::abs(w);
        outside &= uint32_t(x < -edge) | uint32_t(x > edge) << 1 | uint32_t(y < -edge) << 2 |
                   uint32_t(y > edge) << 3 | uint32_t(w <= 0) << 4;
      }
      return outside != 0;
    };
    // One command per draw.
    const auto build = [&](const Draw &draw, const Silhouette *silhouette) {
      const auto &mat = *draw.material;
      // A masked overlay samples its group's coverage. Without the machinery
      // it is left out; a group without sources covers nothing.
      // A rasterized CUT instance (role CUT is a scene instance: drawn here
      // when it is not traced) is an OUTSIDE draw that is never left out:
      // without coverage or the machinery it is drawn whole, as is a SKY one.
      const bool cut = !silhouette && !draw.maskGroup && draw.cutGroup;
      const uint32_t group = draw.maskGroup ? draw.maskGroup : draw.cutGroup;
      const bool outside = draw.maskOutside || cut;
      const bool masked = !silhouette && group;
      int32_t slot = masked && masking ? masks.slot[group] : -1;
      if (cut && draw.camera == REMIXAPI_CAMERA_TYPE_SKY)
        slot = -1;
      if (masked && !cut &&
          (!masking || draw.camera == REMIXAPI_CAMERA_TYPE_SKY || (slot < 0 && !outside) ||
           (slot >= 0 && !outside && !masks.area[slot].extent.width)))
        return;
      Gpu::Command out = {};
      out.push.mvp = multiply(m_cameras[draw.camera], draw.model);
      if (!draw.ui && !m_cameraValid[draw.camera] && draw.camera != REMIXAPI_CAMERA_TYPE_WORLD)
        out.push.mvp = multiply(m_cameras[REMIXAPI_CAMERA_TYPE_WORLD], draw.model);
      if (cullEnabled && !silhouette && !draw.ui && !draw.spriteFlags && !draw.surface->bonesPerVertex &&
          outsideView(draw, g.mesh(draw.surface, command), out.push.mvp)) {
        ++m_profile.culledDraws;
        return;
      }
      const bool tested = silhouette && silhouette->depthTest && draw.camera == REMIXAPI_CAMERA_TYPE_WORLD;
      // A sprite overlay (raster/sprite.cpp): a blended draw that is not masked.
      const bool sprite = !silhouette && draw.spriteFlags && draw.blend.enabled && !draw.maskGroup && !draw.cutGroup &&
                          !draw.ui && g.sprite.layout && g.sprite.vertex && g.sprite.fragment && !g.sprite.failed;
      try {
        out.pipeline = silhouette ? silhouette->pipelines[tested] : g.pipeline(draw, slot >= 0, sprite);
      } catch (VkResult result) {
        // With the path tracer no draw's variant is waited for
        // (preparePipelines): the draw is left out until it has compiled.
        if (result == VK_NOT_READY && tracing && !silhouette && (!masked || !cut)) {
          ++m_profile.unreadyDraws;
          return;
        }
        // A masked draw's variant is still compiling (preparePipelines does
        // not wait for it): left out of this frame, a CUT instance drawn whole.
        if (!masked || result != VK_NOT_READY || (cut && slot < 0))
          throw;
        if (!cut)
          return;
        slot = -1;
        out.pipeline = g.pipeline(draw, false);
      }
      const size_t meshesBefore = g.meshes.size(), materialsBefore = g.materials.size();
      auto &mesh = g.mesh(draw.surface, command);
      out.set = g.material(draw.material, command);
      m_profile.meshesCreated += g.meshes.size() - meshesBefore;
      m_profile.materialsCreated += g.materials.size() - materialsBefore;
      if (slot >= 0) {
        out.coverage = masks.targets[slot].set;
        out.area = outside ? whole : masks.area[slot];
      }
      const size_t firstSkinned = skinned.size();
      out.vertices = out.indices = mesh.data->buffer;
      out.vertexOffset = mesh.data->offset;
      out.indexOffset = mesh.data->offset + mesh.indexOffset;
      if (sprite) {
        out.sprite = true;
        out.spriteBuffer = mesh.data->buffer;
        // A mesh without sprite vertices: any data (its flags that read them are off).
        out.spriteOffset = mesh.data->offset + (draw.surface->sprite.empty() ? 0 : mesh.spriteOffset);
        const auto &blend = draw.blend;
        out.spriteParameters[0] = draw.spriteSoft[0] > 0 ? 1 / draw.spriteSoft[0] : 0;
        out.spriteParameters[1] = draw.spriteSoft[1];
        out.spriteParameters[2] = draw.spriteSoft[2];
        // MODULATE's k: dst x src + src x dst, or dst x src alone.
        out.spriteParameters[3] = blend.src == VK_BLEND_FACTOR_DST_COLOR && blend.dst == VK_BLEND_FACTOR_SRC_COLOR ? 2.f : 1.f;
      }
      // The captured depth is the world camera's.
      const uint32_t spriteFlags =
          !sprite ? 0u
                  : draw.spriteFlags & (draw.camera == REMIXAPI_CAMERA_TYPE_WORLD
                                            ? 0x1ffu
                                            : 0x1ffu & ~uint32_t(REMIXAPI_SPRITE_SOFT_DEPTH_EXT));
      g.sprite.soft += (spriteFlags & REMIXAPI_SPRITE_SOFT_DEPTH_EXT) != 0;
      out.firstIndex = draw.firstIndex;
      out.count = draw.indexCount ? draw.indexCount : uint32_t(draw.surface->indices.size());
      out.baseVertex = draw.vertexOffset;
      out.layer = layer(draw);
      if (draw.surface->bonesPerVertex) {
        ScopedTicks skinTime{finish.skinning};
        m_profile.skinnedVertices += draw.surface->vertices.size();
        out.vertices = VK_NULL_HANDLE;
        out.vertexOffset = skinned.size() * sizeof(Vertex);
        if (skinned.size() + draw.surface->vertices.size() > 8 * 1024 * 1024) {
          throw VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        for (size_t v = 0; v < draw.surface->vertices.size(); ++v) {
          Vertex vertex = draw.surface->vertices[v];
          const auto original = vertex;
          std::fill_n(vertex.position, 3, 0.0f);
          for (uint32_t b = 0; b < draw.surface->bonesPerVertex; ++b) {
            const size_t index = v * draw.surface->bonesPerVertex + b;
            const auto &m = (*draw.bones)[draw.surface->bones[index]];
            for (int r = 0; r < 3; ++r) {
              vertex.position[r] += draw.surface->weights[index] *
                                    (m[r] * original.position[0] + m[4 + r] * original.position[1] +
                                     m[8 + r] * original.position[2] + m[12 + r]);
            }
          }
          skinned.push_back(vertex);
        }
      }
      if (draw.ui) {
        out.push.mvp = identity();
        out.push.mvp[0] = 2.0f / draw.canvasWidth;
        out.push.mvp[5] = 2.0f / draw.canvasHeight;
        out.push.mvp[12] = out.push.mvp[13] = -1;
      }
      std::copy_n(mat.tint, 4, out.push.tint);
      const uint32_t frame = uint32_t(std::fmod(seconds * mat.fps, uint32_t(mat.rows) * mat.columns));
      out.push.uv[0] = 1.0f / mat.columns;
      out.push.uv[1] = 1.0f / mat.rows;
      out.push.uv[2] = float(frame % mat.columns) / mat.columns;
      out.push.uv[3] = float(frame / mat.columns) / mat.rows;
      const auto &b = draw.blend;
      out.push.state[0] = (b.colorOp & 3u) | (b.colorOp & 4u) << 10 | (b.alphaOpTexture & 4u) << 11 |
                          b.color1 << 2 | b.color2 << 4 | (b.alphaOpTexture & 3u) << 6 | b.alpha1 << 8 |
                          b.alpha2 << 10;
      out.push.state[1] = draw.ignoreAlpha ? 7 : draw.alphaOverride ? draw.compare : mat.compare;
      const float reference = draw.alphaOverride ? draw.reference : mat.reference;
      std::memcpy(&out.push.state[2], &reference, sizeof(reference));
      out.push.state[3] =
          uint32_t(draw.ignoreAlpha) |
          (g.encodeOutput && !(g.skyPass && draw.camera == REMIXAPI_CAMERA_TYPE_SKY) ? 2u : 0u) |
          (slot >= 0 && outside ? 16u : 0u) | spriteFlags << 5;
      out.push.factor[0] = linear(float((b.factor >> 16) & 255) / 255);
      out.push.factor[1] = linear(float((b.factor >> 8) & 255) / 255);
      out.push.factor[2] = linear(float(b.factor & 255) / 255);
      out.push.factor[3] = float(b.factor >> 24) / 255;
      if (!silhouette) {
        g.commands.push_back(out);
        return;
      }
      auto *bounds = silhouette->group;
      // Screen bounds of the silhouette, from the vertices' bounding box.
      float low[3], high[3];
      const auto extend = [&](const Vertex *vertices, size_t count, float *lo, float *hi) {
        std::fill_n(lo, 3, std::numeric_limits<float>::max());
        std::fill_n(hi, 3, std::numeric_limits<float>::lowest());
        for (size_t v = 0; v < count; ++v)
          for (int c = 0; c < 3; ++c) {
            lo[c] = std::min(lo[c], vertices[v].position[c]);
            hi[c] = std::max(hi[c], vertices[v].position[c]);
          }
      };
      if (draw.surface->bonesPerVertex) {
        extend(skinned.data() + firstSkinned, skinned.size() - firstSkinned, low, high);
      } else {
        if (!mesh.bounded) {
          extend(draw.surface->vertices.data(), draw.surface->vertices.size(), mesh.low, mesh.high);
          mesh.bounded = true;
        }
        std::copy_n(mesh.low, 3, low);
        std::copy_n(mesh.high, 3, high);
      }
      bool bounded = low[0] <= high[0];
      float area[4] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                       std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
      const auto &m = out.push.mvp;
      for (int corner = 0; corner < 8 && bounded; ++corner) {
        const float p[3] = {corner & 1 ? high[0] : low[0], corner & 2 ? high[1] : low[1],
                            corner & 4 ? high[2] : low[2]};
        const float x = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
        const float y = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
        const float w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
        // A box reaching behind the camera has no finite projection.
        if (!(w > 1e-4f) || !std::isfinite(x) || !std::isfinite(y)) {
          bounded = false;
          break;
        }
        area[0] = std::min(area[0], x / w);
        area[1] = std::min(area[1], y / w);
        area[2] = std::max(area[2], x / w);
        area[3] = std::max(area[3], y / w);
      }
      if (!bounded) {
        area[0] = area[1] = -1;
        area[2] = area[3] = 1;
      }
      if (!bounds->bounded) {
        std::copy_n(area, 2, bounds->low);
        std::copy_n(area + 2, 2, bounds->high);
        bounds->bounded = true;
      } else
        for (int c = 0; c < 2; ++c) {
          bounds->low[c] = std::min(bounds->low[c], area[c]);
          bounds->high[c] = std::max(bounds->high[c], area[2 + c]);
        }
      silhouette->commands->push_back(out);
      ++bounds->count;
    };
    // Mask sources (raster/mask.cpp): a coverage target per group and a pass
    // per run, when the machinery is ready (preparePipelines). Before the
    // frame's draws, which need to know whether their group has coverage.
    masks.runs.clear();
    masks.commands.clear();
    masks.used = 0;
    masks.cut = {};
    std::fill(std::begin(masks.slot), std::end(masks.slot), -1);
    masking = masks.ready && !masks.failed;
    if (!m_masks.empty() && masking) {
      struct Plan {
        const MaskRun *run;
        Gpu::Mask::Run pass;
      };
      std::vector<Plan> plans;
      for (const auto &run : m_masks) {
        Gpu::Mask::Run pass;
        if (run.mask.within) {
          // Limited to a group without coverage so far: covers nothing.
          pass.within = masks.slot[run.mask.within];
          if (pass.within < 0)
            continue;
        }
        auto &slot = masks.slot[run.mask.group];
        pass.clear = slot < 0;
        if (pass.clear)
          slot = int32_t(masks.used++);
        pass.target = uint32_t(slot);
        plans.push_back({&run, pass});
      }
      try {
        g.createMaskTargets(masks.used);
      } catch (VkResult result) {
        if (result == VK_ERROR_DEVICE_LOST)
          throw;
        masks.failed = true;
        masking = false;
        masks.used = 0;
        plans.clear();
      }
      const auto right = [](const VkRect2D &r) { return r.offset.x + int32_t(r.extent.width); };
      const auto bottom = [](const VkRect2D &r) { return r.offset.y + int32_t(r.extent.height); };
      for (auto &plan : plans) {
        Gpu::Halo::Group bounds;
        bounds.first = uint32_t(masks.commands.size());
        const Silhouette silhouette = {&bounds, plan.pass.within >= 0 ? masks.within : g.halo.mask,
                                       plan.run->mask.depthTest, &masks.commands};
        for (const auto &draw : plan.run->draws)
          build(draw, &silhouette);
        plan.pass.first = bounds.first;
        plan.pass.count = bounds.count;
        masks.runs.push_back(plan.pass);
        // The pixels the group's coverage can be set in: masked INSIDE draws
        // are confined to them.
        const float margin[2] = {1, 1};
        VkRect2D area = {};
        if (g.coverageArea(bounds, margin))
          area = bounds.area;
        if (plan.pass.within >= 0 && area.extent.width) {
          const auto &limit = masks.area[plan.pass.within];
          const int32_t x0 = std::max(area.offset.x, limit.offset.x), y0 = std::max(area.offset.y, limit.offset.y);
          const int32_t x1 = std::min(right(area), right(limit)), y1 = std::min(bottom(area), bottom(limit));
          area = x1 > x0 && y1 > y0 && limit.extent.width
                     ? VkRect2D{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}}
                     : VkRect2D{};
        }
        auto &total = masks.area[plan.pass.target];
        if (plan.pass.clear || !total.extent.width)
          total = area;
        else if (area.extent.width) {
          const int32_t x0 = std::min(area.offset.x, total.offset.x), y0 = std::min(area.offset.y, total.offset.y);
          const int32_t x1 = std::max(right(area), right(total)), y1 = std::max(bottom(area), bottom(total));
          total = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
        }
      }
      m_profile.maskGroups = masks.used;
      m_profile.maskDraws = masks.commands.size();
      // Role CUT with a traced scene: the groups that hide traced instances
      // from the camera (the lowest numbered with coverage on screen) and the
      // runs their coverage needs, which are drawn ahead of the path tracer
      // (recordCutCoverage) as well.
      if (tracing && m_cutGroups && !plans.empty()) {
        auto &cut = masks.cut;
        uint32_t early = 0; // groups, bit group - 1
        for (uint32_t group = 1; group <= Mask::maxGroup && cut.count < Mask::maxCutGroups; ++group) {
          const int32_t target = masks.slot[group];
          if (!(m_cutGroups >> (group - 1) & 1) || target < 0 || !masks.area[target].extent.width)
            continue;
          early |= 1u << (group - 1);
          cut.targets[cut.count] = uint32_t(target);
          cut.slots[(group - 1) >> 3] |= (cut.count + 1) << (((group - 1) & 7) * 4);
          ++cut.count;
        }
        // And the groups those are limited to, transitively.
        for (bool grown = early != 0; grown;) {
          grown = false;
          for (const auto &plan : plans) {
            const auto &mask = plan.run->mask;
            if ((early >> (mask.group - 1) & 1) && mask.within && !(early >> (mask.within - 1) & 1)) {
              early |= 1u << (mask.within - 1);
              grown = true;
            }
          }
        }
        // The tracer reuses its output while its inputs are unchanged: this
        // is what changes when the coverage may have.
        uint64_t digest = hashBytes(cut.slots, sizeof(cut.slots));
        for (size_t i = 0; i < plans.size(); ++i) {
          const auto &run = *plans[i].run;
          if (!(early >> (run.mask.group - 1) & 1))
            continue;
          auto &pass = masks.runs[i];
          pass.early = true;
          const uint32_t parameters[] = {run.mask.group, run.mask.within, pass.count};
          digest = hashBytes(parameters, sizeof(parameters), digest);
          for (uint32_t c = pass.first; c < pass.first + pass.count; ++c) {
            const auto &source = masks.commands[c];
            digest = hashBytes(&source.push, sizeof(source.push), digest);
            const uint64_t values[] = {source.firstIndex, source.count, uint64_t(int64_t(source.baseVertex))};
            digest = hashBytes(values, sizeof(values), digest);
          }
          for (const auto &draw : run.draws) {
            const uintptr_t identity[] = {reinterpret_cast<uintptr_t>(draw.surface.get()),
                                          reinterpret_cast<uintptr_t>(draw.material.get())};
            digest = hashBytes(identity, sizeof(identity), digest);
            if (draw.bones)
              digest = hashBytes(draw.bones->data(), draw.bones->size() * sizeof(Matrix), digest);
          }
        }
        cut.digest = digest;
      }
    }
    phase = Clock::now();
    for (const auto &entry : ordered)
      build(*entry.draw, nullptr);
    m_profile.buildMilliseconds = since(phase);
    // The coverage passes run before the first masked world draw, or after
    // the world layer.
    masks.split = g.commands.size();
    if (!masks.runs.empty())
      for (size_t i = 0; i < g.commands.size(); ++i)
        if (g.commands[i].layer >= 2 || (g.commands[i].layer == 1 && g.commands[i].coverage)) {
          masks.split = i;
          break;
        }
    // Halo sources: one group of mask commands per halo, when its passes are
    // ready (preparePipelines) and the group reaches the screen.
    g.halo.groups.clear();
    g.halo.commands.clear();
    if (!m_halos.empty() && g.halo.ready && !g.halo.failed) {
      try {
        g.createHaloTargets();
      } catch (VkResult result) {
        g.destroyHaloTargets();
        if (result == VK_ERROR_DEVICE_LOST)
          throw;
        g.halo.failed = true;
      }
    }
    if (!m_halos.empty() && g.halo.ready && !g.halo.failed) {
      for (const auto &source : m_halos) {
        Gpu::Halo::Group group;
        group.halo = source.halo;
        group.first = uint32_t(g.halo.commands.size());
        const Silhouette silhouette = {&group, g.halo.mask, !source.halo.ignoreZ, &g.halo.commands};
        for (const auto &draw : source.draws)
          build(draw, &silhouette);
        m_profile.haloDraws += group.count;
        if (g.haloArea(group))
          g.halo.groups.push_back(group);
      }
      m_profile.haloGroups = g.halo.groups.size();
      m_profile.draws += m_profile.haloDraws;
    }
    m_profile.draws += m_profile.maskDraws;
    static const bool digestEnabled = std::getenv("REMIX_FRAME_DIGEST") != nullptr;
    if (digestEnabled) {
      uint64_t digest = hashBytes(skinned.data(), skinned.size() * sizeof(Vertex));
      for (const auto *list : {&g.commands, &g.halo.commands, &masks.commands})
        for (const auto &command : *list) {
          const uint64_t values[] = {command.vertices ? 0u : command.vertexOffset, command.firstIndex,
                                     command.count, uint64_t(int64_t(command.baseVertex)),
                                     uint64_t(command.layer)};
          digest = hashBytes(values, sizeof(values), digest);
          digest = hashBytes(&command.push, sizeof(command.push), digest);
        }
      m_profile.digest = digest;
    }
    if (!skinned.empty()) {
      const auto skinBegin = Clock::now();
      g.skin = g.geometryBuffer(skinned.data(), skinned.size() * sizeof(Vertex),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, command);
      m_profile.skinUploadMilliseconds = since(skinBegin);
      for (auto *list : {&g.commands, &g.halo.commands, &masks.commands})
        for (auto &draw : *list) {
          if (!draw.vertices) {
            draw.vertices = g.skin->buffer;
            draw.vertexOffset += g.skin->offset;
          }
        }
    }
    m_profile.stagingAllocateMilliseconds = g.stagingAllocateMs;
    m_profile.stagingWriteMilliseconds = g.stagingWriteMs;
    m_profile.stagingBuffers = g.stagingBuffers;
    m_profile.stagingAllocated = g.stagingAllocated;
    m_profile.stagingMemoryFlags = g.stagingMemoryFlags;
    m_profile.createPipelineMilliseconds = g.createPipelineMs;
    m_profile.createMeshMilliseconds = g.createMeshMs;
    m_profile.createTextureMilliseconds = g.createTextureMs;
    m_profile.createMaterialMilliseconds = g.createMaterialMs - g.createTextureMs;
    m_profile.pipelinesCreated = g.pipelinesCreated;
    m_profile.texturesCreated = g.texturesCreated;
  }

  void RasterRenderer::record(VkCommandBuffer command, bool skyOnly, bool skipSky, const float *skyJitter,
                              VkExtent2D skyExtent, int firstLayer, int lastLayer, int part) {
    auto &g = *m_gpu;
    const auto extent = skyOnly && skyExtent.width ? skyExtent : g.extent;
    const VkViewport viewport = {0, 0, float(extent.width), float(extent.height), 0, 1};
    const VkRect2D scissor = {{0, 0}, extent};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    int previousLayer = -1;
    const size_t split = std::min(g.mask.split, g.commands.size());
    const size_t begin = part == 2 ? split : 0, end = part == 1 ? split : g.commands.size();
    auto confined = scissor;
    const auto same = [](const VkRect2D &a, const VkRect2D &b) {
      return a.offset.x == b.offset.x && a.offset.y == b.offset.y && a.extent.width == b.extent.width &&
             a.extent.height == b.extent.height;
    };
    // What this call has bound: consecutive draws mostly share it (the parts
    // of one mesh, the draws of one material), and each bind left out is a
    // command the driver does not validate.
    struct Bound {
      VkPipeline pipeline = VK_NULL_HANDLE;
      VkPipelineLayout layout = VK_NULL_HANDLE;
      VkDescriptorSet sets[2] = {};
      uint32_t setCount = 0;
      VkBuffer vertices = VK_NULL_HANDLE, sprite = VK_NULL_HANDLE, indices = VK_NULL_HANDLE;
      VkDeviceSize vertexOffset = 0, spriteOffset = 0, indexOffset = 0;
      float blend[4] = {};
      bool blendSet = false;
    } bound;
    for (size_t index = begin; index < end; ++index) {
      const auto &draw = g.commands[index];
      if ((skyOnly && draw.layer != 0) || (skipSky && draw.layer == 0) || draw.layer < firstLayer ||
          draw.layer > lastLayer)
        continue;
      if (draw.layer != previousLayer && draw.layer >= 2) {
        VkClearAttachment clear = {};
        clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        clear.clearValue.depthStencil.depth = 1;
        const VkClearRect rect = {scissor, 0, 1};
        vkCmdClearAttachments(command, 1, &clear, 1, &rect);
      }
      previousLayer = draw.layer;
      // A masked overlay: its group's coverage as set 1, confined to the
      // pixels the coverage can be set in.
      const auto layout = draw.coverage ? g.mask.layout : draw.sprite ? g.sprite.layout : g.layout;
      const auto &wanted = draw.coverage ? draw.area : scissor;
      if (!same(wanted, confined)) {
        vkCmdSetScissor(command, 0, 1, &wanted);
        confined = wanted;
      }
      if (draw.pipeline != bound.pipeline) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline);
        bound.pipeline = draw.pipeline;
      }
      // A sprite overlay: the scene's depth as set 1 (its own texture stands in
      // while there is none), its sprite vertices as binding 1.
      const bool soft = draw.sprite && g.sprite.captured && g.sprite.set;
      const VkDescriptorSet sets[] = {draw.set, draw.sprite ? (soft ? g.sprite.set : draw.set) : draw.coverage};
      const uint32_t setCount = draw.coverage || draw.sprite ? 2 : 1;
      if (layout != bound.layout || setCount != bound.setCount || sets[0] != bound.sets[0] ||
          (setCount == 2 && sets[1] != bound.sets[1])) {
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, setCount, sets, 0, nullptr);
        bound.layout = layout;
        bound.setCount = setCount;
        bound.sets[0] = sets[0];
        bound.sets[1] = sets[1];
      }
      if (draw.vertices != bound.vertices || draw.vertexOffset != bound.vertexOffset) {
        vkCmdBindVertexBuffers(command, 0, 1, &draw.vertices, &draw.vertexOffset);
        bound.vertices = draw.vertices;
        bound.vertexOffset = draw.vertexOffset;
      }
      if (draw.sprite && (draw.spriteBuffer != bound.sprite || draw.spriteOffset != bound.spriteOffset)) {
        vkCmdBindVertexBuffers(command, 1, 1, &draw.spriteBuffer, &draw.spriteOffset);
        bound.sprite = draw.spriteBuffer;
        bound.spriteOffset = draw.spriteOffset;
      }
      if (draw.indices != bound.indices || draw.indexOffset != bound.indexOffset) {
        vkCmdBindIndexBuffer(command, draw.indices, draw.indexOffset, VK_INDEX_TYPE_UINT32);
        bound.indices = draw.indices;
        bound.indexOffset = draw.indexOffset;
      }
      if (!bound.blendSet || std::memcmp(bound.blend, draw.push.factor, sizeof(bound.blend))) {
        vkCmdSetBlendConstants(command, draw.push.factor);
        std::copy_n(draw.push.factor, 4, bound.blend);
        bound.blendSet = true;
      }
      auto push = draw.push;
      if (draw.sprite) {
        std::copy_n(draw.spriteParameters, 4, push.factor);
        if (!soft)
          push.state[3] &= ~(REMIXAPI_SPRITE_SOFT_DEPTH_EXT << 5);
      }
      if (skyJitter && draw.layer == 0) {
        // Match the primary ray sample; raster overlays keep their center sample.
        for (int column = 0; column < 4; ++column) {
          push.mvp[column * 4] -= 2 * skyJitter[0] / extent.width * push.mvp[column * 4 + 3];
          push.mvp[column * 4 + 1] -= 2 * skyJitter[1] / extent.height * push.mvp[column * 4 + 3];
        }
      }
      vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(Push), &push);
      vkCmdDrawIndexed(command, draw.count, 1, draw.firstIndex, draw.baseVertex, 0);
    }
  }

} // namespace remix

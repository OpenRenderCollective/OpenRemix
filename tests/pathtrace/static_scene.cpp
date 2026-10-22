// The persistent static set (rtx.native.scene.persistentStatic) against the
// rebuild path: one retained scene, traced every frame by a pipeline that keeps
// the static set's tables and by one that rebuilds everything from the draw
// list, must produce identical trace frames, surface tables, TLAS inputs,
// signatures, descriptors and temporal fields, through updates, visibility
// changes, destroys, mesh and material replacement, dynamic/static
// transitions, history resets and uncommitted frames. CPU only: the mesh
// views are stand-ins with addresses derived from their content.
#include "pathtrace/frame_tables.h"
#include "platform/render_thread.h"
#include "scene/fixtures.h"
#include "scene/static_scene.h"
#include <atomic>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>

using namespace remix;
using namespace remix::test;

namespace {
  struct FakeBuffer {
    uint64_t address = 0;
  };
  struct FakeGroup {
    uint32_t surface = 0;
    uint64_t indices = 0, triangleMap = 0;
  };
  struct FakeGeometry {
    std::unique_ptr<FakeBuffer> input;
    std::shared_ptr<FakeBuffer> accel;
    std::vector<uint64_t> indexOffsets;
    std::vector<FakeGroup> groups;
    uint64_t fingerprint = 0, revision = 1;
    float diagonal = 1;
  };
  struct FakeView {
    std::shared_ptr<FakeGeometry> geometry;
    std::vector<std::shared_ptr<FakeBuffer>> vertices;
    uint64_t fingerprint = 0, contentDigest = 0;
  };
  uint64_t mix(std::initializer_list<uint64_t> values) {
    uint64_t h = 1469598103934665603ull;
    for (const uint64_t v : values)
      h = scene::hashBytes(&v, sizeof(v), h);
    return h;
  }
  // The tracer's view lookup, with addresses from (mesh, opacity key).
  struct FakeEnv {
    using View = FakeView;
    using VertexTable = scene::TemporalTable<std::shared_ptr<FakeBuffer>>;
    using Span = scene::TraceFrame::InstanceList::ConstSpan;
    std::map<std::pair<const scene::Mesh *, std::string>, std::unique_ptr<FakeView>> views;
    std::map<const scene::Mesh *, uint64_t> compactions;
    uint64_t conversions = 0;
    static std::string opaque(Span instance) {
      std::vector<std::string> keys(instance[0].draw.mesh->surfaces.size());
      for (const auto &surface : instance)
        keys[surface.draw.surfaceIndex] = surface.opacity->key;
      std::string out;
      for (const auto &key : keys)
        out += key;
      return out;
    }
    uint64_t accel(const scene::Mesh *mesh, const std::string &key) {
      return mix({uint64_t(uintptr_t(mesh)), scene::fastHash(key.data(), key.size()), compactions[mesh], 3});
    }
    void resolveViews(const std::vector<Span> &instances, std::vector<const FakeView *> &out) {
      out.clear();
      for (const auto instance : instances) {
        const auto *mesh = instance[0].draw.mesh.get();
        auto key = std::make_pair(mesh, opaque(instance));
        auto &view = views[key];
        if (!view) {
          ++conversions;
          view = std::make_unique<FakeView>();
          const uint64_t seed = mix({uint64_t(uintptr_t(mesh)), scene::fastHash(key.second.data(), key.second.size())});
          auto geometry = std::make_shared<FakeGeometry>();
          geometry->input = std::make_unique<FakeBuffer>(FakeBuffer{mix({seed, 1}) & ~0xffffull});
          geometry->accel = std::make_shared<FakeBuffer>(FakeBuffer{accel(mesh, key.second)});
          geometry->fingerprint = mix({seed, 2});
          for (uint32_t s = 0; s < mesh->surfaces.size(); ++s)
            geometry->indexOffsets.push_back(64ull * s);
          // One group per surface, two where a partition is mixed.
          size_t cursor = 0;
          for (uint32_t s = 0; s < mesh->surfaces.size(); ++s) {
            const char kind = key.second[cursor];
            const size_t triangles = mesh->surfaces[s]->indices.size() / 3;
            const size_t length = kind == 2 ? 1 + (triangles + 7) / 8 : 1;
            cursor += length;
            if (kind == 2) {
              geometry->groups.push_back({s, 4096 + s * 128ull, 8192 + s * 128ull});
              geometry->groups.push_back({s, 4096 + s * 128ull + 64, 8192 + s * 128ull + 64});
            } else {
              geometry->groups.push_back({s, 4096 + s * 128ull, 0});
            }
          }
          geometry->diagonal = float(mesh->surfaces.size());
          view->geometry = geometry;
          for (uint32_t s = 0; s < mesh->surfaces.size(); ++s)
            view->vertices.push_back(std::make_shared<FakeBuffer>(FakeBuffer{mix({seed, 100 + s})}));
          view->fingerprint = mix({seed, 5});
          view->contentDigest = mix({seed, 6});
        }
        out.push_back(view.get());
      }
    }
    bool findViews(const std::vector<Span> &instances, std::vector<const FakeView *> &out) {
      out.clear();
      for (const auto instance : instances) {
        const auto found = views.find({instance[0].draw.mesh.get(), opaque(instance)});
        if (found == views.end())
          return false;
        out.push_back(found->second.get());
      }
      return true;
    }
    std::shared_ptr<const void> owner(const FakeView &view, uint32_t surface) const { return view.vertices.at(surface); }
    [[noreturn]] void tooManyObjects() const { throw std::runtime_error("too many descriptors"); }
    // A BLAS compaction: the mesh's views move.
    std::vector<const void *> compact(const scene::Mesh *mesh) {
      ++compactions[mesh];
      std::vector<const void *> moved;
      for (auto &[key, view] : views)
        if (key.first == mesh) {
          view->geometry->accel = std::make_shared<FakeBuffer>(FakeBuffer{accel(mesh, key.second)});
          moved.push_back(view->geometry.get());
        }
      return moved;
    }
  };

  // One tracer: its temporal history, opacity cache and frame tables.
  struct Pipeline {
    bool persistent = false;
    scene::StaticScene statics;
    scene::OpacityCache opacity;
    scene::SurfaceEmissionCache emission;
    scene::TemporalHistory history;
    FakeEnv::VertexTable previous, pending;
    FakeEnv env;
    trace::FrameTables<FakeEnv> tables;
    WorkerPool workers{2};
    uint64_t serial = 0;
    std::vector<std::shared_ptr<const void>> retired;
    scene::TraceFrame frame;
    trace::SceneSignature signature, last;
    bool changed = false;
    scene::TemporalEpoch epoch{};
    bool filterEmission = false;
    std::vector<scene::TraceFrame::Surface> traced; // the frame's instances with the set's, in trace order
    std::string validation;

    void prepare(NativeResources &resources) {
      frame = scene::prepareSceneFrame(resources, persistent ? &statics : nullptr, opacity, &workers, false, 0.25,
                                       filterEmission, emission);
      // The traced instances before the tables consume them.
      traced.clear();
      std::vector<std::vector<scene::TraceFrame::Surface>> instances;
      if (frame.staticUnits)
        for (const uint32_t slot : statics.order())
          if (!statics.units()[slot].singular)
            instances.push_back(statics.units()[slot].surfaces);
      for (const auto instance : frame.instances)
        instances.emplace_back(instance.begin(), instance.end());
      std::stable_sort(instances.begin(), instances.end(), [](const auto &a, const auto &b) {
        return std::tie(a[0].draw.classification, a[0].draw.instanceId, a[0].draw.instanceIndex) <
               std::tie(b[0].draw.classification, b[0].draw.instanceId, b[0].draw.instanceIndex);
      });
      for (const auto &instance : instances)
        traced.insert(traced.end(), instance.begin(), instance.end());
      history.begin(epoch, 7, 64, 64);
      pending.clear();
      history.setFallback(&statics);
      const trace::FrameTables<FakeEnv>::Inputs in{frame, &statics, history, previous, pending, workers, ++serial,
                                                   true, true, 4096};
      tables.build(env, in);
      validation = tables.validate(env, in);
      signature = {};
      tables.signature(signature);
      changed = !signature.equal(last);
      last = signature;
    }
    void commit() {
      history.commit();
      statics.committed(serial, retired);
      retired.clear();
      previous = std::move(pending);
      pending = {};
    }
    void compact(const scene::Mesh *mesh) {
      for (const void *geometry : env.compact(mesh))
        tables.compacted(geometry);
    }
  };

  bool sameDraw(const scene::Draw &a, const scene::Draw &b) {
    return a.mesh == b.mesh && a.surface == b.surface && a.material == b.material && a.model == b.model &&
           a.instanceId == b.instanceId && a.instanceIndex == b.instanceIndex && a.surfaceIndex == b.surfaceIndex &&
           a.classification == b.classification && a.decal == b.decal && a.blend.enabled == b.blend.enabled &&
           a.doubleSided == b.doubleSided && a.bones == b.bones;
  }

  void compare(const Pipeline &a, const Pipeline &b, int frame) {
    char where[64];
    std::snprintf(where, sizeof(where), "frame %d", frame);
    auto check = [&](bool value, const char *what) {
      if (!value) {
        std::fprintf(stderr, "Static scene test failed at %s: %s\n", where, what);
        std::exit(1);
      }
    };
    check(a.frame.emitters.size() == b.frame.emitters.size(), "emitter count");
    for (size_t i = 0; i < a.frame.emitters.size(); ++i) {
      const auto &x = a.frame.emitters[i], &y = b.frame.emitters[i];
      check(x.light == y.light && x.layout == y.layout && x.animated == y.animated, "emitter");
    }
    check(a.traced.size() == b.traced.size(), "traced surface count");
    for (size_t i = 0; i < a.traced.size(); ++i) {
      const auto &x = a.traced[i], &y = b.traced[i];
      check(sameDraw(x.draw, y.draw), "traced draw");
      check(x.uv == y.uv && x.animationFrame == y.animationFrame, "traced UVs");
      check(x.opacity->key == y.opacity->key, "opacity partition");
    }
    const auto &p = a.tables, &q = b.tables;
    const auto optics = p.sheetMaterials();
    check(optics == q.sheetMaterials(), "descriptor-indexed sheet optics");
    check(p.surfaceCount() == q.surfaceCount(), "surface table size");
    for (size_t i = 0; i < p.surfaceCount(); ++i) {
      check(!std::memcmp(&p.surface(i), &q.surface(i), sizeof(trace::TraceSurface)), "surface table entry");
      const auto &surface = p.surface(i);
      if (surface.material[0] & trace::measuredSheetMaterialFlag)
        check(surface.textureState[0] < optics.size() && optics[surface.textureState[0]].parameters[1] > 0,
              "measured surface addresses its material's optical data");
    }
    check(p.primaryCount() == q.primaryCount(), "primary draw count");
    for (size_t i = 0; i < p.primaryCount(); ++i) {
      const auto &x = p.primary(i), &y = q.primary(i);
      check(x.model == y.model && x.surfaceIndex == y.surfaceIndex && x.count == y.count && x.blended == y.blended,
            "primary draw");
    }
    check(p.staticInputs() == q.staticInputs(), "static TLAS inputs");
    check(p.dynamicInputs() == q.dynamicInputs(), "dynamic TLAS inputs");
    check(p.staticBytes() == q.staticBytes(), "SHARC static geometry");
    check(p.dynamicBytes() == q.dynamicBytes(), "SHARC dynamic geometry");
    check(p.requestCount() == q.requestCount(), "descriptor count");
    for (size_t i = 0; i < p.requestCount(); ++i)
      check(p.request(i) == q.request(i), "descriptor");
    check(p.flags() == q.flags(), "surface flags");
    check(p.geometryDigest() == q.geometryDigest(), "geometry digest");
    check(p.reuseSignature() == q.reuseSignature(), "PBR reuse signature");
    check(p.instanceCount() == q.instanceCount(), "instance count");
    check(a.signature.bytes() == b.signature.bytes(), "scene signature bytes");
    check(a.signature.equal(b.signature) && b.signature.equal(a.signature), "segmented signature comparison");
    check(a.changed == b.changed, "scene change detection");
    check(a.validation.empty(), a.validation.c_str());
  }

  remixapi_MeshHandle twoSurfaces(NativeResources &r, uint64_t hash, remixapi_MaterialHandle first,
                                  remixapi_MaterialHandle second, float offset = 0) {
    remixapi_HardcodedVertex vertices[4] = {};
    const float positions[4][3] = {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
    for (int i = 0; i < 4; ++i) {
      std::copy_n(positions[i], 3, vertices[i].position);
      vertices[i].position[0] += offset;
      vertices[i].color = 0xffffffff;
      vertices[i].texcoord[0] = i == 1 || i == 2 ? .9f : .1f;
      vertices[i].texcoord[1] = .5f;
    }
    const uint32_t a[] = {0, 1, 2}, b[] = {0, 2, 3};
    remixapi_MeshInfoSurfaceTriangles surfaces[2] = {};
    surfaces[0] = {vertices, 4, a, 3, {}, {}, first};
    surfaces[1] = {vertices, 4, b, 3, {}, {}, second};
    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = hash;
    info.surfaces_values = surfaces;
    info.surfaces_count = 2;
    remixapi_MeshHandle result;
    r.createMesh(info, result);
    return result;
  }

  // Two triangles over the left (opaque) and right (cut out) texel of a 2x1
  // texture: a mixed opacity partition, two geometry groups.
  remixapi_MeshHandle cutout(NativeResources &r, uint64_t hash, remixapi_MaterialHandle material) {
    remixapi_HardcodedVertex vertices[6] = {};
    const float positions[6][3] = {{0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {2, 0, 1}, {3, 0, 1}, {2, 1, 1}};
    const float u[6] = {.2f, .3f, .2f, .7f, .8f, .7f};
    for (int i = 0; i < 6; ++i) {
      std::copy_n(positions[i], 3, vertices[i].position);
      vertices[i].color = 0xffffffff;
      vertices[i].texcoord[0] = u[i];
      vertices[i].texcoord[1] = .5f;
    }
    const uint32_t indices[] = {0, 1, 2, 3, 4, 5};
    remixapi_MeshInfoSurfaceTriangles surface = {};
    surface.vertices_values = vertices;
    surface.vertices_count = 6;
    surface.indices_values = indices;
    surface.indices_count = 6;
    surface.material = material;
    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = hash;
    info.surfaces_values = &surface;
    info.surfaces_count = 1;
    remixapi_MeshHandle result;
    r.createMesh(info, result);
    return result;
  }

  void texture(NativeResources &r, uint64_t hash, const uint8_t *pixels, uint32_t width) {
    remixapi_TextureInfo info{};
    info.hash = hash;
    info.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
    info.width = width;
    info.height = info.depth = info.mipLevels = 1;
    info.data = pixels;
    info.dataSize = 4 * width;
    remixapi_TextureHandle handle;
    r.createTexture(info, handle);
  }

  remixapi_MaterialHandle pbr(NativeResources &r, uint64_t hash, const char *maps, float roughness) {
    const auto path = std::filesystem::path(maps);
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    opaque.alphaTestType = 7;
    opaque.roughnessConstant = roughness;
    opaque.metallicConstant = .3f;
    opaque.roughnessTexture = opaque.metallicTexture = path.c_str();
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.hash = hash;
    info.pNext = &opaque;
    info.albedoTexture = info.normalTexture = path.c_str();
    remixapi_MaterialHandle handle;
    r.createMaterial(info, handle);
    return handle;
  }

  remixapi_MaterialHandle emissive(NativeResources &r, uint64_t hash) {
    const auto path = std::filesystem::path("0xa");
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    opaque.alphaTestType = 7;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.hash = hash;
    info.pNext = &opaque;
    info.emissiveColorConstant = {1, .5f, .25f};
    info.emissiveTexture = path.c_str();
    info.emissiveIntensity = 3;
    remixapi_MaterialHandle handle;
    r.createMaterial(info, handle);
    return handle;
  }

  remixapi_InstanceInfo placed(remixapi_MeshHandle mesh, float x, float y = 0) {
    auto info = instance(mesh, 0);
    info.transform.matrix[0][3] = x;
    info.transform.matrix[1][3] = y;
    return info;
  }

  remixapi_SceneInstanceChangeEXT moveTo(uint64_t handle, float x) {
    remixapi_SceneInstanceChangeEXT u = {};
    u.instance = handle;
    u.fields = REMIXAPI_SCENE_INSTANCE_TRANSFORM_EXT;
    u.transform.matrix[0][0] = u.transform.matrix[1][1] = u.transform.matrix[2][2] = 1;
    u.transform.matrix[0][3] = x;
    return u;
  }

  remixapi_SceneInstanceChangeEXT show(uint64_t handle, bool visible) {
    remixapi_SceneInstanceChangeEXT u = {};
    u.instance = handle;
    u.fields = REMIXAPI_SCENE_INSTANCE_VISIBLE_EXT;
    u.visible = visible;
    return u;
  }

  // The scene, driven identically into every NativeResources it is given.
  struct Scene {
    remixapi_MaterialHandle red{}, green{}, cut{}, rough{}, glow{};
    remixapi_MeshHandle a{}, b{}, c{}, d{}, e{}, f{};
    std::vector<uint64_t> handles;

    void create(NativeResources &r) {
      const uint8_t white[] = {255, 255, 255, 255}, halves[] = {255, 255, 255, 255, 255, 255, 255, 0};
      texture(r, 0xa, white, 1);
      texture(r, 0xb, halves, 2);
      red = material(r, 1, 1, 0, 0);
      green = material(r, 2, 0, 1, 0);
      cut = material(r, 3, 1, 1, 1, 1, std::filesystem::path("0xb").c_str(), 4);
      rough = pbr(r, 4, "0xa", .7f);
      glow = emissive(r, 5);
      a = quad(r, 10, red);
      b = quad(r, 11, green);
      c = cutout(r, 12, cut);
      d = quad(r, 13, rough);
      e = quad(r, 14, glow);
      f = twoSurfaces(r, 15, red, rough);
      for (int i = 0; i < 6; ++i)
        handles.push_back(r.createRetained(placed(a, float(i))));
      handles.push_back(r.createRetained(placed(b, 0, 1)));
      auto cutInfo = placed(c, 0, 2);
      cutInfo.doubleSided = 1;
      handles.push_back(r.createRetained(cutInfo));
      handles.push_back(r.createRetained(placed(d, 1, 2)));
      handles.push_back(r.createRetained(placed(e, 2, 2))); // emissive: traced as a frame draw
      handles.push_back(r.createRetained(placed(f, 3, 2)));
      // GPU instancing: three instances, one of them singular.
      remixapi_Transform locals[3] = {};
      for (int i = 0; i < 3; ++i) {
        locals[i].matrix[0][0] = locals[i].matrix[1][1] = locals[i].matrix[2][2] = i == 1 ? 0.f : 1.f;
        locals[i].matrix[1][3] = float(i) + 5;
      }
      remixapi_InstanceInfoGpuInstancingEXT instancing = {};
      instancing.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_GPU_INSTANCING_EXT;
      instancing.instanceTransforms_values = locals;
      instancing.instanceTransforms_count = 3;
      auto info = placed(f, 4, 4);
      info.pNext = &instancing;
      handles.push_back(r.createRetained(info));
    }

    // Frame-local draws: a dynamic identity, and a legacy (identity-less) draw.
    void local(NativeResources &r, int frame, bool staticLocal) {
      remixapi_InstanceIdentityEXT identity = {};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.instanceId = 777;
      identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
      identity.rasterVisible = 1;
      auto info = placed(a, -3, float(frame % 5));
      info.pNext = &identity;
      r.drawInstance(info);
      r.drawInstance(placed(b, -4));
      if (staticLocal) {
        remixapi_InstanceIdentityEXT fixed = identity;
        fixed.instanceId = 555;
        fixed.classification = REMIXAPI_INSTANCE_STATIC_EXT;
        auto still = placed(d, -5);
        still.pNext = &fixed;
        r.drawInstance(still);
      }
    }

    // Changes before frame's replay. Returns false to skip the replay.
    bool before(NativeResources &r, int frame) {
      switch (frame) {
      case 2: {
        const auto u = show(handles[3], false);
        r.updateRetained(&u, 1);
        break;
      }
      case 4: {
        const auto u = show(handles[3], true);
        r.updateRetained(&u, 1);
        break;
      }
      case 5: {
        r.destroyRetained(&handles[4], 1);
        handles.push_back(r.createRetained(placed(a, 9, 9)));
        break;
      }
      case 6: {
        // Mesh replacement: new vertices under the same hash.
        remixapi_HardcodedVertex vertices[4] = {};
        for (int i = 0; i < 4; ++i) {
          vertices[i].position[0] = float(i & 1) + .25f;
          vertices[i].position[1] = float(i >> 1);
          vertices[i].position[2] = 1;
          vertices[i].color = 0xffffffff;
        }
        const uint32_t indices[] = {0, 1, 2, 1, 3, 2};
        remixapi_MeshInfoSurfaceTriangles surface = {vertices, 4, indices, 6, {}, {}, green};
        remixapi_MeshInfo info = {};
        info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
        info.hash = 11;
        info.surfaces_values = &surface;
        info.surfaces_count = 1;
        remixapi_MeshHandle handle;
        r.createMesh(info, handle);
        break;
      }
      case 7:
        red = material(r, 1, .5f, 0, 0); // Material replacement.
        break;
      case 10:
      case 13: {
        remixapi_MaterialInfoTranslucentEXT optical{};
        optical.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
        optical.refractiveIndex = 1.5f;
        optical.transmittanceColor = {.25f, .5f, 1};
        optical.transmittanceMeasurementDistance = 1;
        optical.thinWallThickness_hasvalue = 1;
        optical.thinWallThickness_value = frame == 10 ? .25f : .5f;
        remixapi_MaterialInfo info{};
        info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
        info.hash = 1;
        info.pNext = &optical;
        r.createMaterial(info, red);
        break;
      }
      case 17:
        red = material(r, 1, .5f, 0, 0);
        break;
      case 8:
      case 9: {
        const auto u = moveTo(handles[5], float(frame)); // Demoted to dynamic until it settles.
        r.updateRetained(&u, 1);
        break;
      }
      case 11:
        return false; // No replay: the static set is not drawn.
      case 14:
        r.destroyMesh(c); // Its instance resolves nothing...
        break;
      case 15:
        c = cutout(r, 12, cut); // ...until the mesh returns.
        break;
      case 16: {
        auto info = placed(b, 0, 1);
        info.doubleSided = 1; // A re-description.
        r.updateRetained(handles[6], info);
        break;
      }
      case 21:
        r.replaceModMaterials({{2, red}}); // A mod material replacement.
        break;
      case 23:
        r.clearModMaterials();
        break;
      default:
        break;
      }
      return true;
    }

    // Changes after the frame's replay (the replay's draws stay as drawn).
    void after(NativeResources &r, int frame) {
      if (frame == 19) {
        r.destroyRetained(&handles[1], 1);
        const auto u = show(handles[2], false);
        r.updateRetained(&u, 1);
      }
      if (frame == 20) {
        const auto u = show(handles[2], true);
        r.updateRetained(&u, 1);
      }
    }
  };

  // expandStatic reproduces the draw list of a replay without the set.
  void compareDrawLists(const NativeResources &expanded, const NativeResources &legacy, int frame) {
    const auto &x = expanded.draws(), &y = legacy.draws();
    bool same = x.size() == y.size();
    for (size_t i = 0; same && i < x.size(); ++i)
      same = x[i].instanceId == y[i].instanceId && x[i].instanceIndex == y[i].instanceIndex &&
             x[i].surfaceIndex == y[i].surfaceIndex && x[i].classification == y[i].classification &&
             x[i].model == y[i].model && x[i].doubleSided == y[i].doubleSided;
    if (!same) {
      std::fprintf(stderr, "Static scene test failed at frame %d: expanded draw list differs from replay\n", frame);
      std::exit(1);
    }
  }

  void sequence() {
    NativeResources resources, legacy;
    Scene scene, legacyScene;
    camera(resources);
    camera(legacy);
    scene.create(resources);
    legacyScene.create(legacy);
    resources.setPersistentStatic(true);
    Pipeline kept, rebuilt;
    kept.persistent = true;
    uint64_t reused = 0, rebuiltSurfaces = 0, unitFrames = 0, layouts = 0, temporal = 0, emitterFrames = 0;
    uint64_t mixedGroups = 0;
    for (int frame = 0; frame < 270; ++frame) {
      if (frame == 17 || frame == 18)
        resources.setPersistentStatic(frame == 18); // Off for a frame: a new epoch after.
      const bool replay = scene.before(resources, frame);
      legacyScene.before(legacy, frame);
      if (replay) {
        resources.drawRetained(nullptr);
        legacy.drawRetained(nullptr);
      }
      scene.local(resources, frame, frame == 12);
      legacyScene.local(legacy, frame, frame == 12);
      scene.after(resources, frame);
      legacyScene.after(legacy, frame);
      if (frame == 10)
        kept.epoch.reset = rebuilt.epoch.reset = 1; // A history reset.
      kept.filterEmission = rebuilt.filterEmission = frame >= 30; // Animated emission layouts.
      if (frame == 13 || frame == 27) {
        // BLAS compaction moves a mesh's acceleration structure.
        const auto *mesh = resources.retained(scene.handles[8])->mesh.get();
        kept.compact(mesh);
        rebuilt.compact(mesh);
      }
      kept.prepare(resources);
      resources.expandStatic();
      compareDrawLists(resources, legacy, frame);
      rebuilt.prepare(resources);
      compare(kept, rebuilt, frame);
      if (kept.frame.staticUnits) {
        const auto &s = kept.tables.statistics();
        reused += s.staticReused;
        rebuiltSurfaces += s.staticRebuilt;
        layouts += s.relayouts;
        temporal += s.temporalSurfaces;
        ++unitFrames;
        emitterFrames += !kept.statics.emitting().empty() && !kept.frame.emitters.empty();
        for (size_t i = 0; i < kept.tables.prefixSurfaces(); ++i)
          mixedGroups += kept.tables.surface(i).triangleMap != 0;
        require(frame < 28 || frame == 249 || frame == 250 || s.relayouts == 0,
                "A frame without static changes laid the prefix out again");
      }
      require(frame != 3 || kept.frame.staticUnits, "Static set unused on a quiet frame");
      require(frame != 12 || !kept.frame.staticUnits, "A frame-local static instance was not traced from draws");
      require(frame != 3 || kept.tables.statistics().staticRebuilt == 0, "A quiet frame rebuilt static surfaces");
      if (frame != 9) { // An uncommitted frame.
        kept.commit();
        rebuilt.commit();
      }
      resources.finishFrame();
      legacy.finishFrame();
    }
    std::printf("unit frames %llu, layouts %llu, reused %llu, rebuilt %llu, temporal %llu, emitter frames %llu\n",
                (unsigned long long)unitFrames, (unsigned long long)layouts, (unsigned long long)reused,
                (unsigned long long)rebuiltSurfaces, (unsigned long long)temporal, (unsigned long long)emitterFrames);
    require(reused > rebuiltSurfaces * 10, "Static surfaces were not kept");
    require(unitFrames >= 260, "The static set was not traced from its units");
    require(layouts >= 10 && layouts <= 40, "Unexpected number of prefix layouts");
    require(temporal > 0 && emitterFrames >= 250 && mixedGroups > 0, "Scenario coverage");
    const auto &settled = *resources.retained(scene.handles[5]);
    require(settled.member && !settled.moving, "A settled instance did not return to the static set");
  }

  // Asynchronous presentation: the same scene, each frame published
  // (publishFrame) into a second NativeResources that a render thread traces
  // while the next frame's API calls run. The tracer's copy syncs from the
  // replica: it must trace exactly what the rebuild path traces from the
  // published draw list, and the replica's expanded draw list must be the
  // replay without the set.
  void published() {
    NativeResources resources, legacy, frame;
    Scene scene, legacyScene;
    camera(resources);
    camera(legacy);
    scene.create(resources);
    legacyScene.create(legacy);
    resources.setConcurrentConsumer(true);
    resources.setPersistentStatic(true);
    Pipeline kept, rebuilt;
    kept.persistent = true;
    RenderThread render;
    std::atomic<uint64_t> unitFrames{0};
    for (int f = 0; f < 270; ++f) {
      if (f == 17 || f == 18)
        resources.setPersistentStatic(f == 18);
      if (f == 60)
        resources.restartStatic(); // A presentation-mode switch.
      const bool replay = scene.before(resources, f);
      legacyScene.before(legacy, f);
      if (replay) {
        resources.drawRetained(nullptr);
        legacy.drawRetained(nullptr);
      }
      scene.local(resources, f, f == 12);
      legacyScene.local(legacy, f, f == 12);
      scene.after(resources, f);
      legacyScene.after(legacy, f);
      // The replay without the set, by value (legacy's next frame runs
      // concurrently with this one's render).
      std::vector<scene::Draw> replayed = legacy.draws();
      legacy.finishFrame();
      // A BLAS compaction of this mesh before the frame is traced.
      std::shared_ptr<scene::Mesh> compacted;
      if (f == 13 || f == 27)
        compacted = resources.retained(scene.handles[8])->mesh;
      render.wait(); // Present: the previous frame is complete.
      resources.publishFrame(frame);
      render.submit([&, f, compacted, replayed = std::move(replayed)] {
        if (f == 10)
          kept.epoch.reset = rebuilt.epoch.reset = 1;
        kept.filterEmission = rebuilt.filterEmission = f >= 30;
        if (compacted) {
          kept.compact(compacted.get());
          rebuilt.compact(compacted.get());
        }
        kept.prepare(frame);
        frame.expandStatic();
        const auto &x = frame.draws();
        bool same = x.size() == replayed.size();
        for (size_t i = 0; same && i < x.size(); ++i)
          same = x[i].instanceId == replayed[i].instanceId && x[i].instanceIndex == replayed[i].instanceIndex &&
                 x[i].surfaceIndex == replayed[i].surfaceIndex &&
                 x[i].classification == replayed[i].classification && x[i].model == replayed[i].model &&
                 x[i].doubleSided == replayed[i].doubleSided;
        if (!same) {
          std::fprintf(stderr, "Static scene test failed at published frame %d: replica draw list differs\n", f);
          std::exit(1);
        }
        rebuilt.prepare(frame);
        compare(kept, rebuilt, f);
        unitFrames += kept.frame.staticUnits;
        require(f != 3 || kept.frame.staticUnits, "Replica unused on a quiet frame");
        require(f != 12 || !kept.frame.staticUnits, "A frame-local static instance was not traced from draws");
        if (f != 9) {
          kept.commit();
          rebuilt.commit();
        }
        frame.finishFrame();
      });
    }
    render.wait();
    require(unitFrames >= 260, "The replica was not traced from its units");
  }

  // Retained table: membership, change lists, and draws kept out of the list.
  void membership() {
    NativeResources r;
    camera(r);
    auto red = material(r, 1, 1, 0, 0);
    auto mesh = quad(r, 2, red);
    const uint64_t first = r.createRetained(placed(mesh, 0)), second = r.createRetained(placed(mesh, 1));
    r.setPersistentStatic(true);
    r.drawRetained(nullptr);
    require(r.staticDrawn() && r.draws().empty() && r.staticDrawCount() == 2 && r.pendingDrawCount() == 2,
            "Static members stayed in the draw list");
    std::vector<uint64_t> changes;
    r.takeStaticChanges(changes);
    require(changes.size() == 2, "Joining members were not reported");
    r.finishFrame();
    r.drawRetained(nullptr);
    r.takeStaticChanges(changes);
    require(changes.empty(), "An unchanged replay reported changes");
    r.finishFrame();
    const auto hide = show(first, false);
    r.updateRetained(&hide, 1);
    r.drawRetained(nullptr);
    r.takeStaticChanges(changes);
    require(changes.size() == 1 && changes[0] == first && !r.retained(first)->member, "A hidden member stayed");
    // A change after the replay keeps what the replay drew.
    r.destroyRetained(&second, 1);
    require(!r.staticDrawn() && r.draws().size() == 1, "A destroy after the replay changed the frame");
    r.finishFrame();
    r.drawRetained(nullptr);
    r.takeStaticChanges(changes);
    require(changes.size() == 1 && changes[0] == second && r.staticDrawCount() == 0, "Destroyed member stayed");
    r.finishFrame();
    // A moved instance leaves the set (dynamic) until it settles.
    const auto visible = show(first, true);
    r.updateRetained(&visible, 1);
    const auto move = moveTo(first, 3);
    r.updateRetained(&move, 1);
    r.drawRetained(nullptr);
    require(r.draws().size() == 1 && r.draws()[0].classification == 1 && r.staticDrawCount() == 0,
            "A moving instance was kept static");
    r.finishFrame();
    for (uint64_t i = 0; i < NativeResources::retainedSettleFrames; ++i) {
      r.drawRetained(nullptr);
      r.finishFrame();
    }
    r.drawRetained(nullptr);
    require(r.staticDrawCount() == 1 && r.draws().empty(), "A settled instance did not rejoin");
    // Rebased replays bypass the set.
    r.finishFrame();
    const float origin[3] = {1, 0, 0};
    r.drawRetained(origin);
    require(!r.staticDrawn() && r.draws().size() == 1 && r.draws()[0].model[12] == 2, "A rebased replay kept the set");
    r.finishFrame();
    r.setPersistentStatic(false);
    r.drawRetained(nullptr);
    require(!r.staticDrawn() && r.draws().size() == 1, "The set outlived the setting");
  }

  // Segmented signatures compare exactly as their concatenated bytes.
  void signatures() {
    trace::SceneSignature a, b;
    const std::vector<uint8_t> bytes = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    a.head = {1, 2, 3};
    a.tailInstances = {4, 5, 6, 7, 8, 9};
    b.head = {1, 2};
    b.frame = {3, 4, 5, 6, 7, 8, 9};
    require(a.bytes() == bytes && b.bytes() == bytes && a.equal(b) && b.equal(a), "Split signatures differ");
    b.frame.back() = 0;
    require(!a.equal(b) && !b.equal(a), "Different signatures are equal");
    uint8_t records[4 * 8] = {};
    for (int i = 0; i < 32; ++i)
      records[i] = uint8_t(i);
    trace::SceneSignature c, d;
    c.prefixSurfaces = {records, 4, 8, 3};
    d.prefixSurfaces = {records, 4, 8, 3};
    require(c.equal(d), "A shared segment differs");
    d.prefixSurfaces = {};
    d.tailSurfaces = {0, 1, 2, 8, 9, 10, 16, 17, 18, 24, 25, 26};
    require(c.equal(d) && d.equal(c), "Strided and packed records differ");
  }
} // namespace

int main() {
  signatures();
  membership();
  sequence();
  published();
  std::puts("persistent static set matches the rebuild path");
}

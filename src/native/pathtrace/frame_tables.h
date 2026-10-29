#pragma once
// The per-frame tables of the path tracer (surface table, primary draws, TLAS
// inputs, SHARC and scene signature bytes, material descriptors, temporal
// staging), built from a TraceFrame and, when in use, the persistent static
// set. Vulkan-free: Env supplies the mesh views, so CPU tests can compare the
// persistent path against the rebuild path directly.
//
// Env requirements (duck-typed):
//   using View;        view.geometry->{input->address, accel->address, indexOffsets,
//                      groups[i].{surface, indices, triangleMap}, revision, fingerprint,
//                      diagonal}, view.vertices[s]->address, view.fingerprint,
//                      view.contentDigest
//   using VertexTable; scene::TemporalTable<shared_ptr<Buffer>> (value->address)
//   void resolveViews(const std::vector<Span> &instances, std::vector<const View *> &views);
//       the tracer's view lookup, conversions and uploads for these instances,
//       in order (Gpu::prepare's passes 0 and 1)
//   bool findViews(const std::vector<Span> &instances, std::vector<const View *> &views);
//       read-only: the current views, false when one needs work (validation)
//   std::shared_ptr<const void> owner(const View &view, uint32_t surface);
//       keeps a vertex buffer alive
//   [[noreturn]] void tooManyObjects();
#include "pathtrace/sharc_policy.h"
#include "pathtrace/tlas_policy.h"
#include "platform/timing.h"
#include "platform/workers.h"
#include "scene/static_scene.h"
#include "scene/temporal.h"
#include "scene/trace_frame.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace remix::trace::detail {
  struct TraceSurface {
    uint64_t vertices = 0, indices = 0;
    float tint[4] = {}, uv[4] = {}, factor[4] = {};
    uint32_t state[4] = {}, textureState[4] = {};
    uint64_t triangleMap = 0, traceIndices = 0;
    uint64_t previousVertices = 0, debugIdentity = 0;
    scene::Matrix previousModel{};
    float emission[4] = {};
    float pbr[4] = {};            // perceptual roughness, metalness, enabled, reserved
    uint32_t pbrTextures[4] = {}; // roughness, metalness, normal; index + 1
    uint32_t material[4] = {};    // scene::MaterialDecode: flags; the rest unused
    float film[4] = {};           // iridescence (filmFields): strength (0: none), film index, thickness nm, structural
    uint32_t filmMaps[4] = {};    // half2(min, max nm), half2(flow amplitude, scale), flow speed, map + 1 | alpha << 31
  };
  static_assert(sizeof(TraceSurface) == 288);
  // IEEE binary16 bits of v, rounded to nearest (subnormals kept).
  inline uint32_t halfBits(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    const uint32_t sign = bits >> 16 & 0x8000u;
    const float a = std::min(std::fabs(v), 65504.f);
    if (a < 6.1035156e-5f)
      return sign | uint32_t(std::lround(a / 5.9604645e-8f));
    std::memcpy(&bits, &a, 4);
    bits += 0x1000u; // round
    return sign | (((bits >> 23) - 112) << 10) | (bits >> 13 & 0x3ffu);
  }
  // The material-only fields of a surface. A remapped map's bias takes its
  // constant's place in pbr (unused beside a map) and its scale material[1 +
  // map]; material[3] holds per map (bits 8 x map) channel, invert << 2, curve
  // << 3 and remapped << 7, and the exponent in 1/16 (bits 16 + 8 x map).
  // Water (normal encoding 5, no such maps) has its scroll rates there
  // instead, as half floats: base, layer 1 and layer 2 in material[1..3].
  // maps[3]: the normal decoding (scene.glsl materialNormal).
  inline void materialFields(const scene::Material &material, float pbr[4], uint32_t decodeOut[4], uint32_t &normal) {
    pbr[0] = material.roughness;
    pbr[1] = material.metallic;
    pbr[2] = material.pbr ? 1.f : 0.f;
    const auto decode = scene::materialDecode(material);
    decodeOut[0] = decode.flags;
    // A translucent body: its refractive index and measurement distance, half
    // floats in pbr[3] (scene.glsl thickSheet); zero for everything else.
    if (material.transmission && material.transmittanceDistance > 0) {
      const uint32_t packed = detail::halfBits(material.refractiveIndex) |
                              detail::halfBits(std::min(material.transmittanceDistance, 65504.f)) << 16;
      std::memcpy(&pbr[3], &packed, 4);
    }
    const std::shared_ptr<scene::Texture> *textures[] = {&material.roughnessTexture, &material.metallicTexture};
    for (unsigned i = 0; i < 2; ++i) {
      const auto &r = material.remap[i];
      if (!*textures[i] || r.identity())
        continue;
      pbr[i] = r.bias;
      std::memcpy(&decodeOut[1 + i], &r.scale, sizeof(float));
      const uint32_t bits = r.channel | uint32_t(r.invert) << 2 | uint32_t(r.curve) << 3 | 128u;
      const uint32_t exponent = uint32_t(std::lround(std::clamp(r.exponent, 0.f, 15.9375f) * 16));
      decodeOut[3] |= bits << (8 * i) | exponent << (16 + 8 * i);
    }
    if (material.normalEncoding == 5) {
      for (unsigned i = 0; i < 3; ++i)
        decodeOut[1 + i] = detail::halfBits(material.scroll[2 * i]) | detail::halfBits(material.scroll[2 * i + 1]) << 16;
      // Two-channel maps (BC5) reconstruct Z here too (bit 0), as other encodings do:
      // without it Z read 0 and the water's normals lay flat along its surface.
      normal = (material.scrollLayers == 3 ? 48 : 16) |
               uint32_t(material.normalTexture && material.normalTexture->pixels.twoChannel);
      return;
    }
    normal = material.normalEncoding == 1   ? 2
             : material.normalEncoding == 4 ? 8
                                            : (material.normalEncoding == 3 ? 4 : 0) |
                                                  uint32_t(material.normalTexture && material.normalTexture->pixels.twoChannel);
  }
  // A material's thin film (scene::Material::iridescence), but for its
  // thickness map's descriptor (filmMaps[3] bits 0-30, set by assign).
  inline void filmFields(const scene::Material &material, float film[4], uint32_t maps[4]) {
    const auto &f = material.iridescence;
    if (f.strength <= 0)
      return;
    film[0] = f.strength;
    film[1] = f.ior;
    film[2] = f.thickness;
    film[3] = f.structural;
    maps[0] = halfBits(f.min) | halfBits(f.max) << 16;
    maps[1] = halfBits(f.flowAmplitude) | halfBits(f.flowScale) << 16;
    std::memcpy(&maps[2], &f.flowSpeed, 4);
    maps[3] = f.fromAlpha ? 1u << 31 : 0;
  }
  // A display-encoded value (gamma 2.2) to linear.
  inline float linear(float c) { return std::pow(c, 2.2f); }
  // Decal paint mode (TraceSurface state bits 7-8): 0 alpha-over, 1 multiply
  // (DST_COLOR*ZERO or ZERO*SRC_COLOR), 2 double multiply (DST_COLOR*SRC_COLOR, mod2x).
  inline uint32_t decalMode(const scene::Blend &b) {
    constexpr uint32_t zero = 0, srcColor = 2, dstColor = 4;
    if (b.op != 0)
      return 0;
    if (b.src == dstColor && b.dst == srcColor)
      return 2;
    if ((b.src == dstColor && b.dst == zero) || (b.src == zero && b.dst == srcColor))
      return 1;
    return 0;
  }
} // namespace remix::trace::detail

namespace remix::trace {
  using detail::TraceSurface;
  struct PrimaryDraw {
    scene::Matrix model;
    uint32_t surfaceIndex = 0, count = 0;
    bool blended = false;
    bool viewModel = false; // drawn after the scene, in front of it (recordPrimary)
    bool reflectionOnly = false; // not in the G-buffer (Draw::reflectionOnly)
  };
  // Per-material surface fields; the texture slots are frame indices.
  struct MaterialSlots {
    uint32_t albedo = 0, emissive = 0;
    float pbr[4] = {}, emission[4] = {};
    uint32_t decode[4] = {}, maps[4] = {};
    float film[4] = {};
    uint32_t filmMaps[4] = {};
  };
  // One entry of the frame's texture descriptor array, in first-use order:
  // Albedo (sampledMaterial), Map (sampledTexture of *texture) or Emission.
  struct DescriptorRequest {
    enum Kind : uint8_t { Albedo, Map, Emission };
    std::shared_ptr<scene::Material> material;
    const std::shared_ptr<scene::Texture> *texture = nullptr;
    Kind kind = Albedo;
    bool operator==(const DescriptorRequest &other) const {
      return material == other.material && kind == other.kind &&
             (kind != Map || texture->get() == other.texture->get());
    }
  };
  // Byte layouts of the SHARC geometry and scene signatures.
  constexpr size_t staticInstanceBytes = 8 + 4 + sizeof(scene::Matrix) + sizeof(uintptr_t) + 8 + 8;
  constexpr size_t staticSurfaceBytes = 4 * 4 * 4;
  constexpr size_t dynamicInstanceBytes = 8 + 4 + sizeof(scene::Matrix) + 8 + 8;
  constexpr size_t sceneInstanceBytes = sizeof(scene::Matrix) + 8 + 8 + 4 + 4;
  constexpr size_t sceneSurfaceBytes = offsetof(TraceSurface, previousVertices);

  // A frame's scene signature: the bytes compared against the previous frame
  // to detect change, kept as segments so an unchanged static prefix is not
  // copied or compared. equal() is exactly the comparison of the
  // concatenated bytes.
  struct SceneSignature {
    struct Segment {
      const uint8_t *data = nullptr;
      size_t count = 0, stride = 0, width = 0; // count records of width bytes, stride apart
      size_t bytes() const { return count * width; }
    };
    std::vector<uint8_t> head, tailInstances, tailMaterials, tailSurfaces, frame;
    std::shared_ptr<const void> prefix; // keeps the prefix segments alive
    Segment prefixInstances, prefixMaterials, prefixSurfaces;
    size_t size() const {
      return head.size() + prefixInstances.bytes() + tailInstances.size() + prefixMaterials.bytes() +
             tailMaterials.size() + prefixSurfaces.bytes() + tailSurfaces.size() + frame.size();
    }
    template <class F> void segments(F &&f) const {
      auto whole = [](const std::vector<uint8_t> &v) { return Segment{v.data(), 1, v.size(), v.size()}; };
      f(whole(head));
      f(prefixInstances);
      f(whole(tailInstances));
      f(prefixMaterials);
      f(whole(tailMaterials));
      f(prefixSurfaces);
      f(whole(tailSurfaces));
      f(whole(frame));
    }
    bool equal(const SceneSignature &other) const {
      if (size() != other.size())
        return false;
      struct Cursor {
        std::vector<Segment> segments;
        size_t segment = 0, record = 0, offset = 0, position = 0;
        void normalize() {
          while (segment < segments.size() &&
                 (record >= segments[segment].count || !segments[segment].width)) {
            ++segment;
            record = offset = 0;
          }
        }
      } a, b;
      segments([&](Segment s) { a.segments.push_back(s); });
      other.segments([&](Segment s) { b.segments.push_back(s); });
      a.normalize();
      b.normalize();
      while (a.segment < a.segments.size() && b.segment < b.segments.size()) {
        const auto &x = a.segments[a.segment], &y = b.segments[b.segment];
        // The same bytes at the same stream position: skip the whole segment.
        if (!a.record && !a.offset && !b.record && !b.offset && a.position == b.position && x.data == y.data &&
            x.count == y.count && x.stride == y.stride && x.width == y.width) {
          a.position += x.bytes();
          b.position += y.bytes();
          a.record = x.count;
          b.record = y.count;
          a.normalize();
          b.normalize();
          continue;
        }
        const size_t n = std::min(x.width - a.offset, y.width - b.offset);
        if (std::memcmp(x.data + a.record * x.stride + a.offset, y.data + b.record * y.stride + b.offset, n))
          return false;
        a.position += n;
        b.position += n;
        if ((a.offset += n) == x.width)
          a.offset = 0, ++a.record;
        if ((b.offset += n) == y.width)
          b.offset = 0, ++b.record;
        a.normalize();
        b.normalize();
      }
      return true;
    }
    // The concatenated bytes (tests and diagnostics).
    std::vector<uint8_t> bytes() const {
      std::vector<uint8_t> out;
      segments([&](Segment s) {
        for (size_t i = 0; i < s.count; ++i)
          out.insert(out.end(), s.data + i * s.stride, s.data + i * s.stride + s.width);
      });
      return out;
    }
  };

  template <class Env> class FrameTables {
  public:
    using View = typename Env::View;
    using VertexTable = typename Env::VertexTable;
    using Span = scene::TraceFrame::InstanceList::ConstSpan;
    using MapKey = std::tuple<const scene::Texture *, uint8_t, uint8_t, uint8_t>;
    struct Flags {
      bool blended = false, decal = false, receivers = false;
      // Opaque surfaces that hold SHARC entries only with sharcGlossySurfaces
      // (static, smoother than the policy's roughness) or sharcDynamicSurfaces
      // (moving; sharp ones need both).
      bool sharpReceivers = false, dynamicReceivers = false, dynamicSharpReceivers = false;
      Flags &operator|=(const Flags &o) {
        blended |= o.blended;
        decal |= o.decal;
        receivers |= o.receivers;
        sharpReceivers |= o.sharpReceivers;
        dynamicReceivers |= o.dynamicReceivers;
        dynamicSharpReceivers |= o.dynamicSharpReceivers;
        return *this;
      }
      bool operator==(const Flags &) const = default;
    };
    struct Inputs {
      scene::TraceFrame &frame;
      scene::StaticScene *statics = nullptr; // traced with the frame when frame.staticUnits
      scene::TemporalHistory &history;
      VertexTable &previousVertices, &pendingVertices;
      WorkerPool &workers;
      uint64_t serial = 0; // numbers this prepared frame
      bool pbrReuse = false, digest = false;
      size_t textureLimit = 4096;
      uint64_t *materialTicks = nullptr, *temporalTicks = nullptr;
    };
    // The static prefix: every traced unit of the persistent static set, laid
    // out as the rebuild path lays out a frame's first (static) instances.
    // Kept across frames while no unit changes.
    struct Prefix {
      uint64_t generation = 0;
      std::vector<TraceSurface> surfaces;
      std::vector<PrimaryDraw> primary;
      std::vector<InstanceInput> inputs;
      std::vector<uint8_t> staticBytes, instanceBytes, materialBytes;
      std::vector<DescriptorRequest> requests;
      std::unordered_map<const scene::Material *, MaterialSlots> materials;
      std::map<MapKey, uint32_t> maps;
      std::vector<uint32_t> first; // by unit slot: its first surface, or ~0u
      size_t instances = 0;
      Flags flags;
      uint64_t geometryDigest = seed, reuseSignature = seed;
      bool reuse = false; // reuseSignature covers the prefix
    };
    // The frame's other instances (all of them without the static set).
    struct Tail {
      std::vector<TraceSurface> surfaces;
      std::vector<PrimaryDraw> primary;
      std::vector<InstanceInput> inputs[2];
      std::vector<uint8_t> staticBytes, dynamicBytes, instanceBytes, materialBytes;
      std::vector<DescriptorRequest> requests;
    };
    struct Statistics {
      // Static surfaces taken from kept tables, and written this frame
      // ((re)built units, or a new layout of the prefix).
      uint64_t staticReused = 0, staticRebuilt = 0, staticUnits = 0, rebuiltUnits = 0;
      uint64_t dynamicSurfaces = 0, temporalSurfaces = 0, relayouts = 0;
      double staticMilliseconds = 0;
    };
    static constexpr uint64_t seed = 1469598103934665603ull;

    // Builds this frame's tables. The frame's instances are consumed (their
    // draws move into the temporal history), as by the rebuild path.
    void build(Env &env, const Inputs &in) {
      m_statistics = {};
      m_statics = in.frame.staticUnits ? in.statics : nullptr;
      std::vector<Span> spans;
      std::vector<uint32_t> dirty;
      const auto staticStart = std::chrono::steady_clock::now();
      if (m_statics) {
        syncUnits(in, dirty);
        for (const uint32_t slot : dirty)
          spans.push_back(unitSpan(slot));
      } else {
        // Traced without the set: its units' history is not theirs this
        // frame, and the prefix is laid out again when they return.
        if (in.statics)
          in.statics->absent(in.serial);
        m_current.reset();
      }
      const size_t unitSpans = spans.size();
      for (size_t i = 0; i < in.frame.instances.size(); ++i)
        spans.push_back(in.frame.instances[i]);
      m_views.clear();
      m_statistics.staticMilliseconds +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - staticStart).count();
      // One lookup for both: new static views convert with the frame's.
      env.resolveViews(spans, m_views);
      if (m_statics) {
        const auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < unitSpans; ++i)
          buildUnit(env, in, dirty[i], *m_views[i]);
        for (const uint32_t slot : m_readdress)
          readdress(slot);
        m_readdress.clear();
        updateTemporal(in);
        if (!m_current || m_layoutDirty)
          layout(env, in);
        m_layoutDirty = false;
        m_statistics.staticUnits = m_current->instances;
        m_statistics.staticReused =
            m_current->surfaces.size() - std::min<uint64_t>(m_statistics.staticRebuilt, m_current->surfaces.size());
        m_statics->acknowledge();
        m_statistics.staticMilliseconds +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      }
      m_tailViews.assign(m_views.begin() + unitSpans, m_views.end());
      buildTail(env, in);
      m_statistics.dynamicSurfaces = m_tail.surfaces.size();
    }

    // Validation: lays the static prefix out from scratch through the rebuild
    // path's code (read-only: current views and history lookups) and compares
    // it with the kept one. Returns the first difference, or "".
    std::string validate(Env &env, const Inputs &in) const {
      if (!m_statics || !m_current)
        return {};
      std::vector<Span> spans;
      for (const uint32_t slot : m_statics->order())
        if (!m_statics->units()[slot].singular)
          spans.push_back(unitSpan(slot));
      std::vector<const View *> views;
      if (!env.findViews(spans, views))
        return "a static unit's view needs work";
      const auto &p = *m_current;
      std::vector<TraceSurface> surfaces;
      std::vector<PrimaryDraw> primary;
      std::vector<InstanceInput> inputs;
      std::vector<uint8_t> staticBytes, instanceBytes, materialBytes;
      std::vector<DescriptorRequest> requests;
      std::unordered_map<const scene::Material *, MaterialSlots> materials;
      std::map<MapKey, uint32_t> maps;
      std::vector<const MaterialSlots *> slots;
      Flags flags;
      uint64_t digest = seed;
      for (size_t i = 0; i < spans.size(); ++i) {
        for (const auto &surface : spans[i]) {
          flags |= surfaceFlags(surface.draw);
          auto [entry, first] = materials.try_emplace(surface.draw.material.get());
          if (first)
            entry->second = assign(surface.draw.material, requests, materialBytes, maps, 0, in.textureLimit, env);
          slots.push_back(&entry->second);
        }
        if (in.digest)
          digest = scene::hashBytes(&views[i]->contentDigest, sizeof(views[i]->contentDigest), digest);
      }
      FactorCache factor;
      for (size_t i = 0, slot = 0; i < spans.size(); ++i) {
        const auto instance = spans[i];
        const auto &view = *views[i];
        const size_t base = surfaces.size(), primaryBase = primary.size(), bytes = staticBytes.size(),
                     scene = instanceBytes.size();
        surfaces.resize(base + groupCount(instance, view));
        primary.resize(primaryBase + instance.size());
        staticBytes.resize(bytes + staticInstanceBytes + staticSurfaceBytes * instance.size());
        instanceBytes.resize(scene + sceneInstanceBytes);
        InstanceInput input;
        fill(in, instance, view, instance[0].draw.instanceId, slots.data() + slot, surfaces.data() + base, base,
             primary.data() + primaryBase, input, staticBytes.data() + bytes, nullptr, instanceBytes.data() + scene,
             factor);
        inputs.push_back(input);
        slot += instance.size();
      }
      auto at = [](const char *what, size_t index) {
        char text[96];
        std::snprintf(text, sizeof(text), "%s %zu differs", what, index);
        return std::string(text);
      };
      if (surfaces.size() != p.surfaces.size())
        return "surface count differs";
      for (size_t i = 0; i < surfaces.size(); ++i)
        if (std::memcmp(&surfaces[i], &p.surfaces[i], sizeof(TraceSurface)))
          return at("surface", i);
      if (primary.size() != p.primary.size())
        return "primary draw count differs";
      for (size_t i = 0; i < primary.size(); ++i) {
        const auto &a = primary[i], &b = p.primary[i];
        if (a.model != b.model || a.surfaceIndex != b.surfaceIndex || a.count != b.count || a.blended != b.blended)
          return at("primary draw", i);
      }
      if (inputs != p.inputs)
        return "static TLAS inputs differ";
      if (staticBytes != p.staticBytes)
        return "SHARC static geometry differs";
      if (instanceBytes != p.instanceBytes)
        return "scene instance signature differs";
      if (materialBytes != p.materialBytes)
        return "material signature differs";
      if (requests != p.requests)
        return "texture descriptors differ";
      if (!(flags == p.flags))
        return "surface flags differ";
      if (in.digest && digest != p.geometryDigest)
        return "geometry digest differs";
      return {};
    }

    // A BLAS was compacted: its device address changed.
    void compacted(const void *geometry) {
      const auto found = m_geometryUnits.find(geometry);
      if (found != m_geometryUnits.end())
        m_readdress.insert(m_readdress.end(), found->second.begin(), found->second.end());
    }
    // Forget every kept table (the views they refer to are gone).
    void reset() {
      m_units.clear();
      m_geometryUnits.clear();
      m_readdress.clear();
      m_current.reset();
      m_pool.clear();
      m_rescan = true;
      m_temporalSteady = false;
      m_temporalPending.clear();
    }

    const Prefix *prefix() const { return m_current.get(); }
    const Tail &tail() const { return m_tail; }
    Flags flags() const { return m_flags; }
    uint64_t geometryDigest() const { return m_geometryDigest; }
    uint64_t reuseSignature() const { return m_reuseSignature; }
    size_t prefixSurfaces() const { return m_current ? m_current->surfaces.size() : 0; }
    size_t surfaceCount() const { return prefixSurfaces() + m_tail.surfaces.size(); }
    size_t primaryCount() const { return (m_current ? m_current->primary.size() : 0) + m_tail.primary.size(); }
    size_t instanceCount() const { return (m_current ? m_current->instances : 0) + m_tailInstances; }
    size_t requestCount() const { return (m_current ? m_current->requests.size() : 0) + m_tail.requests.size(); }
    const TraceSurface &surface(size_t i) const {
      const size_t prefix = prefixSurfaces();
      return i < prefix ? m_current->surfaces[i] : m_tail.surfaces[i - prefix];
    }
    const PrimaryDraw &primary(size_t i) const {
      const size_t prefix = m_current ? m_current->primary.size() : 0;
      return i < prefix ? m_current->primary[i] : m_tail.primary[i - prefix];
    }
    const DescriptorRequest &request(size_t i) const {
      const size_t prefix = m_current ? m_current->requests.size() : 0;
      return i < prefix ? m_current->requests[i] : m_tail.requests[i - prefix];
    }
    // The static TLAS inputs and SHARC bytes: the prefix's, or the tail's
    // without the static set (a frame with the set has no other static ones).
    const std::vector<InstanceInput> &staticInputs() const { return m_current ? m_current->inputs : m_tail.inputs[0]; }
    const std::vector<InstanceInput> &dynamicInputs() const { return m_tail.inputs[1]; }
    const std::vector<uint8_t> &staticBytes() const { return m_current ? m_current->staticBytes : m_tail.staticBytes; }
    const std::vector<uint8_t> &dynamicBytes() const { return m_tail.dynamicBytes; }
    // Nonzero while the static inputs and bytes are those of one kept prefix:
    // equal generations mean equal contents.
    uint64_t staticGeneration() const { return m_current ? m_current->generation : 0; }
    // Changes whenever the prefix's surfaces do (a layout, or patched
    // temporal fields); 0 without a prefix.
    uint64_t prefixVersion() const { return m_current ? m_prefixVersion : 0; }
    // Adds the instance, material and surface segments to a signature.
    void signature(SceneSignature &out) const {
      out.tailInstances = m_tail.instanceBytes;
      out.tailMaterials = m_tail.materialBytes;
      out.tailSurfaces.resize(m_tail.surfaces.size() * sceneSurfaceBytes);
      for (size_t i = 0; i < m_tail.surfaces.size(); ++i)
        std::memcpy(out.tailSurfaces.data() + i * sceneSurfaceBytes, &m_tail.surfaces[i], sceneSurfaceBytes);
      if (m_current) {
        const auto &p = *m_current;
        out.prefix = m_current;
        out.prefixInstances = {p.instanceBytes.data(), 1, p.instanceBytes.size(), p.instanceBytes.size()};
        out.prefixMaterials = {p.materialBytes.data(), 1, p.materialBytes.size(), p.materialBytes.size()};
        out.prefixSurfaces = {reinterpret_cast<const uint8_t *>(p.surfaces.data()), p.surfaces.size(),
                              sizeof(TraceSurface), sceneSurfaceBytes};
      } else {
        out.prefix.reset();
        out.prefixInstances = out.prefixMaterials = out.prefixSurfaces = {};
      }
      // An empty frame uploads (and signs) one default surface.
      if (!surfaceCount()) {
        const TraceSurface empty{};
        out.tailSurfaces.assign(reinterpret_cast<const uint8_t *>(&empty),
                                reinterpret_cast<const uint8_t *>(&empty) + sceneSurfaceBytes);
      }
    }
    const Statistics &statistics() const { return m_statistics; }

  private:
    struct FactorCache {
      uint32_t decoded = 0;
      bool valid = false;
      float values[4] = {};
    };
    // A unit's tables, laid out as if it were a frame's only instance. The
    // material slot fields are placeholders until layout() copies the frame's.
    struct Unit {
      uint64_t generation = 0; // of the StaticUnit these were built from
      bool built = false, traced = false;
      const View *view = nullptr;
      const void *geometry = nullptr;
      std::vector<TraceSurface> surfaces;                      // one per geometry group
      std::vector<uint16_t> material;                          // local material of each entry
      std::vector<uint16_t> source;                            // unit surface of each entry
      std::vector<PrimaryDraw> primary;                        // surfaceIndex relative to the unit
      InstanceInput input;                                     // customIndex relative to the unit
      std::vector<uint8_t> staticBytes;
      uint8_t scene[sceneInstanceBytes] = {};
      std::vector<std::shared_ptr<scene::Material>> materials; // distinct, in first-use order
      Flags flags;
      uint64_t contentDigest = 0;
      std::shared_ptr<const scene::StaticUnitState> state;     // recorded for its history
      // Temporal fields are the steady ones: the committed frame traced this
      // very state, so they change only with the unit or the history.
      bool temporalFinal = false;
    };
    struct Layout {
      size_t surface = 0, primary = 0, input = 0, staticOffset = 0, dynamicOffset = 0, slot = 0;
    };
    struct Placement {
      uint32_t slot = 0;
      size_t surface = 0, primary = 0, bytes = 0, materials = 0; // materials: first entry in m_placementSlots
    };

    static Flags surfaceFlags(const scene::Draw &surface) {
      const auto &material = *surface.material;
      Flags f;
      f.blended = surface.blend.enabled;
      f.decal = surface.decal;
      const bool rough = !material.pbr || material.roughnessTexture ||
                         material.roughness >= REMIX_SHARC_PBR_MIN_ROUGHNESS;
      const bool opaque = !surface.blend.enabled, fixed = surface.classification == 0;
      f.receivers = fixed && opaque && rough;
      f.sharpReceivers = fixed && opaque && !rough;
      f.dynamicReceivers = !fixed && opaque && rough;
      f.dynamicSharpReceivers = !fixed && opaque && !rough;
      return f;
    }

    // A material's slot fields that do not depend on descriptor numbering.
    static MaterialSlots values(const scene::Material &material) {
      MaterialSlots entry;
      detail::materialFields(material, entry.pbr, entry.decode, entry.maps[3]);
      detail::filmFields(material, entry.film, entry.filmMaps);
      const auto decode = scene::materialDecode(material);
      std::copy_n(material.emission, 3, entry.emission);
      entry.emission[3] = decode.selfIlluminationScale;
      return entry;
    }

    // The material's slot fields at its first use in a frame: descriptors are
    // numbered from base + requests.size(), fingerprints appended to bytes.
    // maps deduplicates PBR maps; prefixMaps holds the prefix's.
    static MaterialSlots assign(const std::shared_ptr<scene::Material> &source,
                                std::vector<DescriptorRequest> &requests, std::vector<uint8_t> &bytes,
                                std::map<MapKey, uint32_t> &maps, size_t base, size_t limit, Env &env,
                                const std::map<MapKey, uint32_t> *prefixMaps = nullptr) {
      const auto &material = *source;
      auto append = [&](const void *data, size_t size) {
        const auto *p = static_cast<const uint8_t *>(data);
        bytes.insert(bytes.end(), p, p + size);
      };
      auto count = [&] { return base + requests.size(); };
      TraceSurface value;
      if (count() >= limit)
        env.tooManyObjects();
      value.textureState[0] = uint32_t(count());
      requests.push_back({source, nullptr, DescriptorRequest::Albedo});
      const auto fingerprint = material.texture->fingerprint();
      append(&fingerprint, sizeof(fingerprint));
      const uint8_t sampler[] = {material.filter, material.wrapU, material.wrapV};
      append(sampler, sizeof(sampler));
      detail::materialFields(material, value.pbr, value.material, value.pbrTextures[3]);
      detail::filmFields(material, value.film, value.filmMaps);
      const auto decode = scene::materialDecode(material);
      // The roughness, metallic and normal maps, then the film's thickness map.
      const std::shared_ptr<scene::Texture> *textures[] = {&material.roughnessTexture, &material.metallicTexture,
                                                           &material.normalTexture,
                                                           &material.iridescence.thicknessTexture};
      uint32_t filmMap = 0;
      uint32_t *slotsOut[] = {&value.pbrTextures[0], &value.pbrTextures[1], &value.pbrTextures[2], &filmMap};
      for (unsigned i = 0; i < 4; ++i) {
        const auto &map = *textures[i];
        if (!map || (i == 3 && value.film[0] <= 0))
          continue;
        const MapKey key{map.get(), material.filter, material.wrapU, material.wrapV};
        if (prefixMaps)
          if (const auto found = prefixMaps->find(key); found != prefixMaps->end()) {
            *slotsOut[i] = found->second + 1;
            continue;
          }
        auto [index, added] = maps.emplace(key, uint32_t(count()));
        if (added) {
          if (count() >= limit)
            env.tooManyObjects();
          requests.push_back({source, textures[i], DescriptorRequest::Map});
          const auto mapFingerprint = map->fingerprint();
          append(&mapFingerprint, sizeof(mapFingerprint));
        }
        *slotsOut[i] = index->second + 1;
      }
      value.filmMaps[3] |= filmMap;
      std::copy_n(material.emission, 3, value.emission);
      value.emission[3] = decode.selfIlluminationScale;
      if (material.emissionTexture &&
          (material.emission[0] > 0 || material.emission[1] > 0 || material.emission[2] > 0)) {
        if (count() >= limit)
          env.tooManyObjects();
        value.textureState[2] = uint32_t(count()) + 1;
        requests.push_back({source, nullptr, DescriptorRequest::Emission});
        const auto emissionFingerprint = material.emissionTexture->fingerprint();
        append(&emissionFingerprint, sizeof(emissionFingerprint));
      }
      MaterialSlots entry;
      entry.albedo = value.textureState[0];
      entry.emissive = value.textureState[2];
      std::copy_n(value.pbr, 4, entry.pbr);
      std::copy_n(value.material, 4, entry.decode);
      std::copy_n(value.pbrTextures, 4, entry.maps);
      std::copy_n(value.emission, 4, entry.emission);
      std::copy_n(value.film, 4, entry.film);
      std::copy_n(value.filmMaps, 4, entry.filmMaps);
      return entry;
    }

    // Table entries of an instance: each surface takes its geometry groups.
    static size_t groupCount(Span instance, const View &view) {
      const auto &geometry = *view.geometry;
      size_t count = 0, groupIndex = 0;
      for (const auto &preparedSurface : instance) {
        do
          ++count, ++groupIndex;
        while (groupIndex < geometry.groups.size() &&
               geometry.groups[groupIndex].surface == preparedSurface.draw.surfaceIndex);
      }
      return count;
    }

    // A surface's temporal fields: the committed frame's model and vertex
    // buffer when its geometry corresponds (the history tables, or the static
    // set's own history).
    void temporal(const Inputs &in, const scene::Draw &surface, uint64_t temporalId, TraceSurface &value) const {
      value.previousVertices = 0;
      value.previousModel = {};
      if (const auto *previous = in.history.previousGeometry(surface, temporalId)) {
        const scene::TemporalHistory::Key key{temporalId, surface.instanceIndex, surface.surfaceIndex};
        if (const auto *old = in.previousVertices.find(key)) {
          value.previousVertices = (*old)->address;
          value.previousModel = previous->draw.model;
        } else if (in.statics) {
          if (const uint64_t address = in.statics->previousVertices(key)) {
            value.previousVertices = address;
            value.previousModel = previous->draw.model;
          }
        }
      }
    }

    // One instance's ranges, as the rebuild path writes them. slots: the
    // material slot of each surface.
    void fill(const Inputs &in, Span instance, const View &view, uint64_t temporalId,
              const MaterialSlots *const *slots, TraceSurface *surfaceOut, size_t surfaceBase,
              PrimaryDraw *primaryOut, InstanceInput &input, uint8_t *staticCursor, uint8_t *dynamicCursor,
              uint8_t *sceneCursor, FactorCache &factor) const {
      const auto &draw = instance[0].draw;
      const auto &geometry = *view.geometry;
      const auto &model = draw.model;
      auto write = [](uint8_t *&cursor, const void *data, size_t size) {
        std::memcpy(cursor, data, size);
        cursor += size;
      };
      if (draw.classification == 1) {
        write(dynamicCursor, &draw.instanceId, sizeof(draw.instanceId));
        write(dynamicCursor, &draw.instanceIndex, sizeof(draw.instanceIndex));
        write(dynamicCursor, model.data(), sizeof(model));
        write(dynamicCursor, &geometry.revision, sizeof(geometry.revision));
        write(dynamicCursor, &geometry.fingerprint, sizeof(geometry.fingerprint));
      }
      if (draw.classification == 0) {
        write(staticCursor, &draw.instanceId, sizeof(draw.instanceId));
        write(staticCursor, &draw.instanceIndex, sizeof(draw.instanceIndex));
        write(staticCursor, model.data(), sizeof(model));
        const auto identity = reinterpret_cast<uintptr_t>(draw.mesh.get());
        write(staticCursor, &identity, sizeof(identity));
        write(staticCursor, &geometry.revision, sizeof(geometry.revision));
        write(staticCursor, &geometry.fingerprint, sizeof(geometry.fingerprint));
      }
      float scale = 0;
      for (int c = 0; c < 3; ++c)
        scale = std::max(scale, std::hypot(model[c * 4], model[c * 4 + 1], model[c * 4 + 2]));
      input = {draw.instanceId, geometry.accel->address, draw.instanceIndex, uint32_t(surfaceBase), model,
               geometry.diagonal * scale, geometry.revision,
               std::none_of(instance.begin(), instance.end(),
                            [](const auto &surface) { return surface.draw.doubleSided; }),
               // Wholly blended instances may be left out of shadow and bounce rays
               // (skipBlendedSecondaryRays); surfaces drawing caustics stay in them, the
               // light through them carrying the caustics.
               std::all_of(instance.begin(), instance.end(),
                           [](const auto &surface) {
                             return surface.draw.blend.enabled && !surface.draw.decal &&
                                    !surface.draw.material->caustics;
                           })};
      input.viewModel = draw.viewModel;
      input.reflectionOnly = draw.reflectionOnly;
      input.decal = std::all_of(instance.begin(), instance.end(), [](const auto &surface) { return surface.draw.decal; });
      write(sceneCursor, model.data(), sizeof(model));
      write(sceneCursor, &view.fingerprint, sizeof(view.fingerprint));
      write(sceneCursor, &draw.instanceId, sizeof(draw.instanceId));
      write(sceneCursor, &draw.instanceIndex, sizeof(draw.instanceIndex));
      write(sceneCursor, &draw.classification, sizeof(draw.classification));
      size_t groupIndex = 0, next = surfaceBase, slot = 0;
      TraceSurface *out = surfaceOut;
      for (const auto &preparedSurface : instance) {
        const auto &surface = preparedSurface.draw;
        const auto &material = *surface.material;
        const auto &entry = *slots[slot++];
        TraceSurface value;
        value.textureState[0] = entry.albedo;
        value.textureState[2] = entry.emissive;
        std::copy_n(entry.pbr, 4, value.pbr);
        std::copy_n(entry.decode, 4, value.material);
        std::copy_n(entry.maps, 4, value.pbrTextures);
        std::copy_n(entry.emission, 4, value.emission);
        std::copy_n(entry.film, 4, value.film);
        std::copy_n(entry.filmMaps, 4, value.filmMaps);
        value.vertices = view.vertices.at(surface.surfaceIndex)->address;
        const uint64_t debugKey[] = {surface.instanceId, surface.instanceIndex, surface.surfaceIndex};
        value.debugIdentity = scene::hashBytes(debugKey, sizeof(debugKey));
        value.indices = geometry.input->address + geometry.indexOffsets.at(surface.surfaceIndex);
        // A texture upload (including an unrelated animated tile in a shared
        // terrain atlas) does not change vertex correspondence. Keep motion and
        // let per-pixel depth/normal rejection, lighting confidence and TAA color
        // clipping respond to the shading that actually changed on screen.
        temporal(in, surface, temporalId, value);
        std::copy_n(material.tint, 4, value.tint);
        std::copy(preparedSurface.uv.begin(), preparedSurface.uv.end(), value.uv);
        const auto &b = surface.blend;
        // Blend factors are nearly always the same constant: decode each once.
        if (b.factor != factor.decoded || !factor.valid) {
          factor.decoded = b.factor;
          factor.valid = true;
          factor.values[0] = detail::linear(float((b.factor >> 16) & 255) / 255);
          factor.values[1] = detail::linear(float((b.factor >> 8) & 255) / 255);
          factor.values[2] = detail::linear(float(b.factor & 255) / 255);
          factor.values[3] = float(b.factor >> 24) / 255;
        }
        std::copy_n(factor.values, 4, value.factor);
        value.state[0] = (b.colorOp & 3u) | (b.colorOp & 4u) << 10 | (b.alphaOpTexture & 4u) << 11 |
                         b.color1 << 2 | b.color2 << 4 | (b.alphaOpTexture & 3u) << 6 | b.alpha1 << 8 |
                         b.alpha2 << 10;
        value.state[1] = surface.ignoreAlpha ? 7 : surface.alphaOverride ? surface.compare : material.compare;
        const float reference = surface.alphaOverride ? surface.reference : material.reference;
        std::memcpy(&value.state[2], &reference, 4);
        value.state[3] = uint32_t(surface.ignoreAlpha) | uint32_t(b.enabled) << 1 |
                         uint32_t(surface.doubleSided) << 2 | uint32_t(material.transmission) << 3 |
                         uint32_t(draw.classification == 0) << 4 |
                         uint32_t(material.translucent && !surface.decal) << 5 | uint32_t(surface.decal) << 6 |
                         (surface.decal ? detail::decalMode(b) : 0u) << 7 |
                         // Bit 11: light through it draws caustics.
                         uint32_t(material.caustics) << 11 |
                         // remixapi_InstanceInfoMaskEXT CUT: the group (bits 16-21) whose
                         // coverage hides the surface from camera rays.
                         uint32_t(surface.cutGroup) << 16 |
                         // remixapi_InstanceInfoCameraCullingEXT (bits 9-10): camera rays
                         // skip its back faces / its parts in clusters the camera cannot see.
                         uint32_t(surface.cameraCull & 3u) << 9;
        if (draw.classification == 0) {
          write(staticCursor, value.tint, sizeof(value.tint));
          write(staticCursor, value.factor, sizeof(value.factor));
          write(staticCursor, value.state, sizeof(value.state));
          write(staticCursor, value.material, sizeof(value.material));
        }
        *primaryOut++ = {model, uint32_t(next), uint32_t(surface.surface->indices.size()), b.enabled, draw.viewModel,
                         draw.reflectionOnly};
        do {
          const auto &group = geometry.groups.at(groupIndex++);
          value.triangleMap = group.triangleMap ? geometry.input->address + group.triangleMap : 0;
          value.traceIndices = geometry.input->address + group.indices;
          *out++ = value;
          ++next;
        } while (groupIndex < geometry.groups.size() && geometry.groups[groupIndex].surface == surface.surfaceIndex);
      }
    }

    Span unitSpan(uint32_t slot) const {
      const auto &surfaces = m_statics->units()[slot].surfaces;
      return {surfaces.data(), surfaces.size()};
    }

    // Takes the static set's changes: units gone, units to (re)build.
    void syncUnits(const Inputs &in, std::vector<uint32_t> &dirty) {
      auto &statics = *m_statics;
      const auto &units = statics.units();
      if (m_units.size() < units.size())
        m_units.resize(units.size());
      for (const auto &removed : statics.removed()) {
        forget(removed.slot);
        statics.record(removed.id, removed.index, in.serial, nullptr);
        m_layoutDirty = true;
      }
      for (const uint32_t slot : statics.rebuilt())
        if (units[slot].live)
          dirty.push_back(slot);
      // After reset(): every unit this table has not built.
      if (m_rescan) {
        for (const uint32_t slot : statics.order())
          if (!m_units[slot].built || m_units[slot].generation != units[slot].generation)
            dirty.push_back(slot);
        m_rescan = false;
      }
      // Trace order, as the rebuild path meets them.
      const auto less = [&](uint32_t a, uint32_t b) {
        return units[a].id != units[b].id ? units[a].id < units[b].id : units[a].index < units[b].index;
      };
      std::sort(dirty.begin(), dirty.end(), less);
      dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
      if (!dirty.empty() || statics.orderChanged())
        m_layoutDirty = true;
    }

    void forget(uint32_t slot) {
      if (slot >= m_units.size())
        return;
      auto &unit = m_units[slot];
      if (unit.geometry) {
        const auto found = m_geometryUnits.find(unit.geometry);
        if (found != m_geometryUnits.end()) {
          std::erase(found->second, slot);
          if (found->second.empty())
            m_geometryUnits.erase(found);
        }
      }
      unit = {};
    }

    void buildUnit(Env &env, const Inputs &in, uint32_t slot, const View &view) {
      const auto &source = m_statics->units()[slot];
      const Span instance{source.surfaces.data(), source.surfaces.size()};
      forget(slot);
      auto &unit = m_units[slot];
      unit.generation = source.generation;
      unit.built = true;
      unit.traced = !source.singular;
      unit.view = &view;
      unit.geometry = view.geometry.get();
      m_geometryUnits[unit.geometry].push_back(slot);
      // Local materials, in first-use order, with placeholder slot fields.
      std::vector<uint16_t> surfaceMaterial;
      surfaceMaterial.reserve(instance.size());
      for (const auto &surface : instance) {
        const auto &material = surface.draw.material;
        size_t index = 0;
        while (index < unit.materials.size() && unit.materials[index] != material)
          ++index;
        if (index == unit.materials.size())
          unit.materials.push_back(material);
        surfaceMaterial.push_back(uint16_t(index));
        unit.flags |= surfaceFlags(surface.draw);
      }
      // Slot fields without descriptor numbers (layout() adds those): the
      // SHARC bytes take the material's decode fields.
      std::vector<MaterialSlots> local;
      local.reserve(unit.materials.size());
      for (const auto &material : unit.materials)
        local.push_back(values(*material));
      std::vector<const MaterialSlots *> slots;
      slots.reserve(instance.size());
      for (const auto index : surfaceMaterial)
        slots.push_back(&local[index]);
      unit.contentDigest = view.contentDigest;
      unit.surfaces.resize(groupCount(instance, view));
      unit.primary.resize(instance.size());
      unit.staticBytes.resize(staticInstanceBytes + staticSurfaceBytes * instance.size());
      FactorCache factor;
      fill(in, instance, view, source.id, slots.data(), unit.surfaces.data(), 0, unit.primary.data(), unit.input,
           unit.staticBytes.data(), nullptr, unit.scene, factor);
      const auto &geometry = *view.geometry;
      size_t groupIndex = 0;
      for (size_t s = 0; s < instance.size(); ++s) {
        do {
          unit.material.push_back(surfaceMaterial[s]);
          unit.source.push_back(uint16_t(s));
          ++groupIndex;
        } while (groupIndex < geometry.groups.size() &&
                 geometry.groups[groupIndex].surface == instance[s].draw.surfaceIndex);
      }
      // What the rebuild path would stage into the temporal history.
      std::shared_ptr<scene::StaticUnitState> state;
      if (unit.traced) {
        state = std::make_shared<scene::StaticUnitState>();
        state->surfaces.reserve(instance.size());
        for (const auto &surface : instance) {
          auto draw = surface.draw;
          draw.instanceId = source.id;
          state->surfaces.emplace_back(std::move(draw), surface.animationFrame);
          state->owners.push_back(env.owner(view, surface.draw.surfaceIndex));
          state->vertices.push_back(view.vertices.at(surface.draw.surfaceIndex)->address);
        }
        m_statistics.staticRebuilt += unit.surfaces.size();
        ++m_statistics.rebuiltUnits;
      }
      unit.state = state;
      m_statics->record(source.id, source.index, in.serial, std::move(state));
      if (unit.traced)
        m_temporalPending.push_back(slot);
    }

    // A compacted BLAS moved: the unit's TLAS input follows it.
    void readdress(uint32_t slot) {
      const auto &units = m_statics->units();
      if (slot >= m_units.size() || slot >= units.size() || !units[slot].live || !m_units[slot].built ||
          m_units[slot].generation != units[slot].generation)
        return;
      auto &unit = m_units[slot];
      const auto address = unit.view->geometry->accel->address;
      if (unit.input.geometry != address) {
        unit.input.geometry = address;
        m_layoutDirty = true;
      }
    }

    // Temporal fields of units whose history may differ from what their
    // tables hold: (re)built units until their own state is committed, and
    // every unit after a history reset or a committed frame traced without
    // the set. Fields outside the scene signature: the kept prefix is patched
    // in place.
    void updateTemporal(const Inputs &in) {
      auto &statics = *m_statics;
      const bool steady = !in.history.reset() && statics.hasCommitted() && !statics.committedAbsent();
      const bool all = !steady || !m_temporalSteady;
      m_temporalSteady = steady;
      const auto &units = statics.units();
      const bool patch = m_current && !m_layoutDirty;
      bool patched = false;
      // Every unit after an event; otherwise only those still pending.
      std::vector<uint32_t> visit;
      if (all)
        visit = statics.order();
      else {
        std::sort(m_temporalPending.begin(), m_temporalPending.end());
        m_temporalPending.erase(std::unique(m_temporalPending.begin(), m_temporalPending.end()),
                                m_temporalPending.end());
        visit = std::move(m_temporalPending);
      }
      m_temporalPending.clear();
      for (const uint32_t slot : visit) {
        if (slot >= m_units.size() || slot >= units.size() || !units[slot].live)
          continue;
        auto &unit = m_units[slot];
        if (!unit.built || !unit.traced || unit.generation != units[slot].generation ||
            (unit.temporalFinal && !all))
          continue;
        const auto &source = units[slot];
        TraceSurface *kept = patch && slot < m_current->first.size() && m_current->first[slot] != ~0u
                                 ? m_current->surfaces.data() + m_current->first[slot]
                                 : nullptr;
        for (size_t i = 0; i < unit.surfaces.size(); ++i) {
          auto &value = unit.surfaces[i];
          temporal(in, source.surfaces[unit.source[i]].draw, source.id, value);
          if (kept && (kept[i].previousVertices != value.previousVertices ||
                       kept[i].previousModel != value.previousModel)) {
            kept[i].previousVertices = value.previousVertices;
            kept[i].previousModel = value.previousModel;
            patched = true;
          }
        }
        m_statistics.temporalSurfaces += unit.surfaces.size();
        unit.temporalFinal = steady && unit.state && statics.committedState(source.id, source.index) == unit.state.get();
        if (!unit.temporalFinal)
          m_temporalPending.push_back(slot);
      }
      m_prefixVersion += patched;
    }

    // Lays the static prefix out from the units' tables, in trace order.
    void layout(Env &env, const Inputs &in) {
      auto &statics = *m_statics;
      std::shared_ptr<Prefix> next;
      for (auto &candidate : m_pool)
        if (candidate.use_count() == 1) {
          next = candidate;
          break;
        }
      if (!next) {
        next = std::make_shared<Prefix>();
        m_pool.push_back(next);
      }
      auto &p = *next;
      p.generation = ++m_generation;
      p.surfaces.clear();
      p.primary.clear();
      p.inputs.clear();
      p.staticBytes.clear();
      p.instanceBytes.clear();
      p.materialBytes.clear();
      p.requests.clear();
      p.materials.clear();
      p.maps.clear();
      p.first.assign(m_units.size(), ~0u);
      p.flags = {};
      p.geometryDigest = seed;
      p.reuseSignature = seed;
      p.reuse = in.pbrReuse;
      // Serial: offsets, and material slots numbered by first use.
      m_placements.clear();
      m_placementSlots.clear();
      size_t surfaces = 0, primary = 0, bytes = 0;
      const uint64_t materialStart = ticks();
      for (const uint32_t slot : statics.order()) {
        const auto &unit = m_units[slot];
        if (!unit.traced)
          continue;
        if (surfaces > 0xffffffu)
          env.tooManyObjects();
        m_placements.push_back({slot, surfaces, primary, bytes, m_placementSlots.size()});
        for (const auto &material : unit.materials) {
          auto [entry, first] = p.materials.try_emplace(material.get());
          if (first)
            entry->second = assign(material, p.requests, p.materialBytes, p.maps, 0, in.textureLimit, env);
          m_placementSlots.push_back(&entry->second);
        }
        p.first[slot] = uint32_t(surfaces);
        surfaces += unit.surfaces.size();
        primary += unit.primary.size();
        bytes += unit.staticBytes.size();
        p.flags |= unit.flags;
        if (in.digest)
          p.geometryDigest = scene::hashBytes(&unit.contentDigest, sizeof(unit.contentDigest), p.geometryDigest);
      }
      if (in.materialTicks)
        *in.materialTicks += ticks() - materialStart;
      p.instances = m_placements.size();
      p.surfaces.resize(surfaces);
      p.primary.resize(primary);
      p.inputs.resize(m_placements.size());
      p.staticBytes.resize(bytes);
      p.instanceBytes.resize(m_placements.size() * sceneInstanceBytes);
      // Parallel: each unit's tables with the frame's slots and offsets.
      constexpr size_t unitsPerTask = 256;
      in.workers.run((m_placements.size() + unitsPerTask - 1) / unitsPerTask, [&](size_t task) {
        const size_t last = std::min(m_placements.size(), (task + 1) * unitsPerTask);
        for (size_t i = task * unitsPerTask; i < last; ++i) {
          const auto &placement = m_placements[i];
          const auto &unit = m_units[placement.slot];
          const auto *slots = m_placementSlots.data() + placement.materials;
          TraceSurface *out = p.surfaces.data() + placement.surface;
          for (size_t s = 0; s < unit.surfaces.size(); ++s) {
            auto value = unit.surfaces[s];
            const auto &entry = *slots[unit.material[s]];
            value.textureState[0] = entry.albedo;
            value.textureState[2] = entry.emissive;
            std::copy_n(entry.pbr, 4, value.pbr);
            std::copy_n(entry.decode, 4, value.material);
            std::copy_n(entry.maps, 4, value.pbrTextures);
            std::copy_n(entry.emission, 4, value.emission);
            std::copy_n(entry.film, 4, value.film);
            std::copy_n(entry.filmMaps, 4, value.filmMaps);
            out[s] = value;
          }
          for (size_t d = 0; d < unit.primary.size(); ++d) {
            auto draw = unit.primary[d];
            draw.surfaceIndex += uint32_t(placement.surface);
            p.primary[placement.primary + d] = draw;
          }
          auto input = unit.input;
          input.customIndex = uint32_t(placement.surface);
          p.inputs[i] = input;
          std::memcpy(p.staticBytes.data() + placement.bytes, unit.staticBytes.data(), unit.staticBytes.size());
          std::memcpy(p.instanceBytes.data() + i * sceneInstanceBytes, unit.scene, sceneInstanceBytes);
        }
      });
      if (in.pbrReuse)
        p.reuseSignature = unitsReuse(seed);
      m_current = std::move(next);
      ++m_prefixVersion;
      ++m_statistics.relayouts;
    }

    // pbrLocalReuse's signature over the static units, in trace order.
    uint64_t unitsReuse(uint64_t hash) const {
      for (const uint32_t slot : m_statics->order())
        if (m_units[slot].traced)
          for (const auto &surface : m_statics->units()[slot].surfaces)
            hash = reuseHash(surface, *m_units[slot].view, hash);
      return hash;
    }
    static uint64_t reuseHash(const scene::TraceFrame::Surface &preparedSurface, const View &view, uint64_t hash) {
      const auto &surface = preparedSurface.draw;
      const uint64_t debugKey[] = {surface.instanceId, surface.instanceIndex, surface.surfaceIndex};
      const uint64_t values[] = {view.fingerprint, scene::hashBytes(debugKey, sizeof(debugKey)),
                                 scene::TemporalHistory::materialSignature(surface), preparedSurface.animationFrame};
      hash = scene::hashBytes(values, sizeof(values), hash);
      return scene::hashBytes(surface.model.data(), sizeof(surface.model), hash);
    }

    // The frame's own instances, continuing the prefix's tables. Without a
    // prefix this is the whole rebuild path.
    void buildTail(Env &env, const Inputs &in) {
      auto &prepared = in.frame;
      auto &t = m_tail;
      const Prefix *p = m_current.get();
      t.surfaces.clear();
      t.primary.clear();
      t.inputs[0].clear();
      t.inputs[1].clear();
      t.staticBytes.clear();
      t.dynamicBytes.clear();
      t.instanceBytes.clear();
      t.materialBytes.clear();
      t.requests.clear();
      m_tailMaterials.clear();
      m_tailMaps.clear();
      m_flags = p ? p->flags : Flags{};
      m_geometryDigest = p ? p->geometryDigest : seed;
      const size_t count = prepared.instances.size();
      m_tailInstances = count;
      const size_t base = p ? p->requests.size() : 0;
      // Serial, draw order: material slots, numbered by first use.
      size_t preparedSurfaces = 0;
      for (size_t index = 0; index < count; ++index)
        preparedSurfaces += prepared.instances[index].size();
      m_surfaceSlots.resize(preparedSurfaces);
      const uint64_t materialStart = ticks();
      for (size_t index = 0, slot = 0; index < count; ++index) {
        const auto instance = prepared.instances[index];
        if (p && instance[0].draw.classification == 0)
          throw std::logic_error("A static instance outside the static set follows its prefix");
        for (const auto &preparedSurface : instance) {
          const auto &surface = preparedSurface.draw;
          m_flags |= surfaceFlags(surface);
          const MaterialSlots *entry = nullptr;
          if (p)
            if (const auto found = p->materials.find(surface.material.get()); found != p->materials.end())
              entry = &found->second;
          if (!entry) {
            auto [slots, firstUse] = m_tailMaterials.try_emplace(surface.material.get());
            if (firstUse)
              slots->second = assign(surface.material, t.requests, t.materialBytes, m_tailMaps, base,
                                     in.textureLimit, env, p ? &p->maps : nullptr);
            entry = &slots->second;
          }
          m_surfaceSlots[slot++] = entry;
        }
      }
      if (in.materialTicks)
        *in.materialTicks += ticks() - materialStart;
      // Serial, draw order: every instance's ranges.
      const size_t surfaceBase = p ? p->surfaces.size() : 0, primaryBase = p ? p->primary.size() : 0;
      m_layouts.resize(count);
      size_t surfaceCount = surfaceBase, primaryCount = primaryBase, inputCount[2] = {}, staticBytes = 0,
             dynamicBytes = 0;
      for (size_t index = 0, slot = 0; index < count; ++index) {
        const auto instance = prepared.instances[index];
        const auto &draw = instance[0].draw;
        const auto &view = *m_tailViews[index];
        if (in.digest)
          m_geometryDigest = scene::hashBytes(&view.contentDigest, sizeof(view.contentDigest), m_geometryDigest);
        if (surfaceCount > 0xffffffu)
          env.tooManyObjects();
        m_layouts[index] = {surfaceCount, primaryCount, inputCount[draw.classification]++, staticBytes,
                            dynamicBytes, slot};
        surfaceCount += groupCount(instance, view);
        primaryCount += instance.size();
        slot += instance.size();
        if (draw.classification == 0)
          staticBytes += staticInstanceBytes + staticSurfaceBytes * instance.size();
        else
          dynamicBytes += dynamicInstanceBytes;
      }
      t.surfaces.resize(surfaceCount - surfaceBase);
      t.primary.resize(primaryCount - primaryBase);
      t.inputs[0].resize(inputCount[0]);
      t.inputs[1].resize(inputCount[1]);
      t.staticBytes.resize(staticBytes);
      t.dynamicBytes.resize(dynamicBytes);
      t.instanceBytes.resize(sceneInstanceBytes * count);
      // Parallel: each instance fills its own ranges. History and previous
      // vertices are only read here; staging follows.
      constexpr size_t instancesPerTask = 256;
      in.workers.run((count + instancesPerTask - 1) / instancesPerTask, [&](size_t task) {
        FactorCache factor;
        const size_t last = std::min(count, (task + 1) * instancesPerTask);
        for (size_t index = task * instancesPerTask; index < last; ++index) {
          const auto instance = prepared.instances[index];
          const auto &draw = instance[0].draw;
          const auto &layout = m_layouts[index];
          // Frame-local identities never gain history (all surfaces share the id).
          const uint64_t temporalId = prepared.persistentIds.count(draw.instanceId) ? draw.instanceId : 0;
          fill(in, instance, *m_tailViews[index], temporalId, m_surfaceSlots.data() + layout.slot,
               t.surfaces.data() + (layout.surface - surfaceBase), layout.surface,
               t.primary.data() + (layout.primary - primaryBase), t.inputs[draw.classification][layout.input],
               t.staticBytes.data() + layout.staticOffset, t.dynamicBytes.data() + layout.dynamicOffset,
               t.instanceBytes.data() + sceneInstanceBytes * index, factor);
        }
      });
      // Serial: previous-vertex and history staging. Consumes the prepared
      // draws: each surface moves into the temporal history.
      const uint64_t stageStart = ticks();
      m_reuseSignature = seed;
      if (in.pbrReuse && p)
        m_reuseSignature = p->reuse ? p->reuseSignature : unitsReuse(seed);
      for (size_t index = 0; index < count; ++index) {
        auto instance = prepared.instances[index];
        const auto &draw = instance[0].draw;
        const uint64_t temporalId = prepared.persistentIds.count(draw.instanceId) ? draw.instanceId : 0;
        const auto &view = *m_tailViews[index];
        if (in.pbrReuse)
          for (const auto &preparedSurface : instance)
            m_reuseSignature = reuseHash(preparedSurface, view, m_reuseSignature);
        if (!temporalId)
          continue;
        for (const auto &preparedSurface : instance)
          in.pendingVertices.emplace(
              {temporalId, preparedSurface.draw.instanceIndex, preparedSurface.draw.surfaceIndex},
              view.vertices.at(preparedSurface.draw.surfaceIndex));
        for (auto &preparedSurface : instance)
          in.history.stage(std::move(preparedSurface.draw), temporalId, preparedSurface.animationFrame);
      }
      if (in.temporalTicks)
        *in.temporalTicks += ticks() - stageStart;
    }

    scene::StaticScene *m_statics = nullptr;
    std::vector<Unit> m_units;
    std::unordered_map<const void *, std::vector<uint32_t>> m_geometryUnits;
    std::vector<uint32_t> m_readdress, m_temporalPending;
    std::shared_ptr<Prefix> m_current;
    std::vector<std::shared_ptr<Prefix>> m_pool;
    std::vector<Placement> m_placements;
    std::vector<const MaterialSlots *> m_placementSlots;
    uint64_t m_generation = 0, m_prefixVersion = 0;
    bool m_layoutDirty = true, m_rescan = true, m_temporalSteady = false;
    Tail m_tail;
    size_t m_tailInstances = 0;
    std::vector<const View *> m_views, m_tailViews;
    std::vector<const MaterialSlots *> m_surfaceSlots;
    std::vector<Layout> m_layouts;
    std::unordered_map<const scene::Material *, MaterialSlots> m_tailMaterials;
    std::map<MapKey, uint32_t> m_tailMaps;
    Flags m_flags;
    uint64_t m_geometryDigest = seed, m_reuseSignature = seed;
    Statistics m_statistics;
  };
} // namespace remix::trace

#include "scene/fixtures.h"
#include "scene/micromap.h"
#include "scene/opacity.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <tuple>

using namespace remix::scene;
using namespace remix::test;

namespace {
  [[noreturn]] void fail(const char *what) {
    require(false, what);
    std::abort();
  }
  // Deterministic LCG; the test must not depend on the platform's random engines.
  struct Random {
    uint64_t state;
    uint32_t next() {
      state = state * 6364136223846793005ull + 1442695040888963407ull;
      return uint32_t(state >> 33);
    }
    float unit() { return float(next() & 0xffffff) / float(0x1000000); }
  };

  bool inside(std::array<float, 2> p, const std::array<std::array<float, 2>, 3> &t) {
    auto edge = [](std::array<float, 2> a, std::array<float, 2> b, std::array<float, 2> c) {
      return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    };
    const float d0 = edge(t[0], t[1], p), d1 = edge(t[1], t[2], p), d2 = edge(t[2], t[0], p);
    return (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
  }

  // The shader's texture alpha (textureLod at mip 0) for the supported wrap modes.
  float sampleAlpha(const Draw &draw, float u, float v) {
    const auto &m = *draw.material;
    const auto &p = m.texture->pixels;
    auto wrap = [](int64_t x, int64_t n, uint32_t mode) -> int64_t {
      if (mode == 0) return std::clamp<int64_t>(x, 0, n - 1);
      if (mode == 2) {
        const int64_t period = ((x % (2 * n)) + 2 * n) % (2 * n);
        return period < n ? period : 2 * n - 1 - period;
      }
      return ((x % n) + n) % n;
    };
    auto texel = [&](int64_t x, int64_t y) {
      x = wrap(x, p.width, m.wrapU);
      y = wrap(y, p.height, m.wrapV);
      return p.mips[0][4 * (size_t(y) * p.width + size_t(x)) + 3] / 255.f;
    };
    const float x = u * p.width, y = v * p.height;
    if (!m.filter) return texel(int64_t(std::floor(x)), int64_t(std::floor(y)));
    const float fx = x - .5f, fy = y - .5f;
    const int64_t x0 = int64_t(std::floor(fx)), y0 = int64_t(std::floor(fy));
    const float ax = fx - float(x0), ay = fy - float(y0);
    return (texel(x0, y0) * (1 - ax) + texel(x0 + 1, y0) * ax) * (1 - ay) +
           (texel(x0, y0 + 1) * (1 - ax) + texel(x0 + 1, y0 + 1) * ax) * ay;
  }

  // scene.glsl materialAlpha + alphaPass for an alpha-tested surface.
  bool passes(const Draw &draw, const std::array<float, 4> &uvTransform, uint32_t triangle, float b1, float b2) {
    const auto &s = *draw.surface;
    const auto &b = draw.blend;
    const float b0 = 1 - b1 - b2;
    const Vertex *v[3];
    for (unsigned c = 0; c < 3; ++c) v[c] = &s.vertices[s.indices[triangle * 3 + c]];
    const float u = v[0]->uv[0] * b0 + v[1]->uv[0] * b1 + v[2]->uv[0] * b2;
    const float w = v[0]->uv[1] * b0 + v[1]->uv[1] * b1 + v[2]->uv[1] * b2;
    const float texel = sampleAlpha(draw, u * uvTransform[0] + uvTransform[2], w * uvTransform[1] + uvTransform[3]);
    const float vertex = v[0]->color[3] * b0 + v[1]->color[3] * b1 + v[2]->color[3] * b2;
    auto arg = [&](uint32_t i) { return i == 1 ? texel : i == 2 ? vertex : i == 3 ? (b.factor >> 24) / 255.f : 1.f; };
    const float a = arg(b.alpha1), c = arg(b.alpha2);
    const uint32_t op = b.alphaOpTexture;
    float alpha = op == 1 ? a : op == 2 ? c : op == 6 ? std::min(a + c, 1.f)
                : std::min(a * c * (op == 4 || op == 7 ? 2.f : op == 5 ? 4.f : 1.f), 1.f);
    alpha = std::clamp(alpha * draw.material->tint[3], 0.f, 1.f);
    const float ref = draw.material->reference;
    const uint32_t cmp = draw.material->compare;
    return (cmp == 1 && alpha < ref) || (cmp == 3 && alpha <= ref) || (cmp == 4 && alpha > ref) ||
           (cmp == 6 && alpha >= ref);
  }

  // Every point sampled inside an opaque micro-triangle passes the shader's
  // alpha test and every point inside a transparent one fails it.
  void checkSound(const Draw &draw, const std::array<float, 4> &uv, const OpacityPartition &partition,
                  Random &random, uint64_t &checked) {
    const auto &micromap = *partition.micromap;
    for (uint32_t triangle = 0; triangle < partition.triangles; ++triangle) {
      if (partition.opaque(triangle)) continue;
      const int32_t index = micromap.indices[triangle];
      uint32_t level = 0;
      auto state = [&](uint32_t micro) -> uint32_t {
        if (index == OpacityMicromap::FullyOpaque) return OpacityMicromap::Opaque;
        if (index == OpacityMicromap::FullyTransparent) return OpacityMicromap::Transparent;
        if (index < 0) return OpacityMicromap::UnknownOpaque;
        const auto &t = micromap.triangles[size_t(index)];
        return (micromap.data[t.dataOffset + micro / 4] >> (2 * (micro % 4))) & 3u;
      };
      if (index >= 0) {
        const auto &t = micromap.triangles[size_t(index)];
        require(t.format == OpacityMicromap::Format4State && t.subdivisionLevel >= 1 &&
                    t.subdivisionLevel <= OpacityMicromap::maxLevel,
                "Micromap triangle has an invalid format or level");
        level = t.subdivisionLevel;
      }
      const auto &micros = microTriangles(level);
      for (uint32_t micro = 0; micro < micros.size(); ++micro) {
        const uint32_t value = state(micro);
        if (value == OpacityMicromap::UnknownOpaque) continue;
        const auto corners = microCorners(micros[micro], level);
        for (unsigned sample = 0; sample < 24; ++sample) {
          // Random barycentric point of the micro-triangle, including its corners.
          float r1 = random.unit(), r2 = random.unit();
          if (sample < 3) r1 = sample == 1 ? 1.f : 0.f, r2 = sample == 2 ? 1.f : 0.f;
          if (r1 + r2 > 1) r1 = 1 - r1, r2 = 1 - r2;
          const float b1 = corners[0][0] + (corners[1][0] - corners[0][0]) * r1 + (corners[2][0] - corners[0][0]) * r2;
          const float b2 = corners[0][1] + (corners[1][1] - corners[0][1]) * r1 + (corners[2][1] - corners[0][1]) * r2;
          const bool pass = passes(draw, uv, triangle, b1, b2);
          if (value == OpacityMicromap::Opaque && !pass) {
            std::fprintf(stderr, "triangle %u micro %u level %u at (%f, %f)\n", triangle, micro, level, b1, b2);
            fail("Opaque micro-triangle contains a point the alpha test rejects");
          }
          if (value == OpacityMicromap::Transparent && pass) {
            std::fprintf(stderr, "triangle %u micro %u level %u at (%f, %f)\n", triangle, micro, level, b1, b2);
            fail("Transparent micro-triangle contains a point the alpha test accepts");
          }
          ++checked;
        }
      }
    }
  }
} // namespace

int main() {
  // Index order: hardware-measured anchors (RTX 5080, tools/vulkan-probes
  // micromap mode) and a bijection per level whose children nest.
  const std::tuple<uint32_t, uint32_t, uint16_t, uint16_t, bool> anchors[] = {
      {1, 1, 0, 0, false}, {1, 3, 0, 1, true},  {2, 4, 0, 1, false}, {2, 7, 1, 0, false},
      {2, 10, 3, 0, true}, {2, 13, 0, 2, false}, {2, 15, 0, 3, true}};
  for (const auto &[level, index, i, j, upright] : anchors) {
    const auto &m = microTriangles(level)[index];
    require(m.i == i && m.j == j && m.upright == upright, "Micro-triangle order differs from hardware");
  }
  for (uint32_t level = 0; level <= OpacityMicromap::maxLevel; ++level) {
    const auto &table = microTriangles(level);
    require(table.size() == size_t(1) << (2 * level), "Micro-triangle table size");
    std::set<std::tuple<uint16_t, uint16_t, bool>> seen;
    for (const auto &m : table) seen.insert({m.i, m.j, m.upright});
    require(seen.size() == table.size(), "Micro-triangle index is not a bijection");
    if (level == 0 || level > 6) continue;
    const auto &parents = microTriangles(level - 1);
    for (uint32_t index = 0; index < table.size(); ++index) {
      const auto corners = microCorners(table[index], level);
      const std::array<float, 2> centroid{(corners[0][0] + corners[1][0] + corners[2][0]) / 3,
                                          (corners[0][1] + corners[1][1] + corners[2][1]) / 3};
      require(inside(centroid, microCorners(parents[index / 4], level - 1)),
              "Micro-triangle children are not nested in their parent");
    }
  }

  // Soundness over alpha patterns, compares, filtering, wrapping, vertex alpha,
  // combiners and tint; at least some micro-triangles must be proved.
  Random random{12345};
  uint64_t checked = 0, proved = 0, stored = 0, unknown = 0;
  for (unsigned scene = 0; scene < 96; ++scene) {
    OpacityCache cache;
    cache.setMicromapLevel(1 + scene % 6);
    Draw draw;
    draw.doubleSided = true;
    draw.surface = std::make_shared<Surface>();
    for (unsigned v = 0; v < 12; ++v) {
      const float spread = scene % 3 == 0 ? 1.f : scene % 3 == 1 ? 2.3f : .4f;
      draw.surface->vertices.push_back(
          {{random.unit(), random.unit(), random.unit()},
           {(random.unit() - .25f) * spread, (random.unit() - .25f) * spread},
           {1, 1, 1, scene % 4 == 3 ? random.unit() : 1.f}});
    }
    for (unsigned t = 0; t < 8; ++t)
      for (unsigned c = 0; c < 3; ++c) draw.surface->indices.push_back(random.next() % 12);
    draw.material = std::make_shared<Material>();
    const uint32_t compares[] = {1, 3, 4, 6};
    draw.material->compare = compares[scene % 4];
    draw.material->reference = .1f + .8f * random.unit();
    draw.material->filter = (scene / 4) % 2 != 0;
    draw.material->wrapU = draw.material->wrapV = scene % 5 == 0 ? 0u : scene % 5 == 1 ? 2u : 1u;
    draw.material->tint[3] = scene % 7 == 0 ? .75f : 1.f;
    if (scene % 4 == 3) {
      draw.blend.alphaOpTexture = 3; // Texture x vertex alpha.
      draw.blend.alpha1 = 1;
      draw.blend.alpha2 = 2;
    }
    const uint32_t size = scene % 2 ? 16 : 32;
    draw.material->texture = std::make_shared<Texture>();
    std::vector<uint8_t> pixels(size_t(size) * size * 4, 255);
    for (uint32_t y = 0; y < size; ++y)
      for (uint32_t x = 0; x < size; ++x) {
        // Coherent blobs with hard and soft edges, as in foliage cutouts.
        const float d = std::hypot(float(x) - size * .4f, float(y) - size * .55f) / size;
        const float noise = (random.next() % 32) / 255.f;
        pixels[4 * (size_t(y) * size + x) + 3] = uint8_t(std::clamp((d < .3f ? 1.f : .35f - d) + noise, 0.f, 1.f) * 255);
      }
    draw.material->texture->pixels = {size, size, true, {pixels}};
    const std::array<float, 4> uv{1, 1, scene % 6 == 5 ? .25f : 0.f, 0};
    const auto partition = cache.partition(draw, uv);
    if (!partition->micromap) continue;
    checkSound(draw, uv, *partition, random, checked);
    proved += partition->micromap->opaque + partition->micromap->transparent;
    unknown += partition->micromap->unknown;
    stored += partition->micromap->triangles.size();
  }
  std::printf("micromap soundness: %llu samples, %llu proved and %llu unknown micro-triangles, %llu stored\n",
              (unsigned long long)checked, (unsigned long long)proved, (unsigned long long)unknown,
              (unsigned long long)stored);
  require(checked > 10000 && proved > 1000 && stored > 0, "Micromap baking proved too little to test");

  // Level 0 and decals bake nothing; blended and one-sided surfaces keep the
  // shader test; a replaced texture proves the micromap again.
  OpacityCache cache;
  Draw draw;
  draw.doubleSided = true;
  draw.surface = std::make_shared<Surface>();
  // Inset UVs on a clamped 32x32 texture whose right half is transparent:
  // micro-triangles away from the edge are provable on both sides.
  draw.surface->vertices = {{{0, 0, 0}, {.05f, .05f}, {1, 1, 1, 1}}, {{1, 0, 0}, {.95f, .05f}, {1, 1, 1, 1}},
                            {{0, 1, 0}, {.05f, .95f}, {1, 1, 1, 1}}};
  draw.surface->indices = {0, 1, 2};
  draw.material = std::make_shared<Material>();
  draw.material->compare = 4;
  draw.material->reference = .5f;
  draw.material->wrapU = draw.material->wrapV = 0;
  draw.material->texture = std::make_shared<Texture>();
  std::vector<uint8_t> half(32 * 32 * 4, 255);
  for (uint32_t y = 0; y < 32; ++y)
    for (uint32_t x = 16; x < 32; ++x) half[4 * (y * 32 + x) + 3] = 0;
  draw.material->texture->pixels = {32, 32, true, {half}};
  const std::array<float, 4> identity{1, 1, 0, 0};
  require(!cache.partition(draw, identity)->micromap, "Micromap baked while disabled");
  cache.setMicromapLevel(4);
  const auto baked = cache.partition(draw, identity);
  require(baked->micromap && baked->micromap->opaque && baked->micromap->transparent,
          "Half-transparent texture produced no opaque and transparent micro-triangles");
  cache.setMicromapLevel(0);
  require(!cache.partition(draw, identity)->micromap, "Disabling micromaps reused a baked partition");
  cache.setMicromapLevel(4);
  require(cache.partition(draw, identity) == baked, "Unchanged micromap partition was not cached");
  auto decal = draw;
  decal.decal = true;
  require(!cache.partition(decal, identity)->micromap, "A decal received a micromap");
  auto blended = draw;
  blended.blend.enabled = true;
  require(!cache.partition(blended, identity)->micromap, "A blended surface received a micromap");
  auto oneSided = draw;
  oneSided.doubleSided = false;
  require(!cache.partition(oneSided, identity)->micromap, "A one-sided surface received a micromap");
  auto replaced = draw;
  replaced.material = std::make_shared<Material>(*draw.material);
  replaced.material->texture = std::make_shared<Texture>(*draw.material->texture);
  for (uint32_t y = 0; y < 32; ++y) replaced.material->texture->pixels.mips[0][4 * (y * 32 + 24) + 3] = 255;
  const auto reproved = cache.partition(replaced, identity);
  require(reproved != baked && reproved->micromap && reproved->micromap->hash != baked->micromap->hash,
          "A replaced texture reused the previous micromap");
  std::puts("PASS");
  return 0;
}

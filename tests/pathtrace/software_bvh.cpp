// The software acceleration structures (pathtrace/software_bvh.h): the nearest
// hit of a traversal written as shaders/software_ray_query.glsl is, against
// every triangle of every instance tested directly.
#include "pathtrace/software_bvh.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace remix::trace::software;

namespace {
  struct Vec {
    double x, y, z;
  };
  Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
  Vec cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
  double dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
  float real(uint32_t bits) {
    float out;
    std::memcpy(&out, &bits, sizeof out);
    return out;
  }
  struct Hit {
    double t = 1e30;
    uint32_t primitive = ~0u, geometry = ~0u, customIndex = ~0u;
  };
  // margin: how far inside the edges the ray has to pass (negative: outside).
  bool triangle(Vec o, Vec d, Vec v0, Vec e1, Vec e2, double limit, double &t, double margin = 0) {
    const Vec h = cross(d, e2);
    const double a = dot(e1, h), f = 1 / a;
    const Vec s = o - v0;
    const double u = f * dot(s, h);
    const Vec q = cross(s, e1);
    const double v = f * dot(d, q);
    if (!(u >= margin && v >= margin && u + v <= 1 - margin))
      return false;
    t = f * dot(e2, q);
    return t > 0 && t < limit;
  }
  Vec word(const uint32_t *words, size_t index) {
    return {real(words[index * 4]), real(words[index * 4 + 1]), real(words[index * 4 + 2])};
  }
  bool box(Vec o, Vec inverse, Vec low, Vec high, double limit, double &enter) {
    double near = 0, far = limit;
    const double lows[3] = {low.x, low.y, low.z}, highs[3] = {high.x, high.y, high.z};
    const double origin[3] = {o.x, o.y, o.z}, rate[3] = {inverse.x, inverse.y, inverse.z};
    for (unsigned c = 0; c < 3; ++c) {
      const double a = (lows[c] - origin[c]) * rate[c], b = (highs[c] - origin[c]) * rate[c];
      near = std::max(near, std::min(a, b));
      far = std::min(far, std::max(a, b));
    }
    enter = near;
    return near <= far;
  }
  // The leaves of a structure the ray reaches, nearest child first.
  template <class Leaf> void traverse(const uint32_t *words, Vec o, Vec d, const double &limit, Leaf &&leaf) {
    if (words[0] == emptyReference)
      return;
    const Vec inverse = {1 / d.x, 1 / d.y, 1 / d.z};
    uint32_t stack[64], depth = 0, node = words[0];
    for (;;) {
      if (node & leafBit) {
        leaf(node & 0xffffffu, (node >> 24) & 127u);
        if (!depth)
          return;
        node = stack[--depth];
        continue;
      }
      const size_t at = 1 + size_t(node) * nodeWords;
      double enterA, enterB;
      const bool hitA = box(o, inverse, word(words, at), word(words, at + 1), limit, enterA);
      const bool hitB = box(o, inverse, word(words, at + 2), word(words, at + 3), limit, enterB);
      const uint32_t left = words[at * 4 + 3], right = words[(at + 1) * 4 + 3];
      if (hitA && hitB) {
        const bool swap = enterB < enterA;
        stack[depth++] = swap ? left : right;
        node = swap ? right : left;
      } else if (hitA)
        node = left;
      else if (hitB)
        node = right;
      else if (depth)
        node = stack[--depth];
      else
        return;
    }
  }

  struct Mesh {
    std::vector<float> positions[2];
    std::vector<uint32_t> indices[2];
    Structure structure;
  };
  struct Placed {
    const Mesh *mesh;
    Instance instance;
  };
  Vec apply(const float m[3][4], Vec p, double w) {
    return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3] * w,
            m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3] * w,
            m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3] * w};
  }
} // namespace

int main() {
  std::mt19937 random(7);
  std::uniform_real_distribution<float> unit(-1, 1);
  std::vector<Mesh> meshes(5);
  for (size_t m = 0; m < meshes.size(); ++m) {
    auto &mesh = meshes[m];
    // Sizes around the leaf limits: one triangle, a few, and thousands.
    const uint32_t counts[5][2] = {{1, 0}, {3, 2}, {300, 150}, {4000, 1}, {0, 900}};
    Geometry parts[2];
    for (unsigned g = 0; g < 2; ++g) {
      for (uint32_t t = 0; t < counts[m][g]; ++t) {
        const float centre[3] = {unit(random) * 4, unit(random) * 4, unit(random) * 4};
        for (unsigned v = 0; v < 3; ++v) {
          for (unsigned c = 0; c < 3; ++c)
            mesh.positions[g].push_back(centre[c] + unit(random) * .4f);
          mesh.indices[g].push_back(t * 3 + v);
        }
      }
      parts[g] = {mesh.positions[g].data(), mesh.indices[g].data(), counts[m][g], g == 0};
    }
    mesh.structure = buildBottom(parts, 2);
  }
  if (buildBottom(nullptr, 0).words[0] != emptyReference || buildTop(nullptr, 0).words[0] != emptyReference) {
    std::fprintf(stderr, "empty structures are not empty\n");
    return 1;
  }
  std::vector<Placed> placed;
  for (unsigned i = 0; i < 40; ++i) {
    Placed entry{&meshes[i % meshes.size()], {}};
    auto &instance = entry.instance;
    // A rotation about z with a scale (mirrored for some), and an offset.
    const float angle = unit(random) * 3, scale = (1 + .5f * unit(random)) * (i % 7 == 0 ? -1.f : 1.f);
    const float m[3][4] = {{std::cos(angle) * scale, -std::sin(angle), 0, unit(random) * 20},
                           {std::sin(angle) * scale, std::cos(angle), 0, unit(random) * 20},
                           {0, 0, 1.5f, unit(random) * 20}};
    std::memcpy(instance.transform, m, sizeof m);
    instance.customIndex = i * 10;
    instance.mask = 0xff;
    // The test reads a bottom level through its address.
    instance.bottom = reinterpret_cast<uint64_t>(entry.mesh->structure.words.data());
    std::copy_n(entry.mesh->structure.low, 3, instance.low);
    std::copy_n(entry.mesh->structure.high, 3, instance.high);
    placed.push_back(entry);
  }
  std::vector<Instance> instances;
  for (const auto &entry : placed)
    instances.push_back(entry.instance);
  Structure top = buildTop(instances.data(), instances.size());

  unsigned hits = 0, grazing = 0;
  // Every ray's nearest hit in the structures is the one found directly.
  const auto agree = [&](const char *what) {
    for (unsigned ray = 0; ray < 4000; ++ray) {
      const Vec o = {unit(random) * 30, unit(random) * 30, unit(random) * 30};
      Vec d = {unit(random), unit(random), unit(random)};
      if (ray % 5 == 0) // towards an instance, so that most of these hit
        d = Vec{placed[ray % placed.size()].instance.transform[0][3],
                placed[ray % placed.size()].instance.transform[1][3],
                placed[ray % placed.size()].instance.transform[2][3]} -
            o;
      // Every triangle of every instance, in world space.
      const auto direct = [&](double margin) {
        Hit nearest;
        for (const auto &entry : placed)
          for (unsigned g = 0; g < 2; ++g)
            for (size_t t = 0; t < entry.mesh->indices[g].size() / 3; ++t) {
              Vec corner[3];
              for (unsigned v = 0; v < 3; ++v) {
                const float *p = entry.mesh->positions[g].data() + entry.mesh->indices[g][t * 3 + v] * 3;
                corner[v] = apply(entry.instance.transform, {p[0], p[1], p[2]}, 1);
              }
              double distance;
              if (triangle(o, d, corner[0], corner[1] - corner[0], corner[2] - corner[0], nearest.t, distance, margin))
                nearest = {distance, uint32_t(t), g, entry.instance.customIndex};
            }
        return nearest;
      };
      const Hit expected = direct(0);
      // The structures.
      Hit found;
      const uint32_t *words = top.words.data();
      traverse(words, o, d, found.t, [&](uint32_t first, uint32_t count) {
        for (uint32_t i = first; i < first + count; ++i) {
          const uint32_t *instance = words + (size_t(words[1]) + size_t(i) * instanceWords) * 4;
          float inverse[3][4];
          for (unsigned r = 0; r < 3; ++r)
            for (unsigned c = 0; c < 4; ++c)
              inverse[r][c] = real(instance[r * 4 + c]);
          const Vec localOrigin = apply(inverse, o, 1), localDirection = apply(inverse, d, 0);
          const auto *bottom =
              reinterpret_cast<const uint32_t *>(uint64_t(instance[28]) | uint64_t(instance[29]) << 32);
          traverse(bottom, localOrigin, localDirection, found.t, [&](uint32_t firstTriangle, uint32_t triangles) {
            for (uint32_t t = firstTriangle; t < firstTriangle + triangles; ++t) {
              const size_t at = size_t(bottom[1]) + size_t(t) * triangleWords;
              double distance;
              if (triangle(localOrigin, localDirection, word(bottom, at), word(bottom, at + 1), word(bottom, at + 2),
                           found.t, distance))
                found = {distance, bottom[at * 4 + 3], bottom[(at + 1) * 4 + 3] & ~opaqueBit, instance[24]};
            }
          });
        }
      });
      const bool hit = expected.t < 1e30;
      hits += hit;
      const auto differs = [&](const Hit &reference) {
        const bool there = reference.t < 1e30;
        return there != (found.t < 1e30) ||
               (there && (std::abs(found.t - reference.t) > 1e-3 * std::max(1., reference.t) ||
                          ((found.primitive != reference.primitive || found.geometry != reference.geometry ||
                            found.customIndex != reference.customIndex) &&
                           std::abs(found.t - reference.t) > 1e-6)));
      };
      if (differs(expected)) {
        // A ray along a triangle's edge: the structures hold the triangle in
        // its mesh's space as single-precision edges, and may take the edge
        // either way. A few rays of the thousands do.
        if ((!differs(direct(1e-4)) || !differs(direct(-1e-4))) && ++grazing <= 8)
          continue;
        std::fprintf(stderr, "%s, ray %u: expected t %g (instance %u geometry %u triangle %u), found t %g (%u %u %u)\n",
                     what, ray, expected.t, expected.customIndex, expected.geometry, expected.primitive, found.t,
                     found.customIndex, found.geometry, found.primitive);
        return false;
      }
    }
    return true;
  };
  if (!agree("built"))
    return 1;
  // Geometry 0 is opaque, geometry 1 is not.
  for (const auto &mesh : meshes) {
    const uint32_t *words = mesh.structure.words.data();
    const size_t triangles = (mesh.structure.words.size() / 4 - words[1]) / triangleWords;
    for (size_t t = 0; t < triangles; ++t) {
      // An unreferenced slot of the spatial-split build is all zeros.
      const uint32_t *slot = words + (words[1] + t * triangleWords) * 4;
      if (std::all_of(slot, slot + triangleWords * 4, [](uint32_t word) { return word == 0; }))
        continue;
      const uint32_t source = words[(words[1] + t * triangleWords + 1) * 4 + 3];
      if (((source & opaqueBit) != 0) != ((source & ~opaqueBit) == 0)) {
        std::fprintf(stderr, "triangle %zu: opaque flag does not follow its geometry\n", t);
        return 1;
      }
    }
  }
  if (hits < 400) {
    std::fprintf(stderr, "only %u of 4000 rays hit: the test scene is not exercised\n", hits);
    return 1;
  }
  // Refits: a deformed mesh keeps its tree, as do instances that moved.
  {
    auto &mesh = meshes[2];
    Geometry parts[2];
    for (unsigned g = 0; g < 2; ++g)
      parts[g] = {mesh.positions[g].data(), mesh.indices[g].data(), uint32_t(mesh.indices[g].size() / 3), g == 0};
    BottomBuilder bottom;
    TopBuilder topBuilder;
    for (unsigned step = 0; step < 3; ++step) {
      for (auto &positions : mesh.positions)
        for (size_t i = 0; i < positions.size(); i += 3)
          positions[i + 1] += .3f * std::sin(positions[i] + float(step));
      mesh.structure = bottom.update(parts, 2, 1, 0);
      if (bottom.refitted() != (step > 0)) {
        std::fprintf(stderr, "step %u: the bottom level was %s\n", step, bottom.refitted() ? "refitted" : "built");
        return 1;
      }
      instances.clear();
      for (auto &entry : placed) {
        entry.instance.transform[0][3] += unit(random);
        entry.instance.transform[2][3] += unit(random);
        entry.instance.bottom = reinterpret_cast<uint64_t>(entry.mesh->structure.words.data());
        std::copy_n(entry.mesh->structure.low, 3, entry.instance.low);
        std::copy_n(entry.mesh->structure.high, 3, entry.instance.high);
        instances.push_back(entry.instance);
      }
      top = topBuilder.update(instances.data(), instances.size(), true);
      if (topBuilder.refitted() != (step > 0)) {
        std::fprintf(stderr, "step %u: the top level was %s\n", step, topBuilder.refitted() ? "refitted" : "built");
        return 1;
      }
      if (!agree(step ? "refitted" : "rebuilt"))
        return 1;
    }
    // Another topology builds again.
    mesh.structure = bottom.update(parts, 2, 2, 0);
    if (bottom.refitted()) {
      std::fprintf(stderr, "a changed topology was refitted\n");
      return 1;
    }
  }
  std::printf("%u rays hit over four scenes; all agree\n", hits);
  return 0;
}

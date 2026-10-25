#include "scene/light_bvh.h"
#include "scene/light_visibility.h"
#include "scene/resources.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <random>
#include <set>
#include <remix/remix_native_ext.h>

namespace {
void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}
} // namespace
int main() {
  using namespace remix::scene;
  // Sparse animation and input reordering must produce the same GPU records
  // as a fresh hierarchy, including when whole branches lose their power.
  {
    std::mt19937 random(2080);
    std::vector<SphereLight> lights(101);
    for (size_t i = 0; i < lights.size(); ++i) {
      auto &light = lights[i];
      light.id = i + 1;
      light.triangle = uint32_t(i % 3);
      light.radius = .1f + float(i % 7) * .05f;
      for (unsigned c = 0; c < 3; ++c) {
        light.position[c] = float(random() % 1000) / 10;
        light.radiance[c] = float(random() % 100);
        for (unsigned v = 0; v < 3; ++v)
          light.corners[v][c] = light.position[c] + (v == c ? .5f : 0);
      }
    }
    LightBvh cached;
    for (unsigned frame = 0; frame < 256; ++frame) {
      if (frame % 7 == 0)
        std::shuffle(lights.begin(), lights.end(), random);
      for (unsigned edit = 0; edit < 5; ++edit) {
        auto &light = lights[random() % lights.size()];
        for (auto &channel : light.radiance)
          channel = frame % 4 ? float(random() % 100) : 0;
      }
      if (frame == 80)
        for (auto &light : lights)
          std::fill_n(light.radiance, 3, 0.f);
      if (frame == 128)
        for (auto &light : lights) {
          light.radius *= 2;
          light.corners[1][0] += .75f;
        }
      cached.update(lights);
      LightBvh rebuilt;
      rebuilt.update(lights);
      require(cached.active == rebuilt.active && cached.nodes().size() == rebuilt.nodes().size(),
              "Sparse light refit changed hierarchy membership");
      require(std::memcmp(cached.nodes().data(), rebuilt.nodes().data(),
                          cached.nodes().size() * sizeof(LightNode)) == 0,
              "Sparse light refit changed bounds or sampling power");
      require(std::memcmp(cached.emitters().data(), rebuilt.emitters().data(),
                          cached.emitters().size() * sizeof(LightEmitter)) == 0,
              "Cached light ordering changed emitter records");
      const auto generation = cached.generation;
      std::shuffle(lights.begin(), lights.end(), random);
      require(!cached.update(lights) && cached.generation == generation,
              "Reordered identical lights invalidated the hierarchy");
    }
    require(cached.builds == 1 && cached.refits > 200,
            "Radiance animation unnecessarily rebuilt the hierarchy");
  }
  require(LightBvh::falloff(1.e12, 0) == 1 && LightBvh::falloff(144, 16) == 1 &&
              LightBvh::falloff(256, 16) == 0 && LightBvh::falloff(400, 16) == 0,
          "Influence bound changed inner light or failed to reach black");
  double previousFalloff = 1;
  for (unsigned i = 0; i <= 1000; ++i) {
    const double distance = 12 + i * .004;
    const double fade = LightBvh::falloff(distance * distance, 16);
    require(fade >= 0 && fade <= previousFalloff, "Influence fade is not monotonic");
    previousFalloff = fade;
  }
  require(1 - LightBvh::falloff(12.001 * 12.001, 16) < 1.e-6 &&
              LightBvh::falloff(15.999 * 15.999, 16) < 1.e-6,
          "Influence fade has an endpoint discontinuity");
  {
    SphereLight live{}, unlit{};
    live.id = 1; live.radius = .08f; live.radiance[0] = 1;
    unlit.id = 2; unlit.radius = .08f; unlit.position[0] = 100;
    LightBvh bounds;
    bounds.update({live, unlit});
    require(bounds.nodes()[0].maximum[0] < .1f,
            "Unlit slots expanded the live influence bound");
    unlit.radiance[0] = 1;
    bounds.update({live, unlit});
    require(bounds.builds == 1 && bounds.refits == 1 && bounds.nodes()[0].maximum[0] > 100,
            "Reactivated emitter did not restore its bound by refitting");
  }
  remix::NativeResources resources;
  remixapi_LightInfoSphereEXT sphere{};
  sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
  sphere.radius = .08f;
  remixapi_LightInfo info{};
  info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  info.pNext = &sphere;
  info.hash = 10;
  info.radiance = {200, 110, 44};
  remixapi_LightHandle handle{};
  resources.createLight(info, handle);
  resources.drawLight(handle);
  resources.drawLight(handle);
  auto accepted = resources.sphereLights();
  require(accepted.size() == 1, "Repeated draw duplicated emitter power");
  auto rejected = [&] {
    bool failed = false;
    try {
      resources.updateLight(handle, info);
    } catch (...) {
      failed = true;
    }
    require(failed && resources.sphereLights() == accepted,
            "Malformed sphere modified accepted state");
  };
  sphere.radius = -.1f;
  rejected();
  sphere.radius = std::numeric_limits<float>::infinity();
  rejected();
  sphere.radius = .08f;
  sphere.position.x = std::numeric_limits<float>::quiet_NaN();
  rejected();
  sphere.position.x = 0;
  info.radiance.y = -1;
  rejected();
  info.radiance.y = 110;
  sphere.shaping_hasvalue = 1;
  rejected();
  sphere.shaping_hasvalue = 0;
  sphere.radius = 0;
  resources.updateLight(handle, info);
  require(resources.sphereLights().empty(), "Zero-radius compatibility placeholder emitted light");
  sphere.radius = .08f;
  resources.updateLight(handle, info);
  require(resources.sphereLights() == accepted, "Finite-radius update did not restore the emitter");
  resources.finishFrame();
  require(resources.sphereLights().empty(),
          "Lights survived an absent submission");
  resources.drawLight(handle);
  resources.destroyLight(handle);
  require(resources.sphereLights().empty(),
          "Destroyed light survived draw selection");

  for (unsigned count : {1u, 100u, 1000u, 5000u}) {
    std::vector<SphereLight> lights;
    for (unsigned i = 0; i < count; ++i) {
      SphereLight light;
      light.id = i + 1;
      light.radius = .08f;
      light.position[0] = float(i % 71) - 35;
      light.position[1] = float(i / 71) - 35;
      light.position[2] = 4;
      light.radiance[0] = 1 + i % 13;
      lights.push_back(light);
    }
    LightBvh bvh;
    require(bvh.update(lights), "Initial BVH not built");
    require(bvh.active == count && bvh.nodes().size() == 2 * count - 1 &&
                bvh.depth <= unsigned(std::ceil(std::log2(count))) + 1,
            "Unbounded BVH depth or wrong membership");
    std::reverse(lights.begin(), lights.end());
    require(!bvh.update(lights) && bvh.builds == 1,
            "Unchanged reordered lights rebuilt BVH");
    const float p[] = {0, 0, 0}, n[] = {0, 0, 1};
    std::vector<double> pdf(count);
    std::function<void(uint32_t, double)> visit = [&](uint32_t node,
                                                      double probability) {
      const auto &value = bvh.nodes()[node];
      if (value.links[3] == 1) {
        pdf[value.links[2]] = probability;
        return;
      }
      const auto a = LightBvh::weight(bvh.nodes()[value.links[0]], p, n);
      const auto b = LightBvh::weight(bvh.nodes()[value.links[1]], p, n);
      const auto split = std::clamp(a / (a + b), 1.e-6, 1. - 1.e-6);
      visit(value.links[0], probability * split);
      visit(value.links[1], probability * (1 - split));
    };
    visit(0, 1);
    // Any leaf with positive physical attenuation must keep nonzero sampling
    // support at every enclosing node, including nodes near a range boundary.
    std::function<void(uint32_t, bool)> bounded = [&](uint32_t node, bool supported) {
      const auto &value = bvh.nodes()[node];
      supported &= LightBvh::weight(value, p, n, 16) > 0;
      if (value.links[3] == 1) {
        const auto &emitter = bvh.emitters()[value.links[2]];
        double d2 = 0;
        for (unsigned c = 0; c < 3; ++c)
          d2 += double(emitter.positionRadius[c]) * emitter.positionRadius[c];
        require(LightBvh::falloff(d2, 16) == 0 || supported,
                "Influence bounds culled a contributing emitter");
      } else {
        bounded(value.links[0], supported);
        bounded(value.links[1], supported);
      }
    };
    bounded(0, true);
    double sum = 0;
    for (unsigned i = 0; i < count; ++i) {
      require(pdf[i] > 0 && std::isfinite(pdf[i]),
              "A contributing light has zero selection PDF");
      sum += pdf[i];
    }
    require(std::abs(sum - 1) < 1.e-10, "BVH traversal did not reach every "
                                        "emitter with normalized probability");
    for (auto &light : lights)
      light.radiance[0] *= .5f;
    require(bvh.update(lights) && bvh.refits == 1 && bvh.builds == 1,
            "Power change rebuilt topology");
    for (auto &light : lights)
      light.position[0] += 128;
    require(bvh.update(lights) && bvh.refits == 2,
            "Origin shift did not refit bounds");
    for (auto &light : lights)
      light.radiance[0] = 0;
    bvh.update(lights);
    require(!bvh.active && bvh.nodes()[0].minimumPower[3] == 0,
            "Zero-power slots still contribute");
    bvh.update({});
    require(bvh.nodes().empty() && bvh.emitters().empty(),
            "Unloaded lights remained in hierarchy");
    std::printf("%u lights: balanced build, exact PDF sum, power/origin refit, "
                "removal passed\n",
                count);
  }
  // Animated emission updates transport every tick without continually
  // shortening global lighting history. Slow ramps compare to an anchor.
  {
    SphereLight triangle{};
    triangle.id = 1;
    triangle.triangle = 2;
    triangle.radius = 4;
    triangle.radiance[0] = 50;
    triangle.position[2] = -.1f;
    triangle.corners[0][0] = -2; triangle.corners[0][1] = -2;
    triangle.corners[1][0] = 2; triangle.corners[1][1] = -2;
    triangle.corners[2][1] = 2;
    for (auto &corner : triangle.corners) corner[2] = -.1f;
    LightBvh bvh;
    bvh.update({triangle});
    const auto stable = bvh.lightingGeneration;
    const auto visibility = bvh.visibilityGeneration;
    for (int frame = 0; frame < 100; ++frame) {
      triangle.radiance[0] = frame & 1 ? 50.5f : 49.5f;
      require(bvh.update({triangle}), "Animated radiance failed to update transport");
      require(bvh.emitters()[0].radiance[0] == triangle.radiance[0] && bvh.lightingGeneration == stable,
              "Small emission animation invalidated temporal lighting");
    }
    triangle.radiance[0] = 53;
    bvh.update({triangle});
    require(bvh.lightingGeneration == stable, "Small intensity step discarded history");
    triangle.radiance[0] = 56;
    bvh.update({triangle});
    require(bvh.lightingGeneration == stable, "Modest animated variation discarded history");
    triangle.radiance[0] = 68;
    bvh.update({triangle});
    require(bvh.lightingGeneration == stable + 1, "Gradual ramp never triggered lighting response");
    require(bvh.visibilityGeneration == visibility, "Color animation discarded visibility evidence");
    triangle.corners[0][2] += .01f;
    bvh.update({triangle});
    require(bvh.lightingGeneration == stable + 2, "Moving emitter retained stale temporal lighting");
    require(bvh.visibilityGeneration == visibility + 1, "Moving emitter retained visibility history");
    triangle.radiance[0] = 0;
    bvh.update({triangle});
    require(bvh.lightingGeneration == stable + 3, "Extinguished light retained history");
    bvh.update({});
    require(bvh.lightingGeneration == stable + 4, "Removed light retained temporal membership");
    SphereLight dominant = triangle, animated = triangle;
    dominant.id = 10; dominant.radiance[0] = 100;
    animated.id = 11; animated.radiance[0] = 1;
    bvh.update({dominant, animated});
    const auto aggregate = bvh.lightingGeneration;
    for (int frame = 0; frame < 20; ++frame) {
      animated.radiance[0] = frame & 1 ? 2.f : 1.f;
      bvh.update({dominant, animated});
      require(bvh.lightingGeneration == aggregate,
              "One animated emitter shortened the whole scene's history");
    }
  }
  // A receiver inside a volume of lights must not almost exclusively sample
  // whichever internal bounding box contains it. Measure the full discrete
  // estimator against the analytic sphere integral, before visibility or NRD.
  {
    std::vector<SphereLight> lights;
    for (int x = -5; x <= 5; ++x)
      for (int y = -5; y <= 5; ++y)
        for (int z = -5; z <= 5; ++z) {
          SphereLight light{};
          light.id = lights.size() + 1;
          light.radius = .08f;
          light.position[0] = 2 * x + .5f;
          light.position[1] = 2 * y + .75f;
          light.position[2] = 2 * z + .25f;
          light.radiance[0] = 200;
          light.radiance[1] = 110;
          light.radiance[2] = 44;
          lights.push_back(light);
        }
    LightBvh bvh;
    bvh.update(lights);
    for (float offset : {0.f, .5f, 1.f, 2.f}) {
      const float p[] = {offset, 0, 0}, n[] = {0, 0, 1};
      std::vector<double> pdf(lights.size());
      std::function<void(uint32_t, double)> visit = [&](uint32_t i,
                                                        double probability) {
        const auto &node = bvh.nodes()[i];
        if (node.links[3] == 1) {
          pdf[node.links[2]] = probability;
          return;
        }
        const double a = LightBvh::weight(bvh.nodes()[node.links[0]], p, n, 16);
        const double b = LightBvh::weight(bvh.nodes()[node.links[1]], p, n, 16);
        const double split = a == 0 ? 0
                             : b == 0
                                 ? 1
                                 : std::clamp(a / (a + b), 1.e-6, 1. - 1.e-6);
        visit(node.links[0], probability * split);
        visit(node.links[1], probability * (1 - split));
      };
      visit(0, 1);
      double reference = 0, expectation = 0, secondMoment = 0, maximum = 0,
             totalPdf = 0;
      for (size_t i = 0; i < lights.size(); ++i) {
        const auto &light = lights[i];
        double distance2 = 0;
        for (unsigned c = 0; c < 3; ++c)
          distance2 += std::pow(double(light.position[c]) - p[c], 2);
        // These spheres are entirely above or below the receiving plane.
        const double contribution =
            (.2126 * 200 + .7152 * 110 + .0722 * 44) * double(light.radius) *
            light.radius * std::max(light.position[2], 0.f) /
            std::pow(distance2, 1.5) * LightBvh::falloff(distance2, 16);
        totalPdf += pdf[i];
        reference += contribution;
        if (contribution == 0)
          continue;
        require(pdf[i] > 0, "Interior receiver lost a contributing light");
        const double estimate = contribution / pdf[i];
        expectation += pdf[i] * estimate;
        secondMoment += pdf[i] * estimate * estimate;
        maximum = std::max(maximum, estimate);
      }
      require(std::abs(totalPdf - 1) < 1.e-10 &&
                  std::abs(expectation - reference) < 1.e-10,
              "Interior light selection changed the sphere integral");
      const double relativeDeviation =
          std::sqrt(secondMoment - reference * reference) / reference;
      require(relativeDeviation < 4 && maximum / reference < 80,
              "Interior light cluster produced excessive inverse-PDF outliers");
    }
    std::puts(
        "1331 torches: interior receiver energy and selection variance passed");
  }
  {
    VisibilityPartition p;
    p.closed = true; p.clusterCount = 1; p.leafClusters = {0, -1};
    VisibilityPartition::Node a{}, b{};
    a.plane[0] = b.plane[0] = 1; a.plane[3] = 2; b.plane[3] = 3;
    a.children[0] = 1; a.children[1] = -1;
    b.children[0] = -1; b.children[1] = -2;
    p.nodes = {a, b};
    const float start[3] = {0,0,0}, end[3] = {4,0,0}, clear[3] = {1,0,0}, solid[3] = {2.5f,0,0};
    using State = VisibilityPartition::SegmentState;
    require(p.firstSolid(start, end).state == State::Solid && p.firstSolid(start, end).fraction == .5,
            "Segment traversal finds a thin solid even when both endpoints are playable");
    require(p.firstSolid(end, start).fraction == .25 && p.firstSolid(solid, end).fraction == 0 &&
                p.firstSolid(start, clear).state == State::Clear,
            "Segment traversal preserves direction, solid starts and entirely free intervals");
    require(p.firstSolid(start, end, 1).state == State::Unknown,
            "Insufficient traversal budget cannot claim a clear segment");
    p.nodes[0].children[1] = 0;
    require(p.firstSolid(start, clear).state == State::Unknown, "Cyclic partitions exhaust their bounded walk");
    p.nodes[0].children[1] = 9999;
    require(p.firstSolid(start, clear).state == State::Unknown, "Malformed children cannot prove free space");
    p.nodes[0].plane[0] = 0;
    require(p.firstSolid(start, end).state == State::Unknown, "A zero-normal plane supplies no segment proof");
    p.closed = false;
    require(p.firstSolid(start, end).state == State::Unknown, "Open partitions cannot prove solid containment");
  }
  // Lights that come and go, and the emitters of dynamic instances, live in
  // the hierarchy's dynamic part: the static part keeps its indices (no
  // build), and the visibility table, which covers the static nodes only,
  // stays as it is.
  {
    std::mt19937 random(1531);
    auto partition = std::make_shared<VisibilityPartition>();
    // A 4x4 grid of cells in x/y (0..40), one cluster each; a cell sees its
    // column (and, not symmetrically, its row to the right).
    std::function<int32_t(int, int, int, int)> split = [&](int x0, int x1, int y0, int y1) -> int32_t {
      if (x1 - x0 == 1 && y1 - y0 == 1) {
        partition->leafClusters.push_back(int32_t(partition->leafClusters.size()));
        return -int32_t(partition->leafClusters.size());
      }
      const bool alongX = x1 - x0 >= y1 - y0;
      const int middle = alongX ? (x0 + x1) / 2 : (y0 + y1) / 2;
      const int32_t index = int32_t(partition->nodes.size());
      partition->nodes.emplace_back();
      // In front of the plane: the upper half.
      const int32_t front = alongX ? split(middle, x1, y0, y1) : split(x0, x1, middle, y1);
      const int32_t back = alongX ? split(x0, middle, y0, y1) : split(x0, x1, y0, middle);
      VisibilityPartition::Node n{};
      n.plane[alongX ? 0 : 1] = 1;
      n.plane[3] = middle * 10.f;
      n.children[0] = front;
      n.children[1] = back;
      partition->nodes[index] = n;
      return index;
    };
    split(0, 4, 0, 4);
    partition->clusterCount = 16;
    partition->rowBytes = 2;
    partition->visibility.assign(32, 0);
    const auto cell = [&](int cluster) {
      // Where the tree puts the cluster: probe its cells.
      for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 4; ++y) {
          const float p[3] = {x * 10 + 5.f, y * 10 + 5.f, 0};
          if (partition->cluster(p) == cluster)
            return std::pair{x, y};
        }
      return std::pair{-1, -1};
    };
    for (int a = 0; a < 16; ++a)
      for (int b = 0; b < 16; ++b) {
        const auto [ax, ay] = cell(a);
        const auto [bx, by] = cell(b);
        require(ax >= 0 && bx >= 0, "Test partition does not cover its grid");
        if (ax == bx || (ay == by && bx > ax))
          partition->visibility[a * 2 + b / 8] |= uint8_t(1u << (b % 8));
      }
    partition->version = 1;
    std::vector<SphereLight> lamps;
    for (unsigned i = 0; i < 40; ++i) {
      SphereLight light;
      light.id = 100 + i;
      light.radius = .1f;
      light.position[0] = float(random() % 400) / 10;
      light.position[1] = float(random() % 400) / 10;
      light.radiance[0] = 1 + float(i % 5);
      lamps.push_back(light);
    }
    const auto prop = [&](uint64_t instance, float x, unsigned count) {
      std::vector<SphereLight> lights;
      for (unsigned i = 0; i < count; ++i) {
        SphereLight light;
        light.id = instance * 1000 + i;
        light.triangle = 1;
        light.dynamic = true;
        light.radiance[1] = 2;
        for (unsigned v = 0; v < 3; ++v) {
          light.corners[v][0] = x + float(i % 5) + (v == 1);
          light.corners[v][1] = 20 + float(i / 5) + (v == 2);
          for (unsigned c = 0; c < 3; ++c)
            light.position[c] += light.corners[v][c] / 3;
        }
        light.radius = 1;
        lights.push_back(light);
      }
      return lights;
    };
    LightBvh bvh;
    LightVisibility visibility;
    std::vector<LightNode> staticNodes;
    // The table against every (cluster, node) pair computed from the lights.
    const auto checkTable = [&](const char *message) {
      const auto &nodes = bvh.nodes();
      const auto &table = *visibility.table();
      require(visibility.tableNodes() == bvh.staticNodes() &&
                  table.size() == 16 * size_t(visibility.rowWords()),
              "Visibility table does not cover the static nodes");
      std::vector<uint8_t> bits(nodes.size());
      for (int from = 0; from < 16; ++from) {
        for (size_t n = nodes.size(); n-- > 0;) {
          const auto &links = nodes[n].links;
          if (links[3] == 1) {
            const auto &light = bvh.lights()[links[2]];
            const int cluster = partition->cluster(light.position);
            bits[n] = links[2] >= bvh.staticEmitters() || cluster < 0 || partition->visible(from, cluster);
          } else {
            bits[n] = bits[links[0]] | bits[links[1]];
          }
        }
        for (uint32_t n = 0; n < visibility.tableNodes(); ++n)
          require(((table[from * visibility.rowWords() + n / 32] >> (n % 32)) & 1) == bits[n], message);
      }
    };
    const auto update = [&](std::vector<SphereLight> lights) {
      std::shuffle(lights.begin(), lights.end(), random);
      const bool changed = bvh.update(lights);
      visibility.update(partition, bvh, true, .1f);
      const auto &nodes = bvh.nodes();
      require(nodes.empty() || (nodes.size() == 2 * bvh.emitters().size() - 1 &&
                                nodes[0].links[3] == bvh.emitters().size()),
              "Two-part hierarchy is not a full binary tree");
      uint32_t lit = 0;
      for (const auto &light : lights)
        lit += light.radiance[0] > 0 || light.radiance[1] > 0 || light.radiance[2] > 0;
      require(bvh.active == lit, "Hierarchy lights differ from the submitted ones");
      return changed;
    };
    auto frame = lamps;
    update(frame);
    require(bvh.builds == 1 && bvh.staticNodes() == bvh.nodes().size() && bvh.dynamicLights == 0,
            "Static lights alone need no dynamic part");
    checkTable("Initial visibility table differs from the partition");
    // The first emitters of a dynamic instance add the dynamic part: once.
    for (unsigned step = 0; step < 300; ++step) {
      frame = lamps;
      // The prop moves every frame, changes identity every 7 frames, is away
      // for 5 of every 40 frames and has a second copy for some.
      if (step % 40 < 35) {
        const auto first = prop(1 + step / 7, float(step % 30), 12);
        frame.insert(frame.end(), first.begin(), first.end());
        if (step % 3 == 0) {
          const auto second = prop(900, 30 - float(step % 30), 7);
          frame.insert(frame.end(), second.begin(), second.end());
        }
      }
      const uint64_t tables = visibility.version();
      update(frame);
      if (step == 0) {
        require(bvh.builds == 2 && bvh.lastBuildReason == LightBvh::FirstDynamic,
                "The dynamic part was not added by one build");
        staticNodes.assign(bvh.nodes().begin() + 3, bvh.nodes().begin() + bvh.staticNodes());
        checkTable("Visibility table differs after the dynamic part was added");
      } else {
        require(bvh.builds == 2, "Dynamic-instance lights rebuilt the hierarchy");
        require(visibility.version() == tables, "Dynamic-instance lights remade the visibility table");
        require(bvh.staticNodes() == staticNodes.size() + 3 &&
                    std::memcmp(staticNodes.data(), bvh.nodes().data() + 3,
                                staticNodes.size() * sizeof(LightNode)) == 0,
                "Dynamic-instance lights changed static nodes");
      }
      require(bvh.staticLights == lamps.size() && bvh.instanceLights == frame.size() - lamps.size() &&
                  bvh.dynamicLights == bvh.instanceLights,
              "Dynamic-instance lights are not all in the dynamic part");
      const float p[] = {20, 20, 5}, n[] = {0, 0, -1};
      double total = 0;
      std::function<void(uint32_t, double)> visit = [&](uint32_t node, double probability) {
        const auto &value = bvh.nodes()[node];
        if (value.links[3] == 1) {
          total += probability;
          return;
        }
        const double a = LightBvh::weight(bvh.nodes()[value.links[0]], p, n);
        const double b = LightBvh::weight(bvh.nodes()[value.links[1]], p, n);
        if (a + b <= 0)
          return;
        visit(value.links[0], probability * a / (a + b));
        visit(value.links[1], probability * b / (a + b));
      };
      visit(0, 1);
      require(std::abs(total - 1) < 1.e-10, "Two-part traversal lost selection probability");
    }
    require(bvh.dynamicBuilds > 40 && bvh.refits >= 250, "Dynamic part was not rebuilt on its own");
    checkTable("Visibility table differs after dynamic lights came and went");
    // A static light that leaves keeps its slot and returns to it.
    frame = lamps;
    frame.erase(frame.begin() + 7);
    update(frame);
    require(bvh.builds == 2 && bvh.retiredSlots == 1 && bvh.active == lamps.size() - 1,
            "A static light that left rebuilt the hierarchy or kept its power");
    update(lamps);
    require(bvh.builds == 2 && bvh.retiredSlots == 0 && bvh.active == lamps.size() &&
                std::memcmp(staticNodes.data(), bvh.nodes().data() + 3, staticNodes.size() * sizeof(LightNode)) == 0,
            "A returning static light did not take its slot");
    // A new light of no dynamic instance waits in the dynamic part and joins
    // the static part once at rest; a moving one stays.
    SphereLight added = lamps[0], carried = lamps[1];
    added.id = 5000;
    added.position[0] = 33;
    carried.id = 5001;
    const uint64_t refits = bvh.refits;
    // (While another new light keeps moving, the one at rest waits four times as long.)
    for (unsigned step = 0; step < 5 * LightBvh::kSettle && bvh.builds == 2; ++step) {
      frame = lamps;
      frame.push_back(added);
      carried.position[1] = float(step % 40);
      frame.push_back(carried);
      update(frame);
      require(bvh.builds > 2 || bvh.dynamicLights == 2, "New lights did not wait in the dynamic part");
    }
    require(bvh.builds == 3 && bvh.lastBuildReason == LightBvh::Settled && bvh.staticLights == lamps.size() + 1 &&
                bvh.dynamicLights == 1 && bvh.refits - refits >= LightBvh::kSettle,
            "A light at rest did not join the static part by one build");
    checkTable("Visibility table differs after a light joined the static part");
    // A static light that moves is visible from everywhere from then on.
    frame.back() = carried;
    frame[3].position[0] += 15;
    const uint64_t tables = visibility.version();
    update(frame);
    require(bvh.builds == 3 && visibility.version() == tables + 1 && visibility.unrestricted >= 2,
            "A moved static light kept its clusters");
    update({});
    require(bvh.nodes().empty() && bvh.lastBuildReason == LightBvh::Cleared && !visibility.table(),
            "Unloaded lights remained in the two-part hierarchy");
    std::puts("Dynamic part: static indices, visibility table, slots and settling passed");
  }
  {
    // Lights that leave the host's view (its culling) keep their slots for
    // kRetain updates, through builds, and return to them; gone longer, a
    // build drops them.
    std::mt19937 random(1531);

    std::vector<SphereLight> all;
    for (unsigned i = 0; i < 400; ++i) {
      SphereLight light;
      light.id = 10000 + i;
      light.radius = .1f;
      light.position[0] = float(random() % 400) / 10;
      light.position[1] = float(random() % 400) / 10;
      light.radiance[0] = 1;
      all.push_back(light);
    }
    LightBvh bvh;
    bvh.update(all);
    const std::vector<SphereLight> few(all.begin(), all.begin() + 100);
    for (uint64_t step = 0; step < 3 * LightBvh::kBuildInterval; ++step)
      bvh.update(few);
    require(bvh.builds == 1 && bvh.retiredSlots == 300 && bvh.active == 100,
            "Lights out of view for a while rebuilt the hierarchy or lost their power");
    auto withNew = few;
    SphereLight added = all[0];
    added.id = 99999;
    added.position[0] = 55;
    withNew.push_back(added);
    // The first new light makes the dynamic part (a build); at rest it joins
    // the static part (another).
    bvh.update(withNew);
    require(bvh.builds == 2 && bvh.lastBuildReason == LightBvh::FirstDynamic && bvh.retiredSlots == 300,
            "A build dropped the slots of lights out of view for a while");
    for (uint64_t step = 0; step < 3 * LightBvh::kBuildInterval && bvh.builds == 2; ++step)
      bvh.update(withNew);
    require(bvh.builds == 3 && bvh.lastBuildReason == LightBvh::Settled && bvh.retiredSlots == 300 &&
                bvh.dynamicLights == 0,
            "A settling build dropped the slots of lights out of view for a while");
    auto back = all;
    back.push_back(added);
    bvh.update(back);
    require(bvh.builds == 3 && bvh.retiredSlots == 0 && bvh.dynamicLights == 0 && bvh.active == 401,
            "Lights back in view did not return to their slots");
    for (uint64_t step = 0; step < LightBvh::kRetain + LightBvh::kBuildInterval + 1 &&
                            bvh.lastBuildReason != LightBvh::Retired;
         ++step)
      bvh.update(withNew);
    require(bvh.lastBuildReason == LightBvh::Retired && bvh.retiredSlots == 0 && bvh.staticLights == 101,
            "Lights gone for good kept their slots");
    // Input order does not matter: the same lights reversed are unchanged.
    std::vector<SphereLight> reversed(withNew.rbegin(), withNew.rend());
    const uint64_t generation = bvh.generation;
    require(!bvh.update(reversed) && bvh.generation == generation, "Reordered input changed the hierarchy");
    std::puts("Retained slots: lights out of view return to them, gone ones are dropped passed");
  }
  {
    // Props of dynamic instances that leave the host's view and come back
    // keep their dynamic slots (unlit) instead of rebuilding the dynamic
    // part; slots gone kRetain updates go with the next rebuild.
    const auto prop = [](uint64_t instance, float x) {
      std::vector<SphereLight> lights;
      for (unsigned i = 0; i < 20; ++i) {
        SphereLight light;
        light.id = instance * 1000 + i;
        light.group = instance * 2 + 1;
        light.rank = i;
        light.triangle = 1;
        light.dynamic = true;
        light.radiance[0] = 1;
        for (unsigned v = 0; v < 3; ++v) {
          light.corners[v][0] = x + float(i % 5) + (v == 1);
          light.corners[v][1] = float(i / 5) + (v == 2);
          for (unsigned c = 0; c < 3; ++c)
            light.position[c] += light.corners[v][c] / 3;
        }
        light.radius = 1;
        lights.push_back(light);
      }
      return lights;
    };
    const auto join = [](std::initializer_list<std::vector<SphereLight>> parts) {
      std::vector<SphereLight> all;
      for (const auto &part : parts)
        all.insert(all.end(), part.begin(), part.end());
      return all;
    };
    SphereLight lamp;
    lamp.id = 1;
    lamp.radius = .1f;
    lamp.radiance[0] = 1;
    const auto a = prop(1, 0), b = prop(2, 10), c = prop(3, 20), d = prop(4, 30);
    LightBvh bvh;
    bvh.setClusterSize(4);
    bvh.update(join({{lamp}, a, b}));
    bvh.update(join({{lamp}, a, b}));
    const uint64_t builds = bvh.builds, dynamicBuilds = bvh.dynamicBuilds;
    for (unsigned step = 0; step < 200; ++step) {
      const bool away = step % 3 != 0;
      bvh.update(away ? join({{lamp}, a}) : join({{lamp}, a, b}));
      require(bvh.unlitDynamicSlots == (away ? b.size() : 0) && bvh.dynamicLights == (away ? 20u : 40u) &&
                  bvh.active == (away ? 21u : 41u),
              "A prop out of view did not keep its slots unlit");
    }
    require(bvh.builds == builds && bvh.dynamicBuilds == dynamicBuilds,
            "A prop leaving and entering the view rebuilt the dynamic part");
    // A new prop rebuilds it once, keeping the slots of the one away.
    bvh.update(join({{lamp}, a, c}));
    bvh.update(join({{lamp}, a, c}));
    require(bvh.dynamicBuilds == dynamicBuilds + 1 && bvh.unlitDynamicSlots == b.size(),
            "A new prop did not rebuild the dynamic part once, keeping slots of one away");
    // Gone kRetain updates, its slots go with the next rebuild.
    for (uint64_t step = 0; step < LightBvh::kRetain; ++step)
      bvh.update(step % 2 ? join({{lamp}, a, c}) : join({{lamp}, c, a}));
    require(bvh.unlitDynamicSlots == b.size(), "Stale slots were dropped without a rebuild");
    bvh.update(join({{lamp}, a, c, d}));
    require(bvh.unlitDynamicSlots == 0 && bvh.dynamicLights == 60 && bvh.active == 61,
            "A rebuild kept the slots of a prop gone for good");
    std::puts("Dynamic slots: props out of view keep them, gone ones go with a rebuild passed");
  }
  {
    // Upload deltas: an older copy of the emitters and nodes, patched with
    // the ranges changedSince reports, equals the current ones; a build, or
    // another hierarchy, needs a whole copy.
    std::mt19937 random(77);
    std::vector<SphereLight> lights;
    for (unsigned i = 0; i < 3000; ++i) {
      SphereLight light;
      light.id = 1 + i;
      light.group = 1 + 2 * (i / 30);
      light.rank = i % 30;
      light.triangle = 1;
      light.dynamic = i >= 1500;
      light.radiance[0] = 1 + float(i % 3);
      for (unsigned v = 0; v < 3; ++v) {
        light.corners[v][0] = float(i % 100) + (v == 1);
        light.corners[v][1] = float(i / 100) + (v == 2);
        for (unsigned c = 0; c < 3; ++c)
          light.position[c] += light.corners[v][c] / 3;
      }
      light.radius = 1;
      lights.push_back(light);
    }
    LightBvh bvh;
    bvh.setClusterSize(8);
    bvh.update(lights);
    // Dynamic lights out of view keep their slots; settle the first builds.
    for (unsigned step = 0; step < 2 * LightBvh::kBuildInterval; ++step)
      bvh.update(lights);
    LightBvh::Ranges emitterRanges, nodeRanges;
    const auto copies = [&] { return std::pair{bvh.emitters(), bvh.nodes()}; };
    for (unsigned round = 0; round < 20; ++round) {
      const uint64_t since = bvh.generation;
      auto [emitters, nodes] = copies();
      for (unsigned step = 0; step < 1 + round % 4; ++step) {
        auto frame = lights;
        // A few static lights dim; dynamic props of 30 lights flicker out of
        // view (a seventh of them, and five at random).
        for (unsigned d = 0; d < 3; ++d)
          frame[random() % 1500].radiance[1] = float(round + 1);
        std::set<uint64_t> away;
        for (unsigned p = 0; p < 5; ++p)
          away.insert(50 + random() % 50);
        std::erase_if(frame, [&](const SphereLight &light) {
          const uint64_t prop = (light.id - 1) / 30;
          return light.dynamic && (away.count(prop) || prop % 7 == round % 7);
        });
        bvh.update(frame);
      }
      require(bvh.changedSince(bvh.instance(), since, emitterRanges, nodeRanges),
              "Refits asked for a whole upload");
      require(emitters.size() == bvh.emitters().size() && nodes.size() == bvh.nodes().size(),
              "Refits changed the hierarchy's size");
      size_t patched = 0;
      for (const auto &[first, end] : emitterRanges) {
        std::copy(bvh.emitters().begin() + first, bvh.emitters().begin() + end, emitters.begin() + first);
        patched += end - first;
      }
      for (const auto &[first, end] : nodeRanges)
        std::copy(bvh.nodes().begin() + first, bvh.nodes().begin() + end, nodes.begin() + first);
      require(std::memcmp(emitters.data(), bvh.emitters().data(), emitters.size() * sizeof(LightEmitter)) == 0 &&
                  std::memcmp(nodes.data(), bvh.nodes().data(), nodes.size() * sizeof(LightNode)) == 0,
              "Patched copy differs from the hierarchy");
      require(patched < emitters.size() / 2, "Upload deltas cover most emitters");
    }
    require(bvh.changedSince(bvh.instance(), bvh.generation, emitterRanges, nodeRanges) && emitterRanges.empty() &&
                nodeRanges.empty(),
            "An up-to-date copy has changes");
    LightBvh other;
    other.update(lights);
    require(!bvh.changedSince(other.instance(), bvh.generation, emitterRanges, nodeRanges),
            "Another hierarchy's copy was patched");
    const uint64_t before = bvh.generation;
    auto grown = lights;
    SphereLight added = lights[0];
    added.id = 99999;
    added.group = 99999;
    grown.push_back(added);
    bvh.update(grown); // a new dynamic-part light: the dynamic part is built again
    require(!bvh.changedSince(bvh.instance(), before, emitterRanges, nodeRanges), "A build was patched");
    std::puts("Upload deltas: patched copies match, builds and other hierarchies copy whole passed");
  }
  {
    // Leaves of up to N emitters of one surface: runs of a group in rank
    // order, power shares that sum to one, refits that keep them, and a
    // cluster size of one that is a leaf per emitter.
    std::vector<SphereLight> lights;
    const unsigned sizes[] = {100, 70, 30};
    for (unsigned g = 0; g < 3; ++g)
      for (unsigned i = 0; i < sizes[g]; ++i) {
        SphereLight light;
        light.id = 20000 + g * 1000 + i;
        light.triangle = 1;
        light.group = 7 + g;
        light.rank = (i * 37) % sizes[g];
        light.radiance[0] = 1 + float(i % 3);
        for (unsigned v = 0; v < 3; ++v) {
          light.corners[v][0] = float(g * 50) + float(light.rank) * .5f + (v == 1);
          light.corners[v][1] = (v == 2);
          for (unsigned c = 0; c < 3; ++c)
            light.position[c] += light.corners[v][c] / 3;
        }
        light.radius = 1;
        lights.push_back(light);
      }
    for (unsigned i = 0; i < 5; ++i) {
      SphereLight light;
      light.id = 30000 + i;
      light.radius = .1f;
      light.position[2] = float(i);
      light.radiance[1] = 1;
      lights.push_back(light);
    }
    std::reverse(lights.begin(), lights.end()); // input order does not matter
    LightBvh bvh;
    bvh.setClusterSize(16);
    require(bvh.update(lights), "Clustered hierarchy not built");
    require(bvh.leafCount() == 5 + 7 + 5 + 2 && bvh.nodes().size() == 2 * bvh.leafCount() - 1,
            "Groups were not cut into leaves of at most sixteen");
    const auto checkLeaves = [&](const char *message) {
      size_t covered = 0;
      for (const auto &node : bvh.nodes()) {
        if (node.links[3] != 1)
          continue;
        const uint32_t first = node.links[2], count = std::max(node.links[1], 1u);
        double sum = 0, power = 0;
        for (uint32_t e = first; e < first + count; ++e) {
          const auto &light = bvh.lights()[e];
          sum += bvh.emitters()[e].shaping[3];
          power += light.radiance[0] + light.radiance[1];
          if (e > first)
            require(light.group == bvh.lights()[first].group && light.group && light.rank > bvh.lights()[e - 1].rank,
                    message);
        }
        require(count <= 16 && std::abs(sum - 1) < 1e-5 && bvh.emitters()[first + count - 1].shaping[2] == 1 &&
                    (power > 0) == (node.minimumPower[3] > 0),
                message);
        covered += count;
      }
      require(covered == bvh.emitters().size(), message);
    };
    checkLeaves("A leaf mixed surfaces, lost rank order or its power shares");
    // A brighter triangle takes a larger share; the refit keeps the leaves.
    auto brighter = lights;
    for (auto &light : brighter)
      if (light.id == 20005)
        light.radiance[0] = 50;
    const uint64_t builds = bvh.builds;
    require(bvh.update(brighter) && bvh.builds == builds, "A brighter triangle rebuilt the hierarchy");
    checkLeaves("A refit lost a leaf's power shares");
    for (size_t e = 0; e < bvh.lights().size(); ++e)
      if (bvh.lights()[e].id == 20005)
        require(bvh.emitters()[e].shaping[3] > .5f, "A brighter triangle kept its share");
    bvh.setClusterSize(1);
    require(bvh.update(brighter) && bvh.builds == builds + 1 && bvh.leafCount() == brighter.size(),
            "Cluster size one did not make a leaf per emitter");
    checkLeaves("Single-emitter leaves lost their shares");
    std::puts("Clusters: surface runs in rank order, power shares, refits and size one passed");
  }
}

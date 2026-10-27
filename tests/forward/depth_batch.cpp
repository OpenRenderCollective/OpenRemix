#include "forward/depth_batch/batch.h"
#include "forward/options.h"
#include "gpu_fixture.h"
#include <array>
#include <bit>
#include <cstring>
#include <vector>

using namespace remix::forward;
using namespace remix::forward::test;
namespace {
  struct Mesh {
    Buffer vertices, indices;
    std::vector<std::array<float, 4>> positions;
    explicit Mesh(Fixture &fixture, uint32_t stride, std::span<const uint32_t> input) {
      std::vector<std::byte> packed(5 * stride, std::byte{0x7f});
      for (uint32_t i = 0; i < 5; ++i) {
        positions.push_back({float(i), i == 2 ? -0.f : float(i) * .25f, float(i) * -.5f, 0});
        std::memcpy(packed.data() + i * stride, positions.back().data(), 12);
      }
      vertices.create(fixture.physical, fixture.device, packed.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
      indices.create(fixture.physical, fixture.device, input.size_bytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
      vertices.write(packed.data(), packed.size());
      indices.write(input.data(), input.size_bytes());
    }
  };
}
int main() try {
  Options defaults, ordinary;
  ordinary.staticDepthBatch = false;
  require(defaults.staticDepthBatch && !defaults.sameTargets(ordinary),
          "Disabling default batching recreates the matching pipeline targets");
  Fixture fixture(false, true);
  const uint32_t aIndices[]{99, 0, 2, 1, 2, 3, 1, 99}, bIndices[]{2, 1, 0};
  auto a = std::make_shared<Mesh>(fixture, 48, aIndices);
  auto b = std::make_shared<Mesh>(fixture, 112, bIndices);
  std::array<depth_batch::Source, 2> sources{{
      {&a->vertices, &a->indices, 48, 1, 6, 1, 11, true, a},
      {&b->vertices, &b->indices, 112, 0, 3, 0, 25, false, b}}};
  depth_batch::Batch batch(fixture.physical, fixture.device);
  require(batch.prepare(sources) && batch.indexCount() == 6 && batch.visibleSources() == 1,
          "Invisible source indices are excluded without discarding cached geometry");
  const auto positions = static_cast<const std::array<float, 4> *>(batch.positions().mapped);
  for (uint32_t i = 0; i < 4; ++i)
    require(std::memcmp(positions[i].data(), a->positions[i + 1].data(), 12) == 0,
            "Batch preserves native local position bits, stride and signed base vertex");
  const uint32_t expectedA[]{0, 2, 1, 2, 3, 1};
  require(std::memcmp(batch.indices().mapped, expectedA, sizeof(expectedA)) == 0,
          "Batch preserves primitive order and vertex reuse");
  const VkBuffer originalPositions = batch.positions().handle;
  sources[0].visible = false;
  sources[1].visible = true;
  sources[1].materialIndex = 31;
  require(batch.prepare(sources) && batch.positions().handle == originalPositions && batch.indexCount() == 3,
          "Camera and per-frame material-table changes reuse immutable geometry");
  const uint32_t expectedB[]{6, 5, 4};
  require(std::memcmp(batch.indices().mapped, expectedB, sizeof(expectedB)) == 0,
          "Visible index ranges retain the second source's winding");
  const auto materials = static_cast<const uint32_t *>(batch.materials().mapped);
  require(materials[4] == 31 && materials[5] == 31 && materials[6] == 31,
          "Every vertex receives the current frame's material-table index");
  require(batch.prepare(sources) && batch.indexCount() == 3 && batch.visibleSources() == 1,
          "An unchanged fenced slot retains its uploaded draw counts");
  sources[1].visible = false;
  require(batch.prepare(sources) && batch.indexCount() == 0 && batch.visibleSources() == 0 &&
              batch.prepare(sources) && batch.indexCount() == 0,
          "A cached empty visibility set cannot replay stale triangles");
  sources[1].visible = true;
  sources[1].materialIndex = 32;
  require(batch.prepare(sources) && batch.indexCount() == 3 && materials[4] == 32,
          "Visibility restoration refreshes the material stream without rebuilding geometry");
  depth_batch::Batch limited(fixture.physical, fixture.device, 32);
  require(!limited.prepare(sources) && limited.indexCount() == 0 && limited.bytes() == 0,
          "A bounded batch declines entirely before truncating geometry");
  auto malformed = sources;
  malformed[0].vertexOffset = -9;
  require(!batch.prepare(malformed) && batch.indexCount() == 0 && batch.bytes() == 0,
          "Invalid signed offsets retain the ordinary draw fallback");
  malformed = sources;
  malformed[0].indexCount = 5;
  require(!batch.prepare(malformed), "Partial triangle lists cannot create cross-source triangles");
  require(batch.prepare(sources), "A valid source set recovers after rejection");
  limited.prepare({});
  std::weak_ptr<Mesh> retained = a;
  sources = {};
  malformed = {};
  a.reset(); b.reset();
  require(!retained.expired(), "Cached immutable geometry retains its source owners");
  require(!batch.prepare({}) && batch.bytes() == 0 && retained.expired(),
          "An empty source set releases old map owners after the slot fence");
  require(validationErrors == 0, "Depth-batch buffers are validation clean");
  std::puts("Depth batch: exact positions, winding, culling, frame indices, budgets and ownership passed");
} catch (const std::exception &error) {
  std::fprintf(stderr, "Depth batch fixture: %s\n", error.what());
  return 1;
} catch (VkResult error) {
  std::fprintf(stderr, "Depth batch Vulkan: %d\n", int(error));
  return 1;
}

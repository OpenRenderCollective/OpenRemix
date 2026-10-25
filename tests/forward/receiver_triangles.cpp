#include "forward/lightmap/receiver_triangles.h"
#include "forward/lightmap/geometry.h"
#include "gpu_fixture.h"
#include <limits>

using namespace remix;
using namespace remix::forward;
using namespace remix::forward::lightmap;
using remix::forward::test::require;
namespace {
  struct Inputs {
    std::shared_ptr<scene::BakeScene> source = std::make_shared<scene::BakeScene>();
    scene::Draw live;
    Mesh mapped;
    Inputs() {
      auto surface = std::make_shared<scene::Surface>();
      for (const Vec3 p : {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0}, Vec3{0, 1, 0}}) {
        scene::Vertex vertex{};
        std::copy(p.begin(), p.end(), vertex.position);
        vertex.normal[2] = 1;
        surface->vertices.push_back(vertex);
      }
      surface->indices = {0, 1, 2, 0, 2, 3};
      scene::Draw captured;
      captured.surface = surface;
      captured.material = std::make_shared<scene::Material>();
      captured.instanceId = 7;
      captured.classification = REMIXAPI_INSTANCE_STATIC_EXT;
      source->ready = true;
      source->receiverDraws = {1};
      source->draws = {captured};
      source->bindings = {{surface, captured.material}};
      source->charts = {{0, 0, 2, 2, {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}}}};
      for (const auto &vertex : surface->vertices) {
        Vertex mappedVertex{};
        mappedVertex.surface = vertex;
        mappedVertex.coordinate = {vertex.position[0], vertex.position[1], 0, 1};
        mappedVertex.atlasBounds = {0, 0, 1, 1};
        mappedVertex.lights = {1, 2, 3, 4};
        mapped.vertices.push_back(mappedVertex);
      }
      mapped.indices = surface->indices;
      live = captured;
      live.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
      live.surface = std::make_shared<scene::Surface>(*surface);
      live.surface->indices = {2, 0, 1}; // same oriented triangle, changed corner order
      live.surface->vertices[2].uv[0] = 42;
      live.surface->vertices[2].color[0] = .25f;
      live.surface->vertices[2].normal[0] = .1f;
    }
  };
  void cpu() {
    Inputs input;
    ReceiverTriangles directory(input.mapped);
    const auto &captured = input.source->draws[0];
    const auto mapped = directory.map(captured, input.live, 1);
    require(mapped && mapped->indices.size() == 3 && mapped->vertices[0].coordinate == Vec4{1, 1, 0, 1},
            "Oriented live triangle subsets retain the captured corner mapping");
    require(mapped->vertices[0].surface.uv[0] == 42 && mapped->vertices[0].surface.color[0] == .25f &&
                mapped->vertices[0].surface.normal[0] == .1f && mapped->vertices[0].lights == bake::UInt4{1, 2, 3, 4},
            "Live material inputs and captured light ownership remain separate");
    auto changed = input.live;
    changed.model[0] = .99999988f;
    require(directory.map(captured, changed, .0254f).has_value(), "Engine matrix roundoff has a bounded allowance");
    changed.model[12] = .01f;
    require(!directory.map(captured, changed, .0254f), "Receiver movement cannot borrow the original placement");
    changed = input.live;
    changed.model[12] = changed.model[13] = 8e-6f;
    require(!directory.map(captured, changed, 1), "The world-space pose allowance bounds total displacement");
    changed = input.live;
    changed.surface = std::make_shared<scene::Surface>(*changed.surface);
    std::swap(changed.surface->indices[0], changed.surface->indices[1]);
    require(!directory.map(captured, changed, 1), "Opposite-facing coincident triangles do not share charts");
    changed.surface->vertices[0].position[0] = .1f;
    require(!directory.map(captured, changed, 1), "Changed geometry declines the complete subset mapping");
    changed = input.live;
    changed.instanceId++;
    require(!directory.map(captured, changed, 1), "Different object identities do not acquire receiver coverage");
    changed = input.live;
    changed.indexCount = 4;
    require(!directory.map(captured, changed, 1), "Malformed index ranges fail closed");
    changed = input.live;
    changed.surface = std::make_shared<scene::Surface>(*changed.surface);
    changed.surface->indices = {0, 1, 2, 3, 1, 2};
    changed.firstIndex = 3;
    changed.indexCount = 3;
    changed.vertexOffset = -1;
    require(directory.map(captured, changed, 1).has_value(),
            "Index ranges and signed vertex offsets select only the intended live triangle");
    changed = input.live;
    changed.surface = std::make_shared<scene::Surface>(*changed.surface);
    changed.surface->vertices[0].position[0] = std::numeric_limits<float>::quiet_NaN();
    require(!directory.map(captured, changed, 1), "Nonfinite live geometry fails closed");
    auto ambiguous = input.mapped;
    for (uint32_t corner = 0; corner < 3; ++corner) {
      auto vertex = input.mapped.vertices[corner];
      vertex.coordinate[3] = 2;
      ambiguous.indices.push_back(uint32_t(ambiguous.vertices.size()));
      ambiguous.vertices.push_back(vertex);
    }
    require(!ReceiverTriangles(ambiguous).map(captured, input.live, 1),
            "Ambiguous coincident chart ownership cannot choose an arbitrary lightmap");
    for (size_t i = 4; i < ambiguous.vertices.size(); ++i) ambiguous.vertices[i].coordinate[3] = 0;
    require(!ReceiverTriangles(ambiguous).map(captured, input.live, 1),
            "An uncharted coincident triangle does not become evidence of unique ownership");
    std::stop_source stop;
    stop.request_stop();
    bool cancelled = false;
    try { ReceiverTriangles unused(input.mapped, stop.get_token()); }
    catch (const std::runtime_error &) { cancelled = true; }
    require(cancelled, "Receiver directory preparation honours bake cancellation");
  }
  void gpu() {
    test::Fixture fixture;
    Inputs input;
    Layout layout;
    layout.pageSize = 4;
    layout.pages = 1;
    layout.charts = {{0, 0, 0, 2, 2}};
    Geometry geometry(fixture.physical, fixture.device, input.source, layout);
    const auto entry = geometry.find(input.live);
    require(entry && entry->count == 3 && entry->fullyMapped,
            "The runtime uploads a fully mapped replacement index subset");
    std::array<Vertex, 3> vertices;
    entry->vertices->read(vertices.data(), sizeof(vertices));
    require(vertices[0].surface.uv[0] == 42 && vertices[0].coordinate[3] == 1,
            "Uploaded receiver vertices preserve live UVs and captured chart ownership");
    auto fresh = input.live;
    fresh.surface = std::make_shared<scene::Surface>(*fresh.surface);
    require(geometry.find(fresh) == entry, "Fresh engine mesh objects reuse identical derived GPU buffers");
    // The source volume retains entries; later cache insertions cannot move a
    // pointer already held by a recorded frame. Capacity exhaustion falls back.
    for (uint32_t variant = 1; variant < 140; ++variant) {
      fresh.surface = std::make_shared<scene::Surface>(*input.live.surface);
      fresh.surface->vertices[2].uv[0] += float(variant);
      const auto derived = geometry.find(fresh);
      if (variant >= 128) require(!derived, "Derived receiver capacity never grows without bound");
    }
    require(geometry.find(input.live) == entry, "Existing entries survive derived cache capacity exhaustion");
    require(geometry.memoryBytes() < (33ull << 20), "Derived GPU allocations stay inside the receiver budget");
    auto ambiguous = std::make_shared<scene::BakeScene>(*input.source);
    ambiguous->draws.push_back(ambiguous->draws[0]);
    ambiguous->bindings.push_back(ambiguous->bindings[0]);
    ambiguous->receiverDraws = {1, 1};
    ambiguous->charts.push_back(ambiguous->charts[0]);
    ambiguous->charts[1].draw = 1;
    layout.charts.push_back({0, 2, 0, 2, 2});
    Geometry overlapping(fixture.physical, fixture.device, ambiguous, layout);
    require(!overlapping.find(input.live), "Distinct source draws cannot compete for one live receiver subset");
    require(test::validationErrors == 0, "Receiver buffer creation and lifetime are validation clean");
  }
}
int main(int argc, char **) {
  cpu();
  if (argc > 1) gpu();
  std::puts("Receiver triangle remapping preserves live inputs, ownership, pose and bounded buffers");
}

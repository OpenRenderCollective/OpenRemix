#include "scene/fixtures.h"
#include "scene/temporal.h"
#include <cmath>
#include <memory>

// remixapi_MeshInfoDeformEXT (repeated CreateMesh
// under a live hash replaces its vertices; no mesh or a different surface
// count ignores the struct), update lineage for the tracer, retained replay and
// temporal correspondence. Behind "Refresh mesh geometry in place".
using namespace remix::test;
namespace {
  struct Morph {
    remixapi_HardcodedVertex vertices[4] = {};
    uint32_t indices[6] = {0, 2, 1, 0, 3, 2}, bones[4] = {};
    float weights[4] = {1, 1, 1, 1};
    remixapi_MeshInfoSurfaceTriangles surfaces[2] = {};
    remixapi_MeshInfoDeformEXT refresh = {};
    remixapi_MeshInfo info = {};
    Morph(uint64_t hash, remixapi_MaterialHandle material, bool skin) {
      const float positions[4][3] = {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
      for (int i = 0; i < 4; ++i) {
        std::copy_n(positions[i], 3, vertices[i].position);
        vertices[i].color = 0xffffffff;
      }
      for (auto &s : surfaces) {
        s.vertices_values = vertices;
        s.vertices_count = 4;
        s.indices_values = indices;
        s.indices_count = 6;
        s.material = material;
        if (skin) {
          s.skinning_hasvalue = 1;
          s.skinning_value = {1, weights, 4, bones, 4};
        }
      }
      refresh.sType = REMIXAPI_STRUCT_TYPE_MESH_DEFORM_EXT;
      info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
      info.hash = hash;
      info.surfaces_values = surfaces;
      info.surfaces_count = 1;
    }
    remixapi_MeshHandle submit(remix::NativeResources &r, bool chained = true) {
      info.pNext = chained ? &refresh : nullptr;
      remixapi_MeshHandle handle = nullptr;
      r.createMesh(info, handle);
      require(reinterpret_cast<uintptr_t>(handle) == info.hash, "Refresh changed the handle");
      return handle;
    }
  };

  // The object a handle currently resolves to.
  std::shared_ptr<remix::scene::Mesh> current(remix::NativeResources &r, remixapi_MeshHandle handle) {
    remixapi_Transform bone = {};
    bone.matrix[0][0] = bone.matrix[1][1] = bone.matrix[2][2] = 1;
    remixapi_InstanceInfoBoneTransformsEXT skin = {};
    skin.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
    skin.boneTransforms_count = 1;
    skin.boneTransforms_values = &bone;
    auto info = instance(handle);
    info.pNext = &skin; // Ignored by unskinned meshes.
    r.finishFrame();
    r.drawInstance(info);
    auto mesh = r.draws().back().mesh;
    r.finishFrame();
    return mesh;
  }
} // namespace

int main() {
  remix::NativeResources r;
  auto red = material(r, 1, 1, 0, 0);

  // Off (default): the struct is ignored, a changed re-create replaces as before.
  {
    require(!r.refreshGeometry(), "Refresh is on by default");
    Morph m(10, red, false);
    auto handle = m.submit(r);
    auto first = current(r, handle);
    m.vertices[0].position[0] = -1.1f;
    m.submit(r);
    auto next = current(r, handle);
    require(next != first && first->retired && !next->updateStream && !next->motionCorrespondence,
            "Disabled refresh did not behave as a plain replacement");
  }

  r.setRefreshGeometry(true);
  // Unskinned (particles) and skinned (FaceGen heads) morphs.
  for (bool skin : {false, true}) {
    Morph m(skin ? 30 : 20, red, skin);
    // Nothing registered: a plain create.
    auto handle = m.submit(r);
    auto created = current(r, handle);
    require(!created->updateStream && !created->motionCorrespondence, "Refresh of nothing was not a plain create");

    m.vertices[0].position[0] = -1.1f;
    m.submit(r);
    auto first = current(r, handle);
    require(first != created && created->retired && first->updateStream && first->motionCorrespondence,
            "First refresh did not start an update lineage");
    require(created->surfaces[0]->vertices[0].position[0] == -1, "Refresh mutated the queued previous version");
    require(first->surfaces[0]->vertices[0].position[0] == -1.1f, "Refresh lost the new vertices");
    require(!skin || (first->surfaces[0]->bonesPerVertex == 1 && first->surfaces[0]->weights.size() == 4),
            "Refresh lost the skin");

    m.vertices[1].position[1] = -1.2f;
    m.submit(r);
    auto second = current(r, handle);
    require(second != first && second->updateStream == first->updateStream && second->motionCorrespondence,
            "Compatible refresh lost the lineage the tracer refits along");

    m.submit(r);
    require(current(r, handle) == second && !second->retired, "Identical refresh replaced the mesh");

    // Different surface count: a different mesh, not a morph. Ignored.
    m.info.surfaces_count = 2;
    m.vertices[2].position[2] = 5;
    m.submit(r);
    require(current(r, handle) == second && !second->retired, "Surface-count change was not ignored");
    m.info.surfaces_count = 1;

    // Changed topology: still refreshed, but a new lineage (one full build).
    std::swap(m.indices[1], m.indices[2]);
    m.submit(r);
    auto topology = current(r, handle);
    require(topology != second && topology->updateStream && topology->updateStream != second->updateStream,
            "Topology change kept a lineage built for other triangles");
    std::swap(m.indices[1], m.indices[2]);

    // Without the struct the old replacement path applies, ending the lineage.
    m.vertices[3].position[0] = 2;
    m.submit(r, false);
    require(!current(r, handle)->updateStream, "Unchained re-create kept refresh lineage");
  }

  // Retained instances follow the refresh with a stable identity, and temporal
  // history sees the same surface (previous vertices), not a new object.
  {
    Morph m(40, red, false);
    auto handle = m.submit(r);
    m.vertices[0].position[0] = -1.1f;
    m.submit(r); // Starts the lineage, as the plugin's first morph does.
    auto info = instance(handle);
    const uint64_t retained = r.createRetained(info);
    require(retained != 0, "Retained registration failed");
    r.finishFrame();
    r.drawRetained(nullptr);
    require(r.draws().size() == 1, "Retained morph did not draw");
    const auto before = r.draws()[0];
    remix::scene::TemporalHistory history;
    history.begin({1, 1}, 1, 64, 64);
    history.stage(before);
    history.commit();

    m.vertices[0].position[0] = -1.3f;
    m.submit(r);
    r.finishFrame();
    r.drawRetained(nullptr);
    require(r.draws().size() == 1, "Retained morph vanished after a refresh");
    const auto after = r.draws()[0];
    require(after.mesh != before.mesh && after.mesh->updateStream == before.mesh->updateStream,
            "Retained replay did not pick up the refreshed mesh");
    require(after.surface->vertices[0].position[0] == -1.3f, "Retained replay drew stale vertices");
    require(after.instanceId == before.instanceId && after.classification == before.classification,
            "Refresh changed the retained identity");
    history.begin({1, 1}, 1, 64, 64);
    const auto *previous = history.previousGeometry(after);
    require(previous && previous->draw.surface->vertices[0].position[0] == -1.1f,
            "Refreshed surface lost temporal correspondence");
    r.destroyRetained(&retained, 1);
  }
  // remixapi_UpdateMeshVerticesEXT: positions (normals, texture coordinates)
  // only, the rest kept; a lineage like a deform's; invalid calls change nothing.
  {
    Morph m(50, red, true);
    m.vertices[2].color = 0xff336699;
    m.vertices[2].texcoord[0] = .25f;
    m.info.surfaces_count = 2;
    auto handle = m.submit(r);
    auto created = current(r, handle);
    float positions[4][3] = {{-2, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
    float normals[4][3] = {{0, 0, 2}, {0, 0, 0}, {0, 3, 0}, {1, 0, 0}};
    remixapi_MeshVerticesEXT update = {1, 4, &positions[0][0], &normals[0][0], nullptr};
    r.updateMeshVertices(handle, 1, &update);
    auto next = current(r, handle);
    const auto &s = *next->surfaces[1];
    require(next != created && created->retired && next->updateStream && next->motionCorrespondence,
            "Vertex update did not start an update lineage");
    require(next->surfaces[0] == created->surfaces[0], "Vertex update copied a surface it did not change");
    require(s.vertices[0].position[0] == -2 && s.vertices[0].normal[2] == 1 && s.vertices[1].normal[2] == 0 &&
                s.vertices[2].normal[1] == 1,
            "Vertex update lost positions or normals");
    require(s.vertices[2].uv[0] == .25f && s.vertices[2].color[3] == 1 &&
                s.vertices[2].color[0] == created->surfaces[1]->vertices[2].color[0] && s.indices.size() == 6 &&
                s.bonesPerVertex == 1 && s.weights.size() == 4,
            "Vertex update lost what it does not send");
    require(created->surfaces[1]->vertices[0].position[0] == -1, "Vertex update mutated the queued version");
    require(s.center[0] == -.5f, "Vertex update kept the old bounds centre");

    float texcoords[4][2] = {{.5f, .5f}, {}, {}, {}};
    update.normals = nullptr;
    update.texcoords = &texcoords[0][0];
    r.updateMeshVertices(handle, 1, &update);
    auto third = current(r, handle);
    require(third->updateStream == next->updateStream && third->surfaces[1]->vertices[0].uv[0] == .5f &&
                third->surfaces[1]->vertices[0].normal[2] == 1,
            "Texture coordinate update lost the lineage or the normals");
    r.updateMeshVertices(handle, 1, &update);
    require(current(r, handle) == third && !third->retired, "Identical vertex update replaced the mesh");

    auto rejected = [&](remixapi_MeshHandle h, const remixapi_MeshVerticesEXT &u, uint32_t count = 1) {
      bool threw = false;
      const remixapi_MeshVerticesEXT twice[2] = {u, u};
      try {
        r.updateMeshVertices(h, count, count == 2 ? twice : &u);
      } catch (remixapi_ErrorCode) {
        threw = true;
      }
      require(threw && current(r, handle) == third, "Invalid vertex update was accepted or changed the mesh");
    };
    auto bad = update;
    bad.surface = 2;
    rejected(handle, bad);
    bad = update;
    bad.vertexCount = 3;
    rejected(handle, bad);
    bad = update;
    bad.positions = nullptr;
    rejected(handle, bad);
    rejected(handle, update, 2); // the same surface twice
    positions[3][1] = NAN;
    rejected(handle, update);
    positions[3][1] = 1;
    rejected(reinterpret_cast<remixapi_MeshHandle>(uintptr_t(999)), update);

    // Refresh off: a plain re-registration under the same hash.
    r.setRefreshGeometry(false);
    positions[0][0] = -3;
    r.updateMeshVertices(handle, 1, &update);
    auto plain = current(r, handle);
    require(plain != third && !plain->updateStream && plain->surfaces[1]->vertices[0].position[0] == -3,
            "Vertex update with refresh off was not a plain replacement");
    r.setRefreshGeometry(true);
  }
  std::puts("Mesh refresh: fork semantics, update lineage, retained replay and temporal correspondence passed");
  return 0;
}

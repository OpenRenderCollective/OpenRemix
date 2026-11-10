#include "forward/lightmap/decal.h"
#include "forward/lighting.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
using namespace remix;
namespace lm = forward::lightmap;
void require(bool value, const char *message) {
  if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main() {
  scene::BakeScene source;
  source.sceneScale = 1;
  auto material = std::make_shared<scene::Material>();
  scene::Draw receiver;
  receiver.material = material;
  receiver.classification = REMIXAPI_INSTANCE_STATIC_EXT;
  source.draws.push_back(receiver);
  lm::Mesh mapped; mapped.draw = 0;
  const float positions[6][3] = {{-1,-1,0},{1,-1,0},{1,1,0},{-1,-1,0},{1,1,0},{-1,1,0}};
  for (uint32_t i = 0; i < 6; ++i) {
    lm::Vertex vertex{};
    std::copy_n(positions[i], 3, vertex.surface.position);
    vertex.coordinate = {(positions[i][0]+1)/2, (positions[i][1]+1)/2, float(i/3), float(i/3+1)};
    vertex.lights = {i/3,~0u,~0u,~0u};
    vertex.directLights = {i/3+10,~0u,~0u,~0u};
    vertex.atlasBounds = {.01f,.02f,.98f,.99f};
    mapped.indices.push_back(i); mapped.vertices.push_back(vertex);
  }
  scene::Draw paint = receiver;
  paint.decal = true; paint.blend.enabled = true;
  paint.surface = std::make_shared<scene::Surface>();
  for (auto p : {std::array<float,3>{-.8f,-.8f,.001f}, {.8f,-.8f,.001f}, {0,.8f,.001f}}) {
    scene::Vertex vertex{};
    std::copy(p.begin(), p.end(), vertex.position);
    vertex.uv[0] = p[0]; vertex.uv[1] = p[1]; vertex.color[3] = .5f;
    paint.surface->vertices.push_back(vertex);
  }
  paint.surface->indices = {0,1,2}; source.draws.push_back(paint);
  auto meshes = lm::decalMeshes(source, {&mapped,1});
  require(meshes.size()==1 && meshes[0].draw==1 && meshes[0].indices.size()>=6,
          "Paint crossing receiver charts is clipped into independently mapped pieces");
  bool pages[2]{};
  for (const auto &v : meshes[0].vertices) {
    require(std::abs(v.coordinate[0]-(v.surface.position[0]+1)/2)<1e-5 &&
            std::abs(v.coordinate[1]-(v.surface.position[1]+1)/2)<1e-5,
            "Decals borrow receiver atlas coordinates rather than allocate chart space");
    const auto page = uint32_t(v.coordinate[2]); pages[page]=true;
    require(v.coordinate[3]==page+1 && v.lights[0]==page && v.directLights[0]==page+10 &&
            v.atlasBounds==mapped.vertices[0].atlasBounds &&
            std::abs(v.surface.uv[0]-v.surface.position[0])<1e-5 && v.surface.color[3]==.5f,
            "Receiver ownership and bounds coexist with interpolated decal material coordinates and alpha");
  }
  require(pages[0] && pages[1], "Both receiver chart pages remain represented");
  source.draws[1].model[14] = 1;
  require(lm::decalMeshes(source,{&mapped,1}).empty(), "Detached paint cannot steal distant receiver light");
  source.draws[1].model[14] = 0;
  std::stop_source stop; stop.request_stop();
  bool cancelled=false;
  try { lm::decalMeshes(source,{&mapped,1},stop.get_token()); } catch (const std::runtime_error &) { cancelled=true; }
  require(cancelled,"Receiver projection respects cancellation");
}

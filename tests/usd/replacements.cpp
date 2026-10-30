#include "usd/mods.h"
#include "usd/submission.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif
using namespace remix;
void require(bool v, const char *m) {
  if (!v)
    throw std::runtime_error(m);
}
void write(const std::filesystem::path &p, const std::string &s) { std::ofstream(p) << s; }
void settle(NativeResources &r) {
  for (int i = 0; i < 70; ++i) {
    usd::modBoundary(r);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}
void texture(const std::filesystem::path &p, uint8_t red) {
  // Minimal 1x1 RGBA8 DDS. Same path and file size, changed decoded contents.
  uint32_t header[32] = {};
  header[0] = 0x20534444;
  header[1] = 124;
  header[2] = 0x100f;
  header[3] = header[4] = 1;
  header[5] = 4;
  header[7] = 1;
  header[19] = 32;
  header[20] = 0x41;
  header[22] = 32;
  header[23] = 0xff;
  header[24] = 0xff00;
  header[25] = 0xff0000;
  header[26] = 0xff000000;
  header[27] = 0x1000;
  std::ofstream f(p, std::ios::binary);
  f.write((char *)header, sizeof(header));
  uint8_t pixel[] = {red, 0, 0, 255};
  f.write((char *)pixel, 4);
}
int main(int argc, char **argv) {
  try {
    require(argc == 2, "fixture path");
    auto fixture = std::filesystem::absolute(argv[1]);
    auto dir =
        std::filesystem::temp_directory_path() / ("openremix-replacements-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir / "a/maps");
    std::filesystem::create_directories(dir / "b");
    std::filesystem::create_directories(dir / "layers");
    // Windows separators in composed sublayers must be resolved in their authoring layer.
    std::filesystem::copy_file(fixture / "capture.usda", dir / "layers/capture.usda");
    write(dir / "project.usda", "#usda 1.0\n( subLayers = [@layers\\capture.usda@] )\n");
    auto captured = usd::load({(dir / "project.usda").string()});
    require(captured.scene["meshes"].size() == 2, "Windows sublayer composition");
    const auto mod = std::string(R"USD(#usda 1.0
(metersPerUnit = 1; upAxis = "Y")
def Xform "RootNode" {
 def Scope "meshes" {
  def Xform "mesh_0123456789ABCDEF" (prepend references = @)USD") +
                     fixture.string() + R"USD(/capture.usda@</RootNode/meshes/mesh_0123456789ABCDEF>) {
   token visibility = "inherited"
   custom int preserveOriginalDrawCall = 1
   double3 xformOp:translate = (2,0,0)
   uniform token[] xformOpOrder = ["xformOp:translate"]
   def SphereLight "attached" {
    float inputs:intensity = 3
   }
  }
 }
 def Scope "Looks" {
  def Material "mat_0123456789ABCDEF" {
   token outputs:mdl:surface.connect = </RootNode/Looks/mat_0123456789ABCDEF/Shader.outputs:out>
   def Shader "Shader" {
    color3f inputs:diffuse_color_constant = (1,1,1)
    asset inputs:diffuse_texture = @maps\color.dds@
    token outputs:out
   }
  }
 }
 def Scope "lights" {
  over "light_0000000000000123" {
   color3f inputs:color = (0,1,0)
   float inputs:intensity = 5
   float inputs:radius = 1
  }
 }
}
)USD";
    write(dir / "a/mod.usda", mod);
    texture(dir / "a/maps/color.dds", 200);
    write(dir / "b/mod.usda", R"USD(#usda 1.0
def Xform "RootNode" { def Scope "meshes" { def Xform "mesh_0123456789ABCDEF" {
custom int preserveOriginalDrawCall = 0
}
}
}
)USD");
    auto prepared = usd::load({(dir / "a/mod.usda").string(), 1, 1, true});
    require(prepared.textures.size() == 1, "parser prepares texture data off the render thread");
    std::filesystem::remove(dir / "a/maps/color.dds");
    {
      NativeResources isolated;
      usd::Submission ready(isolated, prepared, 77, false);
      isolated.drawInstance(ready.instances().front(),
                            ready.materialHandle("/RootNode/Looks/mat_0123456789ABCDEF"));
      require(isolated.draws().back().material->texture->pixels.mips[0][0] == 200,
              "publication uses prepared textures without reading files");
    }
    texture(dir / "a/maps/color.dds", 200);
    NativeResources resources;
    struct Stop {
      NativeResources &r;
      ~Stop() { usd::stopMods(r); }
    } stop{resources};
    usd::Submission scene(resources, captured, 1);
    // A (re)loaded mod's replacements load when first drawn: one frame asks
    // for them, the next frames create them.
    auto load = [&] {
      settle(resources);
      scene.draw(false, 1);
      resources.finishFrame();
      settle(resources);
    };
    usd::startMods(true);
    require(!usd::configureMods(dir.string().c_str(), 1, 1, true), "configure mods");
    load();
    require(!*usd::modDiagnostic(), usd::modDiagnostic());
    scene.draw(false, 1);
    require(resources.draws().size() == 4, "first sorted replacement preserves originals");
    std::set<uint64_t> identities;
    for (auto &draw : resources.draws())
      identities.insert(draw.instanceId);
    require(identities.size() == 4, "attachments derive identities from repeated live parents");
    auto lights = resources.sphereLights();
    require(lights.size() == 2 && lights[0].id != lights[1].id, "attached lights have distinct identities");
    require(resources.draws().back().material->texture->pixels.mips[0][0] == 200,
            "material-only hash override and Windows texture path");
    resources.finishFrame();
    scene.draw(false, 1);
    std::set<uint64_t> repeated;
    for (auto &draw : resources.draws())
      repeated.insert(draw.instanceId);
    require(identities == repeated, "attachment identities survive ordinary frames");
    resources.finishFrame();
    auto batched = scene.instances().front();
    remixapi_Transform transforms[2]{};
    for (auto &t : transforms)
      for (int c = 0; c < 3; ++c)
        t.matrix[c][c] = 1;
    transforms[1].matrix[0][3] = 4;
    remixapi_InstanceInfoGpuInstancingEXT batch{};
    batch.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_GPU_INSTANCING_EXT;
    batch.instanceTransforms_count = 2;
    batch.instanceTransforms_values = transforms;
    remixapi_InstanceIdentityEXT parent{};
    parent.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    parent.instanceId = 1234;
    parent.pNext = &batch;
    batched.pNext = &parent;
    usd::modInstance(resources, batched, 0x0123456789ABCDEFull);
    require(resources.draws().size() == 2 &&
                resources.draws()[0].instanceId != resources.draws()[1].instanceId,
            "GPU-instanced parents receive distinct attachment identities");
    require(resources.draws()[1].model[12] - resources.draws()[0].model[12] == 4,
            "GPU-instanced parent transforms propagate to attachments");
    lights = resources.sphereLights();
    require(lights.size() == 2 && lights[0].id != lights[1].id,
            "GPU-instanced attached lights have distinct identities");
    resources.finishFrame();
    {
      usd::Submission first(resources, captured, (1ull << 32) + 1);
      usd::Submission second(resources, captured, (2ull << 32) + 1);
      first.draw(false, 1);
      second.draw(false, 1);
      require(resources.draws().size() == 8, "simultaneous direct-loader scenes have distinct identities");
      resources.finishFrame();
    }
    remixapi_LightInfoUSDEXT light{};
    light.lightType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_USD_EXT;
    for (int c = 0; c < 3; ++c)
      light.transform.matrix[c][c] = 1;
    light.transform.matrix[0][3] = 10;
    remixapi_Float3D red{1, 0, 0};
    light.pColor = &red;
    remixapi_LightInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.pNext = &light;
    info.hash = 0x123;
    remixapi_LightHandle original{};
    resources.createLight(info, original);
    require(usd::modLight(resources, original), "untyped light override matched");
    lights = resources.sphereLights();
    require(lights.size() == 1 && lights[0].position[0] == 10 && lights[0].radiance[1] == 5 &&
                lights[0].radiance[0] == 0 && lights[0].radius == 1,
            "partial light override retains original transform");
    resources.finishFrame();
    texture(dir / "a/maps/color.dds", 50);
    load();
    scene.draw(false, 1);
    require(resources.draws().back().material->texture->pixels.mips[0][0] == 50,
            "changed texture contents reload");
    resources.finishFrame();
    write(dir / "a/maps/color.dds", "malformed texture");
    load();
    require(*usd::modDiagnostic(), "texture failure reports diagnostic");
    // Left out of its material (as RTX Remix leaves it): the mod still applies.
    scene.draw(false, 1);
    require(resources.draws().size() == 4, "a texture that does not decode leaves the mod applied");
    resources.finishFrame();
    auto missing = mod;
    auto texturePath = missing.find("color.dds");
    missing.replace(texturePath, 9, "new.dds");
    write(dir / "a/mod.usda", missing);
    load();
    // A missing texture is left out (and watched): the mod still applies.
    scene.draw(false, 1);
    require(resources.draws().size() == 4, "a missing texture leaves the mod applied");
    resources.finishFrame();
    texture(dir / "a/maps/new.dds", 90);
    load();
    require(!*usd::modDiagnostic(), "creating a missing dependency retries reload");
    scene.draw(false, 1);
    require(resources.draws().back().material->texture->pixels.mips[0][0] == 90,
            "new dependency publishes after recovery");
    resources.finishFrame();
    std::filesystem::remove(dir / "a/mod.usda");
    load();
    scene.draw(false, 1);
    require(resources.draws().empty(), "next sorted replacement becomes active after removal");
    resources.finishFrame();
    std::filesystem::remove(dir / "b/mod.usda");
    load();
    scene.draw(false, 1);
    require(resources.draws().size() == 2, "removing mods restores original meshes");
    require(resources.draws()[0].material->texture->pixels.mips[0][0] == 255,
            "original material retained separately");
    resources.finishFrame();
    resources.drawLight(original);
    require(resources.sphereLights()[0].radiance[0] == 1, "original light retained separately");
    resources.finishFrame();
    usd::stopMods(resources);
    std::filesystem::remove_all(dir);
    std::cout << "Capture replay replacements passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "Resource failure\n";
    return 1;
  }
}

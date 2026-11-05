#include "usd/mods.h"
#include "usd/submission.h"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <thread>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif
using namespace remix;
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  try {
    require(argc == 2, "supply the fixture directory");
    std::filesystem::path fixture = argv[1];
    auto capture = usd::load({(fixture / "capture.usda").string()});
    require(!capture.scene["meshes"].empty(), "capture geometry");
    require(!capture.dependencies.empty(), "composed layer dependencies");
    NativeResources resources;
    {
      usd::Submission scene(resources, capture, 1);
      scene.draw(false, 1);
      require(!resources.draws().empty(), "standalone resource submission");
      auto id = resources.draws()[0].instanceId;
      resources.finishFrame();
      scene.draw(false, 1);
      require(resources.draws()[0].instanceId == id, "stable identities");
      resources.finishFrame();
    }
    auto skeleton = usd::load({(fixture / "skinned/capture.usda").string()});
    bool skinned = false;
    for (auto &mesh : skeleton.scene["meshes"])
      skinned |= mesh["skinned"].get<bool>();
    require(skinned, "captured skeletal pose");
    {
      // AperturePBR's thin film with OpenRemix's iridescence attributes, read
      // into the material JSON and submitted as remixapi_MaterialIridescenceEXT.
      std::ifstream in(fixture / "capture.usda");
      std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      const std::string shader = "int inputs:alpha_test_type = 7";
      const auto at = text.find(shader);
      require(at != std::string::npos, "capture fixture shader");
      text.insert(at + shader.size(), "\nbool inputs:enable_thin_film = true"
                                      "\nfloat inputs:thin_film_thickness_constant = 450"
                                      "\nfloat openremix:iridescence:filmIor = 2.4"
                                      "\nfloat openremix:iridescence:flowAmplitude = 5000"
                                      "\nfloat openremix:iridescence:structural = 0.5");
      const auto filmed = std::filesystem::temp_directory_path() /
                          ("openremix-usd-film-" + std::to_string(getpid()) + ".usda");
      std::ofstream(filmed) << text;
      auto loaded = usd::load({filmed.string()});
      const auto &film = loaded.scene["materials"].at(0).at("iridescence");
      require(film["thickness"] == 450 && std::abs(film["ior"].get<float>() - 2.4f) < 1e-6f &&
                  film["strength"] == 1 && film["structural"] == .5,
              "thin film in the material JSON");
      NativeResources filmResources;
      usd::Submission submitted(filmResources, loaded, 1);
      submitted.draw(false, 1);
      require(!filmResources.draws().empty(), "filmed submission");
      const auto &drawnFilm = filmResources.draws()[0].material->iridescence;
      require(drawnFilm.strength == 1 && drawnFilm.thickness == 450 && std::abs(drawnFilm.ior - 2.4f) < 1e-6f &&
                  drawnFilm.flowAmplitude == 2000 && drawnFilm.structural == .5f,
              "thin film submitted, out-of-range values clamped");
      filmResources.finishFrame();
      std::filesystem::remove(filmed);
    }
    {
      // AperturePBR_Translucent's thin wall and body, read into the material
      // JSON and submitted: a thin wall is the tinted sheet, a body refracts
      // and absorbs over its measurement distance.
      std::ifstream in(fixture / "capture.usda");
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      const std::string shader = "int inputs:alpha_test_type = 7";
      const auto at = text.find(shader);
      require(at != std::string::npos, "capture fixture shader");
      auto translucent = [&](bool thin) {
        auto glass = text;
        glass.insert(at + shader.size(), std::string("\nuniform asset info:mdl:sourceAsset = @AperturePBR_Translucent.mdl@"
                                                     "\nfloat inputs:ior_constant = 1.33"
                                                     "\nfloat inputs:transmittance_measurement_distance = 4") +
                                             (thin ? "\nbool inputs:thin_walled = true" : ""));
        const auto path = std::filesystem::temp_directory_path() /
                          ("openremix-usd-glass-" + std::to_string(getpid()) + ".usda");
        std::ofstream(path) << glass;
        auto loaded = usd::load({path.string()});
        std::filesystem::remove(path);
        return loaded;
      };
      auto body = translucent(false);
      const auto &json = body.scene["materials"].at(0);
      require(json["kind"] == "translucent" && json["transmittance_distance"] == 4 && !json["thin_walled"],
              "translucent body in the material JSON");
      for (bool thin : {false, true}) {
        auto loaded = thin ? translucent(true) : body;
        NativeResources glassResources;
        usd::Submission submitted(glassResources, loaded, 1);
        submitted.draw(false, 1);
        require(!glassResources.draws().empty(), "translucent submission");
        const auto &material = *glassResources.draws()[0].material;
        require(material.transmission && material.transmittanceDistance == (thin ? 0 : 4) &&
                    (thin || std::abs(material.refractiveIndex - 1.33f) < 1e-6f),
                thin ? "thin wall submitted as a sheet" : "body submitted with its measurement distance");
        glassResources.finishFrame();
      }
    }
    {
      // A mod's `over` of a captured material records the parts it authors
      // (scene::ModParts); a definition replaces the whole material.
      const auto path = std::filesystem::temp_directory_path() /
                        ("openremix-usd-parts-" + std::to_string(getpid()) + ".usda");
      auto parts = [&](const char *specifier) {
        std::ofstream(path) << "#usda 1.0\n" << specifier << " Xform \"RootNode\" { " << specifier
                            << " Scope \"Looks\" { " << specifier
                            << " Material \"mat_0123456789ABCDEF\" { " << specifier
                            << " Shader \"Shader\" {\nbool inputs:enable_thin_film = 1\n"
                               "float openremix:iridescence:filmIor = 2\n}\n}\n}\n}\n";
        auto loaded = usd::load({path.string()});
        std::filesystem::remove(path);
        return loaded.scene["replacements"]["materials"].at(0);
      };
      require(parts("over").value("authored", 0u) == scene::ModParts::film, "an override authors its film only");
      require(!parts("def").contains("authored"), "a defined material replaces the whole material");
      std::ofstream(path) << "#usda 1.0\nover Xform \"RootNode\" { over Scope \"Looks\" { over Material "
                             "\"mat_0123456789ABCDEF\" { over Shader \"Shader\" {\nfloat openremix:subsurface:weight = 1\n}\n}\n}\n}\n";
      auto sssOver = usd::load({path.string()});
      std::filesystem::remove(path);
      require(sssOver.scene["replacements"]["materials"].at(0).value("authored", 0u) == scene::ModParts::subsurface,
              "an override authors its subsurface only");
    }
    {
      // RTX Remix's subsurface inputs and OpenRemix's weight, read into the
      // material JSON and submitted.
      std::ifstream sssIn(fixture / "capture.usda");
      std::string text((std::istreambuf_iterator<char>(sssIn)), std::istreambuf_iterator<char>());
      const std::string shader = "int inputs:alpha_test_type = 7";
      const auto at = text.find(shader);
      require(at != std::string::npos, "subsurface fixture shader");
      text.insert(at + shader.size(), "\nbool inputs:subsurface_diffusion_profile = 1"
                                      "\ncolor3f inputs:subsurface_radius = (1, 0.4, 0.2)"
                                      "\nfloat inputs:subsurface_radius_scale = 22"
                                      "\nfloat inputs:subsurface_max_sample_radius = 1"
                                      "\nfloat openremix:subsurface:weight = 0.75");
      const auto sssPath = std::filesystem::temp_directory_path() /
                           ("openremix-usd-sss-" + std::to_string(getpid()) + ".usda");
      std::ofstream(sssPath) << text;
      auto loaded = usd::load({sssPath.string()});
      std::filesystem::remove(sssPath);
      const auto &sss = loaded.scene["materials"].at(0).at("subsurface");
      require(sss["weight"] == .75 && sss["diffusion_profile"] == true && sss["radius_scale"] == 22 &&
                  std::abs(sss["radius"][1].get<float>() - .4f) < 1e-6f && sss["measurement_distance"] == 0 &&
                  sss["transmittance"][0] == 1,
              "subsurface in the material JSON");
      NativeResources sssResources;
      usd::Submission submitted(sssResources, loaded, 1);
      submitted.draw(false, 1);
      require(!sssResources.draws().empty(), "subsurface submission");
      const auto &drawn = sssResources.draws()[0].material->subsurface;
      require(drawn.weight == .75f && drawn.diffusionProfile && std::abs(drawn.radius[2] - .2f) < 1e-6f &&
                  drawn.radiusScale == 22 && drawn.maxSampleRadius == 1,
              "subsurface submitted");
      sssResources.finishFrame();
    }
    {
      // A polygon with no area is left as a fan: one such face must not lose its mesh.
      const auto path = std::filesystem::temp_directory_path() / ("openremix-degenerate-" + std::to_string(getpid()) + ".usda");
      std::ofstream(path) << "#usda 1.0\ndef Mesh \"flat\" {\npoint3f[] points = [(0,0,0),(1,0,0),(2,0,0),(3,0,0),(0,1,0),(1,1,0),(0,0,1)]\n"
                             "int[] faceVertexCounts = [4,3]\nint[] faceVertexIndices = [0,1,2,3,4,5,6]\n}\n";
      auto loaded = usd::load({path.string()});
      std::filesystem::remove(path);
      require(loaded.scene["meshes"].size() == 1 && loaded.scene["warnings"].empty(), "a degenerate polygon keeps its mesh");
    }
    auto converted = usd::load({(fixture / "capture.usda").string(), 1, 2});
    require(converted.scene["meters_per_unit"] == 1 && converted.scene["up_axis"] == "Z",
            "explicit host coordinates");
    require(usd::capturedHash("/RootNode/meshes/mesh_0123456789ABCDEF") == 0x0123456789abcdefull,
            "captured mesh hash");
    auto dir = std::filesystem::temp_directory_path() / ("openremix-usd-test-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir / "a");
    auto write = [&](const std::string &text) { std::ofstream(dir / "a/mod.usda") << text; };
    write("#usda 1.0\ndef Xform \"RootNode\" { def Scope \"meshes\" { def Xform \"mesh_0123456789ABCDEF\" "
          "{\n custom int preserveOriginalDrawCall = 0\n }\n }\n }\n");
    // rtx.zUp: mods and captures follow the scene's axis until the host sets one.
    scene::sceneUpAxis = 2;
    usd::startMods(true);
    require(usd::modUnits().second == 2, "unconfigured mods follow rtx.zUp");
    require(usd::configureMods(dir.string().c_str(), 1, 1, true) == REMIXAPI_ERROR_CODE_SUCCESS, "configure mods");
    require(usd::modUnits().second == 1, "a host's axis overrides rtx.zUp");
    scene::sceneUpAxis = 1;
    for (int i = 0; i < 30; ++i) {
      usd::modBoundary(resources);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    remixapi_InstanceInfo original{};
    original.mesh = reinterpret_cast<remixapi_MeshHandle>(0x0123456789abcdefull);
    // A replacement loads when first drawn; the original draws meanwhile.
    require(!usd::modInstance(resources, original), "a replacement loads before it applies");
    for (int i = 0; i < 30 && !usd::modInstance(resources, original); ++i) {
      usd::modBoundary(resources);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    require(usd::modInstance(resources, original), "empty replacement hides original");
    write("broken USD");
    for (int i = 0; i < 40; ++i) {
      usd::modBoundary(resources);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    require(usd::modInstance(resources, original), "malformed reload retains replacement");
    require(*usd::modDiagnostic(), "malformed reload diagnostic");
    std::filesystem::remove(dir / "a/mod.usda");
    for (int i = 0; i < 40; ++i) {
      usd::modBoundary(resources);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    require(!usd::modInstance(resources, original), "removal restores original");
    // A game light's replacement lights (light_<hash> children) sit in the
    // game light's frame: 2 units along -Z of a spot light at (10, 0, 0)
    // aimed along +X is (12, 0, 0). Its light scale (customLayerData
    // openremix:lightScale) scales them.
    write("#usda 1.0\n(\ncustomLayerData = {\ndouble \"openremix:lightScale\" = 0.5\n}\nmetersPerUnit = 1\n"
          "upAxis = \"Y\"\n)\ndef Xform \"RootNode\" { def Scope \"lights\" { "
          "over \"light_0000000000000456\" {\n def SphereLight \"child\" {\n float inputs:radius = 1\n"
          " double3 xformOp:translate = (0, 0, -2)\n uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
          " }\n }\n }\n }\n");
    remixapi_LightInfoSphereEXT spot{};
    spot.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    spot.position = {10, 0, 0};
    spot.radius = 1;
    spot.volumetricRadianceScale = 1;
    spot.shaping_hasvalue = 1;
    spot.shaping_value.direction = {1, 0, 0};
    spot.shaping_value.coneAngleDegrees = 30;
    remixapi_LightInfo gameLight{};
    gameLight.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    gameLight.pNext = &spot;
    gameLight.hash = 0x456;
    gameLight.radiance = {1, 1, 1};
    remixapi_LightHandle game{};
    resources.createLight(gameLight, game);
    float childRadiance = 0;
    auto childPlaced = [&] {
      for (const auto &light : resources.sphereLights())
        if (std::fabs(light.position[0] - 12) < 1e-3f && std::fabs(light.position[1]) < 1e-3f &&
            std::fabs(light.position[2]) < 1e-3f) {
          childRadiance = light.radiance[0];
          return true;
        }
      return false;
    };
    bool placed = false;
    for (int i = 0; i < 60 && !placed; ++i) {
      usd::modBoundary(resources);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      usd::modLight(resources, game);
      placed = childPlaced();
      resources.finishFrame();
    }
    require(placed, "replacement lights sit in the game light's frame");
    const auto status = usd::modStatus();
    require(status.mods.size() == 1 && status.mods[0].authoredLightScale == 0.5f, "authored light scale");
    const float authored = childRadiance;
    usd::setModLightScale(status.mods[0].name, 4);
    usd::modBoundary(resources);
    usd::modLight(resources, game);
    require(childPlaced() && authored > 0 && std::fabs(childRadiance / authored - 8) < 1e-3f,
            "light scale multiplies mod lights");
    resources.finishFrame();
    require(usd::saveModLightScale(status.mods[0].name).empty(), "light scale saved");
    {
      std::ifstream saved(dir / "a/mod.usda");
      const std::string text{std::istreambuf_iterator<char>(saved), std::istreambuf_iterator<char>()};
      require(text.find("\"openremix:lightScale\" = 4") != std::string::npos, "light scale written to mod.usda");
    }
    // The names USD lights had before "inputs:", which Remix mods still use.
    write("#usda 1.0\n(\nmetersPerUnit = 1\nupAxis = \"Y\"\n)\ndef Xform \"RootNode\" { def Scope \"lights\" { "
          "over \"light_0000000000000456\" {\n def SphereLight \"child\" {\n float intensity = 40\n float radius = 1\n"
          " double3 xformOp:translate = (0, 0, -2)\n uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
          " }\n }\n }\n }\n");
    bool legacy = false;
    for (int i = 0; i < 300 && !legacy; ++i) {
      usd::modBoundary(resources);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      usd::modLight(resources, game);
      legacy = childPlaced() && std::fabs(childRadiance - 40) < 1e-3f;
      resources.finishFrame();
    }
    require(legacy, "legacy light attributes");
    // Lights under a mesh's replacement stay the same lights while its
    // instance moves (the host gives it no identity): new ones each frame
    // would rebuild the light tree, and the guide with it, as it moves.
    write("#usda 1.0\n(\nmetersPerUnit = 1\nupAxis = \"Y\"\n)\ndef Xform \"RootNode\" { def Scope \"meshes\" { "
          "def Xform \"mesh_0123456789ABCDEF\" {\n def SphereLight \"child\" {\n float inputs:radius = 1\n }\n }\n }\n }\n");
    auto attached = [&](std::initializer_list<float> positions) {
      usd::modBoundary(resources);
      for (float x : positions) {
        auto instance = original;
        for (int c = 0; c < 3; ++c)
          instance.transform.matrix[c][c] = 1;
        instance.transform.matrix[0][3] = x;
        usd::modInstance(resources, instance);
      }
      std::map<float, uint64_t> ids;
      for (const auto &light : resources.sphereLights())
        ids[light.position[0]] = light.id;
      resources.finishFrame();
      return ids;
    };
    bool attachedReady = false;
    for (int i = 0; i < 300 && !attachedReady; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      attachedReady = attached({0}).size() == 1;
    }
    require(attachedReady, "lights under a mesh replacement");
    const auto still = attached({0}).at(0);
    const auto moved = attached({1});
    require(moved.size() == 1 && moved.at(1) == still, "a moving instance keeps its attached lights");
    require(attached({100}).at(100) != still, "an instance far from the last frame's gets new lights");
    const auto pair = attached({0, 5});
    const auto swapped = attached({5, 0});
    require(pair.size() == 2 && pair.at(0) != pair.at(5) && swapped == pair,
            "static instances keep their lights in any draw order");
    const auto returning = attached({20}).at(20);
    attached({});
    require(attached({20}).at(20) == returning, "an instance back in view keeps its lights");
    // A skinned replacement: posed by the replaced draw's bones through its own
    // joint indices, whatever its skeleton, and what that draw is (view model).
    // The skeleton named here does not exist, as in mods clearing their joints.
    write("#usda 1.0\n(\nmetersPerUnit = 1\nupAxis = \"Y\"\n)\ndef Xform \"RootNode\" { def Scope \"meshes\" { "
          "def Xform \"mesh_0123456789ABCDEF\" {\n custom int preserveOriginalDrawCall = 0\n"
          "def Mesh \"body\" (prepend apiSchemas = [\"SkelBindingAPI\"]) {\n"
          "point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]\nint[] faceVertexCounts = [3]\nint[] faceVertexIndices = [0,1,2]\n"
          "int[] primvars:skel:jointIndices = [0,2,0] (elementSize = 1\n interpolation = \"vertex\")\n"
          "float[] primvars:skel:jointWeights = [1,1,1] (elementSize = 1\n interpolation = \"vertex\")\n"
          "rel skel:skeleton = </RootNode/meshes/gone>\n}\n"
          "def SphereLight \"child\" {\n float inputs:radius = 1\n }\n }\n }\n }\n");
    const auto skinnedDraw = [&](const std::vector<float> &bonesX, uint32_t categories) {
      std::vector<remixapi_Transform> bones(bonesX.size());
      for (size_t b = 0; b < bonesX.size(); ++b) {
        for (int c = 0; c < 3; ++c)
          bones[b].matrix[c][c] = 1;
        bones[b].matrix[0][3] = bonesX[b];
      }
      remixapi_InstanceInfoBoneTransformsEXT given{};
      given.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
      given.boneTransforms_values = bones.data();
      given.boneTransforms_count = uint32_t(bones.size());
      auto instance = original;
      for (int c = 0; c < 3; ++c)
        instance.transform.matrix[c][c] = 1;
      instance.categoryFlags = categories;
      instance.pNext = bonesX.empty() ? nullptr : &given;
      return usd::modInstance(resources, instance);
    };
    bool skinnedReady = false;
    for (int i = 0; i < 300 && !skinnedReady; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      usd::modBoundary(resources);
      skinnedReady = skinnedDraw({0, 0, 0}, 0) && !resources.draws().empty() && resources.draws()[0].surface->bonesPerVertex;
      resources.finishFrame();
    }
    require(skinnedReady, "a replacement keeps its joint influences");
    usd::modBoundary(resources);
    skinnedDraw({1, 2, 3}, REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL);
    {
      const auto &draw = resources.draws().at(0);
      require(draw.bones && draw.bones->size() == 3 && (*draw.bones)[2][12] == 3, "a replacement takes its draw's bones");
      require(draw.camera == REMIXAPI_CAMERA_TYPE_VIEW_MODEL, "a replacement of a view model is one");
    }
    resources.finishFrame();
    usd::modBoundary(resources);
    skinnedDraw({1}, 0);
    require(resources.draws().at(0).bones->size() == 3 && (*resources.draws().at(0).bones)[0][12] == 1 &&
                (*resources.draws().at(0).bones)[2][12] == 0,
            "bones past those its draw names stay at identity");
    resources.finishFrame();
    usd::modBoundary(resources);
    require(skinnedDraw({}, 0) && resources.draws().at(0).bones->size() == 3 && resources.draws().at(0).bones->at(2)[12] == 0,
            "a replacement of a draw without bones stays in its bind pose");
    resources.finishFrame();
    usd::modBoundary(resources);
    skinnedDraw({10, 0, 10}, 0);
    skinnedDraw({10, 0, 10}, 0);
    require(resources.draws().size() == 1, "a draw repeated with the same pose is one instance");
    resources.finishFrame();
    usd::modBoundary(resources);
    skinnedDraw({10, 0, 10}, 0);
    skinnedDraw({50, 0, 50}, 0);
    require(resources.sphereLights().size() == 2 && resources.sphereLights()[0].id != resources.sphereLights()[1].id,
            "skinned instances sharing a transform are their own instances");
    resources.finishFrame();
    usd::stopMods(resources);
    std::filesystem::remove_all(dir);
    std::cout << "USD tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "USD resource failure\n";
    return 1;
  }
}

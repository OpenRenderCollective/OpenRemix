#include "usd/mods.h"
#include "usd/submission.h"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
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

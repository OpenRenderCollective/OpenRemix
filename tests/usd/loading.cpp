#include "usd/mods.h"
#include "usd/submission.h"
#include <chrono>
#include <cmath>
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
    usd::startMods(true);
    require(usd::configureMods(dir.string().c_str(), 1, 1, true) == REMIXAPI_ERROR_CODE_SUCCESS, "configure mods");
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

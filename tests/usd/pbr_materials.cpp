#include "forward/material.h"
#include "scene/fixtures.h"
#include "usd/loader.h"
#include "usd/mods.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace {
using namespace remix;
using namespace remix::test;
void image(const std::filesystem::path &path, std::array<uint8_t, 4> pixel) {
  // One linear RGBA texel in an ordinary DDS, like generated PBR assets.
  uint32_t header[32]{};
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
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char *>(header), sizeof(header));
  file.write(reinterpret_cast<const char *>(pixel.data()), pixel.size());
}
template <typename Ready>
void settle(NativeResources &resources, uint64_t instance, Ready ready) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  do {
    resources.finishFrame();
    usd::modBoundary(resources);
    resources.drawRetained(nullptr);
    if (ready(*resources.retained(instance)->records.front().material))
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < deadline);
  throw std::runtime_error(std::string("PBR material did not publish: ") +
                           usd::modDiagnostic());
}
} // namespace
void pbrMaterialTests() {
  using namespace remix;
  using namespace remix::test;
  const auto temporary =
      std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
  const auto directory =
      temporary / ("openremix-usd-pbr-" + std::to_string(getpid()));
  require(std::filesystem::create_directory(directory),
          "PBR fixture owns a fresh temporary directory");
  struct Cleanup {
    std::filesystem::path directory, parent;
    ~Cleanup() {
      // Only this freshly created direct child of the temporary folder belongs
      // to the test. Refuse recursive cleanup if that ownership has changed.
      std::error_code error;
      const auto resolved = std::filesystem::weakly_canonical(directory, error);
      if (!error && resolved.parent_path() == parent)
        std::filesystem::remove_all(directory, error);
    }
  } cleanup{directory, temporary};
  std::filesystem::create_directories(directory / "generated");
  const auto albedo = directory / "original.dds",
             normal = directory / "generated/normal.dds",
             roughness = directory / "generated/roughness.dds";
  image(albedo, {200, 100, 50, 255});
  image(normal, {128, 128, 255, 255});
  image(roughness, {64, 64, 64, 255});
  std::ofstream(directory / "generated/mod.usda") << R"USD(#usda 1.0
(metersPerUnit = 0.01; upAxis = "Z")
over "RootNode" {
over "Looks" {
over "mat_F112ECDA59AB324E" {
over "Shader" {
 uniform asset info:mdl:sourceAsset = @AperturePBR_Opaque.mdl@
 int inputs:encoding = 0
 asset inputs:normalmap_texture = @./normal.dds@ (colorSpace = "raw")
 asset inputs:reflectionroughness_texture = @./roughness.dds@ (colorSpace = "raw")
 float inputs:metallic_constant = 0.7
}
}
}
}
)USD";
  const auto deferred =
      usd::load({(directory / "generated/mod.usda").string(), .01, 2, true});
  require(deferred.textures.empty(),
          "Unused material-only replacements stay lazy");
  NativeResources resources;
  struct Stop {
    NativeResources &resources;
    ~Stop() { usd::stopMods(resources); }
  } stop{resources};
  const auto original =
      material(resources, 0x7788, .8f, .6f, .4f, 1, albedo.c_str(), 5);
  resources.setMaterialSourceHash(original, 0xF112ECDA59AB324Eull);
  const auto handle =
      resources.createRetained(instance(quad(resources, 0x7789, original)));
  const auto originalMaterial =
      resources.retained(handle)->records.front().material;
  const remixapi_BakeSceneInfoEXT request{sizeof(request),
                                          REMIXAPI_BAKE_SCENE_VERSION_EXT,
                                          7,
                                          1,
                                          1,
                                          REMIXAPI_BAKE_SCENE_READY_EXT,
                                          1,
                                          0,
                                          &handle,
                                          nullptr};
  resources.setBakeScene(&request);
  const auto before = resources.bakeScene();
  require(usd::configureMods(directory.string().c_str(), .01, 2, false) ==
              REMIXAPI_ERROR_CODE_SUCCESS,
          "Generated PBR replacement directory configures");
  settle(resources, handle,
         [](const auto &m) { return m.normalTexture && m.roughnessTexture; });
  const auto replaced = resources.retained(handle)->records.front();
  const auto &m = *replaced.material;
  require(m.texture == originalMaterial->texture &&
              std::equal(m.tint, m.tint + 4, originalMaterial->tint) &&
              m.compare == originalMaterial->compare &&
              m.reference == originalMaterial->reference,
          "A generated normal/roughness replacement preserves source albedo, "
          "tint and alpha state");
  require(!m.normalTexture->pixels.srgb && !m.roughnessTexture->pixels.srgb &&
              m.normalEncoding == 1,
          "Generated raw PBR textures and MDL normal encoding reach the shared "
          "scene correctly");
  const auto shading = forward::materialData(replaced);
  require((shading.maps[0] & 5) == 5 && shading.maps[1] == 1 &&
              shading.pbr[1] == .7f,
          "Forward+ consumes the resolved normal, roughness and metallic "
          "properties");
  resources.setBakeScene(&request);
  const auto baked = resources.bakeScene();
  require(baked->contentHash != before->contentHash &&
              baked->draws.front().material->normalTexture == m.normalTexture &&
              baked->draws.front().material->roughnessTexture ==
                  m.roughnessTexture,
          "The frozen baker receives the same PBR texture assets as the live "
          "shared scene");
  require(!before->draws.front().material->normalTexture,
          "Publishing a replacement cannot mutate an existing bake snapshot");
  image(roughness, {192, 192, 192, 255});
  usd::reloadMods();
  settle(resources, handle, [](const auto &value) {
    return value.roughnessTexture &&
           value.roughnessTexture->pixels.mips.front()[0] == 192;
  });
  resources.setBakeScene(&request);
  require(resources.bakeScene()->contentHash != baked->contentHash &&
              baked->draws.front()
                      .material->roughnessTexture->pixels.mips.front()[0] == 64,
          "Same-path texture edits invalidate new bakes while keeping the "
          "published snapshot immutable");
  usd::stopMods(resources);
  resources.finishFrame();
  resources.drawRetained(nullptr);
  require(resources.retained(handle)->records.front().material ==
              originalMaterial,
          "Disabling the shared mod restores the source material");
  std::puts("Generated PBR: lazy USD loading, albedo preservation, Forward+ "
            "inputs and immutable bake reload "
            "passed");
}

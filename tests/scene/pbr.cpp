#include "scene/fixtures.h"
#include "scene/temporal.h"
#include <limits>
using namespace remix::test;
int main(int argc, char **argv) {
  remix::NativeResources resources;
  const auto path = std::filesystem::path("0xa");
  uint8_t pixel[] = {128, 64, 255, 255};
  remixapi_TextureInfo texture{};
  texture.hash = 10;
  texture.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
  texture.width = texture.height = texture.depth = texture.mipLevels = 1;
  texture.data = pixel;
  texture.dataSize = 4;
  remixapi_TextureHandle th;
  resources.createTexture(texture, th);
  remixapi_MaterialInfoOpaqueEXT opaque{};
  opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.albedoConstant = {1, 1, 1};
  opaque.opacityConstant = 1;
  opaque.alphaTestType = 7;
  opaque.roughnessConstant = .7f;
  opaque.metallicConstant = .3f;
  opaque.roughnessTexture = opaque.metallicTexture = path.c_str();
  remixapi_MaterialInfo info{};
  info.hash = 20;
  info.pNext = &opaque;
  info.albedoTexture = info.normalTexture = info.emissiveTexture = path.c_str();
  info.emissiveColorConstant = {1, 1, 1};
  info.emissiveIntensity = 2;
  remixapi_MaterialHandle materialHandle;
  resources.createMaterial(info, materialHandle);
  auto mesh = quad(resources, 30, materialHandle);
  auto draw = instance(mesh);
  resources.drawInstance(draw);
  auto original = resources.draws().back().material;
  require(original->pbr && original->roughness == .7f &&
              original->metallic == .3f,
          "PBR constants missing");
  require(original->texture->pixels.srgb &&
              original->emissionTexture->pixels.srgb,
          "PBR altered color-space of colors");
  require(!original->normalTexture->pixels.srgb &&
              original->normalTexture->pixels.mips[0][0] == 128,
          "Normal map was gamma converted");
  require(original->normalTexture == original->roughnessTexture &&
              original->normalTexture == original->metallicTexture,
          "Data views not shared");
  resources.createMaterial(info, materialHandle);
  resources.drawInstance(draw);
  require(resources.draws().back().material == original,
          "Identical PBR material lost identity");
  auto signature = remix::scene::TemporalHistory::materialSignature(
      resources.draws().back());
  for (float bad : {-1.f, 1.1f, std::numeric_limits<float>::quiet_NaN(),
                    std::numeric_limits<float>::infinity()}) {
    opaque.roughnessConstant = bad;
    rejected([&] { resources.createMaterial(info, materialHandle); });
    opaque.roughnessConstant = .7f;
    opaque.metallicConstant = bad;
    rejected([&] { resources.createMaterial(info, materialHandle); });
    opaque.metallicConstant = .3f;
  }
  resources.drawInstance(draw);
  require(resources.draws().back().material == original,
          "Rejected PBR edit replaced material");
  pixel[0] = 200;
  resources.createTexture(texture, th);
  resources.createMaterial(info, materialHandle);
  resources.drawInstance(draw);
  const auto &changed = resources.draws().back();
  require(changed.material->roughnessTexture->pixels.mips[0][0] == 200 &&
              original->roughnessTexture->pixels.mips[0][0] == 128,
          "PBR upload lifetime");
  require(remix::scene::TemporalHistory::materialSignature(changed) !=
              signature,
          "PBR texture change absent from history signature");
  opaque.roughnessTexture = opaque.metallicTexture = nullptr;
  info.normalTexture = nullptr;
  opaque.roughnessConstant = opaque.metallicConstant = 0;
  resources.createMaterial(info, materialHandle);
  resources.drawInstance(draw);
  require(!resources.draws().back().material->pbr,
          "Legacy material became specular");
  remixapi_MaterialInfoPbrEXT pbr{};
  pbr.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_EXT;
  pbr.enabled = 1;
  opaque.pNext = &pbr;
  resources.createMaterial(info, materialHandle);
  resources.drawInstance(draw);
  require(resources.draws().back().material->pbr,
          "Smooth dielectric override missing");
  pbr.enabled = 0;
  opaque.roughnessConstant = 1;
  resources.createMaterial(info, materialHandle);
  resources.drawInstance(draw);
  require(!resources.draws().back().material->pbr && resources.draws().back().material->roughness == 1,
          "A rough Forward+ fallback must preserve explicit diffuse-only path tracing");
  pbr.enabled = 2;
  rejected([&] { resources.createMaterial(info, materialHandle); });
  if (argc == 2) {
    unsigned count = 0;
    opaque.pNext = nullptr;
    opaque.roughnessConstant = 1;
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(argv[1])) {
      if (!entry.is_regular_file() || entry.path().extension() != ".dds")
        continue;
      const auto stem = entry.path().stem().string();
      const auto file = entry.path();
      info.albedoTexture = info.normalTexture = info.emissiveTexture = nullptr;
      opaque.roughnessTexture = opaque.metallicTexture = nullptr;
      if (stem.ends_with("_normal"))
        info.normalTexture = file.c_str();
      else if (stem.ends_with("_roughness"))
        opaque.roughnessTexture = file.c_str();
      else if (stem.ends_with("_metallic"))
        opaque.metallicTexture = file.c_str();
      else if (stem.ends_with("_emissive"))
        info.emissiveTexture = file.c_str();
      else
        continue;
      resources.createMaterial(info, materialHandle);
      resources.drawInstance(draw);
      const auto first = resources.draws().back().material;
      resources.createMaterial(info, materialHandle);
      resources.drawInstance(draw);
      require(first == resources.draws().back().material,
              "File-backed map reload lost material identity");
      resources.finishFrame();
      ++count;
    }
    require(count != 0, "No existing BetaRT DDS maps found");
    std::printf("Loaded %u existing BetaRT DDS maps\n", count);
  }
  std::puts("PBR maps, linear data, validation, snapshots and legacy "
            "compatibility passed");
}

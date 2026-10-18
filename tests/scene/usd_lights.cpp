#include "scene/blackbody.h"
#include "scene/images.h"
#include "scene/resources.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <remix/remix_native_ext.h>
#include <stdexcept>

void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main() {
  try {
    remix::NativeResources resources;
    {
      remix::NativeResources hinted;
      remixapi_LightInfoBakingEXT baking{REMIXAPI_STRUCT_TYPE_LIGHT_BAKING_EXT, nullptr,
                                       REMIXAPI_LIGHT_BAKING_PREFER_BAKED_EXT};
      remixapi_LightInfoSphereEXT sphere{};
      sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
      sphere.pNext = &baking;
      sphere.radius = 1;
      remixapi_LightInfo input{};
      input.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
      input.pNext = &sphere;
      input.hash = 12345;
      input.radiance = {1, 2, 3};
      remixapi_LightHandle light{};
      hinted.createLight(input, light);
      baking.hint = REMIXAPI_LIGHT_BAKING_REQUIRE_MIXED_EXT;
      hinted.drawLight(light);
      require(hinted.sphereLights().front().bakeHint == REMIXAPI_LIGHT_BAKING_PREFER_BAKED_EXT,
              "Light baking hint is copied by value from an optional chain");
      hinted.updateLight(light, input);
      require(hinted.sphereLights().front().bakeHint == REMIXAPI_LIGHT_BAKING_REQUIRE_MIXED_EXT,
              "Authored mixed policy survives a live light update");
      baking.hint = remixapi_LightBakingHintEXT(99);
      bool rejected = false;
      try { hinted.updateLight(light, input); }
      catch (remixapi_ErrorCode error) { rejected = error == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS; }
      require(rejected && hinted.sphereLights().front().bakeHint == REMIXAPI_LIGHT_BAKING_REQUIRE_MIXED_EXT,
              "Invalid baking hints cannot replace a valid light");
      remixapi_LightInfoUSDEXT replacement{};
      replacement.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_USD_EXT;
      const auto replaced = hinted.overrideLight(light, 12346, replacement, false);
      hinted.drawLight(replaced);
      for (const auto &emitter : hinted.sphereLights())
        require(emitter.bakeHint == REMIXAPI_LIGHT_BAKING_REQUIRE_MIXED_EXT,
                "USD asset replacement preserves authored light policy");
    }
    remixapi_LightInfoUSDEXT usd{};
    usd.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_USD_EXT;
    for (int c = 0; c < 3; ++c)
      usd.transform.matrix[c][c] = 1;
    usd.transform.matrix[0][3] = 3;
    usd.transform.matrix[1][3] = 4;
    remixapi_Float3D color{1, .25f, 0};
    float intensity = 4, exposure = 2, radius = 2, width = 6, height = 4, length = 8, cone = .5f,
          softness = .3f, focus = 2;
    usd.pColor = &color;
    usd.pIntensity = &intensity;
    usd.pExposure = &exposure;
    usd.pRadius = &radius;
    usd.pWidth = &width;
    usd.pHeight = &height;
    usd.pLength = &length;
    remixapi_LightInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.hash = 11;
    info.pNext = &usd;
    remixapi_LightHandle handle{};
    for (auto type :
         {REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT, REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT,
          REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT, REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT}) {
      usd.lightType = type;
      resources.createLight(info, handle);
      resources.drawLight(handle);
      auto lights = resources.sphereLights();
      require(!lights.empty(), "USD area light conversion");
      for (const auto &light : lights) {
        require(light.radiance[0] == 16 && light.radiance[1] == 4 && light.radiance[2] == 0,
                "color intensity and exposure");
        require(light.radius > 0, "area dimensions");
      }
      resources.finishFrame();
    }
    usd.lightType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    usd.pConeAngleRadians = &cone;
    usd.pConeSoftness = &softness;
    usd.pFocus = &focus;
    resources.createLight(info, handle);
    resources.drawLight(handle);
    auto sphere = resources.sphereLights()[0];
    require(std::abs(sphere.coneCos - std::cos(cone)) < 1e-6f && sphere.direction[2] == -1 &&
                sphere.softness == softness && sphere.focus == focus,
            "spot shape and direction");
    resources.finishFrame();
    usd.lightType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    resources.createLight(info, handle);
    resources.drawLight(handle);
    require(resources.light().isDistant && resources.light().direction[2] == -1 &&
                resources.light().radiance[0] == 16,
            "USD distant conversion");
    resources.finishFrame();
    remixapi_LightInfoDomeEXT dome{};
    dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
    for (int c = 0; c < 3; ++c)
      dome.transform.matrix[c][c] = 1;
    info.pNext = &dome;
    info.radiance = {2, 3, 4};
    resources.createLight(info, handle);
    resources.drawLight(handle);
    require(resources.dome().enabled && resources.dome().radiance[2] == 4, "dome radiance");
    resources.finishFrame();
    require(!resources.dome().enabled, "dome retirement");
    for (float kelvin : {1000.f, 3000.f, 6500.f, 10000.f}) {
      auto rgb = remix::scene::blackbody(kelvin);
      require(std::abs(.2126f * rgb[0] + .7152f * rgb[1] + .0722f * rgb[2] - 1) < 1e-4f,
              "temperature preserves luminance");
    }
    uint8_t block[16] = {255, 0};
    remixapi_TextureInfo texture{};
    texture.width = texture.height = 4;
    texture.depth = texture.mipLevels = 1;
    texture.data = block;
    texture.dataSize = 8;
    texture.format = REMIXAPI_FORMAT_BC4_UNORM_EXT;
    auto scalar = remix::scene::decodeTexture(texture);
    require(scalar.mips[0][0] == 255 && scalar.mips[0][1] == 255 && scalar.mips[0][3] == 255,
            "BC4 scalar channels");
    texture.dataSize = 16;
    texture.format = REMIXAPI_FORMAT_BC6H_UFLOAT_EXT;
    uint32_t rng = 123;
    bool hdr = false;
    for (unsigned i = 0; i < 256 && !hdr; ++i) {
      for (auto &b : block) {
        rng = rng * 1664525 + 1013904223;
        b = uint8_t(rng >> 24);
      }
      auto image = remix::scene::decodeTexture(texture);
      require(image.hdrMips.size() == 1 && image.hdrMips[0].size() == 64, "BC6H retains float pixels");
      hdr = *std::max_element(image.hdrMips[0].begin(), image.hdrMips[0].end()) > 1;
    }
    require(hdr, "BC6H preserves values above one");
    std::cout << "USD lights and HDR assets passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  } catch (...) {
    return 1;
  }
}

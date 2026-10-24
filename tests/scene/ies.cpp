#include "scene/ies.h"
#include "scene/resources.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <remix/remix_native_ext.h>

namespace {
void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}
bool approx(float a, float b) { return std::abs(a - b) < 1e-4f; }
// 0, 45 and 90 degrees from the axis; two horizontal planes that average to
// 100, 50 and 0 candela.
const std::string spot = "IESNA:LM-63-2002\r\n"
                         "[TEST] spot\r\n"
                         "TILT=NONE\r\n"
                         "1 1000 1 3 2 1 1 0 0 0\r\n"
                         "1 1 10\r\n"
                         "0 45 90\r\n"
                         "0 180\r\n"
                         "120 60 0\r\n"
                         "80 40 0\r\n";
} // namespace

int main() {
  using namespace remix::scene;
  {
    IesTable table{};
    require(parseIes(spot, table), "A plain LM-63 profile was rejected");
    require(approx(table[0], 1), "The axis did not hold the profile's peak");
    // Bin 85 lies at 60 degrees: a third of the way from 45 (0.5) to 90 (0).
    require(approx(table[85], .5f * 2 / 3), "Angles between the file's were not interpolated");
    require(approx(table[kIesBins - 1], 0) && approx(table[160], 0), "Light was emitted beyond the file's angles");
  }
  {
    // Tilt data sits between the TILT line and the photometric header.
    std::string tilted = spot;
    tilted.replace(tilted.find("TILT=NONE"), 9, "TILT=INCLUDE\r\n1\r\n2\r\n0 90\r\n1 1");
    IesTable table{};
    require(parseIes(tilted, table) && approx(table[0], 1), "Tilt data was not skipped");
  }
  {
    IesTable table{};
    require(!parseIes("not a profile", table), "Text without a TILT line was accepted");
    require(!parseIes(spot.substr(0, spot.size() - 8), table), "A truncated profile was accepted");
    std::string dark = spot;
    dark.replace(dark.find("120 60 0"), 8, "0 0 0");
    dark.replace(dark.find("80 40 0"), 7, "0 0 0");
    require(!parseIes(dark, table), "A profile emitting nothing was accepted");
  }
  {
    // Lights share a profile by its text; the axis is the shaping direction,
    // else the light's own.
    remix::NativeResources resources;
    remixapi_LightInfoIesProfileEXT ies{};
    ies.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_IES_PROFILE_EXT;
    ies.data = spot.data();
    ies.size = spot.size();
    remixapi_LightInfoSphereEXT sphere{};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.pNext = &ies;
    sphere.radius = .1f;
    remixapi_LightInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.pNext = &sphere;
    info.hash = 1;
    info.radiance = {10, 10, 10};
    remixapi_LightHandle first{}, second{};
    resources.createLight(info, first);
    sphere.shaping_hasvalue = 1;
    sphere.shaping_value.direction = {1, 0, 0};
    sphere.shaping_value.coneAngleDegrees = 180;
    info.hash = 2;
    resources.createLight(info, second);
    resources.drawLight(first);
    resources.drawLight(second);
    const auto lights = resources.sphereLights();
    require(lights.size() == 2, "Profiled lights were not emitted");
    require(resources.iesProfiles() && resources.iesProfiles()->size() == kIesBins,
            "Lights with the same profile text did not share it");
    for (const auto &light : lights) {
      require(light.ies == 1, "A light lost its profile");
      const bool shaped = light.id == 2;
      require(approx(light.direction[0], shaped ? 1 : 0) && approx(light.direction[2], shaped ? 0 : -1),
              "A profile's axis was not the shaping direction, else straight down");
    }
    bool failed = false;
    try {
      ies.size = 5;
      info.hash = 3;
      remixapi_LightHandle third{};
      resources.createLight(info, third);
    } catch (...) {
      failed = true;
    }
    require(failed, "A light with a malformed profile was accepted");
  }
  {
    // Light cookies: a slot per texture, the projection's up made
    // perpendicular to the axis, sphere lights only.
    remix::NativeResources resources;
    remixapi_LightInfoCookieEXT cookie{};
    cookie.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_COOKIE_EXT;
    cookie.texture = nullptr; // the white texture
    cookie.up = {0, 1, 1};
    cookie.fovDegrees = 90;
    remixapi_LightInfoSphereEXT sphere{};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.pNext = &cookie;
    sphere.radius = .1f;
    sphere.shaping_hasvalue = 1;
    sphere.shaping_value.direction = {0, 0, -1};
    sphere.shaping_value.coneAngleDegrees = 180;
    remixapi_LightInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.pNext = &sphere;
    info.hash = 1;
    info.radiance = {10, 10, 10};
    remixapi_LightHandle first{}, second{};
    resources.createLight(info, first);
    info.hash = 2;
    resources.createLight(info, second);
    resources.drawLight(first);
    resources.drawLight(second);
    require(resources.cookieTextures().size() == 1, "Cookies with one texture did not share a slot");
    for (const auto &light : resources.sphereLights()) {
      require(light.cookie == 1, "A light lost its cookie");
      require(approx(light.cookieUp[0], 0) && approx(light.cookieUp[1], 1) && approx(light.cookieUp[2], 0),
              "The cookie's up was not made perpendicular to the axis");
      require(approx(light.cookieTan, 1), "The cookie's width was not its field of view");
    }
    auto rejected = [&](const char *message) {
      bool failed = false;
      try {
        remixapi_LightHandle other{};
        info.hash = 9;
        resources.createLight(info, other);
      } catch (...) {
        failed = true;
      }
      require(failed, message);
    };
    cookie.up = {0, 0, 2};
    rejected("A cookie whose up lies along the axis was accepted");
    cookie.up = {0, 1, 0};
    cookie.fovDegrees = 180;
    rejected("A cookie spanning 180 degrees was accepted");
  }
  {
    // A light's volumetric radiance scale reaches its emitter; negative
    // scales are rejected.
    remix::NativeResources resources;
    remixapi_LightInfoSphereEXT sphere{};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.radius = .1f;
    sphere.volumetricRadianceScale = 1;
    remixapi_LightInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.pNext = &sphere;
    info.hash = 1;
    info.radiance = {10, 10, 10};
    remixapi_LightHandle plain{}, scaled{};
    resources.createLight(info, plain);
    sphere.volumetricRadianceScale = 4;
    info.hash = 2;
    resources.createLight(info, scaled);
    resources.drawLight(plain);
    resources.drawLight(scaled);
    for (const auto &light : resources.sphereLights())
      require(approx(light.volumetric, light.id == 2 ? 4 : 1), "A light's volumetric scale was not its own");
    bool failed = false;
    try {
      sphere.volumetricRadianceScale = -1;
      info.hash = 3;
      remixapi_LightHandle negative{};
      resources.createLight(info, negative);
    } catch (...) {
      failed = true;
    }
    require(failed, "A negative volumetric scale was accepted");
    // The sun's scale reaches the frame with its light.
    remixapi_LightInfoDistantEXT sun{};
    sun.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    sun.direction = {0, 0, -1};
    sun.angularDiameterDegrees = .5f;
    sun.volumetricRadianceScale = 3;
    info.pNext = &sun;
    info.hash = 4;
    remixapi_LightHandle sunHandle{};
    resources.createLight(info, sunHandle);
    resources.drawLight(sunHandle);
    require(approx(resources.light().volumetric, 3), "The sun's volumetric scale was lost");
  }
  return 0;
}

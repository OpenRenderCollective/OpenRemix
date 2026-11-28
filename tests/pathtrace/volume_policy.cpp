#include "pathtrace/volume_policy.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
  bool approximately(float a, float b) { return std::abs(a - b) <= 1e-3f * std::max(std::abs(a), std::abs(b)); }
} // namespace

int main() {
  using namespace remix;
  using namespace remix::trace;
  TraceOptions options; // the game's fog: the defaults are volumetrics off, the override on
  options.volumetrics = true;
  options.volumetricOverride = false;
  GameFog fog;
  // No fog from the game and no override: no medium.
  require(!volumeMedium(options, fog).active, "No fog, no medium");
  // Exponential fog is the extinction itself.
  fog.mode = 1;
  fog.density = .002f;
  auto medium = volumeMedium(options, fog);
  require(medium.active && approximately(medium.extinction, .002f), "Exponential fog density is the extinction");
  // Linear fog and squared-exponential fog leave 5% where their curve does.
  fog.mode = 3;
  fog.end = 6000;
  medium = volumeMedium(options, fog);
  require(medium.active && approximately(std::exp(-medium.extinction * 6000), volumeVisibility), "Linear fog ends at 5%");
  fog.mode = 2;
  fog.density = .001f;
  medium = volumeMedium(options, fog);
  const float distance = std::sqrt(-std::log(volumeVisibility)) / .001f;
  require(approximately(std::exp(-medium.extinction * distance), volumeVisibility), "exp2 fog matches at its 5% distance");
  // The fog colour tints the scattering; its level is left to the lights.
  fog.color[0] = .5f;
  fog.color[1] = .25f;
  fog.color[2] = 0;
  medium = volumeMedium(options, fog);
  require(medium.albedo[0] == 1 && medium.albedo[1] > 0 && medium.albedo[1] < .5f && medium.albedo[2] == 0,
          "Fog colour is normalized to its brightest channel, in linear light");
  fog.color[0] = fog.color[1] = 0;
  require(volumeMedium(options, fog).albedo[1] == 1, "Black fog scatters white");
  // A fog mode without its parameter is no fog.
  fog.mode = 3;
  fog.end = 0;
  require(!volumeMedium(options, fog).active, "Linear fog without an end is no fog");
  // The override replaces the game's fog: meters, through the scene scale.
  options.volumetricOverride = true;
  options.volumetricDistance = 100;
  options.sceneScale = .0254f; // inches
  options.volumetricColor[2] = .5f;
  medium = volumeMedium(options, fog);
  require(medium.active && approximately(std::exp(-medium.extinction * 100 / .0254f), volumeVisibility),
          "Override distance is in meters");
  require(medium.albedo[2] == .5f, "Override colour");
  options.volumetrics = false;
  require(!volumeMedium(options, fog).active, "Volumetrics off");
  // Grid sizes stay within what the buffers are sized for.
  options.volumetricCells = 47;
  require(volumeCells(options) == 46, "Cells per side are even");
  options.volumetricCells = 1000;
  options.volumetricLevels = 0;
  require(volumeCells(options) == 96 && volumeLevels(options) == 1, "Grid size limits");
  // The defaults are the Ultra quality and Immediate response presets.
  const TraceOptions defaults;
  require(defaults.volumetricCellSize == volumeQualities[3].cellSize &&
              defaults.volumetricCells == volumeQualities[3].cells &&
              defaults.volumetricLevels == volumeQualities[3].levels &&
              defaults.volumetricSteps == volumeQualities[3].steps,
          "Default quality is the Ultra preset");
  require(defaults.volumetricUpdateInterval == volumeResponses[3].interval &&
              defaults.volumetricHistory == volumeResponses[3].history,
          "Default response is the Immediate preset");
  // Raster fog's glow: a sphere light's slot carries its power for the
  // sample budget's shares, and the header the budget and the moving jitter.
  {
    TraceOptions glowOptions;
    glowOptions.volumetrics = glowOptions.volumetricRaster = true;
    glowOptions.volumetricRasterGlowSun = glowOptions.volumetricRasterGlowSky = false;
    glowOptions.volumetricLightScale = 2;
    glowOptions.volumetricRasterBudget = 24;
    std::vector<scene::LightEmitter> emitters(1);
    auto &e = emitters[0];
    e.positionRadius[0] = 3;
    e.positionRadius[3] = .5f;
    e.radiance[0] = e.radiance[1] = e.radiance[2] = 4;
    e.corners[1][3] = 1;
    const float eye[3] = {}, forward[3] = {1, 0, 0}, sun[3] = {0, 0, -1}, sunRadiance[4] = {};
    std::vector<std::pair<float, uint32_t>> scored;
    auto header = std::make_unique<RasterGlowHeader>();
    RasterGlowFrame glow;
    require(rasterGlowFrame(glowOptions, emitters, {}, true, eye, forward, sun, sunRadiance, 10, scored, *header, glow) &&
                glow.lights == 1,
            "One sphere light glows");
    require(approximately(header->lights[0].power, 4 * .25f * 3.14159265f * 2), "A glow slot carries its power");
    require(header->budget == 24 && header->temporal == 1, "The header carries the budget and the moving jitter");
    glowOptions.volumetricRasterBudget = 5000;
    glowOptions.volumetricRasterTemporal = false;
    rasterGlowFrame(glowOptions, emitters, {}, true, eye, forward, sun, sunRadiance, 10, scored, *header, glow);
    require(header->budget == 1024 && header->temporal == 0, "The budget is capped; the jitter can hold still");
  }
  return 0;
}

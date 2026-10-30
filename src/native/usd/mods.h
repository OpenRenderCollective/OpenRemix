#pragma once
#include <remix/remix_c.h>
#include <string>
#include <utility>
#include <vector>
namespace remix {
  class NativeResources;
}
namespace remix::usd {
  void startMods(bool disabled);
  void stopMods(NativeResources &);
  void modBoundary(NativeResources &);
  bool modInstance(NativeResources &, const remixapi_InstanceInfo &, uint64_t originalHash = 0);
  bool modLight(NativeResources &, remixapi_LightHandle, uint64_t originalHash = 0);
  remixapi_ErrorCode configureMods(const char *directory, double meters, uint32_t axis, bool watch);
  const char *modDiagnostic();
  // Host units mods are converted to (configureMods): meters per unit and up
  // axis (1 Y, 2 Z). USD captures are written in them.
  std::pair<double, uint32_t> modUnits();

  // Developer panel view of the loaded mods. Toggles keep every mod loaded:
  // they change which replacements apply from the next frame on.
  // What of a mod applies.
  struct ModParts {
    bool enabled = true;
    bool meshes = true;         // replacement geometry
    bool lights = true;         // lights authored under mesh replacements
    bool lightOverrides = true; // game light replacements
    bool materials = true;
    bool keepOriginal = false;  // draw the game's mesh beside its replacement
    bool operator==(const ModParts &) const = default;
  };
  struct ModInfo {
    std::string name; // mod directory relative to the mods directory
    size_t meshes = 0, lights = 0, lightOverrides = 0, materials = 0;
    size_t missingTextures = 0; // left out of their materials (not found or not decodable)
    ModParts parts;
  };
  struct ModStatus {
    bool active = false;  // mod loading started
    bool enabled = true;  // replacements applied at all
    bool loading = false; // a parse is in flight
    std::string directory, diagnostic;
    std::vector<ModInfo> mods;
  };
  ModStatus modStatus();
  void setModsEnabled(bool enabled);
  void setModEnabled(const std::string &name, bool enabled);
  void setModParts(const std::string &name, const ModParts &parts);
  void reloadMods();
} // namespace remix::usd

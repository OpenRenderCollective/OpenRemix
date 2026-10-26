#include "runtime/profiles.h"
#include "ui/profiles.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>

using namespace remix;
namespace fs = std::filesystem;
static void require(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
static std::string read(const fs::path &file) {
  std::ifstream input(file);
  std::ostringstream out;
  out << input.rdbuf();
  return out.str();
}
static void write(const fs::path &file, const std::string &text) { std::ofstream(file) << text; }

int main(int argc, char **argv) {
  require(argc == 2, "Expected isolated test directory");
  const fs::path root = argv[1];
  fs::create_directories(root);
  config::Settings changed;
  changed.backend = 0;
  changed.vsync = true;
  changed.clearColor = {.1f, .2f, .3f, .4f};
  auto &t = changed.trace;
  t.samples = 7;
  t.bounces = 4;
  t.russianRoulette = false;
  t.rouletteMinBounces = 3;
  t.accumulate = true;
  t.forceRebuild = true;
  t.denoiser = false;
  t.taa = false;
  t.validation = true;
  t.upscaler = upscale::Mode::Fsr31;
  t.dlssRayReconstruction = false;
  t.dlssQuality = 2;
  t.autoExposure = false; t.bloom = false;
  t.exposure = 2.3f; t.autoExposureTarget = .13f; t.autoExposureSpeed = 3.7f;
  t.autoExposureCentre = .62f; t.autoExposureMinEv = -10; t.autoExposureMaxEv = 5;
  t.bloomIntensity = .21f; t.bloomRadius = .74f;
  t.fsrQuality = 4;
  t.fsrSharpness = .37f;
  t.sigma = false;
  t.blueNoiseShadows = false;
  t.blueNoiseLocalLights = false;
  t.localLights = false;
  t.localDenoiser = false;
  t.torchLights = false;
  t.emissiveLights = false;
  t.emissiveCandidates = 6;
  t.adaptiveLocalSampling = true;
  t.stabilizeEmissiveLighting = true;
  t.emissiveSolidAngleSampling = true;
  t.freezeEmissiveRadiance = true;
  t.separateLocalLighting = 2;
  t.localSamples = 9;
  t.localIntensity = 2.27f;
  t.localRange = .01f;
  t.localDirectClamp = 13.4f;
  t.indirectClamp = 0.01f;
  t.localFireflyFilter = false;
  t.localGuiding = false;
  t.shadowDebug = 2;
  t.debugView = 11;
  t.sunAngularDiameter = 1.2f;
  t.rasterPrimary = false;
  t.waterIndirectReuse = false;
  t.translucencyDenoiser = false;
  t.translucencyReuse = false;
  t.pbrLocalReuse = false;
  t.sky = false;
  t.sharc = false;
  t.sharcDebug = true;
  t.sharcCapacityLog2 = 18;
  t.sharcDownscale = 2;
  t.sharcSceneScale = 13;
  t.sharcAccumulationFrames = 48;
  t.sharcStaleFrames = 81;
  t.sharcMotionFrames = 7;
  t.sharcFireflyClamp = 3.5f;
  t.sharcMaxUpdatePaths = 12345;
  t.sharcDynamicSurfaces = false;
  t.sharcGlossySurfaces = false;
  t.overrideLight = true;
  t.lightDirection[0] = -.7f;
  t.lightDirection[1] = .2f;
  t.lightDirection[2] = .9f;
  t.lightColor[0] = .123f;
  t.lightColor[1] = .45f;
  t.lightColor[2] = .67f;
  t.lightIntensity = 7.85f;
  config::Settings parsed;
  config::UnknownSettings unknown;
  std::vector<std::string> warnings;
  std::string error;
  require(config::validate(changed, error), error.c_str());
  require(config::parse(config::serialize(changed), parsed, unknown, warnings, error), error.c_str());
  require(changed == parsed, "Complete nondefault snapshot did not round-trip");
  require(config::serialize(changed).find("sharcReset") == std::string::npos,
          "One-shot action was serialized");
  require(
      config::parse("profile.version = 1\nrtx.native.pathtrace.samples = 5\nfuture.setting = alpha = beta\n",
                    parsed, unknown, warnings, error),
      "Partial profile rejected");
  config::Settings partial;
  partial.trace.samples = 5;
  require(parsed == partial, "Omitted keys leaked previous values");
  require(parsed.trace.translucencyDenoiser && parsed.trace.translucencyReuse,
          "Translucent denoising and opaque reuse must default on");
  require(unknown.at("future.setting") == "alpha = beta" && warnings.size() == 1,
          "Unknown key was not retained/warned");
  require(config::serialize(parsed, unknown).find("future.setting = alpha = beta") != std::string::npos,
          "Unknown key lost on save");
  for (const auto &bad :
       {"profile.version = 2\n", "rtx.native.vsync = true\n", "profile.version = 1\nbad line\n",
        "profile.version = 1\nrtx.native.pathtrace.samples = 17\n",
        "profile.version = 1\nrtx.native.pathtrace.localRange = 0.001\n",
        "profile.version = 1\nrtx.native.pathtrace.lightIntensity = nan\n",
        "profile.version = 1\nrtx.native.clearColor.r = inf\n",
        "profile.version = 1\nrtx.native.pathtrace.upscaler = unknown\n",
        "profile.version = 1\nprofile.version = 1\n"}) {
    const auto before = parsed;
    const auto extra = unknown;
    const auto notices = warnings;
    require(!config::parse(bad, parsed, unknown, warnings, error), "Invalid profile accepted");
    require(parsed == before && unknown == extra && warnings == notices, "Invalid load mutated output");
  }
  require(config::parse("\xef\xbb\xbfprofile.version = 1\r\nrtx.native.pathtrace.samples = 5\r\n", parsed,
                        unknown, warnings, error) &&
              parsed == partial,
          "UTF-8 BOM/CRLF file rejected");
  require(config::set(parsed, "rtx.native.vsync", "TrUe") == config::SetResult::Success && parsed.vsync,
          "VSync case-insensitive API semantics changed");
  require(config::set(parsed, "rtx.native.pathtrace.samples", "true") == config::SetResult::Success &&
              parsed.trace.samples == 1,
          "Integer aliases changed");
  require(config::set(parsed, "rtx.native.pathtrace.dlssRayReconstruction", "true") ==
              config::SetResult::Invalid,
          "Strict RR boolean changed");
  require(config::set(parsed, "rtx.native.pathtrace.localRange", ".01") == config::SetResult::Success,
          "Minimum light range rejected");
  require(config::set(parsed, "rtx.native.pathtrace.sunAngularDiameter", "-1") == config::SetResult::Success,
          "Inherited sun diameter rejected");
  require(config::parse("profile.version = 1\nrtx.native.pathtrace.sharcReset = 1\n", parsed, unknown,
                        warnings, error) &&
              !parsed.trace.sharcReset && unknown.empty(),
          "File replayed one-shot action");

  config::Profiles profiles;
  profiles.startup(root);
  require(profiles.active() == "Default" && !profiles.dirty() && profiles.files().empty(),
          "Initial default state");
  require(profiles.edit(changed) && profiles.dirty() && profiles.save() && !profiles.dirty(),
          "Save Default snapshot");
  const auto original = read(root / "profiles/Default.conf");
  auto edited = changed;
  edited.trace.samples = 11;
  require(profiles.edit(edited) && profiles.duplicate("Edited copy"), "Duplicate failed");
  require(profiles.current() == edited && !profiles.dirty() && profiles.active() == "Edited copy",
          "Duplicate omitted unsaved edits");
  require(read(root / "profiles/Default.conf") == original, "Duplicate changed original file");
  for (const auto &name :
       {"", "..", "../escape", "a/b", "a\\b", "con", "trailing.", "bad:name", "Edited copy", "edited COPY"})
    require(!profiles.duplicate(name), "Invalid or duplicate profile name accepted");
  config::Profiles restored;
  restored.startup(root);
  require(restored.current() == edited && restored.active() == "Edited copy",
          "Startup did not restore last successful selection");
  require(profiles.load("Default") && profiles.current() == changed, "Switch leaked edited values");
  write(root / "profiles/Partial.conf",
        "profile.version = 1\nrtx.native.pathtrace.samples = 5\nfuture.setting = keep\n");
  write(root / "outside.conf", "profile.version = 1\n");
  fs::create_directories(root / "profiles/nested");
  write(root / "profiles/nested/Hidden.conf", "profile.version = 1\n");
  profiles.refresh();
  require(profiles.files().size() == 3, "Refresh must only list local profile files");
  require(profiles.load("Partial") && profiles.current() == partial && profiles.save(),
          "Partial load/save failed");
  require(read(root / "profiles/Partial.conf").find("future.setting = keep") != std::string::npos,
          "Save dropped unknown data");
  require(profiles.create("New") && profiles.current() == config::Settings{} && profiles.dirty(),
          "New did not start from defaults");
  restored.startup(root);
  require(restored.active() == "Partial", "Unsaved new profile changed startup selection");

  ui::ProfileActions actions;
  require(!actions.request(profiles, ui::ProfileAction::Load, "Default"), "Dirty load did not ask");
  require(actions.resolve(profiles, ui::UnsavedChoice::Cancel) && profiles.active() == "New",
          "Cancel changed profile");
  actions.request(profiles, ui::ProfileAction::Load, "Default");
  require(actions.resolve(profiles, ui::UnsavedChoice::Save) && profiles.current() == changed,
          "Save then load failed");
  require(fs::exists(root / "profiles/New.conf"), "Save-before-load omitted original profile");
  profiles.edit(edited);
  actions.request(profiles, ui::ProfileAction::Revert);
  require(actions.resolve(profiles, ui::UnsavedChoice::Discard) && profiles.current() == changed &&
              !profiles.dirty(),
          "Discard/revert failed");
  profiles.edit(edited);
  actions.request(profiles, ui::ProfileAction::New, "Fresh");
  require(actions.resolve(profiles, ui::UnsavedChoice::Discard) && profiles.current() == config::Settings{},
          "Discard/new leaked settings");
  require(profiles.load("Default"), "Load before failed write");
  profiles.edit(edited);
  fs::rename(root / "profiles", root / "saved-profiles");
  write(root / "profiles", "directory blocked");
  actions.request(profiles, ui::ProfileAction::Load, "Partial");
  require(!actions.resolve(profiles, ui::UnsavedChoice::Save) && profiles.dirty() && profiles.hasError(),
          "Failed save discarded edits or pending action");
  require(actions.pending == ui::ProfileAction::Load && profiles.current() == edited,
          "Failed save switched profile");
  require(read(root / "saved-profiles/Default.conf") == original, "Failed save damaged previous file");
  fs::remove(root / "profiles");
  fs::rename(root / "saved-profiles", root / "profiles");
  actions.resolve(profiles, ui::UnsavedChoice::Cancel);
  write(root / "profiles/Invalid.conf", "profile.version = 999\n");
  require(!profiles.load("Invalid") && profiles.current() == edited && profiles.active() == "Default",
          "Invalid load replaced current profile");
  write(root / "state.conf", "lastProfile = Invalid\n");
  restored.startup(root);
  require(restored.current() == config::Settings{} && restored.hasError(),
          "Invalid startup did not fall back with diagnostic");
  write(root / "state.conf", "lastProfile = Missing\n");
  restored.startup(root);
  require(restored.current() == config::Settings{} && restored.hasError(), "Missing startup fallback");
  profiles.savePanel({true, 680, 3});
  restored.startup(root);
  require(restored.panel() == config::PanelState{true, 680, 3}, "Panel state not restored separately");
  require(read(root / "profiles/Default.conf") == original, "Panel state saved unsaved rendering edits");
  auto invalid = changed;
  invalid.trace.lightColor[0] = std::numeric_limits<float>::infinity();
  require(!profiles.edit(invalid) && profiles.current() == edited, "Invalid UI snapshot partially applied");
  {
    // Host defaults (remixapi_SetConfigDefaultEXT): what a saved profile lists
    // stays; what it does not list, and an unsaved or new profile, take them.
    const fs::path dir = root / "defaults";
    fs::create_directories(dir / "profiles");
    write(dir / "profiles" / "Mine.conf", "profile.version = 1\nrtx.native.pathtrace.restir = 0\n");
    write(dir / "state.conf", "lastProfile = Mine\n");
    config::Profiles p;
    p.startup(dir);
    require(p.active() == "Mine", "Defaults: startup profile");
    require(p.setDefault("rtx.native.pathtrace.restir", "1") && !p.current().trace.restir,
            "A saved value outlives a host default");
    require(p.setDefault("rtx.native.pathtrace.localRange", "0") && p.current().trace.localRange == 0 && !p.dirty(),
            "A host default fills what a profile does not list, unsaved changes aside");
    require(!p.setDefault("rtx.native.pathtrace.noSuchSetting", "1"), "Unknown default rejected");
    require(p.create("Fresh") && p.current().trace.restir && p.current().trace.localRange == 0,
            "A new profile starts from the host defaults");
    require(p.load("Mine") && !p.current().trace.restir && p.current().trace.localRange == 0,
            "A loaded profile keeps its own values over the defaults");
  }
  return 0;
}

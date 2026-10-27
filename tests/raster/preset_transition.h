// Called with an active live-GI/ray-reflection renderer. Preserve the bake
// controller across target replacement, then prove stable preset reapplication.
template <class Render>
void source2Transition(Target &target, remix::forward::Options requested, Render render, size_t pixel) {
  namespace presets = remix::forward::presets;
  require(requested.rtGI && requested.rtReflections && target.forward->statistics().rayGI.active &&
              target.forward->statistics().rayReflections.available,
          "Preset transition begins with live GI and an available ray-reflection path");
  const auto controller = target.forward->bakeController();
  const auto previousVolume = controller->volume();
  requested.baking.reflectionProbes = 1;
  requested.baking.reflectionResolution = 8;
  const auto quality = requested;
  presets::applySource2(requested);
  require(!requested.rtGI && !requested.rtReflections && requested.ssr &&
              requested.samples == quality.samples && requested.baking.samples == quality.baking.samples &&
              requested.baking.reflectionResolution == quality.baking.reflectionResolution,
          "Source 2 replaces live techniques while retaining the chosen quality budgets");
  target.enableForward(requested, true);
  require(target.forward->bakeController() == controller,
          "Preset target replacement retains the fenced bake controller");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  std::vector<uint8_t> image;
  do {
    image = render();
    const auto &stats = target.forward->statistics();
    if (stats.baking.state == REMIXAPI_BAKE_READY_EXT && controller->volume() != previousVolume) break;
    require(stats.baking.state != REMIXAPI_BAKE_FAILED_EXT, "Source 2 replacement bake succeeds");
    require(std::chrono::steady_clock::now() < deadline, "Source 2 replacement publishes before its deadline");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (true);
  require(image[pixel] > 150 && image[pixel + 1] < 3 &&
              !target.forward->statistics().rayGI.active && !target.forward->statistics().rayReflections.active,
          "Source 2 restores baked red lighting instead of retaining green live GI");
  const auto published = controller->volume();
  const auto hash = target.forward->statistics().baking.activeHash;
  presets::applySource2(requested);
  target.forward->updateLighting(requested);
  for (uint32_t frame = 0; frame < remix::gpuFrameCount + 1; ++frame) {
    image = render();
    require(controller->volume() == published &&
                target.forward->statistics().baking.state == REMIXAPI_BAKE_READY_EXT &&
                target.forward->statistics().baking.activeHash == hash && image[pixel] > 150,
            "Reapplying Source 2 preserves completed lighting across fenced frame slots");
  }
  std::printf("Source 2 transition %ux: RGB %u %u %u; completed bake retained\n", requested.samples,
              unsigned(image[pixel]), unsigned(image[pixel + 1]), unsigned(image[pixel + 2]));
}

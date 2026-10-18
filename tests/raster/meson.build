if get_option('gpu_tests')
  raster_test = executable('remix-raster-test', 'rendering.cpp',
    include_directories : test_includes, dependencies : vulkan_dep, link_with : [raster_library, forward_library],
    cpp_args : cpp.get_supported_arguments('-Wno-missing-field-initializers'))
  raster_no_oidn_test = executable('remix-raster-no-oidn-test',
    'rendering.cpp', '../../src/native/forward/lightmap/denoise.cpp',
    include_directories : test_includes, dependencies : vulkan_dep, link_with : [raster_library, forward_library],
    cpp_args : cpp.get_supported_arguments('-Wno-missing-field-initializers'))
  test('Forward+ surface baking without optional OIDN', raster_no_oidn_test,
    args : [meson.current_build_dir() / 'no-oidn-readback', 'no-oidn'], timeout : 180)
  test('Raster GPU pixel readback', raster_test, args : [meson.current_build_dir() / 'readback'], timeout : 90)
  test('Forward+ GPU lighting, MSAA and compatibility readback', raster_test,
    args : [meson.current_build_dir() / 'forward-readback', 'forward'], timeout : 120)
  test('Forward+ skinned frame slots preserve animated geometry and shadows', raster_test,
    args : [meson.current_build_dir() / 'forward-skinning-readback', 'forward-skinning'], timeout : 120)
  test('Forward+ lighting component isolation and MSAA', raster_test,
    args : [meson.current_build_dir() / 'lighting-view-readback', 'lighting-views'], timeout : 120)
  test('Forward+ automatic exposure transitions, manual mode and MSAA state', raster_test,
      args : [meson.current_build_dir() / 'auto-exposure-readback', 'auto-exposure'], timeout : 120)
  test('Forward+ measured water HDR, MSAA and frame-slot readback', raster_test,
    args : [meson.current_build_dir() / 'water-readback', 'water'], timeout : 120)
  test('Forward+ water environment reflection and baked visibility', raster_test,
    args : [meson.current_build_dir() / 'water-environment-readback', 'water-environment'], timeout : 120)
  test('Forward+ automatic exposure with water HDR composition', raster_test,
      args : [meson.current_build_dir() / 'water-exposure-readback', 'water-exposure'], timeout : 120)
  test('Forward+ water shoreline coverage and refraction', raster_test,
    args : [meson.current_build_dir() / 'water-shoreline-readback', 'water-shoreline'], timeout : 120)
  test('Forward+ live dome background and MSAA coverage', raster_test,
    args : [meson.current_build_dir() / 'sky-readback', 'sky'], timeout : 120)
  test('Forward+ live GI replacement, dynamic transport and MSAA', raster_test,
    args : [meson.current_build_dir() / 'live-gi-readback', 'live-gi'], timeout : 180)
  test('Forward+ hardware reflection transport, current actors and MSAA', raster_test,
    args : [meson.current_build_dir() / 'hardware-reflection-readback', 'hardware-reflections'], timeout : 180)
  test('Forward+ SSR transport and MSAA guide readback', raster_test,
    args : [meson.current_build_dir() / 'reflection-readback', 'reflections'], timeout : 120)
  test('Forward+ baked irradiance publication and surface readback', raster_test,
    args : [meson.current_build_dir() / 'bake-readback', 'baking'], timeout : 120)
  test('Forward+ mixed dynamic local shadows, cache and MSAA', raster_test,
    args : [meson.current_build_dir() / 'local-shadow-readback', 'local-shadows'], timeout : 180)
    test('Forward+ unbaked point-light planar self-shadow regression', raster_test,
      args : [meson.current_build_dir() / 'local-shadow-plane-readback', 'local-shadow-plane'], timeout : 60)
  test('Forward+ separable baked direct diffuse and mixed shadows', raster_test,
    args : [meson.current_build_dir() / 'baked-direct-readback', 'baked-direct'], timeout : 180)
  test('Forward+ styled baked visibility, toggles, mixed shadows and cache', raster_test,
    args : [meson.current_build_dir() / 'styled-mixed-readback', 'styled-mixed'], timeout : 180)
    test('Forward+ styled indirect surfaces, dynamic probes, cache and cancellation', raster_test,
      args : [meson.current_build_dir() / 'styled-indirect-readback', 'styled-indirect'], timeout : 180)
  test('Forward+ per-light static visibility and live specular', raster_test,
    args : [meson.current_build_dir() / 'mixed-readback', 'mixed'], timeout : 180)
  test('Forward+ compact static occlusion and mixed-light overflow', raster_test,
    args : [meson.current_build_dir() / 'compact-occlusion-readback', 'compact-occlusion'], timeout : 180)
  test('Forward+ hidden metallic specular after partial visibility saturation', raster_test,
    args : [meson.current_build_dir() / 'overflow-specular-readback', 'overflow-specular'], timeout : 180)
  test('Forward+ sparse partial overflow masks retain exposed metallic lighting', raster_test,
    args : [meson.current_build_dir() / 'overflow-mask-readback', 'overflow-masks'], timeout : 180)
  test('Forward+ terrain paint inherits lighting, coverage and baked shadows', raster_test,
    args : [meson.current_build_dir() / 'terrain-readback', 'terrain-lighting'], timeout : 180)
  test('Forward+ compact clear visibility and realtime caster shadows', raster_test,
    args : [meson.current_build_dir() / 'clear-visibility-readback', 'clear-visibility'], timeout : 180)
  test('Forward+ baked sunlight and independent moving-caster shadows', raster_test,
    args : [meson.current_build_dir() / 'mixed-sun-readback', 'mixed-sun'], timeout : 180)
  test('Forward+ surface lightmap publication, shadows and MSAA', raster_test,
    args : [meson.current_build_dir() / 'lightmap-readback', 'lightmaps'], timeout : 180)
  test('Forward+ SSR and parallax cubemap source blending', raster_test,
    args : [meson.current_build_dir() / 'probe-blend-readback', 'probe-blend'], timeout : 120)
  test('Forward+ visibility-gated sky reflection fallback', raster_test,
    args : [meson.current_build_dir() / 'sky-reflection-readback', 'sky-reflections'], timeout : 120)
endif

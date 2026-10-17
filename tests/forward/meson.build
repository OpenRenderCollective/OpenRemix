forward_policy_test = executable('remix-forward-policy-test', 'lighting.cpp',
  '../../src/native/forward/lighting.cpp', '../../src/native/runtime/settings.cpp',
  '../../src/native/forward/water/data.cpp',
  include_directories : test_includes, link_with : scene_library, cpp_args : test_cpp_args)
test('Forward+ cluster, cascade and configuration policy', forward_policy_test)
forward_denoise_test = executable('remix-forward-lightmap-denoise-test', 'lightmap_denoise.cpp',
  include_directories : test_includes, link_with : lightmap_denoise_library, cpp_args : test_cpp_args)
test('Offline lightmap denoising preserves directional SH and chart isolation', forward_denoise_test,
  timeout : 120)
forward_grid_test = executable('remix-forward-grid-test', 'grid.cpp',
  '../../src/native/forward/bake/grid.cpp', '../../src/native/forward/lighting.cpp',
  include_directories : test_includes, link_with : scene_library, cpp_args : test_cpp_args)
test('Irradiance grid placement and visibility reconstruction', forward_grid_test)
forward_probes_test = executable('remix-forward-probes-test', 'probes.cpp',
  '../../src/native/forward/probes/data.cpp', '../../src/native/forward/probes/placement.cpp', '../../src/native/forward/probes/selection.cpp',
  '../../src/native/forward/probes/regions.cpp', '../../src/native/forward/probes/sites.cpp',
  '../../src/native/forward/lighting.cpp',
  include_directories : test_includes, link_with : scene_library, cpp_args : test_cpp_args)
test('Reflection probe parallax, filtering and packed data', forward_probes_test)
forward_regions_test = executable('remix-forward-regions-test', 'regions.cpp',
  '../../src/native/forward/probes/regions.cpp', '../../src/native/forward/probes/sites.cpp', '../../src/native/forward/lighting.cpp',
  '../../src/native/forward/probes/data.cpp', '../../src/native/forward/probes/placement.cpp', '../../src/native/forward/probes/selection.cpp',
  include_directories : test_includes, link_with : scene_library, cpp_args : test_cpp_args)
test('BSP reflection region occupancy and room fitting', forward_regions_test)
if get_option('gpu_tests')
  water_snapshot_test = executable('remix-forward-water-snapshot-test', 'water_snapshot.cpp',
    include_directories : test_includes, link_with : forward_library, dependencies : vulkan_dep,
    cpp_args : test_cpp_args)
  test('Water opaque HDR and actual MSAA depth snapshots', water_snapshot_test, timeout : 60)
  water_test = executable('remix-forward-water-test', 'water.cpp',
    include_directories : test_includes, link_with : forward_library, dependencies : vulkan_dep,
    cpp_args : test_cpp_args)
  test('Water boundary optics and opaque-depth refraction queries', water_test, timeout : 60)
  bake_transport_test = executable('remix-bake-transport-test', 'bake_transport.cpp',
    include_directories : test_includes, link_with : forward_library, dependencies : vulkan_dep,
    cpp_args : test_cpp_args)
  test('Hardware diffuse bake transport and textured visibility', bake_transport_test, timeout : 180)
  forward_ray_test = executable('remix-forward-ray-test', 'ray_scene.cpp',
    include_directories : test_includes, link_with : forward_library, dependencies : vulkan_dep,
    cpp_args : test_cpp_args)
  test('Forward+ hardware ray scene and candidate traversal', forward_ray_test, timeout : 60)
  forward_lifecycle_test = executable('remix-forward-lifecycle-test', 'lifecycle.cpp',
    include_directories : [test_includes, include_directories('../../samples')],
    dependencies : sdl3_dep, link_with : remix, cpp_args : test_cpp_args)
  test('Forward+ runtime backend transitions and publication', forward_lifecycle_test,
    timeout : 240, is_parallel : false)
endif

forward_lightmaps_test = executable('remix-forward-lightmaps-test', 'lightmaps.cpp',
  '../../src/native/forward/lightmap/layout.cpp', '../../src/native/forward/lightmap/packing.cpp', '../../src/native/forward/lightmap/fit.cpp', '../../src/native/forward/lightmap/parameterization.cpp', '../../src/native/forward/lightmap/curved.cpp', '../../src/native/forward/lightmap/harmonic.cpp', '../../src/native/forward/lightmap/fan.cpp', '../../src/native/forward/lightmap/seams.cpp', '../../src/native/forward/lightmap/curved_seams.cpp', '../../src/native/forward/lightmap/seam_mesh.cpp', '../../src/native/forward/lightmap/surface_walk.cpp', '../../src/native/forward/lightmap/coverage.cpp', '../../src/native/forward/lightmap/data.cpp', '../../src/native/forward/lighting.cpp',
  '../../src/native/forward/material.cpp',
  include_directories : test_includes, link_with : scene_library, cpp_args : test_cpp_args)
test('Surface lightmap atlas planning and world texel ownership', forward_lightmaps_test)

forward_mixed_test = executable('remix-forward-mixed-test', 'mixed.cpp',
  '../../src/native/forward/mixed/data.cpp', '../../src/native/forward/lightmap/layout.cpp', '../../src/native/forward/lightmap/packing.cpp', '../../src/native/forward/lightmap/fit.cpp', '../../src/native/forward/lightmap/parameterization.cpp', '../../src/native/forward/lightmap/curved.cpp', '../../src/native/forward/lightmap/harmonic.cpp', '../../src/native/forward/lightmap/fan.cpp', '../../src/native/forward/lightmap/seams.cpp', '../../src/native/forward/lightmap/curved_seams.cpp', '../../src/native/forward/lightmap/seam_mesh.cpp', '../../src/native/forward/lightmap/surface_walk.cpp', '../../src/native/forward/lightmap/coverage.cpp',
  '../../src/native/forward/mixed/refine.cpp', '../../src/native/forward/mixed/occlusion.cpp',
  '../../src/native/forward/ray/capture.cpp', '../../src/native/forward/bake/data.cpp',
  '../../src/native/forward/lightmap/data.cpp', '../../src/native/forward/material.cpp',
  '../../src/native/forward/lighting.cpp',
  include_directories : test_includes, link_with : scene_library, cpp_args : test_cpp_args)
test('Mixed light channel ownership and visibility planning', forward_mixed_test)

forward_shadow_test = executable('remix-forward-shadow-test', 'shadows.cpp',
  '../../src/native/forward/shadows/plan.cpp', '../../src/native/forward/lighting.cpp',
  include_directories : test_includes, link_with : scene_library, cpp_args : test_cpp_args)
test('Local shadow projection, selection and explicit caster membership', forward_shadow_test)

forward_budget_test = executable('remix-forward-budget-test', 'budget.cpp',
  include_directories : test_includes, link_with : scene_library, cpp_args : test_cpp_args)
test('Forward+ adaptive bake work and lighting identity', forward_budget_test)
forward_cache_test = executable('remix-forward-cache-test', 'cache.cpp',
  '../../src/native/forward/bake/cache.cpp', '../../src/native/forward/probes/data.cpp',
  '../../src/native/forward/lightmap/data.cpp', '../../src/native/forward/mixed/data.cpp',
  '../../src/native/forward/mixed/occlusion.cpp',
  '../../src/native/forward/lighting.cpp',
  include_directories : test_includes, link_with : [scene_library, lightmap_denoise_library], cpp_args : test_cpp_args)
test('Bake cache identity, payload integrity and mixed light ownership', forward_cache_test,
  args : [meson.current_build_dir() / 'cache'])

forward_world_grid_test = executable('remix-forward-world-grid-test', 'world_grid.cpp', 'curved.cpp', 'curved_join.cpp',
  '../../src/native/forward/lightmap/layout.cpp', '../../src/native/forward/lightmap/packing.cpp', '../../src/native/forward/lightmap/fit.cpp', '../../src/native/forward/lightmap/parameterization.cpp', '../../src/native/forward/lightmap/curved.cpp', '../../src/native/forward/lightmap/harmonic.cpp', '../../src/native/forward/lightmap/fan.cpp',
  '../../src/native/forward/lightmap/seams.cpp', '../../src/native/forward/lightmap/curved_seams.cpp', '../../src/native/forward/lightmap/seam_mesh.cpp', '../../src/native/forward/lightmap/surface_walk.cpp', '../../src/native/forward/lightmap/coverage.cpp', '../../src/native/forward/lightmap/data.cpp',
  '../../src/native/forward/material.cpp', '../../src/native/forward/lighting.cpp',
  include_directories : test_includes, link_with : scene_library, cpp_args : test_cpp_args)
test('World-space lightmap density and connected chart seams', forward_world_grid_test)

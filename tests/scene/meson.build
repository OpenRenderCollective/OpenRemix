scene_test = executable('remix-scene-test', 'resources.cpp',
  include_directories : test_includes, link_with : scene_library)
configure_file(input : 'fixtures/colors.png', output : 'raster-colors.png', copy : true)
test('Scene resources and image decoding', scene_test, args : [meson.current_build_dir() / 'resources'])
retained_test = executable('remix-retained-test', 'retained.cpp',
  include_directories : test_includes, link_with : scene_library)
test('Retained instance registration, replay and lifetimes', retained_test)
publish_test = executable('remix-publish-test', 'publish.cpp',
  include_directories : test_includes, link_with : scene_library, dependencies : threads_dep)
test('Published frames and resource lifetime', publish_test, timeout : 60)
refresh_test = executable('remix-mesh-refresh-test', 'refresh.cpp',
  include_directories : test_includes, link_with : scene_library)
test('Mesh refresh lineage, retained replay and correspondence', refresh_test)
temporal_test = executable('remix-temporal-test', 'temporal.cpp',
  include_directories : test_includes, link_with : scene_library, cpp_args : test_cpp_args)
test('Temporal submitted-frame ownership and correspondence', temporal_test)
light_bvh_test = executable('remix-light-bvh-test', 'light_bvh.cpp',
  include_directories : test_includes, link_with : scene_library)
test('Analytic lights and importance hierarchy', light_bvh_test)
ies_test = executable('remix-ies-test', 'ies.cpp',
  include_directories : test_includes, link_with : scene_library)
test('IES profiles and light cookies', ies_test)
emission_test = executable('remix-emission-test', 'emission.cpp',
  include_directories : test_includes, link_with : scene_library, cpp_args : test_cpp_args)
test('Emission reduction and area light ownership', emission_test)
trace_frame_test = executable('remix-trace-frame-test', 'trace_frame.cpp',
  include_directories : test_includes, link_with : scene_library, cpp_args : test_cpp_args)
test('CPU tracing preparation and snapshot ownership', trace_frame_test)
pbr_test = executable('remix-pbr-resource-test', 'pbr.cpp',
  include_directories : test_includes, link_with : scene_library)
test('PBR material validation and texture lifetime', pbr_test)
materials_ext_test = executable('remix-material-extensions-test', 'materials_ext.cpp',
  include_directories : test_includes, link_with : scene_library)
test('Instance material and dynamic textures', materials_ext_test)
opacity_test = executable('remix-opacity-test', 'opacity.cpp',
  include_directories : test_includes, link_with : scene_library, cpp_args : test_cpp_args)
test('Opaque traversal atlas coverage and snapshot ownership', opacity_test)
compressed_test = executable('remix-compressed-texture-test', 'compressed.cpp',
  include_directories : test_includes, link_with : scene_library)
test('Compressed BC textures, coverage and aliasing', compressed_test)
usd_lights_test = executable('remix-usd-lights-test', 'usd_lights.cpp', include_directories : test_includes, link_with : scene_library)
test('USD light conversion and BC4 BC6H assets', usd_lights_test)
particles_test = executable('remix-particles-test', 'particles.cpp',
  include_directories : test_includes, link_with : scene_library)
test('Particle emitters: validation, presets, slices and spawns', particles_test)

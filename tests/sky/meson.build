if numos_enabled
  # NUMOS's own unit tests, built with this project's toolchain against the core library.
  numos_test = executable('remix-numos-test', '../../third_party/numos/numos-sky/tests/main.cpp',
    include_directories : include_directories('../../third_party/numos/numos-sky/src'),
    dependencies : numos_dep, override_options : ['cpp_std=c++17', 'warning_level=0', 'werror=false'])
  test('NUMOS core settings, weather, seeds and pass table', numos_test)
  if not metal_backend
    sky_shader_test = executable('remix-sky-shader-test', 'shaders.cpp', sky_shaders[0],
      include_directories : test_includes, link_with : [sky_library, pipeline_library],
      dependencies : [numos_dep, vulkan_dep], cpp_args : test_cpp_args)
    test('Embedded NUMOS shaders match the pass table', sky_shader_test)
  endif
endif

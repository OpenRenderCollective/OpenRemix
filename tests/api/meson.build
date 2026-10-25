if system != 'windows'
  fixture = shared_library('remix_loader_fixture', 'loader_fixture.cpp',
    include_directories : remix_public_include, cpp_args : ['-DREMIX_LIBRARY_EXPORTS=1'])
  loader_test = executable('remix-loader-test', 'loader.cpp',
    include_directories : remix_public_include, dependencies : dl_dep)
  cpp_test = executable('remix-cpp-test', 'cpp_header.cpp',
    include_directories : remix_public_include, dependencies : dl_dep)
  test('Loader fixture', loader_test, args : [fixture.full_path()])
  test('Library loader', loader_test, args : [remix.full_path(), '--native-runtime'])
  test('C++ SDK wrapper', cpp_test, args : [remix.full_path()])
endif
particles_api_test = executable('remix-particles-api-test', 'particles.cpp',
  include_directories : remix_public_include, link_with : remix)
test('Particle exports before startup', particles_api_test)
if get_option('gpu_tests')
  extension_test = executable('remix-native-extensions-test', 'native_extensions.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, threads_dep, filesystem_dep], link_with : remix)
  test('Native input events and presented screenshot', extension_test,
    args : [meson.current_build_dir() / 'native-extensions'], timeout : 30)
endif

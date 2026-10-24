if numos_enabled
  # NUMOS's own unit tests, built with this project's toolchain against the core library.
  numos_test = executable('remix-numos-test', '../../third_party/numos/numos-sky/tests/main.cpp',
    include_directories : include_directories('../../third_party/numos/numos-sky/src'),
    dependencies : numos_dep, override_options : ['cpp_std=c++17', 'warning_level=0', 'werror=false'])
  test('NUMOS core settings, weather, seeds and pass table', numos_test)
endif

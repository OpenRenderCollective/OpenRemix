usd_test = executable('remix-usd-test', 'loading.cpp',
  include_directories : test_includes, cpp_args : usd_args,
  dependencies : usd_deps + [threads_dep], link_with : [usd_library, scene_library])
test('USD composition, host coordinates and reload', usd_test, args : [meson.current_source_dir() / 'fixtures'], timeout : 120)
usd_probe = executable('remix-usd-probe', 'probe.cpp',
  include_directories : test_includes, cpp_args : usd_args,
  dependencies : usd_deps + [threads_dep], link_with : [usd_library, scene_library])
replacements_test = executable('remix-usd-replacements-test', 'replacements.cpp',
  include_directories : test_includes, cpp_args : usd_args,
  dependencies : usd_deps + [threads_dep], link_with : [usd_library, scene_library])
test('USD captured hash replay and replacement reload', replacements_test, args : [meson.current_source_dir() / 'fixtures'], timeout : 120)
usd_api_test = executable('remix-usd-api-test', 'api.cpp', include_directories : test_includes, link_with : remix)
test('USD CPU-only versioned API', usd_api_test, args : [meson.current_source_dir() / 'fixtures/capture.usda'])

if get_option('gpu_tests') and system == 'linux'
  usd_gpu_api_test = executable('remix-usd-gpu-api-test', 'api_gpu.cpp', include_directories : test_includes, dependencies : threads_dep, link_with : remix)
  test('USD frame-boundary reload API', usd_gpu_api_test, args : [meson.current_source_dir() / 'fixtures/capture.usda'], timeout : 30)
endif
capture_test = executable('remix-usd-capture-test', 'capture.cpp',
  include_directories : test_includes, cpp_args : usd_args,
  dependencies : usd_deps + [threads_dep], link_with : [usd_library, scene_library])
test('USD scene capture round trip', capture_test, timeout : 120)

runtime_test = executable('remix-runtime-test', 'lifecycle.cpp',
  include_directories : [test_includes, include_directories('../../samples')], dependencies : [sdl3_dep, dl_dep, threads_dep])
test('API stubs and failed startup cleanup', runtime_test, args : [remix.full_path()], timeout : 30)
test('Sky API before startup', executable('remix-sky-api-test', 'sky_api.cpp',
  include_directories : test_includes, dependencies : [dl_dep]), args : [remix.full_path()], timeout : 30)
host_pass_test = executable('remix-host-pass-test', 'host_passes.cpp',
  include_directories : test_includes, dependencies : [dl_dep, threads_dep])
if get_option('gpu_tests')
  test(metal_backend ? 'Metal presentation and lifecycle' : 'Vulkan presentation and lifecycle', runtime_test, args : [remix.full_path(), 'gpu'], timeout : 90)
  if not metal_backend
    test('Host passes on the GPU', host_pass_test, args : [remix.full_path()], timeout : 120)
  endif
  test('Compressed BC textures on the GPU', runtime_test, args : [remix.full_path(), 'compressed'], timeout : 90)
  if system != 'windows'
    test('Live process exit cleanup', runtime_test, args : [remix.full_path(), 'exit'], timeout : 30)
  endif
  if system == 'linux'
    faults = shared_library('remix-vulkan-faults', 'faults.cpp', dependencies : [vulkan_dep, dl_dep])
    foreach fault : ['instance', 'device', 'framebuffer', 'submit', 'device-lost']
      test('Vulkan failure cleanup ' + fault, runtime_test, args : [remix.full_path(), 'fault'],
        env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=' + fault], timeout : 30)
    endforeach
    foreach fault : ['acquire-out-of-date', 'acquire-suboptimal', 'present-suboptimal', 'surface-lost']
      test('Vulkan recovery ' + fault, runtime_test, args : [remix.full_path(), 'recover'],
        env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=' + fault], timeout : 30)
    endforeach
  endif
endif

profile_test = executable('remix-profiles-test', 'profiles.cpp',
  '../../src/native/runtime/settings.cpp', '../../src/native/runtime/profiles.cpp',
  '../../src/native/ui/profile_actions.cpp', include_directories : [remix_public_include, native_include],
  dependencies : filesystem_dep)
test('Configuration profiles and unsaved actions', profile_test, args : ['@CONFIG_DIR@'])
particle_settings_test = executable('remix-particle-settings-test', 'particle_settings.cpp',
  '../../src/native/runtime/settings.cpp', include_directories : native_include)
test('Particle settings in profiles', particle_settings_test)
if numos_enabled
  sky_settings_test = executable('remix-sky-settings-test', 'sky_settings.cpp', '../../src/native/runtime/settings.cpp',
    include_directories : native_include, dependencies : numos_dep)
  test('Sky settings and NUMOS keys in profiles', sky_settings_test)
endif
if get_option('gpu_tests')
  profile_runtime_test = executable('remix-profile-runtime-test', 'profile_runtime.cpp',
    '../../src/native/runtime/settings.cpp', '../../src/native/runtime/profiles.cpp',
    include_directories : [test_includes, include_directories('../../samples')],
    dependencies : [sdl3_dep, filesystem_dep], link_with : remix)
  test('Profile startup and API precedence', profile_runtime_test,
    args : ['@CONFIG_DIR@', meson.current_build_dir() / 'profile-images'], timeout : 600, is_parallel : false)
  if system == 'linux'
    test('Profile preferences survive unsupported backend', profile_runtime_test,
      args : ['@CONFIG_DIR@', meson.current_build_dir() / 'profile-fallback-images'],
      env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=pathtrace-capability'], timeout : 60, depends : faults, is_parallel : false)
    test('Profile panel at high DPI', profile_runtime_test,
      args : ['@CONFIG_DIR@', meson.current_build_dir() / 'profile-hidpi-images'],
      env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=ui-high-dpi', 'REMIX_TEST_PROFILE_UI_ONLY=1'], timeout : 60, depends : faults, is_parallel : false)
  endif
endif


if not metal_backend
  pipeline_test = executable('remix-pipeline-test', 'pipelines.cpp',
    include_directories : native_include, link_with : pipeline_library,
    dependencies : [vulkan_dep, threads_dep, filesystem_dep], cpp_args : test_cpp_args)
  test('Pipeline preparation, persistence and shutdown', pipeline_test, args : ['@CONFIG_DIR@'], timeout : 30)
endif

if get_option('gpu_tests') and system == 'linux'
  preparation_gpu_test = executable('remix-preparation-gpu-test', 'preparation_gpu.cpp',
    include_directories : [test_includes, include_directories('../../samples')],
    dependencies : [runtime_test_dep, dl_dep])
  test('Background preparation presentation input and activation', preparation_gpu_test,
    args : [meson.current_build_dir() / 'preparation-images'], timeout : 180, is_parallel : false)
  view_model_gpu_test = executable('remix-view-model-gpu-test', 'view_model_gpu.cpp',
    include_directories : test_includes, dependencies : [runtime_test_dep, dl_dep])
  test('Path-traced view models in front of the scene, without shadows on it', view_model_gpu_test,
    args : [meson.current_build_dir() / 'view-model-images'], timeout : 180, is_parallel : false)
  test('Preparation retains last game frame through resize and VSync', preparation_gpu_test,
    args : [meson.current_build_dir() / 'retained-frame-images'],
    env : ['REMIX_TEST_LAST_FRAME=1'], timeout : 180, is_parallel : false)
endif

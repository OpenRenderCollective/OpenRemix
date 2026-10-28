project('remix-shell', ['c', 'cpp'], version : '0.1003.0', meson_version : '>=1.3',
  default_options : ['cpp_std=c++20', 'c_std=c11', 'warning_level=2', 'b_vscrt=from_buildtype'])
cpp = meson.get_compiler('cpp')
system = host_machine.system()
if system not in ['linux', 'windows', 'darwin', 'android']
  error('Supported platforms: Linux, Windows, macOS and Android.')
endif
if (system in ['linux', 'windows'] and host_machine.cpu_family() != 'x86_64') or (system in ['darwin', 'android'] and host_machine.cpu_family() != 'aarch64')
  error('Supported targets: Linux/Windows x64 and macOS/Android arm64.')
endif
if cpp.get_id() not in ['clang', 'clang-cl']
  error('Use Clang (clang-cl with the Visual Studio toolchain on Windows).')
endif
if system == 'windows'
  add_project_arguments('-DNOMINMAX', '-D_CRT_SECURE_NO_WARNINGS', '-D_SILENCE_ALL_CXX20_DEPRECATION_WARNINGS', '-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH', language : ['c', 'cpp'])
  if cpp.get_id() == 'clang'
    add_project_arguments('-include', meson.project_source_root() / 'src/native/platform/mingw_compat.h', language : 'cpp')
  endif
endif
# One GPU backend per build: the Metal backend replaces the Vulkan renderer and
# links neither Vulkan nor MoltenVK.
metal_backend = get_option('gpu_backend') == 'metal'
if metal_backend and system != 'darwin'
  error('The Metal backend (gpu_backend=metal) is macOS only.')
endif
if metal_backend
  add_languages('objcpp', native : false)
  add_project_arguments('-DREMIX_METAL=1', language : ['c', 'cpp', 'objcpp'])
  metal_frameworks = dependency('appleframeworks', modules : ['Metal', 'MetalFX', 'QuartzCore', 'Foundation'])
endif
add_project_arguments('-DREMIX_HAS_SHARC=' + (get_option('sharc') ? '1' : '0'),
  language : metal_backend ? ['c', 'cpp', 'objcpp'] : ['c', 'cpp'])
threads_dep = dependency('threads')
dl_dep = cpp.find_library('dl', required : false)
remix_public_include = include_directories('public/include')
prefix = get_option('deps_root')
if prefix != ''
  sdl3_dep = declare_dependency(
    dependencies : cpp.find_library('SDL3', dirs : [prefix / 'lib']),
    compile_args : ['-I' + prefix / 'include'])
elif get_option('use_system_sdl')
  sdl3_dep = dependency('sdl3', version : '>=3.4.14')
else
  cmake = import('cmake')
  opts = cmake.subproject_options()
  opts.set_override_option('werror', 'false')
  opts.add_cmake_defines({'SDL_SHARED': true, 'SDL_STATIC': false,
    'SDL_TEST_LIBRARY': false, 'SDL_TESTS': false, 'SDL_EXAMPLES': false})
  sdl_project = cmake.subproject('SDL3', options : opts)
  sdl3_dep = sdl_project.dependency('SDL3-shared')
endif
if not metal_backend
  if system == 'android'
    vulkan_dep = cpp.find_library('vulkan')
  else
    vulkan_dep = dependency('vulkan', version : '>=1.1')
  endif
  vulkan_dep = declare_dependency(dependencies : vulkan_dep,
    include_directories : include_directories('include/vulkan/include', is_system : true))
endif
subdir('third_party/imgui')
subdir('third_party/nrd')
if not metal_backend
  subdir('third_party/fsr31')
  subdir('third_party/dlss')
endif
subdir('third_party/xz-embedded')
subdir('third_party/numos')
subdir('src/native')
subdir('samples/remix-native')
# The desktop samples open a window of their own.
if system != 'android'
  subdir('samples/remix-bench')
  subdir('samples/remix-cornell')
  subdir('samples/remix-sponza')
endif
if get_option('enable_tests')
  subdir('tests')
endif
if get_option('vulkan_probes')
  if metal_backend
    error('Vulkan probes need gpu_backend=vulkan.')
  endif
  subdir('tools/vulkan-probes')
endif
install_subdir('public/include/remix', install_dir : get_option('includedir'))
if get_option('sharc')
  install_data('third_party/sharc/License.md', rename : 'SHARC.md', install_dir : get_option('datadir') / 'remix/licenses')
endif
install_data('LICENSE', 'ThirdPartyLicenses.txt', install_dir : get_option('datadir') / 'remix/licenses')
install_data('third_party/image/LICENSES.txt', rename : 'ImageDecoders.txt',
  install_dir : get_option('datadir') / 'remix/licenses')

install_data('src/native/scene/OpenUSD-LICENSE.txt', install_dir : get_option('datadir') / 'remix/licenses')

native_include = include_directories('.')
# The renderer's shaders are generated at build time (scripts/shaders/generate.py) into
# this build directory, where the sources' "pathtrace/shaders/embedded.h" includes find
# them; none of the output is committed. The tools are built at pinned revisions, once
# per machine (scripts/shaders/toolchain.py), so every machine generates the same SPIR-V.
shader_python = find_program('python3')
shader_scripts = meson.project_source_root() / 'scripts/shaders'
# Generation imports these modules; bytecode would land in the source tree.
shader_env = {'PYTHONDONTWRITEBYTECODE' : '1'}
shader_tools = custom_target('shader-tools',
  output : 'shader-tools.json',
  command : [shader_python, shader_scripts / 'toolchain.py', '--output', '@OUTPUT@',
             '--cache', get_option('shader_tools')] + (metal_backend ? ['--metal'] : []) +
             (numos_enabled and not metal_backend ? ['--slang'] : []),
  depend_files : files('../../scripts/shaders/toolchain.py', '../../scripts/shaders/toolchain.lock.json'),
  env : shader_env, console : true)
shader_generate = [shader_python, shader_scripts / 'generate.py', '--tools', shader_tools,
                   '--output', meson.current_build_dir(), '--depfile', '@DEPFILE@', '--depfile-target', '@OUTPUT0@']
shader_generator_files = files('../../scripts/shaders/generate.py', '../../scripts/shaders/metal.py')
subdir('raster/shaders')
subdir('denoise/shaders')
subdir('postfx/shaders')
subdir('upscale/shaders')
subdir('pathtrace/shaders')
if not metal_backend
  subdir('particles/shaders')
endif
if numos_enabled and not metal_backend
  subdir('sky/shaders')
endif
image_include = include_directories('../../third_party', is_system : true)
# Metal builds share the API, scene, platform and settings code and replace the
# Vulkan runtime and renderers with src/native/metal (one backend per build).
filesystem_dep = cpp.find_library('stdc++fs', required : false)
if metal_backend
  shell_deps = [sdl3_dep, threads_dep, imgui_dep, metal_frameworks, nrd_dep, numos_dep]
  shell_args = ['-DREMIX_LIBRARY_EXPORTS=1'] + cpp.get_supported_arguments('-Wno-missing-field-initializers', '-Wno-unused-parameter')
  subdir('platform')
  subdir('api')
  subdir('scene')
  usd_libraries = []
  if get_option('usd')
    subdir('usd')
  endif
  subdir('metal')
  subdir('metal/shaders')
  remix = shared_library('remix',
    # The Metal sources include the Vulkan backends' generated headers too (pathtrace/gpu.h).
    api_sources + metal_shared_sources + metal_sources + [metal_shaders[0], metal_shaders[2]] +
      [raster_shaders, denoise_shaders, postfx_shaders, upscale_shaders, pathtrace_shaders[0]],
    name_prefix : 'lib',
    link_with : usd_libraries + [shell_platform, scene_library],
    dependencies : shell_deps + [xz_dep, filesystem_dep],
    include_directories : [remix_public_include, native_include, image_include],
    cpp_args : shell_args,
    objcpp_args : metal_args,
    override_options : ['objcpp_std=c++20'],
    gnu_symbol_visibility : 'hidden',
    version : meson.project_version(), soversion : '0',
    # The SDK's ABI minor 1003 exceeds Mach-O's 8-bit version component.
    darwin_versions : ['0.0.0', '0.0.0'],
    install_rpath : '@loader_path',
    install : true)
  subdir_done()
endif
shell_deps = [sdl3_dep, vulkan_dep, threads_dep, imgui_dep, nrd_dep, fsr31_dep, dlss_dep, numos_dep]
shell_args = ['-DREMIX_LIBRARY_EXPORTS=1'] + cpp.get_supported_arguments('-Wno-missing-field-initializers', '-Wno-unused-parameter')
subdir('platform')
subdir('api')
subdir('runtime')
subdir('ui')
# GCC 8's standard library ships filesystem in a separate static archive (filesystem_dep above).
pipeline_library = static_library('remix_pipelines',
  'runtime/preparation.cpp', 'runtime/pipeline_cache.cpp', 'runtime/pipelines.cpp',
  dependencies : [vulkan_dep, threads_dep, filesystem_dep], include_directories : native_include,
  cpp_args : cpp.get_supported_arguments('-Wno-missing-field-initializers'),
  gnu_symbol_visibility : 'hidden', pic : true)
subdir('scene')
usd_libraries = []
if get_option('usd')
  subdir('usd')
endif
subdir('raster')
subdir('denoise')
subdir('postfx')
subdir('upscale')
sky_libraries = []
if numos_enabled
  subdir('sky')
  sky_libraries = [sky_library]
endif
subdir('pathtrace')
subdir('particles')
imgui_vulkan = static_library('remix_imgui_vulkan', 'ui/vulkan_backend.cpp',
  include_directories : [native_include, imgui_include],
  dependencies : vulkan_dep, link_with : pipeline_library,
  gnu_symbol_visibility : 'hidden', pic : true,
  override_options : ['warning_level=0', 'werror=false'])
shell_link_args = []
if system in ['linux', 'android']
  shell_link_args += ['-Wl,--version-script=' + meson.current_source_dir() / 'api/remix.map']
endif
shell_rpath = ''
shell_versions = {}
if system not in ['windows', 'android']
  shell_versions = {'version' : meson.project_version(), 'soversion' : '0'}
endif
if system == 'darwin'
  shell_rpath = '@loader_path'
elif system == 'linux'
  shell_rpath = '$ORIGIN'
endif
remix = shared_library('remix',
  # runtime/hdr_output.cpp embeds the upscaler's HDR10 present stage.
  api_sources + runtime_sources + ui_sources + [upscale_shaders],
  name_prefix : system == 'windows' ? '' : 'lib',
  link_with : usd_libraries + sky_libraries + [shell_platform, imgui_vulkan, scene_library, raster_library, pathtrace_library, denoise_library, upscale_library, particles_library], dependencies : shell_deps,
  include_directories : [remix_public_include, native_include, image_include], cpp_args : shell_args,
  link_args : shell_link_args, link_depends : 'api/remix.map',
  gnu_symbol_visibility : 'hidden',
  kwargs : shell_versions,
  # The SDK's ABI minor 1003 exceeds Mach-O's 8-bit version component.
  darwin_versions : ['0.0.0', '0.0.0'],
  install_rpath : shell_rpath,
  install : true)

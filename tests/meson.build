python = find_program('python3')
add_test_setup('isolatedconfig', exe_wrapper : [python, meson.current_source_dir() / 'isolated_config.py'], is_default : true)
test_include = include_directories('.')
test_includes = [remix_public_include, native_include, test_include, image_include]
# Vulkan and Remix structs are designated-initialized with only the fields set,
# as in the runtime libraries (werror builds).
test_cpp_args = cpp.get_supported_arguments('-Wno-missing-field-initializers')
if metal_backend
  # gpu_backend=metal: the backend-independent tests, and the Metal backend's.
  subdir('abi')
  subdir('api')
  subdir('scene')
  subdir('sky')
  subdir('ui')
  subdir('platform')
  subdir('runtime')
  subdir('metal')
  if get_option('usd')
    subdir('usd')
  endif
  subdir_done()
endif
# Render fixtures inspect the internal preparation status without adding an
# export to the Remix ABI. They execute the same compiled runtime objects.
runtime_test_dep = declare_dependency(
  objects : remix.extract_all_objects(recursive : false),
  link_with : usd_libraries + [shell_platform, imgui_vulkan, scene_library,
    raster_library, pathtrace_library, denoise_library, upscale_library, particles_library],
  dependencies : shell_deps,
  compile_args : ['-DREMIX_LIBRARY_EXPORTS=1'] + test_cpp_args)
subdir('abi')
subdir('api')
subdir('scene')
subdir('sky')
subdir('ui')
subdir('platform')
subdir('runtime')
subdir('raster')
subdir('pathtrace')
subdir('denoise')
subdir('postfx')

if get_option('usd')
  subdir('usd')
endif

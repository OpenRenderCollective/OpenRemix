# NUMOS sky (Vulkan): the embedded shaders and, through numos::IGpu, the NUMOS renderer.
sky_library = static_library('remix_sky',
  'bridge.cpp', 'renderer.cpp', 'shader_code.cpp', sky_shaders[0], sky_shaders[2], sky_shaders[3],
  link_with : [scene_library, pipeline_library],
  dependencies : [vulkan_dep, numos_dep, xz_dep], include_directories : [remix_public_include, native_include],
  cpp_args : cpp.get_supported_arguments('-Wno-missing-field-initializers'),
  gnu_symbol_visibility : 'hidden', pic : true)

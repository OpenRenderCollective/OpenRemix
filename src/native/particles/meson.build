particles_library = static_library('remix_particles', 'renderer.cpp', particles_shaders,
  link_with : [scene_library, pipeline_library], dependencies : vulkan_dep,
  include_directories : [native_include, remix_public_include],
  cpp_args : cpp.get_supported_arguments('-Wno-missing-field-initializers'),
  gnu_symbol_visibility : 'hidden', pic : true)

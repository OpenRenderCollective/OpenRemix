denoise_library = static_library('remix_denoise', 'renderer.cpp', 'nrd.cpp', denoise_shaders,
  link_with : pipeline_library,
  dependencies : [vulkan_dep, nrd_dep],
  include_directories : [remix_public_include, native_include],
  cpp_args : cpp.get_supported_arguments('-Wno-missing-field-initializers'),
  gnu_symbol_visibility : 'hidden', pic : true)

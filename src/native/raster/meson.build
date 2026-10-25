raster_library = static_library('remix_raster',
  'halo.cpp', 'mask.cpp', 'memory.cpp', 'pipelines.cpp', 'renderer.cpp', 'rendering.cpp', 'resources.cpp',
  'sprite.cpp', raster_shaders,
  link_with : [scene_library, pipeline_library],
  dependencies : [vulkan_dep], include_directories : [remix_public_include, native_include, image_include],
  cpp_args : cpp.get_supported_arguments('-Wno-missing-field-initializers'),
  gnu_symbol_visibility : 'hidden', pic : true)

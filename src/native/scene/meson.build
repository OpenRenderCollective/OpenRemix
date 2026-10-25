scene_library = static_library('remix_scene',
  'emission.cpp', 'trace_frame.cpp', 'opacity.cpp',
  'content_hash.cpp', 'images.cpp', 'instances.cpp', 'lights.cpp', 'light_bvh.cpp', 'ies.cpp', 'light_visibility.cpp', 'materials.cpp', 'math.cpp', 'meshes.cpp', 'particles.cpp', 'publish.cpp', 'resources.cpp', 'retained.cpp', 'static_scene.cpp', 'textures.cpp', 'temporal.cpp', 'ui.cpp', 'validation.cpp',
  dependencies : [filesystem_dep], include_directories : [remix_public_include, native_include, image_include],
  cpp_args : cpp.get_supported_arguments('-Wno-missing-field-initializers'),
  gnu_symbol_visibility : 'hidden', pic : true)

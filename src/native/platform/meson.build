shell_platform = static_library('remix_platform',
  'window.cpp', 'events.cpp', 'input.cpp',
  dependencies : metal_backend ? [sdl3_dep, threads_dep] : [sdl3_dep, vulkan_dep, threads_dep],
  include_directories : [remix_public_include, native_include],
  cpp_args : shell_args, gnu_symbol_visibility : 'hidden', pic : true)

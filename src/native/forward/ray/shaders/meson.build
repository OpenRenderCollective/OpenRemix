# Generated from maintained GLSL with the pinned build toolchain.
live_ray_shaders = custom_target('live_ray-shaders',
  output : 'embedded.h', command : shader_generate + ['live_ray'],
  depfile : 'embedded.d', depend_files : shader_generator_files, env : shader_env)

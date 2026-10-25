# Generated from maintained GLSL with the pinned build toolchain.
live_gi_shaders = custom_target('live_gi-shaders',
  output : 'embedded.h', command : shader_generate + ['live_gi'],
  depfile : 'embedded.d', depend_files : shader_generator_files, env : shader_env)

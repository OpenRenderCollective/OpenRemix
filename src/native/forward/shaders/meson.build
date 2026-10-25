# Generated from maintained GLSL with the pinned build toolchain.
forward_shaders = custom_target('forward-shaders',
  output : 'embedded.h', command : shader_generate + ['forward'],
  depfile : 'embedded.d', depend_files : shader_generator_files, env : shader_env)

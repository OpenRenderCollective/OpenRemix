# Generated from maintained GLSL with the pinned build toolchain.
cascade_shaders = custom_target('cascades-shaders',
  output : 'embedded.h', command : shader_generate + ['cascades'],
  depfile : 'embedded.d', depend_files : shader_generator_files, env : shader_env)

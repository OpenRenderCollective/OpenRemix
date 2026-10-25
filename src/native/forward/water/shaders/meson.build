# Generated from maintained GLSL with the pinned build toolchain.
water_shaders = custom_target('water-shaders',
  output : 'embedded.h', command : shader_generate + ['water'],
  depfile : 'embedded.d', depend_files : shader_generator_files, env : shader_env)

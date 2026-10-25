# Generated from maintained GLSL with the pinned build toolchain.
water_ray_shaders = custom_target('water_ray-shaders',
  output : 'embedded.h', command : shader_generate + ['water_ray'],
  depfile : 'embedded.d', depend_files : shader_generator_files, env : shader_env)

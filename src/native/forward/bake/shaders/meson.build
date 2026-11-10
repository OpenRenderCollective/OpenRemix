# Generated from maintained GLSL with the pinned build toolchain.
bake_shaders = custom_target('bake-shaders',
  output : 'embedded.h', command : shader_generate + ['bake'],
  depfile : 'embedded.d', depend_files : shader_generator_files, env : shader_env)

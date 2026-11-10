# Generated from maintained GLSL with the pinned build toolchain.
probe_batch_shaders = custom_target('probe_batch-shaders',
  output : 'embedded.h', command : shader_generate + ['probe_batch'],
  depfile : 'embedded.d', depend_files : shader_generator_files, env : shader_env)

shadow_test_shaders = custom_target('shadow_test-shaders',
  output : 'embedded.h', command : shader_generate + ['shadow_test'],
  depfile : 'embedded.d', depend_files : shader_generator_files, env : shader_env)

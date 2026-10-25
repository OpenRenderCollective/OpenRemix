mask_test_shaders = custom_target('mask_test-shaders',
  output : 'embedded.h', command : shader_generate + ['mask_test'],
  depfile : 'embedded.d', depend_files : shader_generator_files, env : shader_env)

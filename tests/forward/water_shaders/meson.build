water_test_shaders = custom_target('water_test-shaders',
  output : 'embedded.h', command : shader_generate + ['water_test'],
  depfile : 'embedded.d', depend_files : shader_generator_files, env : shader_env)

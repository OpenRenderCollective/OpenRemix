cluster_test_shaders = custom_target('cluster_test-shaders',
  output : 'embedded.h', command : shader_generate + ['cluster_test'],
  depfile : 'embedded.d', depend_files : shader_generator_files, env : shader_env)

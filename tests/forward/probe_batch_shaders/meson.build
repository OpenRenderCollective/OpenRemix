probe_batch_test_shaders = custom_target('probe_batch_test-shaders',
  output : 'embedded.h', command : shader_generate + ['probe_batch_test'],
  depfile : 'embedded.d', depend_files : shader_generator_files, env : shader_env)

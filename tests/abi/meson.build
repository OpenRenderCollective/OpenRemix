abi_test = executable('remix-abi-test', 'layout.c', include_directories : remix_public_include)
test('C ABI layouts', abi_test)
audit_env = environment()
if system == 'windows'
  # Python has no link interface from which Meson can infer external DLL paths.
  foreach prefix : [get_option('usd_root'), get_option('usd_tbb_root'), get_option('oidn_root')]
    if prefix != ''
      audit_env.prepend('PATH', prefix / 'bin', prefix / 'lib')
    endif
  endforeach
endif
test('SDK and export baseline', python, depends : remix, env : audit_env,
  args : [files('audit.py'), meson.project_source_root(), remix.full_path(), get_option('usd') ? '--usd' : '--no-usd'])
if system == 'windows'
  exports_test = executable('remix-windows-exports-test', 'windows_exports.c',
    include_directories : remix_public_include, link_with : remix)
  test('Windows exports and compatibility widgets', exports_test,
    args : [remix.full_path(), meson.current_source_dir() / 'exports.txt'])
endif

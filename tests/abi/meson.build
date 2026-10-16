abi_test = executable('remix-abi-test', 'layout.c', include_directories : remix_public_include)
test('C ABI layouts', abi_test)
test('SDK and export baseline', python, args : [files('audit.py'), meson.project_source_root(), remix.full_path(), get_option('usd') ? '--usd' : '--no-usd'])
if system == 'windows'
  exports_test = executable('remix-windows-exports-test', 'windows_exports.c',
    include_directories : remix_public_include, link_with : remix)
  test('Windows exports and compatibility widgets', exports_test,
    args : [remix.full_path(), meson.current_source_dir() / 'exports.txt'])
endif

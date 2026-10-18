# The Metal backend's libraries match the SPIR-V they were translated from
# (no shader tools needed: scripts/shaders/generate.py metal --check).
test('Metal libraries match the embedded SPIR-V', python,
  args : [meson.project_source_root() / 'scripts/shaders/generate.py', 'metal', '--check'])
# The Metal copies of the panel's tabs (src/native/metal/ui) match the current upstream tabs.
test('Metal UI copies match the upstream tabs', python,
  args : [meson.project_source_root() / 'scripts/macos/metal-ui.py'])
if get_option('gpu_tests')
  render_test = executable('remix-metal-render-test', 'render.cpp',
    include_directories : [remix_public_include, image_include, include_directories('../../samples')],
    dependencies : dl_dep)
  # Path tracing pipelines compile on first use (Apple's compiler service).
  test('Metal raster and path traced rendering', render_test,
    args : [remix.full_path(), meson.current_build_dir() / 'render'], timeout : 600, is_parallel : false)
  overlays_test = executable('remix-metal-overlays-test', 'overlays.cpp',
    include_directories : [remix_public_include, image_include], dependencies : dl_dep)
  test('Metal halos, masks, cut coverage and soft sprites', overlays_test,
    args : [remix.full_path(), meson.current_build_dir() / 'overlays'], timeout : 600, is_parallel : false)
endif

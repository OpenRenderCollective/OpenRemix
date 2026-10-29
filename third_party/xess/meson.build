# Intel XeSS SDK 3.0.2 (the git submodule xess, pinned in upstream.json):
# Windows' XeSS Super Resolution. Its headers, and the signed libxess.dll that
# src/native/upscale/xess.cpp loads at runtime; nothing is linked.
fs = import('fs')
if not fs.exists('xess/inc/xess/xess_vk.h')
  error('XeSS on Windows needs the XeSS SDK submodule: git submodule update --init third_party/xess/xess')
endif
xess_sdk_dep = declare_dependency(include_directories : include_directories('xess/inc', is_system : true))
xess_runtime_dir = meson.current_source_dir() / 'xess/bin'
xess_binaries = ['libxess.dll']
xess_install_dir = get_option('bindir') / 'xess'
foreach binary : xess_binaries
  install_data('xess/bin' / binary, install_dir : xess_install_dir)
endforeach
install_data('xess/LICENSE.txt', 'xess/third-party-programs.txt', 'upstream.json', install_dir : xess_install_dir)
install_data('xess/LICENSE.txt', rename : 'XeSS.txt', install_dir : get_option('datadir') / 'remix/licenses')

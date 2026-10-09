has_dlss = get_option('dlss') and host_machine.cpu_family() == 'x86_64' and (system == 'linux' or (system == 'windows' and cpp.get_id() == 'clang-cl'))
has_dlss5 = has_dlss and get_option('dlss5')
dlss5_arg = has_dlss5 ? '-DREMIX_HAS_DLSS5=1' : '-DREMIX_HAS_DLSS5=0'
dlss_dep = declare_dependency(compile_args : ['-DREMIX_HAS_DLSS=0', dlss5_arg])
if has_dlss
  ngx_library = system == 'linux' ? 'lib/Linux_x86_64/libnvsdk_ngx.a' : 'lib/Windows_x86_64/x64/nvsdk_ngx_d.lib'
  dlss_dep = declare_dependency(
    include_directories : include_directories('include', is_system : true),
    compile_args : ['-DREMIX_HAS_DLSS=1', dlss5_arg],
    link_args : [meson.current_source_dir() / ngx_library], dependencies : dl_dep)
  dlss_runtime_dir = system == 'linux' ? 'lib/Linux_x86_64/rel' : 'lib/Windows_x86_64/rel'
  dlss_binaries = system == 'linux' ? ['libnvidia-ngx-dlss.so.310.9.1', 'libnvidia-ngx-dlssd.so.310.9.1'] : ['nvngx_dlss.dll', 'nvngx_dlssd.dll', 'nvngx_dlssg.dll']
  # Ray Reconstruction's CNN model (preset C): the last release with it,
  # searched before the current one when that model is selected.
  dlss_cnn_runtime_dir = system == 'linux' ? 'lib/Linux_x86_64/rel-310.2.1' : 'lib/Windows_x86_64/rel-310.2.1'
  dlss_cnn_binary = system == 'linux' ? 'libnvidia-ngx-dlssd.so.310.2.1' : 'nvngx_dlssd.dll'
endif
if has_dlss
  dlss_install_dir = (system == 'windows' ? get_option('bindir') : get_option('libdir')) / 'dlss/310.9.1'
  foreach binary : dlss_binaries
    install_data(dlss_runtime_dir / binary, install_dir : dlss_install_dir)
  endforeach
  install_data('LICENSE.txt', 'upstream.json', 'sources.sha256', install_dir : dlss_install_dir)
  dlss_cnn_install_dir = (system == 'windows' ? get_option('bindir') : get_option('libdir')) / 'dlss/310.2.1'
  install_data(dlss_cnn_runtime_dir / dlss_cnn_binary, install_dir : dlss_cnn_install_dir)
  install_data('LICENSE-310.2.1.txt', rename : 'LICENSE.txt', install_dir : dlss_cnn_install_dir)
endif

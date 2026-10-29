# AMD FidelityFX SDK 2.4.0 (the git submodule FidelityFX-SDK, pinned in
# upstream.json): Windows' FSR. Its ffx-api headers, and the signed Vulkan
# DLLs src/native/upscale/ffx_library.cpp loads at runtime; nothing is linked.
fs = import('fs')
fidelityfx_kit = 'FidelityFX-SDK/Kits/FidelityFX'
if not fs.exists(fidelityfx_kit / 'api/include/ffx_api.h')
  error('FSR on Windows needs the FidelityFX SDK submodule: git submodule update --init third_party/fidelityfx/FidelityFX-SDK')
endif
fidelityfx_dep = declare_dependency(include_directories : include_directories(
  fidelityfx_kit / 'api/include', fidelityfx_kit / 'upscalers/include',
  fidelityfx_kit / 'framegeneration/include', is_system : true))
# The loader loads the effect DLLs from its own folder.
fidelityfx_runtime_dir = meson.current_source_dir() / fidelityfx_kit / 'signedbin'
fidelityfx_binaries = ['amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll',
  'amd_fidelityfx_framegeneration_vk.dll']
fidelityfx_install_dir = get_option('bindir') / 'fidelityfx'
foreach binary : fidelityfx_binaries
  install_data(fidelityfx_kit / 'signedbin' / binary, install_dir : fidelityfx_install_dir)
endforeach
install_data(fidelityfx_kit / 'docs/license.md', rename : 'LICENSE.md', install_dir : fidelityfx_install_dir)
install_data(fidelityfx_kit / 'docs/license.md', rename : 'FidelityFX-SDK.md',
  install_dir : get_option('datadir') / 'remix/licenses')

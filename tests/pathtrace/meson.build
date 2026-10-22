policy_test = executable('remix-tlas-policy-test', 'tlas_policy.cpp',
  include_directories : test_includes, link_with : pathtrace_library)
test('Tracing scene and TLAS policy', policy_test)
sparse_policy_test = executable('remix-sparse-policy-test', 'sparse_policy.cpp',
  include_directories : test_includes)
test('Sparse rendering sampling rate', sparse_policy_test)
volume_policy_test = executable('remix-volume-policy-test', 'volume_policy.cpp',
  include_directories : test_includes)
test('Volumetric medium from fog and settings', volume_policy_test)
static_scene_test = executable('remix-static-scene-test', 'static_scene.cpp',
  include_directories : test_includes, link_with : scene_library, dependencies : threads_dep)
test('Persistent static set matches the rebuild path', static_scene_test)
material_optics_test = executable('remix-trace-material-optics-test', 'material_optics.cpp',
  include_directories : test_includes, link_with : scene_library)
test('Path-tracer measured sheet material representation', material_optics_test)
upscale_depth_test = executable('remix-upscale-depth-test', 'upscale_depth.cpp', include_directories : test_includes)
test('FSR wide-frustum depth precision', upscale_depth_test)
# CPU cost of Present for a scene of about 12k scene instances. A benchmark,
# not a test: meson test --benchmark "Scene frame CPU cost" (display + ray-tracing device).
scene_frame_benchmark = executable('remix-scene-frame-benchmark', 'scene_frame.cpp',
  include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
benchmark('Scene frame CPU cost', scene_frame_benchmark, env : ['REMIX_BENCH_SCENE=1'], timeout : 1800,
  verbose : true)
if get_option('gpu_tests')
  measured_sheet_test = executable('remix-measured-sheet-test', 'sheets.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
  test('Path-tracer primary measured sheets, energy and layering', measured_sheet_test,
    args : [meson.current_build_dir() / 'measured-sheet-scenes'],
    env : ['REMIX_FG_PRESENT_METERING=0'], timeout : 600, is_parallel : false)
  test('Path-tracer measured sheet indirect transport and cache', measured_sheet_test,
    args : [meson.current_build_dir() / 'measured-sheet-indirect'],
    env : ['REMIX_FG_PRESENT_METERING=0', 'REMIX_TEST_SHEET_INDIRECT=1'],
    timeout : 600, is_parallel : false)
  subdir('upscale')
  if system != 'windows'
    process_exit_test = executable('remix-tracing-exit-test', 'process_exit.cpp',
      include_directories : [test_includes, include_directories('../../samples')],
      dependencies : [runtime_test_dep])
    foreach mode : ['off', 'fsr31', 'sr', 'rr']
      test('Active tracing process exit ' + mode, process_exit_test,
        args : [mode], timeout : 600, is_parallel : false)
    endforeach
  endif
  soft_sprites_test = executable('remix-soft-sprites-test', 'soft_sprites.cpp',
    include_directories : test_includes, dependencies : [filesystem_dep] + [runtime_test_dep])
  foreach reconstruction : ['off', 'fsr31']
    test('Soft sprite overlays over a traced scene ' + reconstruction, soft_sprites_test,
      args : [meson.current_build_dir() / ('soft-sprites-' + reconstruction)],
      env : reconstruction == 'off' ? [] : ['REMIX_TEST_SOFT_RECONSTRUCTION=' + reconstruction],
      timeout : 300, is_parallel : false)
  endforeach
  alpha_coverage_test = executable('remix-alpha-coverage-test', 'alpha_coverage.cpp',
    include_directories : test_includes, dependencies : [filesystem_dep] + [runtime_test_dep])
  foreach reconstruction : ['off', 'fsr31', 'dlss']
    test('Decal alpha coverage and shadows ' + reconstruction, alpha_coverage_test,
      args : [meson.current_build_dir() / ('alpha-coverage-' + reconstruction)],
      env : reconstruction == 'off' ? [] : ['REMIX_TEST_ALPHA_RECONSTRUCTION=' + reconstruction],
      timeout : 300, is_parallel : false)
  endforeach
  foreach reconstruction : ['off', 'fsr31', 'dlss']
    test('Tagged decal material coverage and receiver depth ' + reconstruction, alpha_coverage_test,
      args : [meson.current_build_dir() / ('material-decals-' + reconstruction)],
      env : ['REMIX_TEST_MATERIAL_DECAL=1'] + (reconstruction == 'off' ? [] : ['REMIX_TEST_ALPHA_RECONSTRUCTION=' + reconstruction]),
      timeout : 600, is_parallel : false)
  endforeach
  if get_option('nrd')
    nrd_motion_test = executable('remix-nrd-motion-test', 'nrd_motion.cpp',
      include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
    test('NRD moving camera guide agreement', nrd_motion_test,
      args : [meson.current_build_dir() / 'nrd-motion'], timeout : 300, is_parallel : false)
  endif
  layered_depth_test = executable('remix-layered-depth-test', 'layered_depth.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
  test('Raster primary close cutout depth ordering', layered_depth_test,
    args : [meson.current_build_dir() / 'layered-depth'], timeout : 180, is_parallel : false)
  guide_cells_test = executable('remix-guide-cells-test', 'guide_cells.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
  test('Colored guide cells and linear lighting energy', guide_cells_test,
    args : [meson.current_build_dir() / 'guide-cell-scenes'], timeout : 300)
  debug_views_test = executable('remix-debug-views-test', 'debug_views.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
  test('Live diagnostics preserve lighting histories', debug_views_test,
    args : [meson.current_build_dir() / 'debug-view-scenes'], timeout : 300)
  test('PBR local diagnostics preserve zero-bounce lighting', debug_views_test,
    args : [meson.current_build_dir() / 'debug-view-pbr-scenes', 'pbr'], timeout : 300)
  adaptive_sampling_test = executable('remix-adaptive-sampling-test', 'adaptive_sampling.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
  test('Adaptive light budget continuity and configured floors', adaptive_sampling_test,
    args : [meson.current_build_dir() / 'adaptive-sampling-scenes'],
    env : ['REMIX_EMISSIVE_HIT_DISTANCE=budget'], timeout : 180)
  texture_history_test = executable('remix-texture-history-test', 'texture_history.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
  test('FSR texture history at translated world coordinates', texture_history_test,
    args : [meson.current_build_dir() / 'texture-history-scenes'], timeout : 240)
  upscale_test = executable('remix-upscale-test', 'upscale.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
  test('FSR 3.1 presets and temporal composition', upscale_test,
    args : [meson.current_build_dir() / 'fsr-scenes'], timeout : 300)
  test('PBR denoising with FSR presets motion and resizing', upscale_test,
    args : [meson.current_build_dir() / 'pbr-fsr-scenes'], env : ['REMIX_TEST_PBR=1'], timeout : 300)
  foreach mode : ['fsr31', 'sr', 'rr']
    test('Wide-range moving camera reconstruction ' + mode, upscale_test,
      args : [meson.current_build_dir() / ('camera-range-' + mode)],
      env : ['REMIX_TEST_CAMERA_RANGE=1'] + (mode == 'fsr31' ? [] : ['REMIX_TEST_NGX=' + mode]),
      timeout : 300, is_parallel : false)
  endforeach
  if system == 'linux'
    foreach fault : ['fsr-allocation', 'fsr-pipeline']
      test('FSR 3.1 fallback ' + fault, upscale_test,
        args : [meson.current_build_dir() / fault],
        env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=' + fault], timeout : 90)
    endforeach
  endif
  guide_boundary_test = executable('remix-guide-boundary-test', 'guide_boundary.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
  test('Torch guide translation and sparse stationary feedback', guide_boundary_test,
    args : [meson.current_build_dir() / 'guide-boundary-scenes'], timeout : 180)
  torch_stability_test = executable('remix-torch-stability-test', 'torch_stability.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
  test('Four torches and motion stability', torch_stability_test,
    args : [meson.current_build_dir() / 'torch-stability-scenes'], timeout : 300)
  test('Mixed emissive and torch sampling stability', torch_stability_test,
    args : [meson.current_build_dir() / 'mixed-light-scenes'],
    env : ['REMIX_TEST_MIXED_LIGHTS=1', 'REMIX_TEST_ADAPTIVE_LOCAL=1'], timeout : 300)
  test('PBR mixed local lighting energy and motion', torch_stability_test,
    args : [meson.current_build_dir() / 'pbr-mixed-light-scenes'],
    env : ['REMIX_TEST_MIXED_LIGHTS=1', 'REMIX_TEST_ADAPTIVE_LOCAL=1', 'REMIX_TEST_PBR=1'], timeout : 300)
  test('PBR combined direct pass energy and motion', torch_stability_test,
    args : [meson.current_build_dir() / 'pbr-combined-pass-scenes'],
    env : ['REMIX_TEST_MIXED_LIGHTS=1', 'REMIX_TEST_ADAPTIVE_LOCAL=1', 'REMIX_TEST_PBR=1', 'REMIX_TEST_SPLIT_LOCAL=3'], timeout : 300)
  foreach blue : ['0', '1']
    test('Soft torch sampler energy and history mode ' + blue, torch_stability_test,
      args : [meson.current_build_dir() / ('soft-torch-noise-scenes-' + blue)],
      env : ['REMIX_TEST_LOCAL_NOISE=' + blue, 'REMIX_TEST_SOFT_TORCH=1',
             'REMIX_TEST_SIGNAL=0'], timeout : 300)
    test('Local emitter sampler energy and history mode ' + blue, torch_stability_test,
      args : [meson.current_build_dir() / ('local-noise-scenes-' + blue)],
      env : ['REMIX_TEST_LOCAL_NOISE=' + blue, 'REMIX_TEST_MIXED_LIGHTS=1',
             'REMIX_TEST_ADAPTIVE_LOCAL=1', 'REMIX_TEST_PBR=1', 'REMIX_TEST_SPLIT_LOCAL=3',
             'REMIX_TEST_SIGNAL=0'], timeout : 300)
  endforeach
  if system == 'linux'
    test('Local blue-noise allocation fallback', torch_stability_test,
      args : [meson.current_build_dir() / 'local-blue-noise-fallback'],
      env : ['REMIX_TEST_LOCAL_NOISE=1', 'REMIX_TEST_MIXED_LIGHTS=1', 'REMIX_TEST_PBR=1',
             'REMIX_TEST_SIGNAL=0', 'REMIX_TEST_PRIMARY=raster',
             'LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=local-blue-noise-allocation'], timeout : 300)
  endif
  test('PBR local lighting with SHARC', torch_stability_test,
    args : [meson.current_build_dir() / 'pbr-sharc-scenes'],
    env : ['REMIX_TEST_MIXED_LIGHTS=1', 'REMIX_TEST_ADAPTIVE_LOCAL=1', 'REMIX_TEST_PBR=1',
           'REMIX_TEST_SPLIT_LOCAL=3', 'REMIX_TEST_SHARC=1', 'REMIX_TEST_SIGNAL=1'], timeout : 600)
  foreach pbr : ['0', '1']
    test('Separate indirect pass two bounce material ' + pbr, torch_stability_test,
      args : [meson.current_build_dir() / ('split-indirect-scenes-' + pbr)],
      env : ['REMIX_TEST_MIXED_LIGHTS=1', 'REMIX_TEST_ADAPTIVE_LOCAL=1',
             'REMIX_TEST_SPLIT_LOCAL=3', 'REMIX_TEST_SIGNAL=1', 'REMIX_TEST_BOUNCES=2',
             'REMIX_TEST_SHARC=1', 'REMIX_SPLIT_INDIRECT=1'] + (pbr == '1' ? ['REMIX_TEST_PBR=1'] : []), timeout : 360)
  endforeach
  if system == 'linux'
    foreach fault : ['indirect-allocation', 'indirect-consumer']
      test('Separate indirect fallback ' + fault, torch_stability_test,
        args : [meson.current_build_dir() / fault],
        env : ['REMIX_TEST_MIXED_LIGHTS=1', 'REMIX_TEST_PBR=1', 'REMIX_TEST_SPLIT_LOCAL=3',
               'REMIX_TEST_SIGNAL=1', 'REMIX_TEST_BOUNCES=2', 'REMIX_TEST_PRIMARY=raster',
               'REMIX_SPLIT_INDIRECT=1', 'LD_PRELOAD=' + faults.full_path(),
               'REMIX_TEST_FAULT=' + fault], timeout : 300)
    endforeach
  endif
  test('Separate indirect multisample transparent composition', torch_stability_test,
    args : [meson.current_build_dir() / 'split-indirect-transparency'],
    env : ['REMIX_TEST_MIXED_LIGHTS=1', 'REMIX_TEST_PBR=1', 'REMIX_TEST_SPLIT_LOCAL=3',
           'REMIX_TEST_SIGNAL=2', 'REMIX_TEST_BOUNCES=2', 'REMIX_TEST_SAMPLES=2',
           'REMIX_SPLIT_INDIRECT=1'], timeout : 360)
  test('Experimental PBR local reuse and reference fallback', torch_stability_test,
    args : [meson.current_build_dir() / 'pbr-reuse-scenes'],
    env : ['REMIX_TEST_MIXED_LIGHTS=1', 'REMIX_TEST_PBR=1', 'REMIX_TEST_PBR_REUSE=1',
           'REMIX_TEST_SPLIT_LOCAL=3', 'REMIX_PBR_REUSE_DIAGNOSTICS=' + meson.current_build_dir() / 'pbr-reuse-counters.csv'], timeout : 300)
  test('Near emissive solid-angle sampling and motion', torch_stability_test,
    args : [meson.current_build_dir() / 'angular-emission-scenes'],
    env : ['REMIX_TEST_MIXED_LIGHTS=1', 'REMIX_TEST_SOLID_ANGLE=1'], timeout : 360)
  foreach angular : ['0', '1']
    test('Broad near emitter reference mode ' + angular, torch_stability_test,
      args : [meson.current_build_dir() / ('near-emitter-scenes-' + angular)],
      env : ['REMIX_TEST_MIXED_LIGHTS=1', 'REMIX_TEST_NEAR_EMITTERS=1', 'REMIX_TEST_SOLID_ANGLE=' + angular], timeout : 360)
  endforeach
  test('Many emissive clusters and sampling stability', torch_stability_test,
    args : [meson.current_build_dir() / 'emissive-cluster-scenes'],
    env : ['REMIX_TEST_MIXED_LIGHTS=1', 'REMIX_TEST_EMISSIVE_CLUSTERS=1', 'REMIX_TEST_ADAPTIVE_LOCAL=1'], timeout : 300)
  foreach mode : ['1', '2', '3']
    test('Separate direct histories mode ' + mode, torch_stability_test,
      args : [meson.current_build_dir() / ('separate-light-scenes-' + mode)],
      env : ['REMIX_TEST_MIXED_LIGHTS=1', 'REMIX_TEST_ADAPTIVE_LOCAL=1', 'REMIX_TEST_SPLIT_LOCAL=' + mode], timeout : 360)
  endforeach
  water_indirect_test = executable('remix-water-indirect-test', 'water_indirect.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
  test('Water indirect reuse and reference energy', water_indirect_test,
    args : [meson.current_build_dir() / 'water-indirect-scenes'], timeout : 240)
  local_light_test = executable('remix-local-light-test', 'local_lights.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
  test('Local sphere lights and many-light hierarchy', local_light_test,
    args : [meson.current_build_dir() / 'local-light-scenes'], timeout : 300)
  shadow_test = executable('remix-shadow-test', 'shadows.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
  test('Path-tracer measured sheet shadow visibility', shadow_test,
    args : [meson.current_build_dir() / 'sheet-shadow-scenes'],
    env : ['REMIX_TEST_SHEET_VISIBILITY=1', 'REMIX_FG_PRESENT_METERING=0'], timeout : 480, is_parallel : false)
  test('SIGMA sun penumbra and temporal separation', shadow_test,
    args : [meson.current_build_dir() / 'shadow-scenes'], timeout : 240)
  test('SIGMA hashed shadow comparison', shadow_test,
    args : [meson.current_build_dir() / 'shadow-white-scenes'],
    env : ['REMIX_TEST_WHITE_SHADOWS=1'], timeout : 240)
  if system == 'linux'
    test('Blue-noise shadow allocation fallback', shadow_test,
      args : [meson.current_build_dir() / 'blue-noise-fallback'],
      env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=blue-noise-allocation'], timeout : 120)
    test('Local visibility guiding resource fallback', local_light_test,
      args : [meson.current_build_dir() / 'guiding-fallback'],
      env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=light-guiding-allocation'], timeout : 90)
    test('Local visibility guiding shader fallback', local_light_test,
      args : [meson.current_build_dir() / 'guiding-shader-fallback'],
      env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=light-guiding-compute'], timeout : 90)
    test('Local light resource fallback', local_light_test,
      args : [meson.current_build_dir() / 'local-fallback'],
      env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=local-descriptors'], timeout : 90)
    foreach fault : ['sigma-allocation', 'sigma-descriptor-limit', 'sigma-compute']
      test('SIGMA fallback ' + fault, shadow_test,
        args : [meson.current_build_dir() / fault],
        env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=' + fault], timeout : 60)
    endforeach
  endif
  sharc_test = executable('remix-sharc-test', 'sharc.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
  test('SHARC diffuse cache and temporal changes', sharc_test,
    args : [meson.current_build_dir() / 'sharc-scenes'], timeout : 240)
  test('SHARC PBR cache materials and temporal changes', sharc_test,
    args : [meson.current_build_dir() / 'sharc-pbr-scenes'],
    env : ['REMIX_TEST_SHARC_PBR=1'], timeout : 600)
  if system == 'linux'
    foreach fault : ['sharc-compute', 'sharc-capability', 'sharc-allocation']
      test('SHARC fallback ' + fault, sharc_test,
        args : [meson.current_build_dir() / fault],
        env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=' + fault], timeout : 45)
    endforeach
  endif
  environment_test = executable('remix-environment-test', 'environment.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
  test('Sky illumination and temporal changes', environment_test,
    args : [meson.current_build_dir() / 'sky-scenes'], timeout : 240)
  test('Emissive maps and sampled area lights', environment_test,
    args : [meson.current_build_dir() / 'emission-scenes'], env : ['REMIX_TEST_EMISSION=1'], timeout : 240)
  test('Stable emissive casting preserves maps and controls', environment_test,
    args : [meson.current_build_dir() / 'stable-emission-scenes'],
    env : ['REMIX_TEST_EMISSION=1', 'REMIX_TEST_EMISSION_STABILITY=1'], timeout : 240)
  test('Prism and gamma display transfer', environment_test,
    args : [meson.current_build_dir() / 'prism-scenes'], env : ['REMIX_TEST_DISPLAY=1'], timeout : 240)
  if system == 'linux'
    test('Prism and gamma UNORM display transfer', environment_test,
      args : [meson.current_build_dir() / 'prism-unorm-scenes'],
      env : ['REMIX_TEST_DISPLAY=1', 'LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=unorm-swapchain'], timeout : 240)
  endif
  pathtrace_test = executable('remix-pathtrace-test', 'rendering.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
  test('Opaque traversal shared mesh and alpha replacement', pathtrace_test,
    args : [meson.current_build_dir() / 'opacity-scenes', 'opaque-variants'], timeout : 180)
  test('Compatible mesh updates and queued version isolation', pathtrace_test,
    args : [meson.current_build_dir() / 'mesh-update-scenes', 'mesh-update'], timeout : 120)
  test('Refreshed mesh geometry refits in place', pathtrace_test,
    args : [meson.current_build_dir() / 'mesh-refresh-scenes', 'mesh-refresh'], timeout : 120)
  test('Geometry content reuse and shading history invalidation', pathtrace_test,
    args : [meson.current_build_dir() / 'geometry-history-scenes', 'geometry-history'], timeout : 120)
  test('Path tracing capabilities and GPU scenes', pathtrace_test,
    args : [meson.current_build_dir() / 'pathtrace-scenes'], timeout : 180)
  test('Diffuse SH convergence against raw reference', pathtrace_test,
    args : [meson.current_build_dir() / 'denoise-quality'], env : ['REMIX_TEST_DENOISE_QUALITY=1'], timeout : 240)
  test('TAA stationary edge and disabled jitter', pathtrace_test,
    args : [meson.current_build_dir() / 'taa-stability'], env : ['REMIX_TEST_TAA_STABILITY=1'], timeout : 180)
  test('TAA stationary edge with shared atlas animation', pathtrace_test,
    args : [meson.current_build_dir() / 'taa-atlas-stability'],
    env : ['REMIX_TEST_TAA_STABILITY=1', 'REMIX_TEST_TAA_ATLAS=1'], timeout : 180)
  test('FSR 3.1 stationary edge alignment', pathtrace_test,
    args : [meson.current_build_dir() / 'fsr-edge'], env : ['REMIX_TEST_FSR_STABILITY=1'], timeout : 180)
  test('FSR 3.1 stationary edge with shared atlas animation', pathtrace_test,
    args : [meson.current_build_dir() / 'fsr-atlas-edge'],
    env : ['REMIX_TEST_FSR_STABILITY=1', 'REMIX_TEST_TAA_ATLAS=1'], timeout : 180)
  test('Tracing presentation and lifecycle', runtime_test, args : [remix.full_path(), 'gpu'],
    env : ['REMIX_TEST_BACKEND=pathtrace'], timeout : 180)
  test('Pending frames and rendering resource replacement', runtime_test,
    args : [remix.full_path(), 'gpu'],
    env : ['REMIX_TEST_BACKEND=pathtrace', 'REMIX_TEST_FRAME_CHANGES=1'], timeout : 600)
  if system != 'windows'
    test('Tracing live process exit cleanup', runtime_test, args : [remix.full_path(), 'exit'],
      env : ['REMIX_TEST_BACKEND=pathtrace'], timeout : 30)
  endif
  if system == 'linux'
    foreach fault : ['submit', 'device-lost']
      test('Tracing failure cleanup ' + fault, runtime_test, args : [remix.full_path(), 'fault'],
        env : ['REMIX_TEST_BACKEND=pathtrace', 'LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=' + fault], timeout : 30)
    endforeach
    test('Temporal descriptor limits retain raw tracing', pathtrace_test,
      args : [meson.current_build_dir() / 'descriptor-limit-scenes'],
      env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=temporal-descriptor-limit'], timeout : 45)
    test('Temporal allocation failure retains raw tracing', pathtrace_test,
      args : [meson.current_build_dir() / 'pathtrace-scenes', 'denoise-failure'],
      env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=denoise-allocation'], timeout : 45)
    test('Tracing initialization failure fallback', pathtrace_test,
      args : [meson.current_build_dir() / 'pathtrace-scenes', 'init-failure'],
      env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=compute'], timeout : 30)
    test('Tracing transparent pipeline failure fallback', pathtrace_test,
      args : [meson.current_build_dir() / 'pathtrace-scenes', 'init-failure'],
      env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=transparency-compute'], timeout : 30)
  endif
endif
buffer_cache_test = executable('remix-buffer-cache-test', 'buffer_cache.cpp',
  include_directories : test_includes, dependencies : vulkan_dep, cpp_args : ['-UNDEBUG'])
test('Upload buffer cache ownership and bounds', buffer_cache_test)
eon_test = executable('remix-eon-test', 'eon.cpp', include_directories : native_include)
test('EON furnace reciprocity and sampling PDFs', eon_test)
if get_option('gpu_tests')
  pbr_render_test = executable('remix-pbr-render-test', 'pbr.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
  test('EON PBR maps and primary visibility agreement', pbr_render_test,
    args : [meson.current_build_dir() / 'pbr-scenes'], timeout : 300)
  pbr_denoise_test = executable('remix-pbr-denoise-test', 'pbr_denoise.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
  test('PBR denoising noise energy and camera cuts', pbr_denoise_test,
    args : [meson.current_build_dir() / 'pbr-denoise-scenes'], timeout : 300)
  foreach primary : ['raster', 'ray']
    test('Opaque blended texels retain NRD ' + primary, pbr_denoise_test,
      args : [meson.current_build_dir() / ('pbr-blend-' + primary)],
      env : ['REMIX_TEST_OPAQUE_BLEND=1'] + (primary == 'ray' ? ['REMIX_TEST_RAY_PRIMARY=1'] : []),
      timeout : 600, is_parallel : false)
    test('Fractional decal material retains NRD ' + primary, pbr_denoise_test,
      args : [meson.current_build_dir() / ('pbr-decal-' + primary)],
      env : ['REMIX_TEST_MATERIAL_DECAL=1'] + (primary == 'ray' ? ['REMIX_TEST_RAY_PRIMARY=1'] : []),
      timeout : 600, is_parallel : false)
  endforeach
endif

if get_option('gpu_tests')
  foreach mode : ['sr', 'rr', 'auto']
    test('DLSS ' + mode + ' reconstruction', upscale_test,
      args : [meson.current_build_dir() / ('dlss-' + mode)], env : ['REMIX_TEST_NGX=' + mode], timeout : 600)
  endforeach
  if system == 'linux'
    foreach fault : ['unavailable', 'init', 'allocation', 'rr-unavailable', 'sr-unavailable', 'rr-create', 'sr-create', 'rr-evaluate', 'sr-evaluate']
      mode = fault.startswith('sr-') ? 'sr' : 'rr'
      test('DLSS fallback ' + fault, upscale_test,
        args : [meson.current_build_dir() / ('dlss-' + fault)],
        env : ['REMIX_TEST_NGX=' + mode, 'REMIX_NGX_FAULT=' + fault], timeout : 180)
    endforeach
  endif
endif

if get_option('gpu_tests')
  foreach primary : ['raster', 'ray']
    test('DLSS RR PBR references ' + primary, pbr_denoise_test,
      args : [meson.current_build_dir() / ('rr-pbr-' + primary)],
      env : ['REMIX_TEST_RR=1'] + (primary == 'ray' ? ['REMIX_TEST_RAY_PRIMARY=1'] : []), timeout : 600)
  endforeach
endif

if get_option('gpu_tests')
  foreach mode : ['sr', 'rr']
    test('DLSS ' + mode + ' texture history and animated objects', texture_history_test,
      args : [meson.current_build_dir() / ('dlss-textures-' + mode)],
      env : ['REMIX_TEST_NGX=' + mode], timeout : 300)
  endforeach
endif

if get_option('gpu_tests')
  test('DLSS RR dense mixed lighting and saved settings', upscale_test,
    args : [meson.current_build_dir() / 'dlss-dense'],
    env : ['REMIX_TEST_RR_DENSE=1'], timeout : 300)
endif

if get_option('gpu_tests')
  test('DLSS RR water reference and reuse settings', water_indirect_test,
    args : [meson.current_build_dir() / 'dlss-water'],
    env : ['REMIX_TEST_RR_REUSE=1'], timeout : 300)
endif

if get_option('gpu_tests')
  fireflies_test = executable('remix-fireflies-test', 'fireflies.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
  test('Local firefly controls HDR and history resets', fireflies_test,
    args : [meson.current_build_dir() / 'firefly-scenes'],
    env : ['REMIX_TEST_FIREFLY_RESETS=1'], timeout : 300)
  test('Adaptive indirect controls and history resets', fireflies_test,
    args : [meson.current_build_dir() / 'adaptive-indirect-scenes'],
    env : ['REMIX_TEST_ROULETTE_RESETS=1'], timeout : 300)
endif

if get_option('gpu_tests')
  foreach family : ['torch', 'emissive', 'mixed']
    test('Firefly outgoing luminance ' + family, fireflies_test,
      args : [meson.current_build_dir() / ('firefly-' + family)],
      env : ['REMIX_TEST_FIREFLY_FAMILY=' + family, 'REMIX_TEST_FIREFLY_DIRECT=0.1',
             'REMIX_TEST_FIREFLY_INDIRECT=0.05', 'REMIX_TEST_FIREFLY_LOCAL_SAMPLES=4',
             'REMIX_TEST_FIREFLY_PRIMARY=ray', 'REMIX_TEST_FIREFLY_SPLIT=2'], timeout : 300)
  endforeach
endif

if get_option('gpu_tests')
  usd_lights_gpu = executable('remix-usd-lights-gpu-test', 'usd_lights.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
  test('Authored USD light GPU fixtures', usd_lights_gpu,
    args : [meson.current_build_dir() / 'usd-lights'], timeout : 300, is_parallel : false)
endif

if get_option('gpu_tests') and get_option('nrd')
  translucency_test = executable('remix-translucency-test', 'translucency.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep] + [runtime_test_dep])
  test('Checked translucent opaque reuse energy and rays', translucency_test,
    args : [meson.current_build_dir() / 'translucency-reuse'],
    env : ['REMIX_TEST_TRANSLUCENCY_REUSE=1'], timeout : 600, is_parallel : false)
  foreach primary : ['raster', 'ray']
    test('Translucent surface denoising and motion ' + primary, translucency_test,
      args : [meson.current_build_dir() / ('translucency-' + primary)],
      env : primary == 'ray' ? ['REMIX_TEST_RAY_PRIMARY=1'] : [], timeout : 600, is_parallel : false)
  endforeach
endif

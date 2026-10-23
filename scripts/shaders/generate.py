#!/usr/bin/env python3
"""Regenerate or verify embedded renderer SPIR-V; normal builds need no shader tools."""
import argparse
import lzma
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import nrd
import fsr31

ROOT = Path(__file__).resolve().parents[2]
# Backends whose SPIR-V is optimized with spirv-opt -O (fully inlined) and stored
# XZ-compressed in embedded.bin. Inlined modules are ~10x larger, but NVIDIA's
# driver compiles them about twice as fast; the runtime decompresses on use.
OPTIMIZED = {'pathtrace'}
# Offline transport benefits from inlining too, but retains its word-array ABI.
INLINED = {'bake'}
BACKENDS = {
    'probe_batch_test': ('forward::test::probe_batch_input', 'vulkan1.1', ('query',)),
    'probe_batch': ('forward::probe_batch', 'vulkan1.1', ('gather',)),
    'sun_depth_test': ('forward::test::sun_depth_input', 'vulkan1.1', ('query',)),
    'cluster_test': ('forward::test::cluster_input', 'vulkan1.1', ('query',)),
    'shadow_test': ('forward::test::shadow_input', 'vulkan1.1', ('query',)),
    'style_test': ('forward::test::style_input', 'vulkan1.1', ('query',)),
    'probe_test': ('forward::test::probe_input', 'vulkan1.1', ('reflection',)),
    'mask_test': ('forward::test::mask_input', 'vulkan1.1', ('decode',)),
    'water_ray': ('forward::water::hardware', 'vulkan1.2', ('frag',)),
    'water_test': ('forward::test::water_input', 'vulkan1.1', ('vert', 'frag', 'query', 'depth')),
    'water': ('forward::water', 'vulkan1.1', ('snapshot', 'snapshotSingle', 'vert', 'frag', 'overlayVert', 'overlayFrag', 'compositeFrag', 'compositeSingle')),
    'live_gi': ('forward::ray::gi', 'vulkan1.2', ('frag', 'guides')),
    'live_ray': ('forward::ray', 'vulkan1.2', ('launch', 'merge')),
    'bake': ('forward::bake', 'vulkan1.2', ('atlas', 'transport', 'parallel', 'visibility', 'aggregate', 'aggregateParallel', 'depth')),
    'forward_test': ('forward::test', 'vulkan1.2', ('comp', 'probes', 'water')),
    'forward': ('forward', 'vulkan1.1', ('vert', 'lightmapVert', 'frag', 'prepassedFrag', 'viewModelFrag', 'cluster', 'shadowVert', 'shadowFrag', 'opaqueDepthFrag', 'localShadowVert', 'localShadowFrag', 'compositeVert', 'compositeFrag', 'compositeSingle', 'ssrFrag', 'prepassedSsrFrag', 'ssrResolve', 'ssrResolveSingle', 'ssrReduce', 'ssrTrace', 'ssrFilter')),
    'postfx': ('postfx', 'vulkan1.1', ('meter', 'resolve', 'down', 'up')),
    'upscale': ('upscale', 'vulkan1.2', ('comp', 'vert', 'frag')),
    'raster': ('raster', 'vulkan1.1', ('vert', 'frag', 'haloMask', 'haloVert', 'haloBlur', 'haloComposite', 'maskWithin', 'maskOverlay', 'spriteVert', 'spriteFrag')),
    'pathtrace': ('trace', 'vulkan1.2', ('vert', 'frag', 'comp', 'temporal', 'transparency', 'temporalTransparency', 'frontGuides', 'psr', 'regirPresample', 'volumeUpdate', 'orcaSparse', 'sharcOrcaSparse', 'singleTransparency', 'singleTemporalTransparency', 'sharcSingleTransparency', 'sharcSingleTemporalTransparency', 'confidence', 'gbufferVert', 'gbufferFrag', 'depthFrag', 'sharcUpdate', 'sharcResolve', 'sharcQuery', 'sharcTransparency', 'sharcTemporal', 'sharcTemporalTransparency', 'sharcConfidence', 'sigmaTemporal', 'sharcSigmaTemporal', 'localTemporal', 'localSigmaTemporal', 'sharcLocalTemporal', 'sharcLocalSigmaTemporal', 'splitLocalTemporal', 'splitLocalSigmaTemporal', 'sharcSplitLocalTemporal', 'sharcSplitLocalSigmaTemporal', 'splitLocalDirect', 'splitRestirInitial', 'splitRestirSpatial', 'indirectTemporal', 'sharcIndirectTemporal', 'vertexAssemble', 'sparseCompact', 'sparseArgs', 'indirect', 'sharcIndirect', 'sheetLighting', 'sheetLightingTemporal', 'sharcSheetLighting', 'sharcSheetLightingTemporal')),
    'denoise': ('denoise', 'vulkan1.2', ('comp', 'sigma', 'local', 'localSigma', 'splitLocal', 'splitLocalSigma')),
}


def shader_directory(backend):
    if backend == 'probe_batch_test':
        return ROOT / 'tests/forward/probe_batch_shaders'
    if backend == 'probe_batch':
        return ROOT / 'src/native/forward/probe_batch/shaders'
    if backend == 'sun_depth_test':
        return ROOT / 'tests/forward/sun_depth_shaders'
    if backend == 'cluster_test':
        return ROOT / 'tests/forward/cluster_shaders'
    if backend == 'shadow_test':
        return ROOT / 'tests/forward/shadow_shaders'
    if backend == 'probe_test':
        return ROOT / 'tests/forward/probe_shaders'
    if backend == 'style_test':
        return ROOT / 'tests/forward/style_shaders'
    if backend == 'mask_test':
        return ROOT / 'tests/forward/mask_shaders'
    if backend == 'water_ray':
        return ROOT / 'src/native/forward/water/ray_shaders'
    if backend == 'water_test':
        return ROOT / 'tests/forward/water_shaders'
    if backend == 'water':
        return ROOT / 'src/native/forward/water/shaders'
    if backend == 'live_gi':
        return ROOT / 'src/native/forward/ray/gi_shaders'
    if backend == 'live_ray':
        return ROOT / 'src/native/forward/ray/shaders'
    if backend == 'bake':
        return ROOT / 'src/native/forward/bake/shaders'
    if backend == 'forward_test':
        return ROOT / 'tests/forward/shaders'
    return ROOT / 'src/native' / backend / 'shaders'


def defines_for(backend, stage, stages):
    """Preprocessor defines and source file of one embedded stage."""
    directory = shader_directory(backend)
    if backend in ('shadow_test', 'cluster_test', 'sun_depth_test', 'probe_batch_test'):
        return [], directory / 'query.comp'
    if backend == 'probe_test':
        return [], directory / 'reflection.comp'
    defines = ['-DTEMPORAL=1'] if stage in ('temporal', 'temporalTransparency', 'confidence') else []
    if backend == 'probe_batch':
        return [], directory / (stage + '.comp')
    if backend == 'water_test':
        return [], directory / (stage + '.comp' if stage in ('query', 'depth') else 'input.' + stage)
    if backend == 'water':
        forward = ROOT / 'src/native/forward/shaders'
        if stage == 'vert': return [], forward / 'forward.vert'
        if stage == 'frag': return ['-DWATER_SURFACE=1'], forward / 'forward.frag'
        if stage == 'overlayVert': return [], forward / 'composite.vert'
        if stage == 'overlayFrag': return [], directory / 'overlay.frag'
        if stage in ('compositeFrag', 'compositeSingle'):
            return ['-DWATER_VOLUME=1'] + (['-DSINGLE_SAMPLE=1'] if stage == 'compositeSingle' else []), forward / 'composite.frag'
        return (['-DSINGLE_SAMPLE=1'] if stage == 'snapshotSingle' else []), directory / 'snapshot.comp'
    if stage in ('sigma', 'sigmaTemporal', 'sharcSigmaTemporal'): defines += ['-DSIGMA_PASS=1']
    if stage in ('sigmaTemporal', 'sharcSigmaTemporal'): defines += ['-DTEMPORAL=1']
    if stage in ('indirectTemporal', 'sharcIndirectTemporal'): defines += ['-DTEMPORAL=1', '-DINDIRECT_PASS=1']
    if stage in ('indirect', 'sharcIndirect'): defines += ['-DINDIRECT_PASS=1']
    if stage in ('sheetLighting', 'sharcSheetLighting'): defines += ['-DSHEET_LIGHTING_PASS=1']
    if stage in ('sheetLightingTemporal', 'sharcSheetLightingTemporal'): defines += ['-DTEMPORAL=1', '-DSHEET_LIGHTING_PASS=1']
    if stage == 'frontGuides': defines += ['-DTEMPORAL=1', '-DFRONT_GUIDES_PASS=1']
    if stage == 'psr': defines += ['-DTEMPORAL=1', '-DPSR_PASS=1']
    if stage == 'regirPresample': defines += ['-DREGIR_PRESAMPLE_PASS=1']
    if stage == 'volumeUpdate': defines += ['-DVOLUME_UPDATE_PASS=1']
    if stage == 'sparseCompact': defines += ['-DSPARSE_COMPACT_PASS=1']
    if stage == 'sparseArgs': defines += ['-DSPARSE_ARGS_PASS=1']
    if stage in ('orcaSparse', 'sharcOrcaSparse'): defines += ['-DORCA_SPARSE_PASS=1']
    if stage == 'splitLocalDirect': defines += ['-DTEMPORAL=1', '-DSPLIT_LIGHTING_PASS=1']
    if stage == 'splitRestirInitial': defines += ['-DTEMPORAL=1', '-DSPLIT_LIGHTING_PASS=1', '-DLOCAL_PASS=1', '-DRESTIR_INITIAL=1']
    if stage == 'splitRestirSpatial': defines += ['-DTEMPORAL=1', '-DSPLIT_LIGHTING_PASS=1', '-DLOCAL_PASS=1', '-DRESTIR_SPATIAL=1']
    if 'split' in stage.lower(): defines += ['-DSPLIT_LOCAL_PASS=1']
    if 'local' in stage.lower():
        defines += ['-DLOCAL_PASS=1']
        if 'Temporal' in stage: defines += ['-DTEMPORAL=1']
        if 'Sigma' in stage: defines += ['-DSIGMA_PASS=1']
    if 'Single' in stage or stage.startswith('single'):
        defines += ['-DTRANSPARENCY_PASS=1', '-DSINGLE_SHEET_PASS=1']
        if 'Temporal' in stage: defines += ['-DTEMPORAL=1']
    if stage == 'confidence': defines += ['-DCONFIDENCE_PASS=1']
    if stage in ('transparency', 'temporalTransparency'): defines += ['-DTRANSPARENCY_PASS=1']
    if stage.startswith('sharc') and stage not in ('sharcUpdate', 'sharcResolve'):
        defines += ['-DSHARC_QUERY=1']
    if stage in ('sharcTemporal', 'sharcTemporalTransparency', 'sharcConfidence'):
        defines += ['-DTEMPORAL=1']
    if stage == 'sharcConfidence': defines += ['-DCONFIDENCE_PASS=1']
    if stage in ('sharcTransparency', 'sharcTemporalTransparency'): defines += ['-DTRANSPARENCY_PASS=1']
    if stage == 'sharcUpdate': defines += ['-DSHARC_UPDATE_PASS=1']
    source_stage = 'comp' if defines else stage
    source = directory / (backend + '.' + source_stage)
    if backend == 'style_test':
        source = directory / 'query.comp'
    if backend == 'mask_test':
        source = directory / 'decode.comp'
    if backend == 'water_ray':
        source = ROOT / 'src/native/forward/shaders/forward.frag'
        defines = ['-DWATER_SURFACE=1', '-DWATER_RAY=1']
    if backend == 'live_gi':
        source = ROOT / 'src/native/forward/shaders/forward.frag'
        defines = ['-DLIVE_GI=1'] + (['-DREFLECTION_GUIDES=1'] if stage == 'guides' else [])
    if backend == 'live_ray':
        source = directory / 'screen.comp'
        defines = ['-DRAY_LAUNCH=1'] if stage == 'launch' else []
    if backend == 'bake':
        source = directory / ('transport.comp' if stage in ('parallel', 'visibility', 'aggregate', 'aggregateParallel') else stage + '.comp')
        if stage in ('parallel', 'visibility', 'aggregateParallel'): defines = ['-DPARALLEL_BAKE=1']
        if stage == 'visibility': defines += ['-DCOMPACT_VISIBILITY=1']
        if stage in ('aggregate', 'aggregateParallel'): defines += ['-DAGGREGATE_DIFFUSE=1']
    if backend == 'forward_test':
        source = directory / ('ray-scene.comp' if stage == 'comp' else stage + '.comp')
    if backend == 'forward':
        source = directory / {
            'vert': 'forward.vert', 'lightmapVert': 'forward.vert', 'frag': 'forward.frag', 'cluster': 'cluster.comp',
            'shadowVert': 'forward.vert', 'shadowFrag': 'forward.frag', 'opaqueDepthFrag': 'depth.frag',
            'localShadowVert': 'forward.vert', 'localShadowFrag': 'forward.frag',
            'compositeVert': 'composite.vert', 'compositeFrag': 'composite.frag',
            'compositeSingle': 'composite.frag',
            'ssrFrag': 'forward.frag', 'prepassedFrag': 'forward.frag',
            'prepassedSsrFrag': 'forward.frag', 'viewModelFrag': 'forward.frag', 'ssrResolve': 'reflections.comp',
            'ssrResolveSingle': 'reflections.comp', 'ssrReduce': 'reflections.comp',
            'ssrTrace': 'reflections.comp', 'ssrFilter': 'reflections.comp',
        }[stage]
        if stage == 'lightmapVert': defines = ['-DSURFACE_LIGHTMAP=1']
        if stage in ('shadowVert', 'shadowFrag'): defines = ['-DSHADOW_PASS=1']
        if stage in ('localShadowVert', 'localShadowFrag'): defines = ['-DSHADOW_PASS=1', '-DLOCAL_SHADOW_PASS=1']
        if stage == 'compositeSingle': defines = ['-DSINGLE_SAMPLE=1']
        if stage in ('ssrFrag', 'prepassedSsrFrag'): defines = ['-DREFLECTION_GUIDES=1']
        if stage in ('prepassedFrag', 'prepassedSsrFrag'): defines += ['-DPREPASSED_WORLD=1']
        if stage == 'viewModelFrag': defines = ['-DVIEWMODEL_PASS=1']
        if stage in ('ssrResolve', 'ssrResolveSingle'): defines = ['-DSSR_RESOLVE=1']
        if stage == 'ssrResolveSingle': defines += ['-DSINGLE_SAMPLE=1']
        if stage == 'ssrReduce': defines = ['-DSSR_REDUCE=1']
        if stage == 'ssrTrace': defines = ['-DSSR_TRACE=1']
        if stage == 'ssrFilter': defines = ['-DSSR_FILTER=1']
    if backend == 'postfx':
        source = directory / 'postfx.comp'
        defines += ['-DPOST_PASS=' + str(stages.index(stage))]
    if stage in ('gbufferVert', 'gbufferFrag', 'depthFrag'):
        source = directory / ('gbuffer.vert' if stage == 'gbufferVert' else 'gbuffer.frag')
        if stage == 'depthFrag': defines += ['-DSTABLE_DEPTH=1']
    if stage.startswith('halo'):
        # Halo outlines: the raster fragment shader as a silhouette mask, and
        # the fullscreen blur and composite passes.
        defines = ['-D' + {'haloMask': 'HALO_MASK', 'haloVert': 'HALO_VERT', 'haloBlur': 'HALO_BLUR',
                           'haloComposite': 'HALO_COMPOSITE'}[stage] + '=1']
        source = directory / {'haloMask': 'raster.frag', 'haloVert': 'halo.vert'}.get(stage, 'halo.frag')
    if backend == 'raster' and stage.startswith('mask'):
        # Masked overlays: the raster fragment shader as a coverage source
        # limited to another group's coverage, and as an overlay tested
        # against a group's coverage.
        defines = ['-DHALO_MASK=1', '-DMASK_WITHIN=1'] if stage == 'maskWithin' else ['-DMASK_OVERLAY=1']
        source = directory / 'raster.frag'
    if backend == 'raster' and stage.startswith('sprite'):
        # Sprite overlays (remixapi_InstanceInfoSpriteEXT): the raster stages
        # with sprite vertices, frame and sequence blending and soft depth.
        defines = ['-DSPRITE=1']
        source = directory / ('raster.vert' if stage == 'spriteVert' else 'raster.frag')
    if stage == 'sharcResolve': source = directory / 'sharc-resolve.comp'
    if stage == 'vertexAssemble': source = directory / 'vertex-assemble.comp'
    return defines, source


def compile_stage(backend, stage, temporary):
    """SPIR-V words of one stage, compiled and validated; for an optimized
    backend, the optimized module and its XZ stream."""
    _, environment, stages = BACKENDS[backend]
    defines, source = defines_for(backend, stage, stages)
    binary = Path(temporary) / (backend + '-' + stage + '.spv')
    subprocess.run(['glslangValidator', '-V', '--target-env', environment, *defines,
                    '-I' + str(ROOT/'third_party/nrd/Shaders'), '-I' + str(ROOT/'third_party/sharc/include'),
                    '-o', str(binary), str(source)], check=True, stdout=subprocess.DEVNULL)
    if backend in OPTIMIZED or backend in INLINED:
        optimized = binary.with_suffix('.opt.spv')
        subprocess.run(['spirv-opt', '-O', '--target-env=' + environment, str(binary), '-o', str(optimized)],
                       check=True)
        binary = optimized
    subprocess.run(['spirv-val', '--target-env', environment, str(binary)], check=True)
    data = binary.read_bytes()
    if backend in OPTIMIZED:
        return data, compress(data)
    return struct.unpack('<' + 'I' * (len(data) // 4), data)


def compress(data):
    """Deterministic XZ stream (LZMA2, CRC32) that XZ Embedded decodes."""
    return lzma.compress(data, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32,
                         preset=9 | lzma.PRESET_EXTREME)


def decompressed_stages(backend):
    """SPIR-V of every stage from a compressed backend's embedded.h and embedded.bin."""
    directory = shader_directory(backend)
    blob = (directory / 'embedded.bin').read_bytes()
    entries = re.findall(r'\{(\d+)u, (\d+)u, (\d+)u\}, // (\w+)', (directory / 'embedded.h').read_text())
    stages = {}
    for offset, compressed, size, stage in entries:
        data = lzma.decompress(blob[int(offset):int(offset) + int(compressed)])
        if len(data) != int(size):
            raise SystemExit(f'{directory}: {stage} size mismatch')
        stages[stage] = data
    return stages


def validate_stage(backend, stage, text, temporary):
    _, environment, _ = BACKENDS[backend]
    target = shader_directory(backend) / 'embedded.h'
    binary = Path(temporary) / (backend + '-' + stage + '.spv')
    if backend in OPTIMIZED:
        data = text.get(stage)
        if data is None:
            raise SystemExit(f'{target}: missing {stage} shader')
        binary.write_bytes(data)
    else:
        match = re.search(stage + r'Shader\[\] = \{(.*?)\};', text, re.S)
        if not match:
            raise SystemExit(f'{target}: missing {stage} shader')
        words = [int(word, 16) for word in re.findall(r'0x[0-9a-fA-F]+', match[1])]
        binary.write_bytes(struct.pack('<' + 'I' * len(words), *words))
    subprocess.run(['spirv-val', '--target-env', environment, str(binary)], check=True)


def compressed_output(backend, stages, results):
    """embedded.h (stage index) and embedded.bin (XZ streams) of a compressed backend."""
    namespace = BACKENDS[backend][0]
    header = ('// Generated by scripts/shaders/generate.py ' + backend + '; do not edit.\n'
              '// SPIR-V optimized with spirv-opt -O, each stage an XZ stream in embedded.bin.\n'
              '#pragma once\n#include <cstddef>\n#include <cstdint>\nnamespace remix::' + namespace + ' {\n'
              'enum class Stage : uint32_t {\n')
    header += ''.join(f'  {stage},\n' for stage in stages) + '  Count\n};\n'
    header += ('struct EmbeddedShader {\n  size_t offset, compressedBytes, bytes;\n};\n'
               'inline constexpr EmbeddedShader embeddedShaders[] = {\n')
    blob = bytearray()
    for stage in stages:
        data, packed = results[stage]
        header += f'  {{{len(blob)}u, {len(packed)}u, {len(data)}u}}, // {stage}\n'
        blob += packed
    header += '};\ninline constexpr const char *stageNames[] = {\n'
    header += ''.join(f'  "{stage}",\n' for stage in stages) + '};\n}\n'
    return header, bytes(blob)


def process(backends, check, validate):
    """Compiles every stage of the backends concurrently, one task per stage."""
    with tempfile.TemporaryDirectory() as temporary, \
            ThreadPoolExecutor(max_workers=os.cpu_count() or 1) as pool:
        if validate:
            texts = {b: decompressed_stages(b) if b in OPTIMIZED else
                     (shader_directory(b) / 'embedded.h').read_text() for b in backends}
            tasks = [pool.submit(validate_stage, b, stage, texts[b], temporary)
                     for b in backends for stage in BACKENDS[b][2]]
            for task in tasks:
                task.result()
            return
        tasks = {(b, stage): pool.submit(compile_stage, b, stage, temporary)
                 for b in backends for stage in BACKENDS[b][2]}
        for backend in backends:
            namespace, _, stages = BACKENDS[backend]
            target = shader_directory(backend) / 'embedded.h'
            if backend in OPTIMIZED:
                header, blob = compressed_output(backend, stages, {s: tasks[backend, s].result() for s in stages})
                if check:
                    if target.read_text() != header or target.with_suffix('.bin').read_bytes() != blob:
                        raise SystemExit(f'{target}: embedded shaders differ from these shader tools')
                else:
                    target.write_text(header)
                    target.with_suffix('.bin').write_bytes(blob)
                continue
            output = ('// Generated by scripts/shaders/generate.py ' + backend + '; do not edit.\n'
                      '#pragma once\n#include <cstdint>\nnamespace remix::' + namespace + ' {\n')
            for stage in stages:
                words = tasks[backend, stage].result()
                output += 'inline constexpr uint32_t ' + stage + 'Shader[] = {\n'
                for i in range(0, len(words), 8):
                    output += '  ' + ', '.join(f'0x{x:08x}' for x in words[i:i+8]) + ',\n'
                output += '};\n'
            output += '}\n'
            if check:
                if target.read_text() != output:
                    raise SystemExit(f'{target}: embedded shaders differ from this glslang version')
            else:
                target.write_text(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    # metal translates the other backends' SPIR-V; 'all' leaves it out (it needs macOS and Xcode).
    parser.add_argument('backend', choices=[*BACKENDS, 'nrd', 'fsr31', 'metal', 'all'])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--check', action='store_true', help='Compare generated output without modifying files')
    mode.add_argument('--validate', action='store_true', help='Validate checked-in SPIR-V using only spirv-val')
    args = parser.parse_args()
    selected = [*BACKENDS, 'nrd', 'fsr31'] if args.backend == 'all' else [args.backend]
    glsl = [backend for backend in selected if backend in BACKENDS]
    if glsl:
        process(glsl, args.check, args.validate)
    if 'metal' in selected:
        import metal
        metal.process(args.check, args.validate)
    for backend in selected:
        if backend in ('nrd', 'fsr31'):
            (nrd if backend == 'nrd' else fsr31).process(args.check, args.validate)


if __name__ == '__main__':
    main()

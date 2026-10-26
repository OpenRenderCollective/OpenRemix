#!/usr/bin/env python3
"""Generate the renderer's embedded SPIR-V at build time, or regenerate or verify the committed NRD and FSR 3.1 SPIR-V.

The build (src/native/meson.build) runs it with the pinned tools of toolchain.py; its output is not committed."""
import argparse
import json
import lzma
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import re
import struct
import sys
import subprocess
import tempfile
import threading
import nrd
import fsr31

ROOT = Path(__file__).resolve().parents[2]
# Where the generated backends are written and read (--output): the build passes its
# src/native, so the sources' "pathtrace/shaders/embedded.h" includes find them.
OUTPUT = ROOT / '_work/shaders'
# Every file a glslang compile read (--depfile).
DEPENDENCIES = set()
_dependencies_lock = threading.Lock()
# Backends whose SPIR-V is optimized with spirv-opt -O (fully inlined) and stored
# XZ-compressed in embedded.bin. Inlined modules are ~10x larger, but NVIDIA's
# driver compiles them about twice as fast; the runtime decompresses on use.
OPTIMIZED = {'pathtrace'}
BACKENDS = {
    'postfx': ('postfx', 'vulkan1.2', ('meter', 'resolve', 'down', 'up')),
    'upscale': ('upscale', 'vulkan1.2', ('comp', 'vert', 'frag', 'hdr10', 'hdrUi')),
    'raster': ('raster', 'vulkan1.1', ('vert', 'frag', 'haloMask', 'haloVert', 'haloBlur', 'haloComposite', 'maskWithin', 'maskOverlay', 'spriteVert', 'spriteFrag', 'skyComposite')),
    'pathtrace': ('trace', 'vulkan1.2', ('vert', 'frag', 'comp', 'temporal', 'transparency', 'temporalTransparency', 'frontGuides', 'psr', 'regirPresample', 'volumeUpdate', 'orcaSparse', 'sharcOrcaSparse', 'singleTransparency', 'singleTemporalTransparency', 'sharcSingleTransparency', 'sharcSingleTemporalTransparency', 'confidence', 'gbufferVert', 'gbufferFrag', 'depthFrag', 'sharcUpdate', 'sharcResolve', 'sharcQuery', 'sharcTransparency', 'sharcTemporal', 'sharcTemporalTransparency', 'sharcConfidence', 'sigmaTemporal', 'sharcSigmaTemporal', 'localTemporal', 'localSigmaTemporal', 'sharcLocalTemporal', 'sharcLocalSigmaTemporal', 'splitLocalTemporal', 'splitLocalSigmaTemporal', 'sharcSplitLocalTemporal', 'sharcSplitLocalSigmaTemporal', 'splitLocalDirect', 'splitRestirInitial', 'splitRestirSpatial', 'indirectTemporal', 'sharcIndirectTemporal', 'vertexAssemble', 'sparseCompact', 'sparseArgs', 'indirect', 'sharcIndirect', 'sheetLighting', 'sheetLightingTemporal', 'sharcSheetLighting', 'sharcSheetLightingTemporal')),
    'denoise': ('denoise', 'vulkan1.2', ('comp', 'sigma', 'local', 'localSigma', 'splitLocal', 'splitLocalSigma')),
    'particles': ('particles', 'vulkan1.1', ('simulate', 'spawn', 'args', 'vert', 'frag', 'light', 'spawnTraced')),
}
# Stages that need a newer target than their backend's (ray queries: SPIR-V 1.4).
STAGE_ENVIRONMENTS = {('particles', 'light'): 'vulkan1.2', ('particles', 'spawnTraced'): 'vulkan1.2'}


def defines_for(backend, stage, stages):
    """Preprocessor defines and source file of one embedded stage."""
    directory = ROOT / 'src/native' / backend / 'shaders'
    defines = ['-DTEMPORAL=1'] if stage in ('temporal', 'temporalTransparency', 'confidence') else []
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
    if backend == 'upscale' and stage in ('hdr10', 'hdrUi'):
        # HDR10 presentation and its UI brightness (runtime/hdr_output.cpp).
        source = directory / {'hdr10': 'hdr10.frag', 'hdrUi': 'hdr_ui.frag'}[stage]
    if backend == 'raster' and stage == 'skyComposite':
        # The NUMOS sky's fullscreen composite (raster/sky.cpp).
        source = directory / 'sky.frag'
    if backend == 'particles' and stage not in ('vert', 'frag'):
        # Particles: one compute source, a pass per stage; the traced stages
        # with ray queries against the scene.
        defines = ['-DPASS_' + {'simulate': 'SIMULATE', 'spawn': 'SPAWN', 'args': 'ARGS', 'light': 'LIGHT',
                                'spawnTraced': 'SPAWN'}[stage] + '=1']
        if stage in ('light', 'spawnTraced'): defines += ['-DTRACED=1']
        source = directory / 'particles.comp'
    if stage == 'sharcResolve': source = directory / 'sharc-resolve.comp'
    if stage == 'vertexAssemble': source = directory / 'vertex-assemble.comp'
    return defines, source


def glslang(arguments, output):
    """glslangValidator -V, recording the files it read."""
    depfile = output.with_suffix('.d')
    subprocess.run(['glslangValidator', '-V', *arguments, '--depfile', str(depfile), '-o', str(output)],
                   check=True, stdout=subprocess.DEVNULL)
    # "output: input include ...", spaces and Windows drive colons in paths escaped with a backslash.
    # The separator is the first colon before a space.
    text = re.split(r':\s', depfile.read_text(), maxsplit=1)[1].replace('\\\n', ' ')
    paths = [path.replace('\\ ', ' ').replace('\\:', ':') for path in re.findall(r'(?:\\ |[^\s])+', text)]
    depend(*paths)


def depend(*paths):
    """Record inputs of the generated files; a scratch copy (metal.py's mode variants) is not one."""
    resolved = [Path(path).resolve() for path in paths]
    with _dependencies_lock:
        DEPENDENCIES.update(str(path) for path in resolved if path.is_relative_to(ROOT))


def compile_stage(backend, stage, temporary):
    """SPIR-V words of one stage, compiled and validated; for an optimized
    backend, the optimized module and its XZ stream."""
    _, environment, stages = BACKENDS[backend]
    environment = STAGE_ENVIRONMENTS.get((backend, stage), environment)
    defines, source = defines_for(backend, stage, stages)
    binary = Path(temporary) / (backend + '-' + stage + '.spv')
    glslang(['--target-env', environment, *defines, '-I' + str(ROOT/'third_party/nrd/Shaders'),
             '-I' + str(ROOT/'third_party/sharc/include'), str(source)], binary)
    if backend in OPTIMIZED:
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
    directory = OUTPUT / backend / 'shaders'
    blob = (directory / 'embedded.bin').read_bytes()
    entries = re.findall(r'\{(\d+)u, (\d+)u, (\d+)u\}, // (\w+)', (directory / 'embedded.h').read_text())
    stages = {}
    for offset, compressed, size, stage in entries:
        data = lzma.decompress(blob[int(offset):int(offset) + int(compressed)])
        if len(data) != int(size):
            raise SystemExit(f'{directory}: {stage} size mismatch')
        stages[stage] = data
    return stages


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
    header += ''.join(f'  "{stage}",\n' for stage in stages) + '};\n'
    header += '// embedded.bin (embedded_data.cpp).\nextern const unsigned char embeddedData[];\nextern const size_t embeddedDataBytes;\n}\n'
    return header, bytes(blob)


def embedded_data(generator, namespace):
    """embedded_data.cpp: the blob beside it, by #embed (found next to the including file)."""
    return ('// Generated by scripts/shaders/' + generator + '; do not edit.\n'
            '#include <cstddef>\n'
            '#if defined(__clang__)\n'
            '#pragma clang diagnostic ignored "-Wc23-extensions"\n'
            '#pragma clang diagnostic ignored "-Wc++11-narrowing"\n'
            '#endif\n'
            'namespace remix::' + namespace + ' {\n'
            'extern const unsigned char embeddedData[];\nextern const size_t embeddedDataBytes;\n'
            'const unsigned char embeddedData[] = {\n#embed "embedded.bin"\n};\n'
            'const size_t embeddedDataBytes = sizeof(embeddedData);\n}\n')


def process(backends):
    """Compiles every stage of the backends concurrently, one task per stage."""
    with tempfile.TemporaryDirectory() as temporary, \
            ThreadPoolExecutor(max_workers=os.cpu_count() or 1) as pool:
        tasks = {(b, stage): pool.submit(compile_stage, b, stage, temporary)
                 for b in backends for stage in BACKENDS[b][2]}
        for backend in backends:
            namespace, _, stages = BACKENDS[backend]
            target = OUTPUT / backend / 'shaders/embedded.h'
            target.parent.mkdir(parents=True, exist_ok=True)
            if backend in OPTIMIZED:
                header, blob = compressed_output(backend, stages, {s: tasks[backend, s].result() for s in stages})
                target.write_text(header)
                target.with_suffix('.bin').write_bytes(blob)
                target.with_name('embedded_data.cpp').write_text(embedded_data('generate.py ' + backend, namespace))
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
            target.write_text(output)


def use_tools(record):
    """The pinned tools toolchain.py recorded, ahead of any installed ones."""
    tools = json.loads(Path(record).read_text())
    os.environ['PATH'] = os.pathsep.join(tools['path'] + [os.environ.get('PATH', '')])
    if 'spirv-cross' in tools:
        os.environ['REMIX_SPIRV_CROSS'] = tools['spirv-cross']
    if 'slangc' in tools:
        os.environ['REMIX_SLANGC'] = tools['slangc']


def write_depfile(path, target):
    def escape(name):
        return name.replace('\\', '/').replace(' ', '\\ ')
    Path(path).write_text(escape(target) + ': ' + ' '.join(escape(d) for d in sorted(DEPENDENCIES)) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    # metal translates the other backends' SPIR-V; 'all' leaves it out (it needs macOS and Xcode).
    parser.add_argument('backend', choices=['postfx', 'upscale', 'raster', 'pathtrace', 'denoise', 'particles', 'nrd', 'fsr31', 'metal', 'sky', 'all'])
    parser.add_argument('--output', type=Path, help='Generated src/native tree (the build\'s); default _work/shaders')
    parser.add_argument('--tools', type=Path, help='Pinned tools recorded by toolchain.py --output')
    parser.add_argument('--depfile', type=Path, help='Make-style dependencies of the generated files')
    parser.add_argument('--depfile-target', default='', help='The output the depfile names')
    mode = parser.add_mutually_exclusive_group()
    # Only NRD's and FSR 3.1's SPIR-V is committed; the other backends' is generated by the build.
    mode.add_argument('--check', action='store_true', help='nrd, fsr31: compare generated output without modifying files')
    mode.add_argument('--validate', action='store_true', help='nrd, fsr31: validate committed SPIR-V using only spirv-val')
    args = parser.parse_args()
    global OUTPUT
    if args.output:
        OUTPUT = args.output.resolve()
    if args.tools:
        use_tools(args.tools)
    selected = [*BACKENDS, 'nrd', 'fsr31'] if args.backend == 'all' else [args.backend]
    glsl = [backend for backend in selected if backend in BACKENDS]
    if (args.check or args.validate) and (glsl or 'metal' in selected or 'sky' in selected):
        parser.error('--check and --validate apply to the committed nrd and fsr31 SPIR-V only')
    if glsl:
        process(glsl)
    if 'metal' in selected:
        import metal
        metal.process()
    if 'sky' in selected:
        import sky
        sky.process()
    if args.depfile:
        write_depfile(args.depfile, args.depfile_target)
    for backend in selected:
        if backend in ('nrd', 'fsr31'):
            (nrd if backend == 'nrd' else fsr31).process(args.check, args.validate)


if __name__ == '__main__':
    # metal.py and sky.py import generate: one module, so it sees --output and records dependencies here.
    sys.modules['generate'] = sys.modules[__name__]
    main()

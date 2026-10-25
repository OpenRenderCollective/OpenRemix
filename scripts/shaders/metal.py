"""Metal libraries of the native Metal backend, translated from the Vulkan backends' SPIR-V.

SPIR-V --pinned SPIRV-Cross fork--> MSL --xcrun metal--> AIR --metallib--> one library per stage,
each an XZ stream in metal/shaders/embedded.bin of the build's generated tree (generate.py --output),
which the build generates before this from the same sources. The input is the SPIR-V the Vulkan
backends embed, so both backends run the same shaders; for the backends embedded after spirv-opt -O
(pathtrace), the same GLSL and defines compiled by glslang without it: Apple's compiler service
fails on the fully inlined lighting kernels and builds the structured ones in seconds. Descriptor sets become argument buffers at
buffer(set); embedded.h maps every (set, binding) of a stage to its argument buffer IDs, so the
host binds resources by their Vulkan set and binding.
"""
import hashlib, json, os, re, struct, subprocess, tempfile, threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import generate

ROOT = generate.ROOT
LOCK = json.loads(Path(__file__).with_name('toolchain.lock.json').read_text())['spirv-cross-msl']
# MSL 3.0 / macOS 13: the authored deployment target of the macOS builds.
MSL_VERSION, METAL_STD, MACOS_MIN = '30000', 'metal3.0', '13.0'
# Metal stage name: (SPIR-V backend, SPIR-V stage).
STAGES = {
    'rasterVert': ('raster', 'vert'), 'rasterFrag': ('raster', 'frag'),
    # Sprite overlays, masked overlays and halo outlines (raster/sprite.cpp, mask.cpp, halo.cpp).
    'spriteVert': ('raster', 'spriteVert'), 'spriteFrag': ('raster', 'spriteFrag'),
    'maskWithin': ('raster', 'maskWithin'), 'maskOverlay': ('raster', 'maskOverlay'),
    'haloMask': ('raster', 'haloMask'), 'haloVert': ('raster', 'haloVert'),
    'haloBlur': ('raster', 'haloBlur'), 'haloComposite': ('raster', 'haloComposite'),
    'compositeVert': ('pathtrace', 'vert'), 'compositeFrag': ('pathtrace', 'frag'),
    'gbufferVert': ('pathtrace', 'gbufferVert'), 'gbufferFrag': ('pathtrace', 'gbufferFrag'),
    'depthFrag': ('pathtrace', 'depthFrag'),
    'trace': ('pathtrace', 'comp'), 'transparency': ('pathtrace', 'transparency'),
    'singleTransparency': ('pathtrace', 'singleTransparency'),
    'postMeter': ('postfx', 'meter'), 'postResolve': ('postfx', 'resolve'),
    'postDown': ('postfx', 'down'), 'postUp': ('postfx', 'up'),
    # Temporal tracing (NRD/TAA inputs at set 1) and the denoiser's passes.
    'temporal': ('pathtrace', 'temporal'), 'temporalTransparency': ('pathtrace', 'temporalTransparency'),
    'singleTemporalTransparency': ('pathtrace', 'singleTemporalTransparency'),
    'frontGuides': ('pathtrace', 'frontGuides'), 'regirPresample': ('pathtrace', 'regirPresample'),
    'psr': ('pathtrace', 'psr'), 'confidence': ('pathtrace', 'confidence'),
    'sigmaTemporal': ('pathtrace', 'sigmaTemporal'), 'localTemporal': ('pathtrace', 'localTemporal'),
    'localSigmaTemporal': ('pathtrace', 'localSigmaTemporal'),
    'denoise': ('denoise', 'comp'), 'denoiseSigma': ('denoise', 'sigma'), 'denoiseLocal': ('denoise', 'local'),
    'denoiseLocalSigma': ('denoise', 'localSigma'),
    # Reconstruction (MetalFX): input preparation and the display composite.
    'reconstructPrepare': ('upscale', 'comp'), 'reconstructVert': ('upscale', 'vert'),
    'reconstructFrag': ('upscale', 'frag'),
    # SHARC. Metal has no 64-bit atomic compare-exchange: the update pass uses
    # SHARC's lock buffer (pathtrace/shaders/sharc.glsl).
    'sharcUpdate': ('pathtrace', 'sharcUpdate', ['-DHASH_GRID_ENABLE_64_BIT_ATOMICS=0']),
    'sharcResolve': ('pathtrace', 'sharcResolve'), 'sharcQuery': ('pathtrace', 'sharcQuery'),
    'sharcTransparency': ('pathtrace', 'sharcTransparency'),
    'sharcSingleTransparency': ('pathtrace', 'sharcSingleTransparency'),
    'sharcTemporal': ('pathtrace', 'sharcTemporal'),
    'sharcTemporalTransparency': ('pathtrace', 'sharcTemporalTransparency'),
    'sharcSingleTemporalTransparency': ('pathtrace', 'sharcSingleTemporalTransparency'),
    'sharcConfidence': ('pathtrace', 'sharcConfidence'), 'sharcSigmaTemporal': ('pathtrace', 'sharcSigmaTemporal'),
    'sharcLocalTemporal': ('pathtrace', 'sharcLocalTemporal'),
    'sharcLocalSigmaTemporal': ('pathtrace', 'sharcLocalSigmaTemporal'),
    # The separate indirect pass and the front sheet's lighting (pathtrace/
    # renderer.cpp createRawPipelines, temporal.cpp beginFrame).
    'indirect': ('pathtrace', 'indirect'), 'sharcIndirect': ('pathtrace', 'sharcIndirect'),
    'indirectTemporal': ('pathtrace', 'indirectTemporal'), 'sharcIndirectTemporal': ('pathtrace', 'sharcIndirectTemporal'),
    'sheetLighting': ('pathtrace', 'sheetLighting'), 'sharcSheetLighting': ('pathtrace', 'sharcSheetLighting'),
    'sheetLightingTemporal': ('pathtrace', 'sheetLightingTemporal'),
    'sharcSheetLightingTemporal': ('pathtrace', 'sharcSheetLightingTemporal'),
    # Volumetrics (volume.cpp) and sparse rendering (sparse.cpp).
    'volumeUpdate': ('pathtrace', 'volumeUpdate'),
    'sparseCompact': ('pathtrace', 'sparseCompact'), 'sparseArgs': ('pathtrace', 'sparseArgs'),
}
# denoise.comp switches on a push constant (pass.mode), so every image some
# mode writes is a read_write texture in its translation, and Apple GPUs read
# read_write textures through a slower, coherent path. Each mode also has a
# variant of its own (pass.mode fixed, dead code removed by spirv-opt), in
# which the images it only reads are plain textures.
DENOISE_MODES = 6
for _name, _stage in (('denoise', 'comp'), ('denoiseSigma', 'sigma'), ('denoiseLocal', 'local'),
                      ('denoiseLocalSigma', 'localSigma')):
    for _mode in range(DENOISE_MODES):
        STAGES[f'{_name}Mode{_mode}'] = ('denoise', _stage, [], _mode)
# NRD's DXC SPIR-V (denoise/shaders/nrd_embedded.h), by identifier.
NRD_IDENTIFIERS = re.findall(r'\{"([^"]+)", shader\d+, sizeof', (ROOT / 'src/native/denoise/shaders/nrd_embedded.h').read_text())
for index, identifier in enumerate(NRD_IDENTIFIERS):
    STAGES[f'nrd{index}'] = ('nrd', str(index))
NONE = 0xffff


# process() translates stages on a thread pool: every worker may ask first.
_cache_lock = threading.Lock()


def spirv(backend, stage, cache={}):
    """The SPIR-V a Vulkan backend embeds for one stage."""
    if backend == 'nrd':
        with _cache_lock:
            if backend not in cache:
                text = (ROOT / 'src/native/denoise/shaders/nrd_embedded.h').read_text()
                arrays = dict(re.findall(r'inline constexpr uint32_t (shader\d+)\[\] = \{(.*?)\};', text, re.S))
                stages = []
                for name in re.findall(r'\{"[^"]+", (shader\d+), sizeof', text):
                    words = [int(word, 16) for word in re.findall(r'0x[0-9a-fA-F]+', arrays[name])]
                    stages.append(struct.pack('<' + 'I' * len(words), *words))
                cache[backend] = stages
        return cache[backend][int(stage)]
    if backend in generate.OPTIMIZED:
        with _cache_lock:
            if backend not in cache:
                cache[backend] = generate.decompressed_stages(backend)
        return cache[backend][stage]
    text = (generate.OUTPUT / backend / 'shaders/embedded.h').read_text()
    match = re.search(r'\b' + stage + r'Shader\[\] = \{(.*?)\};', text, re.S)
    words = [int(word, 16) for word in re.findall(r'0x[0-9a-fA-F]+', match[1])]
    return struct.pack('<' + 'I' * len(words), *words)


def interface(msl, reflection, name):
    """Entry point buffers, argument IDs per (set, binding) and the threadgroup size."""
    entry = re.search(r'^(?:kernel|vertex|fragment) \S+ ' + name + r'\((.*?)\)\n', msl, re.M | re.S)[1]
    sets = {int(s): int(b) for s, b in re.findall(r'spvDescriptorSet(\d+) \[\[buffer\((\d+)\)\]\]', entry)}
    push = re.search(r'constant \w+& \w+ \[\[buffer\((\d+)\)\]\]', entry)
    if any(sets[s] != s for s in sets):
        raise SystemExit(f'{name}: descriptor set buffers are not at buffer(set)')
    resources = []
    for kind in ('ubos', 'ssbos', 'textures', 'images', 'acceleration_structures', 'separate_images',
                 'separate_samplers'):
        for r in reflection.get(kind, []):
            resources.append((kind, r))
    bindings = []
    for set_index in sets:
        body = re.search(r'struct spvDescriptorSetBuffer' + str(set_index) + r'\n\{\n(.*?)\n\};', msl, re.S)[1]
        members = {}
        for line in body.splitlines():
            m = re.match(r'\s*(.*?)(\w+) \[\[id\((\d+)\)\]\];', line)
            members[m[2]] = (m[1], int(m[3]))
        for kind, r in resources:
            if r['set'] != set_index:
                continue
            if kind in ('ubos', 'ssbos'):
                # Buffer blocks match by block type: anonymous instances are named m_<id>.
                found = [v for t, v in members.values() if re.search(r'\b' + r['name'] + r'\*', t)]
                if len(found) != 1:
                    raise SystemExit(f'{name}: buffer block {r["name"]} not found in set {set_index}')
                ident = found[0]
            else:
                ident = members[r['name']][1]
            sampler = members.get(r['name'] + 'Smplr', (None, NONE))[1] if kind == 'textures' else NONE
            count = r.get('array', [1])[0]
            bindings.append((set_index, r['binding'], ident, sampler, count))
    bindings.sort()
    size = reflection['entryPoints'][0].get('workgroup_size', [0, 0, 0])
    blocks = {(r['set'], r['binding']): r['block_size'] for r in reflection.get('ubos', [])}
    return {'sets': len(sets), 'push': int(push[1]) if push else 0xff, 'bindings': bindings, 'blocks': blocks,
            'threads': size}


def structured(backend, stage, path, extra=()):
    """The stage's glslang SPIR-V before spirv-opt (generate.compile_stage's input)."""
    _, environment, stages = generate.BACKENDS[backend]
    defines, source = generate.defines_for(backend, stage, stages)
    defines = defines + list(extra)
    generate.glslang(['--target-env', environment, *defines, '-I' + str(ROOT / 'third_party/nrd/Shaders'),
                      '-I' + str(ROOT / 'third_party/sharc/include'), str(source)], path)


def mode_variant(backend, stage, path, mode):
    """The stage's SPIR-V with its push constant pass.mode fixed to mode, optimized."""
    _, environment, stages = generate.BACKENDS[backend]
    defines, source = generate.defines_for(backend, stage, stages)
    glsl = path.with_suffix('.comp')
    glsl.write_text(source.read_text().replace('pass.mode', f'{mode}u'))
    raw = path.with_suffix('.raw.spv')
    generate.glslang(['--target-env', environment, *defines, '-I' + str(source.parent),
                      '-I' + str(ROOT / 'third_party/nrd/Shaders'), str(glsl)], raw)
    generate.depend(source)
    subprocess.run(['spirv-opt', '-O', str(raw), '-o', str(path)], check=True)


def compile_stage(name, spirv_cross, temporary):
    """Metal library and interface of one stage."""
    backend, stage, *extra = STAGES[name]
    words = spirv(backend, stage)
    directory = Path(temporary)
    source = directory / (name + '.spv')
    if len(extra) > 1:
        mode_variant(backend, stage, source, extra[1])
    elif backend in generate.OPTIMIZED:
        structured(backend, stage, source, *extra[:1])
    else:
        source.write_bytes(words)
    reflection = json.loads(subprocess.run([spirv_cross, str(source), '--reflect'], check=True,
                                           capture_output=True, text=True).stdout)
    model = reflection['entryPoints'][0]['mode']
    msl = directory / (name + '.metal')
    subprocess.run([spirv_cross, str(source), '--msl', '--msl-version', MSL_VERSION,
                    '--msl-argument-buffers', '--msl-argument-buffer-tier', '1',
                    '--msl-force-active-argument-buffer-resources',
                    '--msl-device-argument-buffer', '0', '--msl-device-argument-buffer', '1',
                    # Vulkan clip space is Y-down; Metal's is Y-up.
                    '--flip-vert-y', '--rename-entry-point', 'main', name, model,
                    '--output', str(msl)], check=True)
    air, library = msl.with_suffix('.air'), msl.with_suffix('.metallib')
    subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-std=' + METAL_STD, '-mmacosx-version-min=' + MACOS_MIN,
                    '-O2', '-Wno-unused-variable', '-Wno-unused-const-variable',
                    # out parameters a pass leaves unwritten (pbr_reuse.glsl reuseLocal)
                    '-Wno-uninitialized', '-Wno-uninitialized-const-reference', '-c', str(msl), '-o', str(air)],
                   check=True)
    subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(air), '-o', str(library)], check=True)
    data = library.read_bytes()
    return interface(msl.read_text(), reflection, name), data, generate.compress(data), digest(words)


def digest(words):
    return hashlib.sha256(words).hexdigest()[:16]


def header(results, blob_offsets):
    lines = ['// Generated by scripts/shaders/generate.py metal; do not edit.',
             '// Metal libraries translated from the embedded SPIR-V (scripts/shaders/metal.py),',
             '// each an XZ stream in embedded.bin.',
             '#pragma once', '#include <cstddef>', '#include <cstdint>', 'namespace remix::metal::shaders {',
             'inline constexpr uint16_t NONE = 0xffff;',
             'enum class Stage : uint32_t {']
    lines += [f'  {name},' for name in STAGES] + ['  Count', '};']
    lines += ['// Argument buffer IDs of a Vulkan (set, binding): sampler is the combined sampler\'s,',
              '// 0xffff without one; count is the descriptor count.',
              'struct Binding {', '  uint8_t set;', '  uint16_t binding, id, sampler, count;', '};',
              'struct Library {',
              '  size_t offset, compressedBytes, bytes;',
              '  const char *function;',
              '  uint8_t sets, push; // argument buffers at buffer(0..sets-1); push constants (0xff: none)',
              '  uint16_t threads[3]; // compute threadgroup size',
              '  uint32_t firstBinding, bindingCount;',
              '  const char *spirv; // SHA-256 prefix of the translated SPIR-V',
              '};', 'inline constexpr Binding bindings[] = {']
    first = {}
    for name in STAGES:
        first[name] = sum(len(results[n][0]['bindings']) for n in list(STAGES)[:list(STAGES).index(name)])
        for b in results[name][0]['bindings']:
            lines.append(f'  {{{b[0]}, {b[1]}, {b[2]}, {b[3]}, {b[4]}}}, // {name}')
    lines += ['};', 'inline constexpr Library libraries[] = {']
    for name in STAGES:
        face, data, packed, sha = results[name]
        offset = blob_offsets[name]
        t = face['threads']
        lines.append(f'  {{{offset}u, {len(packed)}u, {len(data)}u, "{name}", {face["sets"]}, {face["push"]}, '
                     f'{{{t[0]}, {t[1]}, {t[2]}}}, {first[name]}u, {len(face["bindings"])}u, "{sha}"}},')
    lines += ['};']
    # The std140 blocks the host writes (metal/frame.h): sizes from reflection,
    # so a field added to the shaders fails the build instead of the GPU.
    trace_frame = results['trace'][0]['blocks'][(0, 3)]
    denoise_frame = results['denoise'][0]['blocks'][(1, 25)]
    lines += ['// std140 sizes of the path tracing Frame (scene.glsl, set 0 binding 3) and the',
              '// temporal constants (inputs.glsl, set 1 binding 25) in the translated shaders.',
              f'inline constexpr size_t traceFrameBytes = {trace_frame};',
              f'inline constexpr size_t denoiseFrameBytes = {denoise_frame};']
    lines += ['// NRD shader identifiers (nrd::PipelineDesc::shaderIdentifier) and their stages.',
              'struct NrdShader {', '  const char *identifier;', '  Stage stage;', '};',
              'inline constexpr NrdShader nrdShaders[] = {']
    lines += [f'  {{"{identifier}", Stage::nrd{index}}},' for index, identifier in enumerate(NRD_IDENTIFIERS)]
    lines += ['};', '// embedded.bin (embedded_data.cpp).', 'extern const unsigned char embeddedData[];',
              'extern const size_t embeddedDataBytes;', '} // namespace remix::metal::shaders', '']
    return '\n'.join(lines)


def process():
    spirv_cross = os.environ.get('REMIX_SPIRV_CROSS', 'spirv-cross')
    run = subprocess.run([spirv_cross, '--version'], capture_output=True, text=True)
    version = run.stdout + run.stderr
    # The commit is reported by git describe: "9f9991c" or "...-g9f9991c5".
    if not re.search(r'(?<![0-9a-f])' + LOCK['revision'][:7], version):
        raise SystemExit(f'Metal shaders require SPIRV-Cross {LOCK["repository"]} at {LOCK["revision"][:8]}; '
                         'build it with scripts/shaders/toolchain.py --metal (the build does) and pass --tools')
    with tempfile.TemporaryDirectory() as temporary, ThreadPoolExecutor(max_workers=os.cpu_count() or 1) as pool:
        tasks = {name: pool.submit(compile_stage, name, spirv_cross, temporary) for name in STAGES}
        results = {name: task.result() for name, task in tasks.items()}
    blob, offsets = bytearray(), {}
    for name in STAGES:
        offsets[name] = len(blob)
        blob += results[name][2]
    target = generate.OUTPUT / 'metal/shaders/embedded.h'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(header(results, offsets))
    target.with_suffix('.bin').write_bytes(bytes(blob))
    target.with_name('embedded_data.cpp').write_text(generate.embedded_data('generate.py metal', 'metal::shaders'))
    # The committed NRD SPIR-V is translated too.
    generate.depend(ROOT / 'src/native/denoise/shaders/nrd_embedded.h')

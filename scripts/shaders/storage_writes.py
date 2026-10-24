"""Widen unknown-format image writes for Vulkan VUID-OpImageWrite-08795.

NRD declares RWTexture2D<float3> SH1 resources backed by RGBA16F images. DXC
emits three-component OpImageWrite operands for its unknown-format annotations.
Vulkan requires at least the view's component count. Pad unused components with
zero; the three SH coefficients and every upstream HLSL source remain unchanged.
"""
import re
import subprocess


def widen(binary):
    assembly = subprocess.check_output(['spirv-dis', str(binary)], text=True)
    definitions = {}
    types = {}
    for line in assembly.splitlines():
        m = re.match(r'\s*(%\S+) = (\w+)(.*)', line)
        if not m: continue
        result, op, rest = m.groups(); operands = rest.split()
        if op in ('OpTypeFloat', 'OpTypeInt'): types[result] = (result, 1)
        elif op == 'OpTypeVector': types[result] = (operands[0], int(operands[1]))
        elif operands: definitions[result] = operands[0]
    declarations, replacements, padded_types = [], [], {}
    for line in assembly.splitlines():
        m = re.match(r'\s*OpImageWrite (%\S+) (%\S+) (%\S+)(.*)', line)
        if not m:
            replacements.append(line); continue
        image, coord, value, rest = m.groups()
        scalar, width = types[definitions[value]]
        if width == 4:
            replacements.append(line); continue
        if scalar not in padded_types:
            index = len(padded_types)
            vector, zero = f'%remix_vec4_{index}', f'%remix_zero_{index}'
            # Reuse an existing vec4 to avoid duplicate non-aggregate types.
            vector = next((t for t, shape in types.items() if shape == (scalar, 4)), vector)
            if vector not in types: declarations.append(f'{vector} = OpTypeVector {scalar} 4')
            declarations.append(f'{zero} = OpConstantNull {scalar}')
            padded_types[scalar] = vector, zero
        vector, zero = padded_types[scalar]
        padded = f'%remix_padded_{len(replacements)}'
        replacements += [f'{padded} = OpCompositeConstruct {vector} {value}' + (' ' + zero) * (4-width),
                         f'OpImageWrite {image} {coord} {padded}{rest}']
    if not declarations: return
    assembly = '\n'.join(replacements) + '\n'
    position = re.search(r'^\s*%\S+ = OpFunction\b', assembly, re.M).start()
    assembly = assembly[:position] + '\n'.join(declarations) + '\n' + assembly[position:]
    subprocess.run(['spirv-as', '--target-env', 'vulkan1.2', '-o', str(binary), '-'],
                   input=assembly, text=True, check=True)

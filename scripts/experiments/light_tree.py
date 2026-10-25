#!/usr/bin/env python3
"""Selection probabilities of a saved light tree at one receiver.

Touch lighttree.request in the profile directory (api/lifecycle.cpp); the
renderer writes lighttree.bin (LightBvh::save). This walks the tree with
light_bvh.glsl's lightWeight (without the local light range falloff) and
lists each emitter's probability next to its unshadowed contribution. A
light with a large contribution share and a tiny probability is what
renders black with rare bright pixels (only Ray Reconstruction averages
it). --target prints the weights along the path to the emitter nearest a
position.

Requires numpy. Positions are in game units:
  light_tree.py lighttree.bin -2812,1878,-58 0,0.40,0.91 --target -2850,2160,0
"""
import argparse
import math
import numpy as np

NODE = np.dtype([('minimum', '<f4', 4), ('maximum', '<f4', 4), ('links', '<u4', 4)])
EMITTER = np.dtype([('positionRadius', '<f4', 4), ('radiance', '<f4', 4), ('corners', '<f4', (3, 4)),
                    ('directionCone', '<f4', 4), ('shaping', '<f4', 4)])

def vector(text):
    return np.array([float(x) for x in text.split(',')])

def weight(node, p, n):
    lo, hi = node['minimum'][:3].astype(float), node['maximum'][:3].astype(float)
    half = .5 * (hi - lo)
    delta = .5 * (hi + lo) - p
    radius2 = half @ half
    if delta @ n + half @ np.abs(n) <= 0:
        return 0.
    nearest = np.maximum(np.abs(delta) - half, 0)
    cosine = min(1., (delta @ n + math.sqrt(radius2)) / math.sqrt(max(delta @ delta, radius2, 1e-12)))
    distance2 = max(math.sqrt((nearest @ nearest) * (delta @ delta)), .01 * radius2)
    return float(node['minimum'][3]) * cosine / max(distance2, float(node['maximum'][3]), 1e-12)

def contribution(e, p, n):
    position = e['positionRadius'][:3].astype(float)
    v = position - p
    d2 = max(v @ v, 1.)
    w = v / math.sqrt(d2)
    if e['radiance'][3]:
        c = e['corners'][:, :3].astype(float)
        normal = np.cross(c[1] - c[0], c[2] - c[0])
        area = .5 * np.linalg.norm(normal)
    else:
        area = math.pi * float(e['positionRadius'][3]) ** 2
    return max(0., w @ n) * area * float(e['radiance'][:3].mean()) / d2

def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('tree')
    parser.add_argument('position', type=vector)
    parser.add_argument('normal', type=vector)
    parser.add_argument('--target', type=vector, help='print the path to the emitter nearest this position')
    parser.add_argument('--rows', type=int, default=20)
    args = parser.parse_args()
    data = open(args.tree, 'rb').read()
    node_count, emitter_count = np.frombuffer(data, '<u4', 2)
    nodes = np.frombuffer(data, NODE, node_count, 8)
    emitters = np.frombuffer(data, EMITTER, emitter_count, 8 + NODE.itemsize * int(node_count))
    p, n = args.position, args.normal / np.linalg.norm(args.normal)

    # Probability of reaching each leaf; parents for --target.
    leaves, parent, lost = {}, {}, 0.
    stack = [(0, 1.)]
    while stack:
        index, probability = stack.pop()
        links = nodes[index]['links']
        if links[3] <= 1:
            leaves[index] = probability
            continue
        a, b = weight(nodes[links[0]], p, n), weight(nodes[links[1]], p, n)
        parent[int(links[0])], parent[int(links[1])] = index, index
        if a == 0 and b == 0:
            lost += probability
            probability = 0.
        split = 0 if a == 0 else 1 if b == 0 else min(max(a / (a + b), 1e-6), 1 - 1e-6)
        stack += [(int(links[0]), probability * split), (int(links[1]), probability * (1 - split))]

    rows, leaf_of = [], {}
    for leaf, probability in leaves.items():
        links = nodes[leaf]['links']
        for e in range(links[2], links[2] + max(int(links[1]), 1)):
            leaf_of[e] = leaf
            rows.append((probability * float(emitters[e]['shaping'][3]), contribution(emitters[e], p, n), e))
    total = sum(r[1] for r in rows) or 1.
    variance = sum(c * c / q for q, c, _ in rows if c > 0 and q > 0) / total ** 2
    print(f'nodes {node_count} emitters {emitter_count}; probability at dead ends {lost:.4f}')
    print(f'relative variance of the unshadowed estimate {variance:.2f} (1 is ideal)')
    print('by contribution: probability, contribution share, kind, position')
    for q, c, e in sorted(rows, key=lambda r: -r[1])[:args.rows]:
        kind = 'tri' if emitters[e]['radiance'][3] else 'sphere'
        print(f'  {q:.6f} {c / total:.4f} {kind:6} {np.round(emitters[e]["positionRadius"][:3], 1)}')

    if args.target is not None:
        e = min(leaf_of, key=lambda e: np.linalg.norm(emitters[e]['positionRadius'][:3] - args.target))
        path, index = [], leaf_of[e]
        while index in parent:
            path.append((parent[index], index))
            index = parent[index]
        print(f'path to emitter {e} at {np.round(emitters[e]["positionRadius"][:3], 1)}:')
        for node, child in reversed(path):
            links = nodes[node]['links']
            other = int(links[1]) if child == links[0] else int(links[0])
            ours, theirs = weight(nodes[child], p, n), weight(nodes[other], p, n)
            print(f'  node {node}: take {ours / (ours + theirs) if ours + theirs else 0:.4f}')
            for name, k in (('ours', child), ('other', other)):
                m = nodes[k]
                print(f'    {name:5} box {np.round(m["minimum"][:3])}..{np.round(m["maximum"][:3])}'
                      f' power {m["minimum"][3]:.4g} r2max {m["maximum"][3]:.4g}')

if __name__ == '__main__':
    main()

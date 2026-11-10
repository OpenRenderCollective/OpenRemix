#!/usr/bin/env python3
"""Read-only comparison of whole-volume PVS predictions and traced bake visibility.

Capture with REMIX_DEBUG_BAKE_VISIBILITY_REPORT=/absolute/path/report.json.
This tool never changes a bake. A missing, solid, invalid or over-budget region
is unknown. The biased-point alternative covers the sampled visibility origins,
not every later receiver-area antialiasing site or replacement geometry edit.
"""
import argparse
from collections import Counter
import json
import math
from pathlib import Path


class Partition:
    def __init__(self, data):
        self.nodes = data['nodes']
        self.leaves = data['leaves']
        self.count = data['clusters']
        self.stride = data['rowBytes']
        self.visibility = bytes.fromhex(data['visibility'])
        self.usable = (data['closed'] and self.count > 0 and self.nodes and
                       self.stride >= (self.count+7)//8 and
                       len(self.visibility) == self.count*self.stride)

    def clusters(self, bounds, capacity=64, maximum_steps=8192):
        if not self.usable:
            return None, 'partition'
        if bounds is None or len(bounds) != 2 or any(len(v) != 3 for v in bounds):
            return None, 'bounds'
        low, high = bounds
        if any(not math.isfinite(x) for v in bounds for x in v) or any(a > b for a,b in zip(low,high)):
            return None, 'bounds'
        stack, seen, result = [0], set(), set()
        steps = 0
        while stack:
            steps += 1
            if steps > maximum_steps:
                return None, 'work limit'
            index = stack.pop()
            if index < 0:
                leaf = -1-index
                if leaf >= len(self.leaves) or not 0 <= self.leaves[leaf] < self.count:
                    return None, 'unclustered leaf'
                result.add(self.leaves[leaf])
                if len(result) > capacity:
                    return None, 'cluster limit'
                continue
            # The host's closed flag checks child ranges, not acyclicity.
            # Refuse repeated nodes rather than trusting a malformed graph.
            if index >= len(self.nodes) or index in seen:
                return None, 'node graph'
            seen.add(index)
            node = self.nodes[index]
            if len(node) != 6 or not all(math.isfinite(x) for x in node[:4]):
                return None, 'plane'
            normal, distance = node[:3], node[3]
            if sum(x*x for x in normal) <= 0:
                return None, 'plane'
            minimum = sum(n*(a if n >= 0 else b) for n,a,b in zip(normal,low,high))-distance
            maximum = sum(n*(b if n >= 0 else a) for n,a,b in zip(normal,low,high))-distance
            guard = 64*(2**-23)*(1+abs(distance)+sum(abs(n)*(abs(a)+abs(b))
                                                      for n,a,b in zip(normal,low,high)))
            if maximum >= -guard:
                stack.append(node[4])
            if minimum <= guard:
                stack.append(node[5])
        return tuple(sorted(result)), None

    def disconnected(self, first, second):
        # Same-cluster receivers stay connected even with a malformed self bit.
        # Keep either direction of an asymmetric PVS relation conservatively.
        for a in first:
            for b in second:
                if a == b or ((self.visibility[a*self.stride+b//8] >> (b%8)) & 1) or (
                        (self.visibility[b*self.stride+a//8] >> (a%8)) & 1):
                    return False
        return bool(first and second)


def analyse(data):
    if data['version'] not in (1,2):
        raise ValueError('Unsupported bake visibility report')
    partition = Partition(data['partition'])
    emitters = [partition.clusters(light['bounds']) for light in data['lights']]
    output = dict(version=2, source=data['source'], lights=len(emitters), charts=len(data['charts']),
                  captureVersion=data['version'],
                  lightsWithShapeMetadata=sum(all(k in light for k in ('triangle','coneCos','focus'))
                                              for light in data['lights']),
                  knownEmitterBounds=sum(c is not None for c,reason in emitters), alternatives={})
    for field in ['bounds','biasedPoints']:
        counters = Counter({key:0 for key in ('candidatePairs','candidatePointPairs','unknownPairs',
                            'predictedDisconnectedPairs','predictedDisconnectedPointPairs',
                            'verifiedBlockedPairs','clearClassificationPairs','isotropicClearConflicts',
                            'unresolvedPairs')})
        unknown = Counter()
        contradictions = []
        known_charts = 0
        connectivity = {}
        for chart, value in enumerate(data['charts']):
            clusters, reason = partition.clusters(value[field])
            if clusters is not None:
                known_charts += 1
            else:
                unknown[reason] += 1
            for light, state in value['candidates']:
                if not 0 <= light < len(emitters) or state not in (0,1,2):
                    raise ValueError('Invalid captured light identity/classification')
                counters['candidatePairs'] += 1
                counters['candidatePointPairs'] += value['points']
                emitter, emitter_reason = emitters[light]
                if clusters is None or emitter is None:
                    counters['unknownPairs'] += 1
                    continue
                key = clusters, emitter
                if key not in connectivity:
                    connectivity[key] = partition.disconnected(*key)
                if not connectivity[key]:
                    continue
                counters['predictedDisconnectedPairs'] += 1
                counters['predictedDisconnectedPointPairs'] += value['points']
                if state == 1:
                    counters['verifiedBlockedPairs'] += 1
                elif state == 2:
                    counters['clearClassificationPairs'] += 1
                    captured = data['lights'][light]
                    # Shaped/one-sided emitters can return the reserved clear
                    # value when all sampled weights vanish. Isotropic spheres
                    # in disjoint volumes cannot use that zero-weight sentinel.
                    isotropic = (captured.get('triangle') is False and
                                 captured.get('coneCos',0) <= -1 and captured.get('focus',1) <= 0)
                    if isotropic:
                        counters['isotropicClearConflicts'] += 1
                    if len(contradictions) < 100:
                        contradictions.append(dict(chart=chart,light=light,style=captured['style'],
                                                   isotropic=isotropic))
                else:
                    # Partial visibility and unclassified budget fallback both
                    # remain unresolved. Neither can validate a black bake.
                    counters['unresolvedPairs'] += 1
        output['alternatives'][field] = dict(counters=counters,knownChartBounds=known_charts,
                                             unknownChartReasons=unknown,clearClassifications=contradictions)
    output['scope'] = ('Offline candidate evidence only. No rejection is applied. Point-pair costs are '
                       'an upper-bound estimate, not measured rays or validated all-map savings.')
    return output


def self_test():
    data = dict(closed=True,clusters=2,rowBytes=1,nodes=[[1,0,0,0,-1,-2]],leaves=[0,1],visibility='0102')
    p = Partition(data)
    assert p.clusters([[.1,0,0],[1,1,1]])[0] == (0,)
    assert p.clusters([[-1,0,0],[-.1,1,1]])[0] == (1,)
    assert p.clusters([[-1,0,0],[1,1,1]])[0] == (0,1)
    assert p.clusters([[0,0,0],[0,1,1]])[0] == (0,1)
    assert p.disconnected((0,),(1,)) and not p.disconnected((0,1),(1,))
    asymmetric = Partition(dict(data,visibility='0302'))
    assert not asymmetric.disconnected((1,),(0,))
    assert not Partition(dict(data,visibility='0000')).disconnected((0,),(0,))
    assert Partition(dict(data,leaves=[0,-1])).clusters([[-1,0,0],[-.1,1,1]])[0] is None
    assert Partition(dict(data,nodes=[[1,1,0,0,-1,-2]])).clusters([[-1,.2,0],[-.1,1,1]])[0] == (0,1)
    assert Partition(dict(data,nodes=[[1,0,0,0,0,-2]])).clusters([[.1,0,0],[1,1,1]])[0] is None
    assert p.clusters([[-1,0,0],[1,1,1]],capacity=1)[0] is None
    assert p.clusters([[float('nan'),0,0],[1,1,1]])[0] is None
    assert Partition(dict(data,visibility='')).clusters([[1,0,0],[2,1,1]])[0] is None
    capture = dict(version=1,source='fixture',partition=data,
                   lights=[dict(style=1,bounds=[[-1,0,0],[-.1,1,1]])],
                   charts=[dict(points=8,bounds=[[.1,0,0],[1,1,1]],
                                biasedPoints=[[.1,0,0],[1,1,1]],candidates=[[0,2]])])
    counters = analyse(capture)['alternatives']['bounds']['counters']
    assert counters['clearClassificationPairs'] == 1 and counters['predictedDisconnectedPointPairs'] == 8
    assert not counters['isotropicClearConflicts']
    capture['lights'][0].update(triangle=False,coneCos=-1,focus=0)
    assert analyse(capture)['alternatives']['bounds']['counters']['isotropicClearConflicts'] == 1
    capture['lights'][0]['coneCos'] = -4.37113883e-8
    assert not analyse(capture)['alternatives']['bounds']['counters']['isotropicClearConflicts']
    capture['charts'][0]['candidates'] = [[0,0]]
    counters = analyse(capture)['alternatives']['bounds']['counters']
    assert counters['unresolvedPairs'] == 1 and not counters['verifiedBlockedPairs']
    print('Whole-volume partition boundaries, unknown regions and asymmetric visibility checks pass')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture',nargs='?',type=Path)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--self-test',action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
    if args.capture:
        result = json.dumps(analyse(json.loads(args.capture.read_text())),indent=2)+'\n'
        if args.output:
            args.output.write_text(result)
        else:
            print(result,end='')
    elif not args.self_test:
        parser.error('Supply a capture or --self-test')


if __name__ == '__main__':
    main()

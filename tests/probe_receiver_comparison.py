"""Receiver comparison gates reject mismatched references and missing coverage."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

COMMON = Path(__file__).resolve().parents[1] / 'scripts' / 'common'
sys.path.insert(0, str(COMMON))
from probe_receivers import compare_receivers


def report(values):
    return dict(sceneHash='1234', scope='fixed indirect test', sampled=len(values), receivers=[
        dict(draw=0, chart=i, cluster=0, position=[i, 0, 0], normal=[0, 0, 1],
             surface=[1, 1, 1], probe=[value]*3, covered=True) for i, value in enumerate(values)
    ])


class ReceiverComparisonTests(unittest.TestCase):
    def test_errors_and_bounded_stable_order(self):
        result = compare_receivers(report([1, 1, 1]), report([2, 3, 3]), 2)
        self.assertEqual(result['regressed'], 3)
        self.assertEqual(result['coverageLost'], 0)
        self.assertAlmostEqual(result['after']['meanAbsoluteError'], 5/3)
        self.assertAlmostEqual(result['after']['rootMeanSquaredError'], 3**.5)
        self.assertEqual([row['index'] for row in result['largestRegressions']], [1, 2])
        identical = compare_receivers(report([1]), report([1]))
        self.assertEqual(identical['changed'], 0)
        self.assertEqual(identical['before']['maximumAbsoluteError'], 0)

    def test_coverage_does_not_become_black_light(self):
        before, after = report([2, 99, 1]), report([0, 1, 1])
        before['receivers'][1]['covered'] = False
        after['receivers'][0]['covered'] = False
        result = compare_receivers(before, after)
        self.assertEqual((result['coverageLost'], result['coverageGained']), (1, 1))
        self.assertEqual(result['before']['count'], 1)
        self.assertEqual(result['before']['meanAbsoluteError'], 0)
        self.assertEqual(result['regressed'], 0)
        before['receivers'][2]['covered'] = False
        after['receivers'][2]['covered'] = False
        result = compare_receivers(before, after)
        self.assertIsNone(result['before'])
        self.assertIsNone(result['after'])

    def test_changed_measurements_reject(self):
        for field, value in [('sceneHash', 'other'), ('scope', 'different units'), ('sampled', 2)]:
            after = report([1])
            after[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                compare_receivers(report([1]), after)
        for field, value in [('position', [1, 0, 0]), ('normal', [1, 0, 0]),
                             ('surface', [2, 2, 2]), ('chart', 3), ('draw', 2), ('cluster', 7)]:
            after = report([1])
            after['receivers'][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                compare_receivers(report([1]), after)

    def test_malformed_samples_reject(self):
        for field, value in [('probe', [float('nan')]*3), ('probe', [float('inf')]*3),
                             ('probe', [True]*3), ('normal', [0, 1]), ('covered', 1), ('draw', True)]:
            after = report([1])
            after['receivers'][0][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                compare_receivers(report([1]), after)
        for limit in (0, 129, True):
            with self.assertRaises(ValueError):
                compare_receivers(report([1]), report([1]), limit)

    def test_cli_protects_existing_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            before, after, output = (root/name for name in ('before.json', 'after.json', 'result.json'))
            before.write_text(json.dumps(report([1])))
            after.write_text(json.dumps(report([2])))
            command = [sys.executable, str(COMMON/'compare-probe-receivers.py'), str(before), str(after), '--output', str(output)]
            subprocess.run(command, check=True, capture_output=True)
            original = output.read_bytes()
            self.assertEqual(json.loads(original)['regressed'], 1)
            retry = subprocess.run(command, capture_output=True)
            self.assertNotEqual(retry.returncode, 0)
            self.assertEqual(output.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()

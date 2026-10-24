"""Analytical checks for offline sampled influence proposals."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts" / "common"))
from reflection_regions import inside, propose_influence, propose_seeded_influence, support


class InfluenceTests(unittest.TestCase):
    def setUp(self):
        self.origin = [0, 0, 0]
        self.minimum, self.maximum = [-5, -5, -5], [5, 5, 5]
        self.samples = [([-.5, 0, 0], "good"), ([.5, 0, 0], "good"),
                        ([-2, 0, 0], "bad"), ([2, 0, 0], "bad"),
                        ([.75, 0, 0], "unknown")]

    def fit(self, samples=None, **kwargs):
        return propose_influence(self.origin, self.minimum, self.maximum,
                                 self.samples if samples is None else samples, .1, .01, **kwargs)

    def test_inward_cuts_preserve_support_and_origin(self):
        result = self.fit()
        self.assertIsNotNone(result)
        self.assertEqual(result["cuts"], 2)
        self.assertEqual(result["trainingSupport"], dict(good=2, bad=0, unknown=1))
        self.assertTrue(inside(self.origin, result["minimum"], result["maximum"]))
        self.assertGreater(result["minimum"][0], -2)
        self.assertLess(result["maximum"][0], 2)
        self.assertTrue(all(result["minimum"][c] >= self.minimum[c] and
                            result["maximum"][c] <= self.maximum[c] for c in range(3)))
        self.assertEqual(result, self.fit(list(reversed(self.samples))))

    def test_held_out_obstruction_is_not_certified(self):
        result = self.fit()
        checks = support([([.75, 0, 0], "bad")], result["minimum"], result["maximum"])
        self.assertEqual(checks["bad"], 1)

    def test_origin_conflict_and_unknowns_refuse_support(self):
        self.assertIsNone(self.fit(self.samples + [(self.origin, "bad")]))
        self.assertIsNone(self.fit([([0, 0, 0], "unknown")]))
        self.assertIsNone(self.fit([([1, 0, 0], "good"), ([1, 0, 0], "bad")]))

    def test_cut_budget_is_bounded(self):
        self.assertIsNone(self.fit(maximum_cuts=1))
        with self.assertRaises(ValueError):
            self.fit(maximum_cuts=25)
        with self.assertRaises(ValueError):
            self.fit([([0, 0, 0], "invalid")])
        with self.assertRaises(ValueError):
            self.fit(maximum_cuts=1.5)

    def test_seed_growth_recovers_from_greedy_dead_end(self):
        samples = [(p + [0], label) for points, label in [
            ([[-3, -4], [1, -4], [1, -4], [2, -1]], "good"),
            ([[2, -3], [-1, 4], [-2, 0], [0, -2], [-1, -2], [-1, 2]], "bad")]
                   for p in points]
        self.assertIsNone(self.fit(samples))
        result = propose_seeded_influence(self.origin, self.minimum, self.maximum, samples, .1, .01)
        self.assertEqual(result["strategy"], "seedGrowth")
        self.assertEqual(result["trainingSupport"], dict(good=1, bad=0, unknown=0))
        self.assertTrue(inside(self.origin, result["minimum"], result["maximum"]))
        self.assertEqual(result, propose_seeded_influence(
            self.origin, self.minimum, self.maximum, list(reversed(samples)), .1, .01))
        # Independent evidence can reject a training-supported proposal.
        self.assertEqual(support([([1, 0, 0], "bad")], result["minimum"], result["maximum"])["bad"], 1)

    def test_seed_growth_keeps_origin_conflicts_rejected(self):
        self.assertIsNone(propose_seeded_influence(self.origin, self.minimum, self.maximum,
                                                 self.samples + [(self.origin, "bad")], .1, .01))


if __name__ == "__main__":
    unittest.main()

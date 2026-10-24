"""Pinned compiler preflight accepts native version formats without relaxing pins."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts' / 'shaders'))
from nrd import matches_dxc_version


class DxcVersionTests(unittest.TestCase):
    def test_linux_product_version(self):
        self.assertTrue(matches_dxc_version('libdxcompiler.so: 1.9.0.5191'))
        self.assertFalse(matches_dxc_version('libdxcompiler.so: 1.9.0.51910'))
        self.assertFalse(matches_dxc_version('libdxcompiler.so: 11.9.0.5191'))

    def test_windows_product_and_release_must_both_match(self):
        actual = 'dxcompiler.dll: 1.9(5191-d355aa83)(1.9.2602.24) - 1.9.2602.24 (d355aa836)'
        self.assertTrue(matches_dxc_version(actual))
        self.assertFalse(matches_dxc_version(actual.replace('5191-', '5190-')))
        self.assertFalse(matches_dxc_version(actual.replace('(1.9.2602.24)', '(1.9.2602.23)')))
        self.assertFalse(matches_dxc_version(actual.replace('dxcompiler.dll:', 'other.dll:')))


if __name__ == '__main__':
    unittest.main()

"""Pinned compiler formats and shader dependency/output path contracts."""
import sys
import unittest
from unittest.mock import patch
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts' / 'shaders'))
from nrd import matches_dxc_version
import generate
from generate import depfile_inputs


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


class ShaderDependencyTests(unittest.TestCase):
    def test_windows_drives_and_spaces(self):
        self.assertEqual(depfile_inputs(r'D\:/build/out.spv: D\:/Source\ Folder/main.comp D\:/Source/include.glsl'),
                         ['D:/Source Folder/main.comp', 'D:/Source/include.glsl'])

    def test_posix_continuation(self):
        self.assertEqual(depfile_inputs('out.spv: /source/main.comp ' + chr(92) + '\n /source/include.glsl'),
                         ['/source/main.comp', '/source/include.glsl'])


class ShaderOutputTests(unittest.TestCase):
    def test_default_outputs_stay_under_work(self):
        for backend in generate.BACKENDS:
            self.assertTrue(generate.generated_directory(backend).is_relative_to(generate.ROOT / '_work'))

    def test_explicit_build_tree_preserves_fixture_paths(self):
        build = generate.ROOT / '_work' / 'path-fixture'
        with patch.object(generate, 'OUTPUT', build / 'src/native'):
            self.assertEqual(generate.generated_directory('bake'), build / 'src/native/forward/bake/shaders')
            self.assertEqual(generate.generated_directory('forward_test'), build / 'tests/forward/shaders')


if __name__ == '__main__':
    unittest.main()

"""Owned scratch cleanup and publication without compiling external tools."""
import importlib.util
from pathlib import Path
import stat
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('shader_toolchain', ROOT / 'scripts/shaders/toolchain.py')
tools = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tools)


class ToolchainCleanup(unittest.TestCase):
    def test_readonly_and_boundaries(self):
        with tempfile.TemporaryDirectory(prefix='remix-shader-tools-') as temporary:
            root = Path(temporary)
            cache = root / 'cache'
            work = cache / 'scratch' / 'work'
            work.mkdir(parents=True)
            packed = work / 'readonly.idx'
            packed.write_bytes(b'owned scratch fixture')
            packed.chmod(stat.S_IREAD)
            tools.remove_scratch(work, cache)
            self.assertFalse(work.exists())
            for path in [cache, root / 'outside']:
                with self.assertRaises(ValueError):
                    tools.remove_scratch(path, cache, ignore_errors=True)
            tools.remove_scratch(cache / 'missing', cache, ignore_errors=True)
            with self.assertRaises(FileNotFoundError):
                tools.remove_scratch(cache / 'missing', cache)

    def test_complete_tools_are_published_once(self):
        with tempfile.TemporaryDirectory(prefix='remix-shader-tools-') as temporary:
            cache = Path(temporary) / 'cache'
            calls = []

            def make(work, binaries):
                calls.append(work)
                work.mkdir()
                packed = work / 'readonly.idx'
                packed.write_bytes(b'git pack fixture')
                packed.chmod(stat.S_IREAD)
                (binaries / 'compiler').write_bytes(b'compiled fixture')

            binaries = tools.toolset(cache, 'fixture', [], make)
            self.assertEqual((binaries / 'compiler').read_bytes(), b'compiled fixture')
            self.assertTrue((binaries.parent / 'complete').is_file())
            self.assertFalse((binaries.parent / 'work').exists())
            self.assertEqual(tools.toolset(cache, 'fixture', [], make), binaries)
            self.assertEqual(len(calls), 1)
            self.assertEqual(list(cache.iterdir()), [binaries.parent])


if __name__ == '__main__':
    unittest.main()

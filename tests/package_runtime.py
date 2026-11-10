"""Windows bundles keep separately pinned Transformer and CNN RR payloads."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

script = Path(__file__).resolve().parents[1] / 'scripts/windows/package-runtime.py'
spec = importlib.util.spec_from_file_location('package_runtime', script)
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)


class DlssBundleTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.build = self.root / 'build'
        self.vendor = self.root / 'third_party/dlss'
        self.vendor.mkdir(parents=True)
        self.manifest = {'version': '310.9.1', 'cnnVersion': '310.2.1'}
        self.output = self.root / 'bundle'
        self.bundle = runtime.Bundle(self.output)
        hashes = []
        for name in ['nvngx_dlss.dll', 'nvngx_dlssd.dll', 'nvngx_dlssg.dll']:
            self.payload('src/native/upscale/' + name, name.encode())
            hashes.append(hashlib.sha256(name.encode()).hexdigest() + '  lib/Windows_x86_64/rel/' + name)
        self.payload('src/native/upscale/cnn/nvngx_dlssd.dll', b'cnn')
        hashes.append(hashlib.sha256(b'cnn').hexdigest() +
                      '  lib/Windows_x86_64/rel-310.2.1/nvngx_dlssd.dll')
        for name in ['LICENSE.txt', 'README.remix.md', 'upstream.json', 'LICENSE-310.2.1.txt']:
            (self.vendor / name).write_text(name)
        (self.vendor / 'sources.sha256').write_text('\n'.join(hashes) + '\n')

    def payload(self, relative, value):
        path = self.build / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(value)

    def test_both_models_have_their_own_hash_payload_and_license(self):
        runtime.bundle_dlss(self.bundle, self.root, self.build, self.manifest)
        self.bundle.write()
        self.assertEqual((self.output / 'dlss/310.9.1/nvngx_dlssd.dll').read_bytes(), b'nvngx_dlssd.dll')
        self.assertEqual((self.output / 'dlss/310.2.1/nvngx_dlssd.dll').read_bytes(), b'cnn')
        self.assertEqual((self.output / 'dlss/310.2.1/LICENSE.txt').read_text(), 'LICENSE-310.2.1.txt')

    def test_cnn_cannot_substitute_for_the_transformer_with_the_same_basename(self):
        self.payload('src/native/upscale/nvngx_dlssd.dll', b'cnn')
        with self.assertRaisesRegex(ValueError, 'rel/nvngx_dlssd.dll'):
            runtime.bundle_dlss(self.bundle, self.root, self.build, self.manifest)
        self.assertFalse(self.output.exists())

    def test_corrupt_cnn_rejects_before_writing_a_bundle(self):
        self.payload('src/native/upscale/cnn/nvngx_dlssd.dll', b'corrupt')
        with self.assertRaisesRegex(ValueError, 'rel-310.2.1/nvngx_dlssd.dll'):
            runtime.bundle_dlss(self.bundle, self.root, self.build, self.manifest)
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()

"""Corresponding source must not bundle private session records or credentials."""
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path


class SourceArchiveTests(unittest.TestCase):
    def test_private_files_excluded_and_source_retained(self):
        script = Path(__file__).resolve().parents[1] / 'tools/installer-source.py'
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name in ('tools/installer-source.py', 'airdrop/service.py', 'desktop/Session.cs',
                         'docs/build.md', 'docs/signpath-readiness.md', 'PRIVACY.md', 'CODE-SIGNING.md', 'assets/brand/PulseDrop.ico', 'assets/readme/hero.svg', 'LICENSE', '.gitattributes', '.gitignore', 'HANDOVER.md', 'docs/device-test.md',
                         'dist/identity.json', 'airdrop/private.key', 'desktop/obj/cache.json'):
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(script.read_bytes() if name == 'tools/installer-source.py' else b'fixture')
            stage = root / 'build/stage'
            (stage / 'source').mkdir(parents=True)
            subprocess.run([sys.executable, str(root / 'tools/installer-source.py'),
                            '--stage', str(stage), '--deps', str(root / 'deps')], check=True)
            with zipfile.ZipFile(stage / 'source/PulseDrop.zip') as archive:
                self.assertEqual(set(archive.namelist()), {
                    'tools/installer-source.py', 'airdrop/service.py', 'desktop/Session.cs',
                    'docs/build.md', 'docs/signpath-readiness.md', 'PRIVACY.md', 'CODE-SIGNING.md', 'assets/brand/PulseDrop.ico', 'assets/readme/hero.svg', 'LICENSE', '.gitattributes', '.gitignore'})


if __name__ == '__main__':
    unittest.main()

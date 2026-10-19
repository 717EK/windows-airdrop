"""A corrupt cached compiler must fail before extracting or executing build tools."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


@unittest.skipUnless(os.name == 'nt', 'Windows PowerShell bootstrap')
class CiBootstrapTests(unittest.TestCase):
    def test_rejects_corrupt_compiler_before_any_execution(self):
        script = Path(__file__).resolve().parents[1] / 'tools/bootstrap-ci.ps1'
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'setup-downloads').mkdir()
            (root / 'setup-downloads/mingw.7z').write_bytes(b'not an approved compiler')
            env = os.environ.copy()
            # Avoid PowerShell 7 modules inherited through a Windows Store Python launcher.
            env['PSMODULEPATH'] = str(Path(env['SYSTEMROOT']) / 'System32/WindowsPowerShell/v1.0/Modules')
            result = subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                                     '-File', str(script), '-Deps', str(root)],
                                    capture_output=True, text=True, timeout=20, env=env)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('CI dependency hash mismatch: mingw.7z', result.stdout + result.stderr)
            self.assertFalse((root / 'mingw64').exists())
            self.assertFalse((root / 'airdrop-venv').exists())

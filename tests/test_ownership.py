"""Ownership survives elapsed time and is released by actual process death."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'airdrop'))
from ownership import receiver_lease


class OwnershipTests(unittest.TestCase):
    def test_second_process_is_refused_then_crash_releases_lock(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            script = "import os,sys; sys.path.insert(0,sys.argv[2]); from ownership import receiver_lease; from pathlib import Path; " \
                "lease=receiver_lease(Path(sys.argv[1])); lease.__enter__(); print('ready',flush=True); sys.stdin.readline(); os._exit(9)"
            child = subprocess.Popen([sys.executable, '-E', '-s', '-u', '-c', script, str(root), str(Path(__file__).resolve().parents[1] / 'airdrop')],
                cwd=Path(__file__).resolve().parents[1] / 'airdrop', stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
            try:
                self.assertEqual(child.stdout.readline().strip(), 'ready')
                with self.assertRaisesRegex(RuntimeError, 'Another receiver'):
                    with receiver_lease(root): self.fail('must not acquire a live lock')
                # Exit the lock owner, not a Windows Store Python launcher process.
                child.stdin.write('crash\n'); child.stdin.flush(); child.wait(timeout=5)
                self.assertEqual(child.returncode, 9)
                with receiver_lease(root): self.assertTrue((root / '.receiver.lock').exists())
            finally:
                child.stdin.close()
                if child.poll() is None: child.wait(timeout=5)
                child.stdout.close()

    def test_exception_releases_ownership_and_preserves_staging(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            staged = root / '.incoming-other'
            staged.mkdir(); (staged / 'file').write_bytes(b'preserve')
            with self.assertRaises(ValueError):
                with receiver_lease(root): raise ValueError('startup failed')
            with receiver_lease(root): self.assertEqual((staged / 'file').read_bytes(), b'preserve')

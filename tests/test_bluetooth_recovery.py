"""Run the real recovery script under isolated service/radio command mocks."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

@unittest.skipUnless(os.name == 'nt', 'Windows PowerShell recovery harness')
class RecoveryTests(unittest.TestCase):
    def scenario(self, name, success):
        repo = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            result = subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                str(repo/'tests/recovery-harness.ps1'), '-Scenario', name,
                '-RecoveryScript', str(repo/'desktop/RecoverBluetooth.ps1'), '-Root', str(root)],
                capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
            state = json.loads((root/'harness-state.json').read_text(encoding='utf-8-sig'))
            self.assertEqual(state['states']['CDPUserSvc_unit'], 'Running')
            self.assertEqual(state['states']['BluetoothUserService_unit'], 'Stopped')
            if name != 'restore-fails': self.assertEqual(state['states']['CDPSvc'], 'Running')
            if success:
                receipt=json.loads((root/'recovery-result.json').read_text(encoding='utf-8-sig'))
                self.assertTrue(receipt['publisher_ready_after_restore'])
                self.assertEqual(receipt['radio_cycled'], name == 'fallback')
            else:
                self.assertFalse((root/'recovery-result.json').exists())
                self.assertGreater(state['killed'], 0)
            return state

    def test_soft_success_avoids_radio_cycle(self):
        self.assertEqual(self.scenario('soft', True)['cycles'], 0)

    def test_blocked_soft_step_cycles_once_then_restores(self):
        self.assertEqual(self.scenario('fallback', True)['cycles'], 1)

    def test_advertising_lost_during_restore_is_not_adopted(self):
        self.assertEqual(self.scenario('lost-after-restore', False)['cycles'], 0)

    def test_service_restoration_failure_kills_publisher(self):
        self.scenario('restore-fails', False)

    def test_total_failure_still_restores_services(self):
        self.assertEqual(self.scenario('never-starts', False)['cycles'], 1)

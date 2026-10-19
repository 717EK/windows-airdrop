"""Preserve distinct failure causes instead of reporting every error as a decline."""
import errno
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'airdrop'))
from send import PeerRefused, error_event


class SendErrorTests(unittest.TestCase):
    def test_connection_file_disk_and_refusal_errors_stay_distinct(self):
        cases = [(ConnectionResetError(), 'connection_reset'), (BrokenPipeError(), 'connection_reset'),
                 (ConnectionRefusedError(), 'not_listening'),
                 (TimeoutError(), 'timeout'), (FileNotFoundError(), 'file_missing'),
                 (OSError(errno.ENOSPC, 'disk full'), 'disk_full'),
                 (PermissionError(), 'access_denied'), (PeerRefused('/Ask', 403), 'peer_refused')]
        for error, code in cases:
            with self.subTest(code=code):
                event = json.loads(json.dumps(error_event(error)))
                self.assertEqual(event['event'], 'send-error')
                self.assertEqual(event['code'], code)
                self.assertTrue(event['message'])

    def test_refused_upload_is_not_described_as_a_declined_approval(self):
        self.assertIn('upload', error_event(PeerRefused('/Upload', 500))['message'])
        self.assertIn('transfer request', error_event(PeerRefused('/Ask', 403))['message'])

    def test_invalid_data_diagnostic_is_bounded_and_one_line(self):
        message = error_event(ValueError('bad\r\n' + 'x' * 1000))['message']
        self.assertLessEqual(len(message), 300)
        self.assertNotIn('\n', message)
        self.assertNotIn('\r', message)

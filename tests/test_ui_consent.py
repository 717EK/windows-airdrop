"""Desktop consent channel: only a matching explicit yes accepts; silence declines."""
import contextlib
import io
import json
from pathlib import Path
import sys
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'airdrop'))
import service


class UiConsentTest(unittest.TestCase):
    def ask(self, answer_for, timeout=2):
        out = io.StringIO()
        def respond():
            for _ in range(200):
                lines = out.getvalue().splitlines()
                if lines:
                    event = json.loads(lines[0])
                    for answer in answer_for(event):
                        service._answers.put(answer)
                    return
                threading.Event().wait(0.01)
        threading.Thread(target=respond, daemon=True).start()
        with contextlib.redirect_stdout(out):
            result = service.ui_confirm("Phone", [{'FileName': 'a.png', 'FileSize': 5}], timeout)
        return result, [json.loads(line) for line in out.getvalue().splitlines()]

    def test_matching_yes_accepts(self):
        result, events = self.ask(lambda e: [{'id': 'stale', 'accept': True}, {'id': e['id'], 'accept': True}])
        self.assertTrue(result)
        self.assertEqual(events[0]['files'], ['a.png'])
        self.assertEqual(events[0]['bytes'], 5)

    def test_decline_and_timeout(self):
        self.assertFalse(self.ask(lambda e: [{'id': e['id'], 'accept': 'yes'}])[0])
        result, events = self.ask(lambda e: [], timeout=0.3)
        self.assertFalse(result)
        self.assertEqual(events[-1]['event'], 'ask-expired')

    def test_name_and_model_validation(self):
        self.assertEqual(service.display_name('  My PC\n'), 'My PC')
        for bad in ('', 'x' * 41):
            with self.assertRaises(Exception):
                service.display_name(bad)
        self.assertEqual(service.model_name('iMac21,1'), 'iMac21,1')
        with self.assertRaises(Exception):
            service.model_name('../evil')


if __name__ == '__main__':
    unittest.main()

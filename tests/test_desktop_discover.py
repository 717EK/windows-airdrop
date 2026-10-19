import sys
from pathlib import Path
import plistlib
import unittest
import io
import json
import tempfile
import threading
from contextlib import redirect_stdout
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'airdrop'))
from desktop_discover import endpoint, probe, discover


class DiscoveryTests(unittest.TestCase):
    @patch('desktop_discover.ServiceBrowser')
    @patch('desktop_discover.Zeroconf')
    @patch('desktop_discover.certificate', return_value=('cert', 'key'))
    @patch('desktop_discover.ssl.SSLContext')
    @patch('desktop_discover.probe')
    def test_peer_heard_after_scan_start_is_discovered(self, attempt, context, cert, zc, browser):
        device = {'event': 'device', 'name': 'Late iPhone', 'address': 'fe80::123', 'port': 8770}
        attempt.return_value = device
        with tempfile.TemporaryDirectory() as tmp:
            peers = Path(tmp) / 'candidates.json'
            peers.write_text('[]', encoding='utf-8')
            args = SimpleNamespace(address='fe80::456', ifindex=48, sid='001122334455', seconds=2,
                                   candidate=[], candidates_file=peers)
            updated = threading.Timer(0.4, lambda: peers.write_text(json.dumps(['fe80::123']), encoding='utf-8'))
            updated.start()
            output = io.StringIO()
            try:
                with redirect_stdout(output): discover(args)
            finally:
                updated.join()
            attempt.assert_called_once()
            self.assertIn('Late iPhone', output.getvalue())

    @patch('desktop_discover.ServiceBrowser')
    @patch('desktop_discover.Zeroconf')
    @patch('desktop_discover.certificate', return_value=('cert', 'key'))
    @patch('desktop_discover.ssl.SSLContext')
    @patch('desktop_discover.probe')
    def test_retry_refused_peer_and_list_it_once(self, attempt, context, cert, zc, browser):
        device = {'event': 'device', 'name': 'Test iPhone', 'address': 'fe80::123', 'port': 8770}
        attempt.side_effect = [ConnectionRefusedError(), device]
        args = SimpleNamespace(address='fe80::456', ifindex=48, sid='001122334455', seconds=2,
                               candidate=['fe80::123', 'fe80::123'])
        output = io.StringIO()
        with redirect_stdout(output):
            discover(args)
        self.assertEqual(attempt.call_count, 2)
        self.assertEqual(output.getvalue().count('"event":"device"'), 1)
        browser.return_value.cancel.assert_called_once()
        zc.return_value.close.assert_called_once()

    def test_only_native_link_local_endpoints(self):
        self.assertEqual(endpoint('fe80::123%48', 8770), ('fe80::123', 8770))
        for ip, port in [('127.0.0.1', 8770), ('::1', 8770), ('fe80::1', 0), ('fe80::1', 65536)]:
            with self.assertRaises(ValueError):
                endpoint(ip, port)

    @patch('desktop_discover.Connection')
    @patch('desktop_discover.post')
    def test_list_only_receivers_that_disclose_a_name(self, post, connection):
        post.return_value = plistlib.dumps({'ReceiverComputerName': 'Test iPhone'})
        result = probe('fe80::123', 8770, 'fe80::456', 48, None)
        self.assertEqual(result['name'], 'Test iPhone')
        self.assertEqual(result['port'], 8770)
        connection.return_value.close.assert_called_once()
        post.return_value = plistlib.dumps({})
        self.assertIsNone(probe('fe80::123', 8770, 'fe80::456', 48, None))

    @patch('desktop_discover.Connection')
    @patch('desktop_discover.post', side_effect=ConnectionRefusedError)
    def test_refused_endpoint_closes_socket(self, post, connection):
        with self.assertRaises(ConnectionRefusedError):
            probe('fe80::123', 8770, 'fe80::456', 48, None)
        connection.return_value.close.assert_called_once()


if __name__ == '__main__':
    unittest.main()

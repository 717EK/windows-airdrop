"""Actual HTTPS request/response tests and hostile archive boundary checks."""
import gzip
import hashlib
import http.client
import io
from pathlib import Path
import plistlib
import stat
import ssl
import sys
import tempfile
import threading
import time
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'airdrop'))
from protocol import *
import service
from service import certificate, make_server, read_body
from send import send_file
from announce import answer
from zeroconf._protocol.incoming import DNSIncoming


def newc(name, payload, mode=stat.S_IFREG | 0o600):
    encoded = name.encode() + b'\x00'
    fields = [1, mode, 0, 0, 1, 0, len(payload), 0, 0, 0, 0, len(encoded), 0]
    header = b'070701' + ''.join(f'{n:08x}' for n in fields).encode()
    entry = header + encoded
    entry += b'\x00' * ((-len(entry)) % 4)
    entry += payload + b'\x00' * ((-len(payload)) % 4)
    return entry


class PayloadTests(unittest.TestCase):
    def test_chunk_diagnostics_distinguish_normal_termination(self):
        import contextlib, json
        from email.message import Message
        from types import SimpleNamespace
        headers = Message(); headers['Transfer-Encoding'] = 'chunked'
        handler = SimpleNamespace(headers=headers, rfile=io.BytesIO(b'3\r\nabc\r\n0\r\n\r\n'))
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(read_body(handler, 1024, lambda body: body == b'abc'), b'abc')
        event = json.loads(output.getvalue())
        self.assertEqual(event['termination'], 'zero-chunk-and-trailer')
        self.assertEqual(event['received'], 3)

    def test_incomplete_archive_at_eof_is_refused(self):
        from email.message import Message
        from types import SimpleNamespace
        headers = Message()
        headers['Transfer-Encoding'] = 'chunked'
        handler = SimpleNamespace(headers=headers, rfile=io.BufferedReader(io.BytesIO(b'3\r\nabc\r\n')))
        with self.assertRaises(ValueError):
            read_body(handler, 1024, lambda body: False)

    def test_mdns_announcement_has_valid_ptr_srv_txt_and_aaaa(self):
        packet = DNSIncoming(answer('001122334455', 'fe80::211:22ff:fe33:4455'))
        self.assertTrue(packet.valid)
        self.assertEqual({record.type for record in packet.answers()}, {12, 33, 16, 28})

    def test_independent_newc_fixture(self):
        archive = newc('./hello.txt', b'hello\n') + newc('TRAILER!!!', b'')
        self.assertEqual(parse_cpio(archive), [('hello.txt', b'hello\n')])
        for end in range(len(archive) - 3):
            with self.assertRaises(ValueError):
                parse_cpio(archive[:end])

    def test_odc_roundtrip_stored_and_gzip(self):
        files = [('hello.txt', b'hello\x00world'), ('photo.jpg', bytes(range(256)))]
        self.assertEqual(parse_cpio(decode_dvzip(encode_dvzip(files))), files)
        archive = encode_cpio(files)
        self.assertEqual(parse_cpio(decode_dvzip(gzip.compress(archive))), files)
        stored = (len(archive) | 0x80000000).to_bytes(4, 'big') + archive
        self.assertEqual(parse_cpio(decode_dvzip(stored)), files)

    def test_rejects_bombs_truncation_and_trailing_compression(self):
        bomb = gzip.compress(b'X' * (1024 * 1024 + 1))
        from unittest.mock import patch
        with patch('protocol.MAX_ARCHIVE_BYTES', 1024 * 1024):
            for bad in [bomb, b'\x00\x00', b'\x00\x00\x01\x00x', gzip.compress(b'a') + b'extra']:
                with self.assertRaises(ValueError):
                    decode_dvzip(bad)
        valid = encode_cpio([('x', b'a')])
        with self.assertRaises(ValueError):
            parse_cpio(valid + b'bad')
        with self.assertRaises(ValueError):
            parse_cpio(encode_cpio([]))

    def test_links_refused_and_windows_names_flattened(self):
        malicious = newc('symlink', b'C:/Windows', stat.S_IFLNK | 0o777) + newc('TRAILER!!!', b'')
        with self.assertRaises(ValueError):
            parse_cpio(malicious)
        for source, expected in [('../../secret.txt', 'secret.txt'), ('CON.txt', '_CON.txt'),
                                  ('x:stream', 'x_stream'), ('x\u202eexe', 'x_exe')]:
            self.assertEqual(safe_name(source), expected)


class HTTPTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        service.ASK_COOLDOWN = 0  # tests decline and re-ask from the same address
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dest = Path(cls.tmp.name) / 'received'
        cls.dest.mkdir()
        cls.accept = True
        cls.prompts = []
        def confirm(sender, files):
            cls.prompts.append((sender, files))
            return cls.accept
        cls.server = make_server(('::1', 0), 0, cls.dest, confirm)
        cert, key = certificate(Path(cls.tmp.name) / 'identity', '001122334455')
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        cls.server.socket = context.wrap_socket(cls.server.socket, server_side=True, do_handshake_on_connect=False)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()
        cls.tmp.cleanup()

    def setUp(self):
        self.__class__.accept = True
        self.connection = http.client.HTTPSConnection('::1', self.server.server_port,
            context=ssl._create_unverified_context(), timeout=5)

    def tearDown(self):
        self.connection.close()

    def post(self, path, body, headers=None, chunked=False):
        self.connection.request('POST', path, io.BytesIO(body) if chunked else body,
                                headers=headers or {}, encode_chunked=chunked)
        reply = self.connection.getresponse()
        return reply.status, reply.read()

    def ask(self, name='hello.txt', size=64):
        return plistlib.dumps({'SenderComputerName': 'Test phone', 'TransferID': {'id': 'test-transfer'},
                              'Files': [{'FileName': name, 'FileSize': size}]}, fmt=plistlib.FMT_BINARY)

    def test_upload_larger_than_approved_size_is_rejected(self):
        self.assertEqual(self.post('/Ask', self.ask('small.txt', 1))[0], 200)
        before = set(self.dest.iterdir())
        self.assertEqual(self.post('/Upload', encode_dvzip([('small.txt', b'much more than one byte')]))[0], 400)
        self.assertEqual(set(self.dest.iterdir()), before)

    def test_received_file_is_marked_as_downloaded(self):
        self.assertEqual(self.post('/Ask', self.ask('motw.txt'))[0], 200)
        self.assertEqual(self.post('/Upload', encode_dvzip([('motw.txt', b'zone\n')]))[0], 200)
        saved = list(self.dest.glob('*/motw.txt'))
        self.assertEqual(len(saved), 1)
        self.assertIn('ZoneId=3', Path(str(saved[0]) + ':Zone.Identifier').read_text())

    def test_declined_peer_cannot_ask_again_immediately(self):
        service.ASK_COOLDOWN = 10
        try:
            self.__class__.accept = False
            self.assertEqual(self.post('/Ask', self.ask('first.txt'))[0], 403)
            self.connection.close()
            self.__class__.accept = True
            prompts = len(self.prompts)
            self.assertEqual(self.post('/Ask', self.ask('second.txt'))[0], 403)
            self.assertEqual(len(self.prompts), prompts)  # no second prompt was shown
        finally:
            service.ASK_COOLDOWN = 0
            self.server.cooldown.clear()

    def test_peer_text_cannot_forge_output_lines(self):
        self.assertEqual(service.clean('x\r{"event":"ask"}\nSaved transfer to C:\\'), 'x?{"event":"ask"}?Saved transfer to C:\\')
        self.assertEqual(service.clean('name\u202eexe.pdf'), 'name?exe.pdf')

    def test_discover_and_consented_chunked_upload_with_exact_hash(self):
        status, body = self.post('/Discover', plistlib.dumps({}), chunked=True)
        self.assertEqual(status, 200)
        self.assertTrue(plistlib.loads(body)['IsAirDropable'])
        self.assertIn(b'ContainerFormats', plistlib.loads(body)['ReceiverMediaCapabilities'])
        self.assertEqual(self.post('/Ask', self.ask(), chunked=True)[0], 200)
        status, _ = self.post('/Upload', encode_dvzip([('hello.txt', b'hello\n')]),
                              {'TransferID': 'test-transfer', 'Content-Type': 'application/x-dvzip'}, chunked=True)
        self.assertEqual(status, 200)
        saved = list(self.dest.glob('*/hello.txt'))
        self.assertEqual(len(saved), 1)
        self.assertEqual(hashlib.sha256(saved[0].read_bytes()).digest(), hashlib.sha256(b'hello\n').digest())
        self.assertEqual(self.prompts[-1][0], 'Test phone')
        self.assertEqual(self.post('/Upload', b'no approval')[0], 403)


    def test_sender_and_receiver_exchange_file_over_real_tls(self):
        path = Path(self.tmp.name) / 'outbound.txt'
        path.write_bytes(b'PC to iPhone protocol test\n')
        import contextlib, json
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            digest = send_file(path, '::1', self.server.server_port, '::1', 0, '001122334455', ssl._create_unverified_context())
        events = [json.loads(line) for line in output.getvalue().splitlines() if line.startswith('{"event": "send-trace"')]
        self.assertEqual(len({event['transfer_id'] for event in events}), 1)
        self.assertEqual(events[0]['name'], path.name)
        self.assertEqual(events[-1]['phase'], 'accepted')
        self.assertEqual(events[-1]['source_bytes'], path.stat().st_size)
        self.assertEqual(events[-1]['sha256'], hashlib.sha256(path.read_bytes()).hexdigest())
        self.assertEqual([e['elapsed_ms'] for e in events], sorted(e['elapsed_ms'] for e in events))
        self.assertEqual([e['endpoint'] for e in events if e['phase'] == 'http-response'], ['/Discover', '/Ask', '/Upload'])
        outputs = list(self.dest.glob('*/outbound.txt'))
        self.assertEqual(len(outputs), 1)
        self.assertEqual(hashlib.sha256(outputs[0].read_bytes()).hexdigest(), digest)

    def test_sender_reports_real_refusal_without_attempting_upload(self):
        from unittest.mock import patch
        import send
        path = Path(self.tmp.name) / 'declined-sender.txt'
        path.write_bytes(b'This file must not be uploaded after a decline.\n')
        self.__class__.accept = False
        with patch('send.post', wraps=send.post) as requests:
            with self.assertRaises(send.PeerRefused) as refused:
                send_file(path, '::1', self.server.server_port, '::1', 0, '001122334455', ssl._create_unverified_context())
        self.assertEqual([call.args[1] for call in requests.call_args_list], ['/Discover', '/Ask'])
        self.assertEqual(send.error_event(refused.exception)['code'], 'peer_refused')
        self.assertIn('transfer request (HTTP 403)', send.error_event(refused.exception)['message'])
        self.assertFalse(list(self.dest.glob('*/declined-sender.txt')))

    def test_https_client_and_receiver_disable_nagle_for_control_messages(self):
        from unittest.mock import patch
        import socket
        from send import Connection, post
        options = []
        handler = self.server.RequestHandlerClass
        setup = handler.setup
        def record(instance):
            setup(instance)
            options.append(instance.connection.getsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY))
        connection = Connection('::1', self.server.server_port, '::1', 0, ssl._create_unverified_context())
        try:
            with patch.object(handler, 'setup', record):
                post(connection, '/Discover', plistlib.dumps({}))
            self.assertEqual(connection.sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY), 1)
            self.assertEqual(options, [1])
        finally:
            connection.close()

    def test_16_mib_sender_and_receiver_over_real_tls(self):
        path = Path(self.tmp.name) / 'large-tls.bin'
        with path.open('wb') as out:
            for _ in range(256):
                out.write(bytes(range(256)) * 256)
        with path.open('rb') as source:
            expected = hashlib.file_digest(source, 'sha256').hexdigest()
        digest = send_file(path, '::1', self.server.server_port, '::1', 0, '001122334455', ssl._create_unverified_context())
        self.assertEqual(digest, expected)
        saved = list(self.dest.glob('*/large-tls.bin'))
        self.assertEqual(len(saved), 1)
        self.assertEqual(saved[0].stat().st_size, 16 * 1024 * 1024)
        with saved[0].open('rb') as source:
            self.assertEqual(hashlib.file_digest(source, 'sha256').hexdigest(), expected)

    def test_sender_retries_transient_discovery_without_repeating_approval(self):
        from unittest.mock import patch
        import send
        import streaming
        path = Path(self.tmp.name) / 'reopened.txt'
        path.write_bytes(b'Phone listener reopened\n')
        original = send.post
        calls = []
        def interrupted(conn, endpoint, *args, **kwargs):
            self.assertEqual(prepared.call_count, 1, 'prepare once before opening the discovery window')
            calls.append(endpoint)
            if len(calls) == 1:
                raise ConnectionRefusedError('phone listener temporarily closed')
            return original(conn, endpoint, *args, **kwargs)
        with patch('streaming.prepare', wraps=streaming.prepare) as prepared, patch('send.post', side_effect=interrupted):
            send_file(path, '::1', self.server.server_port, '::1', 0, '001122334455', ssl._create_unverified_context())
        self.assertEqual(calls, ['/Discover', '/Discover', '/Ask', '/Upload'])
        self.assertEqual(list(self.dest.glob('*/reopened.txt'))[0].read_bytes(), path.read_bytes())

    def test_sender_stops_discovery_retry_at_deadline(self):
        from unittest.mock import patch
        path = Path(self.tmp.name) / 'offline.txt'
        path.write_bytes(b'offline\n')
        with patch('send.Connection') as connection, patch('send.time.monotonic', side_effect=[0, 21, 22]), patch('send.post', side_effect=ConnectionRefusedError) as post:
            with self.assertRaises(ConnectionRefusedError):
                send_file(path, '::1', self.server.server_port, '::1', 0, '001122334455', ssl._create_unverified_context())
        post.assert_called_once()
        connection.return_value.close.assert_called_once()

    def test_complete_archive_without_final_http_chunk_is_saved(self):
        self.assertEqual(self.post('/Ask', self.ask('stalled.txt'))[0], 200)
        body = encode_dvzip([('stalled.txt', b'complete upload\n')])
        self.connection.putrequest('POST', '/Upload')
        self.connection.putheader('Transfer-Encoding', 'chunked')
        self.connection.endheaders()
        self.connection.send(f'{len(body):x}\r\n'.encode() + body + b'\r\n')
        response = self.connection.getresponse()
        self.assertEqual(response.status, 200)
        self.assertEqual(response.getheader('Connection'), 'close')
        response.read()
        outputs = list(self.dest.glob('*/stalled.txt'))
        self.assertEqual(len(outputs), 1)
        self.assertEqual(outputs[0].read_bytes(), b'complete upload\n')

    def test_interrupted_upload_can_be_retried_without_publishing_partial_data(self):
        self.assertEqual(self.post('/Ask', self.ask('retry.txt'))[0], 200)
        before = set(self.dest.iterdir())
        self.connection.putrequest('POST', '/Upload')
        self.connection.putheader('Transfer-Encoding', 'chunked')
        self.connection.endheaders()
        self.connection.send(b'1000\r\npartial')
        self.connection.close()
        time.sleep(0.2)
        self.assertEqual(set(self.dest.iterdir()), before)
        self.assertEqual(self.post('/Ask', self.ask('retry.txt'))[0], 200)
        self.assertEqual(self.post('/Upload', encode_dvzip([('retry.txt', b'hello\n')]), chunked=True)[0], 200)
        self.assertEqual(list(self.dest.glob('*/retry.txt'))[0].read_bytes(), b'hello\n')

    def test_partial_chunk_header_survives_a_read_timeout(self):
        self.assertEqual(self.post('/Ask', self.ask('paused.txt'))[0], 200)
        body = encode_dvzip([('paused.txt', b'paused upload\n')])
        self.connection.putrequest('POST', '/Upload')
        self.connection.putheader('Transfer-Encoding', 'chunked')
        self.connection.endheaders()
        header = f'{len(body):x}\r\n'.encode()
        self.connection.send(header[:1])
        def finish():
            time.sleep(3.3)
            self.connection.send(header[1:] + body + b'\r\n0\r\n\r\n')
        sender = threading.Thread(target=finish)
        sender.start()
        try:
            response = self.connection.getresponse()
            self.assertEqual(response.status, 200)
            response.read()
        finally:
            sender.join()
        outputs = list(self.dest.glob('*/paused.txt'))
        self.assertEqual(len(outputs), 1)
        self.assertEqual(outputs[0].read_bytes(), b'paused upload\n')

    def test_upload_requires_consent_and_decline_is_enforced(self):
        self.assertEqual(self.post('/Upload', encode_dvzip([('x', b'x')]))[0], 403)
        self.connection.close()
        self.__class__.accept = False
        self.assertEqual(self.post('/Ask', self.ask())[0], 403)

    def test_wrong_transfer_id_and_unapproved_names_are_rejected(self):
        self.assertEqual(self.post('/Ask', self.ask())[0], 200)
        self.assertEqual(self.post('/Upload', b'ignored', {'TransferID': 'other'})[0], 400)
        self.connection.close()
        self.assertEqual(self.post('/Ask', self.ask())[0], 200)
        before = set(self.dest.iterdir())
        self.assertEqual(self.post('/Upload', encode_dvzip([('other.txt', b'bad')]))[0], 400)
        self.assertEqual(set(self.dest.iterdir()), before)

    def test_oversized_ask_and_ambiguous_framing_are_rejected(self):
        bad = plistlib.dumps({'Files': [{'FileName': 'huge', 'FileSize': MAX_FILE_BYTES + 1}]})
        self.assertEqual(self.post('/Ask', bad)[0], 400)
        self.connection.close()
        self.assertEqual(self.post('/Discover', b'0\r\n\r\n',
                         {'Content-Length': '5', 'Transfer-Encoding': 'chunked'})[0], 400)


if __name__ == '__main__':
    unittest.main()

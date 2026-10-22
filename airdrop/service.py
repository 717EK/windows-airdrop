"""Windows AirDrop research CLI. Everyone mode; explicit consent for each receive."""
import argparse
import io
import tempfile
import ctypes
import datetime
import hashlib
import http.client
import ipaddress
import json
import os
from pathlib import Path
import plistlib
import queue
import re
import shutil
import socket
import ssl
import threading
import time
import sys
import uuid
import unicodedata
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from protocol import MAX_FILE_BYTES, MAX_ARCHIVE_BYTES, MAX_FILES, decode_dvzip, parse_cpio

NAME = 'Windows PC'
MODEL = 'MacBookPro18,3'
MAX_METADATA = 256 * 1024
PEER_CONNECTIONS = 3      # concurrent connections per peer address
CONNECTION_SECONDS = 300  # absolute budget for handshake/metadata
UPLOAD_SECONDS = 7200     # approved, streamed upload; idle reads still time out at 120 s
ASK_COOLDOWN = 10         # seconds before a declined peer may ask again
MEDIA_CAPABILITIES = Path(__file__).with_name('media_capabilities.json').read_bytes()


QUICK_STALL_SECONDS = 0.25  # probe this soon after the sender pauses at a chunk boundary
FULL_STALL_SECONDS = 3      # the full completeness check, as before, once this quiet


def read_body(handler, limit, complete=None, progress=None, target=None, quick=None):
    out = target if target is not None else io.BytesIO()
    received = 0
    began = time.perf_counter()
    def finish(termination='content-length'):
        if complete is not None:
            print(json.dumps({'event': 'http-framing', 'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                'termination': termination, 'received': received,
                'elapsed_ms': round((time.perf_counter() - began) * 1000, 3)}), flush=True)
        out.flush()
        return out.getvalue() if target is None else None
    def is_complete(check=None):
        position = out.tell()
        try:
            out.flush(); out.seek(0)
            return (check or complete)(out.getvalue() if target is None else out)
        finally:
            out.seek(position)
    def copy(size):
        nonlocal received
        while size:
            data = exact(min(size, 64 * 1024))
            out.write(data); received += len(data); size -= len(data)
            if progress: progress(received)
    te = handler.headers.get_all('Transfer-Encoding', [])
    cl = handler.headers.get_all('Content-Length', [])
    if len(te) > 1 or len(cl) > 1 or (te and cl):
        raise ValueError('ambiguous request framing')
    def exact(n):
        data = handler.rfile.read(n)
        if len(data) != n:
            raise ValueError('truncated body')
        return data
    if te:
        if te[0].lower().strip() != 'chunked':
            raise ValueError('unsupported transfer encoding')
        checked = -1        # archive length at the last full completeness check
        quick_checked = -1  # archive length at the last quick probe

        def next_chunk():
            nonlocal checked, quick_checked
            reader = handler.rfile
            raw = getattr(reader, 'raw', None)
            if complete is None or not hasattr(raw, '_timeout_occurred'):
                return reader.readline(128)
            line = bytearray()
            previous_timeout = handler.connection.gettimeout()
            deadline = time.monotonic() + (previous_timeout or 120)
            quiet_since = time.monotonic()
            # iOS often never sends the final zero-length chunk: it stops after the
            # archive and waits for our answer. A short quiet period plus a cheap
            # probe answers it in ~0.25 s instead of 3 s.
            handler.connection.settimeout(QUICK_STALL_SECONDS if quick else FULL_STALL_SECONDS)
            try:
                while len(line) < 128:
                    try:
                        available = reader.peek(1)
                    except socket.timeout:
                        # CPython marks SocketIO unusable after a timeout; peek consumed nothing.
                        raw._timeout_occurred = False
                        # Re-check only after new data, so mid-transfer pauses don't use up the recovery.
                        if not line and received:
                            done = None
                            if quick and received != quick_checked:
                                quick_checked = received
                                # The probe filters; the full check proves it before we stop reading.
                                if is_complete(quick):
                                    checked = received
                                    if is_complete():
                                        done = 'quick probe, full check'
                            if done is None and received != checked and \
                                    time.monotonic() - quiet_since >= FULL_STALL_SECONDS - 0.01:
                                checked = received
                                if is_complete():
                                    done = 'full check'
                            if done:
                                handler.close_connection = True
                                print(f'Upload archive complete after {received} bytes ({done}); answering stalled final chunk', flush=True)
                                return None
                        if time.monotonic() >= deadline:
                            raise
                        continue
                    if not available:
                        if not line and received and is_complete():
                            handler.close_connection = True
                            return None
                        raise ValueError(f'upload ended before chunk header (received={received} bytes)')
                    take = min(len(available), 128 - len(line))
                    newline = available.find(b'\n', 0, take)
                    line.extend(reader.read(newline + 1 if newline >= 0 else take))
                    if newline >= 0:
                        break
                return bytes(line)
            finally:
                handler.connection.settimeout(previous_timeout)

        while True:
            line = next_chunk()
            if line is None:
                return finish("archive-validated-without-final-chunk")
            if not line.endswith(b'\r\n'):
                raise ValueError(f'invalid chunk header (line_bytes={len(line)}, received={received} bytes)')
            token = line.split(b';', 1)[0].strip()
            if not token or any(c not in b'0123456789abcdefABCDEF' for c in token):
                raise ValueError('invalid chunk size')
            size = int(token, 16)
            if size > limit - received:
                raise ValueError('request exceeds size limit')
            if not size:
                trailer_size = 0
                while True:
                    line = handler.rfile.readline(1024)
                    trailer_size += len(line)
                    if not line.endswith(b'\r\n') or trailer_size > 8192:
                        raise ValueError('invalid chunk trailer')
                    if line == b'\r\n':
                        return finish('zero-chunk-and-trailer')
            copy(size)
            if exact(2) != b'\r\n':
                raise ValueError('invalid chunk terminator')
    if not cl or not cl[0].isascii() or not cl[0].isdigit():
        raise ValueError('Content-Length or chunked body required')
    length = int(cl[0])
    if length > limit:
        raise ValueError('request exceeds size limit')
    copy(length)
    return finish()


def certificate(folder, sid):
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID
    folder.mkdir(parents=True, exist_ok=True)
    cert, key = folder / 'certificate.pem', folder / 'key.pem'
    if cert.exists() and key.exists():
        existing = x509.load_pem_x509_certificate(cert.read_bytes())
        names = existing.subject.get_attributes_for_oid(NameOID.COMMON_NAME)
        fresh = existing.not_valid_after_utc > datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(days=1)
        if fresh and names and names[0].value == sid:
            return cert, key
    private = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, sid)])
    now = datetime.datetime.now(datetime.timezone.utc)
    pair = x509.CertificateBuilder().subject_name(subject).issuer_name(subject).public_key(private.public_key())
    pair = pair.serial_number(x509.random_serial_number()).not_valid_before(now - datetime.timedelta(minutes=5))
    pair = pair.not_valid_after(now + datetime.timedelta(days=30)).sign(private, hashes.SHA256())
    key.write_bytes(private.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL,
                                         serialization.NoEncryption()))
    cert.write_bytes(pair.public_bytes(serialization.Encoding.PEM))
    return cert, key


def clean(text):
    """Make peer-controlled text safe for one stdout line: the desktop app parses this stream."""
    return ''.join('?' if unicodedata.category(c).startswith(('C', 'Z')) and c != ' ' else c for c in str(text))[:300]


def mark_of_the_web(path):
    """Tag a received file as Internet-zone so SmartScreen and Office Protected View apply."""
    try:
        with open(f'{path}:Zone.Identifier', 'w', encoding='ascii') as zone:
            zone.write('[ZoneTransfer]\r\nZoneId=3\r\nReferrerUrl=airdrop:\r\n')
    except OSError as error:
        print(f'Could not mark {path.name} as downloaded: {type(error).__name__}', flush=True)


def confirm_receive(sender, files):
    from protocol import safe_name
    message = f'Receive {len(files)} file(s) from {safe_name(sender)}?\n\n' + '\n'.join(safe_name(str(f.get('FileName', 'file'))) for f in files)
    return ctypes.windll.user32.MessageBoxW(None, message, 'AirDrop to Windows', 0x00000004 | 0x00000030 | 0x00040000) == 6


_answers = queue.Queue()
def read_answers(stream):
    while True:
        try:
            line = stream.readline()
            if not line:
                return
            _answers.put(json.loads(line))
        except Exception as error:  # one bad line must not stop later answers
            print(f'Ignored desktop answer: {type(error).__name__}', flush=True)


def ui_confirm(sender, files, timeout=55):
    """Ask the desktop app (JSON on stdout, answer on stdin). Anything but an explicit yes declines."""
    from protocol import safe_name
    ask_id = uuid.uuid4().hex
    print(json.dumps({'event': 'ask', 'id': ask_id, 'sender': safe_name(sender),
                      'files': [safe_name(str(f.get('FileName', 'file'))) for f in files],
                      'bytes': sum(f.get('FileSize', 0) for f in files)}), flush=True)
    deadline = time.monotonic() + timeout
    while (left := deadline - time.monotonic()) > 0:
        try:
            answer = _answers.get(timeout=left)
        except queue.Empty:
            break
        if isinstance(answer, dict) and answer.get('id') == ask_id:
            return answer.get('accept') is True
    print(json.dumps({'event': 'ask-expired', 'id': ask_id}), flush=True)
    return False


def display_name(value):
    value = ''.join(c for c in value if c.isprintable()).strip()
    if not 1 <= len(value) <= 40:
        raise argparse.ArgumentTypeError('name must be 1-40 printable characters')
    return value


def model_name(value):
    if not re.fullmatch(r'[A-Za-z]{2,16}\d{1,3},\d{1,3}', value):
        raise argparse.ArgumentTypeError('invalid model identifier')
    return value


def make_server(address, port, destination, confirm=confirm_receive, receiver_name=NAME, receiver_model=MODEL):
    class Server(ThreadingHTTPServer):
        address_family = socket.AF_INET6
        daemon_threads = True
        block_on_close = False
        request_queue_size = 8
        # Bound concurrent TLS handshakes and requests, overall and per peer.
        slots = threading.BoundedSemaphore(8)
        per_peer = {}
        per_peer_lock = threading.Lock()
        def process_request(self, request, client_address):
            peer = client_address[0]
            with self.per_peer_lock:
                crowded = self.per_peer.get(peer, 0) >= PEER_CONNECTIONS
                if not crowded:
                    self.per_peer[peer] = self.per_peer.get(peer, 0) + 1
            if crowded:
                request.close()
                return
            if not self.slots.acquire(False):
                self.release_peer(peer)
                request.close()
                return
            try:
                super().process_request(request, client_address)
            except BaseException:
                self.slots.release()
                self.release_peer(peer)
                raise
        def release_peer(self, peer):
            with self.per_peer_lock:
                left = self.per_peer.get(peer, 1) - 1
                if left > 0:
                    self.per_peer[peer] = left
                else:
                    self.per_peer.pop(peer, None)
        def handle_error(self, request, client_address):
            if isinstance(sys.exc_info()[1], OSError):
                return
            super().handle_error(request, client_address)
        def process_request_thread(self, request, client_address):
            try:
                super().process_request_thread(request, client_address)
            finally:
                self.slots.release()
                self.release_peer(client_address[0])

    ask_slot = threading.Lock()  # one prompt at a time; others are declined, not queued
    cooldown = {}

    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'
        disable_nagle_algorithm = True
        approved = None
        def setup(self):
            self.request.settimeout(120)
            # Absolute budget per connection, so slow peers cannot hold slots indefinitely.
            self.watchdog = threading.Timer(CONNECTION_SECONDS, self.abort)
            self.watchdog.daemon = True
            self.watchdog.start()
            try:
                self.request.do_handshake()
            except (ssl.SSLError, OSError) as error:
                self.watchdog.cancel()
                print(f'TLS handshake failed: {type(error).__name__}: {clean(error)}', flush=True)
                self.close_connection = True
                raise
            super().setup()
            print(f"TLS ready peer={self.client_address[0]} version={self.connection.version()}", flush=True)
        def abort(self):
            try:
                socket.socket.shutdown(self.request, socket.SHUT_RDWR)
            except OSError:
                pass
        def finish(self):
            self.watchdog.cancel()
            super().finish()
        def log_message(self, fmt, *args):
            print(f"peer={self.client_address[0]} " + clean(fmt % args), flush=True)
        def answer(self, status, data=b''):
            self.send_response(status)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Content-Type', 'application/x-apple-binary-plist')
            if status != 200 or self.close_connection:
                self.send_header('Connection', 'close')
                self.close_connection = True
            self.end_headers()
            self.wfile.write(data)
        def do_POST(self):
            try:
                if self.path == '/Upload':
                    if not self.approved or time.monotonic() - self.approved[0] > 120:
                        self.answer(403)
                        return
                    _, transfer_id, asked_files = self.approved
                    self.approved = None  # one upload per explicit approval
                    header_id = self.headers.get('TransferID')
                    if transfer_id and header_id and header_id != transfer_id:
                        raise ValueError('transfer identifier mismatch')
                    self.watchdog.cancel()
                    self.watchdog = threading.Timer(UPLOAD_SECONDS, self.abort)
                    self.watchdog.daemon = True
                    self.watchdog.start()
                    print('Upload started; reading approved archive', flush=True)
                    from protocol import safe_name
                    label = safe_name(str(asked_files[0].get('FileName', 'file')))
                    if len(asked_files) > 1:
                        label += f' and {len(asked_files) - 1} more'
                    total = sum(f.get('FileSize', 0) for f in asked_files)
                    last = [0.0]
                    def progress(received, final=False):
                        # Archive bytes are compressed, so this is an estimate of file bytes.
                        if final or time.monotonic() - last[0] >= 0.25:
                            last[0] = time.monotonic()
                            print(json.dumps({'event': 'progress', 'name': label, 'received': received, 'total': total}), flush=True)
                    progress(0, True)
                    from streaming import complete, likely_complete, receive
                    approved = {safe_name(f['FileName']): f.get('FileSize', 0) for f in asked_files}
                    if len(approved) != len(asked_files):
                        raise ValueError('duplicate approved file names')
                    with tempfile.TemporaryFile(dir=destination) as raw:
                        read_body(self, MAX_ARCHIVE_BYTES, lambda body: complete(body, destination), progress, target=raw,
                                  quick=likely_complete)
                        try:
                            final_dir, receipts = receive(raw, destination, approved, mark_of_the_web)
                        except OSError as error:
                            print(f'Could not save transfer: {type(error).__name__}', flush=True)
                            self.answer(500)
                            return
                    for receipt in receipts: print(json.dumps(receipt), flush=True)
                    print(f'Saved transfer to {final_dir}', flush=True)
                    self.answer(200)
                    return
                if self.path not in ('/Discover', '/Ask'):
                    self.answer(404)
                    return
                data = plistlib.loads(read_body(self, MAX_METADATA))
                if not isinstance(data, dict):
                    raise ValueError('metadata must be a dictionary')
                if self.path == '/Ask':
                    peer = self.client_address[0]
                    files = data.get('Files')
                    if not isinstance(files, list) or not 1 <= len(files) <= MAX_FILES:
                        raise ValueError('only file transfers are supported')
                    total = 0
                    for f in files:
                        if not isinstance(f, dict) or not isinstance(f.get('FileName'), str) or len(f['FileName']) > 1024:
                            raise ValueError('invalid file metadata')
                        size = f.get('FileSize', 0)
                        if type(size) is not int or size < 0 or f.get('FileIsDirectory'):
                            raise ValueError('invalid file size or directory transfer')
                        total += size
                    if total > MAX_FILE_BYTES:
                        raise ValueError('transfer exceeds 1 GiB')
                    if shutil.disk_usage(destination).free < total * 3 + 64 * 1024 * 1024:
                        raise ValueError('not enough free disk space for this transfer')
                    if time.monotonic() < cooldown.get(peer, 0) or not ask_slot.acquire(blocking=False):
                        print(f'Declined ask from {peer}: another request is pending or it was just declined', flush=True)
                        self.answer(403)
                        return
                    try:
                        accepted = confirm(str(data.get('SenderComputerName', 'nearby device'))[:128], files)
                    finally:
                        ask_slot.release()
                    if not accepted:
                        cooldown[peer] = time.monotonic() + ASK_COOLDOWN
                        self.answer(403)
                        return
                    transfer_id = data.get('TransferID', {})
                    if isinstance(transfer_id, dict):
                        transfer_id = transfer_id.get('id')
                    self.approved = (time.monotonic(), transfer_id, files)
                self.answer(200, plistlib.dumps({'ReceiverComputerName': receiver_name, 'ReceiverModelName': receiver_model,
                    'ReceiverMediaCapabilities': MEDIA_CAPABILITIES, 'IsAirDropable': True}, fmt=plistlib.FMT_BINARY))
            except (ValueError, plistlib.InvalidFileException, UnicodeError, OverflowError, RecursionError) as error:
                print(f'Refused malformed transfer: {clean(error)}', flush=True)
                self.answer(400)
            except (OSError, ssl.SSLError) as error:
                self.close_connection = True
                print(f'Transfer connection failed: {type(error).__name__}', flush=True)

    server = Server(address, Handler)
    server.cooldown = cooldown
    return server


def save(destination, files):
    transfer_id = uuid.uuid4().hex
    transfer_dir = destination / ('.incoming-' + transfer_id)
    transfer_dir.mkdir()
    receipts = []
    try:
        used = set()
        for name, data in files:
            if name.casefold() in used:
                raise ValueError('duplicate output name')
            used.add(name.casefold())
            with (transfer_dir / name).open('xb') as out:
                out.write(data)
            mark_of_the_web(transfer_dir / name)
            receipts.append({'received': name, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
    except BaseException:
        if transfer_dir.resolve().parent != destination.resolve():
            raise ValueError('invalid cleanup directory')
        shutil.rmtree(transfer_dir)
        raise
    final_dir = destination / transfer_id
    transfer_dir.rename(final_dir)
    return final_dir, receipts


def serve(args):
    from ownership import receiver_lease
    destination = Path.home() / 'Downloads' / 'Windows AirDrop'
    destination.mkdir(parents=True, exist_ok=True)
    with receiver_lease(destination):
        serve_locked(args, destination)


def serve_locked(args, destination):
    from zeroconf import Zeroconf, ServiceInfo, IPVersion
    ip = ipaddress.IPv6Address(args.address.split('%')[0])
    if not ip.is_link_local:
        raise SystemExit('receiver must bind the AWDL link-local address')
    # Folder age cannot prove another receiver has released its staging files.
    cert, key = certificate(Path.home() / '.windows-airdrop' / 'identity', args.sid)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(cert, key)
    confirm = confirm_receive
    if args.ui_consent:
        threading.Thread(target=read_answers, args=(sys.stdin,), daemon=True).start()
        confirm = ui_confirm
    server = make_server((str(ip), 8771, 0, args.ifindex), 8771, destination, confirm, args.name, args.model)
    server.socket = context.wrap_socket(server.socket, server_side=True, do_handshake_on_connect=False)
    zc = Zeroconf(interfaces=[args.ifindex], ip_version=IPVersion.V6Only)
    info = ServiceInfo('_airdrop._tcp.local.', f'{args.sid}._airdrop._tcp.local.', addresses=[ip.packed],
                       port=8771, properties={'flags': '136'}, server=f'{args.sid}.local.', interface_index=args.ifindex)
    try:
        zc.register_service(info, cooperating_responders=True)  # Native and UDP announcers share this MAC-derived identity.
        timer = threading.Timer(args.seconds, server.shutdown)
        timer.daemon = True
        timer.start()
        print(f'{args.name} receiving on [{ip}%{args.ifindex}]:8771 into {destination}', flush=True)
        server.serve_forever()
    finally:
        zc.unregister_service(info)
        zc.close()
        server.server_close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--address', required=True)
    parser.add_argument('--ifindex', type=int, required=True)
    parser.add_argument('--sid', required=True)
    parser.add_argument('--seconds', type=int, default=300)
    parser.add_argument('--name', type=display_name, default=NAME)
    parser.add_argument('--model', type=model_name, default=MODEL)
    parser.add_argument('--ui-consent', action='store_true', help='ask the desktop app over stdin/stdout')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600 or len(args.sid) != 12 or any(c not in '0123456789abcdef' for c in args.sid):
        parser.error('invalid duration or service ID')
    serve(args)

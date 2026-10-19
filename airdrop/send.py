"""Send one explicitly selected file through the native AirDrop HTTPS protocol."""
import argparse
import datetime
import errno
import tempfile
import time
import json
import hashlib
import http.client
import io
import ipaddress
from pathlib import Path
import plistlib
import socket
import ssl
import uuid
import traceback

from protocol import MAX_FILE_BYTES
from service import NAME, MODEL, MAX_METADATA, certificate, clean, display_name, model_name


class PeerRefused(ValueError):
    def __init__(self, endpoint, status):
        self.endpoint, self.status = endpoint, status
        super().__init__(f'{endpoint} refused: HTTP {status}')


def error_event(error):
    if isinstance(error, PeerRefused):
        code = 'peer_refused'
        phase = 'transfer request' if error.endpoint == '/Ask' else 'upload' if error.endpoint == '/Upload' else 'discovery'
        message = f'The receiving device refused {phase} (HTTP {error.status}). Check it before retrying.'
    elif isinstance(error, (ConnectionResetError, BrokenPipeError)):
        code, message = 'connection_reset', 'The connection ended during transfer. Check the iPhone before retrying; the file may be incomplete.'
    elif isinstance(error, ConnectionRefusedError):
        code, message = 'not_listening', 'The iPhone is not accepting AirDrop connections. Refresh Everyone mode and the device list.'
    elif isinstance(error, TimeoutError):
        code, message = 'timeout', 'The transfer timed out. Check the iPhone before retrying.'
    elif isinstance(error, FileNotFoundError):
        code, message = 'file_missing', 'The selected file is no longer available. Choose it again.'
    elif isinstance(error, OSError) and error.errno == errno.ENOSPC:
        code, message = 'disk_full', 'Windows has insufficient free space to prepare this transfer.'
    elif isinstance(error, PermissionError):
        code, message = 'access_denied', 'Windows denied file or network access. Check permissions and the logs.'
    elif isinstance(error, ssl.SSLError):
        code, message = 'tls', 'The secure connection failed. Refresh the device list and try again.'
    elif isinstance(error, ValueError):
        code, message = 'invalid_data', clean(error)[:300]
    else:
        code, message = 'failed', 'The transfer failed. Open the logs for details.'
    return {'event': 'send-error', 'code': code, 'message': message}


class Connection(http.client.HTTPSConnection):
    def __init__(self, peer, port, address, ifindex, context, trace=None):
        super().__init__(peer, port, context=context, timeout=120, blocksize=64 * 1024)
        self.trace = trace
        self.source_ip = address
        self.ifindex = ifindex
    def connect(self):
        transport = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
        try:
            transport.settimeout(self.timeout)
            transport.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            transport.bind((self.source_ip, 0, 0, self.ifindex))
            if self.trace: self.trace("tcp-start")
            transport.connect((self.host, self.port, 0, self.ifindex))
            if self.trace: self.trace("tcp-connected")
            self.sock = self._context.wrap_socket(transport, server_hostname=self.host)
            if self.trace: self.trace("tls-ready", tls=self.sock.version())
        except BaseException:
            transport.close()
            raise


def post(conn, endpoint, body, headers=None, chunked=False):
    trace = getattr(conn, "trace", None)
    if trace: trace("http-request", endpoint=endpoint)
    conn.request('POST', endpoint, (io.BytesIO(body) if isinstance(body, bytes) else body) if chunked else body,
                 headers=headers or {'Content-Type': 'application/octet-stream'}, encode_chunked=chunked)
    if trace: trace("http-body-written", endpoint=endpoint)
    response = conn.getresponse()
    if trace: trace("http-response", endpoint=endpoint, status=response.status)
    data = response.read(MAX_METADATA + 1)
    if len(data) > MAX_METADATA:
        conn.close()
        raise ValueError('oversized peer response')
    if response.status != 200:
        raise PeerRefused(endpoint, response.status)
    return data


def send_file(path, peer, port, address, ifindex, sid, context, name=NAME, model=MODEL):
    path = Path(path)
    if not path.is_file() or path.stat().st_size > MAX_FILE_BYTES:
        raise ValueError('choose a regular file of at most 1 GiB')
    conn = None
    transfer_id = str(uuid.uuid4()).upper()
    started = time.perf_counter()
    phase = 'preparing'
    def trace(stage, **details):
        print(json.dumps({'event': 'send-trace', 'transfer_id': transfer_id,
            'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'elapsed_ms': round((time.perf_counter() - started) * 1000, 3),
            'phase': stage, **details}), flush=True)
    trace('selected', name=path.name, source_bytes=path.stat().st_size, peer=peer, port=port)
    extension = path.suffix.lower()
    uti = {'.txt': 'public.plain-text', '.png': 'public.png', '.jpg': 'public.jpeg',
           '.jpeg': 'public.jpeg', '.pdf': 'com.adobe.pdf'}.get(extension, 'public.data')
    ask = {'SenderComputerName': name, 'SenderModelName': model, 'SenderID': sid,
           'SenderIdentityAuthTag': b'\x00\x00\x00', 'BundleID': 'com.apple.finder',
           'TransferID': {'id': transfer_id}, 'TransferType': {'files': {}}, 'Items': [],
           'ConvertMediaFormats': False, 'Files': [{'FileName': path.name, 'FileSize': path.stat().st_size,
               'FileType': uti, 'FileBomPath': './' + path.name, 'FileIsDirectory': False,
               'ConvertMediaFormats': False}]}
    try:
        from streaming import prepare
        with tempfile.TemporaryFile() as body:
            print('Preparing file on disk...', flush=True)
            size, digest = prepare(path, body)
            encoded_size = body.seek(0, 2)
            body.seek(0)
            trace('prepared', source_bytes=size, archive_bytes=encoded_size, sha256=digest)
            ask['Files'][0]['FileSize'] = size
            phase = 'discovery'
            # Preparing a large archive must not consume the phone's short discovery window.
            deadline = time.monotonic() + 20
            while True:
                conn = Connection(peer, port, address, ifindex, context, trace=trace)
                conn.timeout = min(5, max(0.1, deadline - time.monotonic()))
                try:
                    discover = post(conn, '/Discover', plistlib.dumps({'DeviceSupportFlags': 111611}, fmt=plistlib.FMT_BINARY))
                    info = plistlib.loads(discover)
                    if not isinstance(info, dict):
                        raise ValueError('peer discovery reply is not a dictionary')
                    print(f"Peer discovery name: {clean(info.get('ReceiverComputerName', '(not disclosed)'))}", flush=True)
                    break
                except (ConnectionError, TimeoutError, http.client.RemoteDisconnected):
                    if time.monotonic() >= deadline:
                        raise
                    print('Waiting for the phone to reopen AirDrop. Keep it unlocked with Everyone enabled...', flush=True)
                    time.sleep(min(1, max(0, deadline - time.monotonic())))
                finally:
                    conn.close()  # Retry only discovery, never an approval request or upload.
                    conn = None
            conn = Connection(peer, port, address, ifindex, context, trace=trace)
            phase = 'approval'
            trace(phase)
            print('Requesting approval on the phone...', flush=True)
            post(conn, '/Ask', plistlib.dumps(ask, fmt=plistlib.FMT_BINARY))
            encoded_size = body.seek(0, 2)
            body.seek(0)
            phase = 'upload'
            trace(phase)
            class Upload:
                last = 0.0
                def read(self, count):
                    data = body.read(count)
                    if time.monotonic() - self.last >= 0.25 or not data:
                        self.last = time.monotonic()
                        print(json.dumps({'event': 'send-progress', 'sent': min(size, body.tell() * size // encoded_size), 'total': size, 'wire_read': body.tell(), 'wire_total': encoded_size, 'elapsed_ms': round((time.perf_counter() - started) * 1000, 3)}), flush=True)
                    return data
            post(conn, '/Upload', Upload(), {'Content-Type': 'application/x-dvzip', 'TotalBytes': str(size),
                                        'TransferID': transfer_id}, chunked=True)
            trace('accepted', source_bytes=size, archive_bytes=encoded_size, sha256=digest)
            print(f'Peer accepted upload: {size} bytes, SHA-256 {digest}', flush=True)
            return digest
    except Exception as error:
        trace('failed', operation=phase, error_type=type(error).__name__, errno=getattr(error, 'errno', None), winerror=getattr(error, 'winerror', None))
        raise
    finally:
        if conn is not None: conn.close()



if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('file')
    parser.add_argument('--peer', required=True)
    parser.add_argument('--port', type=int, default=8770)
    parser.add_argument('--address', required=True)
    parser.add_argument('--ifindex', type=int, required=True)
    parser.add_argument('--sid', required=True)
    parser.add_argument('--name', type=display_name, default=NAME)
    parser.add_argument('--model', type=model_name, default=MODEL)
    args = parser.parse_args()
    if not ipaddress.IPv6Address(args.peer).is_link_local or not ipaddress.IPv6Address(args.address).is_link_local:
        parser.error('use link-local AWDL peer and source addresses')
    try:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE  # AirDrop Everyone mode uses a separate Apple trust model.
        cert, key = certificate(Path.home() / '.windows-airdrop' / 'identity', args.sid)
        context.load_cert_chain(cert, key)
        send_file(args.file, args.peer, args.port, args.address, args.ifindex, args.sid, context, args.name, args.model)
    except Exception as error:
        traceback.print_exc()
        print(json.dumps(error_event(error)), flush=True)
        raise SystemExit(1)

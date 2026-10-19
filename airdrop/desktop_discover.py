"""Resolve AirDrop services, confirming candidate peers over HTTPS before listing them."""
import argparse
import concurrent.futures
import ipaddress
import json
from pathlib import Path
import plistlib
import ssl
import threading
import time

from zeroconf import IPVersion, ServiceBrowser, Zeroconf
from send import Connection, post
from service import certificate


def endpoint(address, port):
    address = str(ipaddress.IPv6Address(address.split('%')[0]))
    if not ipaddress.IPv6Address(address).is_link_local or not 1 <= port <= 65535:
        raise ValueError('invalid native AirDrop endpoint')
    return address, port


def probe(address, port, source, ifindex, context):
    conn = Connection(address, port, source, ifindex, context)
    conn.timeout = 5
    try:
        data = post(conn, '/Discover', plistlib.dumps({'DeviceSupportFlags': 111611}, fmt=plistlib.FMT_BINARY))
        info = plistlib.loads(data)
        name = info.get('ReceiverComputerName') if isinstance(info, dict) else None
        if isinstance(name, str) and name.strip():
            return {'event': 'device', 'name': name[:100], 'address': address, 'port': port}
    finally:
        conn.close()


def discover(args):
    endpoint(args.address, 8771)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE  # Everyone-mode discovery; not authenticated Apple identity.
    cert, key = certificate(Path.home() / '.windows-airdrop' / 'identity', args.sid)
    context.load_cert_chain(cert, key)
    seen, inflight, confirmed, last_attempt = set(), set(), set(), {}
    lock = threading.Lock()
    deadline = time.monotonic() + args.seconds
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        def attempt(address, port):
            target = (address, port)
            try:
                if time.monotonic() >= deadline:
                    return
                result = probe(address, port, args.address, args.ifindex, context)
                if result:
                    with lock:
                        confirmed.add(target)
                    print(json.dumps(result, ensure_ascii=True, separators=(',', ':')), flush=True)
            except (OSError, ValueError, plistlib.InvalidFileException):
                pass  # A peer can be present at the radio layer without an open receive endpoint.
            finally:
                with lock:
                    last_attempt[target] = time.monotonic()
                    inflight.discard(target)

        def submit(address, port):
            try:
                address, port = endpoint(address, port)
            except ValueError:
                return
            if address == args.address:
                return
            target = (address, port)
            with lock:
                if target not in seen and len(seen) >= 32:
                    return
                seen.add(target)
                if (target in inflight or target in confirmed or time.monotonic() >= deadline
                        or time.monotonic() - last_attempt.get(target, float('-inf')) < 1):
                    return
                inflight.add(target)
            pool.submit(attempt, address, port)

        class Listener:
            def add_service(self, zc, kind, name):
                info = zc.get_service_info(kind, name, timeout=1500)
                if info:
                    for address in info.parsed_scoped_addresses():
                        submit(address, info.port)
            update_service = add_service
            def remove_service(self, *unused):
                pass

        zc = Zeroconf(interfaces=[args.ifindex], ip_version=IPVersion.V6Only)
        browser = ServiceBrowser(zc, '_airdrop._tcp.local.', Listener())
        def current_candidates():
            path = getattr(args, 'candidates_file', None)
            if not path:
                return []
            try:
                with Path(path).open('r', encoding='utf-8') as source:
                    data = json.loads(source.read(4097))
                return [item for item in data[:32] if isinstance(item, str)] if isinstance(data, list) else []
            except (OSError, ValueError, UnicodeError):
                return []  # The producer may be replacing the list; retry on the next pass.
        try:
            # Explicit fallback probes only peers heard by the radio. A device is never listed
            # until /Discover returns its name; port 8770 is not treated as an SRV result.
            for address in args.candidate[:16]:
                submit(address, 8770)
            next_candidates = 0
            # iPhones listen intermittently; one refused connection must not consume the whole scan.
            while time.monotonic() < deadline:
                time.sleep(min(0.25, max(0, deadline - time.monotonic())))
                if time.monotonic() >= next_candidates:
                    for address in current_candidates():
                        submit(address, 8770)
                    next_candidates = time.monotonic() + 1
                with lock:
                    targets = list(seen)
                for address, port in targets:
                    submit(address, port)
        finally:
            browser.cancel()
            zc.close()
    print('Discovery finished. If your phone is missing, refresh Everyone mode or use Bluetooth recovery.', flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--address', required=True)
    p.add_argument('--ifindex', type=int, required=True)
    p.add_argument('--sid', required=True)
    p.add_argument('--seconds', type=int, default=12, choices=range(1, 31))
    p.add_argument('--candidate', action='append', default=[])
    p.add_argument('--candidates-file', type=Path)
    discover(p.parse_args())

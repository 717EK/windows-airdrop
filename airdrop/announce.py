"""Repeat a standards-shaped AirDrop mDNS answer from the exact AWDL source IP."""
import argparse
import ipaddress
import socket
import struct
import time


def dns_name(name):
    labels = name.rstrip('.').split('.')
    if any(not label or len(label.encode()) > 63 for label in labels):
        raise ValueError('invalid DNS name')
    return b''.join(bytes([len(label.encode())]) + label.encode() for label in labels) + b'\x00'


def answer(sid, address, port=8771):
    service = '_airdrop._tcp.local.'
    instance = f'{sid}.{service}'
    hostname = f'{sid}.local.'
    def rr(name, kind, value, ttl=120):
        return dns_name(name) + struct.pack('!HHIH', kind, 1 if kind == 12 else 0x8001, ttl, len(value)) + value
    return struct.pack('!6H', 0, 0x8400, 0, 4, 0, 0) + b''.join([
        rr(service, 12, dns_name(instance)),
        rr(instance, 33, struct.pack('!HHH', 0, 0, port) + dns_name(hostname)),
        rr(instance, 16, b'\x09flags=136'),
        rr(hostname, 28, ipaddress.IPv6Address(address).packed),
    ])


def announce(address, ifindex, sid, seconds):
    message = answer(sid, address)
    with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_IF, ifindex)
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_HOPS, 255)
        sock.bind((address, 5353, 0, ifindex))
        until = time.monotonic() + seconds
        count = 0
        while time.monotonic() < until:
            sock.sendto(message, ('ff02::fb', 5353, 0, ifindex))
            count += 1
            time.sleep(.25)
        print(f'mDNS UDP submissions={count}; on-air delivery requires peer response')


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--address', required=True)
    p.add_argument('--ifindex', type=int, required=True)
    p.add_argument('--sid', required=True)
    p.add_argument('--seconds', type=int, default=30)
    a = p.parse_args()
    announce(a.address, a.ifindex, a.sid, a.seconds)

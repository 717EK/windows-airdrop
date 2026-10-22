"""Measure outgoing archive preparation/size, not radio or phone throughput."""
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'airdrop'))
from streaming import prepare, receive

with tempfile.TemporaryDirectory() as folder:
    root = Path(folder)
    for label, block in [('text', (b'PulseIt lossless document transfer.\n' * 2000)[:65536]),
                         ('incompressible', os.urandom(65536))]:
        source = root / (label + '.bin')
        with source.open('wb') as out:
            for _ in range(256): out.write(block)
        with source.open('rb') as data:
            expected = hashlib.file_digest(data, 'sha256').hexdigest()
        with tempfile.TemporaryFile() as archive:
            start = time.perf_counter()
            size, digest = prepare(source, archive)
            seconds = time.perf_counter() - start
            encoded = archive.seek(0, 2)
            archive.seek(0)
            final, receipts = receive(archive, root, {source.name: size})
        with (final / source.name).open('rb') as data:
            assert hashlib.file_digest(data, 'sha256').hexdigest() == digest == expected == receipts[0]['sha256']
        print(json.dumps({'case': label, 'source_bytes': size, 'encoded_bytes': encoded,
                          'wire_reduction_percent': (1 - encoded / size) * 100,
                          'preparation_seconds': seconds, 'verified_sha256': digest}))

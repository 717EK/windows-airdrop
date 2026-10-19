"""Local incompressible HTTPS transfer; measures application/disk, not AirDrop radio."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import ssl
import sys
import time

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'airdrop'))
spec = importlib.util.spec_from_file_location('transfer_tests', root / 'tests/test_airdrop.py')
tests = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tests)
from send import send_file

tests.HTTPTests.setUpClass()
try:
    source = Path(tests.HTTPTests.tmp.name) / 'incompressible-32MiB.bin'
    block = os.urandom(65536)
    with source.open('wb') as output:
        for _ in range(512): output.write(block)
    with source.open('rb') as data:
        expected = hashlib.file_digest(data, 'sha256').hexdigest()
    started = time.perf_counter()
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        digest = send_file(source, '::1', tests.HTTPTests.server.server_port, '::1', 0,
                           '001122334455', ssl._create_unverified_context())
    seconds = time.perf_counter() - started
    saved, = tests.HTTPTests.dest.glob('*/' + source.name)
    with saved.open('rb') as data:
        assert hashlib.file_digest(data, 'sha256').hexdigest() == digest == expected
    assert saved.stat().st_size == source.stat().st_size
    print(json.dumps({'scope': 'local TLS including preparation/extraction',
                      'bytes': source.stat().st_size, 'seconds': seconds,
                      'MiB_per_second': source.stat().st_size / 1048576 / seconds,
                      'sha256_matches': True, 'radio_speed_measured': False}))
finally:
    tests.HTTPTests.tearDownClass()

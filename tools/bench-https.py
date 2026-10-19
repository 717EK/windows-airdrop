"""Opt-in loopback metadata benchmark; no Apple device or radio required."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import plistlib
import ssl
import sys
import time

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'airdrop'))
spec = importlib.util.spec_from_file_location('transfer_tests', root / 'tests/test_airdrop.py')
tests = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tests)
from send import Connection, post

tests.HTTPTests.setUpClass()
try:
    request = plistlib.dumps({'DeviceSupportFlags': 111611}, fmt=plistlib.FMT_BINARY)
    for run in range(3):
        connection = Connection('::1', tests.HTTPTests.server.server_port, '::1', 0, ssl._create_unverified_context())
        started = time.perf_counter()
        try:
            with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
                for _ in range(40):
                    response = plistlib.loads(post(connection, '/Discover', request))
                    assert isinstance(response.get('ReceiverComputerName'), str)
        finally:
            connection.close()
        elapsed = time.perf_counter() - started
        print(json.dumps({'run': run + 1, 'requests': 40, 'seconds': elapsed, 'ms_per_request': elapsed * 25}))
finally:
    tests.HTTPTests.tearDownClass()

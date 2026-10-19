import gzip
import hashlib
import io
import os
from pathlib import Path
import stat
import struct
import sys
import tempfile
import tracemalloc
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'airdrop'))
import streaming
from protocol import encode_dvzip, encode_cpio

class StreamingTests(unittest.TestCase):
    def test_prepare_shrinks_text_but_preserves_incompressible_data_and_hashes(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for name, content, compressible in [('notes.txt', b'PulseDrop document line\n' * 50000, True),
                                                ('noise.bin', os.urandom(256 * 1024), False)]:
                path = root / name
                path.write_bytes(content)
                with tempfile.TemporaryFile() as body:
                    size, digest = streaming.prepare(path, body)
                    encoded = body.seek(0, 2)
                    if compressible: self.assertLess(encoded, size // 20)
                    else: self.assertGreater(encoded, size * 0.98)
                    body.seek(0)
                    final, receipts = streaming.receive(body, root, {name: len(content)})
                self.assertEqual((final / name).read_bytes(), content)
                self.assertEqual(receipts[0]['sha256'], hashlib.sha256(content).hexdigest())
                self.assertEqual(digest, receipts[0]['sha256'])

    def test_48_mib_roundtrip_uses_bounded_python_memory(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); source=root/'large.bin'; block=bytes(range(256))*256
            expected=hashlib.sha256()
            with source.open('wb') as out:
                for _ in range(768): out.write(block); expected.update(block)
            tracemalloc.start()
            try:
                with tempfile.TemporaryFile() as raw:
                    size,digest=streaming.prepare(source,raw)
                    final,receipts=streaming.receive(raw,root,{'large.bin':size})
                _,peak=tracemalloc.get_traced_memory()
            finally: tracemalloc.stop()
            self.assertEqual(size,48*1024*1024)
            self.assertEqual(digest,expected.hexdigest())
            self.assertEqual(receipts[0]['sha256'],digest)
            self.assertEqual((final/'large.bin').stat().st_size,size)
            self.assertLess(peak,8*1024*1024)

    def test_gzip_and_compressed_dvzip(self):
        files=[('test.txt',b'hello')]
        for body in (encode_dvzip(files),gzip.compress(encode_cpio(files))):
            with tempfile.TemporaryFile() as decoded:
                streaming.decode(io.BytesIO(body),decoded)
                self.assertEqual(streaming.unpack(decoded)[0]['bytes'],5)

    def test_truncation_and_size_mismatch_leave_no_partial_directory(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            body=encode_dvzip([('test.txt',b'hello')])
            for raw,approved in ((body[:-1],{'test.txt':5}),(body,{'test.txt':4}),(body,{'other.txt':5})):
                with self.assertRaises(ValueError): streaming.receive(io.BytesIO(raw),root,approved)
                self.assertEqual(list(root.iterdir()),[])

    def test_bomb_limit_and_missing_cpio_trailer(self):
        raw=io.BytesIO(gzip.compress(b'X'*65537))
        with patch('streaming.MAX_ARCHIVE_BYTES',65536), tempfile.TemporaryFile() as decoded:
            with self.assertRaises(ValueError): streaming.decode(raw,decoded)
        self.assertFalse(streaming.complete(io.BytesIO(encode_dvzip([('x',b'a')])[:-4])))

    def test_cancelled_extract_cleans_staging(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            def cancel(path): raise OSError('cancelled')
            with self.assertRaises(OSError):
                streaming.receive(io.BytesIO(encode_dvzip([('x',b'a')])),root,{'x':1},cancel)
            self.assertEqual(list(root.iterdir()),[])

    def test_startup_preserves_staging_from_another_session(self):
        import service
        from types import SimpleNamespace
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            staging = root / 'Downloads' / 'Windows AirDrop' / '.incoming-other-session'
            staging.mkdir(parents=True)
            data = staging / 'unfinished.bin'
            data.write_bytes(b'active transfer')
            os.utime(staging, (0, 0))
            # Stop before socket setup; startup must not delete another session's data.
            with patch.object(service.Path, 'home', return_value=root), \
                    patch.object(service, 'certificate', side_effect=RuntimeError('stop before bind')):
                with self.assertRaisesRegex(RuntimeError, 'stop before bind'):
                    service.serve(SimpleNamespace(address='fe80::123', sid='test'))
            self.assertEqual(data.read_bytes(), b'active transfer')

if __name__=='__main__': unittest.main()

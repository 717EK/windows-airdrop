"""Disk-backed DVZip/CPIO transfers with bounded memory and atomic publication."""
import hashlib
import os
from pathlib import Path
import shutil
import stat
import struct
import tempfile
import uuid
import zlib
from protocol import MAX_FILE_BYTES, MAX_ARCHIVE_BYTES, MAX_FILES, safe_name

BLOCK = 64 * 1024

def exact(source, size):
    data = source.read(size)
    if len(data) != size:
        raise ValueError('truncated archive')
    return data

def decode(source, target):
    source.seek(0)
    total = 0
    def write(data):
        nonlocal total
        total += len(data)
        if total > MAX_ARCHIVE_BYTES:
            raise ValueError('archive exceeds size limit')
        target.write(data)
    def inflate_parts(parts, wbits):
        decoder = zlib.decompressobj(wbits)
        for data in parts:
            while data:
                chunk = decoder.decompress(data, BLOCK)
                write(chunk)
                if decoder.unused_data:
                    raise ValueError('trailing compressed data')
                data = decoder.unconsumed_tail
        if not decoder.eof:
            raise ValueError('truncated compressed data')
    prefix = source.read(2)
    source.seek(0)
    if prefix == b'\x1f\x8b':
        inflate_parts(iter(lambda: source.read(BLOCK), b''), 31)
    else:
        while header := source.read(4):
            if len(header) != 4:
                raise ValueError('truncated DVZip header')
            value, = struct.unpack('>I', header)
            size = value & 0x7fffffff
            if not size or size > MAX_ARCHIVE_BYTES:
                raise ValueError('invalid DVZip block length')
            def parts():
                left = size
                while left:
                    chunk = exact(source, min(BLOCK, left))
                    left -= len(chunk)
                    yield chunk
            if value & 0x80000000:
                for chunk in parts(): write(chunk)
            else:
                inflate_parts(parts(), 15)
    target.seek(0)

def unpack(source, directory=None, approved=None, mark=None):
    total = count = 0
    receipts, used = [], set()
    while True:
        count += 1
        if count > MAX_FILES * 4:
            raise ValueError('too many archive members')
        magic = exact(source, 6)
        if magic == b'070707':
            header = magic + exact(source, 70)
            mode, namesize, size = int(header[18:24], 8), int(header[59:65], 8), int(header[65:76], 8)
            aligned = False
        elif magic == b'070701':
            header = magic + exact(source, 104)
            fields = [int(header[6+i*8:14+i*8], 16) for i in range(13)]
            mode, size, namesize = fields[1], fields[6], fields[11]
            aligned = True
        else:
            raise ValueError('missing CPIO header or trailer')
        if not 1 <= namesize <= 4096 or not 0 <= size <= MAX_ARCHIVE_BYTES:
            raise ValueError('invalid archive entry bounds')
        encoded = exact(source, namesize)
        if encoded[-1] != 0 or b'\0' in encoded[:-1]:
            raise ValueError('invalid archive filename')
        name = encoded[:-1].decode('utf-8')
        if aligned: exact(source, (-source.tell()) % 4)
        if name == 'TRAILER!!!':
            if size or not receipts:
                raise ValueError('invalid archive trailer or empty archive')
            if approved is not None and used != {name.casefold() for name in approved}:
                raise ValueError('upload is missing approved files')
            for chunk in iter(lambda: source.read(BLOCK), b''):
                if any(chunk): raise ValueError('trailing archive data')
            return receipts
        regular = stat.S_ISREG(mode)
        if not regular and not stat.S_ISDIR(mode):
            raise ValueError('archive contains a link or special file')
        skip = not regular or name.replace('\\', '/').rsplit('/', 1)[-1].startswith('._')
        output = safe_name(name)
        if not skip:
            if output.casefold() in used:
                raise ValueError('duplicate output name')
            used.add(output.casefold())
            total += size
            if total > MAX_FILE_BYTES or len(receipts) >= MAX_FILES:
                raise ValueError('transfer exceeds file limits')
            if approved is not None:
                if output not in approved: raise ValueError('upload names differ from approved request')
                if size > approved[output]: raise ValueError(f'{output} is larger than the approved size')
        digest = hashlib.sha256()
        dest = (directory / output).open('xb') if directory is not None and not skip else None
        try:
            left = size
            while left:
                chunk = exact(source, min(BLOCK, left))
                left -= len(chunk)
                if not skip: digest.update(chunk)
                if dest: dest.write(chunk)
        finally:
            if dest: dest.close()
        if aligned: exact(source, (-source.tell()) % 4)
        if not skip:
            if mark and directory is not None: mark(directory / output)
            receipts.append({'received': output, 'bytes': size, 'sha256': digest.hexdigest()})

def complete(source, directory=None):
    try:
        with tempfile.TemporaryFile(dir=directory) as decoded:
            decode(source, decoded)
            unpack(decoded)
        return True
    except (ValueError, UnicodeError, OverflowError, zlib.error):
        return False

def receive(source, destination, approved, mark=None):
    transfer = uuid.uuid4().hex
    staging = destination / ('.incoming-' + transfer)
    staging.mkdir()
    try:
        with tempfile.TemporaryFile(dir=destination) as decoded:
            decode(source, decoded)
            receipts = unpack(decoded, staging, approved, mark)
        final = destination / transfer
        staging.rename(final)
        return final, receipts
    finally:
        if staging.exists(): shutil.rmtree(staging)

def prepare(path, target):
    """Use bounded, fast lossless blocks; store already-compressed data unchanged."""
    digest = hashlib.sha256()
    name = ('./' + path.name).encode('utf-8') + b'\0'
    def entry(encoded, size, inode):
        values = [0, inode, stat.S_IFREG | 0o600, 0, 0, 1, 0]
        return (b'070707' + b''.join(f'{v:06o}'.encode() for v in values)
                + f'{0:011o}{len(encoded):06o}{size:011o}'.encode() + encoded)
    def block(data):
        sample = data[:4096]
        compressed = zlib.compress(sample, 1)
        if len(compressed) < len(sample) * 0.98:
            if len(data) > len(sample): compressed = zlib.compress(data, 1)
            if len(compressed) < len(data) * 0.98:
                target.write(struct.pack('>I', len(compressed))); target.write(compressed)
                return
        target.write(struct.pack('>I', len(data) | 0x80000000)); target.write(data)
    with path.open('rb') as source:
        size = os.fstat(source.fileno()).st_size
        if not stat.S_ISREG(os.fstat(source.fileno()).st_mode) or size > MAX_FILE_BYTES:
            raise ValueError('choose a regular file of at most 1 GiB')
        block(entry(name, size, 1))
        left = size
        while left:
            chunk = exact(source, min(BLOCK, left))
            digest.update(chunk); block(chunk); left -= len(chunk)
        if source.read(1) or os.fstat(source.fileno()).st_size != size:
            raise ValueError('file changed while preparing transfer')
    block(entry(b'TRAILER!!!\0', 0, 2))
    target.seek(0)
    return size, digest.hexdigest()

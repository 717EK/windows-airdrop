"""Bounded AirDrop DVZip/CPIO file payloads; no native archive dependency."""
import re
import stat
import struct
import unicodedata
import zlib

MAX_FILE_BYTES = 1024 * 1024 * 1024
MAX_ARCHIVE_BYTES = MAX_FILE_BYTES + 16 * 1024 * 1024
MAX_FILES = 100


def inflate(data, wbits, limit):
    decoder = zlib.decompressobj(wbits)
    result = decoder.decompress(data, limit + 1)
    if len(result) > limit or decoder.unconsumed_tail or not decoder.eof or decoder.unused_data:
        raise ValueError('oversized, truncated or trailing compressed data')
    return result


def decode_dvzip(body):
    if body.startswith(b'\x1f\x8b'):
        return inflate(body, 31, MAX_ARCHIVE_BYTES)
    out = bytearray()
    offset = 0
    while offset < len(body):
        if offset + 4 > len(body):
            raise ValueError('truncated DVZip header')
        value, = struct.unpack_from('>I', body, offset)
        offset += 4
        size = value & 0x7fffffff
        if not size or size > len(body) - offset:
            raise ValueError('invalid DVZip block length')
        block = body[offset:offset + size]
        offset += size
        left = MAX_ARCHIVE_BYTES - len(out)
        decoded = block if value & 0x80000000 else inflate(block, 15, left)
        if len(decoded) > left:
            raise ValueError('archive exceeds size limit')
        out.extend(decoded)
    return bytes(out)


def safe_name(name):
    base = name.replace('\\', '/').rsplit('/', 1)[-1]
    base = ''.join('_' if c in '<>:"/\\|?*' or unicodedata.category(c).startswith('C') else c for c in base)
    base = base[:180].strip(' .')  # truncate first: Windows drops a trailing dot or space
    if not base:
        base = 'file'
    if re.fullmatch(r'(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])', base.split('.')[0], re.I):
        base = '_' + base
    return base


def parse_cpio(data):
    """Read POSIX odc or newc archives. Links and special files are refused."""
    files = []
    offset = total = members = 0
    while True:
        members += 1
        if members > MAX_FILES * 4:
            raise ValueError('too many archive members')
        magic = data[offset:offset + 6]
        if magic == b'070707':
            header = data[offset:offset + 76]
            if len(header) != 76:
                raise ValueError('truncated odc header')
            try:
                mode = int(header[18:24], 8)
                namesize = int(header[59:65], 8)
                size = int(header[65:76], 8)
            except ValueError:
                raise ValueError('invalid odc header') from None
            offset += 76
            aligned = False
        elif magic in (b'070701', b'070702'):
            header = data[offset:offset + 110]
            if len(header) != 110:
                raise ValueError('truncated newc header')
            try:
                fields = [int(header[6+i*8:14+i*8], 16) for i in range(13)]
            except ValueError:
                raise ValueError('invalid newc header') from None
            mode, size, namesize = fields[1], fields[6], fields[11]
            if magic == b'070702':
                raise ValueError('CRC cpio is not supported')
            offset += 110
            aligned = True
        else:
            raise ValueError('missing CPIO header or trailer')
        if not 1 <= namesize <= 4096 or not 0 <= size <= MAX_ARCHIVE_BYTES or offset + namesize > len(data):
            raise ValueError('invalid archive entry bounds')
        name = data[offset:offset + namesize]
        if name[-1] != 0 or b'\x00' in name[:-1]:
            raise ValueError('invalid archive filename')
        name = name[:-1].decode('utf-8', errors='strict')
        offset += namesize
        if aligned:
            offset = (offset + 3) & ~3
        if offset + size > len(data):
            raise ValueError('truncated archive entry')
        payload = data[offset:offset + size]
        offset += size
        if aligned:
            offset = (offset + 3) & ~3
        if name == 'TRAILER!!!':
            if size or any(data[offset:]):
                raise ValueError('invalid archive trailer')
            if not files:
                raise ValueError('archive has no regular files')
            return files
        if stat.S_ISDIR(mode):
            continue
        if not stat.S_ISREG(mode):
            raise ValueError('archive contains a link or special file')
        if name.replace('\\', '/').rsplit('/', 1)[-1].startswith('._'):
            continue
        total += size
        if total > MAX_FILE_BYTES or len(files) >= MAX_FILES:
            raise ValueError('transfer exceeds file limits')
        files.append((safe_name(name), payload))


def encode_cpio(files):
    out = bytearray()
    for inode, (name, payload) in enumerate([*files, ('TRAILER!!!', b'')], 1):
        name = ('./' + name if name != 'TRAILER!!!' else name).encode() + b'\x00'
        values = [0, inode, stat.S_IFREG | 0o600, 0, 0, 1, 0]
        out.extend(b'070707' + b''.join(f'{v:06o}'.encode() for v in values))
        out.extend(f'{0:011o}{len(name):06o}{len(payload):011o}'.encode())
        out.extend(name)
        out.extend(payload)
    return bytes(out)


def encode_dvzip(files):
    data = zlib.compress(encode_cpio(files))
    return len(data).to_bytes(4, 'big') + data

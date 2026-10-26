"""Lists the files inside the game's .nar archives (the directory only; file contents are read with nartool).

NAR v1 layout: "NAR\\0", version 0x01000000, file data..., directory, u32 directory size ^ 0x4076551F, "NAR\\0".
The directory is XOR-encrypted with a fixed 16-byte key (in 0x2000-byte blocks) and bzip2-compressed. Each entry:
u16 name length, name, u32 type (0 stored, 1 empty, 2 compressed), u32 offset, u32 packed size, u32 size,
u32 timestamp, u32 checksum.
"""
import bz2
import struct

_KEY = struct.unpack("<4I", bytes.fromhex("195b7b2c655e79256e4b0721627f0029"))


def _dexor(buf: bytes) -> bytes:
    out = bytearray()
    for block in range(0, len(buf), 0x2000):
        chunk = buf[block:block + 0x2000]
        n = (len(chunk) + 3) // 4
        words = struct.unpack(f"<{n}I", chunk + b"\0" * (n * 4 - len(chunk)))
        out += struct.pack(f"<{n}I", *[w ^ _KEY[i % 4] for i, w in enumerate(words)])
    return bytes(out[:len(buf)])


def read_dir(path):
    """Returns [(name, type, offset, packed_size, size)] for one .nar file. Names look like /cstrike/resource/x.res."""
    with open(path, "rb") as f:
        f.seek(-8, 2)
        size = struct.unpack("<I", f.read(4))[0] ^ 0x4076551F
        f.seek(-8 - size, 2)
        data = bz2.decompress(_dexor(f.read(size)))
    count = struct.unpack_from("<I", data, 16)[0]
    pos, entries = 20, []
    while pos < len(data) and len(entries) < count:
        n = struct.unpack_from("<H", data, pos)[0]
        pos += 2
        name = data[pos:pos + n].decode("latin1")
        pos += n
        typ, off, packed, unpacked, _ts, _crc = struct.unpack_from("<6I", data, pos)
        pos += 24
        entries.append((name, typ, off, packed, unpacked))
    return entries

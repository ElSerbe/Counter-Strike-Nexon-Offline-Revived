"""Decrypts and encrypts the game's .cso tables (encrypted CSV, e.g. maps/zbs_wave_<map>.cso).

hw.dll FUN_02758c80: TEA, 32 rounds, fixed key, on 8-byte blocks; the plain text is CSV padded with zeros.
Usage: csodec.py d <in.cso> <out.csv>    |    csodec.py e <in.csv> <out.cso>
"""
import struct
import sys

M = 0xFFFFFFFF
DELTA = 0x9E3779B9
K = (0xB38520FD, 0x5F30E75F, 0xE2EB67E0, 0xFC67A521)


def decrypt(data: bytes) -> bytes:
    data += b"\0" * (-len(data) % 8)
    out = bytearray()
    for i in range(0, len(data), 8):
        v0, v1 = struct.unpack_from("<2I", data, i)
        s = (DELTA << 5) & M
        for _ in range(32):
            v1 = (v1 - ((((v0 << 4) + K[2]) & M) ^ ((v0 + s) & M) ^ (((v0 >> 5) + K[3]) & M))) & M
            v0 = (v0 - ((((v1 << 4) + K[0]) & M) ^ ((v1 + s) & M) ^ (((v1 >> 5) + K[1]) & M))) & M
            s = (s - DELTA) & M
        out += struct.pack("<2I", v0, v1)
    return bytes(out)


def encrypt(data: bytes) -> bytes:
    data += b"\0" * (-len(data) % 8)
    out = bytearray()
    for i in range(0, len(data), 8):
        v0, v1 = struct.unpack_from("<2I", data, i)
        s = 0
        for _ in range(32):
            s = (s + DELTA) & M
            v0 = (v0 + ((((v1 << 4) + K[0]) & M) ^ ((v1 + s) & M) ^ (((v1 >> 5) + K[1]) & M))) & M
            v1 = (v1 + ((((v0 << 4) + K[2]) & M) ^ ((v0 + s) & M) ^ (((v0 >> 5) + K[3]) & M))) & M
        out += struct.pack("<2I", v0, v1)
    return bytes(out)


if __name__ == "__main__":
    mode, src, dst = sys.argv[1:4]
    data = open(src, "rb").read()
    if mode == "d":
        open(dst, "wb").write(decrypt(data).rstrip(b"\0"))
    else:
        open(dst, "wb").write(encrypt(data))

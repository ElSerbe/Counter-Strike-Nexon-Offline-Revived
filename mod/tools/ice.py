"""ICE block cipher (Matthew Kwan's reference algorithm), used by CSO to encrypt v20/v21 studio model data."""
import struct

_SMOD = [[333, 313, 505, 369], [379, 375, 319, 391], [361, 445, 451, 397], [397, 425, 395, 505]]
_SXOR = [[0x83, 0x85, 0x9b, 0xcd], [0xcc, 0xa7, 0xad, 0x41], [0x4b, 0x2e, 0xd4, 0x33], [0xea, 0xcb, 0x2e, 0x04]]
_PBOX = [0x00000001, 0x00000080, 0x00000400, 0x00002000, 0x00080000, 0x00200000, 0x01000000, 0x40000000,
         0x00000008, 0x00000020, 0x00000100, 0x00004000, 0x00010000, 0x00800000, 0x04000000, 0x20000000,
         0x00000004, 0x00000010, 0x00000200, 0x00008000, 0x00020000, 0x00400000, 0x08000000, 0x10000000,
         0x00000002, 0x00000040, 0x00000800, 0x00001000, 0x00040000, 0x00100000, 0x02000000, 0x80000000]
_KEYROT = [0, 1, 2, 3, 2, 1, 3, 0, 1, 3, 2, 0, 3, 1, 0, 2]


def _gf_mult(a, b, m):
    res = 0
    while b:
        if b & 1:
            res ^= a
        a <<= 1
        b >>= 1
        if a >= 256:
            a ^= m
    return res


def _gf_exp7(b, m):
    if b == 0:
        return 0
    x = _gf_mult(b, b, m)
    x = _gf_mult(b, x, m)
    x = _gf_mult(x, x, m)
    return _gf_mult(b, x, m)


def _perm32(x):
    res, i = 0, 0
    while x:
        if x & 1:
            res |= _PBOX[i]
        i += 1
        x >>= 1
    return res


_SBOX = [[0] * 1024 for _ in range(4)]
for _i in range(1024):
    _col = (_i >> 1) & 0xff
    _row = (_i & 1) | ((_i & 0x200) >> 8)
    for _s, _shift in enumerate((24, 16, 8, 0)):
        _SBOX[_s][_i] = _perm32(_gf_exp7(_col ^ _SXOR[_s][_row], _SMOD[_s][_row]) << _shift)


class IceKey:
    def __init__(self, n, key):
        self.size = max(1, n)
        self.rounds = 8 if n < 1 else n * 16
        self.sched = [[0, 0, 0] for _ in range(self.rounds)]
        for i in range(self.size):
            kb = [0] * 4
            for j in range(4):
                kb[3 - j] = (key[i * 8 + j * 2] << 8) | key[i * 8 + j * 2 + 1]
            self._build(kb, i * 8, _KEYROT[:8])
            self._build(kb, self.rounds - 8 - i * 8, _KEYROT[8:])

    def _build(self, kb, n, keyrot):
        for i in range(8):
            kr = keyrot[i]
            sk = self.sched[n + i]
            sk[0] = sk[1] = sk[2] = 0
            for j in range(15):
                for k in range(4):
                    idx = (kr + k) & 3
                    bit = kb[idx] & 1
                    sk[j % 3] = ((sk[j % 3] << 1) | bit) & 0xFFFFFFFF
                    kb[idx] = (kb[idx] >> 1) | ((bit ^ 1) << 15)

    @staticmethod
    def _f(p, sk):
        tl = ((p >> 16) & 0x3ff) | (((p >> 14) | (p << 18)) & 0xffc00)
        tr = (p & 0x3ff) | ((p << 2) & 0xffc00)
        al = sk[2] & (tl ^ tr)
        ar = al ^ tr
        al ^= tl
        al ^= sk[0]
        ar ^= sk[1]
        return _SBOX[0][al >> 10] | _SBOX[1][al & 0x3ff] | _SBOX[2][ar >> 10] | _SBOX[3][ar & 0x3ff]

    def _crypt(self, block, order):
        l, r = struct.unpack(">II", block)
        for i in order:
            l ^= self._f(r, self.sched[i])
            r ^= self._f(l, self.sched[i + 1 if order.step > 0 else i - 1])
        return struct.pack(">II", r, l)

    def encrypt(self, block):
        return self._crypt(block, range(0, self.rounds, 2))

    def decrypt(self, block):
        return self._crypt(block, range(self.rounds - 1, 0, -2))


def decrypt_region(key, data, start, size, chunk=0x400):
    """CSO's scheme (hw.dll 0x25f41c0): 8-byte blocks in 0x400-byte chunks; a final chunk whose size isn't a
    multiple of 8 is left as it is."""
    end = start + size
    pos = start
    while pos < end:
        n = min(chunk, end - pos)
        if n % 8:
            break
        for b in range(pos, pos + n, 8):
            data[b:b + 8] = key.decrypt(bytes(data[b:b + 8]))
        pos += n

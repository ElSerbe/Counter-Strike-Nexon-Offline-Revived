"""Writes Studio (voxel) map files (.vxl) the game can load, for the offline Studio's sample maps.

Format (hw.dll CVoxelDoc::Load 0x21570e0 / Write 0x2157870):
  "csov" "...." u32 20151001 | u32 uncompressed size | 5-byte LZMA props | raw LZMA stream
The uncompressed text is INI-like (parser 0x21572a0):
  [section]\n  then per line  key=value\n  (text)  or  key:<u32 length><bytes>\n  (binary);
  a repeated key is written as "+" on the following lines.
Sections the loaders check (CVoxelWorld 0x2247990 / Load_C 0x2248360):
  global_data version=20151013 (other keys optional, read by VoxelGlobalData::Read 0x224a970)
  chunk       version=20151002, then "chunk" binary blobs (none = no blocks)
  entity      version=20151003
  water       version=20151008
  script      optional; version=20180802 when present
  treeview    optional; version=20200327
Usage: gen_vxl.py <out.vxl>      (an empty world)
"""
import lzma
import struct
import sys

MAGIC = b"csov" + b"...." + struct.pack("<I", 20151001)


def text(key, value):
    return f"{key}={value}\n".encode()


def binary(key, data):
    return key.encode() + b":" + struct.pack("<I", len(data)) + data + b"\n"


def build(sections):
    out = b""
    for name, lines in sections:
        out += b"[" + name.encode() + b"]\n" + b"".join(lines)
    return out


def pack(body):
    alone = lzma.compress(body, format=lzma.FORMAT_ALONE, filters=[{"id": lzma.FILTER_LZMA1, "dict_size": 1 << 16}])
    props, data = alone[:5], alone[13:]  # .lzma = props(5) + size(8) + stream
    return MAGIC + struct.pack("<I", len(body)) + props + data


# world settings as the game's editor writes them for a new map (VoxelGlobalData)
def global_data():
    h = bytes.fromhex
    return [
        text("version", 20151013), text("skyname", "vostok"),
        binary("fog_color", h("00005d4300005f4300006643")), binary("day_light", h("ffffff")),
        binary("inv_light", h("0080ff")), binary("gamemode", struct.pack("<I", 39)),
        binary("playtime", struct.pack("<I", 20)), binary("respawn", b"\x01"), binary("weather", b"\x00"),
        binary("maxplayernum", struct.pack("<I", 10)), binary("fog_density.v2", h("0000803e")),
        binary("vis_dist", h("0000c845")), binary("reset_trigger", b"\x00"), binary("friendly_fire", b"\x01"),
        binary("player_collision", b"\x01"), binary("enable_tps", b"\x00"),
        binary("minplayernum", struct.pack("<I", 1)), binary("end_rule", struct.pack("<I", 0)),
        binary("init_back", b"\x00"), binary("human_destroy", b"\x00"), binary("zombi_destroy", b"\x00"),
        binary("zombi_seethru", b"\x00"), binary("fog_on", b"\x01"), binary("respawntime", struct.pack("<I", 0)),
        binary("defeatbywipe", b"\x00"), binary("enemyfire", b"\x01"), binary("seekerPenalty", struct.pack("<I", 5)),
        binary("hiderproplimit", struct.pack("<I", 0)), binary("maxplaydate", struct.pack("<I", 30)),
        binary("maxlevel", struct.pack("<I", 30)), binary("maxsheltertier", struct.pack("<I", 4)),
        binary("shelterlastdaywin", b"\x00"), binary("shelterbuildrange", struct.pack("<I", 20)),
        binary("shelterMonsterInfo", bytes(3000)), binary("remove_dim_light", b"\x00"),
        binary("gdmknifekillbonus", b"\x00"), binary("gdmupperlvkillbonus", b"\x00"), binary("use_level", b"\x00"),
        binary("level_setting", h("1e000a000a00050a0a0a0100")), binary("use_diff", b"\x00"),
        binary("diff_setting", h("0a00640096005e015802")),
    ]


# The world is 24 x 24 x 12 chunks (z is up) of 16^3 blocks. A chunk blob: u16 0, u16 size, u8 x, y, z,
# u8 encoding (7 = raw: 4096 x u16, index x*256 + y*16 + z, block id in the low 12 bits; 0 = air).
CHUNKS_X, CHUNKS_Y, CHUNKS_Z = 24, 24, 12


def chunk(cx, cy, cz, voxels):
    data = struct.pack("<4096H", *voxels)
    return struct.pack("<HHBBBB", 0, 8 + len(data), cx, cy, cz, 7) + data


def chunk_lines(blobs):
    lines = [text("version", 20151002)]
    for i, b in enumerate(blobs):
        lines.append(binary("chunk" if i == 0 else "+", b))
    return lines


def world(blobs):
    return build([
        ("global_data", global_data()),
        ("chunk", chunk_lines(blobs)),
        ("entity", [text("version", 20151003)]),
        ("water", [text("version", 20151008), text("frontier_count", 0)]),
    ])


def empty_world():
    return world([])


def flat_world(block=1, chunks=CHUNKS_X):
    """A one-block floor over the middle chunks x chunks area, at the bottom of the world."""
    layer = [0] * 4096
    for x in range(16):
        for y in range(16):
            layer[x * 256 + y * 16 + 0] = block
    lo = (CHUNKS_X - chunks) // 2
    return world([chunk(cx, cy, 0, layer) for cx in range(lo, lo + chunks) for cy in range(lo, lo + chunks)])


def unpack(raw):
    assert raw[:12] == MAGIC, "not a csov file"
    size = struct.unpack("<I", raw[12:16])[0]
    alone = raw[16:21] + b"\xff" * 8 + raw[21:]  # size unknown: our streams end with an end marker
    return lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(alone)[:size]


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--dump":
        sys.stdout.buffer.write(unpack(open(sys.argv[2], "rb").read()))
        sys.exit(0)
    body = flat_world() if len(sys.argv) < 3 or sys.argv[2] != "--empty" else empty_world()
    raw = pack(body)
    assert unpack(raw) == body
    open(sys.argv[1], "wb").write(raw)
    print(f"{sys.argv[1]}: {len(raw)} bytes ({len(body)} uncompressed)")

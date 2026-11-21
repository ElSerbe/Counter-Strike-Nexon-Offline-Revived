"""Lists the weapon properties (WeaponProp) each weapon's prop class registers in mp.dll: name and field offset.

The game takes these values only from the server's WeaponProp metadata ({"weaponProp": [{"ingameID": n,
"weaponProp": {"Name": value, ...}}]}, hw.dll 0x25b3d00); the defaults mp.dll registers are empty, so a missing value
reads as nothing ("IWeaponProp::Get Size of mVal : 0" in the console) and the weapon misbehaves.

Each prop class constructor (e.g. LEAPSTRIKEGUN_prop's) builds every name as a std::string on the stack (16 bytes
copied from .rdata with movups plus immediates), then calls the registration 0x10644460 with the field
(lea eax, [edi + offset]) and the name (lea edx, [ebp - x]). This walks those constructors with capstone and a tiny
tracker of the stack bytes; the class is named from the RTTI of the vtable the constructor stores.

Usage: weaponprop_keys.py <mp.dll> <out.json>
"""
import json
import re
import struct
import sys

import capstone
import pefile

REGISTER = 0x10644460
MAKE_STRING = 0x10637120


def disasm_all(md, img, base, start, end):
    """Linear disassembly that steps over bytes capstone can't decode (data in the code) instead of stopping."""
    at = start
    while at < end:
        last = at
        for ins in md.disasm(img[at - base:end - base], at):
            yield ins
            last = ins.address + ins.size
        at = last + 1 if last == at or last < end else last


def main(path, out):
    pe = pefile.PE(path, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    text = [s for s in pe.sections if s.Name.startswith(b".text")][0]
    t0 = base + text.VirtualAddress
    tb = img[text.VirtualAddress:text.VirtualAddress + text.Misc_VirtualSize]

    def u32(va):
        return struct.unpack_from("<I", img, va - base)[0]

    def readable(va, n=4):
        return base <= va and va + n <= base + len(img)

    def rtti_name(vtable):
        if not readable(vtable - 4):
            return None
        col = u32(vtable - 4)
        if not readable(col + 16):
            return None
        td = u32(col + 12)
        if not readable(td + 8):
            return None
        raw = img[td + 8 - base:td + 8 - base + 80].split(b"\0")[0]
        m = re.match(rb"\.\?AV([A-Za-z0-9_]+_prop)@@", raw)
        return m.group(1).decode() if m else None

    calls = []
    for i in range(len(tb) - 5):
        if tb[i] == 0xE8 and t0 + i + 5 + struct.unpack_from("<i", tb, i + 1)[0] == REGISTER:
            calls.append(t0 + i)

    starts = [t0 + m.start() + 1 for m in re.finditer(rb"\xcc\x55\x8b", tb)]

    def fstart(va):
        import bisect
        i = bisect.bisect_right(starts, va) - 1
        return starts[i] if i >= 0 else None

    last_of = {}  # function start -> its last registration call
    for c in calls:
        f = fstart(c)
        if f:
            last_of[f] = max(last_of.get(f, 0), c)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    result = {}
    for f in sorted(last_of):
        last_call = last_of[f]
        stack = {}  # ebp offset -> byte
        xmm = {}
        regs = {}  # reg -> ("ebp", off) | ("edi", off) | ("imm", v)
        strings = {}  # std::string object's ebp offset -> text
        pushes = []
        names = None
        default = None
        props = []
        for ins in disasm_all(md, img, base, f, last_call + 5):
            ops = ins.operands
            m, a = ins.mnemonic, ins.address
            if m in ("movups", "movaps", "movdqu", "movdqa") and len(ops) == 2:
                d, s = ops
                if d.type == capstone.x86.X86_OP_REG and s.type == capstone.x86.X86_OP_MEM:
                    if s.mem.base == 0 and s.mem.index == 0 and readable(s.mem.disp, 16):
                        xmm[d.reg] = img[s.mem.disp - base:s.mem.disp - base + 16]
                elif d.type == capstone.x86.X86_OP_MEM and s.type == capstone.x86.X86_OP_REG:
                    if ins.reg_name(d.mem.base) == "ebp" and s.reg in xmm:
                        for k in range(16):
                            stack[d.mem.disp + k] = xmm[s.reg][k]
            elif m == "xorps" and ops[0].reg == ops[1].reg:
                xmm[ops[0].reg] = b"\0" * 16
            elif m == "mov" and len(ops) == 2 and ops[0].type == capstone.x86.X86_OP_MEM and \
                    ops[1].type == capstone.x86.X86_OP_IMM and ins.reg_name(ops[0].mem.base) == "ebp":
                size = ops[0].size
                v = ops[1].imm & ((1 << (8 * size)) - 1)
                for k in range(size):
                    stack[ops[0].mem.disp + k] = (v >> (8 * k)) & 0xFF
            elif m == "lea" and ops[1].type == capstone.x86.X86_OP_MEM:
                b = ins.reg_name(ops[1].mem.base) if ops[1].mem.base else None
                regs[ins.reg_name(ops[0].reg)] = (b, ops[1].mem.disp) if b in ("ebp", "edi", "esi") else None
            elif m == "sub" and ops[0].type == capstone.x86.X86_OP_REG and ops[1].type == capstone.x86.X86_OP_REG:
                x, y = regs.get(ins.reg_name(ops[0].reg)), regs.get(ins.reg_name(ops[1].reg))
                regs[ins.reg_name(ops[0].reg)] = ("imm", x[1] - y[1]) if x and y and x[0] == y[0] == "ebp" else None
            elif m == "mov" and len(ops) == 2 and ops[0].type == capstone.x86.X86_OP_REG and                     ops[1].type == capstone.x86.X86_OP_REG:
                regs[ins.reg_name(ops[0].reg)] = regs.get(ins.reg_name(ops[1].reg))
            elif m == "push":
                o = ops[0]
                pushes.append(regs.get(ins.reg_name(o.reg)) if o.type == capstone.x86.X86_OP_REG else None)
                pushes = pushes[-6:]
            elif m == "call" and ops[0].type == capstone.x86.X86_OP_IMM:
                tgt = ops[0].imm
                if tgt == MAKE_STRING and len(pushes) >= 2:
                    buf, ln = pushes[-1], pushes[-2]
                    this = regs.get("ecx")
                    if buf and ln and buf[0] == "ebp" and ln[0] == "imm" and this and this[0] == "ebp":
                        strings[this[1]] = bytes(stack.get(buf[1] + k, 0) for k in range(ln[1])).decode("latin1")
                elif tgt == REGISTER:
                    name = regs.get("edx")
                    field = pushes[-1] if pushes else None
                    text_ = strings.get(name[1]) if name and name[0] == "ebp" else None
                    if text_ is None and name and name[0] == "ebp":
                        # a short name (under 16 bytes) is written straight into the std::string (small-string
                        # buffer at +0, size at +0x10), without the constructor call
                        size = sum(stack.get(name[1] + 0x10 + k, 0) << (8 * k) for k in range(4))
                        if 0 < size < 16:
                            text_ = bytes(stack.get(name[1] + k, 0) for k in range(size)).decode("latin1")
                    if text_ and field and field[0] in ("edi", "esi"):
                        props.append({"name": text_, "offset": field[1], "default": round(default, 4)})
                    default = None
                pushes = []
            elif m == "mov" and len(ops) == 2 and ops[0].type == capstone.x86.X86_OP_MEM and \
                    ops[1].type == capstone.x86.X86_OP_IMM and names is None:
                names = rtti_name(ops[1].imm)
            if m == "mov" and len(ops) == 2 and ops[0].type == capstone.x86.X86_OP_MEM and                     ops[1].type == capstone.x86.X86_OP_IMM and ins.reg_name(ops[0].mem.base) == "esp" and                     ops[0].mem.disp == 0:
                default = struct.unpack("<f", struct.pack("<I", ops[1].imm & 0xFFFFFFFF))[0]
            if m == "mov" and len(ops) == 2 and ops[1].type == capstone.x86.X86_OP_IMM and names is None:
                names = rtti_name(ops[1].imm)
        if props:
            key = names or hex(f)
            result.setdefault(key, {"ctor": hex(f), "props": []})["props"] += props
    json.dump(result, open(out, "w"), indent=1)
    print(len(result), "prop classes,", sum(len(v["props"]) for v in result.values()), "properties")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])

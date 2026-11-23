#!/usr/bin/env python3
"""Minimal PS2 VU micro-mode disassembler: vudis.py <code.bin> [start] [end]."""
import struct
import sys

BC = "xyzw"


def dest(i):
    d = (i >> 21) & 0xF
    return "".join(c for c, b in zip("xyzw", (8, 4, 2, 1)) if d & b)


def upper(i):
    ft, fs, fd, op = (i >> 16) & 31, (i >> 11) & 31, (i >> 6) & 31, i & 63
    flags = "".join(f for f, b in (("I", 31), ("E", 30), ("M", 29), ("D", 28), ("T", 27)) if i >> b & 1)
    d = dest(i)
    names = {
        0x1C: "mulq", 0x1D: "maxi", 0x1E: "muli", 0x1F: "minii", 0x20: "addq", 0x21: "maddq",
        0x22: "addi", 0x23: "maddi", 0x24: "subq", 0x25: "msubq", 0x26: "subi", 0x27: "msubi",
        0x28: "add", 0x29: "madd", 0x2A: "mul", 0x2B: "max", 0x2C: "sub", 0x2D: "msub",
        0x2E: "opmsub", 0x2F: "mini"}
    bcn = ["add", "sub", "madd", "msub", "max", "mini", "mul"]
    if op < 0x1C:
        s = f"{bcn[op >> 2]}{BC[op & 3]}.{d} vf{fd}, vf{fs}, vf{ft}{BC[op & 3]}"
    elif op < 0x3C:
        n = names.get(op, f"?u{op:02x}")
        if op in (0x1C, 0x20, 0x21, 0x24, 0x25):
            s = f"{n}.{d} vf{fd}, vf{fs}, Q"
        elif op in (0x1D, 0x1E, 0x1F, 0x22, 0x23, 0x26, 0x27):
            s = f"{n}.{d} vf{fd}, vf{fs}, I"
        else:
            s = f"{n}.{d} vf{fd}, vf{fs}, vf{ft}"
    else:
        x = ((i >> 6) & 31) << 2 | (i & 3)
        bca = ["adda", "suba", "madda", "msuba"]
        if x < 0x10:
            s = f"{bca[x >> 2]}{BC[x & 3]}.{d} ACC, vf{fs}, vf{ft}{BC[x & 3]}"
        elif x < 0x14:
            s = f"itof{[0, 4, 12, 15][x & 3]}.{d} vf{ft}, vf{fs}"
        elif x < 0x18:
            s = f"ftoi{[0, 4, 12, 15][x & 3]}.{d} vf{ft}, vf{fs}"
        elif x < 0x1C:
            s = f"mula{BC[x & 3]}.{d} ACC, vf{fs}, vf{ft}{BC[x & 3]}"
        else:
            n = {0x1C: "mulaq", 0x1D: "abs", 0x1E: "mulai", 0x1F: "clip", 0x20: "addaq", 0x21: "maddaq",
                 0x22: "addai", 0x23: "maddai", 0x24: "subaq", 0x25: "msubaq", 0x26: "subai", 0x27: "msubai",
                 0x28: "adda", 0x29: "madda", 0x2A: "mula", 0x2C: "suba", 0x2D: "msuba", 0x2E: "opmula",
                 0x2F: "nop"}.get(x, f"?ux{x:02x}")
            if n == "nop":
                s = "nop"
            elif n == "abs":
                s = f"abs.{d} vf{ft}, vf{fs}"
            elif n == "clip":
                s = f"clipw.xyz vf{fs}, vf{ft}w"
            elif n.endswith("q"):
                s = f"{n}.{d} ACC, vf{fs}, Q"
            elif n.endswith("i"):
                s = f"{n}.{d} ACC, vf{fs}, I"
            else:
                s = f"{n}.{d} ACC, vf{fs}, vf{ft}"
    return (f"[{flags}] " if flags else "") + s


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def lower(i, pc, ibit):
    if ibit:
        return f"loi {struct.unpack('<f', struct.pack('<I', i))[0]!r}"
    op = (i >> 25) & 0x7F
    ft, fs, fd = (i >> 16) & 31, (i >> 11) & 31, (i >> 6) & 31
    d = dest(i)
    imm11 = sx(i & 0x7FF, 11)
    tgt = (pc + 8 + imm11 * 8) & 0x3FFF
    if op == 0x00:
        return f"lq.{d} vf{ft}, {imm11}(vi{fs})"
    if op == 0x01:
        return f"sq.{d} vf{fs}, {imm11}(vi{ft})"
    if op == 0x04:
        return f"ilw.{d} vi{ft}, {imm11}(vi{fs})"
    if op == 0x05:
        return f"isw.{d} vi{ft}, {imm11}(vi{fs})"
    if op in (0x08, 0x09):
        imm15 = ((i >> 10) & 0x7800) | (i & 0x7FF)
        return f"{'iaddiu' if op == 8 else 'isubiu'} vi{ft}, vi{fs}, {imm15}"
    fl = {0x10: "fceq", 0x11: "fcset", 0x12: "fcand", 0x13: "fcor", 0x14: "fseq", 0x15: "fsset",
          0x16: "fsand", 0x17: "fsor", 0x18: "fmeq", 0x1A: "fmand", 0x1B: "fmor", 0x1C: "fcget"}
    if op in fl:
        if op in (0x10, 0x11, 0x12, 0x13):
            return f"{fl[op]} vi1, 0x{i & 0xFFFFFF:06x}"
        if op in (0x14, 0x15, 0x16, 0x17):
            return f"{fl[op]} vi{ft}, 0x{((i >> 10) & 0x800) | (i & 0x7FF):03x}"
        return f"{fl[op]} vi{ft}, vi{fs}"
    if op == 0x20:
        return f"b {tgt:04x}"
    if op == 0x21:
        return f"bal vi{ft}, {tgt:04x}"
    if op == 0x24:
        return f"jr vi{fs}"
    if op == 0x25:
        return f"jalr vi{ft}, vi{fs}"
    br = {0x28: "ibeq", 0x29: "ibne", 0x2C: "ibltz", 0x2D: "ibgtz", 0x2E: "iblez", 0x2F: "ibgez"}
    if op in br:
        if op in (0x28, 0x29):
            return f"{br[op]} vi{ft}, vi{fs}, {tgt:04x}"
        return f"{br[op]} vi{fs}, {tgt:04x}"
    if op == 0x40:
        lo = i & 63
        if lo == 0x30:
            return f"iadd vi{fd}, vi{fs}, vi{ft}"
        if lo == 0x31:
            return f"isub vi{fd}, vi{fs}, vi{ft}"
        if lo == 0x32:
            return f"iaddi vi{ft}, vi{fs}, {sx(fd, 5)}"
        if lo == 0x34:
            return f"iand vi{fd}, vi{fs}, vi{ft}"
        if lo == 0x35:
            return f"ior vi{fd}, vi{fs}, vi{ft}"
        if lo >= 0x3C:
            x = ((i >> 6) & 31) << 2 | (i & 3)
            fsf, ftf = BC[(i >> 21) & 3], BC[(i >> 23) & 3]
            m = {
                0x30: f"move.{d} vf{ft}, vf{fs}", 0x31: f"mr32.{d} vf{ft}, vf{fs}",
                0x34: f"lqi.{d} vf{ft}, (vi{fs}++)", 0x35: f"sqi.{d} vf{fs}, (vi{ft}++)",
                0x36: f"lqd.{d} vf{ft}, (--vi{fs})", 0x37: f"sqd.{d} vf{fs}, (--vi{ft})",
                0x38: f"div Q, vf{fs}{fsf}, vf{ft}{ftf}", 0x39: f"sqrt Q, vf{ft}{ftf}",
                0x3A: f"rsqrt Q, vf{fs}{fsf}, vf{ft}{ftf}", 0x3B: "waitq",
                0x3C: f"mtir vi{ft}, vf{fs}{fsf}", 0x3D: f"mfir.{d} vf{ft}, vi{fs}",
                0x3E: f"ilwr.{d} vi{ft}, (vi{fs})", 0x3F: f"iswr.{d} vi{ft}, (vi{fs})",
                0x40: f"rnext.{d} vf{ft}, R", 0x41: f"rget.{d} vf{ft}, R", 0x42: f"rinit R, vf{fs}{fsf}",
                0x43: f"rxor R, vf{fs}{fsf}", 0x64: f"mfp.{d} vf{ft}, P", 0x68: f"xtop vi{ft}",
                0x69: f"xitop vi{ft}", 0x6C: f"xgkick vi{fs}", 0x70: f"esadd P, vf{fs}",
                0x71: f"ersadd P, vf{fs}", 0x72: f"eleng P, vf{fs}", 0x73: f"erleng P, vf{fs}",
                0x74: f"eatanxy P, vf{fs}", 0x75: f"eatanxz P, vf{fs}", 0x76: f"esum P, vf{fs}",
                0x78: f"esqrt P, vf{fs}{fsf}", 0x79: f"esin P, vf{fs}{fsf}", 0x7A: f"ercpr P, vf{fs}{fsf}",
                0x7B: "waitp", 0x7C: f"eatan P, vf{fs}{fsf}", 0x7D: f"eexp P, vf{fs}{fsf}"}
            if x == 0x30 and i == 0x8000033C:
                return "nop"
            return m.get(x, f"?lx{x:02x}")
    return f"?l{op:02x} {i:08x}"


def main():
    code = open(sys.argv[1], "rb").read()
    start = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    end = int(sys.argv[3], 16) if len(sys.argv) > 3 else len(code)
    for pc in range(start, min(end, len(code)), 8):
        lo, up = struct.unpack_from("<II", code, pc)
        ibit = (up >> 31) & 1
        print(f"{pc:04x}: {up:08x} {lo:08x}  {upper(up):<42} {lower(lo, pc, ibit)}")


if __name__ == "__main__":
    main()

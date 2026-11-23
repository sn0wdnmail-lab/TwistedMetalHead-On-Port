import sys, struct
from capstone import Cs, CS_ARCH_MIPS, CS_MODE_MIPS32, CS_MODE_LITTLE_ENDIAN
data = open(sys.argv[1], 'rb').read()
# ELF32 section headers
e_shoff, = struct.unpack_from('<I', data, 0x20)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', data, 0x2E)
secs = []
for i in range(e_shnum):
    name, typ, flags, addr, off, size = struct.unpack_from('<IIIIII', data, e_shoff + i * e_shentsize)
    secs.append((name, typ, flags, addr, off, size))
shstr = secs[e_shstrndx]
def sname(n):
    o = shstr[4] + n
    return data[o:data.index(b'\0', o)].decode()
mode = sys.argv[2] if len(sys.argv) > 2 else 'secs'
if mode == 'secs':
    for s in secs:
        print(sname(s[0]), hex(s[3]), hex(s[4]), hex(s[5]))
    sys.exit()
text = [s for s in secs if sname(s[0]) == '.text'][0]
md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 + CS_MODE_LITTLE_ENDIAN)
code = data[text[4]:text[4] + text[5]]
if mode == 'find':
    # find strings and references (lui/addiu pairs) to them
    target = sys.argv[3].encode()
    for s in secs:
        blob = data[s[4]:s[4]+s[5]]
        i = blob.find(target)
        if i >= 0 and s[1] != 8:
            print('string in', sname(s[0]), 'addr', hex(s[3] + i))
    sys.exit()
start = int(sys.argv[3], 16); count = int(sys.argv[4]) if len(sys.argv) > 4 else 60
for ins in md.disasm(code[start - text[3]:], start):
    print('%08x: %s %s' % (ins.address, ins.mnemonic, ins.op_str))
    count -= 1
    if count == 0: break

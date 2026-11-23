#!/usr/bin/env python3
"""cdmap.py <iso> <log>: print the files touched by [cd] read lines, in order (collapsed)."""
import re
import struct
import sys

iso = open(sys.argv[1], 'rb')


def rd(lba, n=2048):
    iso.seek(lba * 2048)
    return iso.read(n)


def walk(lba, size, path, out):
    data = rd(lba, size)
    i = 0
    while i < len(data):
        l = data[i]
        if l == 0:
            i = (i // 2048 + 1) * 2048
            continue
        rec = data[i:i + l]
        elba, = struct.unpack_from('<I', rec, 2)
        esz, = struct.unpack_from('<I', rec, 10)
        fl, nl = rec[25], rec[32]
        name = rec[33:33 + nl]
        if name not in (b'\x00', b'\x01'):
            n = name.decode(errors='replace').split(';')[0]
            if fl & 2:
                walk(elba, esz, path + n + '/', out)
            else:
                out.append((elba, esz, path + n))
        i += l


pvd = rd(16)
lba, = struct.unpack_from('<I', pvd, 156 + 2)
sz, = struct.unpack_from('<I', pvd, 156 + 10)
files = []
walk(lba, sz, '/', files)


def name_of(l):
    for s, z, n in files:
        if s <= l < s + max(1, (z + 2047) // 2048):
            return n
    return f'<lsn {l}>'


last, count = None, 0
text = open(sys.argv[2], 'rb').read().decode('latin1')
for m in re.finditer(r'\[cd\] read lsn=(\d+) sectors=(\d+)', text):
    n = name_of(int(m.group(1)))
    if n == last:
        count += 1
        continue
    if last:
        print(f'{count:6d} {last}')
    last, count = n, 1
if last:
    print(f'{count:6d} {last}')

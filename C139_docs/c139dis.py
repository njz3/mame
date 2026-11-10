"""c139dis.py <game> <region> <start hex> <end hex> : disassemble a range of a dumped CPU region (68000/68020).
Example: python -I c139dis.py ridgera2 maincpu 1bf38 1bf72"""
import os, sys
import capstone
here = os.path.dirname(os.path.abspath(__file__))
game, region, a, b = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4], 16)
gdir = os.path.join(here, 'dumps', game)
info = {}
cpus = {}
for line in open(os.path.join(gdir, 'regions.txt')):
    p = line.split()
    if p[0] == 'CPU':
        cpus[p[1].lstrip(':').replace(':', '_')] = p[2]
    else:
        info[p[0].lstrip(':').replace(':', '_')] = int(p[2])
data = open(os.path.join(gdir, region + '.bin'), 'rb').read()
w = info.get(region, 2)
if w == 4:
    data = b''.join(data[i:i + 4][::-1] for i in range(0, len(data) - 3, 4))
elif w == 2:
    d = bytearray(data); d[0::2], d[1::2] = data[1::2], data[0::2]; data = bytes(d)
mode = capstone.CS_MODE_M68K_000 if cpus.get(region) in ('m68000', 'm68010') else capstone.CS_MODE_M68K_020
md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN | mode)
md.skipdata = True
for ins in md.disasm(data[a:b], a):
    print('%06x: %-20s %-8s %s' % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str))

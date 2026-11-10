"""c139scan.py <dumps dir> <game> [<game>...] : find and disassemble the code that drives the Namco C139.

For each 68000/68020 CPU region of a game (dumped by agent.lua 'dumpall'), the region is put back in big-endian
order, every absolute reference to the C139 registers or RAM is located, and the routines around those
references (from the previous rts/rte to the next one, extended to whole routines) are disassembled into
<dumps>/<game>/c139_<region>.asm, with the referencing lines marked '>>'.
"""
import os, re, struct, sys
import capstone

# C139 windows per driver: (registers base, ram base); the S22 ones are 32-bit addresses
WINDOWS = {
    'namcos2': (0x4a0000, 0x480000),
    'namcos21': (0xb80000, 0xb00000),
    'namcos22': (0x20020000, 0x20010000),
    'namcos22s': (0x420000, 0x410000),
}
REGNAMES = ['STATUS', 'MODE', 'CONTROL', 'START', 'RXSIZE', 'TXSIZE', 'RXOFFSET', 'TXOFFSET']


def load_regions(gdir):
    regs = {}
    cpus = {}
    for line in open(os.path.join(gdir, 'regions.txt')):
        p = line.split()
        if p[0] == 'CPU':
            cpus[p[1]] = p[2]
        else:
            regs[p[0]] = (int(p[1], 16), int(p[2]), p[3])
    return regs, cpus


def to_be(data, width):
    if width == 4:
        return b''.join(data[i:i + 4][::-1] for i in range(0, len(data) - 3, 4))
    if width == 2:
        b = bytearray(data)
        b[0::2], b[1::2] = data[1::2], data[0::2]
        return bytes(b)
    return data


def routine_bounds(md, be, off, lo=0x100, hi=0x200):
    """disassemble from a bit before 'off' and cut at the rts/rte around it"""
    start = max(0, off - lo)
    end = min(len(be), off + hi)
    ins = list(md.disasm(be[start:end], start))
    return ins


DRIVER_WINDOWS = {
    'namcos2.cpp': ['namcos2'],
    'namcos21.cpp': ['namcos21'], 'namcos21_c67.cpp': ['namcos21'], 'namcos21_de.cpp': ['namcos21'],
    'namcos22.cpp': ['namcos22', 'namcos22s'],
}


def game_driver(game):
    tsv = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'c139_machines.tsv')
    for line in open(tsv, encoding='utf-8'):
        p = line.rstrip('\n').split('\t')
        if p[1] == game:
            return p[0].split('/')[-1]
    return None


def scan(gdir, game, out):
    regs, cpus = load_regions(gdir)
    windows = [WINDOWS[w] for w in DRIVER_WINDOWS.get(game_driver(game), list(WINDOWS))]
    report = []
    for tag, (size, width, endian) in sorted(regs.items()):
        name = tag.lstrip(':').replace(':', '_')
        cpu = cpus.get(tag)
        if cpu not in ('m68000', 'm68020', 'm68ec020', 'm68010'):
            continue
        be = to_be(open(os.path.join(gdir, name + '.bin'), 'rb').read(), width)
        mode = capstone.CS_MODE_M68K_000 if cpu in ('m68000', 'm68010') else capstone.CS_MODE_M68K_020
        md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN | mode)
        md.skipdata = True
        hits = []
        for rbase, mbase in windows:
            for base, kind, length in ((rbase, 'REG', 0x10), (mbase, 'RAM', 0x4000)):
                for m in re.finditer(re.escape(struct.pack('>I', base)[:2]), be):
                    o = m.start()
                    if o % 2 or o + 4 > len(be):
                        continue
                    v = struct.unpack('>I', be[o:o + 4])[0]
                    if base <= v < base + length:
                        hits.append((o, v, kind, base))
        if not hits:
            continue
        # keep the hits that disassemble as part of an instruction
        good = []
        for o, v, kind, base in hits:
            for back in (2, 4, 6):
                s = o - back
                if s < 0:
                    continue
                ins = next(md.disasm(be[s:s + 14], s), None)
                if ins and s + ins.size > o and ('$%x' % v) in ins.op_str.lower():
                    good.append((s, v, kind, base, ins.mnemonic, ins.op_str))
                    break
        if not good:
            continue
        good.sort()
        # group the hits into ranges and disassemble each range with some context
        ranges = []
        for g in good:
            if ranges and g[0] - ranges[-1][1] < 0x180:
                ranges[-1][1] = g[0]
                ranges[-1][2].append(g)
            else:
                ranges.append([g[0], g[0], [g]])
        path = os.path.join(gdir, 'c139_%s.asm' % name)
        marks = {g[0] for g in good}
        with open(path, 'w') as f:
            f.write('; %s %s (%s) - C139 accesses\n' % (game, tag, cpu))
            for a, b, gl in ranges:
                f.write('\n; ---- range %06x-%06x\n' % (a, b))
                for ins in md.disasm(be[max(0, a - 0x60):min(len(be), b + 0x80)], max(0, a - 0x60)):
                    m = '>>' if ins.address in marks else '  '
                    note = ''
                    for g in gl:
                        if g[0] == ins.address:
                            off = g[1] - g[3]
                            note = '   ; C139 %s %s' % (g[2], (REGNAMES[off // 2] if g[2] == 'REG' and off // 2 < 8 else '+%x' % off))
                    f.write('%s %06x: %-8s %s%s\n' % (m, ins.address, ins.mnemonic, ins.op_str, note))
        regs_used = sorted({REGNAMES[(g[1] - g[3]) // 2] for g in good if g[2] == 'REG' and (g[1] - g[3]) // 2 < 8})
        report.append('%s %s: %d accesses (%d ranges), regs %s' % (game, name, len(good), len(ranges), ','.join(regs_used)))
    return report


if __name__ == '__main__':
    root = sys.argv[1]
    for game in sys.argv[2:]:
        gdir = os.path.join(root, game)
        if not os.path.exists(os.path.join(gdir, 'regions.txt')):
            print(game, 'no dump')
            continue
        r = scan(gdir, game, None)
        print('\n'.join(r) if r else '%s: no C139 access found' % game)

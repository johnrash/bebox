#!/usr/bin/env python3
"""compare-ppc.py oracle.bin emulator.bin names.txt cases.S

Compares the result records of the instruction test executor and prints
the differing fields of mismatching cases, grouped by instruction."""
import collections
import struct
import sys

RECORD = 384


def fields(rec):
    out = collections.OrderedDict()
    cr, xer, fpscr, ctr, lr = struct.unpack('>5I', rec[0:20])
    out['cr'] = cr
    out['xer'] = xer
    out['fpscr'] = fpscr
    out['ctr'] = ctr
    out['lr'] = lr
    for i in range(10):
        out['r%d' % (i + 3)] = struct.unpack('>I', rec[20 + 4 * i:24 + 4 * i])[0]
    for i in range(32):
        out['f%d' % i] = struct.unpack('>Q', rec[64 + 8 * i:72 + 8 * i])[0]
    out['mem'] = rec[320:384].hex()
    return out


# Fields the architecture leaves undefined or that QEMU does not model;
# both records are normalized before they are compared.
FR = 1 << 18
FI = 1 << 17
XX = 1 << 25
FX = 1 << 31
FPRF = 0x1f << 12
FP_NAMES = ('fadd', 'fsub', 'fmul', 'fdiv', 'fmadd', 'fmsub', 'fnmadd',
            'fnmsub', 'frsp', 'fctiw', 'fctiwz', 'fres', 'frsqrte')


EXC_BITS = 0x1ff80700 & ~(1 << 29)    # OX..VXVC, VXSOFT, VXSQRT, VXCVI


def normalize(name, rec, other, inp, insn):
    rec = bytearray(rec)
    base = name.rstrip('.').rstrip('s') if name not in ('fres',) else name
    fpscr = struct.unpack('>I', rec[8:12])[0]
    cr = struct.unpack('>I', rec[0:4])[0]
    if name.rstrip('.').rstrip('o') in FP_NAMES or base in FP_NAMES:
        # QEMU never sets FPSCR[FR] (fraction rounded)
        fpscr &= ~FR
    if inp is not None and inp['fpscr'] & EXC_BITS and (
            name.rstrip('.').rstrip('s') in FP_NAMES or name in FP_NAMES
            or name.startswith('fcmp')):
        # QEMU sets FX for every exception; the architecture only when an
        # exception bit changes from 0 to 1.
        fpscr &= ~FX
        cr &= ~0x08000000
    if name.startswith('mtfsb1'):
        # QEMU neither applies the FX rule here nor lets NI be set
        fpscr &= ~(FX | (1 << 2))
        cr &= ~0x08000000
    if name.startswith('fcmp'):
        fpscr &= ~(1 << 16)     # QEMU also writes FPRF[C]
    if name.startswith('divw') and inp is not None and inp['gpr']:
        rb = (insn >> 11) & 31
        ra = (insn >> 16) & 31
        b = inp['gpr'][rb - 3] if 3 <= rb <= 12 else None
        a = inp['gpr'][ra - 3] if 3 <= ra <= 12 else None
        if b == 0 or (name.startswith('divw') and not name.startswith('divwu')
                      and a == 0x80000000 and b == 0xffffffff):
            # undefined result: ignore rD and CR0[LT,GT,EQ]
            rd = (insn >> 21) & 31
            if 3 <= rd <= 12:
                rec[20 + 4 * (rd - 3):24 + 4 * (rd - 3)] = b'\0\0\0\0'
            if insn & 1:
                cr &= ~0xe0000000
    if name == 'fres' and inp is not None:
        frb = (insn >> 11) & 31
        if inp['fpr'][frb] & 0x7fffffffffffffff == 0:
            # QEMU bug: fres(+-0) with RN = -inf returns 0.5 instead of inf
            return b''
    if name in ('fres', 'frsqrte'):
        # estimates: FI undefined, XX not affected (PEM); QEMU sets both
        fpscr &= ~(XX | FI | FX)
        cr &= ~0x08000000
    if name in ('fctiw', 'fctiwz'):
        fpscr &= ~FPRF          # undefined
    if name in ('fctiw', 'fctiwz', 'mffs'):
        # the high word of the target is undefined (0 in QEMU, 0xfff80000
        # on Motorola parts and here)
        for i in range(32):
            off = 64 + 8 * i
            hi = struct.unpack('>I', rec[off:off + 4])[0]
            ohi = struct.unpack('>I', other[off:off + 4])[0]
            undefined = {0, 0xfff80000, 0xffffffff}
            if hi != ohi and hi in undefined and ohi in undefined:
                rec[off:off + 4] = b'\0\0\0\0'
    rec[8:12] = struct.pack('>I', fpscr)
    rec[0:4] = struct.pack('>I', cr)
    return bytes(rec)


def main():
    oracle = open(sys.argv[1], 'rb').read()
    emu = open(sys.argv[2], 'rb').read()
    names = open(sys.argv[3]).read().split('\n')
    insns = []
    inputs = []
    cur = None
    for line in open(sys.argv[4]):
        line = line.strip()
        if line.startswith('.long 0x') and line.count(',') == 5:
            words = [int(w.rstrip(','), 16) for w in line.split()[1:]]
            insns.append('%08x' % words[0])
            cur = {'cr': words[1], 'xer': words[2], 'fpscr': words[3],
                   'fpr': [], 'gpr': None}
            inputs.append(cur)
        elif line.startswith('.long') and cur is not None \
                and cur['gpr'] is None:
            vals = []
            for w in line.split(None, 1)[1].split(','):
                w = w.strip()
                vals.append(int(w, 16) if w.startswith('0x') else None)
            cur['gpr'] = vals
        elif line.startswith('.quad') and cur is not None:
            cur['fpr'] += [int(w.rstrip(','), 16) for w in line.split()[1:]]
    n = min(len(oracle), len(emu)) // RECORD
    if len(oracle) != len(emu):
        print('size mismatch: oracle %d bytes, emulator %d bytes'
              % (len(oracle), len(emu)))
    bad = collections.Counter()
    total = collections.Counter()
    shown = collections.Counter()
    for i in range(n):
        name = names[i].split()[0] if i < len(names) and names[i] else '?'
        total[name] += 1
        a = oracle[i * RECORD:(i + 1) * RECORD]
        b = emu[i * RECORD:(i + 1) * RECORD]
        inp = inputs[i] if i < len(inputs) else None
        insn = int(insns[i], 16) if i < len(insns) else 0
        a, b = (normalize(name, a, b, inp, insn),
                normalize(name, b, a, inp, insn))
        if a == b:
            continue
        bad[name] += 1
        if shown[name] >= 3:
            continue
        shown[name] += 1
        fa, fb = fields(a), fields(b)
        diffs = ['%s %s/%s' % (k, ('%x' % fa[k]) if isinstance(fa[k], int)
                               else fa[k], ('%x' % fb[k])
                               if isinstance(fb[k], int) else fb[k])
                 for k in fa if fa[k] != fb[k]]
        print('case %d %s (%s): oracle/emu %s' % (
            i, names[i], insns[i] if i < len(insns) else '?',
            '; '.join(diffs)))
        if i < len(inputs):
            insn = int(insns[i], 16)
            inp = inputs[i]
            fa, fb, fc = (insn >> 16) & 31, (insn >> 11) & 31, (insn >> 6) & 31
            print('    in: cr %08x xer %08x fpscr %08x fA(f%d) %016x '
                  'fB(f%d) %016x fC(f%d) %016x' % (
                      inp['cr'], inp['xer'], inp['fpscr'], fa,
                      inp['fpr'][fa], fb, inp['fpr'][fb], fc,
                      inp['fpr'][fc]))
    print()
    print('%d cases, %d mismatches' % (n, sum(bad.values())))
    for name in sorted(bad, key=lambda k: -bad[k]):
        print('  %-10s %5d / %5d' % (name, bad[name], total[name]))
    sys.exit(1 if bad or len(oracle) != len(emu) else 0)


if __name__ == '__main__':
    main()

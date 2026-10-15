#!/usr/bin/env python3
"""ppc-testgen.py [--seed N] [--count N] [--groups g1,g2] output.S

Generates random single instruction test cases for tests/linux/ppc/exec.S.
Each case gives an instruction word and the full input state (CR, XER,
FPSCR, CTR, LR, r3-r12, f0-f31 and 64 bytes of scratch memory). Register
values that must point into the scratch memory are emitted as symbolic
expressions (scratch + offset), so the case file is assembled and linked
together with the executor.

Groups: int, rot, cmp, cr, spr, load, store, string, fp, fpload, fpscr.
"""
import argparse
import random
import struct

SCRATCH = 'scratch'


def u32(v):
    return v & 0xffffffff


# ---------------------------------------------------------------- encoders

def d_form(op, rt, ra, imm):
    return u32((op << 26) | (rt << 21) | (ra << 16) | (imm & 0xffff))


def x_form(op, rt, ra, rb, xo, rc=0):
    return u32((op << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1)
               | rc)


def xo_form(rt, ra, rb, oe, xo, rc):
    return u32((31 << 26) | (rt << 21) | (ra << 16) | (rb << 11)
               | (oe << 10) | (xo << 1) | rc)


def m_form(op, rs, ra, sh, mb, me, rc):
    return u32((op << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6)
               | (me << 1) | rc)


def a_form(op, frt, fra, frb, frc, xo, rc):
    return u32((op << 26) | (frt << 21) | (fra << 16) | (frb << 11)
               | (frc << 6) | (xo << 1) | rc)


# ---------------------------------------------------------------- values

INT_SPECIAL = [0, 1, 2, 0xffffffff, 0xfffffffe, 0x7fffffff, 0x80000000,
               0x80000001, 0x0000ffff, 0xffff0000, 0x00008000, 0xffff8000,
               31, 32, 33, 63, 64]


def rand_int(r):
    c = r.random()
    if c < 0.3:
        return r.choice(INT_SPECIAL)
    if c < 0.5:
        return r.randrange(0, 256)
    if c < 0.6:
        return u32(-r.randrange(1, 256))
    return r.getrandbits(32)


def dbits(x):
    return struct.unpack('>Q', struct.pack('>d', x))[0]


def fbits_to_dbits(f):
    """single precision bits -> double bits (exact, NaN payload kept)."""
    exp = (f >> 23) & 0xff
    if exp == 0xff:
        return ((f >> 31) << 63) | (0x7ff << 52) | ((f & 0x7fffff) << 29)
    return dbits(struct.unpack('>f', struct.pack('>I', f))[0])


FP_SPECIAL = [
    0x0000000000000000, 0x8000000000000000,     # +-0
    0x7ff0000000000000, 0xfff0000000000000,     # +-inf
    0x7ff8000000000000, 0xfff8000000000001,     # QNaN
    0x7ff4000000000000, 0xfff0000000000123,     # SNaN
    0x0000000000000001, 0x800fffffffffffff,     # double denormals
    0x0010000000000000, 0x7fefffffffffffff,     # min normal, max
    dbits(1.0), dbits(-1.0), dbits(0.5), dbits(2.0), dbits(3.0),
    dbits(1.5), dbits(2.5), dbits(-2.5), dbits(0.1), dbits(1e300),
    dbits(-1e-300), dbits(2147483647.0), dbits(2147483648.0),
    dbits(-2147483648.0), dbits(-2147483649.0), dbits(4294967296.0),
    dbits(1.0e-40), dbits(3.4028234663852886e38), dbits(3.5e38),
    dbits(1.17549435e-38), dbits(1.4e-45),
]


def rand_fp(r):
    c = r.random()
    if c < 0.3:
        return r.choice(FP_SPECIAL)
    if c < 0.55:
        # single precision representable
        f = r.getrandbits(32)
        if (f >> 23) & 0xff == 0xff:
            f &= ~(1 << 30)
        return fbits_to_dbits(f)
    if c < 0.8:
        # moderate magnitudes
        x = r.uniform(-1e6, 1e6)
        if r.random() < 0.3:
            x = float(int(x)) + r.choice([0.0, 0.5, 0.25, 0.75])
        return dbits(x)
    return r.getrandbits(64)


# ---------------------------------------------------------------- case

class Case:
    def __init__(self, r):
        self.insn = 0
        self.cr = r.getrandbits(32)
        self.xer = (r.getrandbits(3) << 29) | r.randrange(0, 128)
        self.fpscr = 0
        self.ctr = rand_int(r)
        self.lr = rand_int(r)
        self.gpr = [rand_int(r) for _ in range(10)]   # r3..r12
        self.fpr = [rand_fp(r) for _ in range(32)]
        self.mem = [r.getrandbits(8) for _ in range(64)]
        self.comment = ''

    def set_gpr(self, reg, value):
        self.gpr[reg - 3] = value

    def emit(self, out):
        out.append('\t# %s' % self.comment)
        words = [self.insn, self.cr, self.xer, self.fpscr, self.ctr, self.lr]
        out.append('\t.long ' + ', '.join('0x%08x' % w for w in words))
        out.append('\t.long ' + ', '.join(
            v if isinstance(v, str) else '0x%08x' % v for v in self.gpr))
        for i in range(0, 32, 4):
            out.append('\t.quad ' + ', '.join('0x%016x' % v
                                              for v in self.fpr[i:i + 4]))
        out.append('\t.byte ' + ', '.join('0x%02x' % b for b in self.mem))


def reg(r):
    return r.randrange(3, 13)


# ---------------------------------------------------------------- groups

XO_OPS = [('subfc', 8), ('addc', 10), ('mulhwu', 11), ('subf', 40),
          ('mulhw', 75), ('neg', 104), ('subfe', 136), ('adde', 138),
          ('subfze', 200), ('addze', 202), ('subfme', 232), ('addme', 234),
          ('mullw', 235), ('add', 266), ('divwu', 459), ('divw', 491)]

X_LOGICAL = [('and', 28), ('andc', 60), ('nor', 124), ('eqv', 284),
             ('xor', 316), ('orc', 412), ('or', 444), ('nand', 476),
             ('slw', 24), ('srw', 536), ('sraw', 792)]

X_UNARY = [('cntlzw', 26), ('extsh', 922), ('extsb', 954)]

D_ARITH = [('mulli', 7), ('subfic', 8), ('addic', 12), ('addic.', 13),
           ('addi', 14), ('addis', 15)]

D_LOGICAL = [('ori', 24), ('oris', 25), ('xori', 26), ('xoris', 27),
             ('andi.', 28), ('andis.', 29)]


def gen_int(r, c):
    kind = r.random()
    if kind < 0.45:
        name, xo = r.choice(XO_OPS)
        oe = 0 if name in ('mulhw', 'mulhwu') else r.getrandbits(1)
        rc = r.getrandbits(1)
        rb = 0 if name in ('neg', 'addme', 'addze', 'subfme', 'subfze') \
            else reg(r)
        c.insn = xo_form(reg(r), reg(r), rb, oe, xo, rc)
        c.comment = '%s%s%s' % (name, 'o' if oe else '', '.' if rc else '')
    elif kind < 0.7:
        name, xo = r.choice(X_LOGICAL)
        rc = r.getrandbits(1)
        c.insn = x_form(31, reg(r), reg(r), reg(r), xo, rc)
        c.comment = name + ('.' if rc else '')
    elif kind < 0.8:
        name, xo = r.choice(X_UNARY)
        rc = r.getrandbits(1)
        c.insn = x_form(31, reg(r), reg(r), 0, xo, rc)
        c.comment = name + ('.' if rc else '')
    elif kind < 0.85:
        rc = r.getrandbits(1)
        c.insn = x_form(31, reg(r), reg(r), r.randrange(32), 824, rc)
        c.comment = 'srawi' + ('.' if rc else '')
    elif kind < 0.93:
        name, op = r.choice(D_ARITH)
        ra = reg(r)
        if name in ('addi', 'addis') and r.random() < 0.2:
            ra = 0
        c.insn = d_form(op, reg(r), ra, r.getrandbits(16))
        c.comment = name
    else:
        name, op = r.choice(D_LOGICAL)
        c.insn = d_form(op, reg(r), reg(r), r.getrandbits(16))
        c.comment = name


def gen_rot(r, c):
    rc = r.getrandbits(1)
    which = r.randrange(3)
    rs, ra = reg(r), reg(r)
    mb, me = r.randrange(32), r.randrange(32)
    if which == 0:
        c.insn = m_form(21, rs, ra, r.randrange(32), mb, me, rc)
        c.comment = 'rlwinm'
    elif which == 1:
        c.insn = m_form(23, rs, ra, reg(r), mb, me, rc)
        c.comment = 'rlwnm'
    else:
        c.insn = m_form(20, rs, ra, r.randrange(32), mb, me, rc)
        c.comment = 'rlwimi'


def gen_cmp(r, c):
    bf = r.randrange(8)
    which = r.randrange(4)
    if r.random() < 0.3:
        c.set_gpr(reg(r), c.gpr[reg(r) - 3])
    if which == 0:
        c.insn = d_form(11, bf << 2, reg(r), r.getrandbits(16))
        c.comment = 'cmpi'
    elif which == 1:
        c.insn = d_form(10, bf << 2, reg(r), r.getrandbits(16))
        c.comment = 'cmpli'
    elif which == 2:
        c.insn = x_form(31, bf << 2, reg(r), reg(r), 0)
        c.comment = 'cmp'
    else:
        c.insn = x_form(31, bf << 2, reg(r), reg(r), 32)
        c.comment = 'cmpl'


CR_OPS = [('crnor', 33), ('crandc', 129), ('crxor', 193), ('crnand', 225),
          ('crand', 257), ('creqv', 289), ('crorc', 417), ('cror', 449)]


def gen_cr(r, c):
    which = r.random()
    if which < 0.7:
        name, xo = r.choice(CR_OPS)
        c.insn = x_form(19, r.randrange(32), r.randrange(32), r.randrange(32),
                        xo)
        c.comment = name
    elif which < 0.8:
        c.insn = x_form(19, r.randrange(8) << 2, r.randrange(8) << 2, 0, 0)
        c.comment = 'mcrf'
    elif which < 0.9:
        c.insn = x_form(31, r.randrange(8) << 2, 0, 0, 512)
        c.comment = 'mcrxr'
    else:
        fxm = r.getrandbits(8)
        c.insn = u32((31 << 26) | (reg(r) << 21) | (fxm << 12) | (144 << 1))
        c.comment = 'mtcrf'


def spr_field(spr):
    return ((spr & 0x1f) << 5) | ((spr >> 5) & 0x1f)


def gen_spr(r, c):
    which = r.randrange(5)
    spr = r.choice([1, 8, 9])
    if which == 0:
        c.insn = u32((31 << 26) | (reg(r) << 21) | (spr_field(spr) << 11)
                     | (339 << 1))
        c.comment = 'mfspr %d' % spr
    elif which == 1:
        c.insn = u32((31 << 26) | (reg(r) << 21) | (spr_field(spr) << 11)
                     | (467 << 1))
        c.comment = 'mtspr %d' % spr
    else:
        c.insn = x_form(31, reg(r), 0, 0, 19)
        c.comment = 'mfcr'


LOADS = [  # name, op/xo, indexed, update, size
    ('lwz', 32, False, False, 4), ('lwzu', 33, False, True, 4),
    ('lbz', 34, False, False, 1), ('lbzu', 35, False, True, 1),
    ('lhz', 40, False, False, 2), ('lhzu', 41, False, True, 2),
    ('lha', 42, False, False, 2), ('lhau', 43, False, True, 2),
    ('lwzx', 23, True, False, 4), ('lwzux', 55, True, True, 4),
    ('lbzx', 87, True, False, 1), ('lbzux', 119, True, True, 1),
    ('lhzx', 279, True, False, 2), ('lhzux', 311, True, True, 2),
    ('lhax', 343, True, False, 2), ('lhaux', 375, True, True, 2),
    ('lwbrx', 534, True, False, 4), ('lhbrx', 790, True, False, 2),
]

STORES = [
    ('stw', 36, False, False, 4), ('stwu', 37, False, True, 4),
    ('stb', 38, False, False, 1), ('stbu', 39, False, True, 1),
    ('sth', 44, False, False, 2), ('sthu', 45, False, True, 2),
    ('stwx', 151, True, False, 4), ('stwux', 183, True, True, 4),
    ('stbx', 215, True, False, 1), ('stbux', 247, True, True, 1),
    ('sthx', 407, True, False, 2), ('sthux', 439, True, True, 2),
    ('stwbrx', 662, True, False, 4), ('sthbrx', 918, True, False, 2),
]


def gen_mem(r, c, table, store):
    name, op, indexed, update, size = r.choice(table)
    rt = reg(r)
    ra = reg(r)
    while update and ra == rt:
        ra = reg(r)
    # effective address inside the scratch memory
    base = r.randrange(0, 32)
    offset = r.randrange(0, 64 - size - base + 1) if 64 - size - base > 0 \
        else 0
    c.set_gpr(ra, '%s+%d' % (SCRATCH, base))
    if indexed:
        rb = reg(r)
        while rb == ra or (rb == rt and not store) or (update and rb == rt):
            rb = reg(r)
        c.set_gpr(rb, offset)
        c.insn = x_form(31, rt, ra, rb, op)
    else:
        c.insn = d_form(op, rt, ra, offset)
    c.comment = name


def gen_load(r, c):
    gen_mem(r, c, LOADS, False)


def gen_store(r, c):
    gen_mem(r, c, STORES, True)


def gen_string(r, c):
    which = r.randrange(4)
    rt = r.randrange(3, 11)
    nbytes = r.randrange(1, min(32, (13 - rt) * 4) + 1)
    loaded = set(range(rt, rt + (nbytes + 3) // 4))
    free = [x for x in range(3, 13) if x not in loaded]
    if len(free) < 2:
        return gen_int(r, c)
    ra = r.choice(free)
    c.set_gpr(ra, '%s+%d' % (SCRATCH, r.randrange(0, 64 - nbytes + 1)))
    if which < 2:
        c.insn = x_form(31, rt, ra, nbytes & 31, 597 if which == 0 else 725)
        c.comment = ('lswi' if which == 0 else 'stswi') + ' %d' % nbytes
    else:
        rb = r.choice([x for x in free if x != ra])
        c.set_gpr(rb, 0)
        c.xer = (c.xer & ~0x7f) | nbytes
        c.insn = x_form(31, rt, ra, rb, 533 if which == 2 else 661)
        c.comment = ('lswx' if which == 2 else 'stswx') + ' %d' % nbytes


FP_A = [('fdiv', 18), ('fsub', 20), ('fadd', 21), ('fmul', 25),
        ('fmsub', 28), ('fmadd', 29), ('fnmsub', 30), ('fnmadd', 31)]


# qemu-ppc runs user programs with MSR[FE0,FE1] set, so enabled
# exceptions would trap (SIGFPE): the random tests never enable them.
FPSCR_ENABLES = 0xf8


def rand_fpscr(r):
    v = r.randrange(4)                          # rounding mode
    if r.random() < 0.3:
        v |= r.getrandbits(32) & 0x9ff80700     # random sticky bits
    return v


def gen_fp(r, c):
    c.fpscr = rand_fpscr(r)
    rc = 1 if r.random() < 0.3 else 0
    which = r.random()
    frt, fra, frb, frc = (r.randrange(32) for _ in range(4))
    if which < 0.55:
        name, xo = r.choice(FP_A)
        single = r.random() < 0.4
        op = 59 if single else 63
        if name in ('fmul',):
            frb = 0
        if name in ('fdiv', 'fsub', 'fadd'):
            frc = 0
        c.insn = a_form(op, frt, fra, frb, frc, xo, rc)
        c.comment = name + ('s' if single else '')
    elif which < 0.6:
        c.insn = a_form(59, frt, 0, frb, 0, 24, rc)
        c.comment = 'fres'
    elif which < 0.65:
        c.insn = a_form(63, frt, 0, frb, 0, 26, rc)
        c.comment = 'frsqrte'
    elif which < 0.7:
        c.insn = a_form(63, frt, fra, frb, frc, 23, rc)
        c.comment = 'fsel'
    elif which < 0.78:
        c.insn = x_form(63, frt, 0, frb, 12, rc)
        c.comment = 'frsp'
    elif which < 0.86:
        xo = r.choice([14, 15])
        c.insn = x_form(63, frt, 0, frb, xo, rc)
        c.comment = 'fctiw' if xo == 14 else 'fctiwz'
    elif which < 0.93:
        xo = r.choice([0, 32])
        c.insn = x_form(63, r.randrange(8) << 2, fra, frb, xo)
        c.comment = 'fcmpu' if xo == 0 else 'fcmpo'
    else:
        xo, name = r.choice([(72, 'fmr'), (40, 'fneg'), (264, 'fabs'),
                             (136, 'fnabs')])
        c.insn = x_form(63, frt, 0, frb, xo, rc)
        c.comment = name


def gen_fpscr(r, c):
    c.fpscr = rand_fpscr(r) | (r.getrandbits(32) & 0x9ff80700)
    rc = 1 if r.random() < 0.3 else 0
    which = r.randrange(6)
    if which == 0:
        c.insn = x_form(63, r.randrange(32), 0, 0, 583, rc)
        c.comment = 'mffs'
    elif which == 1:
        fm = r.getrandbits(8) & 0xfc       # not the enable fields 6, 7
        c.insn = u32((63 << 26) | (fm << 17) | (r.randrange(32) << 11)
                     | (711 << 1) | rc)
        c.comment = 'mtfsf'
    elif which == 2:
        field = r.randrange(8)
        imm = r.randrange(16)
        if field == 6:
            imm = 0
        elif field == 7:
            imm &= 7
        c.insn = u32((63 << 26) | (field << 23) | (imm << 12) | (134 << 1)
                     | rc)
        c.comment = 'mtfsfi'
    elif which == 3:
        c.insn = x_form(63, r.randrange(32), 0, 0, 70, rc)
        c.comment = 'mtfsb0'
    elif which == 4:
        bit = r.choice([b for b in range(32) if not 24 <= b <= 28])
        c.insn = x_form(63, bit, 0, 0, 38, rc)
        c.comment = 'mtfsb1'
    else:
        c.insn = x_form(63, r.randrange(8) << 2, r.randrange(8) << 2, 0, 64)
        c.comment = 'mcrfs'


FP_MEM = [  # name, op/xo, indexed, update, store, size
    ('lfs', 48, False, False, False, 4), ('lfsu', 49, False, True, False, 4),
    ('lfd', 50, False, False, False, 8), ('lfdu', 51, False, True, False, 8),
    ('stfs', 52, False, False, True, 4), ('stfsu', 53, False, True, True, 4),
    ('stfd', 54, False, False, True, 8), ('stfdu', 55, False, True, True, 8),
    ('lfsx', 535, True, False, False, 4), ('lfdx', 599, True, False, False, 8),
    ('stfsx', 663, True, False, True, 4), ('stfdx', 727, True, False, True, 8),
    ('stfiwx', 983, True, False, True, 4),
]


def gen_fpload(r, c):
    name, op, indexed, update, store, size = r.choice(FP_MEM)
    ra = reg(r)
    base = r.randrange(0, 32) & ~3
    offset = r.randrange(0, 64 - size - base + 1) & ~3
    c.set_gpr(ra, '%s+%d' % (SCRATCH, base))
    frt = r.randrange(32)
    if indexed:
        rb = reg(r)
        while rb == ra:
            rb = reg(r)
        c.set_gpr(rb, offset)
        c.insn = x_form(31, frt, ra, rb, op)
    else:
        c.insn = d_form(op, frt, ra, offset)
    if not store and r.random() < 0.3:
        # put interesting single values into memory
        f = r.choice([0x7f800000, 0xff800000, 0x7fc00000, 0x7f800001,
                      0x00000001, 0x80000000, 0x007fffff, 0x3f800000])
        pos = base + offset
        c.mem[pos:pos + 4] = list(struct.pack('>I', f))
    c.comment = name


GROUPS = {
    'int': gen_int, 'rot': gen_rot, 'cmp': gen_cmp, 'cr': gen_cr,
    'spr': gen_spr, 'load': gen_load, 'store': gen_store,
    'string': gen_string, 'fp': gen_fp, 'fpload': gen_fpload,
    'fpscr': gen_fpscr,
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--count', type=int, default=20000)
    parser.add_argument('--groups', default=','.join(GROUPS))
    parser.add_argument('output')
    args = parser.parse_args()

    r = random.Random(args.seed)
    groups = [GROUPS[g] for g in args.groups.split(',')]
    out = ['\t.section .data', '\t.align 3', '\t.globl case_count',
           'case_count:', '\t.long %d' % args.count, '\t.align 3',
           '\t.globl cases', 'cases:']
    names = []
    for n in range(args.count):
        c = Case(r)
        r.choice(groups)(r, c)
        c.emit(out)
        names.append(c.comment)
    with open(args.output, 'w') as f:
        f.write('\n'.join(out) + '\n')
    with open(args.output + '.names', 'w') as f:
        f.write('\n'.join(names) + '\n')


if __name__ == '__main__':
    main()

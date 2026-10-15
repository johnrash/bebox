/*
 * BeBox emulator - PowerPC integer, branch, load/store and system
 * instructions.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "PpcOps.h"

#include "../Log.h"

namespace bebox {

PpcHandler PpcOps::sPrimary[64];
PpcHandler PpcOps::sOp19[1024];
PpcHandler PpcOps::sOp31[1024];
PpcHandler PpcOps::sOp59[1024];
PpcHandler PpcOps::sOp63[1024];

namespace {

#define RD PPC_RD(i)
#define RS PPC_RS(i)
#define RA PPC_RA(i)
#define RB PPC_RB(i)
#define SIMM PPC_SIMM(i)
#define UIMM PPC_UIMM(i)
#define RC PPC_RCBIT(i)
#define OE PPC_OEBIT(i)
#define G c.gpr

inline u32
EaD(PpcCpu& c, u32 i)
{
	return (RA != 0 ? G[RA] : 0) + SIMM;
}


inline u32
EaX(PpcCpu& c, u32 i)
{
	return (RA != 0 ? G[RA] : 0) + G[RB];
}


inline bool
IsUser(PpcCpu& c)
{
	return (c.GetMsr() & MSR_PR) != 0;
}


inline u32
Rotl(u32 value, u32 shift)
{
	shift &= 31;
	return shift == 0 ? value : (value << shift) | (value >> (32 - shift));
}


inline u32
Mask(u32 mb, u32 me)
{
	u32 begin = 0xffffffffu >> mb;
	u32 end = 0xffffffffu << (31 - me);
	return mb <= me ? begin & end : begin | end;
}


// result = x + y + carryIn, with carry out and signed overflow.
inline u32
AddCarry(u32 x, u32 y, u32 carryIn, bool& carry, bool& overflow)
{
	u64 sum = (u64)x + y + carryIn;
	u32 result = (u32)sum;
	carry = (sum >> 32) != 0;
	overflow = (((x ^ result) & (y ^ result)) >> 31) != 0;
	return result;
}


inline void
SetOverflow(PpcCpu& c, bool overflow)
{
	c.fXerOv = overflow;
	if (overflow)
		c.fXerSo = true;
}


inline void
Finish(PpcCpu& c, u32 i, u32 d, u32 result)
{
	G[d] = result;
	if (RC)
		PpcSetCr0(c, result);
}


// #pragma mark - integer arithmetic


void
Op_addi(PpcCpu& c, u32 i)
{
	G[RD] = (RA != 0 ? G[RA] : 0) + SIMM;
}


void
Op_addis(PpcCpu& c, u32 i)
{
	G[RD] = (RA != 0 ? G[RA] : 0) + (UIMM << 16);
}


void
Op_addic(PpcCpu& c, u32 i)
{
	bool carry, overflow;
	G[RD] = AddCarry(G[RA], SIMM, 0, carry, overflow);
	c.fXerCa = carry;
}


void
Op_addic_rc(PpcCpu& c, u32 i)
{
	bool carry, overflow;
	u32 result = AddCarry(G[RA], SIMM, 0, carry, overflow);
	c.fXerCa = carry;
	G[RD] = result;
	PpcSetCr0(c, result);
}


void
Op_subfic(PpcCpu& c, u32 i)
{
	bool carry, overflow;
	G[RD] = AddCarry(~G[RA], SIMM, 1, carry, overflow);
	c.fXerCa = carry;
}


void
Op_mulli(PpcCpu& c, u32 i)
{
	G[RD] = (u32)((s64)(s32)G[RA] * SIMM);
}


void
Op_add(PpcCpu& c, u32 i)
{
	bool carry, overflow;
	u32 result = AddCarry(G[RA], G[RB], 0, carry, overflow);
	if (OE)
		SetOverflow(c, overflow);
	Finish(c, i, RD, result);
}


void
Op_addc(PpcCpu& c, u32 i)
{
	bool carry, overflow;
	u32 result = AddCarry(G[RA], G[RB], 0, carry, overflow);
	c.fXerCa = carry;
	if (OE)
		SetOverflow(c, overflow);
	Finish(c, i, RD, result);
}


void
Op_adde(PpcCpu& c, u32 i)
{
	bool carry, overflow;
	u32 result = AddCarry(G[RA], G[RB], c.fXerCa, carry, overflow);
	c.fXerCa = carry;
	if (OE)
		SetOverflow(c, overflow);
	Finish(c, i, RD, result);
}


void
Op_addme(PpcCpu& c, u32 i)
{
	bool carry, overflow;
	u32 result = AddCarry(G[RA], 0xffffffff, c.fXerCa, carry, overflow);
	c.fXerCa = carry;
	if (OE)
		SetOverflow(c, overflow);
	Finish(c, i, RD, result);
}


void
Op_addze(PpcCpu& c, u32 i)
{
	bool carry, overflow;
	u32 result = AddCarry(G[RA], 0, c.fXerCa, carry, overflow);
	c.fXerCa = carry;
	if (OE)
		SetOverflow(c, overflow);
	Finish(c, i, RD, result);
}


void
Op_subf(PpcCpu& c, u32 i)
{
	bool carry, overflow;
	u32 result = AddCarry(~G[RA], G[RB], 1, carry, overflow);
	if (OE)
		SetOverflow(c, overflow);
	Finish(c, i, RD, result);
}


void
Op_subfc(PpcCpu& c, u32 i)
{
	bool carry, overflow;
	u32 result = AddCarry(~G[RA], G[RB], 1, carry, overflow);
	c.fXerCa = carry;
	if (OE)
		SetOverflow(c, overflow);
	Finish(c, i, RD, result);
}


void
Op_subfe(PpcCpu& c, u32 i)
{
	bool carry, overflow;
	u32 result = AddCarry(~G[RA], G[RB], c.fXerCa, carry, overflow);
	c.fXerCa = carry;
	if (OE)
		SetOverflow(c, overflow);
	Finish(c, i, RD, result);
}


void
Op_subfme(PpcCpu& c, u32 i)
{
	bool carry, overflow;
	u32 result = AddCarry(~G[RA], 0xffffffff, c.fXerCa, carry, overflow);
	c.fXerCa = carry;
	if (OE)
		SetOverflow(c, overflow);
	Finish(c, i, RD, result);
}


void
Op_subfze(PpcCpu& c, u32 i)
{
	bool carry, overflow;
	u32 result = AddCarry(~G[RA], 0, c.fXerCa, carry, overflow);
	c.fXerCa = carry;
	if (OE)
		SetOverflow(c, overflow);
	Finish(c, i, RD, result);
}


void
Op_neg(PpcCpu& c, u32 i)
{
	u32 a = G[RA];
	u32 result = ~a + 1;
	if (OE)
		SetOverflow(c, a == 0x80000000);
	Finish(c, i, RD, result);
}


void
Op_mullw(PpcCpu& c, u32 i)
{
	s64 product = (s64)(s32)G[RA] * (s32)G[RB];
	u32 result = (u32)product;
	if (OE)
		SetOverflow(c, product != (s64)(s32)result);
	Finish(c, i, RD, result);
}


void
Op_mulhw(PpcCpu& c, u32 i)
{
	s64 product = (s64)(s32)G[RA] * (s32)G[RB];
	Finish(c, i, RD, (u32)((u64)product >> 32));
}


void
Op_mulhwu(PpcCpu& c, u32 i)
{
	u64 product = (u64)G[RA] * G[RB];
	Finish(c, i, RD, (u32)(product >> 32));
}


void
Op_divw(PpcCpu& c, u32 i)
{
	s32 a = (s32)G[RA];
	s32 b = (s32)G[RB];
	u32 result;
	bool overflow = b == 0 || (a == (s32)0x80000000 && b == -1);
	if (overflow)
		result = a < 0 ? 0xffffffff : 0;
	else
		result = (u32)(a / b);
	if (OE)
		SetOverflow(c, overflow);
	Finish(c, i, RD, result);
}


void
Op_divwu(PpcCpu& c, u32 i)
{
	u32 a = G[RA];
	u32 b = G[RB];
	bool overflow = b == 0;
	u32 result = overflow ? 0 : a / b;
	if (OE)
		SetOverflow(c, overflow);
	Finish(c, i, RD, result);
}


// #pragma mark - compare


inline u32
CompareSigned(PpcCpu& c, s32 a, s32 b)
{
	u32 field = a < b ? 8 : a > b ? 4 : 2;
	return field | (c.fXerSo ? 1 : 0);
}


inline u32
CompareUnsigned(PpcCpu& c, u32 a, u32 b)
{
	u32 field = a < b ? 8 : a > b ? 4 : 2;
	return field | (c.fXerSo ? 1 : 0);
}


void
Op_cmpi(PpcCpu& c, u32 i)
{
	PpcSetCrField(c, (i >> 23) & 7, CompareSigned(c, G[RA], SIMM));
}


void
Op_cmpli(PpcCpu& c, u32 i)
{
	PpcSetCrField(c, (i >> 23) & 7, CompareUnsigned(c, G[RA], UIMM));
}


void
Op_cmp(PpcCpu& c, u32 i)
{
	PpcSetCrField(c, (i >> 23) & 7, CompareSigned(c, G[RA], G[RB]));
}


void
Op_cmpl(PpcCpu& c, u32 i)
{
	PpcSetCrField(c, (i >> 23) & 7, CompareUnsigned(c, G[RA], G[RB]));
}


// #pragma mark - logical


void Op_andi_rc(PpcCpu& c, u32 i)
	{ G[RA] = G[RS] & UIMM; PpcSetCr0(c, G[RA]); }
void Op_andis_rc(PpcCpu& c, u32 i)
	{ G[RA] = G[RS] & (UIMM << 16); PpcSetCr0(c, G[RA]); }
void Op_ori(PpcCpu& c, u32 i) { G[RA] = G[RS] | UIMM; }
void Op_oris(PpcCpu& c, u32 i) { G[RA] = G[RS] | (UIMM << 16); }
void Op_xori(PpcCpu& c, u32 i) { G[RA] = G[RS] ^ UIMM; }
void Op_xoris(PpcCpu& c, u32 i) { G[RA] = G[RS] ^ (UIMM << 16); }

void Op_and(PpcCpu& c, u32 i) { Finish(c, i, RA, G[RS] & G[RB]); }
void Op_andc(PpcCpu& c, u32 i) { Finish(c, i, RA, G[RS] & ~G[RB]); }
void Op_or(PpcCpu& c, u32 i) { Finish(c, i, RA, G[RS] | G[RB]); }
void Op_orc(PpcCpu& c, u32 i) { Finish(c, i, RA, G[RS] | ~G[RB]); }
void Op_xor(PpcCpu& c, u32 i) { Finish(c, i, RA, G[RS] ^ G[RB]); }
void Op_nor(PpcCpu& c, u32 i) { Finish(c, i, RA, ~(G[RS] | G[RB])); }
void Op_nand(PpcCpu& c, u32 i) { Finish(c, i, RA, ~(G[RS] & G[RB])); }
void Op_eqv(PpcCpu& c, u32 i) { Finish(c, i, RA, ~(G[RS] ^ G[RB])); }
void Op_extsb(PpcCpu& c, u32 i) { Finish(c, i, RA, (u32)(s32)(s8)G[RS]); }
void Op_extsh(PpcCpu& c, u32 i) { Finish(c, i, RA, (u32)(s32)(s16)G[RS]); }

void
Op_cntlzw(PpcCpu& c, u32 i)
{
	u32 value = G[RS];
	Finish(c, i, RA, value == 0 ? 32 : __builtin_clz(value));
}


// #pragma mark - rotate and shift


void
Op_rlwinm(PpcCpu& c, u32 i)
{
	u32 sh = (i >> 11) & 31;
	u32 mb = (i >> 6) & 31;
	u32 me = (i >> 1) & 31;
	Finish(c, i, RA, Rotl(G[RS], sh) & Mask(mb, me));
}


void
Op_rlwnm(PpcCpu& c, u32 i)
{
	u32 mb = (i >> 6) & 31;
	u32 me = (i >> 1) & 31;
	Finish(c, i, RA, Rotl(G[RS], G[RB] & 31) & Mask(mb, me));
}


void
Op_rlwimi(PpcCpu& c, u32 i)
{
	u32 sh = (i >> 11) & 31;
	u32 mb = (i >> 6) & 31;
	u32 me = (i >> 1) & 31;
	u32 mask = Mask(mb, me);
	Finish(c, i, RA, (Rotl(G[RS], sh) & mask) | (G[RA] & ~mask));
}


void
Op_slw(PpcCpu& c, u32 i)
{
	u32 shift = G[RB] & 63;
	Finish(c, i, RA, shift >= 32 ? 0 : G[RS] << shift);
}


void
Op_srw(PpcCpu& c, u32 i)
{
	u32 shift = G[RB] & 63;
	Finish(c, i, RA, shift >= 32 ? 0 : G[RS] >> shift);
}


void
Op_sraw(PpcCpu& c, u32 i)
{
	u32 shift = G[RB] & 63;
	s32 value = (s32)G[RS];
	u32 result;
	bool carry;
	if (shift >= 32) {
		result = value < 0 ? 0xffffffff : 0;
		carry = value < 0;
	} else {
		result = (u32)(value >> shift);
		carry = value < 0 && shift != 0
			&& ((u32)value & ((1u << shift) - 1)) != 0;
	}
	c.fXerCa = carry;
	Finish(c, i, RA, result);
}


void
Op_srawi(PpcCpu& c, u32 i)
{
	u32 shift = (i >> 11) & 31;
	s32 value = (s32)G[RS];
	u32 result = (u32)(value >> shift);
	c.fXerCa = value < 0 && shift != 0
		&& ((u32)value & ((1u << shift) - 1)) != 0;
	Finish(c, i, RA, result);
}


// #pragma mark - branches


inline bool
BranchCondition(PpcCpu& c, u32 bo, u32 bi)
{
	if ((bo & 4) == 0) {
		c.fCtr--;
		if (((c.fCtr != 0) ? 1u : 0u) == ((bo >> 1) & 1))
			return false;
	}
	if ((bo & 16) == 0) {
		u32 bit = (c.fCr >> (31 - bi)) & 1;
		if (bit != ((bo >> 3) & 1))
			return false;
	}
	return true;
}


void
Op_b(PpcCpu& c, u32 i)
{
	s32 offset = ((s32)(i << 6) >> 6) & ~3;
	u32 target = (i & 2) != 0 ? (u32)offset : c.cia + offset;
	if ((i & 1) != 0)
		c.fLr = c.cia + 4;
	c.pc = target;
	if (i == 0x48000000)		// b .
		c.IdleUntilInterrupt();
}


void
Op_bc(PpcCpu& c, u32 i)
{
	bool taken = BranchCondition(c, (i >> 21) & 31, (i >> 16) & 31);
	if ((i & 1) != 0)
		c.fLr = c.cia + 4;
	if (taken) {
		s32 offset = (s32)(s16)(i & 0xfffc);
		c.pc = (i & 2) != 0 ? (u32)offset : c.cia + offset;
	}
}


void
Op_bclr(PpcCpu& c, u32 i)
{
	u32 target = c.fLr & ~3u;
	bool taken = BranchCondition(c, (i >> 21) & 31, (i >> 16) & 31);
	if ((i & 1) != 0)
		c.fLr = c.cia + 4;
	if (taken)
		c.pc = target;
}


void
Op_bcctr(PpcCpu& c, u32 i)
{
	u32 bo = (i >> 21) & 31;
	u32 bi = (i >> 16) & 31;
	u32 target = c.fCtr & ~3u;
	bool taken = (bo & 16) != 0
		|| ((c.fCr >> (31 - bi)) & 1) == ((bo >> 3) & 1);
	if ((i & 1) != 0)
		c.fLr = c.cia + 4;
	if (taken)
		c.pc = target;
}


void
Op_sc(PpcCpu& c, u32)
{
	c.RaiseSyscall();
}


// #pragma mark - condition register


inline u32
CrBit(PpcCpu& c, u32 bit)
{
	return (c.fCr >> (31 - bit)) & 1;
}


template<int kOp>
void
Op_crlogical(PpcCpu& c, u32 i)
{
	u32 a = CrBit(c, (i >> 16) & 31);
	u32 b = CrBit(c, (i >> 11) & 31);
	u32 r;
	switch (kOp) {
		case 0: r = a & b; break;			// crand
		case 1: r = a & ~b; break;			// crandc
		case 2: r = ~(a ^ b); break;		// creqv
		case 3: r = ~(a & b); break;		// crnand
		case 4: r = ~(a | b); break;		// crnor
		case 5: r = a | b; break;			// cror
		case 6: r = a | ~b; break;			// crorc
		default: r = a ^ b; break;			// crxor
	}
	u32 d = 31 - ((i >> 21) & 31);
	c.fCr = (c.fCr & ~(1u << d)) | ((r & 1) << d);
}


void
Op_mcrf(PpcCpu& c, u32 i)
{
	u32 source = (i >> 18) & 7;
	PpcSetCrField(c, (i >> 23) & 7, (c.fCr >> (28 - 4 * source)) & 0xf);
}


void
Op_mcrxr(PpcCpu& c, u32 i)
{
	u32 field = (c.fXerSo ? 8 : 0) | (c.fXerOv ? 4 : 0) | (c.fXerCa ? 2 : 0);
	PpcSetCrField(c, (i >> 23) & 7, field);
	c.fXerSo = c.fXerOv = c.fXerCa = false;
}


void
Op_mfcr(PpcCpu& c, u32 i)
{
	G[RD] = c.fCr;
}


void
Op_mtcrf(PpcCpu& c, u32 i)
{
	u32 fxm = (i >> 12) & 0xff;
	u32 mask = 0;
	for (int field = 0; field < 8; field++) {
		if ((fxm & (0x80 >> field)) != 0)
			mask |= 0xf0000000u >> (4 * field);
	}
	c.fCr = (c.fCr & ~mask) | (G[RS] & mask);
}


// #pragma mark - loads and stores


#define LOAD(name, type, read, ea, update, convert) \
void \
Op_##name(PpcCpu& c, u32 i) \
{ \
	u32 address = ea(c, i); \
	type value; \
	if (!c.read(address, value)) \
		return; \
	G[RD] = convert; \
	if (update) \
		G[RA] = address; \
}

LOAD(lwz, u32, Read32, EaD, false, value)
LOAD(lwzu, u32, Read32, EaD, true, value)
LOAD(lwzx, u32, Read32, EaX, false, value)
LOAD(lwzux, u32, Read32, EaX, true, value)
LOAD(lbz, u8, Read8, EaD, false, value)
LOAD(lbzu, u8, Read8, EaD, true, value)
LOAD(lbzx, u8, Read8, EaX, false, value)
LOAD(lbzux, u8, Read8, EaX, true, value)
LOAD(lhz, u16, Read16, EaD, false, value)
LOAD(lhzu, u16, Read16, EaD, true, value)
LOAD(lhzx, u16, Read16, EaX, false, value)
LOAD(lhzux, u16, Read16, EaX, true, value)
LOAD(lha, u16, Read16, EaD, false, (u32)(s32)(s16)value)
LOAD(lhau, u16, Read16, EaD, true, (u32)(s32)(s16)value)
LOAD(lhax, u16, Read16, EaX, false, (u32)(s32)(s16)value)
LOAD(lhaux, u16, Read16, EaX, true, (u32)(s32)(s16)value)
LOAD(lwbrx, u32, Read32, EaX, false, BSwap32(value))
LOAD(lhbrx, u16, Read16, EaX, false, (u32)BSwap16(value))

#define STORE(name, write, ea, update, value) \
void \
Op_##name(PpcCpu& c, u32 i) \
{ \
	u32 address = ea(c, i); \
	if (!c.write(address, value)) \
		return; \
	if (update) \
		G[RA] = address; \
}

STORE(stw, Write32, EaD, false, G[RS])
STORE(stwu, Write32, EaD, true, G[RS])
STORE(stwx, Write32, EaX, false, G[RS])
STORE(stwux, Write32, EaX, true, G[RS])
STORE(stb, Write8, EaD, false, (u8)G[RS])
STORE(stbu, Write8, EaD, true, (u8)G[RS])
STORE(stbx, Write8, EaX, false, (u8)G[RS])
STORE(stbux, Write8, EaX, true, (u8)G[RS])
STORE(sth, Write16, EaD, false, (u16)G[RS])
STORE(sthu, Write16, EaD, true, (u16)G[RS])
STORE(sthx, Write16, EaX, false, (u16)G[RS])
STORE(sthux, Write16, EaX, true, (u16)G[RS])
STORE(stwbrx, Write32, EaX, false, BSwap32(G[RS]))
STORE(sthbrx, Write16, EaX, false, BSwap16((u16)G[RS]))


void
Op_lmw(PpcCpu& c, u32 i)
{
	u32 address = EaD(c, i);
	if ((address & 3) != 0) {
		c.RaiseAlignment(address + 4, i);
		return;
	}
	for (u32 r = RD; r < 32; r++, address += 4) {
		u32 value;
		if (!c.Read32(address, value))
			return;
		G[r] = value;
	}
}


void
Op_stmw(PpcCpu& c, u32 i)
{
	u32 address = EaD(c, i);
	if ((address & 3) != 0) {
		c.RaiseAlignment(address + 4, i);
		return;
	}
	for (u32 r = RS; r < 32; r++, address += 4) {
		if (!c.Write32(address, G[r]))
			return;
	}
}


void
LoadString(PpcCpu& c, u32 i, u32 address, u32 count)
{
	u32 r = (RD - 1) & 31;
	u32 shift = 0;
	while (count > 0) {
		if (shift == 0) {
			r = (r + 1) & 31;
			G[r] = 0;
		}
		u8 value;
		if (!c.Read8(address, value))
			return;
		G[r] |= (u32)value << (24 - shift);
		shift = (shift + 8) & 31;
		address++;
		count--;
	}
}


void
StoreString(PpcCpu& c, u32 i, u32 address, u32 count)
{
	u32 r = (RS - 1) & 31;
	u32 shift = 0;
	while (count > 0) {
		if (shift == 0)
			r = (r + 1) & 31;
		if (!c.Write8(address, (u8)(G[r] >> (24 - shift))))
			return;
		shift = (shift + 8) & 31;
		address++;
		count--;
	}
}


void
Op_lswi(PpcCpu& c, u32 i)
{
	u32 count = RB == 0 ? 32 : RB;
	LoadString(c, i, RA != 0 ? G[RA] : 0, count);
}


void
Op_lswx(PpcCpu& c, u32 i)
{
	LoadString(c, i, EaX(c, i), c.fXerBc & 0x7f);
}


void
Op_stswi(PpcCpu& c, u32 i)
{
	u32 count = RB == 0 ? 32 : RB;
	StoreString(c, i, RA != 0 ? G[RA] : 0, count);
}


void
Op_stswx(PpcCpu& c, u32 i)
{
	StoreString(c, i, EaX(c, i), c.fXerBc & 0x7f);
}


void
Op_lwarx(PpcCpu& c, u32 i)
{
	u32 address = EaX(c, i);
	if ((address & 3) != 0) {
		c.RaiseAlignment(address + 4, i);
		return;
	}
	u32 value;
	if (!c.Read32(address, value))
		return;
	u32 pa;
	u32 wimg;
	if (!c.TranslateData(address, false, pa, wimg))
		return;
	G[RD] = value;
	c.Bus().SetReservation(c.Index(), pa, value);
}


void
Op_stwcx(PpcCpu& c, u32 i)
{
	u32 address = EaX(c, i);
	if ((address & 3) != 0) {
		c.RaiseAlignment(address + 4, i);
		return;
	}
	u32 pa;
	u32 wimg;
	if (!c.TranslateData(address, true, pa, wimg))
		return;
	PhysBus& bus = c.Bus();
	u32 field = c.fXerSo ? 1 : 0;
	if (bus.CheckReservation(c.Index(), pa)) {
		BusPage page;
		if (bus.IsThreaded()
			&& (page = bus.PageSnapshot(pa)).write != nullptr) {
			// The other CPU runs at the same time: store only if the word
			// still holds what lwarx loaded (another CPU's store between
			// the check above and this one makes it fail).
			u32 expected;
			u32 desired;
			StoreBE32((u8*)&expected, bus.ReservedValue(c.Index()));
			StoreBE32((u8*)&desired, G[RS]);
			if (__atomic_compare_exchange_n(
					(u32*)(page.write + (pa & PhysBus::kPageMask)), &expected,
					desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
				bus.SnoopStore(pa, 4, c.Index());
				field |= 2;
			}
		} else {
			if (!c.Write32(address, G[RS]))
				return;
			field |= 2;
		}
	}
	bus.ClearReservation(c.Index());
	PpcSetCrField(c, 0, field);
}


void
Op_sync(PpcCpu& c, u32)
{
	// The host orders memory accesses less than sync requires when the
	// CPUs run on their own threads.
	if (c.Bus().IsThreaded())
		std::atomic_thread_fence(std::memory_order_seq_cst);
}


// #pragma mark - cache management


void
Op_dcbz(PpcCpu& c, u32 i)
{
	u32 address = EaX(c, i) & ~31u;
	u32 pa;
	u32 wimg;
	if (!c.TranslateData(address, true, pa, wimg))
		return;
	if ((wimg & 0xc) != 0) {
		// write-through or caching-inhibited
		c.RaiseAlignment(EaX(c, i), i);
		return;
	}
	for (int offset = 0; offset < 32; offset += 8) {
		if (!c.Write64(address + offset, 0))
			return;
	}
}


void
Op_dcbf(PpcCpu& c, u32 i)
{
	u32 pa;
	u32 wimg;
	c.TranslateData(EaX(c, i), false, pa, wimg);
}


void
Op_dcbi(PpcCpu& c, u32 i)
{
	if (IsUser(c)) {
		c.RaiseProgram(PROGRAM_PRIVILEGED);
		return;
	}
	u32 pa;
	u32 wimg;
	c.TranslateData(EaX(c, i), true, pa, wimg);
}


void
Op_nop(PpcCpu&, u32)
{
}


// #pragma mark - system


#define PRIVILEGED() \
	if (IsUser(c)) { \
		c.RaiseProgram(PROGRAM_PRIVILEGED); \
		return; \
	}


void
Op_twi(PpcCpu& c, u32 i)
{
	s32 a = (s32)G[RA];
	s32 b = SIMM;
	u32 to = RD;
	if (((to & 16) && a < b) || ((to & 8) && a > b) || ((to & 4) && a == b)
		|| ((to & 2) && (u32)a < (u32)b) || ((to & 1) && (u32)a > (u32)b))
		c.RaiseProgram(PROGRAM_TRAP);
}


void
Op_tw(PpcCpu& c, u32 i)
{
	s32 a = (s32)G[RA];
	s32 b = (s32)G[RB];
	u32 to = RD;
	if (((to & 16) && a < b) || ((to & 8) && a > b) || ((to & 4) && a == b)
		|| ((to & 2) && (u32)a < (u32)b) || ((to & 1) && (u32)a > (u32)b))
		c.RaiseProgram(PROGRAM_TRAP);
}


void
Op_mfmsr(PpcCpu& c, u32 i)
{
	PRIVILEGED();
	G[RD] = c.GetMsr();
}


void
Op_mtmsr(PpcCpu& c, u32 i)
{
	PRIVILEGED();
	c.SetMsr(G[RS]);
}


void
Op_rfi(PpcCpu& c, u32 i)
{
	PRIVILEGED();
	c.ReturnFromInterrupt();
}


void
Op_mfsr(PpcCpu& c, u32 i)
{
	PRIVILEGED();
	G[RD] = c.GetSr((i >> 16) & 15);
}


void
Op_mfsrin(PpcCpu& c, u32 i)
{
	PRIVILEGED();
	G[RD] = c.GetSr(G[RB] >> 28);
}


void
Op_mtsr(PpcCpu& c, u32 i)
{
	PRIVILEGED();
	c.SetSr((i >> 16) & 15, G[RS]);
}


void
Op_mtsrin(PpcCpu& c, u32 i)
{
	PRIVILEGED();
	c.SetSr(G[RB] >> 28, G[RS]);
}


inline int
SprNumber(u32 i)
{
	return ((i >> 16) & 0x1f) | ((i >> 6) & 0x3e0);
}


void
Op_mfspr(PpcCpu& c, u32 i)
{
	int spr = SprNumber(i);
	switch (spr) {
		case SPR_LR:
			G[RD] = c.fLr;
			return;
		case SPR_CTR:
			G[RD] = c.fCtr;
			return;
		case SPR_XER:
			G[RD] = c.GetXer();
			return;
	}
	if ((spr & 0x10) != 0 && IsUser(c)) {
		c.RaiseProgram(PROGRAM_PRIVILEGED);
		return;
	}
	u32 value;
	if (spr == SPR_TBL_READ || spr == SPR_TBU_READ || !c.ReadSpr(spr, value)) {
		BB_LOG(kLogCpu, kLogInfo, "cpu%d: mfspr of unknown SPR %d at %08x",
			c.Index(), spr, c.cia);
		c.RaiseProgram(PROGRAM_ILLEGAL);
		return;
	}
	G[RD] = value;
}


void
Op_mtspr(PpcCpu& c, u32 i)
{
	int spr = SprNumber(i);
	switch (spr) {
		case SPR_LR:
			c.fLr = G[RS];
			return;
		case SPR_CTR:
			c.fCtr = G[RS];
			return;
		case SPR_XER:
			c.SetXer(G[RS]);
			return;
	}
	if ((spr & 0x10) != 0 && IsUser(c)) {
		c.RaiseProgram(PROGRAM_PRIVILEGED);
		return;
	}
	if (spr == SPR_TBL_READ || spr == SPR_TBU_READ
		|| !c.WriteSpr(spr, G[RS])) {
		BB_LOG(kLogCpu, kLogInfo, "cpu%d: mtspr to unknown SPR %d at %08x",
			c.Index(), spr, c.cia);
		c.RaiseProgram(PROGRAM_ILLEGAL);
	}
}


void
Op_mftb(PpcCpu& c, u32 i)
{
	int spr = SprNumber(i);
	u64 timebase = c.GetTimebase();
	if (spr == SPR_TBL_READ)
		G[RD] = (u32)timebase;
	else if (spr == SPR_TBU_READ)
		G[RD] = (u32)(timebase >> 32);
	else
		c.RaiseProgram(PROGRAM_ILLEGAL);
}


void
Op_tlbie(PpcCpu& c, u32 i)
{
	PRIVILEGED();
	c.TlbInvalidate(G[RB]);
}


void
Op_tlbsync(PpcCpu& c, u32 i)
{
	PRIVILEGED();
	if (c.TlbisyncAsserted()) {
		// Stall: execute this instruction again.
		c.pc = c.cia;
		c.RequestExit();
	}
}


void
Op_tlbld(PpcCpu& c, u32 i)
{
	PRIVILEGED();
	c.TlbLoad(false, G[RB]);
}


void
Op_tlbli(PpcCpu& c, u32 i)
{
	PRIVILEGED();
	c.TlbLoad(true, G[RB]);
}


// #pragma mark - dispatch


void
Op_19(PpcCpu& c, u32 i)
{
	PpcOps::sOp19[(i >> 1) & 0x3ff](c, i);
}


void
Op_31(PpcCpu& c, u32 i)
{
	PpcOps::sOp31[(i >> 1) & 0x3ff](c, i);
}


void
Op_59(PpcCpu& c, u32 i)
{
	PpcOps::sOp59[(i >> 1) & 0x3ff](c, i);
}


void
Op_63(PpcCpu& c, u32 i)
{
	PpcOps::sOp63[(i >> 1) & 0x3ff](c, i);
}

}	// namespace


void
PpcOps::Illegal(PpcCpu& c, u32 i)
{
	BB_LOG(kLogCpu, kLogInfo, "cpu%d: illegal instruction %08x at %08x",
		c.Index(), i, c.cia);
	c.RaiseProgram(PROGRAM_ILLEGAL);
}


void
PpcOps::InitTables()
{
	static bool sInitialized = false;
	if (sInitialized)
		return;
	sInitialized = true;

	for (int i = 0; i < 64; i++)
		sPrimary[i] = Illegal;
	for (int i = 0; i < 1024; i++)
		sOp19[i] = sOp31[i] = sOp59[i] = sOp63[i] = Illegal;

	sPrimary[19] = Op_19;
	sPrimary[31] = Op_31;
	sPrimary[59] = Op_59;
	sPrimary[63] = Op_63;

	InitIntegerOps();
	InitFpuOps();
}


void
PpcOps::InitIntegerOps()
{
	PpcHandler* p = sPrimary;
	p[3] = Op_twi;
	p[7] = Op_mulli;
	p[8] = Op_subfic;
	p[10] = Op_cmpli;
	p[11] = Op_cmpi;
	p[12] = Op_addic;
	p[13] = Op_addic_rc;
	p[14] = Op_addi;
	p[15] = Op_addis;
	p[16] = Op_bc;
	p[17] = Op_sc;
	p[18] = Op_b;
	p[20] = Op_rlwimi;
	p[21] = Op_rlwinm;
	p[23] = Op_rlwnm;
	p[24] = Op_ori;
	p[25] = Op_oris;
	p[26] = Op_xori;
	p[27] = Op_xoris;
	p[28] = Op_andi_rc;
	p[29] = Op_andis_rc;
	p[32] = Op_lwz;
	p[33] = Op_lwzu;
	p[34] = Op_lbz;
	p[35] = Op_lbzu;
	p[36] = Op_stw;
	p[37] = Op_stwu;
	p[38] = Op_stb;
	p[39] = Op_stbu;
	p[40] = Op_lhz;
	p[41] = Op_lhzu;
	p[42] = Op_lha;
	p[43] = Op_lhau;
	p[44] = Op_sth;
	p[45] = Op_sthu;
	p[46] = Op_lmw;
	p[47] = Op_stmw;

	PpcHandler* t = sOp19;
	t[0] = Op_mcrf;
	t[16] = Op_bclr;
	t[33] = Op_crlogical<4>;	// crnor
	t[50] = Op_rfi;
	t[129] = Op_crlogical<1>;	// crandc
	t[150] = Op_nop;			// isync
	t[193] = Op_crlogical<7>;	// crxor
	t[225] = Op_crlogical<3>;	// crnand
	t[257] = Op_crlogical<0>;	// crand
	t[289] = Op_crlogical<2>;	// creqv
	t[417] = Op_crlogical<6>;	// crorc
	t[449] = Op_crlogical<5>;	// cror
	t[528] = Op_bcctr;

	t = sOp31;
	// XO form instructions are listed with and without OE.
	struct { int xo; PpcHandler handler; } xoForms[] = {
		{ 8, Op_subfc }, { 10, Op_addc }, { 11, Op_mulhwu },
		{ 40, Op_subf }, { 75, Op_mulhw }, { 104, Op_neg },
		{ 136, Op_subfe }, { 138, Op_adde }, { 200, Op_subfze },
		{ 202, Op_addze }, { 232, Op_subfme }, { 234, Op_addme },
		{ 235, Op_mullw }, { 266, Op_add }, { 459, Op_divwu },
		{ 491, Op_divw }
	};
	for (const auto& form : xoForms) {
		t[form.xo] = form.handler;
		// mulhw and mulhwu have no OE form
		if (form.xo != 11 && form.xo != 75)
			t[form.xo | 512] = form.handler;
	}
	t[0] = Op_cmp;
	t[4] = Op_tw;
	t[19] = Op_mfcr;
	t[20] = Op_lwarx;
	t[23] = Op_lwzx;
	t[24] = Op_slw;
	t[26] = Op_cntlzw;
	t[28] = Op_and;
	t[32] = Op_cmpl;
	t[54] = Op_dcbf;			// dcbst
	t[55] = Op_lwzux;
	t[60] = Op_andc;
	t[83] = Op_mfmsr;
	t[86] = Op_dcbf;
	t[87] = Op_lbzx;
	t[119] = Op_lbzux;
	t[124] = Op_nor;
	t[144] = Op_mtcrf;
	t[146] = Op_mtmsr;
	t[150] = Op_stwcx;
	t[151] = Op_stwx;
	t[183] = Op_stwux;
	t[210] = Op_mtsr;
	t[215] = Op_stbx;
	t[242] = Op_mtsrin;
	t[246] = Op_nop;			// dcbtst
	t[247] = Op_stbux;
	t[278] = Op_nop;			// dcbt
	t[279] = Op_lhzx;
	t[284] = Op_eqv;
	t[306] = Op_tlbie;
	t[311] = Op_lhzux;
	t[316] = Op_xor;
	t[339] = Op_mfspr;
	t[343] = Op_lhax;
	t[371] = Op_mftb;
	t[375] = Op_lhaux;
	t[407] = Op_sthx;
	t[412] = Op_orc;
	t[439] = Op_sthux;
	t[444] = Op_or;
	t[467] = Op_mtspr;
	t[470] = Op_dcbi;
	t[476] = Op_nand;
	t[512] = Op_mcrxr;
	t[533] = Op_lswx;
	t[534] = Op_lwbrx;
	t[536] = Op_srw;
	t[566] = Op_tlbsync;
	t[595] = Op_mfsr;
	t[597] = Op_lswi;
	t[598] = Op_sync;
	t[659] = Op_mfsrin;
	t[661] = Op_stswx;
	t[662] = Op_stwbrx;
	t[725] = Op_stswi;
	t[790] = Op_lhbrx;
	t[792] = Op_sraw;
	t[824] = Op_srawi;
	t[854] = Op_nop;			// eieio
	t[918] = Op_sthbrx;
	t[922] = Op_extsh;
	t[954] = Op_extsb;
	t[978] = Op_tlbld;
	t[982] = Op_nop;			// icbi
	t[1010] = Op_tlbli;
	t[1014] = Op_dcbz;
}

}	// namespace bebox

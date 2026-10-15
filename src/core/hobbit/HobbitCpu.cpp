/*
 * BeBox emulator - AT&T ATT92010 "Hobbit" processor (interpreter).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "HobbitCpu.h"

#include "../Log.h"
#include "../bus/PhysBus.h"

#include <algorithm>

namespace bebox {

// The stack cache size (MSP - SP never exceeds it).
static const u32 kStackCacheSize = 256;

static inline u32
SignExtend(u32 value, int bits)
{
	u32 sign = 1u << (bits - 1);
	return (value ^ sign) - sign;
}


HobbitCpu::HobbitCpu(PhysBus& bus, const Config& config)
	:
	fBus(bus),
	fConfig(config),
	fPc(0),
	fPsw(0),
	fConfigRegister(0),
	fSp(0),
	fMsp(0),
	fIsp(0),
	fShad(0),
	fVb(0),
	fStb(0),
	fFault(0),
	fNow(0),
	fInstructions(0),
	fNextTimerEvent(~0ull),
	fHeldInReset(false),
	fInterruptLevel(7),
	fNmiLatched(false),
	fAttention(false),
	fCpuPrefix(false),
	fInstructionPc(0),
	fIdlePc(1)
{
	for (int i = 0; i < 2; i++) {
		fTimerBase[i] = 0;
		fTimerReference[i] = 0;
		fTimerPending[i] = false;
	}
	memset(fStackCache, 0, sizeof(fStackCache));
	_FlushTlb();
	fBus.AddListener(this);
}


HobbitCpu::~HobbitCpu()
{
	fBus.RemoveListener(this);
}


void
HobbitCpu::BusMapChanged()
{
	_FlushTlb();
}


void
HobbitCpu::Reset()
{
	fShad = 0;
	_ResetSequence();
}


void
HobbitCpu::_ResetSequence()
{
	fPsw = 0;
	fConfigRegister = 0;
	fPc = 0;
	fCpuPrefix = false;
	fNmiLatched = false;
	for (int i = 0; i < 2; i++) {
		fTimerPending[i] = false;
		_SetTimer(i, _TimerValue(i));
	}
	_FlushTlb();
	fAttention = true;
}


void
HobbitCpu::SetHeldInReset(bool held)
{
	fHeldInReset = held;
}


void
HobbitCpu::SetInterruptLevel(int level)
{
	if (level == fInterruptLevel)
		return;
	// NMI (level 0) is edge triggered
	if (level == 0)
		fNmiLatched = true;
	fInterruptLevel = level;
	fAttention = true;
}


// #pragma mark - running


void
HobbitCpu::Run(u64 until)
{
	if (fHeldInReset) {
		if (fNow < until)
			fNow = until;
		return;
	}
	while (fNow < until) {
		if (fNow >= fNextTimerEvent)
			_TimerEvent();
		if (fAttention && _CheckEvents())
			continue;
		if (fPc == fIdlePc && (fPsw & kPswIplMask) == kPswIplMask
			&& fInterruptLevel == 7 && fIdleCheck && fIdleCheck(*this)) {
			// nothing to do until an interrupt
			u64 wake = std::min(until, fNextTimerEvent);
			if (wake > fNow) {
				fNow = wake;
				continue;
			}
		}
		_Step();
	}
}


void
HobbitCpu::_Step()
{
	u32 pc = fPc;
	fInstructionPc = pc;
	bool trace = (fPsw & (kPswTraceInstruction | kPswTraceBlock)) != 0;
	if (fTrace)
		fTrace(*this, pc);
	fBus.SetCurrentPc(fConfig.index, pc);
	fInstructions++;
	fNow += fConfig.ticksPerInstruction;
	try {
		u16 parcel = _FetchParcel(pc);
		fCpuPrefix = false;
		if ((parcel & 0x8000) == 0)
			_ExecuteOneParcel(parcel, pc);
		else {
			bool wide = (parcel & 0x4000) != 0;
			u32 next = pc + (wide ? 10 : 6);
			u32 a = _FetchParcel(pc + 2);
			u32 b = _FetchParcel(pc + 4);
			u32 srcField = a;
			u32 dstField = b;
			if (wide) {
				srcField = (a << 16) | b;
				dstField = ((u32)_FetchParcel(pc + 6) << 16)
					| _FetchParcel(pc + 8);
			}
			int opcode = (parcel >> 8) & 0x3f;
			int srcMode = (parcel >> 4) & 15;
			int dstMode = parcel & 15;
			fPc = next;
			if (opcode == 0 && !wide)
				_ExecuteMonadic(dstMode, srcMode, (a << 16) | b, pc, next);
			else if (opcode == 0x07)
				_Dqm(srcMode, srcField, dstMode, dstField, wide);
			else {
				Operand src = _DecodeOperand(srcMode, srcField, wide, pc);
				Operand dst = _DecodeOperand(dstMode, dstField, wide, pc);
				_ExecuteDyadic(opcode, src, dst, pc, next);
			}
		}
	} catch (const Fault& fault) {
		fCpuPrefix = false;
		_Exception(fault, fInstructionPc);
		return;
	}
	if (trace && (fPsw & (kPswTraceInstruction | kPswTraceBlock)) != 0) {
		// basic block tracing: only after control transfers
		bool transfer = fPc != fInstructionPc + 2 && fPc != fInstructionPc + 6
			&& fPc != fInstructionPc + 10;
		if ((fPsw & kPswTraceInstruction) != 0 || transfer)
			_Exception(Fault{kExTrace, 0}, fPc);
	}
}


bool
HobbitCpu::_CheckEvents()
{
	fAttention = false;
	if (fNmiLatched) {
		fNmiLatched = false;
		_Event(16, false, 0, fPc);
		return true;
	}
	int ipl = (fPsw & kPswIplMask) >> kPswIplShift;
	if (ipl <= 1)
		return false;
	u32 vector = 0;
	if (fInterruptLevel == 1)
		vector = 20;
	else if (fTimerPending[0]
		&& (fConfigRegister & kConfigTimer1Interrupt) != 0) {
		fTimerPending[0] = false;
		vector = 44;
	} else if (fTimerPending[1]
		&& (fConfigRegister & kConfigTimer2Interrupt) != 0) {
		fTimerPending[1] = false;
		vector = 48;
	} else if (fInterruptLevel >= 2 && fInterruptLevel <= 6
		&& fInterruptLevel < ipl)
		vector = 16 + 4 * fInterruptLevel;
	if (vector == 0)
		return false;
	BB_LOG(kLogCpu, kLogTrace, "hobbit%d: interrupt (vector %u) at %08x",
		fConfig.index, vector, fPc);
	_Event(vector, false, 0, fPc);
	// more may be pending (at a lower priority)
	fAttention = true;
	return true;
}


// The interrupt and exception sequences: an event block on the interrupt
// stack (ISP - 12: exception id, - 8: PC, - 4: PSW), kernel mode, ISP as
// the stack, interrupts inhibited.
void
HobbitCpu::_Event(u32 vectorOffset, bool exception, u32 id, u32 pc)
{
	if ((fPsw & kPswUseSp) != 0)
		fSp = fShad;
	else
		fIsp = fShad;
	u32 psw = fPsw;
	// the sequence accesses the interrupt stack at the kernel level
	fPsw &= ~kPswUser;
	u32 newPc;
	try {
		if (exception)
			_Write32(fIsp - 12, id);
		_Write32(fIsp - 8, pc);
		_Write32(fIsp - 4, psw);
		fIsp -= 16;
		fShad = fIsp;
		newPc = _Read32(fVb + vectorOffset);
	} catch (const Fault& fault) {
		BB_LOG(kLogCpu, kLogError, "hobbit%d: fault %d at %08x in the event "
			"sequence (vector %u, pc %08x, isp %08x, vb %08x, %s): reset",
			fConfig.index, fault.id, fault.address, vectorOffset, pc, fIsp,
			fVb, DebugDescribe(fault.address).c_str());
		fShad = psw;
		_ResetSequence();
		return;
	}
	fPc = newPc & ~1u;
	_SetPsw(psw & 0xffff0000);
}


void
HobbitCpu::_Exception(const Fault& fault, u32 pc)
{
	if (fault.id == kExReadFault || fault.id == kExWriteFault
		|| fault.id == kExBusError || fault.id == kExTableWalkBusError)
		fFault = fault.address;
	if (fault.id == kExReadFault || fault.id == kExWriteFault
		|| fault.id == kExFetchFault || fault.id == kExTrace) {
		BB_LOG(kLogCpu, kLogDebug, "hobbit%d: exception %d at pc %08x "
			"(address %08x)", fConfig.index, fault.id, pc, fault.address);
	} else {
		BB_LOG(kLogCpu, kLogInfo, "hobbit%d: exception %d at pc %08x "
			"(address %08x)", fConfig.index, fault.id, pc, fault.address);
	}
	_Event(4, true, fault.id, pc);
}


// The unimplemented instruction sequence (and trapped niladics): like a
// CALL of the handler, the PC of the instruction in R0.
void
HobbitCpu::_UnimplementedSequence(u32 vectorOffset, u32 pc)
{
	_Write32(Csp(), pc);
	fPc = _Read32(fVb + vectorOffset) & ~1u;
}


// #pragma mark - instructions


void
HobbitCpu::_ExecuteOneParcel(u16 parcel, u32 pc)
{
	int opcode = (parcel >> 10) & 0x1f;
	u32 s = (parcel >> 5) & 0x1f;
	u32 d = parcel & 0x1f;
	u32 next = pc + 2;
	fPc = next;
	u32 csp = Csp();

	switch (opcode) {
		case 0x00:		// KCALL imm10 (times 4)
			_Kcall((parcel & 0x3ff) * 4, next);
			return;
		case 0x01:		// CALL pcrel10
			_Write32(csp, next);
			fPc = pc + SignExtend(parcel & 0x3ff, 10) * 2;
			return;
		case 0x02:
		{
			u32 n = (parcel >> 2) & 0xff;
			switch (parcel & 3) {
				case 0:		// ENTER: one filled, times 16 (-16..-4096)
					_Enter(0xd, (0xffffff00 | n) * 16, true);
					return;
				case 1:		// CATCH
					_Catch(0xd, n * 16, true);
					return;
				case 2:		// RETURN
					_Return(n * 16);
					return;
				case 3:		// POPN
					_Popn(n * 16);
					return;
			}
			return;
		}
		case 0x03:		// JMP pcrel10
			fPc = pc + SignExtend(parcel & 0x3ff, 10) * 2;
			return;
		case 0x04:		// JMPFN
		case 0x05:		// JMPFY
			if ((fPsw & kPswFlag) == 0)
				fPc = pc + SignExtend(parcel & 0x3ff, 10) * 2;
			return;
		case 0x06:		// JMPTN
		case 0x07:		// JMPTY
			if ((fPsw & kPswFlag) != 0)
				fPc = pc + SignExtend(parcel & 0x3ff, 10) * 2;
			return;
		case 0x0b:
			_ExecuteNiladic(parcel & 0x3ff, pc);
			return;
		case 0x08:
		case 0x09:
		case 0x0c:
			_UnimplementedSequence(12, pc);
			return;
	}

	// two operand forms, executed as their long forms
	Operand stkS = { Operand::kMemory, 4, false, csp + s * 4 };
	Operand stkD = { Operand::kMemory, 4, false, csp + d * 4 };
	Operand imm5 = { Operand::kImmediate, 4, true, SignExtend(s, 5) };
	Operand wai5 = { Operand::kImmediate, 4, false, s * 4 };
	switch (opcode) {
		case 0x0a:		// MOV wai5, stk5
			_ExecuteDyadic(0x06, wai5, stkD, pc, next);
			return;
		case 0x0d:		// ADD3 wai5, stk5
			_ExecuteDyadic(0x33, wai5, stkD, pc, next);
			return;
		case 0x0e:		// AND3 imm5, stk5
			_ExecuteDyadic(0x32, imm5, stkD, pc, next);
			return;
		case 0x0f:		// AND stk5, stk5
			_ExecuteDyadic(0x22, stkS, stkD, pc, next);
			return;
		case 0x10:		// CMPEQ imm5, stk5
			_ExecuteDyadic(0x1f, imm5, stkD, pc, next);
			return;
		case 0x11:		// CMPGT stk5, stk5
			_ExecuteDyadic(0x1d, stkS, stkD, pc, next);
			return;
		case 0x12:		// CMPGT imm5, stk5
			_ExecuteDyadic(0x1d, imm5, stkD, pc, next);
			return;
		case 0x13:		// CMPEQ stk5, stk5
			_ExecuteDyadic(0x1f, stkS, stkD, pc, next);
			return;
		case 0x14:		// ADD imm5, stk5
			_ExecuteDyadic(0x23, imm5, stkD, pc, next);
			return;
		case 0x15:		// ADD3 imm5, stk5
			_ExecuteDyadic(0x33, imm5, stkD, pc, next);
			return;
		case 0x16:		// ADD stk5, stk5
			_ExecuteDyadic(0x23, stkS, stkD, pc, next);
			return;
		case 0x17:		// ADD3 stk5, stk5
			_ExecuteDyadic(0x33, stkS, stkD, pc, next);
			return;
		case 0x18:		// MOV stk5, stk5
			_ExecuteDyadic(0x06, stkS, stkD, pc, next);
			return;
		case 0x19:		// MOV istk5, stk5
		{
			Operand src = { Operand::kMemory, 4, false,
				_Read32(csp + s * 4) };
			_ExecuteDyadic(0x06, src, stkD, pc, next);
			return;
		}
		case 0x1a:		// MOV stk5, istk5
		{
			Operand dst = { Operand::kMemory, 4, false,
				_Read32(csp + d * 4) };
			_ExecuteDyadic(0x06, stkS, dst, pc, next);
			return;
		}
		case 0x1b:		// MOV istk5, istk5
		{
			Operand src = { Operand::kMemory, 4, false,
				_Read32(csp + s * 4) };
			Operand dst = { Operand::kMemory, 4, false,
				_Read32(csp + d * 4) };
			_ExecuteDyadic(0x06, src, dst, pc, next);
			return;
		}
		case 0x1c:		// MOV imm5, stk5
			_ExecuteDyadic(0x06, imm5, stkD, pc, next);
			return;
		case 0x1d:		// MOVA stk5, stk5
			_ExecuteDyadic(0x04, stkS, stkD, pc, next);
			return;
		case 0x1e:		// SHL3 uimm5, stk5
			wai5.value = s;
			_ExecuteDyadic(0x3e, wai5, stkD, pc, next);
			return;
		case 0x1f:		// SHR3 uimm5, stk5
			wai5.value = s;
			_ExecuteDyadic(0x3c, wai5, stkD, pc, next);
			return;
	}
}


void
HobbitCpu::_ExecuteNiladic(u16 subcode, u32 pc)
{
	if (subcode >= 0x200) {
		// trapped niladic
		_UnimplementedSequence(8, pc);
		return;
	}
	switch (subcode) {
		case 0x000:		// CPU: the next instruction may address registers
		{
			u16 parcel = _FetchParcel(pc + 2);
			if ((parcel & 0x8000) == 0) {
				// nothing to prefix: just a no-op
				return;
			}
			bool wide = (parcel & 0x4000) != 0;
			u32 next = pc + 2 + (wide ? 10 : 6);
			u32 a = _FetchParcel(pc + 4);
			u32 b = _FetchParcel(pc + 6);
			u32 srcField = a;
			u32 dstField = b;
			if (wide) {
				srcField = (a << 16) | b;
				dstField = ((u32)_FetchParcel(pc + 8) << 16)
					| _FetchParcel(pc + 10);
			}
			int opcode = (parcel >> 8) & 0x3f;
			int srcMode = (parcel >> 4) & 15;
			int dstMode = parcel & 15;
			fPc = next;
			fCpuPrefix = true;
			if (opcode == 0 && !wide)
				_ExecuteMonadic(dstMode, srcMode, (a << 16) | b, pc, next);
			else {
				Operand src = _DecodeOperand(srcMode, srcField, wide, pc);
				Operand dst = _DecodeOperand(dstMode, dstField, wide, pc);
				_ExecuteDyadic(opcode, src, dst, pc, next);
			}
			fCpuPrefix = false;
			return;
		}
		case 0x001:		// KRET
			_Kret();
			return;
		case 0x002:		// NOP
		case 0x003:		// FLUSHI
		case 0x004:		// FLUSHP
			return;
		case 0x005:		// CRET
			_Cret();
			return;
		case 0x008:		// TESTV
			fPsw = (fPsw & ~(kPswFlag | kPswOverflow))
				| ((fPsw & kPswOverflow) != 0 ? kPswFlag : 0);
			return;
		case 0x009:		// TESTC
			fPsw = (fPsw & ~(kPswFlag | kPswCarry))
				| ((fPsw & kPswCarry) != 0 ? kPswFlag : 0);
			return;
		case 0x00a:		// CLRE
			fPsw &= ~kPswEnterGuard;
			return;
		default:		// FLUSHD and the unassigned ones
			_UnimplementedSequence(12, pc);
			return;
	}
}


// Three parcel monadics: the operand is a 32-bit field.
void
HobbitCpu::_ExecuteMonadic(int subcode, int mode, u32 field, u32 pc,
	u32 next)
{
	switch (subcode) {
		case 0x0:		// KCALL word32
		{
			Operand operand = _DecodeOperand(mode, field, true, pc);
			_Kcall(_Load(operand), next);
			return;
		}
		case 0x1:		// CALL flow32
		{
			u32 target = _FlowTarget(mode, field, pc);
			_Write32(Csp(), next);
			fPc = target & ~1u;
			return;
		}
		case 0x2:		// RETURN stk32
			if (mode != 0xd || (s32)field < 0)
				throw Fault{kExIllegal, 0};
			_Return(field);
			return;
		case 0x3:		// JMP flow32
			fPc = _FlowTarget(mode, field, pc) & ~1u;
			return;
		case 0x4:		// JMPFN abs32
		case 0x5:		// JMPFY
		case 0x6:		// JMPTN
		case 0x7:		// JMPTY
		{
			if (mode != 0xe && mode != 0xf)
				throw Fault{kExIllegal, 0};
			bool flag = (fPsw & kPswFlag) != 0;
			if (flag == (subcode >= 0x6))
				fPc = _FlowTarget(mode, field, pc) & ~1u;
			return;
		}
		case 0x8:		// CATCH word32
			if (mode < 0xc)
				throw Fault{kExIllegal, 0};
			_Catch(mode, field, true);
			return;
		case 0x9:		// ENTER word32
			if (mode < 0xc)
				throw Fault{kExIllegal, 0};
			_Enter(mode, field, true);
			return;
		case 0xa:		// LDRAA
			_Write32(Csp() + 4, _FlowTarget(mode, field, pc));
			return;
		case 0xb:		// FLUSHPTE word32: the operand's address
		case 0xc:		// FLUSHPBE
		{
			if (mode < 0xc)
				throw Fault{kExIllegal, 0};
			// (the operand's address calculation may fault)
			_DecodeOperand(mode, field, true, pc);
			if (subcode == 0xb) {
				// the nonpaged segment registers go too: a segment's pages
				// are cached one by one here, so everything goes
				_FlushTlb();
			}
			return;
		}
		case 0xf:		// POPN stk32
			if (mode != 0xd || (s32)field < 0)
				throw Fault{kExIllegal, 0};
			_Popn(field);
			return;
		default:		// FLUSHDCE and the unassigned ones
			if (mode < 0xc)
				throw Fault{kExIllegal, 0};
			_UnimplementedSequence(12, pc);
			return;
	}
}


void
HobbitCpu::_ExecuteDyadic(int opcode, Operand& src, Operand& dst, u32 pc,
	u32 next)
{
	// the accumulator (R4) for the "3" forms
	auto accumulator = [this](u32 value) {
		_Write32(Csp() + 4, value);
	};
	auto setFlags = [this](bool carry, bool overflow) {
		fPsw = (fPsw & ~(kPswCarry | kPswOverflow))
			| (carry ? kPswCarry : 0) | (overflow ? kPswOverflow : 0);
	};

	switch (opcode) {
		case 0x06:		// MOV
			_Store(dst, _Load(src));
			return;
		case 0x04:		// MOVA
			if (src.kind != Operand::kMemory
				|| dst.kind == Operand::kRegister)
				throw Fault{kExIllegal, 0};
			_Store(dst, src.value);
			return;
		case 0x05:		// UREM (no UREM3)
		{
			u32 s = _Load(src);
			u32 d = _Load(dst);
			if (s == 0)
				throw Fault{kExZeroDivide, 0};
			_Store(dst, d % s);
			fPsw &= ~kPswCarry;
			return;
		}

		case 0x01:		// ORI
		case 0x02:		// ANDI
		case 0x03:		// ADDI
		{
			u32 s = _Load(src);
			u32 d = _Load(dst);
			u32 result = opcode == 0x01 ? d | s : opcode == 0x02 ? d & s
				: d + s;
			_Store(dst, result);
			accumulator(d);
			return;
		}

		case 0x1d:		// CMPGT
		case 0x1e:		// CMPHI
		case 0x1f:		// CMPEQ
		{
			u32 a = _Load(src);
			u32 b = _Load(dst);
			bool result = opcode == 0x1d ? (s32)a > (s32)b
				: opcode == 0x1e ? a > b : a == b;
			fPsw = (fPsw & ~kPswFlag) | (result ? kPswFlag : 0);
			return;
		}

		case 0x0c:		// TADD
		case 0x0d:		// TSUB
		{
			u32 s = _Load(src);
			u32 d = _Load(dst);
			bool flag;
			u32 result = 0;
			if (((s | d) & 3) != 0)
				flag = true;
			else {
				if (opcode == 0x0c) {
					result = d + s;
					setFlags(result < d, ((~(d ^ s) & (d ^ result)) >> 31)
						!= 0);
				} else {
					result = d - s;
					setFlags(d < s, (((d ^ s) & (d ^ result)) >> 31) != 0);
				}
				flag = (fPsw & kPswOverflow) != 0;
			}
			fPsw = (fPsw & ~kPswFlag) | (flag ? kPswFlag : 0);
			if (!flag)
				_Store(dst, result);
			return;
		}
	}

	bool toAccumulator = opcode >= 0x30;
	int operation = toAccumulator ? opcode - 0x10 : opcode;
	u32 result;
	switch (operation) {
		case 0x20:		// SUB
		{
			u32 s = _Load(src);
			u32 d = _Load(dst);
			result = d - s;
			setFlags(d < s, (((d ^ s) & (d ^ result)) >> 31) != 0);
			break;
		}
		case 0x21:		// OR
			result = _Load(dst) | _Load(src);
			break;
		case 0x22:		// AND
			result = _Load(dst) & _Load(src);
			break;
		case 0x23:		// ADD
		{
			u32 s = _Load(src);
			u32 d = _Load(dst);
			result = d + s;
			setFlags(result < d, ((~(d ^ s) & (d ^ result)) >> 31) != 0);
			break;
		}
		case 0x24:		// XOR
			result = _Load(dst) ^ _Load(src);
			break;
		case 0x25:		// REM
		{
			u32 s = _Load(src);
			u32 d = _Load(dst);
			if (s == 0)
				throw Fault{kExZeroDivide, 0};
			result = s == 0xffffffff ? 0 : (u32)((s32)d % (s32)s);
			fPsw &= ~kPswOverflow;
			break;
		}
		case 0x26:		// MUL
		{
			u32 s = _Load(src);
			u32 d = _Load(dst);
			u64 unsignedProduct = (u64)d * s;
			s64 signedProduct = (s64)(s32)d * (s32)s;
			result = (u32)unsignedProduct;
			setFlags((unsignedProduct >> 32) != 0,
				signedProduct != (s64)(s32)result);
			break;
		}
		case 0x27:		// DIV
		{
			u32 s = _Load(src);
			u32 d = _Load(dst);
			if (s == 0)
				throw Fault{kExZeroDivide, 0};
			if (d == 0x80000000 && s == 0xffffffff) {
				result = 0x80000000;
				fPsw |= kPswOverflow;
			} else {
				result = (u32)((s32)d / (s32)s);
				fPsw &= ~kPswOverflow;
			}
			break;
		}
		case 0x2c:		// SHR
		{
			u32 s = _Load(src);
			result = (u32)((s32)_Load(dst) >> (s & 31));
			break;
		}
		case 0x2d:		// USHR
		{
			u32 s = _Load(src);
			result = _Load(dst) >> (s & 31);
			break;
		}
		case 0x2e:		// SHL
		{
			u32 s = _Load(src);
			result = _Load(dst) << (s & 31);
			break;
		}
		case 0x2f:		// UDIV (no UDIV3)
		{
			if (toAccumulator) {
				_UnimplementedSequence(12, pc);
				return;
			}
			u32 s = _Load(src);
			u32 d = _Load(dst);
			if (s == 0)
				throw Fault{kExZeroDivide, 0};
			result = d / s;
			fPsw &= ~kPswCarry;
			break;
		}
		default:
			if (fCpuPrefix && (src.kind == Operand::kRegister
					|| dst.kind == Operand::kRegister))
				throw Fault{kExIllegal, 0};
			_UnimplementedSequence(12, pc);
			return;
	}
	if (toAccumulator)
		accumulator(result);
	else
		_Store(dst, result);
}


void
HobbitCpu::_Dqm(int srcMode, u32 srcField, int dstMode, u32 dstField,
	bool wide)
{
	// the destination mode gives the size: 0x0, 0x4, 0x8 two words, 0xc,
	// 0xd, 0xe four
	int words;
	switch (dstMode) {
		case 0x0: case 0x4: case 0x8:
			words = 2;
			break;
		case 0xc: case 0xd: case 0xe:
			words = 4;
			break;
		case 0xf:
			throw Fault{kExIllegal, 0};
		default:
			throw Fault{kExAlignment, 0};
	}
	if (srcMode < 0xc)
		throw Fault{kExIllegal, 0};
	u32 offset = wide ? dstField : SignExtend(dstField & 0xffff, 16);
	u32 dstAddress;
	switch (dstMode) {
		case 0x0: case 0xc:
			dstAddress = _AbsoluteAddress(dstField, wide, fInstructionPc);
			break;
		case 0x4: case 0xd:
			dstAddress = Csp() + offset;
			break;
		default:
		{
			u32 pointer = Csp() + offset;
			if ((pointer & 3) != 0)
				throw Fault{kExAlignment, 0};
			dstAddress = _Read32(pointer);
			break;
		}
	}
	u32 alignment = words * 4 - 1;
	if ((dstAddress & alignment) != 0)
		throw Fault{kExAlignment, 0};

	u32 values[4];
	if (srcMode == 0xf) {
		// a constant, replicated
		u32 value = wide ? srcField : SignExtend(srcField & 0xffff, 16);
		for (int i = 0; i < words; i++)
			values[i] = value;
	} else {
		Operand src = _DecodeOperand(srcMode, srcField, wide,
			fInstructionPc);
		if ((src.value & alignment) != 0)
			throw Fault{kExAlignment, 0};
		for (int i = 0; i < words; i++)
			values[i] = _Read32(src.value + i * 4);
	}
	for (int i = 0; i < words; i++)
		_Write32(dstAddress + i * 4, values[i]);
}


// ENTER: a new stack frame. A stack offset makes the CSP relative to
// itself, other modes load it.
void
HobbitCpu::_Enter(int mode, u32 field, bool wide)
{
	u32 csp = fShad;
	u32 target;
	bool relative = mode == 0xd;
	if (relative) {
		if ((s32)field > 0)
			throw Fault{kExIllegal, 0};
		target = csp + field;
	} else
		target = _StackTarget(mode, field);
	target &= ~15u;
	// the new frame must be writable (else a read fault, as documented)
	if ((fPsw & kPswVirtual) != 0) {
		u32 pa;
		int faultId;
		if (!_Translate(target, kWrite, (fPsw & kPswUser) != 0, pa, faultId,
				false))
			throw Fault{kExReadFault, target};
	}
	if ((fPsw & kPswUseSp) == 0) {
		fIsp = target;
		fShad = target;
	} else if (relative) {
		// write back what no longer fits above the new SP
		if (fMsp - target > kStackCacheSize) {
			_StackWriteBack(target + kStackCacheSize);
			if (fMsp > target + kStackCacheSize)
				fMsp = target + kStackCacheSize;
		}
		fSp = target;
		fShad = target;
	} else {
		// everything goes to memory: an empty cache at the new SP
		_StackWriteBack(fSp);
		fMsp = target;
		fSp = target;
		fShad = target;
	}
	fPsw |= kPswEnterGuard;
}


// The new stack pointer (ENTER) or stack cache bound (CATCH) of the
// modes other than a stack offset, as the manual defines them: an
// absolute operand is the address of the value, an indirect stack offset
// the stack word holding it (one indirection less than the general modes).
u32
HobbitCpu::_StackTarget(int mode, u32 field)
{
	switch (mode) {
		case 0xc:
			return _Read32(field);
		case 0xe:
		{
			u32 address = fShad + field;
			if ((address & 3) != 0)
				throw Fault{kExAlignment, 0};
			return _Read32(address);
		}
		case 0xf:
			return field;
	}
	throw Fault{kExIllegal, 0};
}


// CATCH: the stack cache filled up to a new MSP (no data moves here).
void
HobbitCpu::_Catch(int mode, u32 field, bool wide)
{
	if ((fPsw & kPswUseSp) == 0)
		return;
	u32 target;
	if (mode == 0xd)
		target = fShad + field;
	else
		target = _StackTarget(mode, field);
	_StackFill(target);
}


void
HobbitCpu::_StackWriteBack(u32 msp)
{
	if ((fConfigRegister & kConfigStackCache) == 0) {
		fMsp = std::max(msp, fSp);
		return;
	}
	while (fMsp > msp && fMsp > fSp) {
		u32 block = fMsp - 16;
		for (int i = 0; i < 4; i++)
			_MemWrite32(block + i * 4, _StackWord(block + i * 4));
		fMsp = block;
	}
}


void
HobbitCpu::_StackFill(u32 target)
{
	target = (target + 15) & ~15u;
	u32 limit = fSp + kStackCacheSize;
	if (target > limit)
		target = limit;
	if ((fConfigRegister & kConfigStackCache) == 0) {
		if (target > fMsp)
			fMsp = target;
		return;
	}
	// A fill that faults just stops: what is not cached is read from
	// memory (and faults) when used.
	try {
		while (fMsp < target) {
			u32 words[4];
			for (int i = 0; i < 4; i++)
				words[i] = _MemRead32(fMsp + i * 4);
			for (int i = 0; i < 4; i++)
				_StackWord(fMsp + i * 4) = words[i];
			fMsp += 16;
		}
	} catch (const Fault&) {
	}
}


void
HobbitCpu::_Return(u32 offset)
{
	u32 csp = Csp();
	u32 target = _Read32(csp + offset);
	_SetCsp(csp + (offset & ~15u));
	fPc = target & ~1u;
}


void
HobbitCpu::_Popn(u32 offset)
{
	_SetCsp(Csp() + (offset & ~15u));
}


void
HobbitCpu::_SetCsp(u32 value)
{
	if ((fPsw & kPswUseSp) != 0) {
		fSp = value;
		if (fSp > fMsp)
			fMsp = fSp;
	} else
		fIsp = value;
	fShad = value;
}


void
HobbitCpu::_Kcall(u32 value, u32 next)
{
	u32 psw = fPsw;
	fPsw &= ~kPswUser;
	try {
		_Write32(fIsp - 12, value);
		_Write32(fIsp - 8, next);
		_Write32(fIsp - 4, psw);
		fIsp -= 16;
		fShad = fIsp;
		fPc = _Read32(fVb) & ~1u;
	} catch (const Fault& fault) {
		BB_LOG(kLogCpu, kLogError, "hobbit%d: fault %d at %08x in KCALL "
			"(isp %08x, vb %08x): reset", fConfig.index, fault.id,
			fault.address, fIsp, fVb);
		fShad = psw;
		_ResetSequence();
		return;
	}
	_SetPsw(psw & 0xffff0000);
}


void
HobbitCpu::_Kret()
{
	if ((fPsw & kPswUser) != 0)
		throw Fault{kExPrivilege, 0};
	u32 pc, psw;
	try {
		pc = _Read32(fIsp + 8);
		psw = _Read32(fIsp + 12);
	} catch (const Fault& fault) {
		BB_LOG(kLogCpu, kLogError, "hobbit%d: fault %d at %08x in KRET "
			"(isp %08x): reset", fConfig.index, fault.id, fault.address,
			fIsp);
		fShad = fPsw;
		_ResetSequence();
		return;
	}
	fIsp += 16;
	fPc = pc & ~1u;
	_SetPsw(psw);
	fShad = StackRegister();
}


void
HobbitCpu::_Cret()
{
	if ((fPsw & kPswUser) != 0)
		throw Fault{kExPrivilege, 0};
	u32 sp, msp, pc, psw;
	try {
		sp = _Read32(fIsp);
		msp = _Read32(fIsp + 4);
		pc = _Read32(fIsp + 8);
		psw = _Read32(fIsp + 12);
	} catch (const Fault& fault) {
		BB_LOG(kLogCpu, kLogError, "hobbit%d: fault %d at %08x in CRET "
			"(isp %08x): reset", fConfig.index, fault.id, fault.address,
			fIsp);
		fShad = fPsw;
		_ResetSequence();
		return;
	}
	fIsp += 16;
	fPc = pc & ~1u;
	_SetPsw(psw);
	// the stack cache is emptied, then filled up to the MSP
	fSp = sp & ~15u;
	fMsp = fSp;
	if ((fPsw & kPswUseSp) != 0)
		_StackFill(msp);
	fShad = StackRegister();
}


// The target of a CALL, JMP or LDRAA.
u32
HobbitCpu::_FlowTarget(int mode, u32 field, u32 pc)
{
	switch (mode) {
		case 0xc:		// **$addr: absolute indirect
			return _Read32(field);
		case 0xd:		// *Roffset: stack offset indirect
		{
			u32 address = Csp() + field;
			if ((address & 3) != 0)
				throw Fault{kExAlignment, 0};
			return _Read32(address);
		}
		case 0xe:		// PC relative
			return pc + field;
		case 0xf:		// *$addr: absolute
			return field;
	}
	throw Fault{kExIllegal, 0};
}


// #pragma mark - operands


u32
HobbitCpu::_AbsoluteAddress(u32 field, bool wide, u32 pc) const
{
	if (wide)
		return field;
	field &= 0xffff;
	if ((fConfigRegister & kConfigPcExtension) != 0)
		return (pc & 0xe0000000) | field;
	return field;
}


HobbitCpu::Operand
HobbitCpu::_DecodeOperand(int mode, u32 field, bool wide, u32 pc)
{
	static const u8 kSizes[16] = { 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2,
		4, 4, 4, 4 };
	Operand operand;
	operand.size = kSizes[mode];
	operand.isSigned = (mode & 1) == 0;
	u32 offset = wide ? field : SignExtend(field & 0xffff, 16);
	if (fCpuPrefix) {
		if (mode == 0x7) {
			operand.kind = Operand::kRegister;
			operand.size = 4;
			operand.value = field & 15;
			return operand;
		}
		if (mode < 0xc)
			throw Fault{kExIllegal, 0};
	}
	switch (mode) {
		case 0x0: case 0x1: case 0x2: case 0x3: case 0xc:
			operand.kind = Operand::kMemory;
			operand.value = _AbsoluteAddress(field, wide, pc);
			return operand;
		case 0x4: case 0x5: case 0x6: case 0x7: case 0xd:
			operand.kind = Operand::kMemory;
			operand.value = Csp() + offset;
			return operand;
		case 0x8: case 0x9: case 0xa: case 0xb: case 0xe:
		{
			u32 pointer = Csp() + offset;
			if ((pointer & 3) != 0)
				throw Fault{kExAlignment, 0};
			operand.kind = Operand::kMemory;
			operand.value = _Read32(pointer);
			return operand;
		}
		default:	// 0xf
			operand.kind = Operand::kImmediate;
			operand.value = offset;
			return operand;
	}
}


u32
HobbitCpu::_Load(const Operand& operand)
{
	switch (operand.kind) {
		case Operand::kImmediate:
			return operand.value;
		case Operand::kRegister:
			return _ReadRegister(operand.value);
	}
	switch (operand.size) {
		case 1:
		{
			u32 value = _Read8(operand.value);
			return operand.isSigned ? SignExtend(value, 8) : value;
		}
		case 2:
		{
			u32 value = _Read16(operand.value);
			return operand.isSigned ? SignExtend(value, 16) : value;
		}
		default:
			return _Read32(operand.value);
	}
}


void
HobbitCpu::_Store(const Operand& operand, u32 value)
{
	switch (operand.kind) {
		case Operand::kImmediate:
			throw Fault{kExIllegal, 0};
		case Operand::kRegister:
			_WriteRegister(operand.value, value);
			return;
	}
	switch (operand.size) {
		case 1:
			_Write8(operand.value, (u8)value);
			return;
		case 2:
			_Write16(operand.value, (u16)value);
			return;
		default:
			_Write32(operand.value, value);
			return;
	}
}


// #pragma mark - registers


u32
HobbitCpu::Register(int number)
{
	return _ReadRegister(number);
}


void
HobbitCpu::SetRegister(int number, u32 value)
{
	_WriteRegister(number, value);
}


u32
HobbitCpu::_ReadRegister(int number)
{
	switch (number) {
		case kRegMsp: return fMsp;
		case kRegIsp: return fIsp;
		case kRegSp: return fSp;
		case kRegConfig: return fConfigRegister;
		case kRegPsw: return fPsw;
		case kRegShad: return fShad;
		case kRegVb: return fVb;
		case kRegStb: return fStb;
		case kRegFault: return fFault;
		case kRegId: return fConfig.id;
		case kRegTimer1: return _TimerValue(0);
		case kRegTimer2: return _TimerValue(1);
	}
	throw Fault{kExUnimplementedRegister, 0};
}


void
HobbitCpu::_WriteRegister(int number, u32 value)
{
	if (number == 0 || number > kRegTimer2)
		throw Fault{kExUnimplementedRegister, 0};
	if ((fPsw & kPswUser) != 0)
		throw Fault{kExPrivilege, 0};
	switch (number) {
		case kRegMsp:
			fMsp = value & ~15u;
			break;
		case kRegIsp:
			fIsp = value & ~15u;
			break;
		case kRegSp:
			// an empty stack cache
			fSp = value & ~15u;
			fMsp = fSp;
			break;
		case kRegConfig:
		{
			u32 timers[2] = { _TimerValue(0), _TimerValue(1) };
			if ((value & (kConfigKernelLittle | kConfigTimer1KernelOnly
					| kConfigTimer2KernelOnly)) != 0) {
				BB_LOG(kLogCpu, kLogWarning, "hobbit%d: CONFIG %08x: "
					"unsupported little endian or kernel only timer bits",
					fConfig.index, value);
			}
			fConfigRegister = value & kConfigValid;
			_SetTimer(0, timers[0]);
			_SetTimer(1, timers[1]);
			fAttention = true;
			break;
		}
		case kRegPsw:
			_SetPsw(value);
			break;
		case kRegShad:
			fShad = value & ~15u;
			break;
		case kRegVb:
			fVb = value & ~15u;
			break;
		case kRegStb:
			BB_LOG(kLogMmu, kLogDebug, "hobbit%d: STB %08x at %08x",
				fConfig.index, value, fInstructionPc);
			fStb = value & ~0x7ffu;
			_FlushTlb();
			break;
		case kRegFault:
			fFault = value;
			break;
		case kRegId:
			break;
		case kRegTimer1:
			_SetTimer(0, value);
			break;
		case kRegTimer2:
			_SetTimer(1, value);
			break;
	}
}


void
HobbitCpu::_SetPsw(u32 psw)
{
	psw &= kPswValid;
	if ((psw & kPswUserLittle) != 0) {
		BB_LOG(kLogCpu, kLogWarning, "hobbit%d: user little endian mode is "
			"not supported", fConfig.index);
	}
	fPsw = psw;
	fAttention = true;
}


// #pragma mark - timers


bool
HobbitCpu::_TimerCountsInstructions(int timer) const
{
	if (timer == 0)
		return (fConfigRegister & kConfigTimer1Instructions) != 0;
	return ((fConfigRegister >> kConfigTimer2EventShift) & 0x1f) == 1;
}


u32
HobbitCpu::_TimerValue(int timer) const
{
	if (timer == 1) {
		u32 event = (fConfigRegister >> kConfigTimer2EventShift) & 0x1f;
		if (event > 1)
			return fTimerBase[1];		// stopped
	}
	u64 count = _TimerCountsInstructions(timer) ? fInstructions : _Cycles();
	return fTimerBase[timer] + (u32)(count - fTimerReference[timer]);
}


void
HobbitCpu::_SetTimer(int timer, u32 value)
{
	fTimerBase[timer] = value;
	fTimerReference[timer] = _TimerCountsInstructions(timer) ? fInstructions
		: _Cycles();
	_ScheduleTimers();
}


// The next overflow of a timer whose interrupt is enabled, in ticks
// (instruction counting timers as if each instruction took its ticks).
void
HobbitCpu::_ScheduleTimers()
{
	fNextTimerEvent = ~0ull;
	for (int timer = 0; timer < 2; timer++) {
		u32 enable = timer == 0 ? kConfigTimer1Interrupt
			: kConfigTimer2Interrupt;
		if ((fConfigRegister & enable) == 0)
			continue;
		if (timer == 1
			&& ((fConfigRegister >> kConfigTimer2EventShift) & 0x1f) > 1)
			continue;
		u64 remaining = (1ull << 32) - _TimerValue(timer);
		u64 ticks = _TimerCountsInstructions(timer)
			? remaining * fConfig.ticksPerInstruction
			: remaining * fConfig.ticksPerCycle
				- fNow % fConfig.ticksPerCycle;
		fNextTimerEvent = std::min(fNextTimerEvent, fNow + ticks);
	}
}


void
HobbitCpu::_TimerEvent()
{
	for (int timer = 0; timer < 2; timer++) {
		u32 enable = timer == 0 ? kConfigTimer1Interrupt
			: kConfigTimer2Interrupt;
		if ((fConfigRegister & enable) == 0)
			continue;
		u64 count = _TimerCountsInstructions(timer) ? fInstructions
			: _Cycles();
		u64 overflow = fTimerReference[timer]
			+ ((1ull << 32) - fTimerBase[timer]);
		if (fTimerBase[timer] == 0)
			overflow = fTimerReference[timer] + (1ull << 32);
		if (count >= overflow) {
			// keeps counting from 0
			fTimerBase[timer] = (u32)(count - overflow);
			fTimerReference[timer] = count;
			fTimerPending[timer] = true;
			fAttention = true;
		}
	}
	_ScheduleTimers();
}


// #pragma mark - memory


void
HobbitCpu::_FlushTlb()
{
	for (int i = 0; i < kTlbSize; i++) {
		fFetchTlb[i].tag = ~0u;
		fReadTlb[i].tag = ~0u;
		fWriteTlb[i].tag = ~0u;
	}
}


HobbitCpu::TlbEntry&
HobbitCpu::_LookupMiss(TlbEntry& entry, u32 va, u32 tag, Access access)
{
	u32 pa = va;
	if ((fPsw & kPswVirtual) != 0) {
		int faultId;
		if (!_Translate(va, access, (fPsw & kPswUser) != 0, pa, faultId,
				true))
			throw Fault{faultId, va & ~3u};
	}
	pa &= ~PhysBus::kPageMask;
	const BusPage& page = fBus.Page(pa);
	if (page.read == nullptr && page.device == nullptr) {
		BB_LOG(kLogBus, kLogInfo, "hobbit%d: bus error at %08x (pa %08x), "
			"pc %08x", fConfig.index, va, pa | (va & PhysBus::kPageMask),
			fInstructionPc);
		throw Fault{kExBusError, va};
	}
	entry.tag = tag;
	entry.host = access == kWrite ? page.write : page.read;
	entry.physical = pa;
	return entry;
}


bool
HobbitCpu::_Translate(u32 va, Access access, bool user, u32& pa,
	int& faultId, bool update)
{
	faultId = access == kFetch ? kExFetchFault : access == kWrite
		? kExWriteFault : kExReadFault;
	u32 steAddress = (fStb & 0xfffff000) | ((va >> 22) << 2);
	u32 ste = _PhysRead32(steAddress);
	if ((ste & 1) == 0)
		return false;
	if ((ste & 8) != 0) {
		// nonpaged segment
		u32 offset = va & 0x3fffff;
		if ((offset >> 12) > ((ste >> 12) & 0x3ff))
			return false;
		if (user && (ste & 4) == 0)
			return false;
		if (access == kWrite && (ste & 2) == 0)
			return false;
		pa = (ste & 0xffc00000) | offset;
		return true;
	}
	u32 pteAddress = (ste & 0xfffff000) | (((va >> 12) & 0x3ff) << 2);
	u32 pte = _PhysRead32(pteAddress);
	if ((pte & 1) == 0)
		return false;
	if (user && (pte & 4) == 0)
		return false;
	if (access == kWrite && (pte & 2) == 0)
		return false;
	if (update) {
		u32 updated = pte | 0x08 | (access == kWrite ? 0x10 : 0);
		if (updated != pte)
			_PhysWrite32(pteAddress, updated);
	}
	pa = (pte & 0xfffff000) | (va & 0xfff);
	return true;
}


u32
HobbitCpu::_PhysRead32(u32 pa)
{
	const BusPage& page = fBus.Page(pa);
	if (page.read != nullptr)
		return LoadBE32(page.read + (pa & PhysBus::kPageMask));
	if (page.device == nullptr)
		throw Fault{kExTableWalkBusError, pa};
	return fBus.Read32(pa, fConfig.index);
}


void
HobbitCpu::_PhysWrite32(u32 pa, u32 value)
{
	const BusPage& page = fBus.Page(pa);
	if (page.write != nullptr) {
		StoreBE32(page.write + (pa & PhysBus::kPageMask), value);
		return;
	}
	if (page.device == nullptr)
		throw Fault{kExTableWalkBusError, pa};
	fBus.Write32(pa, value, fConfig.index);
}


u16
HobbitCpu::_FetchParcel(u32 va)
{
	va &= ~1u;
	TlbEntry& entry = _Lookup(fFetchTlb, va, kFetch);
	if (entry.host != nullptr)
		return LoadBE16(entry.host + (va & PhysBus::kPageMask));
	return fBus.Read16(entry.physical | (va & PhysBus::kPageMask),
		fConfig.index);
}


u8
HobbitCpu::_Read8(u32 va)
{
	if (_InStackCache(va))
		return (u8)(_StackWord(va) >> (8 * (3 - (va & 3))));
	TlbEntry& entry = _Lookup(fReadTlb, va, kRead);
	if (entry.host != nullptr)
		return entry.host[va & PhysBus::kPageMask];
	return fBus.Read8(entry.physical | (va & PhysBus::kPageMask),
		fConfig.index);
}


u16
HobbitCpu::_Read16(u32 va)
{
	if ((va & 1) != 0)
		throw Fault{kExAlignment, va};
	if (_InStackCache(va))
		return (u16)(_StackWord(va) >> ((va & 2) != 0 ? 0 : 16));
	TlbEntry& entry = _Lookup(fReadTlb, va, kRead);
	if (entry.host != nullptr)
		return LoadBE16(entry.host + (va & PhysBus::kPageMask));
	return fBus.Read16(entry.physical | (va & PhysBus::kPageMask),
		fConfig.index);
}


u32
HobbitCpu::_Read32(u32 va)
{
	if ((va & 3) != 0)
		throw Fault{kExAlignment, va};
	if (_InStackCache(va))
		return _StackWord(va);
	return _MemRead32(va);
}


u32
HobbitCpu::_MemRead32(u32 va)
{
	TlbEntry& entry = _Lookup(fReadTlb, va, kRead);
	if (entry.host != nullptr)
		return LoadBE32(entry.host + (va & PhysBus::kPageMask));
	return fBus.Read32(entry.physical | (va & PhysBus::kPageMask),
		fConfig.index);
}


void
HobbitCpu::_Write8(u32 va, u8 value)
{
	if (_InStackCache(va)) {
		int shift = 8 * (3 - (va & 3));
		u32& word = _StackWord(va);
		word = (word & ~(0xffu << shift)) | ((u32)value << shift);
		return;
	}
	TlbEntry& entry = _Lookup(fWriteTlb, va, kWrite);
	if (entry.host != nullptr) {
		entry.host[va & PhysBus::kPageMask] = value;
		return;
	}
	fBus.Write8(entry.physical | (va & PhysBus::kPageMask), value,
		fConfig.index);
}


void
HobbitCpu::_Write16(u32 va, u16 value)
{
	if ((va & 1) != 0)
		throw Fault{kExAlignment, va};
	if (_InStackCache(va)) {
		int shift = (va & 2) != 0 ? 0 : 16;
		u32& word = _StackWord(va);
		word = (word & ~(0xffffu << shift)) | ((u32)value << shift);
		return;
	}
	TlbEntry& entry = _Lookup(fWriteTlb, va, kWrite);
	if (entry.host != nullptr) {
		StoreBE16(entry.host + (va & PhysBus::kPageMask), value);
		return;
	}
	fBus.Write16(entry.physical | (va & PhysBus::kPageMask), value,
		fConfig.index);
}


void
HobbitCpu::_Write32(u32 va, u32 value)
{
	if ((va & 3) != 0)
		throw Fault{kExAlignment, va};
	if (_InStackCache(va)) {
		_StackWord(va) = value;
		return;
	}
	_MemWrite32(va, value);
}


void
HobbitCpu::_MemWrite32(u32 va, u32 value)
{
	TlbEntry& entry = _Lookup(fWriteTlb, va, kWrite);
	if (entry.host != nullptr) {
		StoreBE32(entry.host + (va & PhysBus::kPageMask), value);
		return;
	}
	fBus.Write32(entry.physical | (va & PhysBus::kPageMask), value,
		fConfig.index);
}


// #pragma mark - debugging


bool
HobbitCpu::DebugTranslate(u32 va, u32& pa)
{
	if ((fPsw & kPswVirtual) == 0) {
		pa = va;
		return true;
	}
	try {
		int faultId;
		return _Translate(va, kRead, (fPsw & kPswUser) != 0, pa, faultId,
			false);
	} catch (const Fault&) {
		return false;
	}
}


std::string
HobbitCpu::DebugDescribe(u32 va)
{
	char text[160];
	u32 steAddress = (fStb & 0xfffff000) | ((va >> 22) << 2);
	const BusPage& stePage = fBus.Page(steAddress);
	if (stePage.read == nullptr) {
		snprintf(text, sizeof(text), "stb %08x: segment table not in memory",
			fStb);
		return text;
	}
	u32 ste = LoadBE32(stePage.read + (steAddress & PhysBus::kPageMask));
	if ((ste & 9) != 1) {
		snprintf(text, sizeof(text), "stb %08x ste %08x", fStb, ste);
		return text;
	}
	u32 pteAddress = (ste & 0xfffff000) | (((va >> 12) & 0x3ff) << 2);
	const BusPage& ptePage = fBus.Page(pteAddress);
	u32 pte = ptePage.read != nullptr
		? LoadBE32(ptePage.read + (pteAddress & PhysBus::kPageMask)) : 0;
	snprintf(text, sizeof(text), "stb %08x ste %08x pte %08x (at %08x)",
		fStb, ste, pte, pteAddress);
	return text;
}


u32
HobbitCpu::DebugRead32(u32 va)
{
	if ((va & 3) == 0 && _InStackCache(va))
		return _StackWord(va);
	u32 pa;
	if (!DebugTranslate(va, pa))
		return 0;
	const BusPage& page = fBus.Page(pa);
	if (page.read == nullptr || (pa & 3) != 0)
		return 0;
	return LoadBE32(page.read + (pa & PhysBus::kPageMask));
}

}	// namespace bebox

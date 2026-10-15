/*
 * BeBox emulator - PowerPC 603 / 603e processor.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "PpcCpu.h"

#include "PpcOps.h"
#include "../Log.h"

#include <algorithm>

#if defined(__x86_64__) || defined(__i386__)
#	include <xmmintrin.h>
#	define BEBOX_HAVE_MXCSR 1
#endif

namespace bebox {

PpcCpu::PpcCpu(PhysBus& bus, const Config& config)
	:
	pc(0),
	cia(0),
	fNow(0),
	fStopAt(0),
	fRunUntil(0),
	fIntLine(false),
	fSmiLine(false),
	fHresetLine(false),
	fResetWanted(false),
	fSresetPending(false),
	fTlbisyncLine(false),
	fRequests(0),
	fDecPending(false),
	fDozing(false),
	fCheckstop(false),
	fSoftGen(0),
	fFetchTag(kInvalidTag),
	fFetchHost(nullptr),
	fBus(bus),
	fConfig(config),
	fDebugHook(nullptr),
	fTraceHook(nullptr),
	fInstructions(0),
	fExceptions(0),
	fHostMxcsr(0)
{
	PpcOps::InitTables();
	memset(fSoftRead, 0xff, sizeof(fSoftRead));
	memset(fSoftWrite, 0xff, sizeof(fSoftWrite));
	memset(fSoftFetch, 0xff, sizeof(fSoftFetch));
	fBus.AddListener(this);
	HardReset();
}


PpcCpu::~PpcCpu()
{
	fBus.RemoveListener(this);
}


void
PpcCpu::HardReset()
{
	memset(gpr, 0, sizeof(gpr));
	memset(fpr, 0, sizeof(fpr));
	memset(fTgpr, 0, sizeof(fTgpr));
	fCr = 0;
	fLr = 0;
	fCtr = 0;
	fXerSo = fXerOv = fXerCa = false;
	fXerBc = 0;
	fXerOther = 0;
	fFpscr = 0;
	fMsr = MSR_IP;
	fSrr0 = fSrr1 = 0;
	memset(fSprg, 0, sizeof(fSprg));
	fDar = fDsisr = 0;
	fSdr1 = 0;
	fEar = 0;
	fPvr = fConfig.pvr;
	fHid0 = 0;
	fHid1 = fConfig.model == kPpc603e ? fConfig.hid1 : 0;
	fIabr = 0;
	fDmiss = fDcmp = fHash1 = fHash2 = fImiss = fIcmp = fRpa = 0;
	memset(fSr, 0, sizeof(fSr));
	for (int i = 0; i < 4; i++) {
		fIbat[i].upper = fIbat[i].lower = 0;
		fDbat[i].upper = fDbat[i].lower = 0;
		_DecodeBat(fIbat[i]);
		_DecodeBat(fDbat[i]);
	}
	memset(fItlb, 0, sizeof(fItlb));
	memset(fDtlb, 0, sizeof(fDtlb));
	memset(fItlbNextWay, 0, sizeof(fItlbNextWay));
	memset(fDtlbNextWay, 0, sizeof(fDtlbNextWay));

	fTbOffset = -(s64)_TimebaseTicks();
	SetDec(0xffffffff);
	fDecPending = false;
	fSresetPending = false;
	fDozing = false;
	fCheckstop = false;

	fBus.ClearReservation(fConfig.index);
	_UpdateModeTables();
	_FlushSoftTlb();
	pc = cia = 0xfff00100;
	RequestExit();
}


// #pragma mark - signals


void
PpcCpu::SetIntLine(bool asserted)
{
	fIntLine = asserted;
	if (asserted && (_IsForeignThread() || (fMsr & MSR_EE) != 0))
		RequestExit();
}


void
PpcCpu::SetSmiLine(bool asserted)
{
	fSmiLine = asserted;
	if (asserted && (_IsForeignThread() || (fMsr & MSR_EE) != 0))
		RequestExit();
}


void
PpcCpu::SetHresetLine(bool asserted)
{
	fResetWanted = asserted;
	if (_IsForeignThread()) {
		_Request(kRequestReset);
		return;
	}
	if (fHresetLine && !asserted)
		HardReset();
	fHresetLine = asserted;
	if (asserted)
		RequestExit();
}


// Asks the CPU's own thread to act before its next instruction. Setting
// the request before ending the execution loop pairs with the check in
// _UpdateStopAt(), so the request is not missed.
void
PpcCpu::_Request(u32 request)
{
	fRequests.fetch_or(request);
	RequestExit();
}


void
PpcCpu::_ApplyRequests()
{
	u32 requests = fRequests.exchange(0);
	if ((requests & kRequestReset) != 0) {
		bool wanted = fResetWanted;
		if (fHresetLine && !wanted)
			HardReset();
		fHresetLine = wanted;
	}
	if ((requests & kRequestFlush) != 0)
		_FlushSoftTlb();
}


void
PpcCpu::SetSresetLine(bool asserted)
{
	// The soft reset exception is taken on the assertion edge.
	if (asserted) {
		fSresetPending = true;
		RequestExit();
	}
}


void
PpcCpu::SetTlbisyncLine(bool asserted)
{
	fTlbisyncLine = asserted;
}


// #pragma mark - execution


thread_local PpcCpu* PpcCpu::sCurrent = nullptr;


void
PpcCpu::Run(u64 until)
{
	struct Current {
		Current(PpcCpu* cpu) { sCurrent = cpu; }
		~Current() { sCurrent = nullptr; }
	} current(this);
	if (fRequests.load(std::memory_order_relaxed) != 0)
		_ApplyRequests();
	fRunUntil = until;
	if (fHresetLine || fCheckstop) {
		if (fNow < until)
			fNow = until;
		return;
	}

#ifdef BEBOX_HAVE_MXCSR
	fHostMxcsr = _mm_getcsr();
	PpcOps::SetHostRounding(fFpscr);
#endif

	while (fNow < until && !fHresetLine && !fCheckstop) {
		if (fRequests.load(std::memory_order_relaxed) != 0) {
			_ApplyRequests();
			continue;
		}
		if (fNow >= fDecExpire) {
			fDecPending = true;
			fDecExpire += ((u64)1 << 32) * fConfig.ticksPerTimebase;
		}
		if (fSresetPending) {
			fSresetPending = false;
			fDozing = false;
			_EnterException(EXC_RESET, pc, 0);
		}
		_CheckInterrupts();
		if (fDozing) {
			// Sleep until the decrementer could wake us, or the slice
			// ends. External interrupts end the slice through
			// RequestExit().
			u64 wake = until;
			if ((fMsr & MSR_EE) != 0 && !fDecPending)
				wake = std::min(wake, fDecExpire);
			fNow = std::max(fNow, wake);
			continue;
		}

		bool stepping = fDebugHook != nullptr || fTraceHook != nullptr
			|| (fMsr & (MSR_SE | MSR_BE)) != 0 || (fIabr & 2) != 0;
		_Execute(until, stepping);
		if (stepping && fDebugHook != nullptr && fDebugHook->ShouldStop(*this))
			break;
	}

#ifdef BEBOX_HAVE_MXCSR
	_mm_setcsr(fHostMxcsr);
#endif
}


void
PpcCpu::Step()
{
	if (fRequests.load(std::memory_order_relaxed) != 0)
		_ApplyRequests();
	if (fHresetLine || fCheckstop)
		return;
#ifdef BEBOX_HAVE_MXCSR
	fHostMxcsr = _mm_getcsr();
	PpcOps::SetHostRounding(fFpscr);
#endif
	if (fNow >= fDecExpire) {
		fDecPending = true;
		fDecExpire += ((u64)1 << 32) * fConfig.ticksPerTimebase;
	}
	_CheckInterrupts();
	if (!fDozing)
		_Execute(fNow + fConfig.ticksPerInstruction, true);
	else
		fNow += fConfig.ticksPerInstruction;
#ifdef BEBOX_HAVE_MXCSR
	_mm_setcsr(fHostMxcsr);
#endif
}


void
PpcCpu::_UpdateStopAt()
{
	fStopAt.store(std::min(fRunUntil, fDecExpire));
	if (fRequests.load() != 0)
		fStopAt.store(0);
}


void
PpcCpu::_CheckInterrupts()
{
	if ((fMsr & MSR_EE) == 0)
		return;
	if (fSmiLine) {
		_EnterException(EXC_SMI, pc, 0);
	} else if (fIntLine) {
		_EnterException(EXC_EXTERNAL, pc, 0);
	} else if (fDecPending) {
		fDecPending = false;
		_EnterException(EXC_DECREMENTER, pc, 0);
	}
}


void
PpcCpu::_Execute(u64 until, bool singleStep)
{
	fRunUntil = until;
	_UpdateStopAt();
	const u32 ticks = fConfig.ticksPerInstruction;
	const PpcHandler* table = PpcOps::sPrimary;

	if (!singleStep) {
		while (fNow < fStopAt.load(std::memory_order_relaxed)) {
			u32 instruction;
			cia = pc;
			if ((cia & ~0xfffu) == fFetchTag) {
				instruction = LoadBE32(fFetchHost + (cia & 0xfff));
			} else if (!_FetchSlow(cia, instruction)) {
				fNow += ticks;
				continue;
			}
			pc = cia + 4;
			table[instruction >> 26](*this, instruction);
			fNow += ticks;
			fInstructions++;
		}
		return;
	}

	// One instruction with all the checks: debugger, trace hook,
	// instruction address breakpoint, single step and branch trace.
	cia = pc;
	if ((fIabr & 2) != 0 && (cia & ~3u) == (fIabr & ~3u)
		&& ((fIabr & 1) != 0) == ((fMsr & MSR_IR) != 0)) {
		_EnterException(EXC_BREAKPOINT, cia, 0);
		fNow += ticks;
		return;
	}
	u32 instruction;
	if (!_FetchSlow(cia, instruction)) {
		fNow += ticks;
		return;
	}
	if (fTraceHook != nullptr)
		fTraceHook->Instruction(*this, cia, instruction);
	u32 msrBefore = fMsr;
	u64 exceptionsBefore = fExceptions;
	pc = cia + 4;
	table[instruction >> 26](*this, instruction);
	fNow += ticks;
	fInstructions++;

	// Trace exceptions after successful completion (not if the instruction
	// took an exception, and never for rfi or isync).
	if ((msrBefore & (MSR_SE | MSR_BE)) == 0)
		return;
	u32 op = instruction >> 26;
	u32 xo = (instruction >> 1) & 0x3ff;
	bool isRfi = op == 19 && xo == 50;
	bool isIsync = op == 19 && xo == 150;
	bool isSc = op == 17;
	bool tookException = fExceptions != exceptionsBefore;
	if (isRfi || isIsync || isSc || tookException)
		return;
	bool isBranch = op == 16 || op == 18 || (op == 19 && (xo == 16
		|| xo == 528));
	if ((msrBefore & MSR_SE) != 0 || ((msrBefore & MSR_BE) != 0 && isBranch))
		_EnterException(EXC_TRACE, pc, 0);
}


bool
PpcCpu::_FetchSlow(u32 address, u32& instruction)
{
	// another page: translated before?
	const SoftTlbEntry& cached
		= fCurFetch[(address >> 12) & (kSoftTlbSize - 1)];
	if (cached.tag == ((address & ~0xfffu) | fSoftGen)) {
		fFetchTag = address & ~0xfffu;
		fFetchHost = cached.host;
		instruction = LoadBE32(cached.host + (address & 0xfff));
		return true;
	}
	u32 pa;
	u32 wimg;
	bool viaPageTable;
	if (!_Translate(address, kFetch, pa, wimg, viaPageTable))
		return false;
	BusPage page = fBus.PageSnapshot(pa);
	if (page.read != nullptr) {
		SoftTlbEntry& entry = fCurFetch[(address >> 12) & (kSoftTlbSize - 1)];
		entry.tag = (address & ~0xfffu) | fSoftGen;
		entry.pa = pa & ~0xfffu;
		entry.host = page.read;
		fFetchTag = address & ~0xfffu;
		fFetchHost = page.read;
		instruction = LoadBE32(page.read + (address & 0xfff));
		return true;
	}
	fBus.SetCurrentPc(fConfig.index, address);
	instruction = fBus.Read32(pa, fConfig.index);
	return true;
}


// #pragma mark - exceptions


void
PpcCpu::_EnterException(u32 vector, u32 srr0, u32 srr1High, bool tgpr)
{
	fExceptions++;
	fSrr0 = srr0;
	fSrr1 = (srr1High & 0xffff0000) | (fMsr & 0xffff);
	u32 msr = fMsr & (MSR_ME | MSR_IP | MSR_ILE);
	if (vector == EXC_MACHINE_CHECK)
		msr &= ~MSR_ME;
	if ((fMsr & MSR_ILE) != 0)
		msr |= MSR_LE;
	if (tgpr)
		msr |= MSR_TGPR;
	SetMsr(msr);
	pc = ((fMsr & MSR_IP) != 0 ? 0xfff00000 : 0) | vector;
	fDozing = false;
}


bool
PpcCpu::RaiseProgram(u32 reason)
{
	BB_LOG(kLogCpu, kLogDebug, "cpu%d: program exception %08x at %08x",
		fConfig.index, reason, cia);
	_EnterException(EXC_PROGRAM, cia, reason);
	return false;
}


bool
PpcCpu::RaiseFpUnavailable()
{
	_EnterException(EXC_FP_UNAVAILABLE, cia, 0);
	return false;
}


bool
PpcCpu::RaiseSyscall()
{
	_EnterException(EXC_SYSCALL, cia + 4, 0);
	return false;
}


bool
PpcCpu::RaiseAlignment(u32 ea, u32 instruction)
{
	u32 dsisr;
	u32 op = instruction >> 26;
	if (op == 31) {
		// register indirect with index addressing
		dsisr = (((instruction >> 1) & 3) << 15)
			| (((instruction >> 6) & 1) << 14)
			| (((instruction >> 7) & 0xf) << 10);
	} else {
		dsisr = (((instruction >> 26) & 1) << 14)
			| (((instruction >> 27) & 0xf) << 10);
	}
	dsisr |= ((instruction >> 21) & 0x1f) << 5;
	dsisr |= (instruction >> 16) & 0x1f;
	fDsisr = dsisr;
	fDar = ea;
	BB_LOG(kLogCpu, kLogDebug, "cpu%d: alignment exception ea %08x at %08x",
		fConfig.index, ea, cia);
	_EnterException(EXC_ALIGNMENT, cia, 0);
	return false;
}


bool
PpcCpu::RaiseDsi(u32 ea, u32 dsisr)
{
	fDsisr = dsisr;
	fDar = ea;
	BB_LOG(kLogMmu, kLogDebug, "cpu%d: DSI ea %08x dsisr %08x at %08x",
		fConfig.index, ea, dsisr, cia);
	_EnterException(EXC_DSI, cia, 0);
	return false;
}


bool
PpcCpu::RaiseIsi(u32 srr1Bits)
{
	BB_LOG(kLogMmu, kLogDebug, "cpu%d: ISI %08x at %08x", fConfig.index,
		srr1Bits, cia);
	_EnterException(EXC_ISI, cia, srr1Bits);
	return false;
}


static inline u32
PtegAddress(u32 sdr1, u32 hash)
{
	u32 htaborg = sdr1 & 0xffff0000;
	u32 htabmask = sdr1 & 0x1ff;
	u32 high = ((hash >> 10) & 0x1ff) & htabmask;
	return (htaborg & 0xfe000000)
		| ((((htaborg >> 16) & 0x1ff) | high) << 16)
		| ((hash & 0x3ff) << 6);
}


void
PpcCpu::_RaiseTlbMiss(int type, u32 ea, u32 vsid, u32 key, int set)
{
	u32 hash = ((vsid & 0x7ffff) ^ ((ea >> 12) & 0xffff)) & 0x7ffff;
	u32 cmp = 0x80000000 | ((vsid & 0xffffff) << 7) | ((ea >> 22) & 0x3f);
	fHash1 = PtegAddress(fSdr1, hash);
	fHash2 = PtegAddress(fSdr1, ~hash & 0x7ffff);
	u32 srr1 = (fCr & 0xf0000000) | (key << 19);
	u32 vector;
	if (type == kFetch) {
		fImiss = ea;
		fIcmp = cmp;
		srr1 |= 1u << 18;
		srr1 |= (u32)fItlbNextWay[set] << 17;
		vector = EXC_ITLB_MISS;
	} else {
		fDmiss = ea;
		fDcmp = cmp;
		srr1 |= (u32)fDtlbNextWay[set] << 17;
		if (type == kWrite) {
			srr1 |= 1u << 16;
			vector = EXC_DTLB_STORE_MISS;
		} else
			vector = EXC_DTLB_LOAD_MISS;
	}
	BB_LOG(kLogMmu, kLogTrace, "cpu%d: TLB miss %x ea %08x at %08x",
		fConfig.index, vector, ea, cia);
	_EnterException(vector, cia, srr1, true);
}


// #pragma mark - MSR and SPRs


void
PpcCpu::SetMsr(u32 value)
{
	u32 old = fMsr;
	fMsr = value;
	u32 changed = old ^ value;
	if ((changed & MSR_TGPR) != 0) {
		for (int i = 0; i < 4; i++)
			std::swap(gpr[i], fTgpr[i]);
	}
	if ((changed & (MSR_IR | MSR_DR | MSR_PR)) != 0)
		_UpdateModeTables();
	if ((changed & MSR_LE) != 0 && (value & MSR_LE) != 0) {
		BB_LOG(kLogCpu, kLogWarning,
			"cpu%d: little endian mode is not supported (pc %08x)",
			fConfig.index, cia);
	}
	if ((value & MSR_POW) != 0
		&& (fHid0 & (HID0_DOZE | HID0_NAP | HID0_SLEEP)) != 0) {
		fDozing = true;
		RequestExit();
	}
	if ((value & MSR_EE) != 0 && (old & MSR_EE) == 0
		&& (fIntLine || fDecPending || fSmiLine))
		RequestExit();
	if ((value & (MSR_SE | MSR_BE)) != 0)
		RequestExit();
}


u32
PpcCpu::GetXer() const
{
	return (fXerSo ? 0x80000000u : 0) | (fXerOv ? 0x40000000u : 0)
		| (fXerCa ? 0x20000000u : 0) | fXerOther | (fXerBc & 0x7f);
}


void
PpcCpu::SetXer(u32 value)
{
	fXerSo = (value & 0x80000000) != 0;
	fXerOv = (value & 0x40000000) != 0;
	fXerCa = (value & 0x20000000) != 0;
	fXerBc = value & 0x7f;
	fXerOther = value & 0x1ff3ff80;
}


void
PpcCpu::SetFpscr(u32 value)
{
	fFpscr = value;
	PpcOps::UpdateFpscrSummary(*this);
#ifdef BEBOX_HAVE_MXCSR
	PpcOps::SetHostRounding(fFpscr);
#endif
}


u64
PpcCpu::GetTimebase() const
{
	return (u64)((s64)_TimebaseTicks() + fTbOffset);
}


void
PpcCpu::SetTimebase(u64 value)
{
	fTbOffset = (s64)(value - _TimebaseTicks());
}


u32
PpcCpu::GetDec() const
{
	return fDecBaseValue - (u32)(_TimebaseTicks() - fDecBaseTb);
}


void
PpcCpu::SetDec(u32 value)
{
	_ScheduleDecrementer(value);
	_UpdateStopAt();
}


void
PpcCpu::_ScheduleDecrementer(u32 value)
{
	fDecBaseTb = _TimebaseTicks();
	fDecBaseValue = value;
	fDecExpire = (fDecBaseTb + (u64)value + 1) * fConfig.ticksPerTimebase;
}


void
PpcCpu::SetSr(int index, u32 value)
{
	fSr[index & 15] = value;
	_FlushSoftTlb();
}


void
PpcCpu::_DecodeBat(Bat& bat)
{
	u32 bl = (bat.upper >> 2) & 0x7ff;
	bat.mask = (bl << 17) | 0x1ffff;
	bat.bepi = bat.upper & ~bat.mask;
	bat.brpn = bat.lower & ~bat.mask;
	bat.vs = (bat.upper & 2) != 0;
	bat.vp = (bat.upper & 1) != 0;
}


bool
PpcCpu::ReadSpr(int spr, u32& value) const
{
	switch (spr) {
		case SPR_XER: value = GetXer(); return true;
		case SPR_LR: value = fLr; return true;
		case SPR_CTR: value = fCtr; return true;
		case SPR_DSISR: value = fDsisr; return true;
		case SPR_DAR: value = fDar; return true;
		case SPR_DEC: value = GetDec(); return true;
		case SPR_SDR1: value = fSdr1; return true;
		case SPR_SRR0: value = fSrr0; return true;
		case SPR_SRR1: value = fSrr1; return true;
		case SPR_TBL_READ: value = (u32)GetTimebase(); return true;
		case SPR_TBU_READ: value = (u32)(GetTimebase() >> 32); return true;
		// Architecturally write-only, but the 603 reads the timebase
		// through them in supervisor mode, and BeOS's kernel does so.
		case SPR_TBL_WRITE: value = (u32)GetTimebase(); return true;
		case SPR_TBU_WRITE: value = (u32)(GetTimebase() >> 32); return true;
		case SPR_SPRG0: case SPR_SPRG1: case SPR_SPRG2: case SPR_SPRG3:
			value = fSprg[spr - SPR_SPRG0];
			return true;
		case SPR_EAR: value = fEar; return true;
		case SPR_PVR: value = fPvr; return true;
		case SPR_DMISS: value = fDmiss; return true;
		case SPR_DCMP: value = fDcmp; return true;
		case SPR_HASH1: value = fHash1; return true;
		case SPR_HASH2: value = fHash2; return true;
		case SPR_IMISS: value = fImiss; return true;
		case SPR_ICMP: value = fIcmp; return true;
		case SPR_RPA: value = fRpa; return true;
		case SPR_HID0: value = fHid0; return true;
		case SPR_HID1:
			if (fConfig.model != kPpc603e)
				return false;
			value = fHid1;
			return true;
		case SPR_IABR: value = fIabr; return true;
	}
	if (spr >= SPR_IBAT0U && spr < SPR_IBAT0U + 8) {
		const Bat& bat = fIbat[(spr - SPR_IBAT0U) / 2];
		value = (spr & 1) != 0 ? bat.lower : bat.upper;
		return true;
	}
	if (spr >= SPR_DBAT0U && spr < SPR_DBAT0U + 8) {
		const Bat& bat = fDbat[(spr - SPR_DBAT0U) / 2];
		value = (spr & 1) != 0 ? bat.lower : bat.upper;
		return true;
	}
	return false;
}


bool
PpcCpu::WriteSpr(int spr, u32 value)
{
	switch (spr) {
		case SPR_XER: SetXer(value); return true;
		case SPR_LR: fLr = value; return true;
		case SPR_CTR: fCtr = value; return true;
		case SPR_DSISR: fDsisr = value; return true;
		case SPR_DAR: fDar = value; return true;
		case SPR_DEC: SetDec(value); return true;
		case SPR_SDR1: fSdr1 = value; return true;
		case SPR_SRR0: fSrr0 = value; return true;
		case SPR_SRR1: fSrr1 = value; return true;
		case SPR_SPRG0: case SPR_SPRG1: case SPR_SPRG2: case SPR_SPRG3:
			fSprg[spr - SPR_SPRG0] = value;
			return true;
		case SPR_EAR: fEar = value; return true;
		case SPR_TBL_WRITE:
			SetTimebase((GetTimebase() & 0xffffffff00000000ull) | value);
			return true;
		case SPR_TBU_WRITE:
			SetTimebase((GetTimebase() & 0xffffffffull) | ((u64)value << 32));
			return true;
		case SPR_PVR:
			return true;	// read only
		case SPR_DMISS: case SPR_HASH1: case SPR_HASH2: case SPR_IMISS:
			return true;	// read only
		case SPR_DCMP: fDcmp = value; return true;
		case SPR_ICMP: fIcmp = value; return true;
		case SPR_RPA: fRpa = value; return true;
		case SPR_HID0:
			// The cache flash invalidate bits do not stick.
			fHid0 = value & ~(HID0_ICFI | HID0_DCFI);
			if ((fMsr & MSR_POW) != 0
				&& (fHid0 & (HID0_DOZE | HID0_NAP | HID0_SLEEP)) != 0) {
				fDozing = true;
				RequestExit();
			}
			return true;
		case SPR_HID1:
			return fConfig.model == kPpc603e;	// read only PLL config
		case SPR_IABR:
			fIabr = value;
			RequestExit();
			return true;
	}
	if (spr >= SPR_IBAT0U && spr < SPR_IBAT0U + 8) {
		Bat& bat = fIbat[(spr - SPR_IBAT0U) / 2];
		((spr & 1) != 0 ? bat.lower : bat.upper) = value;
		_DecodeBat(bat);
		_FlushSoftTlb();
		return true;
	}
	if (spr >= SPR_DBAT0U && spr < SPR_DBAT0U + 8) {
		Bat& bat = fDbat[(spr - SPR_DBAT0U) / 2];
		((spr & 1) != 0 ? bat.lower : bat.upper) = value;
		_DecodeBat(bat);
		_FlushSoftTlb();
		return true;
	}
	return false;
}


void
PpcCpu::ReturnFromInterrupt()
{
	// MSR[16-23, 25-27, 30-31] come from SRR1; the 603 also clears
	// MSR[TGPR] (it is not saved in SRR1).
	u32 msr = (fMsr & 0xffff0000 & ~MSR_TGPR) | (fSrr1 & 0x0000ff73);
	pc = fSrr0 & ~3u;
	SetMsr(msr);
}


void
PpcCpu::IdleUntilInterrupt()
{
	if ((fMsr & MSR_EE) == 0)
		return;
	fDozing = true;
	RequestExit();
}


void
PpcCpu::SetDebugHook(PpcDebugHook* hook)
{
	fDebugHook = hook;
	RequestExit();
}


void
PpcCpu::SetTraceHook(PpcTraceHook* hook)
{
	fTraceHook = hook;
	RequestExit();
}


void
PpcCpu::BusMapChanged()
{
	// Another CPU's thread must not touch the caches while this CPU uses
	// them: it asks.
	if (_IsForeignThread())
		_Request(kRequestFlush);
	else
		_FlushSoftTlb();
}


// #pragma mark - memory management


void
PpcCpu::_FlushSoftTlb()
{
	if (++fSoftGen >= 0xfff) {
		memset(fSoftRead, 0xff, sizeof(fSoftRead));
		memset(fSoftWrite, 0xff, sizeof(fSoftWrite));
		memset(fSoftFetch, 0xff, sizeof(fSoftFetch));
		fSoftGen = 0;
	}
	fFetchTag = kInvalidTag;
}


void
PpcCpu::_UpdateModeTables()
{
	int dataMode = (fMsr & MSR_DR) == 0 ? 0 : (fMsr & MSR_PR) != 0 ? 2 : 1;
	int fetchMode = (fMsr & MSR_IR) == 0 ? 0 : (fMsr & MSR_PR) != 0 ? 2 : 1;
	fCurRead = fSoftRead[dataMode];
	fCurWrite = fSoftWrite[dataMode];
	fCurFetch = fSoftFetch[fetchMode];
	fFetchTag = kInvalidTag;
}


bool
PpcCpu::_MatchBat(const Bat* bats, u32 ea, u32& pa, u32& wimg, int& pp) const
{
	bool user = (fMsr & MSR_PR) != 0;
	for (int i = 0; i < 4; i++) {
		const Bat& bat = bats[i];
		if (!(user ? bat.vp : bat.vs))
			continue;
		if ((ea & ~bat.mask) != bat.bepi)
			continue;
		pa = bat.brpn | (ea & bat.mask);
		wimg = (bat.lower >> 3) & 0xf;
		pp = bat.lower & 3;
		return true;
	}
	return false;
}


bool
PpcCpu::_Translate(u32 ea, AccessType type, u32& pa, u32& wimg,
	bool& viaPageTable, bool debug)
{
	bool instruction = type == kFetch;
	viaPageTable = false;
	if ((fMsr & (instruction ? MSR_IR : MSR_DR)) == 0) {
		pa = ea;
		wimg = instruction ? 0x1 : 0x3;
		return true;
	}

	int pp;
	if (_MatchBat(instruction ? fIbat : fDbat, ea, pa, wimg, pp)) {
		if (pp == 0 || (type == kWrite && (pp & 1) != 0)) {
			if (debug)
				return false;
			if (instruction)
				return RaiseIsi(0x08000000);
			return RaiseDsi(ea, 0x08000000
				| (type == kWrite ? 0x02000000 : 0));
		}
		return true;
	}

	u32 sr = fSr[ea >> 28];
	if ((sr & 0x80000000) != 0) {
		// direct-store segment
		if (debug)
			return false;
		if (instruction)
			return RaiseIsi(0x10000000);
		return RaiseDsi(ea, 0x80000000 | (type == kWrite ? 0x02000000 : 0));
	}
	if (instruction && (sr & 0x10000000) != 0) {
		if (debug)
			return false;
		return RaiseIsi(0x10000000);	// no-execute segment
	}

	u32 key = (fMsr & MSR_PR) != 0 ? (sr >> 29) & 1 : (sr >> 30) & 1;
	u32 vsid = sr & 0xffffff;
	u32 cmp = 0x80000000 | (vsid << 7) | ((ea >> 22) & 0x3f);
	int set = (ea >> 12) & 31;
	TlbEntry* tlb = instruction ? fItlb[set] : fDtlb[set];
	for (int way = 0; way < 2; way++) {
		if ((tlb[way].cmp & ~0x40u) != cmp
			|| tlb[way].eaTag != (ea & kTlbEaTagMask))
			continue;
		u32 rpa = tlb[way].rpa;
		pp = rpa & 3;
		bool canRead = key == 0 || pp != 0;
		bool canWrite = key == 0 ? pp != 3 : pp == 2;
		if (!debug)
			(instruction ? fItlbNextWay : fDtlbNextWay)[set] = way ^ 1;
		if (!(type == kWrite ? canWrite : canRead)) {
			if (debug)
				return false;
			if (instruction)
				return RaiseIsi(0x08000000);
			return RaiseDsi(ea, 0x08000000
				| (type == kWrite ? 0x02000000 : 0));
		}
		if (type == kWrite && (rpa & 0x80) == 0) {
			// Changed bit not set yet: the miss handler sets it and
			// reloads this very entry, so SRR1[WAY] names the way that
			// hit (BeOS's handler relies on it).
			if (debug)
				return false;
			fDtlbNextWay[set] = way;
			_RaiseTlbMiss(type, ea, vsid, key, set);
			return false;
		}
		pa = (rpa & 0xfffff000) | (ea & 0xfff);
		wimg = (rpa >> 3) & 0xf;
		viaPageTable = true;
		return true;
	}

	if (debug) {
		// Search the hashed page table like the OS's miss handler would.
		u32 hash = ((vsid & 0x7ffff) ^ ((ea >> 12) & 0xffff)) & 0x7ffff;
		for (int h = 0; h < 2; h++) {
			u32 pteg = PtegAddress(fSdr1, h == 0 ? hash : ~hash & 0x7ffff);
			for (int i = 0; i < 8; i++) {
				u32 word0 = fBus.Read32(pteg + i * 8, kInitiatorDma);
				if (word0 != (cmp | (h != 0 ? 0x40 : 0)))
					continue;
				u32 word1 = fBus.Read32(pteg + i * 8 + 4, kInitiatorDma);
				pa = (word1 & 0xfffff000) | (ea & 0xfff);
				wimg = (word1 >> 3) & 0xf;
				return true;
			}
		}
		return false;
	}
	_RaiseTlbMiss(type, ea, vsid, key, set);
	return false;
}


bool
PpcCpu::TranslateData(u32 ea, bool store, u32& pa, u32& wimg)
{
	bool viaPageTable;
	return _Translate(ea, store ? kWrite : kRead, pa, wimg, viaPageTable);
}


bool
PpcCpu::DebugTranslate(u32 ea, bool instruction, u32& pa)
{
	u32 wimg;
	bool viaPageTable;
	return _Translate(ea, instruction ? kFetch : kRead, pa, wimg,
		viaPageTable, true);
}


bool
PpcCpu::DebugRead32(u32 ea, u32& value)
{
	u32 pa;
	if (!DebugTranslate(ea, false, pa))
		return false;
	BusPage page = fBus.PageSnapshot(pa);
	if (page.read == nullptr || (pa & 0xfff) > 0xffc)
		return false;
	value = LoadBE32(page.read + (pa & 0xfff));
	return true;
}


void
PpcCpu::_TlbInvalidate(u32 ea)
{
	int set = (ea >> 12) & 31;
	fItlb[set][0].cmp = fItlb[set][1].cmp = 0;
	fDtlb[set][0].cmp = fDtlb[set][1].cmp = 0;
	_FlushSoftTlb();
}


void
PpcCpu::_TlbLoad(bool instruction, u32 ea)
{
	int set = (ea >> 12) & 31;
	int way = (fSrr1 >> 17) & 1;
	TlbEntry& entry = instruction ? fItlb[set][way] : fDtlb[set][way];
	entry.cmp = instruction ? fIcmp : fDcmp;
	entry.eaTag = ea & kTlbEaTagMask;
	entry.rpa = fRpa;
	(instruction ? fItlbNextWay : fDtlbNextWay)[set] = way ^ 1;
}


bool
PpcCpu::_AccessCrossesPage(u32 ea, int size, AccessType type, u32& pa1,
	u32& pa2)
{
	// The 603 splits a misaligned access that crosses a page boundary in
	// hardware, translation and all (BeOS's memset relies on it).
	// Both halves are translated before either is performed.
	u32 wimg;
	bool viaPageTable;
	if (!_Translate(ea, type, pa1, wimg, viaPageTable))
		return false;
	u32 second = (ea + size - 1) & ~0xfffu;
	if (!_Translate(second, type, pa2, wimg, viaPageTable))
		return false;
	return true;
}


u32
PpcCpu::LoadInstruction()
{
	u32 pa;
	u32 value = 0;
	if (DebugTranslate(cia, true, pa))
		value = fBus.Read32(pa, kInitiatorDma);
	return value;
}


bool
PpcCpu::_ReadSlow(u32 ea, int size, u64& value)
{
	u32 offset = ea & 0xfff;
	if (offset + size > 0x1000) {
		u32 pa1;
		u32 pa2;
		if (!_AccessCrossesPage(ea, size, kRead, pa1, pa2))
			return false;
		fBus.SetCurrentPc(fConfig.index, cia);
		u32 firstLength = 0x1000 - offset;
		value = 0;
		for (int i = 0; i < size; i++) {
			u32 address = (u32)i < firstLength ? pa1 + i
				: pa2 + (i - firstLength);
			value = (value << 8) | fBus.Read8(address, fConfig.index);
		}
		return true;
	}

	u32 pa;
	u32 wimg;
	bool viaPageTable;
	if (!_Translate(ea, kRead, pa, wimg, viaPageTable))
		return false;
	BusPage page = fBus.PageSnapshot(pa);
	if (page.read != nullptr) {
		SoftTlbEntry& entry = fCurRead[(ea >> 12) & (kSoftTlbSize - 1)];
		entry.tag = (ea & ~0xfffu) | fSoftGen;
		entry.pa = pa & ~0xfffu;
		entry.host = page.read;
		const u8* host = page.read + offset;
		switch (size) {
			case 1: value = *host; break;
			case 2: value = LoadBE16(host); break;
			case 4: value = LoadBE32(host); break;
			default: value = LoadBE64(host); break;
		}
		return true;
	}
	fBus.SetCurrentPc(fConfig.index, cia);
	switch (size) {
		case 1: value = fBus.Read8(pa, fConfig.index); break;
		case 2: value = fBus.Read16(pa, fConfig.index); break;
		case 4: value = fBus.Read32(pa, fConfig.index); break;
		default: value = fBus.Read64(pa, fConfig.index); break;
	}
	return true;
}


bool
PpcCpu::_WriteSlow(u32 ea, int size, u64 value)
{
	u32 offset = ea & 0xfff;
	if (offset + size > 0x1000) {
		u32 pa1;
		u32 pa2;
		if (!_AccessCrossesPage(ea, size, kWrite, pa1, pa2))
			return false;
		fBus.SetCurrentPc(fConfig.index, cia);
		u32 firstLength = 0x1000 - offset;
		for (int i = 0; i < size; i++) {
			u32 address = (u32)i < firstLength ? pa1 + i
				: pa2 + (i - firstLength);
			fBus.Write8(address, (u8)(value >> (8 * (size - 1 - i))),
				fConfig.index);
		}
		return true;
	}

	u32 pa;
	u32 wimg;
	bool viaPageTable;
	if (!_Translate(ea, kWrite, pa, wimg, viaPageTable))
		return false;
	BusPage page = fBus.PageSnapshot(pa);
	if (page.write != nullptr) {
		SoftTlbEntry& entry = fCurWrite[(ea >> 12) & (kSoftTlbSize - 1)];
		entry.tag = (ea & ~0xfffu) | fSoftGen;
		entry.pa = pa & ~0xfffu;
		entry.host = page.write;
		u8* host = page.write + offset;
		switch (size) {
			case 1: *host = (u8)value; break;
			case 2: StoreBE16(host, (u16)value); break;
			case 4: StoreBE32(host, (u32)value); break;
			default: StoreBE64(host, value); break;
		}
		fBus.SnoopStore(pa, size, fConfig.index);
		return true;
	}
	fBus.SetCurrentPc(fConfig.index, cia);
	switch (size) {
		case 1: fBus.Write8(pa, (u8)value, fConfig.index); break;
		case 2: fBus.Write16(pa, (u16)value, fConfig.index); break;
		case 4: fBus.Write32(pa, (u32)value, fConfig.index); break;
		default: fBus.Write64(pa, value, fConfig.index); break;
	}
	return true;
}

}	// namespace bebox

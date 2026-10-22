/*
 * BeBox emulator - PowerPC 603 / 603e processor.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * An interpreter of the 32-bit PowerPC architecture as implemented by the
 * Motorola/IBM 603 and 603e: user and supervisor instruction sets, the
 * floating point unit, BATs, segment registers and the 603's software
 * loaded TLBs (no hardware page table walk; misses vector to 0x1000,
 * 0x1100 and 0x1200 with the shadow registers GPR0-3 swapped in).
 *
 * Bit numbers in comments use the IBM convention (bit 0 = MSB).
 */
#ifndef BEBOX_PPC_CPU_H
#define BEBOX_PPC_CPU_H

#include "../Types.h"
#include "../bus/PhysBus.h"

#include <atomic>

namespace bebox {

enum : u32 {
	MSR_POW		= 1u << 18,
	MSR_TGPR	= 1u << 17,
	MSR_ILE		= 1u << 16,
	MSR_EE		= 1u << 15,
	MSR_PR		= 1u << 14,
	MSR_FP		= 1u << 13,
	MSR_ME		= 1u << 12,
	MSR_FE0		= 1u << 11,
	MSR_SE		= 1u << 10,
	MSR_BE		= 1u << 9,
	MSR_FE1		= 1u << 8,
	MSR_IP		= 1u << 6,
	MSR_IR		= 1u << 5,
	MSR_DR		= 1u << 4,
	MSR_RI		= 1u << 1,
	MSR_LE		= 1u << 0
};

enum : u32 {
	HID0_EMCP	= 1u << 31,
	HID0_DOZE	= 1u << 23,
	HID0_NAP	= 1u << 22,
	HID0_SLEEP	= 1u << 21,
	HID0_DPM	= 1u << 20,
	HID0_ICE	= 1u << 15,
	HID0_DCE	= 1u << 14,
	HID0_ILOCK	= 1u << 13,
	HID0_DLOCK	= 1u << 12,
	HID0_ICFI	= 1u << 11,
	HID0_DCFI	= 1u << 10
};

// Special purpose register numbers.
enum {
	SPR_XER = 1, SPR_LR = 8, SPR_CTR = 9, SPR_DSISR = 18, SPR_DAR = 19,
	SPR_DEC = 22, SPR_SDR1 = 25, SPR_SRR0 = 26, SPR_SRR1 = 27,
	SPR_TBL_READ = 268, SPR_TBU_READ = 269,
	SPR_SPRG0 = 272, SPR_SPRG1 = 273, SPR_SPRG2 = 274, SPR_SPRG3 = 275,
	SPR_EAR = 282, SPR_TBL_WRITE = 284, SPR_TBU_WRITE = 285, SPR_PVR = 287,
	SPR_IBAT0U = 528, SPR_DBAT0U = 536,
	SPR_DMISS = 976, SPR_DCMP = 977, SPR_HASH1 = 978, SPR_HASH2 = 979,
	SPR_IMISS = 980, SPR_ICMP = 981, SPR_RPA = 982,
	SPR_HID0 = 1008, SPR_HID1 = 1009, SPR_IABR = 1010
};

// Exception vector offsets.
enum {
	EXC_RESET = 0x100, EXC_MACHINE_CHECK = 0x200, EXC_DSI = 0x300,
	EXC_ISI = 0x400, EXC_EXTERNAL = 0x500, EXC_ALIGNMENT = 0x600,
	EXC_PROGRAM = 0x700, EXC_FP_UNAVAILABLE = 0x800, EXC_DECREMENTER = 0x900,
	EXC_SYSCALL = 0xc00, EXC_TRACE = 0xd00, EXC_ITLB_MISS = 0x1000,
	EXC_DTLB_LOAD_MISS = 0x1100, EXC_DTLB_STORE_MISS = 0x1200,
	EXC_BREAKPOINT = 0x1300, EXC_SMI = 0x1400
};

// SRR1 reason bits of program exceptions.
enum : u32 {
	PROGRAM_FP = 1u << 20,
	PROGRAM_ILLEGAL = 1u << 19,
	PROGRAM_PRIVILEGED = 1u << 18,
	PROGRAM_TRAP = 1u << 17
};

class PpcCpu;

// Hooks for the debugger / GDB stub. Called only while stepping is active.
class PpcDebugHook {
public:
	virtual ~PpcDebugHook() {}
	// Return true to stop before executing the instruction at cpu.pc.
	virtual bool ShouldStop(PpcCpu& cpu) = 0;
};

// Trace hook, called before every instruction while tracing is enabled.
class PpcTraceHook {
public:
	virtual ~PpcTraceHook() {}
	virtual void Instruction(PpcCpu& cpu, u32 pc, u32 instruction) = 0;
};

class PpcCpu : public BusMapListener {
public:
	enum Model {
		kPpc603,
		kPpc603e
	};

	struct Config {
		Model	model = kPpc603e;
		u32		pvr = 0x00060401;
		u32		hid1 = 0xa0000000;	// 603e: PLL_CFG pins
		int		index = 0;			// bus initiator number
		u32		ticksPerInstruction = 2;
		u32		ticksPerTimebase = 32;
	};

	// EA bits 4-14 tag a TLB entry besides the VSID: bits 15-19 select
	// the set and the API in ICMP/DCMP only covers bits 4-9.
	static const u32 kTlbEaTagMask = 0x0ffe0000;

	struct TlbEntry {
		u32 cmp;		// V | VSID | H | API, as loaded from ICMP/DCMP
		u32 rpa;		// RPN | R | C | WIMG | PP, as loaded from RPA
		u32 eaTag;		// EA & kTlbEaTagMask of the tlbld/tlbli operand
	};

	struct Bat {
		u32 upper;
		u32 lower;
		// decoded
		u32 mask;		// EA bits that pass through
		u32 bepi;
		u32 brpn;
		bool vs;
		bool vp;
	};

								PpcCpu(PhysBus& bus, const Config& config);
	virtual						~PpcCpu();

			const Config&		GetConfig() const { return fConfig; }
			int					Index() const { return fConfig.index; }

			// Power-on reset: registers to their reset values, execution at
			// 0xfff00100 with MSR[IP] set.
			void				HardReset();

			// Executes until the local time reaches 'until' (machine
			// ticks), the CPU is halted or RequestExit() is called.
			void				Run(u64 until);
			// Executes exactly one instruction (or takes one pending
			// exception). Used by debuggers and tests.
			void				Step();
			// Ends Run() after the current instruction (from any thread).
			void				RequestExit() { fStopAt.store(0); }

			u64					Now() const { return fNow; }
			// (Ignored from another CPU's thread in threaded mode: the
			// machine sets the time of a CPU between time slices.)
			void				SetNow(u64 now)
									{ if (!_IsForeignThread())
										fNow = now; }
			const u64*			NowPointer() const { return &fNow; }
			// The time an instruction takes (between time slices; the
			// machine varies it to set the processor's speed).
			void				SetTicksPerInstruction(u32 ticks)
									{ fConfig.ticksPerInstruction
										= ticks > 0 ? ticks : 1; }

			// Input signals. Any thread may set them; in threaded mode
			// what changes the CPU's state (reset, a bus map change) is
			// applied by the CPU's own thread before its next instruction.
			void				SetIntLine(bool asserted);
			void				SetSmiLine(bool asserted);
			void				SetHresetLine(bool asserted);
			void				SetSresetLine(bool asserted);
			void				SetTlbisyncLine(bool asserted);

			// True while the CPU does not execute instructions: held in
			// reset, or dozing without a pending exception it could take.
			bool				IsHalted() const
									{ return fHresetLine || fDozing
										|| fCheckstop; }
			bool				IsHeldInReset() const { return fHresetLine; }
			bool				IsCheckstopped() const { return fCheckstop; }
			// Time at which the decrementer next passes through zero.
			u64					DecrementerExpiry() const
									{ return fDecExpire; }

			// Register access for debuggers, tests, the boot loader.
			u32					GetMsr() const { return fMsr; }
			void				SetMsr(u32 value);
			u32					GetCr() const { return fCr; }
			void				SetCr(u32 value) { fCr = value; }
			u32					GetXer() const;
			void				SetXer(u32 value);
			u32					GetFpscr() const { return fFpscr; }
			void				SetFpscr(u32 value);
			u64					GetTimebase() const;
			void				SetTimebase(u64 value);
			u32					GetDec() const;
			void				SetDec(u32 value);
			// Reads an SPR without side effects or privilege checks.
			bool				ReadSpr(int spr, u32& value) const;
			bool				WriteSpr(int spr, u32 value);
			u32					GetSr(int index) const { return fSr[index]; }
			void				SetSr(int index, u32 value);
			const TlbEntry&		ITlb(int set, int way) const
									{ return fItlb[set][way]; }
			const TlbEntry&		DTlb(int set, int way) const
									{ return fDtlb[set][way]; }

			// Debugger memory access through the MMU without side effects
			// on the TLBs. Returns false if the address does not translate.
			bool				DebugTranslate(u32 ea, bool instruction,
									u32& pa);
			bool				DebugRead32(u32 ea, u32& value);

			void				SetDebugHook(PpcDebugHook* hook);
			void				SetTraceHook(PpcTraceHook* hook);

			u64					InstructionCount() const
									{ return fInstructions; }

			// Counts of the slow paths, for profiling (the fast paths
			// count nothing).
			struct Stats {
				// the host side cache of translations missed: read, write,
				// fetch (an instruction fetch from another page that it
				// had: fetchPages)
				u64				softMisses[3] = { 0, 0, 0 };
				u64				fetchPages = 0;
				u64				flushes = 0;	// of that cache
				u64				deviceAccesses = 0;
				u64				crossPage = 0;	// accesses across pages
				u64				exceptions[32] = {};	// by vector / 0x100
			};
			const Stats&		GetStats() const { return fStats; }

	// BusMapListener
	virtual	void				BusMapChanged();

public:
	// Architected state. Public so instruction handlers, the debugger and
	// tests can reach it directly.
			u32					gpr[32];
			u64					fpr[32];		// raw IEEE double bits
			u32					pc;				// next instruction
			u32					cia;			// current instruction

private:
	friend struct PpcOps;
	friend class PpcCpuTest;

	enum {
		kRequestReset = 1,		// fResetWanted changed
		kRequestFlush = 2		// the bus map changed
	};

			// Whether the calling thread is another CPU's (threaded mode).
			bool				_IsForeignThread() const
									{ return fBus.IsThreaded()
										&& sCurrent != nullptr
										&& sCurrent != this; }
			void				_Request(u32 request);
			void				_ApplyRequests();

	enum AccessType {
		kRead = 0,
		kWrite = 1,
		kFetch = 2
	};

	struct SoftTlbEntry {
		u32 tag;		// EA page | generation; 0xffffffff = invalid
		u32 pa;			// physical page address
		u8* host;		// host pointer to the page
	};

	static const int kSoftTlbBits = 8;
	static const int kSoftTlbSize = 1 << kSoftTlbBits;
	static const u32 kInvalidTag = 0xffffffff;

	// Execution
			void				_Execute(u64 until, bool singleStep);
			void				_CheckInterrupts();
			bool				_FetchSlow(u32 address, u32& instruction);
			void				_UpdateStopAt();

	// Exceptions. All of them redirect pc and return false so handlers
	// can 'return c.Raise...()'.
			void				_EnterException(u32 vector, u32 srr0,
									u32 srr1High, bool tgpr = false);
public:
			bool				RaiseProgram(u32 reason);
			bool				RaiseFpUnavailable();
			bool				RaiseSyscall();
			bool				RaiseAlignment(u32 ea, u32 instruction);
			bool				RaiseDsi(u32 ea, u32 dsisr);
			bool				RaiseIsi(u32 srr1Bits);
private:
			void				_RaiseTlbMiss(int type, u32 ea, u32 vsid,
									u32 key, int set);

	// Memory management
			bool				_Translate(u32 ea, AccessType type, u32& pa,
									u32& wimg, bool& viaPageTable,
									bool debug = false);
			bool				_MatchBat(const Bat* bats, u32 ea,
									u32& pa, u32& wimg, int& pp) const;
			void				_DecodeBat(Bat& bat);
			void				_FlushSoftTlb();
			void				_UpdateModeTables();
			void				_TlbInvalidate(u32 ea);
			void				_TlbLoad(bool instruction, u32 ea);

	// Data access
public:
			bool				Read8(u32 ea, u8& value);
			bool				Read16(u32 ea, u16& value);
			bool				Read32(u32 ea, u32& value);
			bool				Read64(u32 ea, u64& value);
			bool				Write8(u32 ea, u8 value);
			bool				Write16(u32 ea, u16 value);
			bool				Write32(u32 ea, u32 value);
			bool				Write64(u32 ea, u64 value);
			// Translates for cache instructions (dcbz, dcbf, ...).
			bool				TranslateData(u32 ea, bool store, u32& pa,
									u32& wimg);
			// Reads the current instruction (cia) again, for exceptions
			// that report instruction fields.
			u32					LoadInstruction();
			u64					ExceptionCount() const
									{ return fExceptions; }

			// Services for the instruction handlers.
			PhysBus&			Bus() { return fBus; }
			bool				TlbisyncAsserted() const
									{ return fTlbisyncLine; }
			void				TlbInvalidate(u32 ea) { _TlbInvalidate(ea); }
			void				TlbLoad(bool instruction, u32 ea)
									{ _TlbLoad(instruction, ea); }
			// rfi: MSR from SRR1, continue at SRR0.
			void				ReturnFromInterrupt();
			// A branch to itself with external interrupts enabled (the
			// idle loop of BeOS R3 and later): nothing changes until an
			// exception, so the CPU dozes until then instead of spinning.
			void				IdleUntilInterrupt();
private:
			bool				_ReadSlow(u32 ea, int size, u64& value);
			bool				_WriteSlow(u32 ea, int size, u64 value);
			bool				_AccessCrossesPage(u32 ea, int size,
									AccessType type, u32& pa1, u32& pa2);

	// Timebase and decrementer
			u64					_TimebaseTicks() const
									{ return fNow / fConfig.ticksPerTimebase; }
			void				_ScheduleDecrementer(u32 value);

public:
	// Registers used by the instruction handlers.
			u32					fCr;
			u32					fLr;
			u32					fCtr;
			bool				fXerSo;
			bool				fXerOv;
			bool				fXerCa;
			u32					fXerBc;
			u32					fXerOther;		// reserved bits, kept as written
			u32					fFpscr;
private:
			u32					fMsr;
			u32					fSrr0;
			u32					fSrr1;
			u32					fSprg[4];
			u32					fDar;
			u32					fDsisr;
			u32					fSdr1;
			u32					fEar;
			u32					fPvr;
			u32					fHid0;
			u32					fHid1;
			u32					fIabr;
			u32					fDmiss;
			u32					fDcmp;
			u32					fHash1;
			u32					fHash2;
			u32					fImiss;
			u32					fIcmp;
			u32					fRpa;
			u32					fTgpr[4];
			u32					fSr[16];
			Bat					fIbat[4];
			Bat					fDbat[4];
			TlbEntry			fItlb[32][2];
			TlbEntry			fDtlb[32][2];
			u8					fItlbNextWay[32];
			u8					fDtlbNextWay[32];

			// Time
			u64					fNow;
			std::atomic<u64>	fStopAt;
			u64					fRunUntil;
			s64					fTbOffset;		// timebase = ticks + offset
			u64					fDecBaseTb;		// timebase at DEC write
			u32					fDecBaseValue;
			u64					fDecExpire;		// machine time of 0 -> -1

			// Signals and pending exceptions
			std::atomic<bool>	fIntLine;
			std::atomic<bool>	fSmiLine;
			std::atomic<bool>	fHresetLine;
			std::atomic<bool>	fResetWanted;	// HRESET as last set
			std::atomic<bool>	fSresetPending;
			std::atomic<bool>	fTlbisyncLine;
			std::atomic<u32>	fRequests;
	// The CPU running on the calling thread (in Run()).
	static thread_local PpcCpu* sCurrent;
			bool				fDecPending;
			bool				fDozing;
			bool				fCheckstop;

			// Software TLB caches for the three translation modes
			// (0: real, 1: translated supervisor, 2: translated user).
			SoftTlbEntry		fSoftRead[3][kSoftTlbSize];
			SoftTlbEntry		fSoftWrite[3][kSoftTlbSize];
			SoftTlbEntry		fSoftFetch[3][kSoftTlbSize];
			SoftTlbEntry*		fCurRead;
			SoftTlbEntry*		fCurWrite;
			SoftTlbEntry*		fCurFetch;
			u32					fSoftGen;
			u32					fFetchTag;
			u8*					fFetchHost;

			PhysBus&			fBus;
			Config				fConfig;
			PpcDebugHook*		fDebugHook;
			PpcTraceHook*		fTraceHook;
			u64					fInstructions;
			u64					fExceptions;
			Stats				fStats;
			u32					fHostMxcsr;
};


// Instruction handler table entry.
typedef void (*PpcHandler)(PpcCpu& cpu, u32 instruction);

// Fast path data accesses.

inline bool
PpcCpu::Read8(u32 ea, u8& value)
{
	const SoftTlbEntry& e = fCurRead[(ea >> 12) & (kSoftTlbSize - 1)];
	if (e.tag == ((ea & ~0xfffu) | fSoftGen)) {
		value = e.host[ea & 0xfff];
		return true;
	}
	u64 v;
	if (!_ReadSlow(ea, 1, v))
		return false;
	value = (u8)v;
	return true;
}


inline bool
PpcCpu::Read16(u32 ea, u16& value)
{
	const SoftTlbEntry& e = fCurRead[(ea >> 12) & (kSoftTlbSize - 1)];
	if (e.tag == ((ea & ~0xfffu) | fSoftGen) && (ea & 0xfff) <= 0xffe) {
		value = LoadBE16(e.host + (ea & 0xfff));
		return true;
	}
	u64 v;
	if (!_ReadSlow(ea, 2, v))
		return false;
	value = (u16)v;
	return true;
}


inline bool
PpcCpu::Read32(u32 ea, u32& value)
{
	const SoftTlbEntry& e = fCurRead[(ea >> 12) & (kSoftTlbSize - 1)];
	if (e.tag == ((ea & ~0xfffu) | fSoftGen) && (ea & 0xfff) <= 0xffc) {
		value = LoadBE32(e.host + (ea & 0xfff));
		return true;
	}
	u64 v;
	if (!_ReadSlow(ea, 4, v))
		return false;
	value = (u32)v;
	return true;
}


inline bool
PpcCpu::Read64(u32 ea, u64& value)
{
	const SoftTlbEntry& e = fCurRead[(ea >> 12) & (kSoftTlbSize - 1)];
	if (e.tag == ((ea & ~0xfffu) | fSoftGen) && (ea & 0xfff) <= 0xff8) {
		value = LoadBE64(e.host + (ea & 0xfff));
		return true;
	}
	return _ReadSlow(ea, 8, value);
}


inline bool
PpcCpu::Write8(u32 ea, u8 value)
{
	const SoftTlbEntry& e = fCurWrite[(ea >> 12) & (kSoftTlbSize - 1)];
	if (e.tag == ((ea & ~0xfffu) | fSoftGen)) {
		e.host[ea & 0xfff] = value;
		fBus.SnoopStore(e.pa | (ea & 0xfff), 1, fConfig.index);
		return true;
	}
	return _WriteSlow(ea, 1, value);
}


inline bool
PpcCpu::Write16(u32 ea, u16 value)
{
	const SoftTlbEntry& e = fCurWrite[(ea >> 12) & (kSoftTlbSize - 1)];
	if (e.tag == ((ea & ~0xfffu) | fSoftGen) && (ea & 0xfff) <= 0xffe) {
		StoreBE16(e.host + (ea & 0xfff), value);
		fBus.SnoopStore(e.pa | (ea & 0xfff), 2, fConfig.index);
		return true;
	}
	return _WriteSlow(ea, 2, value);
}


inline bool
PpcCpu::Write32(u32 ea, u32 value)
{
	const SoftTlbEntry& e = fCurWrite[(ea >> 12) & (kSoftTlbSize - 1)];
	if (e.tag == ((ea & ~0xfffu) | fSoftGen) && (ea & 0xfff) <= 0xffc) {
		StoreBE32(e.host + (ea & 0xfff), value);
		fBus.SnoopStore(e.pa | (ea & 0xfff), 4, fConfig.index);
		return true;
	}
	return _WriteSlow(ea, 4, value);
}


inline bool
PpcCpu::Write64(u32 ea, u64 value)
{
	const SoftTlbEntry& e = fCurWrite[(ea >> 12) & (kSoftTlbSize - 1)];
	if (e.tag == ((ea & ~0xfffu) | fSoftGen) && (ea & 0xfff) <= 0xff8) {
		StoreBE64(e.host + (ea & 0xfff), value);
		fBus.SnoopStore(e.pa | (ea & 0xfff), 8, fConfig.index);
		return true;
	}
	return _WriteSlow(ea, 8, value);
}

}	// namespace bebox

#endif	// BEBOX_PPC_CPU_H

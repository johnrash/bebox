/*
 * BeBox emulator - AT&T ATT92010 "Hobbit" processor (interpreter).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Written from the ATT92010 Hobbit Microprocessor Programmer's Reference
 * Manual (January 1993). The Hobbit is a CRISP: memory to memory, no
 * general registers; instructions of 1, 3 or 5 parcels (16 bit) address
 * their operands absolutely, relative to the current stack pointer (CSP:
 * the SP or the interrupt stack pointer ISP) or indirectly through a word
 * on the stack. "R4" (CSP + 4) is the accumulator.
 *
 * The stack cache holds the words from the SP up to the MSP (at most 256
 * bytes) on the chip: data accesses there need no address translation, and
 * the kernel relies on that (it switches the address space with the top of
 * the old stack still cached). The stack instructions move data between it
 * and memory as the manual defines (ENTER writes back, CATCH fills, RETURN
 * and POPN discard). The prefetch buffer and the instruction caches are
 * not modelled: instructions are fetched from memory every time.
 *
 * Time: the CPU runs on its own clock (machine ticks); a 25 MHz clock
 * cycle is Config::ticksPerCycle ticks. The timers count clock cycles.
 *
 * Stack offset operands are relative to the shadow register (SHAD), not
 * to the SP or ISP: the kernel moves the ISP and then still addresses its
 * frame (and the manual has software set SHAD after it changes the stack
 * pointer selection in the PSW). Stack instructions and events update SP
 * or ISP and SHAD together; direct writes of SP and ISP leave SHAD alone.
 *
 * Bus errors and MMU faults abort the instruction (a C++ exception inside
 * the core) and start the exception sequence.
 */
#ifndef BEBOX_HOBBIT_CPU_H
#define BEBOX_HOBBIT_CPU_H

#include "../Types.h"
#include "../bus/PhysBus.h"

#include <cstdio>
#include <functional>
#include <set>
#include <string>

namespace bebox {

class HobbitCpu : public BusMapListener {
public:
	struct Config {
		int		index = 0;				// bus initiator number
		u32		ticksPerCycle = 4;		// machine ticks per clock cycle
		u32		ticksPerInstruction = 4;
		u32		id = 0x1000003b;		// ATT92010, mask 2
	};

	// Control registers (the CPU prefix register numbers).
	enum {
		kRegMsp = 1,
		kRegIsp = 2,
		kRegSp = 3,
		kRegConfig = 4,
		kRegPsw = 5,
		kRegShad = 6,
		kRegVb = 7,
		kRegStb = 8,
		kRegFault = 9,
		kRegId = 10,
		kRegTimer1 = 11,
		kRegTimer2 = 12
	};

	// Program status word
	enum {
		kPswFlag = 1 << 4,
		kPswCarry = 1 << 5,
		kPswOverflow = 1 << 6,
		kPswTraceInstruction = 1 << 7,
		kPswTraceBlock = 1 << 8,
		kPswUseSp = 1 << 9,			// CSP is the SP (else the ISP)
		kPswUser = 1 << 10,
		kPswEnterGuard = 1 << 11,
		kPswIplShift = 12,
		kPswIplMask = 7 << 12,
		kPswUserLittle = 1 << 15,
		kPswVirtual = 1 << 16,
		kPswValid = 0x1fff0
	};

	// Configuration register
	enum {
		kConfigKernelLittle = 1 << 16,
		kConfigPcExtension = 1 << 17,
		kConfigStackCache = 1 << 18,
		kConfigTimer1Instructions = 1 << 22,
		kConfigTimer1KernelOnly = 1 << 23,
		kConfigTimer1Interrupt = 1 << 24,
		kConfigTimer2EventShift = 25,
		kConfigTimer2KernelOnly = 1 << 30,
		kConfigTimer2Interrupt = 1u << 31,
		kConfigValid = 0xffff0000
	};

	// Exception identifiers
	enum {
		kExZeroDivide = 1,
		kExTrace = 2,
		kExIllegal = 3,
		kExAlignment = 4,
		kExPrivilege = 5,
		kExUnimplementedRegister = 6,
		kExFetchFault = 7,
		kExReadFault = 8,
		kExWriteFault = 9,
		kExBusError = 10,
		kExTableWalkBusError = 11
	};

	// Called before each traced instruction.
	typedef std::function<void(HobbitCpu& cpu, u32 pc)> TraceHook;
	// Whether the idle loop may wait for the next event.
	typedef std::function<bool(HobbitCpu& cpu)> IdleCheck;

								HobbitCpu(PhysBus& bus, const Config& config);
	virtual						~HobbitCpu();

			int					Index() const { return fConfig.index; }

			// The reset sequence (the HRESET pin, or the board's processor
			// reset register releasing the CPU).
			void				Reset();
			// Held in reset by the board (does not run).
			void				SetHeldInReset(bool held);
			bool				IsHeldInReset() const { return fHeldInReset; }
			// Interrupt request lines IL[2:0]: 0 = NMI (edge triggered),
			// 1-6 = levels, 7 = none.
			void				SetInterruptLevel(int level);

			void				Run(u64 until);
			u64					Now() const { return fNow; }
			const u64*			NowPointer() const { return &fNow; }
			void				SetNow(u64 now) { fNow = now; }
			u64					InstructionCount() const
									{ return fInstructions; }

			// State, for the boot loader and debugging.
			u32					Pc() const { return fPc; }
			void				SetPc(u32 pc) { fPc = pc & ~1u; }
			u32					Register(int number);
			void				SetRegister(int number, u32 value);
			u32					Psw() const { return fPsw; }
			// The base of stack offset addresses (and of the accumulator):
			// the shadow register, a copy of the current stack pointer
			// (CSP) that stack instructions keep up to date and that
			// software sets after it changes the stack pointers directly.
			u32					Csp() const { return fShad; }
			// The current stack pointer register: SP or ISP, as the PSW
			// selects.
			u32					StackRegister() const
									{ return (fPsw & kPswUseSp) ? fSp : fIsp; }

			// Address translation as data reads of the current mode see
			// it, without side effects. False if unmapped.
			bool				DebugTranslate(u32 va, u32& pa);
			// The translation tables' entries for an address.
			std::string			DebugDescribe(u32 va);
			// Reads a word as the CPU would (0 when it cannot).
			u32					DebugRead32(u32 va);

			void				SetTraceHook(TraceHook hook)
									{ fTrace = hook; }
			// The kernel's idle loop: whenever the CPU reaches 'pc' with
			// all interrupts enabled (and 'check' agrees), time skips to
			// the next timer event or the end of the time slice.
			void				SetIdleLoop(u32 pc, IdleCheck check)
									{ fIdlePc = pc; fIdleCheck = check; }

	virtual	void				BusMapChanged();

private:
	struct Fault {
		int		id;
		u32		address;		// for faults: the FAULT register value
	};

	// An operand after address calculation.
	struct Operand {
		enum Kind { kMemory, kImmediate, kRegister };
		u8		kind;
		u8		size;		// 1, 2 or 4 bytes
		bool	isSigned;
		u32		value;		// the address, immediate or register number
	};

	enum Access { kRead, kWrite, kFetch };

	// Soft TLB: a direct mapped cache of page translations of each kind
	// (fetch, read, write), tagged with the page and the mode.
	struct TlbEntry {
		u32		tag;		// (va >> 12) << 2 | mode, ~0 = empty
		u8*		host;		// the page in host memory, or null
		u32		physical;	// page base (device pages)
	};
	static const int kTlbSize = 256;

			void				_Step();
			bool				_CheckEvents();
			void				_Event(u32 vectorOffset, bool exception,
									u32 id, u32 pc);
			void				_Exception(const Fault& fault, u32 pc);
			void				_UnimplementedSequence(u32 vectorOffset,
									u32 pc);
			void				_ResetSequence();

			// Instructions
			void				_ExecuteOneParcel(u16 parcel, u32 pc);
			void				_ExecuteNiladic(u16 subcode, u32 pc);
			void				_ExecuteMonadic(int subcode, int mode,
									u32 field, u32 pc, u32 next);
			void				_ExecuteDyadic(int opcode, Operand& src,
									Operand& dst, u32 pc, u32 next);
			void				_Dqm(int srcMode, u32 srcField,
									int dstMode, u32 dstField, bool wide);
			void				_Enter(int mode, u32 field, bool wide);
			void				_Catch(int mode, u32 field, bool wide);
			u32					_StackTarget(int mode, u32 field);
			void				_Return(u32 offset);
			void				_Popn(u32 offset);
			void				_Kcall(u32 value, u32 next);
			void				_Kret();
			void				_Cret();
			u32					_FlowTarget(int mode, u32 field, u32 pc);

			Operand				_DecodeOperand(int mode, u32 field,
									bool wide, u32 pc);
			u32					_Load(const Operand& operand);
			void				_Store(const Operand& operand, u32 value);
			u32					_AbsoluteAddress(u32 field, bool wide,
									u32 pc) const;

			u32					_ReadRegister(int number);
			void				_WriteRegister(int number, u32 value);

			// Memory
			void				_SetCsp(u32 value);
			u32					_ModeTag() const
									{ return ((fPsw >> 15) & 2)
										| ((fPsw >> 10) & 1); }
			TlbEntry&			_Lookup(TlbEntry* table, u32 va,
									Access access)
									{
										u32 tag = ((va >> 12) << 2)
											| _ModeTag();
										TlbEntry& entry = table[(va >> 12)
											& (kTlbSize - 1)];
										if (__builtin_expect(
												entry.tag == tag, 1))
											return entry;
										return _LookupMiss(entry, va, tag,
											access);
									}
			TlbEntry&			_LookupMiss(TlbEntry& entry, u32 va,
									u32 tag, Access access);
			void				_FlushTlb();
			bool				_Translate(u32 va, Access access, bool user,
									u32& pa, int& faultId, bool update);
			u16					_FetchParcel(u32 va);
			u8					_Read8(u32 va);
			u16					_Read16(u32 va);
			u32					_Read32(u32 va);
			void				_Write8(u32 va, u8 value);
			void				_Write16(u32 va, u16 value);
			void				_Write32(u32 va, u32 value);
			u32					_MemRead32(u32 va);
			void				_MemWrite32(u32 va, u32 value);
			bool				_InStackCache(u32 va) const
									{ return (fConfigRegister
											& kConfigStackCache) != 0
										&& va - fSp < fMsp - fSp; }
			u32&				_StackWord(u32 va)
									{ return fStackCache[(va >> 2) & 63]; }
			// Writes the cached words from 'msp' up to the MSP to memory
			// and lowers the MSP to it.
			void				_StackWriteBack(u32 msp);
			// Fills the cache from memory up to 'target' (at most 256
			// bytes above the SP).
			void				_StackFill(u32 target);
			u32					_PhysRead32(u32 pa);
			void				_PhysWrite32(u32 pa, u32 value);

			// Timers
			u64					_Cycles() const
									{ return fNow / fConfig.ticksPerCycle; }
			u32					_TimerValue(int timer) const;
			void				_SetTimer(int timer, u32 value);
			void				_ScheduleTimers();
			void				_TimerEvent();
			bool				_TimerCountsInstructions(int timer) const;

			void				_SetPsw(u32 psw);

			PhysBus&			fBus;
			Config				fConfig;

			u32					fPc;
			u32					fPsw;
			u32					fConfigRegister;
			u32					fSp;
			u32					fMsp;
			u32					fIsp;
			u32					fShad;
			u32					fVb;
			u32					fStb;
			u32					fFault;
			// timers: value = base + (count - reference)
			u32					fTimerBase[2];
			u64					fTimerReference[2];
			bool				fTimerPending[2];

			u64					fNow;
			u64					fInstructions;
			u64					fNextTimerEvent;
			bool				fHeldInReset;
			int					fInterruptLevel;
			bool				fNmiLatched;
			// something may need attention at the next instruction
			// boundary (interrupts, timers, tracing)
			bool				fAttention;
			bool				fCpuPrefix;
			u32					fInstructionPc;

			u32					fStackCache[64];

			TlbEntry			fFetchTlb[kTlbSize];
			TlbEntry			fReadTlb[kTlbSize];
			TlbEntry			fWriteTlb[kTlbSize];

			TraceHook			fTrace;
			u32					fIdlePc;
			IdleCheck			fIdleCheck;
};

}	// namespace bebox

#endif	// BEBOX_HOBBIT_CPU_H

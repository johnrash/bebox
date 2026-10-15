/*
 * BeBox emulator - Hobbit (AT&T 92010) processor tests.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Small programs, assembled here, run on a HobbitCpu with memory at
 * 0x08000000 (code at 0x08000000, data at 0x08010000, stack below
 * 0x08080000); each ends in a jump to itself. Also the file system reader
 * (HobbitVolume) on a small volume built in memory.
 */
#include "core/bus/PhysBus.h"
#include "core/hobbit/HobbitCpu.h"
#include "core/hobbit/HobbitDisasm.h"
#include "core/hobbit/HobbitVolume.h"
#include "core/storage/DiskImage.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace bebox;

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(condition, ...) \
	do { \
		sChecks++; \
		if (!(condition)) { \
			sFailures++; \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} while (0)

static const u32 kRam = 0x08000000;
static const u32 kRamSize = 0x100000;
static const u32 kData = 0x08010000;
static const u32 kStack = 0x08080000;

// Addressing modes
enum {
	kAbsB = 0x0, kAbsUB = 0x1, kAbsH = 0x2, kAbsUH = 0x3,
	kStkB = 0x4, kStkUB = 0x5, kStkH = 0x6, kStkUH = 0x7,
	kIStkB = 0x8,
	kAbs = 0xc, kStk = 0xd, kIStk = 0xe, kImm = 0xf,
	kReg = 0x7		// after a CPU prefix
};

// Two operand opcodes (3 and 5 parcels)
enum {
	kOri = 0x01, kAndi = 0x02, kAddi = 0x03, kMova = 0x04, kUrem = 0x05,
	kMov = 0x06, kDqm = 0x07, kCmpgt = 0x1d, kCmphi = 0x1e, kCmpeq = 0x1f,
	kSub = 0x20, kOr = 0x21, kAnd = 0x22, kAdd = 0x23, kXor = 0x24,
	kRem = 0x25, kMul = 0x26, kDiv = 0x27, kShr = 0x2c, kUshr = 0x2d,
	kShl = 0x2e, kUdiv = 0x2f, kSub3 = 0x30, kAdd3 = 0x33, kMul3 = 0x36
};

// Monadics (3 parcels) and niladics
enum {
	kKcall = 0, kCall = 1, kReturn = 2, kJmp = 3, kJmpTN = 6, kCatch = 8,
	kEnter = 9, kLdraa = 10, kFlushpte = 11, kPopn = 15
};
enum { kNilCpu = 0, kNilKret = 1, kNilNop = 2, kNilCret = 5, kNilTestc = 9 };

class Asm {
public:
	explicit Asm(u32 base) : fBase(base) {}

	u32 Pc() const { return fBase + (u32)fCode.size() * 2; }
	void Parcel(u16 value) { fCode.push_back(value); }

	void One(int opcode, int s, int d)
		{ Parcel((u16)((opcode << 10) | ((s & 31) << 5) | (d & 31))); }
	void Niladic(int subcode) { Parcel((u16)((0x0b << 10) | subcode)); }
	void Stack(int subcode, int n)
		{ Parcel((u16)((0x02 << 10) | ((n & 0xff) << 2) | subcode)); }
	// one parcel jumps (JMP = 3, JMPFN 4, JMPFY 5, JMPTN 6, JMPTY 7) and
	// CALL (1)
	void Jump(int opcode, u32 target)
	{
		s32 offset = (s32)(target - Pc()) / 2;
		Parcel((u16)((opcode << 10) | (offset & 0x3ff)));
	}
	// a jump whose target comes later: returns the index to patch
	size_t JumpForward(int opcode)
	{
		Parcel((u16)(opcode << 10));
		return fCode.size() - 1;
	}
	void Patch(size_t index)
	{
		u32 at = fBase + (u32)index * 2;
		s32 offset = (s32)(Pc() - at) / 2;
		fCode[index] = (u16)((fCode[index] & 0xfc00) | (offset & 0x3ff));
	}
	void Three(int opcode, int smode, int dmode, u16 s, u16 d)
	{
		Parcel((u16)(0x8000 | (opcode << 8) | (smode << 4) | dmode));
		Parcel(s);
		Parcel(d);
	}
	void Five(int opcode, int smode, int dmode, u32 s, u32 d)
	{
		Parcel((u16)(0xc000 | (opcode << 8) | (smode << 4) | dmode));
		Parcel((u16)(s >> 16));
		Parcel((u16)s);
		Parcel((u16)(d >> 16));
		Parcel((u16)d);
	}
	void Monadic(int subcode, int mode, u32 value)
	{
		Parcel((u16)(0x8000 | (mode << 4) | subcode));
		Parcel((u16)(value >> 16));
		Parcel((u16)value);
	}
	void Halt() { Jump(3, Pc()); }

	void Store(u8* ram)
	{
		for (size_t i = 0; i < fCode.size(); i++)
			StoreBE16(ram + (fBase - kRam) + i * 2, fCode[i]);
	}

private:
	u32 fBase;
	std::vector<u16> fCode;
};


struct Machine {
	PhysBus bus;
	std::unique_ptr<u8[]> ram;
	std::unique_ptr<HobbitCpu> cpu;

	Machine()
		:
		ram(new u8[kRamSize]())
	{
		bus.MapMemory(kRam, kRamSize, ram.get(), true);
		HobbitCpu::Config config;
		config.ticksPerCycle = 1;
		config.ticksPerInstruction = 1;
		cpu.reset(new HobbitCpu(bus, config));
		cpu->Reset();
		cpu->SetRegister(HobbitCpu::kRegPsw, HobbitCpu::kPswUseSp);
		cpu->SetRegister(HobbitCpu::kRegSp, kStack);
		cpu->SetRegister(HobbitCpu::kRegShad, kStack);
		cpu->SetRegister(HobbitCpu::kRegIsp, kStack + 0x8000);
		cpu->SetRegister(HobbitCpu::kRegVb, kRam + 0x8000);
		cpu->SetPc(kRam);
	}

	void Run(Asm& program, u64 instructions = 2000)
	{
		program.Store(ram.get());
		cpu->Run(cpu->Now() + instructions);
	}

	u32 Read(u32 address) { return LoadBE32(ram.get() + (address - kRam)); }
	void Write(u32 address, u32 value)
		{ StoreBE32(ram.get() + (address - kRam), value); }
	// a stack word (through the stack cache)
	u32 Stack(int offset)
		{ return cpu->DebugRead32(cpu->Csp() + offset); }
};


static void
TestArithmetic()
{
	Machine m;
	Asm b(kRam);
	b.One(0x1c, 5, 2);					// MOV $5,R8
	b.Five(kAdd, kImm, kStk, 0xffffffff, 8);	// R8 = 4, carry
	b.Niladic(kNilCpu);
	b.Three(kMov, kReg, kStk, 5, 16);	// R16 = PSW
	b.Five(kSub, kImm, kStk, 10, 8);	// R8 = 4 - 10: borrow
	b.Niladic(kNilCpu);
	b.Three(kMov, kReg, kStk, 5, 20);	// R20 = PSW
	b.Five(kMov, kImm, kStk, 0x7fffffff, 24);
	b.Five(kAdd, kImm, kStk, 1, 24);	// signed overflow
	b.Niladic(kNilCpu);
	b.Three(kMov, kReg, kStk, 5, 28);	// R28 = PSW
	b.Five(kMov, kImm, kStk, 0x10000, 32);
	b.Five(kMul, kImm, kStk, 0x10000, 32);	// unsigned overflow
	b.Niladic(kNilCpu);
	b.Three(kMov, kReg, kStk, 5, 36);	// R36 = PSW
	b.Five(kMov, kImm, kStk, 0x80000000, 40);
	b.Five(kDiv, kImm, kStk, 0xffffffff, 40);	// overflow, 0x80000000
	b.Niladic(kNilCpu);
	b.Three(kMov, kReg, kStk, 5, 44);	// R44 = PSW
	b.Five(kMov, kImm, kStk, (u32)-7, 48);
	b.Five(kRem, kImm, kStk, 2, 48);	// -1 (truncating division)
	b.Five(kMov, kImm, kStk, 0xf0000000, 52);
	b.Five(kShr, kImm, kStk, 4, 52);	// 0xff000000
	b.Five(kMov, kImm, kStk, 0xf0000000, 56);
	b.Five(kUshr, kImm, kStk, 4, 56);	// 0x0f000000
	b.One(0x1c, 3, 15);					// MOV $3,R60
	b.One(0x1d, 15, 16);				// MOVA R60,R64
	b.Halt();
	m.Run(b);
	CHECK(m.Stack(8) == 0xfffffffa, "R8 = %08x", m.Stack(8));
	CHECK((m.Stack(16) & HobbitCpu::kPswCarry) != 0, "carry %08x",
		m.Stack(16));
	CHECK((m.Stack(20) & HobbitCpu::kPswCarry) != 0, "borrow %08x",
		m.Stack(20));
	CHECK((m.Stack(28) & HobbitCpu::kPswOverflow) != 0
		&& (m.Stack(28) & HobbitCpu::kPswCarry) == 0, "overflow %08x",
		m.Stack(28));
	CHECK(m.Stack(32) == 0 && (m.Stack(36) & HobbitCpu::kPswCarry) != 0,
		"mul %08x psw %08x", m.Stack(32), m.Stack(36));
	CHECK(m.Stack(40) == 0x80000000
		&& (m.Stack(44) & HobbitCpu::kPswOverflow) != 0, "div %08x %08x",
		m.Stack(40), m.Stack(44));
	CHECK(m.Stack(48) == 0xffffffff, "rem %08x", m.Stack(48));
	CHECK(m.Stack(52) == 0xff000000, "shr %08x", m.Stack(52));
	CHECK(m.Stack(56) == 0x0f000000, "ushr %08x", m.Stack(56));
	CHECK(m.Stack(64) == m.cpu->Csp() + 60, "mova %08x", m.Stack(64));
}


static void
TestOperandSizes()
{
	Machine m;
	m.Write(kData, 0x80ff7f01);
	Asm a(kRam);
	a.Five(kMov, kAbsB, kStk, kData, 8);		// 0xffffff80
	a.Five(kMov, kAbsUB, kStk, kData + 1, 12);	// 0xff
	a.Five(kMov, kAbsH, kStk, kData, 16);		// 0xffff80ff
	a.Five(kMov, kAbsUH, kStk, kData + 2, 20);	// 0x7f01
	a.Five(kMov, kImm, kAbsB, 0x1234, kData + 7);	// byte store
	a.Five(kMov, kImm, kStk, kData + 8, 24);
	a.Five(kMov, kImm, kIStk, 0xcafe, 24);		// *R24 = 0xcafe
	a.Halt();
	m.Run(a);
	CHECK(m.Stack(8) == 0xffffff80, "byte %08x", m.Stack(8));
	CHECK(m.Stack(12) == 0xff, "unsigned byte %08x", m.Stack(12));
	CHECK(m.Stack(16) == 0xffff80ff, "half %08x", m.Stack(16));
	CHECK(m.Stack(20) == 0x7f01, "unsigned half %08x", m.Stack(20));
	CHECK(m.Read(kData + 4) == 0x00000034, "byte store %08x",
		m.Read(kData + 4));
	CHECK(m.Read(kData + 8) == 0xcafe, "indirect store %08x",
		m.Read(kData + 8));
}


// A loop: R8 counts to 10, R12 sums 1..10.
static void
TestBranches()
{
	Machine m;
	Asm a(kRam);
	a.One(0x1c, 0, 2);					// MOV $0,R8
	a.One(0x1c, 0, 3);					// MOV $0,R12
	u32 loop = a.Pc();
	a.One(0x14, 1, 2);					// ADD $1,R8
	a.One(0x16, 2, 3);					// ADD R8,R12
	a.One(0x10, 10, 2);					// CMPEQ $10,R8
	a.Jump(4, loop);					// JMPFN loop
	a.Five(kCmpgt, kImm, kStk, 100, 12);	// 100 > R12: true
	size_t taken = a.JumpForward(7);	// JMPTY
	a.One(0x1c, 1, 4);					// MOV $1,R16 (skipped)
	a.Patch(taken);
	a.Five(kCmphi, kImm, kStk, 1, 12);	// 1 > 55 unsigned: false
	size_t notTaken = a.JumpForward(7);	// JMPTY
	a.One(0x1c, 2, 5);					// MOV $2,R20 (executed)
	a.Patch(notTaken);
	a.Halt();
	m.Run(a);
	CHECK(m.Stack(8) == 10 && m.Stack(12) == 55, "loop %u %u", m.Stack(8),
		m.Stack(12));
	CHECK(m.Stack(16) == 0, "jump taken %u", m.Stack(16));
	CHECK(m.Stack(20) == 2, "jump not taken %u", m.Stack(20));
}


// CALL, ENTER, RETURN: the callee's frame below the caller's; arguments
// at R4 (the accumulator) and up.
static void
TestCalls()
{
	Machine m;
	Asm a(kRam);
	a.One(0x1c, 7, 1);					// MOV $7,R4 (argument)
	size_t call = a.JumpForward(1);		// CALL square
	a.One(0x18, 1, 2);					// MOV R4,R8: result
	a.Halt();
	a.Patch(call);
	// square: R4 = R4 * R4 (the caller's R4 is R20 in the new frame)
	a.Stack(0, 0xff);					// ENTER R-16
	a.Five(kMul3, kStk, kStk, 20, 20);	// R4 = R20 * R20
	a.One(0x18, 1, 5);					// MOV R4,R20
	a.Stack(2, 1);						// RETURN R16
	m.Run(a);
	CHECK(m.Stack(4) == 49 && m.Stack(8) == 49, "call: R4 %u R8 %u",
		m.Stack(4), m.Stack(8));
	CHECK(m.cpu->Csp() == kStack, "csp %08x", m.cpu->Csp());
}


// KCALL through VB + 0, KRET back; a divide by zero takes the exception
// vector (VB + 4) with the id in R4 of the interrupt stack.
static void
TestKernelCalls()
{
	Machine m;
	u32 vb = kRam + 0x8000;
	u32 kcallHandler = kRam + 0x1000;
	u32 exceptionHandler = kRam + 0x2000;
	m.Write(vb, kcallHandler);
	m.Write(vb + 4, exceptionHandler);

	Asm a(kRam);
	a.Parcel(0x0000 | 5);				// KCALL $20 (5 * 4)
	a.Five(kMov, kImm, kStk, 0, 8);
	a.Five(kDiv, kStk, kStk, 8, 12);	// zero divide
	a.Halt();
	a.Store(m.ram.get());

	Asm k(kcallHandler);				// ISP based: R4 = the KCALL value
	k.Five(kMov, kStk, kAbs, 4, kData);
	k.Niladic(kNilKret);
	k.Store(m.ram.get());

	Asm e(exceptionHandler);			// R4 = id, R8 = PC
	e.Five(kMov, kStk, kAbs, 4, kData + 4);
	e.Five(kMov, kStk, kAbs, 8, kData + 8);
	e.Halt();
	e.Store(m.ram.get());

	m.cpu->Run(m.cpu->Now() + 200);
	CHECK(m.Read(kData) == 20, "kcall value %u", m.Read(kData));
	CHECK(m.Read(kData + 4) == HobbitCpu::kExZeroDivide, "exception %u",
		m.Read(kData + 4));
	CHECK(m.Read(kData + 8) == kRam + 2 + 10, "exception pc %08x",
		m.Read(kData + 8));
	CHECK((m.cpu->Psw() & 0xffff) == 0, "kernel mode psw %08x",
		m.cpu->Psw());
}


// The stack cache: ENTER with a large frame writes the cached words back
// to memory; after a CATCH the top of the stack is read on chip even when
// memory no longer holds it (the kernel's context switch).
static void
TestStackCache()
{
	Machine m;
	m.cpu->SetRegister(HobbitCpu::kRegConfig, HobbitCpu::kConfigStackCache);
	Asm a(kRam);
	a.Monadic(kCatch, kStk, 16);				// SP to SP + 16 cached
	a.Five(kMov, kImm, kStk, 0x11111111, 8);	// cached at SP + 8
	a.Five(kMov, kAbs, kAbs, kStack + 8, kData);	// read on chip
	a.Monadic(kEnter, kStk, (u32)-0x400);		// frame > 256 bytes
	a.Halt();
	m.Run(a);
	CHECK(m.Read(kStack + 8) == 0x11111111, "written back %08x",
		m.Read(kStack + 8));
	CHECK(m.Read(kData) == 0x11111111, "read %08x", m.Read(kData));

	Machine n;
	n.cpu->SetRegister(HobbitCpu::kRegConfig, HobbitCpu::kConfigStackCache);
	Asm b(kRam);
	b.Monadic(kCatch, kStk, 16);
	b.Five(kMov, kImm, kStk, 0x22222222, 8);	// in the cache only
	b.Five(kMov, kStk, kAbs, 8, kData);
	b.Halt();
	n.Run(b);
	CHECK(n.Read(kStack + 8) == 0, "not written %08x", n.Read(kStack + 8));
	CHECK(n.Read(kData) == 0x22222222, "on chip %08x", n.Read(kData));

	// ENTER *R4 with R4 = SP flushes the cache and keeps the SP
	Machine o;
	o.cpu->SetRegister(HobbitCpu::kRegConfig, HobbitCpu::kConfigStackCache);
	Asm c(kRam);
	c.Monadic(kCatch, kStk, 16);
	c.Five(kMov, kImm, kStk, 0x33333333, 12);
	c.Niladic(kNilCpu);
	c.Three(kMov, kReg, kStk, 3, 4);			// R4 = SP
	c.Monadic(kEnter, kIStk, 4);				// ENTER *R4
	c.Halt();
	o.Run(c);
	CHECK(o.Read(kStack + 12) == 0x33333333, "flushed %08x",
		o.Read(kStack + 12));
	CHECK(o.cpu->Csp() == kStack, "sp kept %08x", o.cpu->Csp());
}


// Paged and nonpaged translation, the referenced and modified bits, and
// a write fault on a read-only page.
static void
TestMmu()
{
	Machine m;
	u32 segmentTable = kRam + 0x20000;
	u32 pageTable = kRam + 0x21000;
	u32 frame = kRam + 0x30000;
	// segment 0x100 (0x40000000) paged; 0x20 (0x08000000) nonpaged 1 MB
	m.Write(segmentTable + 0x100 * 4, pageTable | 1);
	m.Write(segmentTable + 0x20 * 4, kRam | (0xff << 12) | 8 | 2 | 1);
	m.Write(pageTable + 1 * 4, frame | 2 | 1);		// 0x40001000
	m.Write(pageTable + 2 * 4, frame | 0x1000 | 1);	// 0x40002000 read only
	u32 vb = kRam + 0x8000;
	u32 handler = kRam + 0x2000;
	m.Write(vb + 4, handler);

	Asm a(kRam);
	a.Niladic(kNilCpu);
	a.Five(kMov, kImm, kReg, segmentTable, 8);	// STB
	a.Niladic(kNilCpu);
	a.Five(kOr, kImm, kReg, HobbitCpu::kPswVirtual, 5);
	a.Five(kMov, kImm, kAbs, 0x5a5a5a5a, 0x40001010);
	a.Five(kMov, kAbs, kStk, 0x40002000, 8);
	a.Five(kMov, kImm, kAbs, 1, 0x40002000);	// write fault
	a.Halt();
	a.Store(m.ram.get());
	Asm h(handler);
	h.Five(kMov, kStk, kAbs, 4, kData);			// the exception id
	h.Niladic(kNilCpu);
	h.Three(kMov, kReg, kStk, 9, 8);			// R8 = FAULT
	h.Five(kMov, kStk, kAbs, 8, kData + 4);
	h.Halt();
	h.Store(m.ram.get());
	m.cpu->Run(m.cpu->Now() + 200);
	CHECK(m.Read(frame + 0x10) == 0x5a5a5a5a, "paged write %08x",
		m.Read(frame + 0x10));
	CHECK((m.Read(pageTable + 4) & 0x18) == 0x18, "R and M %08x",
		m.Read(pageTable + 4));
	CHECK((m.Read(pageTable + 8) & 0x18) == 0x08, "R only %08x",
		m.Read(pageTable + 8));
	CHECK(m.Read(kData) == HobbitCpu::kExWriteFault, "write fault %u",
		m.Read(kData));
	CHECK(m.Read(kData + 4) == 0x40002000, "fault address %08x",
		m.Read(kData + 4));
}


// Timer 1 overflows interrupt through VB + 44; an external level 3
// through VB + 28 (enabled by the PSW's priority level).
static void
TestInterrupts()
{
	Machine m;
	u32 vb = kRam + 0x8000;
	u32 timerHandler = kRam + 0x1000;
	u32 levelHandler = kRam + 0x1100;
	m.Write(vb + 44, timerHandler);
	m.Write(vb + 28, levelHandler);

	Asm a(kRam);
	a.Niladic(kNilCpu);
	a.Five(kMov, kImm, kReg, (u32)-100, 11);	// TIMER1
	a.Niladic(kNilCpu);
	a.Five(kMov, kImm, kReg, HobbitCpu::kConfigTimer1Interrupt, 4);
	a.Niladic(kNilCpu);
	a.Five(kOr, kImm, kReg, 7 << 12, 5);		// IPL 7
	a.Halt();
	a.Store(m.ram.get());
	Asm t(timerHandler);
	t.Five(kAdd, kImm, kAbs, 1, kData);
	t.Niladic(kNilCpu);
	t.Five(kMov, kImm, kReg, (u32)-100, 11);	// again in 100 cycles
	t.Niladic(kNilKret);
	t.Store(m.ram.get());
	Asm l(levelHandler);
	l.Five(kAdd, kImm, kAbs, 1, kData + 4);
	l.Halt();
	l.Store(m.ram.get());

	m.cpu->Run(m.cpu->Now() + 1000);
	u32 ticks = m.Read(kData);
	CHECK(ticks >= 8 && ticks <= 10, "timer interrupts %u", ticks);
	m.cpu->SetInterruptLevel(3);
	m.cpu->Run(m.cpu->Now() + 50);
	CHECK(m.Read(kData + 4) == 1, "level 3 interrupt %u", m.Read(kData + 4));
}


static void
TestDisassembler()
{
	struct Case {
		std::vector<u16> parcels;
		const char* text;
	};
	const Case cases[] = {
		{ { 0x0bfc }, "ENTER R-16" },
		{ { 0x0802 }, "RETURN R0" },
		{ { 0x2c00, 0x86f7, 0x0000, 0x0005 }, "CPU MOV $0x0,%PSW" },
		{ { 0xc6f0, 0x0000, 0x0001, 0xd100, 0x03c3 },
			"MOV $0x1,*$0xd10003c3:B" },
	};
	for (const Case& test : cases) {
		int length;
		std::string text = HobbitDisassemble(0x1000, [&](u32 address) {
			size_t index = (address - 0x1000) / 2;
			return index < test.parcels.size() ? test.parcels[index]
				: (u16)0;
		}, length);
		CHECK(text == test.text, "disassembly '%s' (expected '%s')",
			text.c_str(), test.text);
	}
}


// A disk image in memory
class MemoryImage : public DiskImage {
public:
	explicit MemoryImage(size_t size) : fData(size, 0) {}
	virtual u64 Size() const { return fData.size(); }
	virtual bool IsReadOnly() const { return false; }
	virtual bool Read(u64 offset, void* buffer, size_t length)
	{
		if (offset + length > fData.size())
			return false;
		memcpy(buffer, fData.data() + offset, length);
		return true;
	}
	virtual bool Write(u64 offset, const void* buffer, size_t length)
	{
		if (offset + length > fData.size())
			return false;
		memcpy(fData.data() + offset, buffer, length);
		return true;
	}
	virtual std::string Path() const { return "memory"; }
	virtual const char* Format() const { return "memory"; }

	u8* Sector(u32 sector) { return fData.data() + (size_t)sector * 512; }

private:
	std::vector<u8> fData;
};


static void
PutDirectoryEntry(u8* block, int index, const char* name, u32 first,
	u32 count, u32 type, u32 modified, u32 size, u32 creator)
{
	u8* entry = block + 64 + index * 64;
	memcpy(entry, name, strlen(name));
	StoreBE32(entry + 32, first);
	StoreBE32(entry + 36, count);
	StoreBE32(entry + 40, type);
	StoreBE32(entry + 44, modified);
	StoreBE32(entry + 48, modified);
	StoreBE32(entry + 52, size);
	StoreBE32(entry + 56, count * 512);
	StoreBE32(entry + 60, creator);
}


// Root directory at sector 8 with a contiguous file, a deleted one, a
// directory and a file through a block map (whose first extent is empty,
// as on older volumes); the directory at sector 16 holds one file.
static void
TestVolume()
{
	const u32 kText = 0x54455854;	// kText
	const u32 kEdit = 0x45444954;	// kEdit
	const u32 kBapp = 0x42415050;	// kBapp

	MemoryImage image(128 * 512);
	u8* header = image.Sector(0);
	StoreBE32(header, 0x00010000);
	StoreBE32(header + 0x10, 8);
	StoreBE32(header + 0x14, 128);
	StoreBE32(header + 0x18, 512);
	memcpy(header + 0x2c, "test", 4);

	u8* root = image.Sector(8);
	PutDirectoryEntry(root, 0, "hello", 0x80000000 | 40, 2, kText,
		786112304, 600, kEdit);
	PutDirectoryEntry(root, 1, "Xold", 0x80000000 | 50, 1, kText, 0, 10, 0);
	root[64 + 64] = 0;		// deleted
	PutDirectoryEntry(root, 2, "sub", 16, 8, 0xffffffff, 0, 0, 0);
	PutDirectoryEntry(root, 3, "mapped", 60, 2, kBapp, 0, 1000, 0);
	PutDirectoryEntry(image.Sector(16), 0, "inner", 0x80000000 | 80, 1, 0, 0,
		5, 0);
	for (int i = 0; i < 1024; i++)
		image.Sector(40)[i] = (u8)i;
	u8* map = image.Sector(60);
	StoreBE32(map + 8, 0);			// (0, 0): skipped
	StoreBE32(map + 12, 0);
	StoreBE32(map + 16, 70);
	StoreBE32(map + 20, 1);
	StoreBE32(map + 24, 72);
	StoreBE32(map + 28, 1);
	StoreBE32(map + 32, 0xffffffff);
	memset(image.Sector(70), 0xaa, 512);
	memset(image.Sector(72), 0xbb, 512);
	memcpy(image.Sector(80), "inner", 5);

	HobbitVolume volume(image);
	std::string error;
	CHECK(volume.Mount(error), "mount: %s", error.c_str());
	CHECK(volume.Name() == "test", "volume name '%s'", volume.Name().c_str());

	std::vector<HobbitVolume::Entry> entries;
	CHECK(volume.ReadDirectory(8, entries), "root directory");
	CHECK(entries.size() == 3, "%zu root entries (the deleted one shown?)",
		entries.size());
	if (entries.size() == 3) {
		CHECK(entries[0].name == "hello" && entries[1].name == "sub"
			&& entries[2].name == "mapped", "root entry names");
		CHECK(entries[0].type == kText && entries[0].creator == kEdit
			&& entries[0].modified == 786112304 && entries[0].size == 600,
			"entry fields");
		CHECK(entries[1].IsDirectory(), "sub is a directory");
	}

	std::vector<u8> data;
	CHECK(volume.ReadFile("/hello", data, error) && data.size() == 600
		&& data[0] == 0 && data[599] == (u8)599, "contiguous file: %s",
		error.c_str());
	CHECK(volume.ReadFile("/mapped", data, error) && data.size() == 1000
		&& data[0] == 0xaa && data[511] == 0xaa && data[512] == 0xbb
		&& data[999] == 0xbb, "mapped file: %s", error.c_str());
	CHECK(volume.ReadFile("/sub/inner", data, error)
		&& std::string(data.begin(), data.end()) == "inner",
		"file in a directory: %s", error.c_str());
	CHECK(!volume.ReadFile("/sub", data, error), "a directory read as a file");
	CHECK(!volume.ReadFile("/old", data, error), "deleted file found");

	// DR6-DR8 volumes (versions 2 and 3) are not Hobbit volumes
	StoreBE32(header, 0x00030000);
	HobbitVolume dr8(image);
	CHECK(!dr8.Mount(error), "a version 3 volume mounted");
}


int
main()
{
	TestArithmetic();
	TestOperandSizes();
	TestBranches();
	TestCalls();
	TestKernelCalls();
	TestStackCache();
	TestMmu();
	TestInterrupts();
	TestDisassembler();
	TestVolume();
	printf("hobbit-tests: %d checks, %d failures\n", sChecks, sFailures);
	return sFailures == 0 ? 0 : 1;
}

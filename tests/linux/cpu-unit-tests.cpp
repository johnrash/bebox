/*
 * CPU unit tests:
 *  - ppc/systest.S: supervisor level behaviour (exceptions, software TLB
 *    reload, BATs, protection, TGPR, decrementer, doze, trace), run on one
 *    CPU; the program records its own checks.
 *  - ppc/smptest.S: lwarx/stwcx. on two CPUs sharing memory, interleaved
 *    with small time slices.
 *  - the floating point arithmetic instructions' fast path against their
 *    general path, on random operands.
 *
 * usage: cpu-unit-tests <dir with systest.bin and smptest.bin>
 */
#include "core/Log.h"
#include "core/bus/PhysBus.h"
#include "core/cpu/PpcCpu.h"
#include "core/cpu/PpcOps.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace bebox;

static int sFailures = 0;
static int sChecks = 0;


static void
Expect(bool condition, const char* what)
{
	sChecks++;
	if (!condition) {
		sFailures++;
		printf("FAIL: %s\n", what);
	}
}


static std::vector<u8>
ReadFile(const std::string& path)
{
	std::vector<u8> data;
	FILE* file = fopen(path.c_str(), "rb");
	if (file == nullptr) {
		perror(path.c_str());
		exit(2);
	}
	u8 buffer[65536];
	size_t count;
	while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0)
		data.insert(data.end(), buffer, buffer + count);
	fclose(file);
	return data;
}


struct Tracer : public PpcTraceHook {
	u64 remaining;
	Tracer(u64 count) : remaining(count) {}
	virtual void Instruction(PpcCpu& cpu, u32 pc, u32 instruction)
	{
		if (remaining == 0)
			return;
		remaining--;
		printf("  %08x: %08x  msr %08x r3 %08x r4 %08x lr %08x\n", pc,
			instruction, cpu.GetMsr(), cpu.gpr[3], cpu.gpr[4], cpu.fLr);
	}
};


static void
SystemTests(const std::string& dir)
{
	printf("== systest\n");
	std::vector<u8> image = ReadFile(dir + "/systest.bin");
	std::vector<u8> ram(8 << 20, 0);
	memcpy(ram.data(), image.data(), image.size());
	PhysBus bus;
	bus.MapMemory(0, (u32)ram.size(), ram.data(), true);

	PpcCpu::Config config;
	PpcCpu cpu(bus, config);
	cpu.SetMsr(MSR_ME);
	cpu.pc = 0x4000;
	Tracer tracer(getenv("SYSTEST_TRACE") != nullptr
		? strtoull(getenv("SYSTEST_TRACE"), nullptr, 0) : 0);
	if (tracer.remaining != 0)
		cpu.SetTraceHook(&tracer);

	const u32 kDone = 0x3ffc;
	for (int i = 0; i < 1000 && LoadBE32(&ram[kDone]) != 0xd0d0d0d0; i++)
		cpu.Run(cpu.Now() + 100000);
	Expect(LoadBE32(&ram[kDone]) == 0xd0d0d0d0, "systest finished");
	if (LoadBE32(&ram[kDone]) != 0xd0d0d0d0)
		printf("  stopped at pc %08x\n", cpu.pc);

	u32 end = LoadBE32(&ram[0x3ff8]);
	if (end < 0x6000 || end > 0x7000)
		end = 0x6000;
	int count = 0;
	for (u32 record = 0x6000; record < end; record += 12) {
		u32 expected = LoadBE32(&ram[record]);
		u32 actual = LoadBE32(&ram[record + 4]);
		u32 name = LoadBE32(&ram[record + 8]);
		const char* text = name < ram.size() ? (const char*)&ram[name] : "?";
		sChecks++;
		count++;
		if (expected != actual) {
			sFailures++;
			printf("FAIL: %s: expected %08x, got %08x\n", text, expected,
				actual);
		}
	}
	printf("  %d checks recorded by the program\n", count);
}


static void
SmpTests(const std::string& dir)
{
	printf("== smptest\n");
	std::vector<u8> image = ReadFile(dir + "/smptest.bin");
	std::vector<u8> ram(1 << 20, 0);
	memcpy(ram.data(), image.data(), image.size());
	PhysBus bus;
	bus.MapMemory(0, (u32)ram.size(), ram.data(), true);

	// Odd slice lengths so the CPUs interleave at many different points
	// of their lwarx/stwcx. loops.
	for (u64 slice : { 3, 7, 13, 64, 1000 }) {
		memset(ram.data(), 0, ram.size());
		memcpy(ram.data(), image.data(), image.size());
		PpcCpu::Config config0;
		config0.index = 0;
		PpcCpu::Config config1;
		config1.index = 1;
		PpcCpu cpu0(bus, config0);
		PpcCpu cpu1(bus, config1);
		cpu0.SetMsr(MSR_ME);
		cpu1.SetMsr(MSR_ME);
		cpu0.pc = 0x100;
		cpu1.pc = 0x100;
		cpu0.gpr[3] = 0;
		cpu1.gpr[3] = 1;
		u64 now = 0;
		for (int i = 0; i < 20000000; i++) {
			now += slice * config0.ticksPerInstruction;
			cpu0.Run(now);
			cpu1.Run(now);
			if (LoadBE32(&ram[0x1004]) == 2)
				break;
		}
		u32 counter = LoadBE32(&ram[0x1000]);
		char what[128];
		snprintf(what, sizeof(what),
			"two CPUs, slice %llu: counter %u (expected 200000)",
			(unsigned long long)slice, counter);
		Expect(counter == 200000 && LoadBE32(&ram[0x1004]) == 2, what);
		u32 own0 = LoadBE32(&ram[0x1100]);
		u32 own1 = LoadBE32(&ram[0x1104]);
		Expect(own0 == 100000 && own1 == 100000, "each CPU succeeded 100000 times");
	}
}


// The CPUs on two host threads at the same time (threaded mode): lwarx/
// stwcx. must still make every increment count.
static void
ThreadedSmpTests(const std::string& dir)
{
	printf("== smptest, a thread per CPU\n");
	std::vector<u8> image = ReadFile(dir + "/smptest.bin");
	std::vector<u8> ram(1 << 20, 0);
	memcpy(ram.data(), image.data(), image.size());
	PhysBus bus;
	bus.MapMemory(0, (u32)ram.size(), ram.data(), true);
	bus.SetThreaded(true);
	PpcCpu::Config config0;
	config0.index = 0;
	PpcCpu::Config config1;
	config1.index = 1;
	PpcCpu cpu0(bus, config0);
	PpcCpu cpu1(bus, config1);
	PpcCpu* cpus[2] = { &cpu0, &cpu1 };
	std::thread threads[2];
	for (int i = 0; i < 2; i++) {
		PpcCpu& cpu = *cpus[i];
		cpu.SetMsr(MSR_ME);
		cpu.pc = 0x100;
		cpu.gpr[3] = i;
		threads[i] = std::thread([&cpu, &ram]() {
			u64 now = 0;
			for (int slice = 0; slice < 2000000; slice++) {
				now += 997 * cpu.GetConfig().ticksPerInstruction;
				cpu.Run(now);
				if (__atomic_load_n((u32*)&ram[0x1004], __ATOMIC_ACQUIRE)
						== BSwap32(2))
					break;
			}
		});
	}
	for (std::thread& thread : threads)
		thread.join();
	u32 counter = LoadBE32(&ram[0x1000]);
	char what[128];
	snprintf(what, sizeof(what),
		"two threads: counter %u (expected 200000)", counter);
	Expect(counter == 200000 && LoadBE32(&ram[0x1004]) == 2, what);
	u32 own0 = LoadBE32(&ram[0x1100]);
	u32 own1 = LoadBE32(&ram[0x1104]);
	Expect(own0 == 100000 && own1 == 100000,
		"each thread's CPU succeeded 100000 times");
}


// An operand for the floating point tests: all exponents, single precision
// values, small integers, values near the fast path's limits and the
// range's ends, denormals, zeros, infinities and NaNs.
static u64
RandomOperand(std::mt19937_64& random)
{
	auto bits = [](double value) {
		u64 result;
		memcpy(&result, &value, 8);
		return result;
	};
	u64 sign = (random() & 1) << 63;
	switch (random() % 10) {
		case 0:
		case 1:
		case 2:
			return random();
		case 3:
		case 4:
		{
			u32 word = (u32)random();
			if ((word & 0x7f800000) == 0x7f800000)
				word &= ~0x40000000u;
			float value;
			memcpy(&value, &word, 4);
			return bits(value);
		}
		case 5:
			return sign | bits((double)(random() % 64)
				* std::ldexp(1.0, (int)(random() % 9) - 4));
		case 6:
		{
			static const int kExponents[] = { -1074, -1060, -1022, -1021,
				-969, -961, -960, -959, -901, -900, -899, -149, -126, -125,
				127, 128, 899, 900, 901, 1022, 1023 };
			int e = kExponents[random() % (sizeof(kExponents) / sizeof(int))];
			double m = 1 + (double)(random() >> 11) / 9007199254740992.0;
			return sign | bits(std::ldexp(m, e));
		}
		case 7:
			return sign | (random() & 0xfffffffffffffull);	// denormal
		case 8:
			return sign | (random() % 4 == 0 ? 0 : bits(1.0));
		default:
		{
			static const u64 kSpecial[] = { 0x7ff0000000000000ull,
				0x7ff8000000000000ull, 0x7ff4000000000000ull,
				0x7fefffffffffffffull, 0x0010000000000000ull,
				0x47efffffe0000000ull, 0x47efffffffffffffull,
				0x3810000000000000ull, 0x380fffffffffffffull };
			return sign | kSpecial[random() % 9];
		}
	}
}


static void
FastArithmeticTests()
{
	printf("== floating point arithmetic: fast path against general path\n");
	std::vector<u8> ram(4096, 0);
	PhysBus bus;
	bus.MapMemory(0, (u32)ram.size(), ram.data(), true);
	PpcCpu::Config config;
	PpcCpu fast(bus, config);
	PpcCpu general(bus, config);
	fast.SetMsr(MSR_FP);
	general.SetMsr(MSR_FP);
	std::mt19937_64 random(1);
	// fdiv, fsub, fadd, fmul, fmsub, fmadd, fnmsub, fnmadd
	static const u32 kForms[] = { 18, 20, 21, 25, 28, 29, 30, 31 };
	int mismatches = 0;
	const int kCases = 1000000;
	for (int n = 0; n < kCases; n++) {
		bool single = (random() & 1) != 0;
		u32 instruction = ((single ? 59u : 63u) << 26) | (1 << 21) | (2 << 16)
			| (3 << 11) | (4 << 6) | (kForms[random() % 8] << 1)
			| (u32)(random() & 1);
		u64 a = RandomOperand(random);
		u64 b = random() % 8 == 0 ? a ^ (random() % 2 << 63)
			: RandomOperand(random);	// cancellations
		u64 c = RandomOperand(random);
		// mostly the fast path's case, some sticky bits; now and then
		// another rounding mode, enabled exceptions or non-IEEE mode
		u32 fpscr = (u32)random() & 0x9e000000;
		if (random() % 8 == 0)
			fpscr |= (u32)random() & 0xff;
		u32 cr = (u32)random();
		PpcCpu* cpus[2] = { &fast, &general };
		for (int k = 0; k < 2; k++) {
			PpcCpu& cpu = *cpus[k];
			cpu.fpr[1] = 0x123456789abcdefull;
			cpu.fpr[2] = a;
			cpu.fpr[3] = b;
			cpu.fpr[4] = c;
			cpu.SetFpscr(fpscr);
			cpu.fCr = cr;
			PpcOps::sFastArithmetic = k == 0;
			PpcOps::sPrimary[instruction >> 26](cpu, instruction);
		}
		if (fast.fpr[1] != general.fpr[1] || fast.GetFpscr() != general.GetFpscr()
			|| fast.fCr != general.fCr) {
			if (mismatches++ < 10) {
				printf("  %08x a %016llx b %016llx c %016llx fpscr %08x: "
					"fast %016llx %08x, general %016llx %08x\n", instruction,
					(unsigned long long)a, (unsigned long long)b,
					(unsigned long long)c, fpscr,
					(unsigned long long)fast.fpr[1], fast.GetFpscr(),
					(unsigned long long)general.fpr[1], general.GetFpscr());
			}
		}
	}
	PpcOps::sFastArithmetic = true;
	PpcOps::SetHostRounding(0);
	char what[96];
	snprintf(what, sizeof(what), "fast path: %d of %d cases differ",
		mismatches, kCases);
	Expect(mismatches == 0, what);

	// fmsubs f1,f2,f4,f3 rounds once to single, as the 603e (and qemu-ppc)
	// do; rounding to double first gave one unit more in the last place
	for (int k = 0; k < 2; k++) {
		PpcCpu& cpu = k == 0 ? fast : general;
		cpu.fpr[2] = 0x3fb999999999999aull;		// 0.1
		cpu.fpr[3] = 0xbb1f1ab8e0000000ull;
		cpu.fpr[4] = 0xbb1f1ab8e0000000ull;
		cpu.SetFpscr(0);
		PpcOps::sFastArithmetic = k == 0;
		u32 fmsubs = (59u << 26) | (1 << 21) | (2 << 16) | (3 << 11)
			| (4 << 6) | (28 << 1);
		PpcOps::sPrimary[59](cpu, fmsubs);
	}
	PpcOps::sFastArithmetic = true;
	Expect(fast.fpr[1] == 0x3b1bfe7320000000ull
		&& general.fpr[1] == 0x3b1bfe7320000000ull,
		"single precision multiply-add rounded once");
}


int
main(int argc, char** argv)
{
	std::string dir = argc > 1 ? argv[1] : "out";
	SystemTests(dir);
	SmpTests(dir);
	for (int i = 0; i < 5; i++)
		ThreadedSmpTests(dir);
	FastArithmeticTests();
	printf("%d checks, %d failures\n", sChecks, sFailures);
	return sFailures == 0 ? 0 : 1;
}

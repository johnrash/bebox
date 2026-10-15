/*
 * CPU unit tests:
 *  - ppc/systest.S: supervisor level behaviour (exceptions, software TLB
 *    reload, BATs, protection, TGPR, decrementer, doze, trace), run on one
 *    CPU; the program records its own checks.
 *  - ppc/smptest.S: lwarx/stwcx. on two CPUs sharing memory, interleaved
 *    with small time slices.
 *
 * usage: cpu-unit-tests <dir with systest.bin and smptest.bin>
 */
#include "core/Log.h"
#include "core/bus/PhysBus.h"
#include "core/cpu/PpcCpu.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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


int
main(int argc, char** argv)
{
	std::string dir = argc > 1 ? argv[1] : "out";
	SystemTests(dir);
	SmpTests(dir);
	for (int i = 0; i < 5; i++)
		ThreadedSmpTests(dir);
	printf("%d checks, %d failures\n", sChecks, sFailures);
	return sFailures == 0 ? 0 : 1;
}

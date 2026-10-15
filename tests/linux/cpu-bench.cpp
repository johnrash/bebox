/*
 * Interpreter speed: runs ppc/bench.S (11 instructions per iteration,
 * 2,000,000 iterations) and prints millions of instructions per second.
 * Build with FAST=1 for meaningful numbers.
 */
#include "core/bus/PhysBus.h"
#include "core/cpu/PpcCpu.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace bebox;

int
main(int argc, char** argv)
{
	std::string dir = argc > 1 ? argv[1] : "out";
	FILE* file = fopen((dir + "/bench.bin").c_str(), "rb");
	if (file == nullptr) {
		perror("bench.bin");
		return 2;
	}
	std::vector<u8> ram(4 << 20, 0);
	size_t size = fread(ram.data(), 1, ram.size(), file);
	fclose(file);
	(void)size;
	PhysBus bus;
	bus.MapMemory(0, (u32)ram.size(), ram.data(), true);
	PpcCpu::Config config;
	PpcCpu cpu(bus, config);
	cpu.SetMsr(MSR_ME);
	cpu.pc = 0x100;

	auto start = std::chrono::steady_clock::now();
	u64 target = 2000000ull * 11 + 10;
	while (cpu.InstructionCount() < target)
		cpu.Run(cpu.Now() + 2000000);
	auto end = std::chrono::steady_clock::now();
	double seconds = std::chrono::duration<double>(end - start).count();
	printf("%llu instructions in %.3f s: %.1f MIPS\n",
		(unsigned long long)cpu.InstructionCount(), seconds,
		cpu.InstructionCount() / seconds / 1e6);
	return 0;
}

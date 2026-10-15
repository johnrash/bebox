/*
 * Runs a static 32-bit big endian PowerPC Linux ELF (the instruction test
 * executor, tests/linux/ppc/exec.S) on the emulator's CPU core with a tiny
 * system call layer (write, exit), so its output can be compared with
 * qemu-ppc's.
 *
 * usage: ppc-exec [--model 603|603e] [--trace] program.elf > output
 */
#include "core/Log.h"
#include "core/bus/PhysBus.h"
#include "core/cpu/PpcCpu.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace bebox;

static const u32 kLowRamSize = 1 << 20;			// vectors at 0
static const u32 kProgramBase = 0x10000000;
static const u32 kProgramRamSize = 32 << 20;

struct SyscallHook : public PpcDebugHook {
	PhysBus& bus;
	bool exited = false;
	int exitCode = 0;
	bool trace = false;

	SyscallHook(PhysBus& bus) : bus(bus) {}

	virtual bool ShouldStop(PpcCpu& cpu)
	{
		if (trace) {
			u32 insn = 0;
			cpu.DebugRead32(cpu.pc, insn);
			fprintf(stderr, "%08x: %08x\n", cpu.pc, insn);
		}
		if (cpu.pc != 0xc00)
			return false;
		u32 number = cpu.gpr[0];
		switch (number) {
			case 1:		// exit
				exited = true;
				exitCode = (int)cpu.gpr[3];
				return true;
			case 4:		// write
			{
				u32 address = cpu.gpr[4];
				u32 length = cpu.gpr[5];
				std::vector<u8> data(length);
				bus.DmaRead(address, data.data(), length);
				FILE* out = cpu.gpr[3] == 2 ? stderr : stdout;
				fwrite(data.data(), 1, length, out);
				cpu.gpr[3] = length;
				break;
			}
			default:
				fprintf(stderr, "unsupported system call %u at %08x\n",
					number, cpu.cia);
				exited = true;
				exitCode = 99;
				return true;
		}
		u32 srr0;
		cpu.ReadSpr(SPR_SRR0, srr0);
		cpu.ReturnFromInterrupt();
		return false;
	}
};


static bool
LoadElf(const char* path, PhysBus& bus, u32& entry)
{
	FILE* file = fopen(path, "rb");
	if (file == nullptr) {
		perror(path);
		return false;
	}
	std::vector<u8> image;
	u8 buffer[65536];
	size_t count;
	while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0)
		image.insert(image.end(), buffer, buffer + count);
	fclose(file);

	if (image.size() < 52 || memcmp(image.data(), "\x7f" "ELF", 4) != 0
		|| image[4] != 1 || image[5] != 2) {
		fprintf(stderr, "%s: not a 32-bit big endian ELF\n", path);
		return false;
	}
	entry = LoadBE32(&image[24]);
	u32 phoff = LoadBE32(&image[28]);
	u16 phentsize = LoadBE16(&image[42]);
	u16 phnum = LoadBE16(&image[44]);
	for (int i = 0; i < phnum; i++) {
		const u8* ph = &image[phoff + i * phentsize];
		if (LoadBE32(ph) != 1)		// PT_LOAD
			continue;
		u32 offset = LoadBE32(ph + 4);
		u32 vaddr = LoadBE32(ph + 8);
		u32 filesz = LoadBE32(ph + 16);
		u32 memsz = LoadBE32(ph + 20);
		std::vector<u8> segment(memsz, 0);
		memcpy(segment.data(), &image[offset], filesz);
		bus.DmaWrite(vaddr, segment.data(), memsz);
	}
	return true;
}


int
main(int argc, char** argv)
{
	PpcCpu::Model model = PpcCpu::kPpc603e;
	bool trace = false;
	const char* path = nullptr;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
			model = strcmp(argv[++i], "603") == 0 ? PpcCpu::kPpc603
				: PpcCpu::kPpc603e;
		} else if (strcmp(argv[i], "--trace") == 0)
			trace = true;
		else
			path = argv[i];
	}
	if (path == nullptr) {
		fprintf(stderr, "usage: ppc-exec [--model 603|603e] [--trace] "
			"program.elf\n");
		return 2;
	}

	PhysBus bus;
	std::vector<u8> lowRam(kLowRamSize, 0);
	std::vector<u8> programRam(kProgramRamSize, 0);
	bus.MapMemory(0, kLowRamSize, lowRam.data(), true);
	bus.MapMemory(kProgramBase, kProgramRamSize, programRam.data(), true);

	u32 entry;
	if (!LoadElf(path, bus, entry))
		return 1;

	PpcCpu::Config config;
	config.model = model;
	config.pvr = model == PpcCpu::kPpc603 ? 0x00030100 : 0x00060401;
	PpcCpu cpu(bus, config);
	cpu.SetMsr(MSR_FP | MSR_ME);
	cpu.pc = entry;

	SyscallHook hook(bus);
	hook.trace = trace;
	cpu.SetDebugHook(&hook);

	u64 limit = 4000000000ull;
	while (!hook.exited && cpu.InstructionCount() < limit) {
		cpu.Run(cpu.Now() + 1000000);
		if (cpu.IsCheckstopped())
			break;
	}
	fflush(stdout);
	if (!hook.exited) {
		fprintf(stderr, "program did not exit (pc %08x)\n", cpu.pc);
		return 1;
	}
	return hook.exitCode;
}

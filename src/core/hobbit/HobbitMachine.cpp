/*
 * BeBox emulator - the Hobbit BeBox prototype: two AT&T 92010 (Hobbit)
 * processors at 25 MHz on the BeBus.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "HobbitMachine.h"

#include "HobbitBoard.h"
#include "HobbitCpu.h"
#include "HobbitDisasm.h"
#include "HobbitVideo.h"
#include "HobbitVolume.h"
#include "Ncr53c94.h"
#include "../Log.h"
#include "../bus/IoSpace.h"
#include "../bus/PhysBus.h"
#include "../isa/Fdc82077.h"
#include "../isa/Kbc8042.h"
#include "../isa/Rtc146818.h"
#include "../isa/Uart16550.h"
#include "../storage/CdImage.h"
#include "../storage/DiskImage.h"

#include <algorithm>
#include <cstring>

namespace bebox {

static const u32 kFlashSize = 512 * 1024;
static const u32 kDspRamBase = 0x60000000;
static const u32 kDspRamSize = 2 * 1024 * 1024;
// The memory sizes the kernel knows (else it assumes 8 MB).
static const u32 kMemorySizes[] = { 32, 20, 16, 8, 4 };
static const u16 kElfMachineHobbit = 0x39d;
// CONFIG: stack cache, instruction cache and prefetch buffer enabled.
static const u32 kBootConfig = 0x001c0000;


// Ports of the combination chip nothing emulates yet: the parallel port
// (idle, no printer), the joystick controller ("PMC", a microcontroller:
// always ready, nothing to say) and the joystick registers.
class UnemulatedHobbitPorts : public ByteIoDevice {
public:
	virtual u8 IoRead8(u32 port)
	{
		switch (port) {
			case 0x379:		// LPT status: not busy, selected, no error
				return 0xdf;
			case 0x222:		// PMC data
			case 0x226:		// PMC status: ready
				return 0x00;
		}
		return 0x00;
	}

	virtual void IoWrite8(u32 port, u8 value)
	{
		BB_LOG(kLogIsa, kLogDebug, "hobbit: port %03x = %02x (unemulated)",
			port, value);
	}

	virtual const char* IoName() const { return "unemulated"; }
};


static bool
ReadFile(const std::string& path, std::vector<u8>& data)
{
	FILE* file = fopen(path.c_str(), "rb");
	if (file == nullptr)
		return false;
	data.clear();
	u8 buffer[65536];
	size_t count;
	while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0)
		data.insert(data.end(), buffer, buffer + count);
	fclose(file);
	return true;
}


static bool
WriteFile(const std::string& path, const void* data, size_t size)
{
	std::string temporary = path + ".new";
	FILE* file = fopen(temporary.c_str(), "wb");
	if (file == nullptr)
		return false;
	bool ok = fwrite(data, 1, size, file) == size;
	ok = fclose(file) == 0 && ok;
	if (ok)
		ok = rename(temporary.c_str(), path.c_str()) == 0;
	return ok;
}


HobbitMachine::HobbitMachine(const MachineConfig& config)
	:
	fConfig(config),
	fNow(0),
	fRamSize(0),
	fKernelEntry(0),
	fIdlePc(1),
	fScheduleHint(0)
{
	fTicksPerSecond = (u64)kClockHz * kTicksPerCycle;
	fQuantum = (u64)std::max<u32>(1, config.quantumUs) * fTicksPerSecond
		/ 1000000;
	fConfig.cpuCount = 2;
}


HobbitMachine::~HobbitMachine()
{
	fSwitchTimer.reset();
	for (auto& cpu : fCpus)
		cpu.reset();
	for (auto& uart : fUarts)
		uart.reset();
	fKeyboard.reset();
	fRtc.reset();
	fScsi.reset();
	fFloppy.reset();
	fVideo.reset();
	fBoard.reset();
}


bool
HobbitMachine::Init(std::string& error)
{
	fBus.reset(new PhysBus);
	fIo.reset(new IoSpace);

	// memory: the largest size the kernel knows that fits
	u32 megabytes = 4;
	for (u32 size : kMemorySizes) {
		if (size <= fConfig.ramMB) {
			megabytes = size;
			break;
		}
	}
	fConfig.ramMB = megabytes;
	fRamSize = megabytes << 20;
	fRam.reset(new u8[fRamSize]());
	fBus->MapMemory(kRamBase, fRamSize, fRam.get(), true);
	fDspRam.reset(new u8[kDspRamSize]());
	fBus->MapMemory(kDspRamBase, kDspRamSize, fDspRam.get(), true);

	// The boot flash: an endless loop (the machine does the ROM's job).
	fFlash.assign(kFlashSize, 0xff);
	StoreBE16(&fFlash[0], 0x0c00);		// JMP .
	fBus->MapMemory(0, kFlashSize, fFlash.data(), false);

	fBoard.reset(new HobbitBoard(*fBus, *fIo));
	fBus->MapDevice(HobbitBoard::kIoBase, HobbitBoard::kIoSize,
		fBoard.get());
	fBoard->SetReleaseCallback([this](int cpu) { _EnterKernel(cpu); });

	fRtc.reset(new Rtc146818(fScheduler, fTicksPerSecond));
	fIo->Map(0x70, 2, fRtc.get());
	fRtc->SetIrqCallback([this](bool level) {
		fBoard->SetIoInterrupt(HobbitBoard::kIoRtc, level);
	});

	fKeyboard.reset(new Kbc8042(fScheduler, fTicksPerSecond));
	fIo->Map(0x60, 1, fKeyboard.get());
	fIo->Map(0x64, 1, fKeyboard.get());
	fKeyboard->SetIrqCallback([this](int irq, bool level) {
		fBoard->SetIoInterrupt(irq == 12 ? HobbitBoard::kIoMouse
			: HobbitBoard::kIoKeyboard, level);
	});
	fKeyboard->SetResetCallback([this]() { PowerOn(); });

	static const struct {
		const char* name;
		u32 base;
		int bit;
	} kUarts[kSerialPorts] = {
		{ "serial1", 0x3f8, HobbitBoard::kIoSerial1 },
		{ "serial2", 0x2f8, HobbitBoard::kIoSerial2 },
	};
	for (int i = 0; i < kSerialPorts; i++) {
		fUarts[i].reset(new Uart16550(kUarts[i].name, kUarts[i].base,
			1843200, fScheduler, fTicksPerSecond, true));
		fIo->Map(kUarts[i].base, 8, fUarts[i].get());
		int bit = kUarts[i].bit;
		fUarts[i]->SetIrqCallback([this, bit](bool level) {
			fBoard->SetIoInterrupt(bit, level);
		});
	}

	fScsi.reset(new Ncr53c94(fScheduler, fTicksPerSecond, kClockHz));
	fBoard->SetScsi(fScsi.get());
	fScsi->SetIrqCallback([this](bool level) {
		fBoard->SetIoInterrupt(HobbitBoard::kIoScsi, level);
	});
	for (int id = 0; id < 7; id++) {
		const MachineConfig::ScsiTarget& target = fConfig.scsi[id];
		if (target.kind == MachineConfig::ScsiTarget::kDisk
			|| target.kind == MachineConfig::ScsiTarget::kSharedFolder) {
			std::unique_ptr<DiskImage> image = DiskImage::Open(target.image,
				target.readOnly, error);
			if (!image)
				return false;
			fScsi->AttachDevice(id, std::unique_ptr<ScsiDevice>(
				new ScsiDisk(std::move(image), "QUANTUM", "LP240S")));
		} else if (target.kind == MachineConfig::ScsiTarget::kCdRom) {
			std::unique_ptr<ScsiCd> cd(new ScsiCd("TOSHIBA",
				"CD-ROM XM-3401TA"));
			if (!target.image.empty()) {
				std::unique_ptr<CdImage> image = CdImage::Open(target.image,
					error);
				if (!image)
					return false;
				cd->Insert(std::move(image));
			}
			fScsi->AttachDevice(id, std::move(cd));
		}
	}

	fFloppy.reset(new Fdc82077(fScheduler, fTicksPerSecond, *fBoard));
	fFloppy->SetResetPolling(false);
	fIo->Map(0x3f2, 4, fFloppy.get());
	fIo->Map(0x3f7, 1, fFloppy.get());
	fFloppy->SetIrqCallback([this](bool level) {
		fBoard->SetIoInterrupt(HobbitBoard::kIoFloppy, level);
	});
	if (!fConfig.floppyImage.empty()) {
		std::unique_ptr<DiskImage> image = DiskImage::Open(
			fConfig.floppyImage, fConfig.floppyWriteProtected, error);
		if (!image || !fFloppy->InsertDisk(FloppyUnit(), std::move(image),
				fConfig.floppyWriteProtected, error))
			return false;
	}

	// the video card in slot 5
	fVideo.reset(new HobbitVideo(*fBus, fScheduler, fTicksPerSecond));

	fUnemulated.reset(new UnemulatedHobbitPorts);
	fIo->Map(0x378, 3, fUnemulated.get());	// parallel port
	fIo->Map(0x222, 1, fUnemulated.get());	// joystick controller
	fIo->Map(0x226, 1, fUnemulated.get());
	fIo->Map(0x17, 2, fUnemulated.get());	// joystick registers

	for (int i = 0; i < 2; i++) {
		HobbitCpu::Config cpuConfig;
		cpuConfig.index = i;
		cpuConfig.ticksPerCycle = kTicksPerCycle;
		cpuConfig.ticksPerInstruction = std::max<u32>(1,
			kTicksPerCycle * fConfig.cpiPercent / 100);
		fCpus[i].reset(new HobbitCpu(*fBus, cpuConfig));
	}
	fBoard->AttachCpus(fCpus[0].get(), fCpus[1].get());

	_LoadNvram();
	if (!_LoadKernel(error))
		return false;
	u32 hint = fScheduleHint;
	for (auto& cpu : fCpus) {
		cpu->SetIdleLoop(fIdlePc, [hint](HobbitCpu& cpu) {
			return hint == 0 || cpu.DebugRead32(hint) == 0;
		});
	}
	PowerOn();
	return true;
}


bool
HobbitMachine::_ReadKernel(std::vector<u8>& data, std::string& error)
{
	if (!fConfig.hobbitKernelPath.empty()) {
		if (!ReadFile(fConfig.hobbitKernelPath, data)) {
			error = "cannot read the kernel " + fConfig.hobbitKernelPath;
			return false;
		}
		return true;
	}
	// the boot volume: the floppy, else the first SCSI disk
	std::vector<std::string> volumes;
	if (!fConfig.floppyImage.empty())
		volumes.push_back(fConfig.floppyImage);
	for (int id = 0; id < 7; id++) {
		if (fConfig.scsi[id].kind == MachineConfig::ScsiTarget::kDisk
			&& !fConfig.scsi[id].image.empty())
			volumes.push_back(fConfig.scsi[id].image);
	}
	if (volumes.empty()) {
		error = "no boot volume: a Hobbit needs a SCSI disk or a floppy "
			"with /system/kernel (or a kernel file)";
		return false;
	}
	std::string reasons;
	for (const std::string& path : volumes) {
		std::string openError;
		std::unique_ptr<DiskImage> image = DiskImage::Open(path, true,
			openError);
		if (!image) {
			reasons += "\n" + openError;
			continue;
		}
		HobbitVolume volume(*image);
		std::string volumeError;
		if (volume.Mount(volumeError)
			&& volume.ReadFile("system/kernel", data, volumeError)) {
			BB_LOG(kLogMachine, kLogInfo, "kernel from %s (volume \"%s\")",
				path.c_str(), volume.Name().c_str());
			return true;
		}
		reasons += "\n" + path + ": " + volumeError;
	}
	error = "no Hobbit kernel found:" + reasons;
	return false;
}


// Reads the kernel; PowerOn() copies it into memory.
bool
HobbitMachine::_LoadKernel(std::string& error)
{
	std::vector<u8> data;
	if (!_ReadKernel(data, error))
		return false;
	if (data.size() < 52 || memcmp(data.data(), "\x7f" "ELF", 4) != 0
		|| data[4] != 1 || data[5] != 2
		|| LoadBE16(&data[18]) != kElfMachineHobbit) {
		error = "the kernel is not a Hobbit ELF file";
		return false;
	}
	fKernelEntry = LoadBE32(&data[24]);
	u32 programHeaders = LoadBE32(&data[28]);
	u16 entrySize = LoadBE16(&data[42]);
	u16 count = LoadBE16(&data[44]);
	fKernelImage.clear();
	for (u16 i = 0; i < count; i++) {
		u64 offset = programHeaders + (u64)i * entrySize;
		if (offset + 32 > data.size()) {
			error = "the kernel's program headers are damaged";
			return false;
		}
		const u8* header = &data[offset];
		if (LoadBE32(header) != 1)		// PT_LOAD
			continue;
		Segment segment;
		u32 fileOffset = LoadBE32(header + 4);
		segment.address = LoadBE32(header + 8);
		u32 fileSize = LoadBE32(header + 16);
		segment.memorySize = LoadBE32(header + 20);
		if ((u64)fileOffset + fileSize > data.size()
			|| segment.address < kRamBase
			|| (u64)segment.address + segment.memorySize
				> (u64)kRamBase + fRamSize
			|| fileSize > segment.memorySize) {
			error = "a kernel segment does not fit in memory";
			return false;
		}
		segment.data.assign(data.begin() + fileOffset,
			data.begin() + fileOffset + fileSize);
		fKernelImage.push_back(segment);
	}
	if (fKernelImage.empty()) {
		error = "the kernel has nothing to load";
		return false;
	}
	BB_LOG(kLogMachine, kLogInfo, "kernel: %zu segments, entry %08x",
		fKernelImage.size(), fKernelEntry);
	_FindIdleLoop(data);
	return true;
}


// The idle loop is the last backward jump of _null_thread (November 1994
// kernels, interrupts schedule) or _null_process (March 1994, which polls
// _sc_hint between spins). Stripped kernels (the installer's) have
// _null_thread's loop found by its code: CALL _fix_3_minute_bug (PC
// relative), ADD $1,R16, JMP back.
void
HobbitMachine::_FindIdleLoop(const std::vector<u8>& elf)
{
	fIdlePc = 1;
	fScheduleHint = 0;
	u32 sectionHeaders = LoadBE32(&elf[32]);
	u16 entrySize = LoadBE16(&elf[46]);
	u16 count = LoadBE16(&elf[48]);
	if ((u64)sectionHeaders + (u64)count * entrySize > elf.size()
		|| entrySize < 40)
		return;
	u32 idleStart = 0, idleSize = 0;
	for (u16 i = 0; i < count; i++) {
		const u8* header = &elf[sectionHeaders + i * entrySize];
		if (LoadBE32(header + 4) != 2)		// SHT_SYMTAB
			continue;
		u32 offset = LoadBE32(header + 16);
		u32 size = LoadBE32(header + 20);
		u32 link = LoadBE32(header + 24);
		if (link >= count || (u64)offset + size > elf.size())
			return;
		const u8* strings = &elf[sectionHeaders + link * entrySize];
		u32 stringOffset = LoadBE32(strings + 16);
		u32 stringSize = LoadBE32(strings + 20);
		if ((u64)stringOffset + stringSize > elf.size())
			return;
		for (u32 symbol = 0; symbol + 16 <= size; symbol += 16) {
			const u8* entry = &elf[offset + symbol];
			u32 name = LoadBE32(entry);
			if (name >= stringSize)
				continue;
			const char* text = (const char*)&elf[stringOffset + name];
			size_t length = strnlen(text, stringSize - name);
			std::string symbolName(text, length);
			if (symbolName == "_null_thread"
				|| symbolName == "_null_process") {
				idleStart = LoadBE32(entry + 4);
				idleSize = LoadBE32(entry + 8);
			} else if (symbolName == "_sc_hint")
				fScheduleHint = LoadBE32(entry + 4);
		}
	}
	if (idleStart == 0) {
		static const u16 kLoop[] = { 0x80e1, 0, 0, 0x5024, 0x0ffc };
		for (const Segment& segment : fKernelImage) {
			const std::vector<u8>& code = segment.data;
			for (size_t at = 0; at + 10 <= code.size(); at += 2) {
				if (LoadBE16(&code[at]) == kLoop[0]
					&& LoadBE16(&code[at + 6]) == kLoop[3]
					&& LoadBE16(&code[at + 8]) == kLoop[4]) {
					fIdlePc = segment.address + (u32)at;
					BB_LOG(kLogMachine, kLogInfo, "idle loop at %08x (found "
						"by its code)", fIdlePc);
					return;
				}
			}
		}
		return;
	}
	// the function's code, from the loaded segments
	for (const Segment& segment : fKernelImage) {
		if (idleStart < segment.address
			|| (u64)idleStart + idleSize > segment.address
				+ segment.data.size())
			continue;
		const u8* code = segment.data.data() + (idleStart - segment.address);
		for (u32 at = 0; at + 2 <= idleSize; at += 2) {
			u16 parcel = LoadBE16(code + at);
			// a one parcel JMP backwards
			if ((parcel & 0xfc00) == 0x0c00 && (parcel & 0x200) != 0) {
				s32 offset = (s32)((parcel & 0x3ff) | 0xfffffc00) * 2;
				fIdlePc = idleStart + at + offset;
			}
		}
	}
	BB_LOG(kLogMachine, kLogInfo, "idle loop at %08x%s", fIdlePc,
		fScheduleHint != 0 ? " (polls a scheduling hint)" : "");
}


bool
HobbitMachine::_LoadNvram()
{
	std::vector<u8> data;
	if (fConfig.nvramPath.empty() || !ReadFile(fConfig.nvramPath, data)
		|| data.size() < (size_t)Rtc146818::kRamSize + 8)
		return false;
	fRtc->LoadRam(data.data());
	s64 offset;
	memcpy(&offset, &data[Rtc146818::kRamSize], 8);
	fRtc->SetTimeOffset(offset);
	return true;
}


bool
HobbitMachine::SavePersistentState(std::string& error)
{
	if (fConfig.nvramPath.empty() || !fRtc->IsDirty())
		return true;
	std::vector<u8> data(Rtc146818::kRamSize + 8);
	memcpy(data.data(), fRtc->Ram(), Rtc146818::kRamSize);
	s64 offset = fRtc->TimeOffset();
	memcpy(&data[Rtc146818::kRamSize], &offset, 8);
	if (!WriteFile(fConfig.nvramPath, data.data(), data.size())) {
		error = "cannot write " + fConfig.nvramPath;
		return false;
	}
	fRtc->ClearDirty();
	return true;
}


void
HobbitMachine::SetPersistentPaths(const std::string& flash,
	const std::string& nvram)
{
	fConfig.nvramPath = nvram;
	std::string error;
	// written now: the new profile gets the clock and its settings
	if (!nvram.empty()) {
		std::vector<u8> data(Rtc146818::kRamSize + 8);
		memcpy(data.data(), fRtc->Ram(), Rtc146818::kRamSize);
		s64 offset = fRtc->TimeOffset();
		memcpy(&data[Rtc146818::kRamSize], &offset, 8);
		WriteFile(nvram, data.data(), data.size());
	}
}


void
HobbitMachine::PowerOn()
{
	BB_LOG(kLogMachine, kLogInfo, "power on reset");
	memset(fRam.get(), 0, fRamSize);
	memset(fDspRam.get(), 0, kDspRamSize);
	fRtc->Reset();
	fKeyboard->Reset();
	fScsi->Reset();
	fFloppy->Reset();
	fVideo->Reset();
	for (auto& uart : fUarts)
		uart->Reset();
	for (auto& cpu : fCpus) {
		cpu->SetNow(fNow);
		cpu->Reset();
	}
	fBoard->PowerOnReset();

	// the boot ROM's work
	for (const Segment& segment : fKernelImage) {
		u8* target = fRam.get() + (segment.address - kRamBase);
		memcpy(target, segment.data.data(), segment.data.size());
		memset(target + segment.data.size(), 0,
			segment.memorySize - segment.data.size());
	}
	StoreBE32(fRam.get() + (kMemorySizeAddress - kRamBase), fRamSize);
	_EnterKernel(0);
}


// CPU 'cpu' leaves reset: as the boot ROM would, it enters the kernel on
// a boot stack of its own at the top of memory.
void
HobbitMachine::_EnterKernel(int cpu)
{
	HobbitCpu& target = *fCpus[cpu];
	target.SetPc(fKernelEntry);
	// The kernel runs on the SP and soon moves both stacks to the last
	// page of memory, which it keeps for them ("boot stacks": CPU 1 at the
	// top, CPU 2 below 0x800). These temporary ones are in that page too,
	// clear of where the kernel builds its own.
	u32 top = kRamBase + fRamSize - (cpu == 0 ? 0x300 : 0xb00);
	target.SetRegister(HobbitCpu::kRegIsp, top);
	target.SetRegister(HobbitCpu::kRegSp, top - 0x100);
	target.SetRegister(HobbitCpu::kRegShad, top - 0x100);
	target.SetRegister(HobbitCpu::kRegPsw, HobbitCpu::kPswUseSp);
	// the caches on, as the boot ROM leaves them
	target.SetRegister(HobbitCpu::kRegConfig, kBootConfig);
	BB_LOG(kLogMachine, kLogInfo, "CPU %d enters the kernel at %08x",
		cpu + 1, fKernelEntry);
}


VgaCore*
HobbitMachine::Video() const
{
	return fVideo.get();
}


void
HobbitMachine::SetVideoSink(VideoSink* sink)
{
	fVideo->SetVideoSink(sink);
}


void
HobbitMachine::PressInterruptSwitch()
{
	BB_LOG(kLogMachine, kLogInfo, "interrupt switch pressed");
	fBoard->SetNmi(true);
	if (!fSwitchTimer) {
		fSwitchTimer.reset(new Timer(fScheduler, [this]() {
			fBoard->SetNmi(false);
		}, "interrupt-switch"));
	}
	fSwitchTimer->ArmIn(fTicksPerSecond / 20);
}


void
HobbitMachine::SetSerialBackend(int port, SerialBackend* backend)
{
	if (port >= 0 && port < kSerialPorts)
		fUarts[port]->SetBackend(backend);
}


void
HobbitMachine::RunFor(u64 ticks)
{
	u64 target = fNow + ticks;
	while (fNow < target) {
		u64 sliceEnd = std::min(target, fNow + fQuantum);
		u64 next = fScheduler.NextEventTime();
		if (next < sliceEnd)
			sliceEnd = std::max(next, fNow + 1);
		for (auto& cpu : fCpus) {
			if (cpu->Now() < fNow)
				cpu->SetNow(fNow);
			fScheduler.SetClock(cpu->NowPointer());
			cpu->Run(sliceEnd);
		}
		fScheduler.SetClock(nullptr);
		fNow = sliceEnd;
		fScheduler.SetNow(fNow);
		fScheduler.RunDue(fNow);
	}
}


u64
HobbitMachine::InstructionCount() const
{
	return fCpus[0]->InstructionCount() + fCpus[1]->InstructionCount();
}


void
HobbitMachine::SampleLeds(float& left, float& right, float& disk)
{
	left = right = disk = 0;
}


bool
HobbitMachine::CpuActive(int cpu) const
{
	return cpu >= 0 && cpu < 2 && fBoard->CpuRunning(cpu);
}


u64
HobbitMachine::UnclaimedIoCount() const
{
	return fIo->UnclaimedCount();
}


std::string
HobbitMachine::CpuState(int index) const
{
	HobbitCpu& cpu = *fCpus[index];
	char line[200];
	snprintf(line, sizeof(line), "%s pc %08x psw %08x isp %08x sp %08x "
		"vb %08x, %llu instructions", cpu.IsHeldInReset() ? "held in reset"
			: "running", cpu.Pc(), cpu.Psw(), cpu.Register(HobbitCpu::kRegIsp),
		cpu.Register(HobbitCpu::kRegSp), cpu.Register(HobbitCpu::kRegVb),
		(unsigned long long)cpu.InstructionCount());
	return line;
}


bool
HobbitMachine::DebugTranslate(int cpu, u32 va, u32& pa)
{
	return fCpus[cpu]->DebugTranslate(va, pa);
}


void
HobbitMachine::SetTrace(FILE* file, u64 count, u64 start, int cpuMask,
	const std::vector<u32>& pcs)
{
	struct State {
		u64 remaining;
	};
	std::shared_ptr<State> state(new State{count});
	std::shared_ptr<std::vector<u32> > filter(new std::vector<u32>(pcs));
	u64 ticksPerSecond = fTicksPerSecond;
	for (int i = 0; i < 2; i++) {
		if ((cpuMask & (1 << i)) == 0)
			continue;
		fCpus[i]->SetTraceHook([=](HobbitCpu& cpu, u32 pc) {
			if (state->remaining == 0 || cpu.Now() < start)
				return;
			if (!filter->empty() && std::find(filter->begin(),
					filter->end(), pc) == filter->end())
				return;
			state->remaining--;
			int length;
			std::string text = HobbitDisassemble(pc, [&cpu](u32 address) {
				u32 word = cpu.DebugRead32(address & ~3u);
				return (u16)((address & 2) != 0 ? word : word >> 16);
			}, length);
			u32 csp = cpu.Csp();
			fprintf(file, "[%11.6f] cpu%d %08x: %-32s csp %08x psw %08x "
				"R4 %08x R8 %08x\n", (double)cpu.Now() / ticksPerSecond,
				cpu.Index(), pc, text.c_str(), csp, cpu.Psw(),
				cpu.DebugRead32(csp + 4), cpu.DebugRead32(csp + 8));
		});
	}
}

}	// namespace bebox

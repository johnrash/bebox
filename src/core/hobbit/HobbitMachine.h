/*
 * BeBox emulator - the Hobbit BeBox prototype: two AT&T 92010 (Hobbit)
 * processors at 25 MHz on the BeBus.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Physical memory: the boot flash at 0 (512 KB), the I/O window at
 * 0x00800000 (HobbitBoard), RAM at 0x08000000 (4, 8, 16, 20 or 32 MB),
 * the DSPs' 2 MB at 0x60000000, slot n (1-6) at 0x80000000 + n * 2^28.
 *
 * No boot ROM of these machines survives, so the machine does its job:
 * at power on it loads /system/kernel (an ELF file) from the boot volume
 * into memory, stores the memory size where the kernel looks for it
 * (0x08000048) and starts CPU 1 at the kernel's entry point on a boot
 * stack at the top of memory. When the kernel releases CPU 2 from reset,
 * that one enters the kernel the same way.
 *
 * Time: 4 machine ticks per 25 MHz clock cycle.
 */
#ifndef BEBOX_HOBBIT_MACHINE_H
#define BEBOX_HOBBIT_MACHINE_H

#include "../board/Machine.h"
#include "../board/MachineConfig.h"
#include "../board/Scheduler.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace bebox {

class HobbitBoard;
class HobbitCpu;
class HobbitVideo;
class IoSpace;
class Ncr53c94;
class Uart16550;
class UnemulatedHobbitPorts;

class HobbitMachine : public Machine {
public:
	static const u32 kClockHz = 25000000;
	static const u32 kTicksPerCycle = 4;
	static const u32 kRamBase = 0x08000000;
	static const u32 kMemorySizeAddress = 0x08000048;
	static const int kSerialPorts = 2;

	explicit					HobbitMachine(const MachineConfig& config);
	virtual						~HobbitMachine();

	virtual	bool				Init(std::string& error);
	virtual	const MachineConfig& Config() const { return fConfig; }

	virtual	void				PowerOn();
	virtual	void				PressInterruptSwitch();
	virtual	bool				SavePersistentState(std::string& error);
	virtual	void				SetPersistentPaths(const std::string& flash,
									const std::string& nvram);

	virtual	void				RunFor(u64 ticks);
	virtual	u64					Now() const { return fNow; }
	virtual	u64					TicksPerSecond() const
									{ return fTicksPerSecond; }
	virtual	u64					InstructionCount() const;

	virtual	PhysBus&			Bus() { return *fBus; }
	virtual	Scheduler&			GetScheduler() { return fScheduler; }
	virtual	Kbc8042&			Keyboard() { return *fKeyboard; }
	virtual	Rtc146818&			Rtc() { return *fRtc; }
	virtual	Fdc82077*			Floppy() { return fFloppy.get(); }
			// (the kernel's floppy driver uses unit 1)
	virtual	int					FloppyUnit() const { return 1; }
	virtual	JoystickAdc*		GamePorts() { return nullptr; }
	virtual	VgaCore*			Video() const;

	virtual	int					SerialPortCount() const { return kSerialPorts; }
	virtual	void				SetSerialBackend(int port,
									SerialBackend* backend);
	virtual	void				SetNetworkBackend(NetworkBackend* backend) {}
	virtual	bool				HasEthernet() const { return false; }
	virtual	void				SetVideoSink(VideoSink* sink);
	virtual	void				SetAudioSink(AudioSink* sink) {}

	virtual	bool				HasLeds() const { return false; }
	virtual	void				SampleLeds(float& left, float& right,
									float& disk);
	virtual	bool				CpuActive(int cpu) const;
	virtual	u64					UnclaimedIoCount() const;
	virtual	int					CpuCount() const { return 2; }
	virtual	std::string			CpuState(int cpu) const;
	virtual	bool				DebugTranslate(int cpu, u32 va, u32& pa);

			HobbitCpu*			Cpu(int index) { return fCpus[index].get(); }
			HobbitBoard&		Board() { return *fBoard; }

			// Instruction trace: 'count' instructions from 'start' (ticks)
			// on, of the CPUs in 'cpuMask', all of them or only those at
			// 'pcs'.
			void				SetTrace(FILE* file, u64 count, u64 start,
									int cpuMask, const std::vector<u32>& pcs);

protected:
	virtual	ScsiDevice*			ScsiTarget(int id);

private:
	// a loadable segment of the kernel's ELF file
	struct Segment {
		u32				address;
		u32				memorySize;
		std::vector<u8>	data;
	};

			bool				_LoadKernel(std::string& error);
			void				_FindIdleLoop(const std::vector<u8>& elf);
			bool				_ReadKernel(std::vector<u8>& data,
									std::string& error);
			void				_EnterKernel(int cpu);
			bool				_LoadNvram();

			MachineConfig		fConfig;
			u64					fTicksPerSecond;
			u64					fQuantum;
			u64					fNow;
			Scheduler			fScheduler;
			std::unique_ptr<PhysBus> fBus;
			std::unique_ptr<IoSpace> fIo;
			std::unique_ptr<u8[]> fRam;
			u32					fRamSize;
			std::unique_ptr<u8[]> fDspRam;
			std::vector<u8>		fFlash;
			std::unique_ptr<HobbitBoard> fBoard;
			std::unique_ptr<Rtc146818> fRtc;
			std::unique_ptr<Kbc8042> fKeyboard;
			std::unique_ptr<Uart16550> fUarts[kSerialPorts];
			std::unique_ptr<Ncr53c94> fScsi;
			std::unique_ptr<Fdc82077> fFloppy;
			std::unique_ptr<HobbitVideo> fVideo;
			std::unique_ptr<UnemulatedHobbitPorts> fUnemulated;
			std::unique_ptr<HobbitCpu> fCpus[2];
			std::unique_ptr<Timer> fSwitchTimer;
			std::vector<Segment> fKernelImage;
			u32					fKernelEntry;
			// the idle loop's head and the scheduling hint it polls (0:
			// none)
			u32					fIdlePc;
			u32					fScheduleHint;
};

}	// namespace bebox

#endif	// BEBOX_HOBBIT_MACHINE_H

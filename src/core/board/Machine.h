/*
 * BeBox emulator - an emulated machine, as the front ends see it.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Two kinds: the BeBox (BeBoxMachine: PowerPC 603/603e) and the Hobbit
 * BeBox prototype (HobbitMachine: AT&T 92010). Time is counted in machine
 * ticks, whose rate depends on the kind (TicksPerSecond()).
 *
 * All methods run on the emulation thread.
 */
#ifndef BEBOX_MACHINE_H
#define BEBOX_MACHINE_H

#include "MachineConfig.h"
#include "../Types.h"

#include <memory>
#include <string>

namespace bebox {

class AudioSink;
class Fdc82077;
class JoystickAdc;
class Kbc8042;
class NetworkBackend;
class PhysBus;
class Rtc146818;
class Scheduler;
class SerialBackend;
class VgaCore;
class VideoSink;

class Machine {
public:
	// A BeBoxMachine or a HobbitMachine, from config.model.
	static	std::unique_ptr<Machine> Create(const MachineConfig& config);

	virtual						~Machine() {}

	virtual	bool				Init(std::string& error) = 0;
	virtual	const MachineConfig& Config() const = 0;

			// Power on / hard reset (both front panel switches).
	virtual	void				PowerOn() = 0;
			// Front panel Interrupt switch (pressed for 50 ms).
	virtual	void				PressInterruptSwitch() = 0;
			// Saves the flash and NVRAM if they changed.
	virtual	bool				SavePersistentState(std::string& error) = 0;
			// Keeps the flash and NVRAM in other files from now on (a
			// profile copied while the machine runs); the next save writes
			// both there.
	virtual	void				SetPersistentPaths(const std::string& flash,
									const std::string& nvram) = 0;

			// Advances the emulation by 'ticks' machine ticks.
	virtual	void				RunFor(u64 ticks) = 0;
	virtual	u64					Now() const = 0;
	virtual	u64					TicksPerSecond() const = 0;
			u64					MicrosecondsToTicks(u64 us) const
									{ return us * TicksPerSecond()
										/ 1000000; }
	virtual	u64					InstructionCount() const = 0;

	virtual	PhysBus&			Bus() = 0;
	virtual	Scheduler&			GetScheduler() = 0;
	virtual	Kbc8042&			Keyboard() = 0;
	virtual	Rtc146818&			Rtc() = 0;
			// null when the machine has none
	virtual	Fdc82077*			Floppy() = 0;
			// The internal floppy drive's unit number on the controller.
	virtual	int					FloppyUnit() const { return 0; }
	virtual	JoystickAdc*		GamePorts() = 0;
	virtual	VgaCore*			Video() const = 0;

	virtual	int					SerialPortCount() const = 0;
	virtual	void				SetSerialBackend(int port,
									SerialBackend* backend) = 0;
	virtual	void				SetNetworkBackend(NetworkBackend* backend)
									= 0;
	virtual	bool				HasEthernet() const = 0;
	virtual	void				SetVideoSink(VideoSink* sink) = 0;
	virtual	void				SetAudioSink(AudioSink* sink) = 0;

			// Front panel lights: the CPU load bar graphs (levels 0-15)
			// and the disk light (duty cycle), sampled since the last call.
	virtual	bool				HasLeds() const = 0;
	virtual	void				SampleLeds(float& left, float& right,
									float& disk) = 0;
	virtual	bool				CpuActive(int cpu) const = 0;
			// Diagnostics: I/O accesses nothing claimed; the processors.
	virtual	u64					UnclaimedIoCount() const = 0;
	virtual	int					CpuCount() const = 0;
			// One line: running, halted or held in reset, the program
			// counter and the main control registers.
	virtual	std::string			CpuState(int cpu) const = 0;
			// The physical address of a virtual one, as the CPU sees it.
	virtual	bool				DebugTranslate(int cpu, u32 va, u32& pa) = 0;
};

}	// namespace bebox

#endif	// BEBOX_MACHINE_H

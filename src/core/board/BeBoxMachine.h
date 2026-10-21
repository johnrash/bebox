/*
 * BeBox emulator - the BeBox: processors, buses and devices.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Time is counted in machine ticks, 8 per bus clock (264 MHz with the
 * 33 MHz bus): a 66 MHz CPU cycle is 4 ticks, a 133 MHz cycle 2 ticks, a
 * timebase/decrementer tick (bus / 4) is 32 ticks.
 *
 * All methods run on the emulation thread.
 */
#ifndef BEBOX_BEBOX_MACHINE_H
#define BEBOX_BEBOX_MACHINE_H

#include "Machine.h"
#include "MachineConfig.h"
#include "Scheduler.h"
#include "../Types.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace bebox {

class Aip82091;
class BeBoxBoard;
class CirrusGd5434;
class S3Vision864;
class Fdc82077;
class Flash29F0x0;
class IdeController;
class Kbc8042;
class Ncr53c810;
class UnemulatedPorts;
class JoystickAdc;
class NetworkBackend;
class Ne2000;
class IoSpace;
class Mpc105;
class PciBus;
class PhysBus;
class PpcCpu;
class Rtc146818;
class SerialBackend;
class VgaCore;
class VideoSink;
class AudioSink;
class Cs4231a;
class Sio82378;
class Uart16550;

class BeBoxMachine : public Machine {
public:
	static const int kTicksPerBusCycle = 8;
	static const int kSerialPorts = 4;		// serial1-4 (midi1/2 extra)
	// F1 held at most this long after the memory test
	static constexpr double kDebugKeyMaxSeconds = 8;

	explicit					BeBoxMachine(const MachineConfig& config);
	virtual						~BeBoxMachine();

	virtual	bool				Init(std::string& error);
	virtual	const MachineConfig& Config() const { return fConfig; }

	virtual	void				PowerOn();
	virtual	void				PressInterruptSwitch();
	virtual	void				HoldDebugOutputKey();
	virtual	bool				SavePersistentState(std::string& error);
	virtual	void				SetPersistentPaths(const std::string& flash,
									const std::string& nvram);

	virtual	void				RunFor(u64 ticks);
	virtual	u64					Now() const { return fNow; }
	virtual	u64					TicksPerSecond() const
									{ return fTicksPerSecond; }
	virtual	u64					InstructionCount() const;

			// Components
	virtual	PhysBus&			Bus() { return *fBus; }
			IoSpace&			Io() { return *fIo; }
			PciBus&				Pci() { return *fPci; }
	virtual	Scheduler&			GetScheduler() { return fScheduler; }
	virtual	int					CpuCount() const { return (int)fCpus.size(); }
			PpcCpu*				Cpu(int index)
									{ return index < (int)fCpus.size()
										? fCpus[index].get() : nullptr; }
			BeBoxBoard&			Board() { return *fBoard; }
			Mpc105&				Bridge() { return *fBridge; }
			Sio82378&			Sio() { return *fSio; }
	virtual	Rtc146818&			Rtc() { return *fRtc; }
			Flash29F0x0&		Flash() { return *fFlash; }
	virtual	Kbc8042&			Keyboard() { return *fKeyboard; }
			IdeController&		Ide() { return *fIde; }
	virtual	Fdc82077*			Floppy() { return fFloppy.get(); }
	virtual	JoystickAdc*		GamePorts() { return fAdc.get(); }
			Ncr53c810&			Scsi() { return *fScsi; }
			Uart16550*			Serial(int index)
									{ return fUarts[index].get(); }
			u32					RamSize() const { return fConfig.ramMB << 20; }

	virtual	int					SerialPortCount() const { return kSerialPorts; }
	virtual	void				SetSerialBackend(int port,
									SerialBackend* backend);
			// The Ethernet card's host side (when the card is enabled).
	virtual	void				SetNetworkBackend(NetworkBackend* backend);
	virtual	bool				HasEthernet() const
									{ return fEthernet != nullptr; }
	virtual	void				SetVideoSink(VideoSink* sink);
	virtual	void				SetAudioSink(AudioSink* sink);
			Cs4231a&			Sound() { return *fSound; }
	virtual	VgaCore*			Video() const { return fVideo; }

	virtual	bool				HasLeds() const { return true; }
	virtual	void				SampleLeds(float& left, float& right,
									float& disk);
	virtual	bool				CpuActive(int cpu) const;
	virtual	u64					UnclaimedIoCount() const;
	virtual	std::string			CpuState(int cpu) const;
	virtual	bool				DebugTranslate(int cpu, u32 va, u32& pa);

			// ISA interrupt request from a device: drives the 8259 and the
			// Be interrupt controller bit wired to the line.
			void				SetIsaIrq(int irq, bool level);

protected:
	virtual	ScsiDevice*			ScsiTarget(int id);

private:
			bool				_LoadRoms(std::string& error);
			bool				_LoadNvram();
			void				_SetupMemory();
			void				_RequestPowerOn();
			void				_StartWorker();
			void				_StopWorker();
			void				_RunWorker();
			void				_RunSliceThreaded(u64 sliceEnd);
			void				_CheckKernelEntry();
			void				_DebugOutputStarted();

			MachineConfig		fConfig;
			bool				fSaveAll;		// next save: even if clean
			u64					fTicksPerSecond;
			u64					fQuantum;
			u64					fNow;
			Scheduler			fScheduler;
			std::vector<std::unique_ptr<u8[]> > fRamBanks;
			std::vector<u32>	fRamBankSizes;
			std::unique_ptr<PhysBus> fBus;
			std::unique_ptr<IoSpace> fIo;
			std::unique_ptr<PciBus> fPci;
			std::unique_ptr<Mpc105> fBridge;
			std::unique_ptr<Sio82378> fSio;
			std::unique_ptr<BeBoxBoard> fBoard;
			std::unique_ptr<Rtc146818> fRtc;
			std::unique_ptr<Flash29F0x0> fFlash;
			std::unique_ptr<Kbc8042> fKeyboard;
			std::unique_ptr<IdeController> fIde;
			std::unique_ptr<Fdc82077> fFloppy;
			std::unique_ptr<CirrusGd5434> fCirrus;
			std::unique_ptr<S3Vision864> fS3;
			std::unique_ptr<Ncr53c810> fScsi;
			VgaCore*			fVideo = nullptr;
			std::unique_ptr<Aip82091> fAip;
			std::unique_ptr<UnemulatedPorts> fUnemulated;
			std::unique_ptr<JoystickAdc> fAdc;
			std::unique_ptr<Ne2000> fEthernet;
			std::unique_ptr<Cs4231a> fSound;
			std::unique_ptr<Uart16550> fUarts[6];
			std::vector<std::unique_ptr<PpcCpu> > fCpus;
			std::unique_ptr<Timer> fSwitchTimer;
			// Watches for the jump into the kernel (BootItems.h).
			std::unique_ptr<Timer> fKernelEntryTimer;
			// F1 held for the boot ROM until it writes to serial 4
			bool				fDebugKeyHeld = false;

			// Threaded mode (MachineConfig::threadPerCpu): CPU 1 runs on
			// a worker thread, CPU 0 on the thread calling RunFor().
			struct Worker;
			std::unique_ptr<Worker> fWorker;
			// a reset asked for by a device while the CPUs run
			std::atomic<bool>	fPowerOnRequested;
};

}	// namespace bebox

#endif	// BEBOX_BEBOX_MACHINE_H

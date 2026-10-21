/*
 * BeBox emulator - the BeBox: processors, buses and devices.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "BeBoxMachine.h"

#include "BeBoxBoard.h"
#include "BootItems.h"
#include "Flash29F0x0.h"
#include "ThreadGovernor.h"
#include "../Log.h"
#include "../Spin.h"
#include "../bus/IoSpace.h"
#include "../bus/Mpc105.h"
#include "../bus/PciBus.h"
#include "../bus/PhysBus.h"
#include "../cpu/PpcCpu.h"
#include "../isa/Aip82091.h"
#include "../isa/Fdc82077.h"
#include "../isa/Cs4231a.h"
#include "../isa/IdeController.h"
#include "../isa/JoystickAdc.h"
#include "../isa/Ne2000.h"
#include "../isa/Kbc8042.h"
#include "../isa/Rtc146818.h"
#include "../isa/Sio82378.h"
#include "../isa/Uart16550.h"
#include "../pci/Ncr53c810.h"
#include "../storage/CdImage.h"
#include "../video/CirrusGd5434.h"
#include "../video/S3Vision864.h"

#include <algorithm>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace bebox {

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


// Threaded mode: the worker thread runs CPU 1 through the time slices
// the machine starts (generation numbers), spinning between them for a
// while, then sleeping. The governor chooses whether the slices in which
// both CPUs run go to the worker or stay on the machine's thread.
struct BeBoxMachine::Worker {
	std::thread					thread;
	u64							sliceEnd = 0;
	// The slice follows one on both threads: a long wait for it was the
	// machine's thread being slow (written before start).
	bool						continued = false;
	// When the worker waited for the slice (beyond a short spin; 0: it
	// did not) and noticed it (written before done).
	double						waitStart = 0;
	double						waitEnd = 0;
	std::atomic<u64>			start{0};		// slice generation to run
	std::atomic<u64>			done{0};		// last generation run
	std::atomic<bool>			sleeping{false};
	std::atomic<bool>			quit{false};
	std::mutex					lock;
	std::condition_variable		wake;
	ThreadGovernor				governor;
};

// A wait for the other thread longer than this many spins is timed.
static const int kWaitSpins = 256;


BeBoxMachine::BeBoxMachine(const MachineConfig& config)
	:
	fConfig(config),
	fSaveAll(false),
	fNow(0),
	fPowerOnRequested(false)
{
	fTicksPerSecond = (u64)config.busHz * kTicksPerBusCycle;
	fQuantum = (u64)config.quantumUs * fTicksPerSecond / 1000000;
	if (fQuantum == 0)
		fQuantum = 1;
}


BeBoxMachine::~BeBoxMachine()
{
	_StopWorker();
	fSwitchTimer.reset();
	// CPUs unregister from the bus; devices before the bus.
	fCpus.clear();
	for (auto& uart : fUarts)
		uart.reset();
	fCirrus.reset();
	fS3.reset();
	fEthernet.reset();
	fScsi.reset();
	fFlash.reset();
	fKeyboard.reset();
	fIde.reset();
	fFloppy.reset();
	fAip.reset();
	fUnemulated.reset();
	fRtc.reset();
	fBoard.reset();
	fSio.reset();
	fBridge.reset();
}


bool
BeBoxMachine::Init(std::string& error)
{
	fBus.reset(new PhysBus);
	fIo.reset(new IoSpace);
	fPci.reset(new PciBus);

	fBridge.reset(new Mpc105(*fBus, *fIo, *fPci));
	fPci->Attach(0, fBridge.get());
	_SetupMemory();

	fSio.reset(new Sio82378(*fIo, fScheduler, fTicksPerSecond));
	fPci->Attach(11, fSio.get());
	fBridge->SetInterruptAcknowledge([this]() {
		return fSio->InterruptAcknowledge();
	});
	fSio->Dma().SetMemoryAccess(
		[this](u32 address, void* buffer, u32 length) {
			fBridge->MasterRead(address, buffer, length);
		},
		[this](u32 address, const void* buffer, u32 length) {
			fBridge->MasterWrite(address, buffer, length);
		});
	fSio->SetResetCallback([this]() { _RequestPowerOn(); });

	fBoard.reset(new BeBoxBoard(*fBus, *fIo, fScheduler));
	fSio->SetInterruptCallback([this](bool level) {
		fBoard->SetSource(kBeIrq8259, level);
	});

	fRtc.reset(new Rtc146818(fScheduler, fTicksPerSecond));
	fIo->Map(0x70, 6, fRtc.get());
	fRtc->SetIrqCallback([this](bool level) { SetIsaIrq(8, level); });

	struct UartSetup {
		const char* name;
		u32 base;
		u32 clock;
		int isaIrq;
		int beBit;
	};
	static const UartSetup kUarts[6] = {
		{ "serial1", 0x3f8, 1843200, 4, -1 },
		{ "serial2", 0x2f8, 1843200, 3, -1 },
		{ "serial3", 0x380, 8000000, -1, kBeIrqSerial3 },
		{ "serial4", 0x388, 8000000, -1, kBeIrqSerial4 },
		{ "midi1", 0x3a0, 8000000, -1, kBeIrqMidi1 },
		{ "midi2", 0x3a8, 8000000, -1, kBeIrqMidi2 },
	};
	for (int i = 0; i < 6; i++) {
		const UartSetup& setup = kUarts[i];
		fUarts[i].reset(new Uart16550(setup.name, setup.base, setup.clock,
			fScheduler, fTicksPerSecond, setup.isaIrq >= 0));
		// The motherboard revision is wired to midi1's modem status
		// inputs (read through MSR bits 4-7 at 0x3a6).
		if (setup.base == 0x3a0) {
			fUarts[i]->SetHardwiredLines(fConfig.boardRevision == 3 ? 0xe0
				: fConfig.boardRevision == 5 ? 0x70 : 0x60);
		}
		fIo->Map(setup.base, 8, fUarts[i].get());
		if (setup.base == 0x388) {
			fUarts[i]->SetTransmitCallback([this](u8) {
				_DebugOutputStarted();
			});
		}
		if (setup.isaIrq >= 0) {
			int irq = setup.isaIrq;
			fUarts[i]->SetIrqCallback([this, irq](bool level) {
				SetIsaIrq(irq, level);
			});
		} else {
			int bit = setup.beBit;
			fUarts[i]->SetIrqCallback([this, bit](bool level) {
				fBoard->SetSource(bit, level);
			});
		}
	}

	fKeyboard.reset(new Kbc8042(fScheduler, fTicksPerSecond));
	fIo->Map(0x60, 1, fKeyboard.get());
	fIo->Map(0x64, 1, fKeyboard.get());
	fKeyboard->SetIrqCallback([this](int irq, bool level) {
		SetIsaIrq(irq, level);
	});
	fKeyboard->SetResetCallback([this]() { _RequestPowerOn(); });

	fIde.reset(new IdeController(fScheduler, fTicksPerSecond));
	fIo->Map(0x1f0, 8, fIde.get());
	fIo->Map(0x3f6, 1, fIde.get());
	fIde->SetIrqCallback([this](bool level) { SetIsaIrq(14, level); });
	fIde->SetActivityCallback([this](bool active) {
		if (active)
			fBoard->NoteDiskActivity();
	});
	for (int unit = 0; unit < 2; unit++) {
		if (fConfig.ideImage[unit].empty())
			continue;
		std::unique_ptr<DiskImage> image = DiskImage::Open(
			fConfig.ideImage[unit], fConfig.ideReadOnly[unit], error);
		if (!image)
			return false;
		fIde->AttachDevice(unit,
			std::unique_ptr<IdeDevice>(new AtaDisk(std::move(image))));
	}

	fFloppy.reset(new Fdc82077(fScheduler, fTicksPerSecond, fSio->Dma()));
	fIo->Map(0x3f0, 6, fFloppy.get());
	fIo->Map(0x3f7, 1, fFloppy.get());
	fFloppy->SetIrqCallback([this](bool level) { SetIsaIrq(6, level); });
	if (!fConfig.floppyImage.empty()) {
		std::unique_ptr<DiskImage> image = DiskImage::Open(
			fConfig.floppyImage, fConfig.floppyWriteProtected, error);
		if (!image || !fFloppy->InsertDisk(0, std::move(image),
				fConfig.floppyWriteProtected, error))
			return false;
	}

	fAip.reset(new Aip82091);
	fIo->Map(0x398, 2, fAip.get());
	fUnemulated.reset(new UnemulatedPorts);
	fAdc.reset(new JoystickAdc(fScheduler, fTicksPerSecond,
		fConfig.ioCardRevision));
	fIo->Map(0x360, 0x04, fAdc.get());
	fIo->Map(0x397, 0x01, fAdc.get());
	fIo->Map(0x36e, 0x02, fAdc.get());
	fIo->Map(0x364, 0x08, fUnemulated.get());	// IR

	if (fConfig.ethernet) {
		fEthernet.reset(new Ne2000(fScheduler, fTicksPerSecond,
			fConfig.ethernetMac));
		fIo->Map(fConfig.ethernetPort, Ne2000::kPorts, fEthernet.get());
		int irq = fConfig.ethernetIrq;
		fEthernet->SetIrqCallback([this, irq](bool level) {
			SetIsaIrq(irq, level);
		});
	}

	fSound.reset(new Cs4231a(fScheduler, fTicksPerSecond, fSio->Dma(),
		fConfig.soundPlaybackDma, fConfig.soundCaptureDma));
	fIo->Map(0x830, 4, fSound.get());
	fSound->SetIrqCallback([this](bool level) {
		fBoard->SetSource(kBeIrqSound, level);
	});
	fIo->Map(0x390, 0x07, fUnemulated.get());	// GeekPort D/A, digital
	fIo->Map(0x3bc, 0x03, fUnemulated.get());	// parallel port

	// SCSI: PCI device 12, Be interrupt bit 10.
	fScsi.reset(new Ncr53c810(*fBus, *fIo, *fBridge, fScheduler,
		fTicksPerSecond));
	fPci->Attach(12, fScsi.get());
	fScsi->SetIrqCallback([this](bool level) {
		fBoard->SetSource(kBeIrqScsi, level);
	});
	for (int id = 0; id < 7; id++) {
		const MachineConfig::ScsiTarget& target = fConfig.scsi[id];
		if (target.kind == MachineConfig::ScsiTarget::kDisk
			|| target.kind == MachineConfig::ScsiTarget::kSharedFolder) {
			std::unique_ptr<DiskImage> image = DiskImage::Open(target.image,
				target.readOnly, error);
			if (!image)
				return false;
			std::unique_ptr<ScsiDevice> disk(new ScsiDisk(std::move(image),
				"QUANTUM", "FIREBALL1080S"));
			disk->SetActivityCallback([this](bool) {
				fBoard->NoteDiskActivity();
			});
			fScsi->AttachDevice(id, std::move(disk));
		} else if (target.kind == MachineConfig::ScsiTarget::kCdRom) {
			// A Toshiba drive: the boot ROM knows how to boot from it.
			std::unique_ptr<ScsiCd> cd(new ScsiCd(fScheduler,
				fTicksPerSecond, "TOSHIBA", "CD-ROM XM-3401TA"));
			if (!target.image.empty()) {
				std::unique_ptr<CdImage> image = CdImage::Open(target.image,
					error);
				if (!image)
					return false;
				cd->Insert(std::move(image));
			}
			cd->SetActivityCallback([this](bool) {
				fBoard->NoteDiskActivity();
			});
			fScsi->AttachDevice(id, std::move(cd));
		}
	}

	// Video card in PCI slot 1 (device 13, Be interrupt bit 11).
	u32 vram = std::max<u32>(1, std::min<u32>(4, fConfig.vramMB)) << 20;
	if (fConfig.videoCard == MachineConfig::kVideoCirrus5434) {
		fCirrus.reset(new CirrusGd5434(*fBus, *fIo, fScheduler,
			fTicksPerSecond, vram));
		fPci->Attach(13, fCirrus.get());
		fVideo = fCirrus.get();
	} else if (fConfig.videoCard == MachineConfig::kVideoS3Vision864) {
		fS3.reset(new S3Vision864(*fBus, *fIo, fScheduler, fTicksPerSecond,
			vram));
		fPci->Attach(13, fS3.get());
		fVideo = fS3.get();
	}

	Flash29F0x0::Type flashType = fConfig.boardRevision <= 5
		? Flash29F0x0::kAm29F010 : Flash29F0x0::kAm29F040;
	fFlash.reset(new Flash29F0x0(*fBus, flashType, 0xfff00000, 0x100000));
	fFlash->SetWriteEnable([this]() {
		return fBridge->FlashWritesEnabled();
	});
	if (!_LoadRoms(error))
		return false;
	_LoadNvram();

	// Processors
	bool is133 = fConfig.model == MachineConfig::kBeBox133;
	u32 ticksPerCycle = is133 ? 2 : 4;
	for (int i = 0; i < std::max(1, std::min(2, fConfig.cpuCount)); i++) {
		PpcCpu::Config cpuConfig;
		cpuConfig.model = is133 ? PpcCpu::kPpc603e : PpcCpu::kPpc603;
		cpuConfig.pvr = fConfig.pvr != 0 ? fConfig.pvr
			: is133 ? 0x00060400 : 0x00030100;
		cpuConfig.hid1 = 0xa0000000;	// 603e PLL_CFG for 4:1
		cpuConfig.index = i;
		cpuConfig.ticksPerInstruction = std::max<u32>(1,
			ticksPerCycle * fConfig.cpiPercent / 100);
		cpuConfig.ticksPerTimebase = 4 * kTicksPerBusCycle;
		fCpus.emplace_back(new PpcCpu(*fBus, cpuConfig));
	}
	fBoard->AttachCpus(fCpus[0].get(), fCpus.size() > 1 ? fCpus[1].get()
		: nullptr);
	PowerOn();
	if (fConfig.threadPerCpu && fCpus.size() == 2)
		_StartWorker();
	return true;
}


// #pragma mark - threaded mode


void
BeBoxMachine::_StartWorker()
{
	fBus->SetThreaded(true);
	fWorker.reset(new Worker);
	fWorker->thread = std::thread([this]() { _RunWorker(); });
}


void
BeBoxMachine::_StopWorker()
{
	if (!fWorker)
		return;
	{
		std::lock_guard<std::mutex> lock(fWorker->lock);
		fWorker->quit = true;
	}
	fWorker->wake.notify_one();
	fWorker->thread.join();
	fWorker.reset();
	fBus->SetThreaded(false);
}


void
BeBoxMachine::_RunWorker()
{
	Worker& worker = *fWorker;
	PpcCpu& cpu = *fCpus[1];
	fScheduler.SetClock(cpu.NowPointer());
	u64 seen = 0;
	while (true) {
		u64 generation = 0;
		double waitStart = 0;
		for (int spins = 0;; spins++) {
			generation = worker.start.load(std::memory_order_acquire);
			if (generation != seen || worker.quit)
				break;
			if (spins == kWaitSpins && waitStart == 0)
				waitStart = ThreadGovernor::HostSeconds();
			if (spins < 2000) {
				CpuRelax();
				continue;
			}
			// idle (the emulation paused or paced, or both CPUs on the
			// machine's thread): sleep
			std::unique_lock<std::mutex> lock(worker.lock);
			worker.sleeping = true;
			worker.wake.wait(lock, [&]() {
				return worker.start.load() != seen || worker.quit;
			});
			worker.sleeping = false;
			spins = 0;
		}
		if (worker.quit)
			break;
		seen = generation;
		worker.waitStart = worker.continued ? waitStart : 0;
		if (worker.waitStart > 0)
			worker.waitEnd = ThreadGovernor::HostSeconds();
		cpu.Run(worker.sliceEnd);
		worker.done.store(generation, std::memory_order_release);
	}
	fScheduler.SetClock(nullptr);
}


// Both CPUs run to the end of the slice: at the same time, CPU 1 on the
// worker thread, or one after the other on this thread, as the governor
// chooses. Returns whether the slice ran on both threads.
bool
BeBoxMachine::_RunSliceThreaded(u64 sliceEnd, bool continued)
{
	ThreadGovernor& governor = fWorker->governor;
	bool twoThreads = governor.TwoThreads();
	if (!governor.Timing()) {
		if (twoThreads)
			_HandOver(sliceEnd, continued);
		else
			_RunSliceOneThread(sliceEnd);
		return twoThreads;
	}
	u64 instructions = InstructionCount();
	double start = ThreadGovernor::HostSeconds();
	if (twoThreads)
		_HandOver(sliceEnd, continued);
	else
		_RunSliceOneThread(sliceEnd);
	governor.SliceTimed(InstructionCount() - instructions, start,
		ThreadGovernor::HostSeconds());
	return twoThreads;
}


// CPU 1 runs on the worker thread while CPU 0 runs here.
void
BeBoxMachine::_HandOver(u64 sliceEnd, bool continued)
{
	Worker& worker = *fWorker;
	worker.sliceEnd = sliceEnd;
	worker.continued = continued;
	u64 generation = worker.start.load() + 1;
	worker.start.store(generation);
	if (worker.sleeping) {
		std::lock_guard<std::mutex> lock(worker.lock);
		worker.wake.notify_one();
	}
	fScheduler.SetClock(fCpus[0]->NowPointer());
	fCpus[0]->Run(sliceEnd);
	fScheduler.SetClock(nullptr);
	double waitStart = 0;
	for (int spins = 0;
			worker.done.load(std::memory_order_acquire) != generation;
			spins++) {
		if (spins == kWaitSpins)
			waitStart = ThreadGovernor::HostSeconds();
		if (spins < 100000)
			CpuRelax();
		else
			std::this_thread::yield();
	}
	// The time either thread waited (while the worker was slow to notice
	// the slice, both did: counted once).
	double waitEnd = waitStart > 0 ? ThreadGovernor::HostSeconds() : 0;
	double waited = waitEnd - waitStart;
	if (worker.waitStart > 0) {
		waited += worker.waitEnd - worker.waitStart;
		if (waitStart > 0) {
			waited -= std::max(0.0, std::min(worker.waitEnd, waitEnd)
				- std::max(worker.waitStart, waitStart));
		}
	}
	if (waited > 0)
		worker.governor.Waited(waited);
}


// Both CPUs ran since 'start': tells the governor, returns the time.
double
BeBoxMachine::_BothRan(double start)
{
	double now = ThreadGovernor::HostSeconds();
	fWorker->governor.BothRan(now, now - start);
	return now;
}


void
BeBoxMachine::_RunSliceOneThread(u64 sliceEnd)
{
	for (auto& cpu : fCpus) {
		fScheduler.SetClock(cpu->NowPointer());
		cpu->Run(sliceEnd);
	}
	fScheduler.SetClock(nullptr);
}


void
BeBoxMachine::_RequestPowerOn()
{
	// A device asked (a CPU's I/O access): with the CPUs on their own
	// threads, the reset waits for the end of the time slice.
	if (fWorker)
		fPowerOnRequested = true;
	else
		PowerOn();
}


void
BeBoxMachine::_SetupMemory()
{
	// Split the RAM into banks (64 bits wide: a pair of 72-pin SIMMs) of
	// 8, 16, 32 or 64 MB, largest first: banks 0, 2, 4 and 6 (single
	// sided SIMMs), above 256 MB also banks 1, 3, 5 and 7 (the second
	// sides of double sided SIMMs: a pair of 64 MB SIMMs fills two 64 MB
	// banks). The boot ROM's memory test sizes banks of 2-128 MB through
	// the MPC105's row and column bits (Mpc105::_UpdateMemoryMap), but
	// takes a 128 MB bank only as the first bank it finds, so 512 MB is
	// the most it can use.
	static const int kBankOrder[8] = { 0, 2, 4, 6, 1, 3, 5, 7 };
	u32 remaining = std::min<u32>(512, std::max<u32>(8, fConfig.ramMB));
	remaining &= ~7u;
	u32 total = 0;
	for (int i = 0; i < 8 && remaining >= 8; i++) {
		int bank = kBankOrder[i];
		u32 size = 64;
		while (size > remaining)
			size /= 2;
		u32 bytes = size << 20;
		fRamBanks.emplace_back(new u8[bytes]());
		fRamBankSizes.push_back(bytes);
		fBridge->SetBank(bank, fRamBanks.back().get(), bytes);
		BB_LOG(kLogMachine, kLogInfo, "bank %d: %u MB", bank, size);
		remaining -= size;
		total += size;
	}
	fConfig.ramMB = total;
}


bool
BeBoxMachine::_LoadRoms(std::string& error)
{
	std::vector<u8> data;
	if (!fConfig.flashPath.empty() && ReadFile(fConfig.flashPath, data)
		&& data.size() == fFlash->Size()) {
		memcpy(fFlash->Data(), data.data(), data.size());
		BB_LOG(kLogMachine, kLogInfo, "flash loaded from %s",
			fConfig.flashPath.c_str());
		return true;
	}

	memset(fFlash->Data(), 0xff, fFlash->Size());
	if (!ReadFile(fConfig.nubRomPath, data) || data.size() != 16384) {
		error = "cannot read the boot ROM nub (16 KB): " + fConfig.nubRomPath;
		return false;
	}
	memcpy(fFlash->Data(), data.data(), data.size());
	if (!ReadFile(fConfig.mainRomPath, data) || data.size() < 4096) {
		error = "cannot read the main boot ROM: " + fConfig.mainRomPath;
		return false;
	}
	// ROM update images (bebox_bootmain.image) of R4 and later start with
	// a "BOOT" tag that the ROM updater leaves out.
	if (memcmp(data.data(), "BOOT", 4) == 0)
		data.erase(data.begin(), data.begin() + 4);
	if (memcmp(data.data(), "Joy!peff", 8) != 0) {
		error = "not a BeBox boot ROM image: " + fConfig.mainRomPath;
		return false;
	}
	u32 room = fFlash->Size() - 0x4000;
	memcpy(fFlash->Data() + 0x4000, data.data(),
		std::min<size_t>(room, data.size()));
	// The image is valid even when its zero padding does not fit (128 KB
	// flash): the PEF content ends well before.
	return true;
}


bool
BeBoxMachine::_LoadNvram()
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
BeBoxMachine::SavePersistentState(std::string& error)
{
	bool ok = true;
	if (!fConfig.flashPath.empty() && (fFlash->IsDirty() || fSaveAll)) {
		if (WriteFile(fConfig.flashPath, fFlash->Data(), fFlash->Size()))
			fFlash->ClearDirty();
		else {
			error = "cannot write " + fConfig.flashPath;
			ok = false;
		}
	}
	if (!fConfig.nvramPath.empty() && (fRtc->IsDirty() || fSaveAll)) {
		std::vector<u8> data(Rtc146818::kRamSize + 8);
		memcpy(data.data(), fRtc->Ram(), Rtc146818::kRamSize);
		s64 offset = fRtc->TimeOffset();
		memcpy(&data[Rtc146818::kRamSize], &offset, 8);
		if (WriteFile(fConfig.nvramPath, data.data(), data.size()))
			fRtc->ClearDirty();
		else {
			error = "cannot write " + fConfig.nvramPath;
			ok = false;
		}
	}
	if (ok)
		fSaveAll = false;
	return ok;
}


void
BeBoxMachine::SetPersistentPaths(const std::string& flash, const std::string& nvram)
{
	fConfig.flashPath = flash;
	fConfig.nvramPath = nvram;
	fSaveAll = true;
}


void
BeBoxMachine::PowerOn()
{
	BB_LOG(kLogMachine, kLogInfo, "power on reset");
	// Memory does not survive a power cycle (the boot ROM shortens its
	// memory test after a warm reset, which changes when it looks at the
	// keyboard).
	for (size_t i = 0; i < fRamBanks.size(); i++)
		memset(fRamBanks[i].get(), 0, fRamBankSizes[i]);
	fBridge->Reset();
	fSio->Reset();
	fRtc->Reset();
	fFlash->Reset();
	fKeyboard->Reset();
	fDebugKeyHeld = false;
	if (fCirrus)
		fCirrus->Reset();
	if (fS3)
		fS3->Reset();
	fScsi->Reset();
	fIde->Reset();
	fFloppy->Reset();
	fAip->Reset();
	fAdc->Reset();
	if (fEthernet)
		fEthernet->Reset();
	fSound->Reset();
	for (auto& uart : fUarts)
		uart->Reset();
	for (auto& cpu : fCpus) {
		cpu->SetNow(fNow);
		cpu->HardReset();
	}
	// CPU1 is held in reset by the board until the boot ROM releases it.
	fBoard->PowerOnReset();
	if (!fKernelEntryTimer) {
		fKernelEntryTimer.reset(new Timer(fScheduler,
			[this]() { _CheckKernelEntry(); }, "kernel-entry"));
	}
	fKernelEntryTimer->ArmIn(fTicksPerSecond / 4000);
}


// Every 250 us until the boot ROM jumps into the kernel, which reads the
// boot items a few milliseconds later: then adapts them to the kernel.
void
BeBoxMachine::_CheckKernelEntry()
{
	if (BootItems::EntryPoint(*fBus) == 0) {
		fKernelEntryTimer->ArmIn(fTicksPerSecond / 4000);
		return;
	}
	std::string log;
	if (BootItems::AdaptForKernel(*fBus, log))
		BB_LOG(kLogMachine, kLogInfo, "%s", log.c_str());
}


// The boot ROM looks for F1 after the memory test (0.064 s per MB at
// 66 MHz, half that at 133 MHz) and its keyboard setup, and starts writing
// to serial 4 as soon as it has seen it: 13 ms (1.1d7) to 1.8 s (R3) after
// the setup. The R3 and R4.5 ROMs load kernel_joe_alt instead of the
// kernel when F1 is still down as they load it, and a typematic repeat that
// arrives translated during the setup is J to the ROM (R3: boot menu, and
// kernel_joe_alt again). So F1 goes down when the setup turns scan code
// translation off and up at the first byte on serial 4 after that, or
// 'kDebugKeyMaxSeconds' after the memory test for a ROM that never writes
// there.
void
BeBoxMachine::HoldDebugOutputKey()
{
	static const u32 kKeyF1 = 0x05;
	double memoryTest = fConfig.ramMB * (fConfig.model
		== MachineConfig::kBeBox133 ? 0.032 : 0.064);
	fKeyboard->HoldKeyUntranslated(kKeyF1,
		(u64)((kDebugKeyMaxSeconds + memoryTest) * fTicksPerSecond));
	fDebugKeyHeld = true;
}


void
BeBoxMachine::_DebugOutputStarted()
{
	// only output while F1 is down answers it (the nub prints garbage
	// there when it finds no memory it can use)
	if (!fDebugKeyHeld || !fKeyboard->HoldingKey())
		return;
	fDebugKeyHeld = false;
	BB_LOG(kLogMachine, kLogInfo, "boot ROM writes to serial4 at %.3f s: "
		"F1 up", fScheduler.Now() / (double)fTicksPerSecond);
	fKeyboard->ReleaseHeldKey();
}


void
BeBoxMachine::PressInterruptSwitch()
{
	BB_LOG(kLogMachine, kLogInfo, "interrupt switch pressed");
	fBoard->SetInterruptSwitch(true);
	if (!fSwitchTimer) {
		fSwitchTimer.reset(new Timer(fScheduler, [this]() {
			fBoard->SetInterruptSwitch(false);
		}, "interrupt-switch"));
	}
	fSwitchTimer->ArmIn(fTicksPerSecond / 20);
}


void
BeBoxMachine::SetIsaIrq(int irq, bool level)
{
	fSio->SetIrq(irq, level);
	int bit = BeBoxBoard::SourceForIsaIrq(irq);
	if (bit >= 0)
		fBoard->SetSource(bit, level);
}


void
BeBoxMachine::SetAudioSink(AudioSink* sink)
{
	fSound->SetAudioSink(sink);
}


void
BeBoxMachine::SetVideoSink(VideoSink* sink)
{
	if (fVideo != nullptr)
		fVideo->SetVideoSink(sink);
}


void
BeBoxMachine::SetNetworkBackend(NetworkBackend* backend)
{
	if (fEthernet)
		fEthernet->SetBackend(backend);
}


void
BeBoxMachine::SetSerialBackend(int port, SerialBackend* backend)
{
	fUarts[port]->SetBackend(backend);
}


void
BeBoxMachine::RunFor(u64 ticks)
{
	u64 target = fNow + ticks;
	bool twoThreads = false;	// the last slice ran on both threads
	double bothStart = 0;		// both CPUs run since (host time)
	while (fNow < target) {
		u64 sliceEnd = std::min(target, fNow + fQuantum);
		u64 next = fScheduler.NextEventTime();
		if (next < sliceEnd)
			sliceEnd = std::max(next, fNow + 1);
		for (auto& cpu : fCpus) {
			if (cpu->Now() < fNow)
				cpu->SetNow(fNow);
		}
		// A CPU in reset or dozing does next to nothing: then the other
		// one runs on this thread too, without waiting for the worker.
		if (fWorker && !fCpus[0]->IsHalted() && !fCpus[1]->IsHalted()) {
			if (bothStart == 0)
				bothStart = ThreadGovernor::HostSeconds();
			twoThreads = _RunSliceThreaded(sliceEnd, twoThreads);
		} else {
			if (bothStart != 0)
				_BothRan(bothStart);
			bothStart = 0;
			_RunSliceOneThread(sliceEnd);
			twoThreads = false;
		}
		fNow = sliceEnd;
		fScheduler.SetNow(fNow);
		fScheduler.RunDue(fNow);
		if (fPowerOnRequested.exchange(false))
			PowerOn();
	}
	if (fWorker) {
		double now = bothStart != 0 ? _BothRan(bothStart)
			: ThreadGovernor::HostSeconds();
		fWorker->governor.RunEnded(now);
	}
}


u64
BeBoxMachine::InstructionCount() const
{
	u64 count = 0;
	for (const auto& cpu : fCpus)
		count += cpu->InstructionCount();
	return count;
}


double
BeBoxMachine::NominalInstructionTicks() const
{
	double ticksPerCycle = fConfig.model == MachineConfig::kBeBox133 ? 2 : 4;
	return std::max(1.0, ticksPerCycle * fConfig.cpiPercent / 100.0);
}


void
BeBoxMachine::SetInstructionTicks(u32 ticks)
{
	// between time slices: the worker thread is waiting
	for (auto& cpu : fCpus)
		cpu->SetTicksPerInstruction(ticks);
}


void
BeBoxMachine::SampleLeds(float& left, float& right, float& disk)
{
	fBoard->SampleLeds(left, right, disk);
}


bool
BeBoxMachine::CpuActive(int cpu) const
{
	return fBoard->CpuActive(cpu);
}


u64
BeBoxMachine::UnclaimedIoCount() const
{
	return fIo->UnclaimedCount();
}


std::string
BeBoxMachine::CpuState(int index) const
{
	const PpcCpu* cpu = fCpus[index].get();
	char line[160];
	snprintf(line, sizeof(line), "%s pc %08x lr %08x msr %08x, %llu "
		"instructions", cpu->IsHeldInReset() ? "held in reset"
			: cpu->IsHalted() ? "halted" : "running", cpu->pc, cpu->fLr,
		cpu->GetMsr(), (unsigned long long)cpu->InstructionCount());
	return line;
}


bool
BeBoxMachine::DebugTranslate(int cpu, u32 va, u32& pa)
{
	return fCpus[cpu]->DebugTranslate(va, false, pa);
}


ScsiDevice*
BeBoxMachine::ScsiTarget(int id)
{
	return fScsi ? fScsi->Device(id) : nullptr;
}

}	// namespace bebox

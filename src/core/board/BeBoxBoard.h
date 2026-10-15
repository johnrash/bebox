/*
 * BeBox emulator - BeBox motherboard logic ("glue" FPGAs).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Registers (Be Newsletter #27, "OS Writer's Cookbook"), 32 bits, IBM bit
 * numbering (bit 0 = MSB):
 *   0x7ffff0f0  CPU0 interrupt mask
 *   0x7ffff1f0  CPU1 interrupt mask
 *   0x7ffff2f0  interrupt sources (read only)
 *   0x7ffff3f0  cross processor: 1 /SMI CPU0 (1 asserts), 2 /SMI CPU1,
 *               4 /TLBISYNC CPU0 (0 asserts), 5 /TLBISYNC CPU1,
 *               6 which processor is reading (read only)
 *   0x7ffff4f0  resets: 1 /SRESET CPU1 (0 asserts), 2 /HRESET CPU1,
 *               3 FLUSHREQ, 7 disk LED (1 = on)
 * Writes: bit 0 of the value selects set (1) or clear (0); every other
 * bit that is 1 is changed.
 * The front panel bar graph is the 8-bit latch at ISA port 0xc00: high
 * nibble = left column level, low nibble = right column level.
 */
#ifndef BEBOX_BEBOX_BOARD_H
#define BEBOX_BEBOX_BOARD_H

#include "../bus/IoSpace.h"
#include "../bus/PhysBus.h"

#include <atomic>

namespace bebox {

class PpcCpu;
class Scheduler;

// Interrupt source bits (IBM numbering).
enum BeInterruptSource {
	kBeIrqSerial1 = 4, kBeIrqSerial2 = 5, kBeIrqSerial3 = 6,
	kBeIrqSerial4 = 7, kBeIrqMidi1 = 8, kBeIrqMidi2 = 9, kBeIrqScsi = 10,
	kBeIrqPciSlot1 = 11, kBeIrqPciSlot2 = 12, kBeIrqPciSlot3 = 13,
	kBeIrqSound = 14, kBeIrqKeyboard = 15, kBeIrqRtc = 16, kBeIrqIsa5 = 17,
	kBeIrqFloppy = 18, kBeIrqParallel = 19, kBeIrqIsa9 = 20,
	kBeIrqIsa10 = 21, kBeIrqIsa11 = 22, kBeIrqMouse = 23, kBeIrqIde = 24,
	kBeIrqIsa15 = 25, kBeIrq8259 = 26, kBeIrqInfrared = 27,
	kBeIrqAToD = 28, kBeIrqGeekPort = 29
};

class BeBoxBoard : public MmioDevice, public ByteIoDevice {
public:
	static const u32 kRegisterPage = 0x7ffff000;

								BeBoxBoard(PhysBus& bus, IoSpace& io,
									Scheduler& scheduler);

			void				AttachCpus(PpcCpu* cpu0, PpcCpu* cpu1);
			void				PowerOnReset();

			// Interrupt source level as driven by a device.
			void				SetSource(int bit, bool level);
			// The Be controller bit an ISA IRQ is wired to, or -1.
	static	int					SourceForIsaIrq(int irq);

			u32					Sources() const { return fSources; }
			u32					Mask(int cpu) const { return fMask[cpu]; }

			// Front panel.
			u8					LedLatch() const { return fLedLatch; }
			bool				DiskLed() const
									{ return (fResets & 0x01000000) != 0; }
			// Average bar graph levels (0-15) since the last call, and
			// the disk LED: 1 if it was lit at all (emulated disk transfers
			// are too fast for a duty cycle to show), else 0.
			void				SampleLeds(float& left, float& right,
									float& disk);
			// A SCSI or IDE transfer: lights the disk LED for the sample
			// (DR releases leave the LED alone on SCSI accesses).
			void				NoteDiskActivity() { fDiskActivity = true; }
			bool				CpuActive(int cpu) const;
			// Front panel Interrupt switch: an SMI to the processors whose
			// mask enables source bit 1 (CPU0) or 2 (CPU1).
			void				SetInterruptSwitch(bool pressed);

	// MmioDevice (register page)
	virtual	u32					MmioRead(u32 addr, int size, int initiator);
	virtual	void				MmioWrite(u32 addr, u32 value, int size,
									int initiator);
	virtual	const char*			MmioName() const { return "bebox-regs"; }

	// ByteIoDevice (LED latch at 0xc00)
	virtual	u8					IoRead8(u32 port);
	virtual	void				IoWrite8(u32 port, u8 value);
	virtual	const char*			IoName() const { return "led-latch"; }

private:
			void				_Update();
			void				_UpdateCrossProcessor();
			void				_UpdateResets(u32 old);
			void				_AccumulateLeds();
	static	u32					_Apply(u32 current, u32 value);

			PhysBus&			fBus;
			Scheduler&			fScheduler;
			PpcCpu*				fCpus[2];
			u32					fMask[2];
			u32					fSources;
			u32					fCrossProcessor;
			u32					fResets;
			u8					fLedLatch;
			bool				fInterruptSwitch;

			// LED averaging (emulated time weighted)
			u64					fLedLastChange;
			u64					fLedWindowStart;
			double				fLedLeftSum;
			double				fLedRightSum;
			bool				fDiskActivity;
};

}	// namespace bebox

#endif	// BEBOX_BEBOX_BOARD_H

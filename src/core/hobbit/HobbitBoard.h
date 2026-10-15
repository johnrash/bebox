/*
 * BeBox emulator - the Hobbit BeBox board ("Sako"): I/O window, interrupt
 * controller, processor resets.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * The 4 KB I/O window at 0x00800000 holds the board's registers and the
 * PC compatible I/O chips (a VLSI combination chip, the 53C94 SCSI
 * controller), each 8-bit register on its own word: byte lane 3 of word
 * 'port' (0x800000 + port * 4 + 3) for ISA style port numbers, lane 2 for
 * the SCSI controller (0x800802 + reg * 4) and its 16-bit pseudo-DMA
 * port (0x800842).
 *
 * Board registers (lane 3; documented in the September 1993 hardware
 * notes, the rest from the kernel's use):
 *   0x800003  I/O interrupt register: 0 floppy, 1 SER1, 2 SER2, 4 RTC,
 *             5 keyboard, 6 mouse, 7 SCSI (1 = interrupting)
 *   0x800007  I/O interrupt mask of CPU 1 (1 = enabled)
 *   0x80000b  I/O interrupt mask of CPU 2
 *   0x80000f  slot interrupt register (also the joystick port D)
 *   0x800013  slot interrupt mask of CPU 1
 *   0x800017  slot interrupt mask of CPU 2
 *   0x80001b  processor reset: bit 0 CPU 1 (Hobbit A), bit 1 CPU 2;
 *             0 holds the processor in reset
 *   0x80001f  internal processor interrupts: bit 0 Hobbit A, 1 Hobbit B,
 *             2-4 DSP 1-3 (set to interrupt)
 *   0x800dc3  "who am I": bits 3-0 the current bus master (4: CPU 1,
 *             5: CPU 2), bits 7-4 joystick buttons
 * The floppy DMA (words, from the kernel's floppy driver):
 *   0x800020  byte count - 1 (writing it starts the transfer)
 *   0x800024  direction: 0 to memory (disk reads), 1 from memory
 *   0x80002a  address bits 31-16, 0x80002e bits 15-0 (word aligned)
 * The floppy controller is at the secondary PC address (0x370-0x377).
 * Interrupt levels (IL): internal processor interrupts 1, I/O devices 3,
 * slots 5, joystick controller 6.
 */
#ifndef BEBOX_HOBBIT_BOARD_H
#define BEBOX_HOBBIT_BOARD_H

#include "../bus/IoSpace.h"
#include "../bus/PhysBus.h"
#include "../isa/DmaChannels.h"
#include "../Types.h"

#include <functional>

namespace bebox {

class HobbitCpu;

class HobbitBoard : public MmioDevice, public DmaChannels {
public:
	static const u32 kIoBase = 0x00800000;
	static const u32 kIoSize = 0x1000;

	enum {
		kIoFloppy = 0,
		kIoSerial1 = 1,
		kIoSerial2 = 2,
		kIoRtc = 4,
		kIoKeyboard = 5,
		kIoMouse = 6,
		kIoScsi = 7
	};

	// A CPU leaves reset (the boot ROM's job starts).
	typedef std::function<void(int cpu)> ReleaseCallback;

								HobbitBoard(PhysBus& bus, IoSpace& io);

			void				AttachCpus(HobbitCpu* cpu0, HobbitCpu* cpu1);
			void				SetScsi(IoDevice* scsi) { fScsi = scsi; }
			void				SetReleaseCallback(ReleaseCallback callback)
									{ fRelease = callback; }

			// Power on: CPU 1 runs, CPU 2 is held in reset.
			void				PowerOnReset();

			void				SetIoInterrupt(int bit, bool level);
			void				SetSlotInterrupt(int slot, bool level);
			// The front panel interrupt: an NMI to CPU 1.
			void				SetNmi(bool level);
			// The processors' activity, for the front end.
			bool				CpuRunning(int cpu) const;

	virtual	u32					MmioRead(u32 address, int size,
									int initiator);
	virtual	void				MmioWrite(u32 address, u32 value, int size,
									int initiator);
	virtual	const char*			MmioName() const { return "hobbit-io"; }

			// The floppy DMA (channel 2).
	virtual	u32					Transfer(int channel, void* buffer,
									u32 length, bool& terminalCount);
	virtual	bool				IsMasked(int channel) const;

private:
			u8					_Read8(u32 offset, int initiator);
			void				_Write8(u32 offset, u8 value, int initiator);
			void				_UpdateInterrupts();
			void				_SetResets(u8 value);

			PhysBus&			fBus;
			IoSpace&			fIo;
			IoDevice*			fScsi;
			HobbitCpu*			fCpus[2];
			ReleaseCallback		fRelease;

			u8					fIoPending;
			u8					fIoMask[2];
			u8					fSlotPending;
			u8					fSlotMask[2];
			u8					fResets;
			u8					fInternal;
			bool				fNmi;

			u8					fDmaRegisters[16];	// 0x800020-0x80002f
			u32					fDmaAddress;
			u32					fDmaRemaining;
			bool				fDmaFromMemory;
};

}	// namespace bebox

#endif	// BEBOX_HOBBIT_BOARD_H

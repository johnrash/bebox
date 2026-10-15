/*
 * BeBox emulator - Intel 82378ZB System I/O (PCI to ISA bridge).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Contains the two 8259 interrupt controllers, the 8254 timer, the two
 * 8237 DMA controllers, the NMI status/control port (0x61), port 92 and
 * the edge/level control registers (0x4d0/0x4d1).
 */
#ifndef BEBOX_SIO_82378_H
#define BEBOX_SIO_82378_H

#include "Dma8237.h"
#include "Pic8259.h"
#include "Pit8254.h"
#include "../bus/IoSpace.h"
#include "../bus/PciBus.h"

#include <functional>

namespace bebox {

class Sio82378 : public PciDevice, public ByteIoDevice {
public:
	typedef std::function<void(bool)> InterruptCallback;
	typedef std::function<void()> ResetCallback;

								Sio82378(IoSpace& io, Scheduler& scheduler,
									u64 ticksPerSecond);

			void				Reset();
			// The 8259's INT output (Be interrupt controller bit 26).
			void				SetInterruptCallback(InterruptCallback callback)
									{ fInterrupt = callback; }
			// Port 92 alternate reset.
			void				SetResetCallback(ResetCallback callback)
									{ fReset = callback; }

			// ISA interrupt request line 0-15 as driven by a device.
			void				SetIrq(int irq, bool level);
			u8					InterruptAcknowledge();

			Pic8259&			MasterPic() { return fMaster; }
			Pic8259&			SlavePic() { return fSlave; }
			Pit8254&			Pit() { return fPit; }
			Dma8237&			Dma() { return fDma; }

	// PciDevice
	virtual	const char*			PciName() const { return "sio"; }

	// ByteIoDevice (0x20-0x21, 0xa0-0xa1, 0x61, 0x92, 0x4d0-0x4d1)
	virtual	u8					IoRead8(u32 port);
	virtual	void				IoWrite8(u32 port, u8 value);
	virtual	const char*			IoName() const { return "sio"; }

private:
			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			Pic8259				fMaster;
			Pic8259				fSlave;
			Pit8254				fPit;
			Dma8237				fDma;
			u8					fPort61;
			u8					fPort92;
			InterruptCallback	fInterrupt;
			ResetCallback		fReset;
};

}	// namespace bebox

#endif	// BEBOX_SIO_82378_H

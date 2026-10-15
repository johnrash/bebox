/*
 * BeBox emulator - Intel 82378ZB System I/O (PCI to ISA bridge).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "Sio82378.h"

#include "../Log.h"

namespace bebox {

Sio82378::Sio82378(IoSpace& io, Scheduler& scheduler, u64 ticksPerSecond)
	:
	PciDevice(0x8086, 0x0484, 0x000000, 0x03),
	fScheduler(scheduler),
	fTicksPerSecond(ticksPerSecond),
	fMaster("pic1"),
	fSlave("pic2"),
	fPit(scheduler, ticksPerSecond),
	fPort61(0),
	fPort92(0)
{
	fMaster.SetSlave(&fSlave, 2);
	fSlave.SetOutputCallback([this](bool level) {
		fMaster.SetLine(2, level);
	});
	fMaster.SetOutputCallback([this](bool level) {
		if (fInterrupt)
			fInterrupt(level);
	});
	fPit.SetOutputCallback(0, [this](bool level) {
		fMaster.SetLine(0, level);
	});

	io.Map(0x00, 0x10, &fDma);
	io.Map(0x80, 0x10, &fDma);
	io.Map(0xc0, 0x20, &fDma);
	io.Map(0x40b, 1, &fDma);
	io.Map(0x481, 0x0f, &fDma);
	io.Map(0x4d6, 1, &fDma);
	io.Map(0x40, 4, &fPit);
	io.Map(0x20, 2, this);
	io.Map(0xa0, 2, this);
	io.Map(0x61, 1, this);
	io.Map(0x92, 1, this);
	io.Map(0x4d0, 2, this);

	// Default configuration register values (82378ZB data sheet).
	fConfig[4] = 0x07;
	fConfig[0x40] = 0x20;
	fConfig[0x41] = 0x00;
	fConfig[0x48] = 0x01;
	fConfig[0x4c] = 0x4d;
	fConfig[0x4e] = 0x03;
	Reset();
}


void
Sio82378::Reset()
{
	fMaster.Reset();
	fSlave.Reset();
	fMaster.SetLevelTriggered(0);
	fSlave.SetLevelTriggered(0);
	fPit.Reset();
	fDma.Reset();
	fPort61 = 0;
	fPort92 = 0;
	fPit.SetGate(2, false);
}


void
Sio82378::SetIrq(int irq, bool level)
{
	if (irq < 8)
		fMaster.SetLine(irq, level);
	else
		fSlave.SetLine(irq - 8, level);
}


u8
Sio82378::InterruptAcknowledge()
{
	return fMaster.Acknowledge();
}


u8
Sio82378::IoRead8(u32 port)
{
	switch (port) {
		case 0x20: case 0x21:
			return fMaster.Read(port & 1);
		case 0xa0: case 0xa1:
			return fSlave.Read(port & 1);
		case 0x61:
		{
			// Bit 4 toggles with every refresh request (counter 1),
			// bit 5 is counter 2's output.
			u64 pitTicks = (u64)(((unsigned __int128)fScheduler.Now()
				* Pit8254::kClock) / fTicksPerSecond);
			u8 value = fPort61 & 0x0f;
			if (((pitTicks / 18) & 1) != 0)
				value |= 0x10;
			if (fPit.Output(2))
				value |= 0x20;
			return value;
		}
		case 0x92:
			return fPort92;
		case 0x4d0:
			return fMaster.LevelTriggered();
		case 0x4d1:
			return fSlave.LevelTriggered();
	}
	return 0xff;
}


void
Sio82378::IoWrite8(u32 port, u8 value)
{
	switch (port) {
		case 0x20: case 0x21:
			fMaster.Write(port & 1, value);
			break;
		case 0xa0: case 0xa1:
			fSlave.Write(port & 1, value);
			break;
		case 0x61:
			fPort61 = value & 0x0f;
			fPit.SetGate(2, (value & 1) != 0);
			break;
		case 0x92:
		{
			bool reset = (value & 1) != 0 && (fPort92 & 1) == 0;
			fPort92 = value;
			if (reset) {
				BB_LOG(kLogIsa, kLogInfo, "port 92 alternate reset");
				if (fReset)
					fReset();
			}
			break;
		}
		case 0x4d0:
			// IRQ 0, 1 and 2 are always edge triggered
			fMaster.SetLevelTriggered(value & 0xf8);
			break;
		case 0x4d1:
			fSlave.SetLevelTriggered(value);
			break;
	}
}

}	// namespace bebox

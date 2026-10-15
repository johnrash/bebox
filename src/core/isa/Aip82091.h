/*
 * BeBox emulator - Intel 82091AA Advanced Integrated Peripheral
 * configuration registers, and the BeBox I/O ports that are not emulated
 * (GeekPort, infrared, parallel) but must not disturb drivers.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * The AIP holds the floppy controller, the IDE interface, serial ports 1
 * and 2 and the parallel port; the boot ROM configures it through the
 * index/data pair at 0x398/0x399. The functions themselves are separate
 * devices.
 */
#ifndef BEBOX_AIP_82091_H
#define BEBOX_AIP_82091_H

#include "../bus/IoSpace.h"

namespace bebox {

class Aip82091 : public ByteIoDevice {
public:
								Aip82091();

			void				Reset();
			u8					Register(u8 index) const
									{ return fRegisters[index]; }

	virtual	u8					IoRead8(u32 port);
	virtual	void				IoWrite8(u32 port, u8 value);
	virtual	const char*			IoName() const { return "aip-config"; }

private:
			u8					fIndex;
			u8					fRegisters[256];
};

// GeekPort (0x360-0x367, 0x390-0x396), infrared (0x368-0x36b) and the
// parallel port (0x3bc-0x3be): writes are accepted, reads return idle
// values. (The joystick buttons at 0x397 and the A/D converter belong to
// the game port device once it is attached.)
class UnemulatedPorts : public ByteIoDevice {
public:
	virtual	u8					IoRead8(u32 port);
	virtual	void				IoWrite8(u32 port, u8 value) {}
	virtual	const char*			IoName() const { return "unemulated"; }
};

}	// namespace bebox

#endif	// BEBOX_AIP_82091_H

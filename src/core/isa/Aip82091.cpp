/*
 * BeBox emulator - Intel 82091AA configuration registers.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "Aip82091.h"

#include "../Log.h"

namespace bebox {

Aip82091::Aip82091()
{
	Reset();
}


void
Aip82091::Reset()
{
	fIndex = 0;
	memset(fRegisters, 0, sizeof(fRegisters));
	fRegisters[0x00] = 0xa0;	// AIP identification
	fRegisters[0x01] = 0x00;	// revision
}


u8
Aip82091::IoRead8(u32 port)
{
	if (port == 0x398)
		return fIndex;
	return fRegisters[fIndex];
}


void
Aip82091::IoWrite8(u32 port, u8 value)
{
	if (port == 0x398) {
		fIndex = value;
		return;
	}
	if (fIndex <= 1)
		return;		// identification registers are read only
	BB_LOG(kLogIsa, kLogDebug, "aip register %02x = %02x", fIndex, value);
	fRegisters[fIndex] = value;
}


u8
UnemulatedPorts::IoRead8(u32 port)
{
	if (port >= 0x3bc && port <= 0x3be) {
		// parallel port: no printer, not busy
		return port == 0x3bd ? 0xdf : 0xff;
	}
	return 0;
}

}	// namespace bebox

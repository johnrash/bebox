/*
 * BeBox emulator - ISA/PCI I/O port space.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Devices see little endian values: a 16 or 32 bit read at 'port' returns
 * the byte at 'port' in bits 7..0. The MPC105's I/O window swaps lanes for
 * the big endian processor (so drivers use lwbrx/lhbrx, as on real
 * hardware).
 */
#ifndef BEBOX_IO_SPACE_H
#define BEBOX_IO_SPACE_H

#include "../Types.h"

#include <vector>

namespace bebox {

class IoDevice {
public:
	virtual ~IoDevice() {}
	virtual u32 IoRead(u32 port, int size) = 0;
	virtual void IoWrite(u32 port, u32 value, int size) = 0;
	virtual const char* IoName() const = 0;
};

// Helper base for devices with 8-bit registers: wider accesses are split
// into byte accesses at consecutive ports.
class ByteIoDevice : public IoDevice {
public:
	virtual u8 IoRead8(u32 port) = 0;
	virtual void IoWrite8(u32 port, u8 value) = 0;

	virtual u32 IoRead(u32 port, int size)
	{
		u32 value = 0;
		for (int i = 0; i < size; i++)
			value |= (u32)IoRead8(port + i) << (8 * i);
		return value;
	}

	virtual void IoWrite(u32 port, u32 value, int size)
	{
		for (int i = 0; i < size; i++)
			IoWrite8(port + i, (u8)(value >> (8 * i)));
	}
};

class IoSpace {
public:
								IoSpace();

			void				Map(u32 first, u32 count, IoDevice* device);
			void				Unmap(u32 first, u32 count);
			IoDevice*			DeviceAt(u32 port) const;

			u32					Read(u32 port, int size, int initiator = 0,
									u32 pc = 0);
			void				Write(u32 port, u32 value, int size,
									int initiator = 0, u32 pc = 0);

			u64					UnclaimedCount() const
									{ return fUnclaimed; }
			// The bus master of the access being dispatched.
			int					CurrentInitiator() const
									{ return fInitiator; }

private:
	struct Range {
		u32 first;
		u32 count;
		IoDevice* device;
	};

			IoDevice*			fLow[0x10000];		// ISA ports
			std::vector<Range>	fHigh;				// PCI I/O above 64 KB
			u64					fUnclaimed;
			int					fInitiator;
};

}	// namespace bebox

#endif	// BEBOX_IO_SPACE_H

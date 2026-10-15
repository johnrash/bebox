/*
 * BeBox emulator - Intel 8237 DMA controllers (as in the 82378 SIO).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Channels 0-3 belong to controller 1 (byte transfers), 4-7 to controller
 * 2 (word transfers; 4 is the cascade). The 82378 adds a high page
 * register per channel (address bits 31-24), so ISA DMA can reach PCI
 * memory addresses: system memory is at 0x80000000 + physical address.
 */
#ifndef BEBOX_DMA_8237_H
#define BEBOX_DMA_8237_H

#include "DmaChannels.h"
#include "../bus/IoSpace.h"

#include <functional>

namespace bebox {

class Dma8237 : public ByteIoDevice, public DmaChannels {
public:
	// Bus master access to PCI memory.
	typedef std::function<void(u32 address, void* buffer, u32 length)>
		MemoryRead;
	typedef std::function<void(u32 address, const void* buffer, u32 length)>
		MemoryWrite;

								Dma8237();

			void				Reset();
			void				SetMemoryAccess(MemoryRead read,
									MemoryWrite write)
									{ fRead = read; fWrite = write; }

			// Device side. Transfers up to 'length' bytes between 'buffer'
			// and memory (direction from the channel's mode: write
			// transfers move device data to memory). Returns the number
			// of bytes moved; 'terminalCount' is set when the count ran
			// out. Nothing is moved while the channel is masked.
	virtual	u32					Transfer(int channel, void* buffer,
									u32 length, bool& terminalCount);
	virtual	bool				IsMasked(int channel) const;
			// True if the channel is set up to move data into memory.
			bool				IsWriteToMemory(int channel) const;
			u32					Remaining(int channel) const;

	virtual	u8					IoRead8(u32 port);
	virtual	void				IoWrite8(u32 port, u8 value);
	virtual	const char*			IoName() const { return "dma"; }

private:
	struct Channel {
		u16 baseAddress;
		u16 baseCount;
		u16 address;
		u16 count;
		u8 mode;
		u8 page;
		u8 highPage;
	};

	struct Controller {
		bool flipFlop;
		u8 command;
		u8 status;
		u8 mask;		// bit per channel
		u8 request;
		u8 extendedMode[4];
	};

			Controller&			_ControllerOf(int channel)
									{ return fControllers[channel >> 2]; }
			void				_WriteController(int controller, int reg,
									u8 value);
			u8					_ReadController(int controller, int reg);

			Channel				fChannels[8];
			Controller			fControllers[2];
			MemoryRead			fRead;
			MemoryWrite			fWrite;
};

}	// namespace bebox

#endif	// BEBOX_DMA_8237_H

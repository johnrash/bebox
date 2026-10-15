/*
 * BeBox emulator - DMA channels as a device sees them.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * The floppy controller moves its data through a DMA channel: the ISA DMA
 * controller (Dma8237) on the BeBox, the board's floppy DMA on the Hobbit.
 */
#ifndef BEBOX_DMA_CHANNELS_H
#define BEBOX_DMA_CHANNELS_H

#include "../Types.h"

namespace bebox {

class DmaChannels {
public:
	virtual						~DmaChannels() {}

			// Transfers up to 'length' bytes between 'buffer' and memory
			// (the direction is the channel's). Returns the number of bytes
			// moved; 'terminalCount' is set when the count ran out. Nothing
			// is moved while the channel is masked.
	virtual	u32					Transfer(int channel, void* buffer,
									u32 length, bool& terminalCount) = 0;
	virtual	bool				IsMasked(int channel) const = 0;
};

}	// namespace bebox

#endif	// BEBOX_DMA_CHANNELS_H

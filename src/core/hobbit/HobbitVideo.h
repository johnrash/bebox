/*
 * BeBox emulator - the Hobbit BeBox's video card: a Trident VGA with an
 * XGA style graphics engine on a BeBus card (slot 5).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Slot 5 holds the 1 MB frame buffer at 0xd0000000 and the registers at
 * 0xd1000000: the VGA ports at their port number (0x3c0-0x3df) and the
 * graphics engine's index (0x217a) and 32-bit data (0x217c-0x217f, least
 * significant byte first) registers.
 *
 * What app_server uses (vgatri640480.c): a 256 color mode (no pixel
 * doubling, 16 bytes per offset unit), the palette (8 bits per component),
 * and two engine operations on pixel maps (base, size and format at
 * 0x94-0x9c for map A, the source, and 0xb4-0xbc for map B, the
 * destination): bit blits (pixel operation 0xa8 with the direction in bits
 * 2-0) and area fills with the foreground color (0x0a). The other engine
 * registers: 0xc4 pixel bit mask, 0xc8 foreground/background mix,
 * 0xd8/0xdc colors, 0xe0 operation dimensions (width - 1, height - 1 <<
 * 16), 0xe4/0xe8 destination/source linear address, 0xf0/0xf8
 * source/destination x, y, 0xfc pixel operation (starts it), 0x90 status
 * (bit 31 busy).
 */
#ifndef BEBOX_HOBBIT_VIDEO_H
#define BEBOX_HOBBIT_VIDEO_H

#include "../bus/PhysBus.h"
#include "../video/VgaCore.h"

namespace bebox {

class HobbitVideo : public VgaCore, public MmioDevice {
public:
	static const u32 kFrameBufferBase = 0xd0000000;
	static const u32 kRegistersBase = 0xd1000000;
	static const u32 kRegistersSize = 0x6000;
	static const u32 kVramSize = 1024 * 1024;

								HobbitVideo(PhysBus& bus, Scheduler& scheduler,
									u64 ticksPerSecond);
	virtual						~HobbitVideo();

			void				Reset();

	virtual	u32					MmioRead(u32 address, int size,
									int initiator);
	virtual	void				MmioWrite(u32 address, u32 value, int size,
									int initiator);
	virtual	const char*			MmioName() const { return "hobbit-video"; }

protected:
	virtual	void				ComputeMode(DisplayMode& mode);

private:
			u8					_ReadRegister(u32 offset);
			void				_WriteRegister(u32 offset, u8 value);
			void				_Execute();
			void				_Blit(bool fill);

			PhysBus&			fBus;
			u8					fEngineIndex;
			u32					fEngine[64];	// indexes 0x00-0xfc
};

}	// namespace bebox

#endif	// BEBOX_HOBBIT_VIDEO_H

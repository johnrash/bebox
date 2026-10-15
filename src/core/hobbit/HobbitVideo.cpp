/*
 * BeBox emulator - the Hobbit BeBox's video card: a Trident VGA with an
 * XGA style graphics engine on a BeBus card (slot 5).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "HobbitVideo.h"

#include "../Log.h"

namespace bebox {

// Graphics engine ports (offsets in the register window)
static const u32 kEngineFirst = 0x2120;
static const u32 kEngineLast = 0x217f;
static const u32 kEngineIndex = 0x217a;
static const u32 kEngineData = 0x217c;

// Engine registers (indexes)
enum {
	kRegStatus = 0x90,
	kRegMapABase = 0x94,
	kRegMapASize = 0x98,
	kRegMapAFormat = 0x9c,
	kRegMapBBase = 0xb4,
	kRegMapBSize = 0xb8,
	kRegMapBFormat = 0xbc,
	kRegBitMask = 0xc4,
	kRegMix = 0xc8,
	kRegForeground = 0xd8,
	kRegBackground = 0xdc,
	kRegDimensions = 0xe0,
	kRegDestinationAddress = 0xe4,
	kRegSourceAddress = 0xe8,
	kRegSourceXY = 0xf0,
	kRegDestinationXY = 0xf8,
	kRegPixelOperation = 0xfc
};


static inline u8
Mix(int mix, u8 source, u8 destination)
{
	switch (mix & 15) {
		case 0x0: return 0;
		case 0x1: return source & destination;
		case 0x2: return source & ~destination;
		case 0x3: return source;
		case 0x4: return ~source & destination;
		case 0x5: return destination;
		case 0x6: return source ^ destination;
		case 0x7: return source | destination;
		case 0x8: return ~source & ~destination;
		case 0x9: return source ^ ~destination;
		case 0xa: return ~destination;
		case 0xb: return source | ~destination;
		case 0xc: return ~source;
		case 0xd: return ~source | destination;
		case 0xe: return ~source | ~destination;
		default: return 0xff;
	}
}


HobbitVideo::HobbitVideo(PhysBus& bus, Scheduler& scheduler,
	u64 ticksPerSecond)
	:
	VgaCore(scheduler, ticksPerSecond, kVramSize),
	fBus(bus)
{
	fBus.MapMemory(kFrameBufferBase, kVramSize, Vram(), true);
	fBus.MapDevice(kRegistersBase, kRegistersSize, this);
	Reset();
}


HobbitVideo::~HobbitVideo()
{
	fBus.Unmap(kFrameBufferBase, kVramSize);
	fBus.Unmap(kRegistersBase, kRegistersSize);
}


void
HobbitVideo::Reset()
{
	ResetVga();
	// The RAMDAC takes 8 bits per color component: app_server loads the
	// palette from /system/Color_map through a gamma table of 8-bit
	// values.
	fDac8Bit = true;
	fEngineIndex = 0;
	for (u32& value : fEngine)
		value = 0;
}


u32
HobbitVideo::MmioRead(u32 address, int size, int initiator)
{
	u32 offset = address - kRegistersBase;
	u32 value = 0;
	for (int i = 0; i < size; i++)
		value = (value << 8) | _ReadRegister(offset + i);
	return value;
}


void
HobbitVideo::MmioWrite(u32 address, u32 value, int size, int initiator)
{
	u32 offset = address - kRegistersBase;
	for (int i = 0; i < size; i++)
		_WriteRegister(offset + i, (u8)(value >> (8 * (size - 1 - i))));
}


u8
HobbitVideo::_ReadRegister(u32 offset)
{
	if (offset >= 0x3b0 && offset <= 0x3df)
		return VgaRead(offset);
	if (offset == kEngineIndex)
		return fEngineIndex;
	if (offset >= kEngineData && offset <= kEngineData + 3) {
		// the engine finishes at once: never busy
		if (fEngineIndex == kRegStatus)
			return 0;
		return (u8)(fEngine[fEngineIndex >> 2] >> (8 * (offset - kEngineData)));
	}
	if (offset < kEngineFirst || offset > kEngineLast) {
		BB_LOG(kLogVideo, kLogDebug, "hobbit-video: read of %04x", offset);
	}
	return 0xff;
}


void
HobbitVideo::_WriteRegister(u32 offset, u8 value)
{
	if (offset >= 0x3b0 && offset <= 0x3df) {
		VgaWrite(offset, value);
		return;
	}
	if (offset == kEngineIndex) {
		fEngineIndex = value & 0xfc;
		return;
	}
	if (offset >= kEngineData && offset <= kEngineData + 3) {
		int shift = 8 * (offset - kEngineData);
		u32& reg = fEngine[fEngineIndex >> 2];
		reg = (reg & ~(0xffu << shift)) | ((u32)value << shift);
		// the most significant byte comes last
		if (fEngineIndex == kRegPixelOperation && offset == kEngineData + 3)
			_Execute();
		return;
	}
	BB_LOG(kLogVideo, kLogDebug, "hobbit-video: write of %02x to %04x",
		value, offset);
}


void
HobbitVideo::_Execute()
{
	u32 operation = fEngine[kRegPixelOperation >> 2];
	switch ((operation >> 24) & 15) {
		case 0x8:		// PxBlt
			_Blit(false);
			return;
		case 0xa:		// area fill
			_Blit(true);
			return;
	}
	BB_LOG(kLogVideo, kLogWarning, "hobbit-video: unsupported engine "
		"operation %08x", operation);
}


// A rectangle copied from map A (or filled with the foreground color) to
// map B, starting at the linear addresses, stepping as the direction bits
// say (bit 2: right to left, bit 1: bottom to top).
void
HobbitVideo::_Blit(bool fill)
{
	u32 operation = fEngine[kRegPixelOperation >> 2];
	u32 dimensions = fEngine[kRegDimensions >> 2];
	int width = (int)(dimensions & 0xffff) + 1;
	int height = (int)(dimensions >> 16) + 1;
	int sourcePitch = (int)(fEngine[kRegMapASize >> 2] & 0xffff) + 1;
	int destinationPitch = (int)(fEngine[kRegMapBSize >> 2] & 0xffff) + 1;
	int stepX = (operation & 4) != 0 ? -1 : 1;
	int stepY = (operation & 2) != 0 ? -1 : 1;
	u32 source = fEngine[kRegSourceAddress >> 2]
		+ fEngine[kRegMapABase >> 2];
	u32 destination = fEngine[kRegDestinationAddress >> 2]
		+ fEngine[kRegMapBBase >> 2];
	int mix = fEngine[kRegMix >> 2] & 0xff;
	u8 mask = (u8)fEngine[kRegBitMask >> 2];
	u8 color = (u8)fEngine[kRegForeground >> 2];
	u8* vram = Vram();
	u32 vramMask = VramSize() - 1;
	for (int y = 0; y < height; y++) {
		u32 s = source + (s32)(y * stepY * sourcePitch);
		u32 d = destination + (s32)(y * stepY * destinationPitch);
		for (int x = 0; x < width; x++) {
			u8& target = vram[(d + x * stepX) & vramMask];
			u8 pixel = fill ? color : vram[(s + x * stepX) & vramMask];
			u8 result = Mix(mix, pixel, target);
			target = (result & mask) | (target & ~mask);
		}
	}
}


// The 256 color mode: 8 pixels per character clock, 16 bytes per offset
// unit.
void
HobbitVideo::ComputeMode(DisplayMode& mode)
{
	if ((fGc[6] & 1) == 0 || (fGc[5] & 0x40) == 0) {
		VgaCore::ComputeMode(mode);
		return;
	}
	mode.format = kIndexed8;
	mode.width = (fCrtc[1] + 1) * 8;
	int lines = fCrtc[0x12] | ((fCrtc[7] & 0x02) << 7)
		| ((fCrtc[7] & 0x40) << 3);
	mode.lineRepeat = _StandardLineRepeat();
	mode.height = (lines + 1) / mode.lineRepeat;
	mode.start = _StandardStart() * 4;
	mode.pitch = fCrtc[0x13] * 16;
	mode.pixelDoubling = 1;
}

}	// namespace bebox

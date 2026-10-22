/*
 * BeBox emulator - S3 Vision864 PCI graphics card (Number Nine #9 GXE64).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "S3Vision864.h"

#include "../Log.h"
#include "../bus/Mpc105.h"

#include <algorithm>
#include <cstring>

namespace bebox {

namespace {

// 8514/A style registers (16 bits wide; the color registers and PIX_TRANS
// also take 32-bit accesses).
const u32 kEnginePorts[] = { 0x42e8, 0x46e8, 0x4ae8, 0x82e8, 0x86e8, 0x8ae8,
	0x8ee8, 0x92e8, 0x96e8, 0x9ae8, 0x9ee8, 0xa2e8, 0xa6e8, 0xaae8, 0xaee8,
	0xb2e8, 0xb6e8, 0xbae8, 0xbee8, 0xe2e8 };

bool
WidePort(u32 port)
{
	return port == 0xe2e8 || (port >= 0xa2e8 && port <= 0xb2e8);
}


u32
SwapBytes(u32 value, int size)
{
	switch (size) {
		case 2:
			return (u16)((value >> 8) | (value << 8));
		case 4:
			return (value >> 24) | ((value >> 8) & 0xff00)
				| ((value << 8) & 0xff0000) | (value << 24);
	}
	return value;
}


// 12-bit coordinates are two's complement (drivers use small negative
// values when clipping).
int
Coordinate(u16 value)
{
	return (int)((s16)(value << 4) >> 4);
}


// 14-bit signed Bresenham terms
int
Term(u16 value)
{
	return (int)((s16)(value << 2) >> 2);
}

}	// namespace


class S3Vision864::Ports : public ByteIoDevice {
public:
	Ports(S3Vision864& card) : fCard(card) {}
	virtual u8 IoRead8(u32 port) { return fCard.VgaRead(port); }
	virtual void IoWrite8(u32 port, u8 value) { fCard.VgaWrite(port, value); }
	virtual const char* IoName() const { return "vga"; }

private:
	S3Vision864& fCard;
};


class S3Vision864::EnginePorts : public IoDevice {
public:
	EnginePorts(S3Vision864& card) : fCard(card) {}

	virtual u32 IoRead(u32 port, int size)
	{
		return fCard._EngineRead(port, size);
	}

	virtual void IoWrite(u32 port, u32 value, int size)
	{
		fCard._EngineWrite(port, value, size);
	}

	virtual const char* IoName() const { return "s3-engine"; }

private:
	S3Vision864& fCard;
};


// 0xa0000-0xbffff in PCI memory space. With memory mapped I/O enabled,
// A0000h-A7FFFh takes image transfer data (PIX_TRANS) and A8000h-AFFFFh
// holds the engine registers at A0000h + port. Values arrive in byte
// address order (the processor is big endian); registers are little
// endian like the I/O ports.
class S3Vision864::LegacyWindow : public MmioDevice {
public:
	LegacyWindow(S3Vision864& card) : fCard(card) {}

	virtual u32 MmioRead(u32 addr, int size, int initiator)
	{
		u32 offset = addr - (Mpc105::kPciMemoryBase + 0xa0000);
		if (fCard._MmioEnabled() && offset < 0x10000) {
			if (offset < 0x8000)
				return 0;
			return SwapBytes(fCard._EngineRead(offset, size), size);
		}
		u32 value = 0;
		for (int i = 0; i < size; i++)
			value = (value << 8) | fCard._WindowRead(offset + i);
		return value;
	}

	virtual void MmioWrite(u32 addr, u32 value, int size, int initiator)
	{
		u32 offset = addr - (Mpc105::kPciMemoryBase + 0xa0000);
		if (fCard._MmioEnabled() && offset < 0x10000) {
			if (offset < 0x8000)
				fCard._PixTransWrite(SwapBytes(value, size), size);
			else
				fCard._EngineWrite(offset, SwapBytes(value, size), size);
			return;
		}
		for (int i = 0; i < size; i++)
			fCard._WindowWrite(offset + i, (u8)(value >> (8 * (size - 1 - i))));
	}

	virtual const char* MmioName() const { return "s3-legacy"; }

private:
	S3Vision864& fCard;
};


S3Vision864::S3Vision864(PhysBus& bus, IoSpace& io, Scheduler& scheduler,
	u64 ticksPerSecond, u32 vramSize)
	:
	PciDevice(0x5333, 0x88c0, 0x030000, 0x00),
	VgaCore(scheduler, ticksPerSecond, vramSize),
	fBus(bus),
	fIo(io),
	fLegacyMapped(false),
	fLinearBase(0),
	fLinearSize(0)
{
	DeclareBar(0, kBarMemory, kBarSize, false);
	SetInterruptPin(1);
	fPorts = new Ports(*this);
	fEnginePorts = new EnginePorts(*this);
	fLegacy = new LegacyWindow(*this);
	io.Map(0x3b0, 0x30, fPorts);
	for (u32 port : kEnginePorts)
		io.Map(port, WidePort(port) ? 4 : 2, fEnginePorts);
	Reset();
}


S3Vision864::~S3Vision864()
{
	if (fLinearBase != 0)
		fBus.Unmap(fLinearBase, fLinearSize);
	if (fLegacyMapped)
		fBus.Unmap(Mpc105::kPciMemoryBase + 0xa0000, 0x20000);
	fIo.Unmap(0x3b0, 0x30);
	for (u32 port : kEnginePorts)
		fIo.Unmap(port, WidePort(port) ? 4 : 2);
	delete fPorts;
	delete fEnginePorts;
	delete fLegacy;
}


void
S3Vision864::Reset()
{
	ResetVga();
	u32 size = VramSize();
	u8 sizeCode = size >= (4u << 20) ? 0 : size >= (2u << 20) ? 4 : 6;
	fCrtc[0x2d] = 0x88;
	fCrtc[0x2e] = 0xc0;
	fCrtc[0x2f] = 0x00;
	fCrtc[0x30] = 0xc0;		// Vision864
	// reset straps: memory size, fast page mode, PCI bus
	fCrtc[0x36] = (u8)((sizeCode << 5) | 0x0c | 0x02);
	fCrtc[0x37] = 0xe5;
	fCrtc[0x68] = 0x0f;
	fBank = 0;
	fDacCommand = 0;
	fDacReads = 0;
	memset(fCursorFg, 0, sizeof(fCursorFg));
	memset(fCursorBg, 0, sizeof(fCursorBg));
	fCursorFgIndex = fCursorBgIndex = 0;
	memset(&fEngine, 0, sizeof(fEngine));
	fEngine.wrtMask = 0xffffffff;
	fEngine.rdMask = 0xffffffff;
	fEngine.scissorRight = fEngine.scissorBottom = 0x0fff;
	fEngine.frgdMix = 0x27;		// foreground color, source
	fEngine.bkgdMix = 0x07;
	fEngine.pendingPort = 0;
	memset(&fTransfer, 0, sizeof(fTransfer));
	fTransferBytes = 0;
	fTransferCount = 0;
	fConfig[4] = 0;
	_UpdateMappings();
}


void
S3Vision864::BarsChanged()
{
	// the PCI base address and CR59/CR5A are the same register bits
	u32 address = BarAddress(0);
	fCrtc[0x59] = (u8)(address >> 24);
	fCrtc[0x5a] = (u8)((fCrtc[0x5a] & 0x7f) | ((address >> 16) & 0x80));
	_UpdateMappings();
}


void
S3Vision864::_UpdateMappings()
{
	bool memory = MemoryEnabled();
	if (memory && !fLegacyMapped) {
		fBus.MapDevice(Mpc105::kPciMemoryBase + 0xa0000, 0x20000, fLegacy);
		fLegacyMapped = true;
	} else if (!memory && fLegacyMapped) {
		fBus.Unmap(Mpc105::kPciMemoryBase + 0xa0000, 0x20000);
		fLegacyMapped = false;
	}

	static const u32 kWindowSizes[4] = { 64 << 10, 1 << 20, 2 << 20,
		8 << 20 };
	u32 address = ((fCrtc[0x59] << 24) | (fCrtc[0x5a] << 16));
	u32 size = kWindowSizes[fCrtc[0x58] & 3];
	address &= ~(size - 1);
	u32 base = Mpc105::kPciMemoryBase + address;
	bool linear = memory && _LinearEnabled() && address != 0
		&& address < 0x3f000000 && size >= (1u << 20);
	if (linear && base == fLinearBase && size == fLinearSize)
		return;
	if (fLinearBase != 0) {
		fBus.Unmap(fLinearBase, fLinearSize);
		fLinearBase = 0;
	}
	if (!linear)
		return;
	u32 vram = VramSize();
	for (u32 offset = 0; offset < size; offset += vram)
		fBus.MapMemory(base + offset, std::min(vram, size - offset), Vram(),
			true);
	fLinearBase = base;
	fLinearSize = size;
	BB_LOG(kLogVideo, kLogDebug, "s3: linear window at %08x (%u KB)", base,
		size >> 10);
}


// #pragma mark - VGA extensions


u8
S3Vision864::CrtcRead(u8 index)
{
	switch (index) {
		case 0x30:
			return 0xc0;
		case 0x45:
			// reading the mode register resets the color stack pointers
			fCursorFgIndex = fCursorBgIndex = 0;
			return fCrtc[0x45];
		case 0x4a:
			return fCursorFg[fCursorFgIndex++ & 3];
		case 0x4b:
			return fCursorBg[fCursorBgIndex++ & 3];
	}
	return VgaCore::CrtcRead(index);
}


void
S3Vision864::CrtcWrite(u8 index, u8 value)
{
	if (index < 0x2d) {
		VgaCore::CrtcWrite(index, value);
		return;
	}
	if (index == 0x38 || index == 0x39) {
		fCrtc[index] = value;
		return;
	}
	// read only: chip ID; reset straps
	if (index <= 0x30 || index == 0x36 || index == 0x37)
		return;
	if (index < 0x40 ? !_S3Unlocked() : !_SystemUnlocked()) {
		BB_LOG(kLogVideo, kLogDebug, "s3: CR%02x write while locked", index);
		return;
	}
	BB_LOG(kLogVideo, kLogTrace, "CR%02x = %02x", index, value);
	switch (index) {
		case 0x4a:
			fCursorFg[fCursorFgIndex++ & 3] = value;
			return;
		case 0x4b:
			fCursorBg[fCursorBgIndex++ & 3] = value;
			return;
	}
	fCrtc[index] = value;
	switch (index) {
		case 0x35:
		case 0x51:
			fBank = (fCrtc[0x35] & 0x0f) | (((fCrtc[0x51] >> 2) & 3) << 4);
			break;
		case 0x6a:
			fBank = value & 0x7f;
			break;
		case 0x59:
		case 0x5a:
		{
			// mirror into the PCI base address (bits 23-31)
			u32 address = ((fCrtc[0x59] << 24) | (fCrtc[0x5a] << 16))
				& 0xff800000;
			fConfig[0x10] = 0;
			fConfig[0x11] = 0;
			fConfig[0x12] = (u8)(address >> 16);
			fConfig[0x13] = (u8)(address >> 24);
			_UpdateMappings();
			break;
		}
		case 0x53:
		case 0x58:
			_UpdateMappings();
			break;
	}
}


// The 21C498 command register: through RS2 (CR55 bits 0-1, or CR43 bit 1
// when those are 0), or as the fifth access after four reads of 3C6h; the
// sixth and seventh reads return the manufacturer and device codes.
u8
S3Vision864::DacMaskRead()
{
	bool rs2 = (fCrtc[0x55] & 3) != 0 ? (fCrtc[0x55] & 1) != 0
		: (fCrtc[0x43] & 2) != 0;
	if (rs2)
		return fDacCommand;
	int reads = fDacReads++;
	switch (reads) {
		case 4:
			return fDacCommand;
		case 5:
			return 0x84;
		case 6:
			fDacReads = 0;
			return 0x98;
	}
	return fDacMask;
}


void
S3Vision864::DacMaskWrite(u8 value)
{
	bool rs2 = (fCrtc[0x55] & 3) != 0 ? (fCrtc[0x55] & 1) != 0
		: (fCrtc[0x43] & 2) != 0;
	if (rs2 || fDacReads == 4) {
		fDacReads = 0;
		fDacCommand = value;
		fDac8Bit = (value & 0x02) != 0;
		BB_LOG(kLogVideo, kLogDebug, "s3: RAMDAC command = %02x", value);
		return;
	}
	fDacReads = 0;
	fDacMask = value;
}


u32
S3Vision864::LegacyBankOffset(u32 offset)
{
	return (fCrtc[0x31] & 1) != 0 ? fBank << 16 : 0;
}


u8
S3Vision864::_WindowRead(u32 offset)
{
	if (_EnhancedMapping()) {
		if (offset >= 0x10000)
			return 0xff;
		return Vram()[(LegacyBankOffset(offset) + offset)
			& (VramSize() - 1)];
	}
	return LegacyRead(offset);
}


void
S3Vision864::_WindowWrite(u32 offset, u8 value)
{
	if (_EnhancedMapping()) {
		if (offset < 0x10000) {
			Vram()[(LegacyBankOffset(offset) + offset) & (VramSize() - 1)]
				= value;
		}
		return;
	}
	LegacyWrite(offset, value);
}


// CRTC clocks per pixel in the enhanced modes: the CRTC counts bytes
// through the 21C498's pixel port (CR67 pixel mode; else the RAMDAC mode).
int
S3Vision864::_HorizontalFactor() const
{
	switch (fCrtc[0x67] >> 4) {
		case 3:
		case 5:
			return 2;
		case 7:
		case 13:
			return 4;
	}
	switch (_DacMode()) {
		case 1:
		case 3:
		case 6:
			return 2;
		case 5:
			return 4;
	}
	return 1;
}


void
S3Vision864::ComputeMode(DisplayMode& mode)
{
	bool enhanced = (fEngine.advFunc & 1) != 0
		|| (fCrtc[0x3a] & 0x10) != 0;
	if (!enhanced) {
		VgaCore::ComputeMode(mode);
		return;
	}
	int chars = (fCrtc[1] | ((fCrtc[0x5d] & 0x02) << 7)) + 1;
	int width = chars * ((fCrtc[0x43] & 0x80) != 0 ? 16 : 8);
	mode.width = width / _HorizontalFactor();
	int lines = fCrtc[0x12] | ((fCrtc[7] & 0x02) << 7)
		| ((fCrtc[7] & 0x40) << 3) | ((fCrtc[0x5e] & 0x02) << 9);
	mode.lineRepeat = _StandardLineRepeat();
	mode.height = (lines + 1) / mode.lineRepeat;
	u32 start = (fCrtc[0x0c] << 8) | fCrtc[0x0d];
	if ((fCrtc[0x69] & 0x1f) != 0)
		start |= (fCrtc[0x69] & 0x1f) << 16;
	else
		start |= (((fCrtc[0x31] >> 4) & 3) << 16) | ((fCrtc[0x51] & 3) << 18);
	mode.start = start * 4;
	u32 offset = fCrtc[0x13];
	if ((fCrtc[0x51] & 0x30) != 0)
		offset |= ((fCrtc[0x51] >> 4) & 3) << 8;
	else if ((fCrtc[0x43] & 0x04) != 0)
		offset |= 0x100;
	mode.pitch = offset * 8;
	mode.pixelDoubling = 1;
	switch (_DacMode()) {
		case 1:
			mode.format = kRgb555;
			break;
		case 3:
		case 6:
			mode.format = kRgb565;
			break;
		case 5:
			mode.format = kRgbx8888;
			break;
		default:
			mode.format = kIndexed8;
			break;
	}
}


// The hardware cursor works in half pixels in 32 bpp (the 21C498 takes a
// pixel in two transfers): BeOS's drivers write twice the pointer's x, and
// a cursor pattern with every pixel doubled. 15/16 bpp is taken to work
// the same way.
int
S3Vision864::_CursorUnitsPerPixel() const
{
	return _HorizontalFactor() >= 2 ? 2 : 1;
}


bool
S3Vision864::HardwareCursor(int& x, int& y) const
{
	if ((fCrtc[0x45] & 1) == 0)
		return false;
	x = (((fCrtc[0x46] & 7) << 8) | fCrtc[0x47]) / _CursorUnitsPerPixel();
	y = ((fCrtc[0x48] & 7) << 8) | fCrtc[0x49];
	return true;
}


// The pattern's rows from CR4F on, from the cursor's line on.
bool
S3Vision864::OverlayLines(int height, int& top, int& bottom) const
{
	int x, y;
	if (!HardwareCursor(x, y))
		return false;
	int offsetY = fCrtc[0x4f] & 0x3f;
	top = std::max(0, y);
	bottom = std::min(height, y + 64 - offsetY);
	return top < bottom;
}


// 64x64 cursor, 2 bits per pixel: per row four groups of 16 pixels, each a
// 16-bit word of A bits followed by a word of B bits (leftmost pixel in
// bit 7 of the first byte).
void
S3Vision864::DrawOverlays(u32* frame, int width, int height, int stride)
{
	int cursorX, cursorY;
	if (!HardwareCursor(cursorX, cursorY))
		return;
	// position and pattern columns in cursor units
	int units = _CursorUnitsPerPixel();
	int originX = ((fCrtc[0x46] & 7) << 8) | fCrtc[0x47];
	u32 start = (((fCrtc[0x4c] & 0x1f) << 8) | fCrtc[0x4d]) * 1024;
	int offsetX = fCrtc[0x4e] & 0x3f;
	int offsetY = fCrtc[0x4f] & 0x3f;
	bool x11 = (fCrtc[0x55] & 0x10) != 0;
	u32 foreground, background;
	switch (fMode.format) {
		case kRgbx8888:
			foreground = fCursorFg[0] | (fCursorFg[1] << 8)
				| (fCursorFg[2] << 16);
			background = fCursorBg[0] | (fCursorBg[1] << 8)
				| (fCursorBg[2] << 16);
			break;
		case kRgb565:
		case kRgb555:
		{
			auto convert = [this](const u8* c) {
				u16 p = (u16)(c[0] | (c[1] << 8));
				u32 r, g, b;
				if (fMode.format == kRgb565) {
					r = (p >> 11) & 0x1f;
					g = (p >> 5) & 0x3f;
					g = (g << 2) | (g >> 4);
				} else {
					r = (p >> 10) & 0x1f;
					g = (p >> 5) & 0x1f;
					g = (g << 3) | (g >> 2);
				}
				b = p & 0x1f;
				r = (r << 3) | (r >> 2);
				b = (b << 3) | (b >> 2);
				return (r << 16) | (g << 8) | b;
			};
			foreground = convert(fCursorFg);
			background = convert(fCursorBg);
			break;
		}
		default:
			foreground = PaletteColor(fCursorFg[0]);
			background = PaletteColor(fCursorBg[0]);
			break;
	}
	u32 mask = VramSize() - 1;
	const u8* vram = Vram();
	for (int row = offsetY; row < 64; row++) {
		int y = cursorY + row - offsetY;
		if (y < 0 || y >= height)
			continue;
		u32* line = frame + (size_t)y * stride;
		int firstX = std::max(0, (originX + units - 1) / units);
		int lastX = std::min(width - 1, (originX + 63 - offsetX) / units);
		for (int x = firstX; x <= lastX; x++) {
			// the column under the left part of the pixel
			int column = x * units - originX + offsetX;
			if (column < offsetX || column > 63)
				continue;
			u32 address = start + row * 16 + (column / 16) * 4
				+ (column % 16) / 8;
			int bit = 7 - (column & 7);
			bool a = (vram[address & mask] >> bit) & 1;
			bool b = (vram[(address + 2) & mask] >> bit) & 1;
			if (x11) {
				if (a)
					line[x] = b ? foreground : background;
			} else if (!a)
				line[x] = b ? foreground : background;
			else if (b)
				line[x] ^= 0x00ffffff;
		}
	}
}


// #pragma mark - graphics engine registers


u32
S3Vision864::_EngineRead(u32 port, int size)
{
	if ((fCrtc[0x40] & 1) == 0 && port != 0x4ae8)
		return size == 1 ? 0xff : size == 2 ? 0xffff : 0xffffffff;
	u32 base = port & ~1u;
	u32 value;
	switch (base) {
		case 0x42e8:
			value = (fEngine.subsysStat & 0x0f) | 0x80;
			break;
		case 0x4ae8:
			value = fEngine.advFunc;
			break;
		case 0x82e8: value = fEngine.curY; break;
		case 0x86e8: value = fEngine.curX; break;
		case 0x8ae8: value = fEngine.destY; break;
		case 0x8ee8: value = fEngine.destX; break;
		case 0x92e8: value = fEngine.errTerm; break;
		case 0x96e8: value = fEngine.majAxis; break;
		case 0x9ae8:
			// FIFO empty, engine idle
			value = 0x0400;
			break;
		case 0xa2e8:
			value = size == 4 ? fEngine.bkgdColor
				: _ColorRead(fEngine.bkgdColor);
			break;
		case 0xa6e8:
			value = size == 4 ? fEngine.frgdColor
				: _ColorRead(fEngine.frgdColor);
			break;
		case 0xaae8:
			value = size == 4 ? fEngine.wrtMask : _ColorRead(fEngine.wrtMask);
			break;
		case 0xaee8:
			value = size == 4 ? fEngine.rdMask : _ColorRead(fEngine.rdMask);
			break;
		case 0xb2e8:
			value = size == 4 ? fEngine.colorCmp
				: _ColorRead(fEngine.colorCmp);
			break;
		case 0xb6e8: value = fEngine.bkgdMix; break;
		case 0xbae8: value = fEngine.frgdMix; break;
		case 0xbee8: value = _MultifunctionRead(); break;
		default:
			value = 0xffffffff;
			break;
	}
	if (size == 1)
		return (port & 1) != 0 ? (value >> 8) & 0xff : value & 0xff;
	return size == 2 ? value & 0xffff : value;
}


void
S3Vision864::_EngineWrite(u32 port, u32 value, int size)
{
	BB_LOG(kLogVideo, kLogTrace, "s3: write%d %04x = %x", size * 8, port,
		value);
	if ((fCrtc[0x40] & 1) == 0 && port != 0x4ae8 && port != 0x46e8) {
		BB_LOG(kLogVideo, kLogDebug, "s3: engine write %04x while disabled",
			port);
		return;
	}
	if (size == 1) {
		_EngineWrite8(port, (u8)value);
		return;
	}
	u32 base = port & ~1u;
	if (base == 0xe2e8) {
		_PixTransWrite(value, size);
		return;
	}
	if (size == 4) {
		switch (base) {
			case 0xa2e8:
				_ColorWrite(fEngine.bkgdColor, value, 4);
				return;
			case 0xa6e8:
				_ColorWrite(fEngine.frgdColor, value, 4);
				return;
			case 0xaae8:
				_ColorWrite(fEngine.wrtMask, value, 4);
				return;
			case 0xaee8:
				_ColorWrite(fEngine.rdMask, value, 4);
				return;
			case 0xb2e8:
				_ColorWrite(fEngine.colorCmp, value, 4);
				return;
		}
		_EngineWrite16(base, (u16)value);
		if (fIo.DeviceAt(base + 2) == fEnginePorts)
			_EngineWrite16(base + 2, (u16)(value >> 16));
		return;
	}
	_EngineWrite16(base, (u16)value);
}


// Byte writes change that byte of a register; the registers that start
// something (CMD, SHORT_STROKE, the multifunction register) take the word
// when its high byte arrives.
void
S3Vision864::_EngineWrite8(u32 port, u8 value)
{
	u32 base = port & ~1u;
	int shift = (port & 1) * 8;
	u32* color = nullptr;
	u16* reg = nullptr;
	switch (base) {
		case 0xe2e8:
			_PixTransWrite(value, 1);
			return;
		case 0xa2e8: color = &fEngine.bkgdColor; break;
		case 0xa6e8: color = &fEngine.frgdColor; break;
		case 0xaae8: color = &fEngine.wrtMask; break;
		case 0xaee8: color = &fEngine.rdMask; break;
		case 0xb2e8: color = &fEngine.colorCmp; break;
		case 0x82e8: reg = &fEngine.curY; break;
		case 0x86e8: reg = &fEngine.curX; break;
		case 0x8ae8: reg = &fEngine.destY; break;
		case 0x8ee8: reg = &fEngine.destX; break;
		case 0x92e8: reg = &fEngine.errTerm; break;
		case 0x96e8: reg = &fEngine.majAxis; break;
		case 0xb6e8: reg = &fEngine.bkgdMix; break;
		case 0xbae8: reg = &fEngine.frgdMix; break;
		case 0x42e8:
		case 0x46e8:
		case 0x4ae8:
		{
			u16 current = base == 0x4ae8 ? fEngine.advFunc
				: base == 0x42e8 ? fEngine.subsysCntl : 0;
			_EngineWrite16(base, (u16)((current & ~(0xff << shift))
				| (value << shift)));
			return;
		}
	}
	if (color != nullptr) {
		if (_BytesPerPixel() == 4 && (fEngine.multMisc & 0x200) == 0
			&& fEngine.highHalf)
			shift += 16;
		*color = (*color & ~(0xffu << shift)) | ((u32)value << shift);
		return;
	}
	if (reg != nullptr) {
		*reg = (u16)((*reg & ~(0xff << shift)) | (value << shift));
		return;
	}
	if ((port & 1) == 0) {
		fEngine.pendingPort = port;
		fEngine.pendingLow = value;
		return;
	}
	u8 low = fEngine.pendingPort == base ? fEngine.pendingLow : 0;
	fEngine.pendingPort = 0;
	_EngineWrite16(base, (u16)(low | (value << 8)));
}


void
S3Vision864::_EngineWrite16(u32 port, u16 value)
{
	BB_LOG(kLogVideo, kLogTrace, "s3: %04x = %04x", port, value);
	switch (port) {
		case 0x42e8:
			fEngine.subsysStat &= ~(value & 0x0f);
			fEngine.subsysCntl = value;
			if ((value & 0xc000) == 0x8000)
				fTransfer.active = false;	// engine reset
			break;
		case 0x46e8:
			break;
		case 0x4ae8:
			fEngine.advFunc = value;
			_UpdateMappings();
			break;
		case 0x82e8: fEngine.curY = value; break;
		case 0x86e8: fEngine.curX = value; break;
		case 0x8ae8: fEngine.destY = value; break;
		case 0x8ee8: fEngine.destX = value; break;
		case 0x92e8: fEngine.errTerm = value; break;
		case 0x96e8: fEngine.majAxis = value; break;
		case 0x9ae8:
			fEngine.cmd = value;
			_Command(value);
			break;
		case 0x9ee8:
			_ShortStroke((u8)value);
			_ShortStroke((u8)(value >> 8));
			break;
		case 0xa2e8: _ColorWrite(fEngine.bkgdColor, value, 2); break;
		case 0xa6e8: _ColorWrite(fEngine.frgdColor, value, 2); break;
		case 0xaae8: _ColorWrite(fEngine.wrtMask, value, 2); break;
		case 0xaee8: _ColorWrite(fEngine.rdMask, value, 2); break;
		case 0xb2e8: _ColorWrite(fEngine.colorCmp, value, 2); break;
		case 0xb6e8: fEngine.bkgdMix = value; break;
		case 0xbae8: fEngine.frgdMix = value; break;
		case 0xbee8: _MultifunctionWrite(value); break;
		default:
			BB_LOG(kLogVideo, kLogDebug, "s3: write %04x to %04x", value,
				port);
			break;
	}
}


// 16-bit accesses to the color registers: in 32 bpp they reach one half,
// selected by RSF (BEE8h index E bit 4), which toggles with each access.
void
S3Vision864::_ColorWrite(u32& reg, u32 value, int size)
{
	if (size == 4) {
		reg = value;
		return;
	}
	if (_BytesPerPixel() == 4 && (fEngine.multMisc & 0x200) == 0) {
		if (fEngine.highHalf)
			reg = (reg & 0x0000ffff) | ((value & 0xffff) << 16);
		else
			reg = (reg & 0xffff0000) | (value & 0xffff);
		fEngine.highHalf = !fEngine.highHalf;
		return;
	}
	reg = value & 0xffff;
}


u16
S3Vision864::_ColorRead(u32 reg)
{
	if (_BytesPerPixel() == 4 && (fEngine.multMisc & 0x200) == 0) {
		u16 value = fEngine.highHalf ? (u16)(reg >> 16) : (u16)reg;
		fEngine.highHalf = !fEngine.highHalf;
		return value;
	}
	return (u16)reg;
}


void
S3Vision864::_MultifunctionWrite(u16 value)
{
	u16 data = value & 0x0fff;
	switch (value >> 12) {
		case 0x0: fEngine.minAxis = data; break;
		case 0x1: fEngine.scissorTop = data; break;
		case 0x2: fEngine.scissorLeft = data; break;
		case 0x3: fEngine.scissorBottom = data; break;
		case 0x4: fEngine.scissorRight = data; break;
		case 0xa: fEngine.pixCntl = data; break;
		case 0xd: fEngine.multMisc2 = data; break;
		case 0xe:
			fEngine.multMisc = data;
			fEngine.highHalf = (data & 0x10) != 0;
			break;
		case 0xf: fEngine.readSel = data; break;
		default:
			BB_LOG(kLogVideo, kLogDebug, "s3: multifunction %x = %03x",
				value >> 12, data);
			break;
	}
}


u16
S3Vision864::_MultifunctionRead()
{
	int select = fEngine.readSel & 0x0f;
	fEngine.readSel = (fEngine.readSel & ~0x0f) | ((select + 1) & 0x0f);
	switch (select) {
		case 0: return fEngine.minAxis;
		case 1: return 0x1000 | fEngine.scissorTop;
		case 2: return 0x2000 | fEngine.scissorLeft;
		case 3: return 0x3000 | fEngine.scissorBottom;
		case 4: return 0x4000 | fEngine.scissorRight;
		case 5: return 0xa000 | fEngine.pixCntl;
		case 6: return 0xe000 | fEngine.multMisc;
		case 7: return 0x0400;
		case 8: return (fEngine.subsysStat & 0x0f) | 0x80;
		case 9: return 0;
		case 10: return 0xd000 | fEngine.multMisc2;
	}
	return 0;
}


// #pragma mark - graphics engine


int
S3Vision864::_BytesPerPixel() const
{
	switch ((fCrtc[0x50] >> 4) & 3) {
		case 1: return 2;
		case 2: return 3;
		case 3: return 4;
	}
	return 1;
}


u32
S3Vision864::_EngineStride() const
{
	// CR50 bits 7-6 (640, 800, 1280 as on the older chips) extended by
	// bit 0 (1152, 1600)
	switch ((((fCrtc[0x50] >> 6) & 3) << 1) | (fCrtc[0x50] & 1)) {
		case 1: return 1152;
		case 2: return 640;
		case 4: return 800;
		case 5: return 1600;
		case 6: return 1280;
	}
	return (fCrtc[0x31] & 0x02) != 0 ? 2048 : 1024;
}


u32
S3Vision864::_PixelAddress(int x, int y) const
{
	return ((u32)y * _EngineStride() + (u32)x) * _BytesPerPixel();
}


u32
S3Vision864::_ReadPixel(int x, int y) const
{
	if (x < 0 || y < 0)
		return 0;
	u32 address = _PixelAddress(x, y);
	u32 mask = VramSize() - 1;
	const u8* vram = fVram.data();
	u32 value = 0;
	for (int i = _BytesPerPixel() - 1; i >= 0; i--)
		value = (value << 8) | vram[(address + i) & mask];
	return value;
}


bool
S3Vision864::_Clipped(int x, int y) const
{
	bool inside = x >= fEngine.scissorLeft && x <= fEngine.scissorRight
		&& y >= fEngine.scissorTop && y <= fEngine.scissorBottom;
	if ((fEngine.multMisc & 0x20) != 0)
		return inside;
	return !inside;
}


u32
S3Vision864::_Mix(u16 mix, u32 s, u32 d) const
{
	switch (mix & 0x0f) {
		case 0x0: return ~d;
		case 0x1: return 0;
		case 0x2: return 0xffffffff;
		case 0x3: return d;
		case 0x4: return ~s;
		case 0x5: return s ^ d;
		case 0x6: return ~(s ^ d);
		case 0x7: return s;
		case 0x8: return ~(s & d);
		case 0x9: return ~s | d;
		case 0xa: return s | ~d;
		case 0xb: return s | d;
		case 0xc: return s & d;
		case 0xd: return s & ~d;
		case 0xe: return ~s & d;
		default: return ~(s | d);
	}
}


// The source a mix register selects: background color, foreground color,
// CPU data or the bitmap (source pixel).
u32
S3Vision864::_MixSource(u16 mix, int x, int y, u32 cpuPixel) const
{
	switch ((mix >> 5) & 3) {
		case 0: return fEngine.bkgdColor;
		case 1: return fEngine.frgdColor;
		case 2: return cpuPixel;
	}
	return _ReadPixel(x, y);
}


// Writes one pixel through the mix, color compare, write mask and the
// scissors. 'source' is the bitmap source pixel (BitBLT, pattern) or the
// CPU pixel.
void
S3Vision864::_WritePixel(int x, int y, u32 source, bool foreground)
{
	if (x < 0 || y < 0 || _Clipped(x, y))
		return;
	u16 mix = foreground ? fEngine.frgdMix : fEngine.bkgdMix;
	u32 s;
	switch ((mix >> 5) & 3) {
		case 0: s = fEngine.bkgdColor; break;
		case 1: s = fEngine.frgdColor; break;
		default: s = source; break;
	}
	int bpp = _BytesPerPixel();
	u32 pixelMask = bpp == 4 ? 0xffffffff : (1u << (bpp * 8)) - 1;
	if ((fEngine.multMisc & 0x100) != 0) {
		// color compare: update only where the source differs (or, with
		// SRC NE, equals)
		bool equal = (s & pixelMask) == (fEngine.colorCmp & pixelMask);
		if (equal != ((fEngine.multMisc & 0x80) != 0))
			return;
	}
	u32 d = _ReadPixel(x, y);
	u32 result = _Mix(mix, s, d);
	result = (result & fEngine.wrtMask) | (d & ~fEngine.wrtMask);
	u32 address = _PixelAddress(x, y);
	u32 mask = VramSize() - 1;
	for (int i = 0; i < bpp; i++)
		fVram[(address + i) & mask] = (u8)(result >> (8 * i));
}


void
S3Vision864::_Command(u16 cmd)
{
	int command = cmd >> 13;
	BB_LOG(kLogVideo, kLogDebug, "s3: command %04x (%d) cur %d,%d dest %d,%d "
		"size %d,%d err %d mix %02x/%02x pixcntl %03x fg %x bg %x mask %x "
		"bpp %d stride %u", cmd, command,
		Coordinate(fEngine.curX), Coordinate(fEngine.curY),
		Coordinate(fEngine.destX), Coordinate(fEngine.destY),
		fEngine.majAxis, fEngine.minAxis, Term(fEngine.errTerm),
		fEngine.frgdMix, fEngine.bkgdMix, fEngine.pixCntl, fEngine.frgdColor,
		fEngine.bkgdColor, fEngine.wrtMask, _BytesPerPixel(),
		_EngineStride());
	fTransfer.active = false;
	switch (command) {
		case 0:
			break;
		case 1:
			if ((cmd & 0x100) != 0)
				BB_LOG(kLogVideo, kLogInfo, "s3: line with CPU data");
			_Line(cmd);
			break;
		case 2:
		case 6:
		case 7:
			if ((cmd & 0x100) != 0)
				_StartTransfer(cmd);
			else
				_RectOrBlt(cmd);
			break;
		default:
			BB_LOG(kLogVideo, kLogInfo, "s3: unimplemented command %04x",
				cmd);
			break;
	}
}


// Rectangle fill (2), BitBLT (6) and pattern fill (7) without CPU data.
void
S3Vision864::_RectOrBlt(u16 cmd)
{
	int command = cmd >> 13;
	int width = (fEngine.majAxis & 0x0fff) + 1;
	int height = (fEngine.minAxis & 0x0fff) + 1;
	int stepX = (cmd & 0x20) != 0 ? 1 : -1;
	int stepY = (cmd & 0x80) != 0 ? 1 : -1;
	int srcX = Coordinate(fEngine.curX);
	int srcY = Coordinate(fEngine.curY);
	int dstX = command == 2 ? srcX : Coordinate(fEngine.destX);
	int dstY = command == 2 ? srcY : Coordinate(fEngine.destY);
	bool draw = (cmd & 0x10) != 0;
	int mixSelect = (fEngine.pixCntl >> 6) & 3;
	if (draw) {
		for (int j = 0; j < height; j++) {
			int dy = dstY + j * stepY;
			for (int i = 0; i < width; i++) {
				int dx = dstX + i * stepX;
				u32 source = 0;
				if (command == 6) {
					source = _ReadPixel(srcX + i * stepX, srcY + j * stepY);
				} else if (command == 7) {
					source = _ReadPixel(srcX + ((dx - dstX) & 7),
						srcY + ((dy - dstY) & 7));
				}
				bool foreground = true;
				if (mixSelect == 3)
					foreground = (source & fEngine.rdMask) != 0;
				_WritePixel(dx, dy, source, foreground);
			}
		}
	}
	if (command == 2)
		fEngine.curY = (u16)((srcY + height * stepY) & 0x0fff);
	else {
		fEngine.curY = (u16)((srcY + height * stepY) & 0x0fff);
		fEngine.destY = (u16)((dstY + height * stepY) & 0x0fff);
	}
}


// Bresenham (from the step registers) or radial (direction in the
// command) lines.
void
S3Vision864::_Line(u16 cmd)
{
	int x = Coordinate(fEngine.curX);
	int y = Coordinate(fEngine.curY);
	int count = (fEngine.majAxis & 0x0fff) + 1;
	bool draw = (cmd & 0x10) != 0;
	bool lastOff = (cmd & 0x04) != 0;
	if ((cmd & 0x08) != 0) {
		static const int kDx[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
		static const int kDy[8] = { 0, -1, -1, -1, 0, 1, 1, 1 };
		int direction = (cmd >> 5) & 7;
		for (int n = 0; n < count; n++) {
			if (draw && !(lastOff && n == count - 1))
				_WritePixel(x, y, 0, true);
			if (n < count - 1) {
				x += kDx[direction];
				y += kDy[direction];
			}
		}
	} else {
		int stepX = (cmd & 0x20) != 0 ? 1 : -1;
		int stepY = (cmd & 0x80) != 0 ? 1 : -1;
		bool yMajor = (cmd & 0x40) != 0;
		int error = Term(fEngine.errTerm);
		int axial = Term(fEngine.destY);
		int diagonal = Term(fEngine.destX);
		for (int n = 0; n < count; n++) {
			if (draw && !(lastOff && n == count - 1))
				_WritePixel(x, y, 0, true);
			if (n == count - 1)
				break;
			if (error >= 0) {
				if (yMajor)
					x += stepX;
				else
					y += stepY;
				error += diagonal;
			} else
				error += axial;
			if (yMajor)
				y += stepY;
			else
				x += stepX;
		}
		fEngine.errTerm = (u16)(error & 0x3fff);
	}
	fEngine.curX = (u16)(x & 0x0fff);
	fEngine.curY = (u16)(y & 0x0fff);
}


// Short stroke vector: length (bits 0-3), draw or move (bit 4) and a
// direction in 45 degree steps (bits 5-7).
void
S3Vision864::_ShortStroke(u8 vector)
{
	static const int kDx[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
	static const int kDy[8] = { 0, -1, -1, -1, 0, 1, 1, 1 };
	int length = vector & 0x0f;
	bool draw = (vector & 0x10) != 0 && (fEngine.cmd & 0x10) != 0;
	int direction = vector >> 5;
	int x = Coordinate(fEngine.curX);
	int y = Coordinate(fEngine.curY);
	for (int n = 0; n < length; n++) {
		if (draw)
			_WritePixel(x, y, 0, true);
		x += kDx[direction];
		y += kDy[direction];
	}
	fEngine.curX = (u16)(x & 0x0fff);
	fEngine.curY = (u16)(y & 0x0fff);
}


// Rectangle with its data from the CPU (WAIT YES): monochrome (the mix
// select takes CPU data: 1 bit per pixel) or pixels of the current depth.
// Each line starts with a new bus word.
void
S3Vision864::_StartTransfer(u16 cmd)
{
	Transfer& t = fTransfer;
	t.active = true;
	t.mono = ((fEngine.pixCntl >> 6) & 3) == 2;
	t.width = (fEngine.majAxis & 0x0fff) + 1;
	t.height = (fEngine.minAxis & 0x0fff) + 1;
	t.dx = (cmd & 0x20) != 0 ? 1 : -1;
	t.dy = (cmd & 0x80) != 0 ? 1 : -1;
	int command = cmd >> 13;
	t.left = Coordinate(command == 2 ? fEngine.curX : fEngine.destX);
	t.x = t.left;
	t.y = Coordinate(command == 2 ? fEngine.curY : fEngine.destY);
	t.column = 0;
	t.line = 0;
	switch ((cmd >> 9) & 3) {
		case 0: t.busBits = 8; break;
		case 1: t.busBits = 16; break;
		default: t.busBits = 32; break;
	}
	fTransferBytes = 0;
	fTransferCount = 0;
}


// One pixel of a CPU transfer; returns at the end of a line.
void
S3Vision864::_TransferPixel(u32 cpuPixel, bool cpuBit)
{
	Transfer& t = fTransfer;
	if ((fEngine.cmd & 0x10) != 0) {
		int mixSelect = (fEngine.pixCntl >> 6) & 3;
		bool foreground = mixSelect == 2 ? cpuBit : true;
		u16 mix = foreground ? fEngine.frgdMix : fEngine.bkgdMix;
		u32 source = ((mix >> 5) & 3) == 3 ? _ReadPixel(t.x, t.y) : cpuPixel;
		_WritePixel(t.x, t.y, source, foreground);
	}
	t.x += t.dx;
	if (++t.column == t.width) {
		t.column = 0;
		t.x = t.left;
		t.y += t.dy;
		if (++t.line == t.height) {
			t.active = false;
			int command = fEngine.cmd >> 13;
			if (command == 2)
				fEngine.curY = (u16)(t.y & 0x0fff);
			else
				fEngine.destY = (u16)(t.y & 0x0fff);
		}
	}
}


// PIX_TRANS data (little endian value as written to E2E8h). BYTE SWAP
// (command bit 12) takes the low byte of each word first.
void
S3Vision864::_PixTransWrite(u32 value, int size)
{
	Transfer& t = fTransfer;
	if (!t.active) {
		BB_LOG(kLogVideo, kLogDebug, "s3: PIX_TRANS %08x without transfer",
			value);
		return;
	}
	u8 bytes[4];
	bool lowFirst = (fEngine.cmd & 0x1000) != 0;
	for (int i = 0; i < size; i++) {
		int index = lowFirst || size == 1 ? i : (i ^ 1);
		bytes[i] = (u8)(value >> (8 * index));
	}
	int bpp = _BytesPerPixel();
	int lineAtStart = t.line;
	for (int i = 0; i < size && t.active && t.line == lineAtStart; i++) {
		u8 byte = bytes[i];
		if (t.mono) {
			for (int bit = 7; bit >= 0 && t.active
					&& t.line == lineAtStart; bit--)
				_TransferPixel(0, (byte >> bit) & 1);
			continue;
		}
		fTransferBytes |= (u32)byte << (8 * fTransferCount);
		if (++fTransferCount == bpp) {
			_TransferPixel(fTransferBytes, true);
			fTransferBytes = 0;
			fTransferCount = 0;
		}
	}
}

}	// namespace bebox

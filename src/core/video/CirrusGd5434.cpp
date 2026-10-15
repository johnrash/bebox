/*
 * BeBox emulator - Cirrus Logic CL-GD5434 PCI graphics card.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "CirrusGd5434.h"

#include "../Log.h"
#include "../bus/Mpc105.h"

#include <algorithm>

namespace bebox {

class CirrusGd5434::Ports : public ByteIoDevice {
public:
	Ports(CirrusGd5434& card) : fCard(card) {}
	virtual u8 IoRead8(u32 port) { return fCard.VgaRead(port); }
	virtual void IoWrite8(u32 port, u8 value) { fCard.VgaWrite(port, value); }
	virtual const char* IoName() const { return "vga"; }

private:
	CirrusGd5434& fCard;
};


// 0xa0000-0xbffff in PCI memory space.
class CirrusGd5434::LegacyWindow : public MmioDevice {
public:
	LegacyWindow(CirrusGd5434& card) : fCard(card) {}

	virtual u32 MmioRead(u32 addr, int size, int initiator)
	{
		u32 offset = addr - (Mpc105::kPciMemoryBase + 0xa0000);
		u32 value = 0;
		for (int i = 0; i < size; i++)
			value = (value << 8) | fCard.LegacyRead(offset + i);
		return value;
	}

	virtual void MmioWrite(u32 addr, u32 value, int size, int initiator)
	{
		u32 offset = addr - (Mpc105::kPciMemoryBase + 0xa0000);
		for (int i = 0; i < size; i++) {
			u8 byte = (u8)(value >> (8 * (size - 1 - i)));
			if (fCard.fBltActive)
				fCard._BltFeed(byte);
			else
				fCard.LegacyWrite(offset + i, byte);
		}
	}

	virtual const char* MmioName() const { return "vga-legacy"; }

private:
	CirrusGd5434& fCard;
};


// Linear aperture with byte swapping: byte address ^ 1 (16-bit) or ^ 3.
class CirrusGd5434::SwapAperture : public MmioDevice {
public:
	SwapAperture(CirrusGd5434& card, u32 swap) : fCard(card), fSwap(swap),
		fBase(0) {}

	void SetBase(u32 base) { fBase = base; }

	virtual u32 MmioRead(u32 addr, int size, int initiator)
	{
		u32 mask = fCard.VramSize() - 1;
		u32 offset = addr - fBase;
		u32 value = 0;
		for (int i = 0; i < size; i++)
			value = (value << 8) | fCard.Vram()[((offset + i) ^ fSwap) & mask];
		return value;
	}

	virtual void MmioWrite(u32 addr, u32 value, int size, int initiator)
	{
		u32 mask = fCard.VramSize() - 1;
		u32 offset = addr - fBase;
		for (int i = 0; i < size; i++) {
			fCard.Vram()[((offset + i) ^ fSwap) & mask]
				= (u8)(value >> (8 * (size - 1 - i)));
		}
	}

	virtual const char* MmioName() const { return "vga-swap"; }

private:
	CirrusGd5434& fCard;
	u32 fSwap;
	u32 fBase;
};


// The linear aperture while a BitBLT takes its source from system memory:
// writes anywhere in it are source data, in PCI byte lane order (after the
// aperture's byte swapping). Reads see video memory.
class CirrusGd5434::BltSink : public MmioDevice {
public:
	BltSink(CirrusGd5434& card) : fCard(card), fBase(0) {}

	void SetBase(u32 base) { fBase = base; }

	virtual u32 MmioRead(u32 addr, int size, int initiator)
	{
		u32 offset = addr - fBase;
		u32 swap = _Swap(offset);
		u32 mask = fCard.VramSize() - 1;
		u32 value = 0;
		for (int i = 0; i < size; i++)
			value = (value << 8) | fCard.Vram()[((offset + i) ^ swap) & mask];
		return value;
	}

	virtual void MmioWrite(u32 addr, u32 value, int size, int initiator)
	{
		u32 offset = addr - fBase;
		u32 swap = _Swap(offset);
		u8 lanes[4];
		bool used[4] = { false, false, false, false };
		for (int i = 0; i < size; i++) {
			int lane = ((offset + i) ^ swap) & 3;
			lanes[lane] = (u8)(value >> (8 * (size - 1 - i)));
			used[lane] = true;
		}
		for (int lane = 0; lane < 4; lane++) {
			if (used[lane] && fCard.fBltActive)
				fCard._BltFeed(lanes[lane]);
		}
	}

	virtual const char* MmioName() const { return "cirrus-blt"; }

private:
	static u32 _Swap(u32 offset)
	{
		switch (offset >> 22) {
			case 1: return 1;
			case 2: return 3;
		}
		return 0;
	}

	CirrusGd5434& fCard;
	u32 fBase;
};


CirrusGd5434::CirrusGd5434(PhysBus& bus, IoSpace& io, Scheduler& scheduler,
	u64 ticksPerSecond, u32 vramSize)
	:
	PciDevice(0x1013, 0x00a8, 0x030000, 0x00),
	VgaCore(scheduler, ticksPerSecond, vramSize),
	fBus(bus),
	fIo(io),
	fMappedBase(0),
	fLegacyMapped(false)
{
	DeclareBar(0, kBarMemory, kApertureSize, true);
	SetInterruptPin(1);
	fPorts = new Ports(*this);
	fLegacy = new LegacyWindow(*this);
	fSwap16 = new SwapAperture(*this, 1);
	fSwap32 = new SwapAperture(*this, 3);
	fBltSink = new BltSink(*this);
	fBltActive = false;
	io.Map(0x3b0, 0x30, fPorts);
	Reset();
}


CirrusGd5434::~CirrusGd5434()
{
	if (fMappedBase != 0)
		fBus.Unmap(fMappedBase, kApertureSize);
	if (fLegacyMapped)
		fBus.Unmap(Mpc105::kPciMemoryBase + 0xa0000, 0x20000);
	fIo.Unmap(0x3b0, 0x30);
	delete fPorts;
	delete fLegacy;
	delete fSwap16;
	delete fSwap32;
	delete fBltSink;
}


void
CirrusGd5434::Reset()
{
	if (fBltActive)
		_FinishBlt();
	ResetVga();
	fHiddenDac = 0;
	fCursorX = fCursorY = 0;
	fSeq[6] = 0x0f;		// extensions locked
	// DRAM control: 64-bit data bus; memory size for the driver's probe.
	u32 size = VramSize();
	fSeq[0x0f] = size >= (4u << 20) ? 0x98 : size >= (2u << 20) ? 0x18
		: 0x10;
	fSeq[0x0a] = size >= (2u << 20) ? 0x18 : 0x10;
	fSeq[0x17] = 0x00;
	fSeq[0x1f] = 0x22;
	fCrtc[0x27] = 0xa8;		// CL-GD5434
	fCrtc[0x25] = 0x28;
	fConfig[4] = 0;
	BarsChanged();
}


void
CirrusGd5434::BarsChanged()
{
	if (fMappedBase != 0) {
		fBus.Unmap(fMappedBase, kApertureSize);
		fMappedBase = 0;
	}
	bool memory = MemoryEnabled();
	if (memory && !fLegacyMapped) {
		fBus.MapDevice(Mpc105::kPciMemoryBase + 0xa0000, 0x20000, fLegacy);
		fLegacyMapped = true;
	} else if (!memory && fLegacyMapped) {
		fBus.Unmap(Mpc105::kPciMemoryBase + 0xa0000, 0x20000);
		fLegacyMapped = false;
	}
	u32 address = BarAddress(0);
	if (!memory || address == 0 || address >= 0x3f000000)
		return;
	u32 base = Mpc105::kPciMemoryBase + address;
	u32 aperture = 4u << 20;
	u32 vram = VramSize();
	for (u32 offset = 0; offset < aperture; offset += vram)
		fBus.MapMemory(base + offset, vram, Vram(), true);
	fSwap16->SetBase(base + aperture);
	fBus.MapDevice(base + aperture, aperture, fSwap16);
	fSwap32->SetBase(base + 2 * aperture);
	fBus.MapDevice(base + 2 * aperture, aperture, fSwap32);
	for (u32 offset = 0; offset < aperture; offset += vram)
		fBus.MapMemory(base + 3 * aperture + offset, vram, Vram(), true);
	fMappedBase = base;
	BB_LOG(kLogVideo, kLogDebug, "cirrus: linear aperture at %08x", base);
}


u8
CirrusGd5434::SequencerRead(u8 index)
{
	switch (index) {
		case 0x06:
			return _Unlocked() ? 0x12 : 0x0f;
		case 0x10: case 0x30: case 0x50: case 0x70: case 0x90: case 0xb0:
		case 0xd0: case 0xf0:
			return (u8)(fCursorX >> 3);
		case 0x11: case 0x31: case 0x51: case 0x71: case 0x91: case 0xb1:
		case 0xd1: case 0xf1:
			return (u8)(fCursorY >> 3);
	}
	if (index > 0x1f)
		return 0xff;
	return fSeq[index];
}


void
CirrusGd5434::SequencerWrite(u8 index, u8 value)
{
	BB_LOG(kLogVideo, kLogTrace, "cirrus: SR%02x = %02x", index, value);
	switch (index & 0x1f) {
		case 0x06:
			fSeq[6] = (value & 0x17) == 0x12 ? 0x12 : 0x0f;
			return;
		case 0x10:
			fCursorX = ((u32)value << 3) | (index >> 5);
			return;
		case 0x11:
			fCursorY = ((u32)value << 3) | (index >> 5);
			return;
	}
	if (index > 0x1f)
		return;
	if (index >= 0x07 && !_Unlocked()) {
		BB_LOG(kLogVideo, kLogDebug, "cirrus: SR%02x write while locked",
			index);
		return;
	}
	if (index == 0x12)
		fDacExtended = (value & 0x02) != 0;
	fSeq[index] = value;
	if (index == 0x07)
		BB_LOG(kLogVideo, kLogDebug, "cirrus: SR7 = %02x", value);
}


u8
CirrusGd5434::CrtcRead(u8 index)
{
	switch (index) {
		case 0x22:
			return fLatch[fGc[4] & 3];
		case 0x24:
			return fAtcFlipFlop ? 0x80 : 0x00;
		case 0x26:
			return fAtcIndex & 0x3f;
		case 0x25:
		case 0x27:
			return fCrtc[index];
	}
	if (index > 0x27)
		return 0xff;
	return fCrtc[index];
}


void
CirrusGd5434::CrtcWrite(u8 index, u8 value)
{
	if (index == 0x22 || index == 0x24 || index == 0x25 || index == 0x26
		|| index == 0x27)
		return;
	if (index > 0x1d)
		return;
	VgaCore::CrtcWrite(index, value);
}


u8
CirrusGd5434::GraphicsRead(u8 index)
{
	if (index == 0x31) {
		// busy (bit 0) and progress (bit 3) only while a system memory
		// source BLT waits for data
		return (fGc[0x31] & ~0x0b) | (fBltActive ? 0x09 : 0x00);
	}
	if (index > 0x3f)
		return 0xff;
	return fGc[index];
}


void
CirrusGd5434::GraphicsWrite(u8 index, u8 value)
{
	if (index > 0x3f)
		return;
	fGc[index] = value;
	if (index == 0x31) {
		if ((value & 0x04) != 0 && fBltActive) {
			BB_LOG(kLogVideo, kLogDebug, "cirrus: BLT reset");
			_FinishBlt();
		}
		if ((value & 0x02) != 0)
			_StartBlt();
	}
}


// #pragma mark - BitBLT engine


void
CirrusGd5434::_StartBlt()
{
	fGc[0x31] &= ~0x02;
	if (fBltActive)
		_FinishBlt();

	Blt blt;
	blt.width = (fGc[0x20] | ((fGc[0x21] & 0x1f) << 8)) + 1;
	blt.height = (fGc[0x22] | ((fGc[0x23] & 0x07) << 8)) + 1;
	blt.dstPitch = fGc[0x24] | ((fGc[0x25] & 0x1f) << 8);
	blt.srcPitch = fGc[0x26] | ((fGc[0x27] & 0x1f) << 8);
	blt.dst = fGc[0x28] | (fGc[0x29] << 8) | ((fGc[0x2a] & 0x3f) << 16);
	blt.src = fGc[0x2c] | (fGc[0x2d] << 8) | ((fGc[0x2e] & 0x3f) << 16);
	blt.mode = fGc[0x30];
	blt.rop = fGc[0x32];
	blt.bpp = ((blt.mode >> 4) & 3) + 1;
	blt.foreground = fGc[0x01] | (fGc[0x11] << 8) | (fGc[0x13] << 16)
		| ((u32)fGc[0x15] << 24);
	blt.background = fGc[0x00] | (fGc[0x10] << 8) | (fGc[0x12] << 16)
		| ((u32)fGc[0x14] << 24);
	blt.skipLeft = fGc[0x2f] & 7;
	BB_LOG(kLogVideo, kLogDebug, "cirrus: BLT mode %02x rop %02x %ux%u "
		"dst %06x/%u src %06x/%u", blt.mode, blt.rop, blt.width, blt.height,
		blt.dst, blt.dstPitch, blt.src, blt.srcPitch);

	if ((blt.mode & 0x02) != 0) {
		BB_LOG(kLogVideo, kLogInfo,
			"cirrus: BLT to system memory is not emulated");
		return;
	}
	if ((blt.mode & 0x04) != 0 && (blt.mode & 0x40) == 0) {
		// Source data come from the CPU through the aperture, line by
		// line: monochrome lines are byte padded (dword padded with
		// GR33 bit 0), color lines are 'width' bytes.
		u32 pixels = blt.width / blt.bpp;
		if ((blt.mode & 0x80) != 0) {
			fBltLineBytes = (fGc[0x33] & 0x01) != 0
				? (pixels + 31) / 32 * 4 : (pixels + 7) / 8;
		} else
			fBltLineBytes = blt.width;
		fBlt = blt;
		fBltLine.clear();
		fBltLinesDone = 0;
		fBltReceived = 0;
		fBltDraining = false;
		fBltActive = true;
		_RouteApertureToBlt(true);
		return;
	}
	if ((blt.mode & 0x40) != 0)
		_BltPattern(blt);
	else
		_BltVideoToVideo(blt);
}


void
CirrusGd5434::_FinishBlt()
{
	fBltActive = false;
	fBltLine.clear();
	_RouteApertureToBlt(false);
}


void
CirrusGd5434::_RouteApertureToBlt(bool toBlt)
{
	if (fMappedBase == 0)
		return;
	if (toBlt) {
		fBltSink->SetBase(fMappedBase);
		fBus.MapDevice(fMappedBase, kApertureSize, fBltSink);
	} else
		BarsChanged();
}


void
CirrusGd5434::_BltFeed(u8 byte)
{
	fBltReceived++;
	if (fBltDraining) {
		// the CPU pads the source data to a dword
		if ((fBltReceived & 3) == 0)
			_FinishBlt();
		return;
	}
	fBltLine.push_back(byte);
	if (fBltLine.size() < fBltLineBytes)
		return;
	u32 dst = (fBlt.dst + fBltLinesDone * fBlt.dstPitch) & (VramSize() - 1);
	if ((fBlt.mode & 0x80) != 0)
		_BltExpandLine(fBlt, dst, fBltLine.data());
	else
		_BltCopyLine(fBlt, dst, fBltLine.data());
	fBltLine.clear();
	if (++fBltLinesDone < fBlt.height)
		return;
	if ((fBltReceived & 3) == 0)
		_FinishBlt();
	else
		fBltDraining = true;
}


// Cirrus raster operation codes on one byte.
u8
CirrusGd5434::_Rop(u8 src, u8 dst) const
{
	switch (fBlt.rop) {
		case 0x00: return 0x00;
		case 0x05: return src & dst;
		case 0x06: return dst;
		case 0x09: return src & ~dst;
		case 0x0b: return ~dst;
		case 0x0d: return src;
		case 0x0e: return 0xff;
		case 0x50: return ~src & dst;
		case 0x59: return src ^ dst;
		case 0x6d: return src | dst;
		case 0x90: return ~src | ~dst;
		case 0x95: return ~(src ^ dst);
		case 0xad: return src | ~dst;
		case 0xd0: return ~src;
		case 0xd6: return ~src | dst;
		case 0xda: return ~src & ~dst;
	}
	return src;
}


// One line of color expansion: bit i (MSB first) selects the foreground
// or background color of pixel i; with transparency (mode bit 3) clear
// bits leave the destination alone. The first skipLeft pixels are clipped.
void
CirrusGd5434::_BltExpandLine(const Blt& blt, u32 dst, const u8* bits)
{
	u32 mask = VramSize() - 1;
	u8* vram = Vram();
	u32 pixels = blt.width / blt.bpp;
	bool transparent = (blt.mode & 0x08) != 0;
	u8 invert = (fGc[0x33] & 0x02) != 0 ? 0xff : 0x00;
	for (u32 i = blt.skipLeft; i < pixels; i++) {
		bool set = ((bits[i / 8] ^ invert) >> (7 - (i & 7))) & 1;
		if (!set && transparent)
			continue;
		u32 color = set ? blt.foreground : blt.background;
		u32 d = dst + i * blt.bpp;
		for (int k = 0; k < blt.bpp; k++) {
			u8& pixel = vram[(d + k) & mask];
			pixel = _Rop((u8)(color >> (8 * k)), pixel);
		}
	}
}


void
CirrusGd5434::_BltCopyLine(const Blt& blt, u32 dst, const u8* source)
{
	u32 mask = VramSize() - 1;
	u8* vram = Vram();
	for (u32 x = 0; x < blt.width; x++) {
		u8& pixel = vram[(dst + x) & mask];
		pixel = _Rop(source[x], pixel);
	}
}


void
CirrusGd5434::_BltVideoToVideo(const Blt& blt)
{
	fBlt = blt;
	u32 mask = VramSize() - 1;
	u8* vram = Vram();
	if ((blt.mode & 0x80) != 0) {
		// monochrome source packed in video memory, each line starting
		// on a byte
		u32 lineBytes = (blt.width / blt.bpp + 7) / 8;
		std::vector<u8> bits(lineBytes);
		u32 src = blt.src;
		for (u32 y = 0; y < blt.height; y++) {
			for (u32 i = 0; i < lineBytes; i++)
				bits[i] = vram[(src + i) & mask];
			src += lineBytes;
			_BltExpandLine(blt, (blt.dst + y * blt.dstPitch) & mask,
				bits.data());
		}
		return;
	}
	bool backwards = (blt.mode & 0x01) != 0;
	for (u32 y = 0; y < blt.height; y++) {
		u32 dst = backwards ? blt.dst - y * blt.dstPitch
			: blt.dst + y * blt.dstPitch;
		u32 src = backwards ? blt.src - y * blt.srcPitch
			: blt.src + y * blt.srcPitch;
		for (u32 x = 0; x < blt.width; x++) {
			u32 d = backwards ? dst - x : dst + x;
			u32 s = backwards ? src - x : src + x;
			u8& pixel = vram[d & mask];
			pixel = _Rop(vram[s & mask], pixel);
		}
	}
}


// 8x8 patterns: monochrome (8 bytes, with color expansion) or in color
// (8 pixels per row). The low 3 bits of the source address select the
// first pattern row, GR2F the first pattern column.
void
CirrusGd5434::_BltPattern(const Blt& blt)
{
	fBlt = blt;
	u32 mask = VramSize() - 1;
	u8* vram = Vram();
	bool expand = (blt.mode & 0x80) != 0;
	bool transparent = (blt.mode & 0x08) != 0;
	u32 rowBytes = expand ? 1 : 8 * (blt.bpp == 3 ? 4 : blt.bpp);
	u32 base = blt.src & ~(expand ? 7u : rowBytes * 8 - 1);
	u32 row = blt.src & 7;
	u32 pixels = blt.width / blt.bpp;
	u8 invert = (fGc[0x33] & 0x02) != 0 ? 0xff : 0x00;
	for (u32 y = 0; y < blt.height; y++) {
		u32 dst = blt.dst + y * blt.dstPitch;
		u32 line = base + ((row + y) & 7) * rowBytes;
		for (u32 i = blt.skipLeft; i < pixels; i++) {
			u32 column = i & 7;
			u32 d = dst + i * blt.bpp;
			if (expand) {
				bool set = ((vram[line & mask] ^ invert) >> (7 - column)) & 1;
				if (!set && transparent)
					continue;
				u32 color = set ? blt.foreground : blt.background;
				for (int k = 0; k < blt.bpp; k++) {
					u8& pixel = vram[(d + k) & mask];
					pixel = _Rop((u8)(color >> (8 * k)), pixel);
				}
			} else {
				u32 p = line + column * (blt.bpp == 3 ? 4 : blt.bpp);
				for (int k = 0; k < blt.bpp; k++) {
					u8& pixel = vram[(d + k) & mask];
					pixel = _Rop(vram[(p + k) & mask], pixel);
				}
			}
		}
	}
}


u8
CirrusGd5434::DacMaskRead()
{
	if (fHiddenDacCounter >= 4) {
		fHiddenDacCounter = 0;
		return fHiddenDac;
	}
	fHiddenDacCounter++;
	return fDacMask;
}


void
CirrusGd5434::DacMaskWrite(u8 value)
{
	if (fHiddenDacCounter >= 4) {
		fHiddenDac = value;
		fHiddenDacCounter = 0;
		BB_LOG(kLogVideo, kLogDebug, "cirrus: hidden DAC = %02x", value);
		return;
	}
	fHiddenDacCounter = 0;
	fDacMask = value;
}


u32
CirrusGd5434::LegacyBankOffset(u32 offset)
{
	if ((fSeq[7] & 1) == 0 && (fGc[0x0b] & 1) == 0 && fGc[9] == 0)
		return 0;
	u32 granularity = (fGc[0x0b] & 0x20) != 0 ? 16384 : 4096;
	u8 bank = fGc[9];
	if ((fGc[0x0b] & 1) != 0 && offset >= 0x8000)
		bank = fGc[0x0a];
	u32 base = (u32)bank * granularity;
	// The bank register replaces the upper address bits of the window.
	if ((fGc[0x0b] & 1) != 0)
		return base - (offset >= 0x8000 ? 0x8000 : 0);
	return base;
}


void
CirrusGd5434::ComputeMode(DisplayMode& mode)
{
	if ((fSeq[7] & 1) == 0) {
		VgaCore::ComputeMode(mode);
		return;
	}
	mode.width = _StandardWidth();
	mode.lineRepeat = _StandardLineRepeat();
	mode.height = _StandardHeight() / mode.lineRepeat;
	u32 start = (fCrtc[0x0c] << 8) | fCrtc[0x0d] | ((fCrtc[0x1b] & 1) << 16)
		| ((fCrtc[0x1b] & 0x0c) << 15) | ((fCrtc[0x1d] & 0x80) << 12);
	mode.start = start * 4;
	mode.pitch = (fCrtc[0x13] | ((fCrtc[0x1b] & 0x10) << 4)) * 8;
	mode.pixelDoubling = 1;
	switch ((fSeq[7] >> 1) & 7) {
		case 0:
			mode.format = kIndexed8;
			break;
		case 1:
		case 3:
			if ((fHiddenDac & 0xc0) == 0xc0 && (fHiddenDac & 0x0f) == 1)
				mode.format = kRgb565;
			else
				mode.format = kRgb555;
			break;
		case 2:
			mode.format = kRgb888;
			break;
		case 4:
			// The offset counts 16 byte units in 32 bpp: BeOS programs
			// CR13 = a0h for 640 pixels (2560 bytes; its BitBLT pitch).
			mode.format = kRgbx8888;
			mode.pitch *= 2;
			break;
		default:
			mode.format = kIndexed8;
			break;
	}
}


bool
CirrusGd5434::HardwareCursor(int& x, int& y) const
{
	if ((fSeq[0x12] & 1) == 0)
		return false;
	x = (int)fCursorX;
	y = (int)fCursorY;
	return true;
}


void
CirrusGd5434::DrawOverlays(u32* frame, int width, int height, int stride)
{
	if ((fSeq[0x12] & 1) == 0)
		return;
	bool large = (fSeq[0x12] & 0x04) != 0;
	int size = large ? 64 : 32;
	// The patterns are in the last 16 KB of the memory in use: 4 MB only
	// with DRAM bank switching on (SR0F bit 7), which BeOS's driver turns
	// off (it puts the cursor below 2 MB).
	u32 vram = VramSize();
	if (vram > (2u << 20) && (fSeq[0x0f] & 0x80) == 0)
		vram = 2u << 20;
	u32 pattern = vram - 16384;
	if (large)
		pattern += ((fSeq[0x13] >> 2) & 0x0f) * 1024;
	else
		pattern += (fSeq[0x13] & 0x3f) * 256;
	u32 planeSize = size * size / 8;
	auto extColor = [this](int index) {
		const u8* e = &fExtPalette[index * 3];
		u32 r = (e[0] << 2) | (e[0] >> 4);
		u32 g = (e[1] << 2) | (e[1] >> 4);
		u32 b = (e[2] << 2) | (e[2] >> 4);
		return (r << 16) | (g << 8) | b;
	};
	u32 background = extColor(0);
	u32 foreground = extColor(15);
	const u8* data = Vram();
	for (int y = 0; y < size; y++) {
		int screenY = (int)fCursorY + y;
		if (screenY < 0 || screenY >= height)
			continue;
		for (int x = 0; x < size; x++) {
			int screenX = (int)fCursorX + x;
			if (screenX < 0 || screenX >= width)
				continue;
			u32 bitIndex = y * size + x;
			u32 byte = bitIndex / 8;
			int bit = 7 - (bitIndex & 7);
			bool p0 = (data[(pattern + byte) % vram] >> bit) & 1;
			bool p1 = (data[(pattern + planeSize + byte) % vram] >> bit) & 1;
			u32& pixel = frame[(size_t)screenY * stride + screenX];
			if (!p0 && p1)
				pixel = background;
			else if (p0 && !p1)
				pixel ^= 0x00ffffff;
			else if (p0 && p1)
				pixel = foreground;
		}
	}
}

}	// namespace bebox

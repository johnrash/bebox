/*
 * BeBox emulator - VGA core shared by the emulated graphics cards.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "VgaCore.h"

#include "../Log.h"

#include <algorithm>

namespace bebox {

VgaCore::VgaCore(Scheduler& scheduler, u64 ticksPerSecond, u32 vramSize)
	:
	fScheduler(scheduler),
	fTicksPerSecond(ticksPerSecond),
	fSink(nullptr),
	fFrameStart(0)
{
	fVram.assign(vramSize, 0);
	fFrameTicks = ticksPerSecond / 60;
	fRetraceTimer.reset(new Timer(scheduler, [this]() { _Retrace(); },
		"vga-retrace"));
	ResetVga();
}


VgaCore::~VgaCore()
{
}


void
VgaCore::ResetVga()
{
	fMisc = 0x01;		// color emulation (CRTC at 0x3d4)
	fFeature = 0;
	fSeqIndex = 0;
	memset(fSeq, 0, sizeof(fSeq));
	fSeq[0] = 0x03;
	fCrtcIndex = 0;
	memset(fCrtc, 0, sizeof(fCrtc));
	fGcIndex = 0;
	memset(fGc, 0, sizeof(fGc));
	fGc[8] = 0xff;
	fAtcIndex = 0;
	fAtcFlipFlop = false;
	memset(fAtc, 0, sizeof(fAtc));
	fDacMask = 0xff;
	fDacReadIndex = fDacWriteIndex = 0;
	fDacComponent = 0;
	fDacReading = false;
	memset(fPalette, 0, sizeof(fPalette));
	memset(fExtPalette, 0, sizeof(fExtPalette));
	fDacExtended = false;
	fDac8Bit = false;
	fHiddenDacCounter = 0;
	memset(fLatch, 0, sizeof(fLatch));
	fFrameStart = fScheduler.Now();
	fRetraceTimer->Arm(fFrameStart + fFrameTicks);
}


void
VgaCore::_Retrace()
{
	fFrameStart = fScheduler.Now();
	fRetraceTimer->Arm(fFrameStart + fFrameTicks);
	if (fSink != nullptr)
		RenderFrame();
}


// #pragma mark - registers


u8
VgaCore::SequencerRead(u8 index)
{
	return fSeq[index];
}


void
VgaCore::SequencerWrite(u8 index, u8 value)
{
	fSeq[index] = value;
}


u8
VgaCore::CrtcRead(u8 index)
{
	return fCrtc[index];
}


void
VgaCore::CrtcWrite(u8 index, u8 value)
{
	// CR11 bit 7 protects registers 0-7 (except the line compare bit 4 of
	// CR07).
	if ((fCrtc[0x11] & 0x80) != 0 && index <= 7) {
		if (index == 7)
			fCrtc[7] = (fCrtc[7] & ~0x10) | (value & 0x10);
		return;
	}
	fCrtc[index] = value;
}


u8
VgaCore::GraphicsRead(u8 index)
{
	return fGc[index];
}


void
VgaCore::GraphicsWrite(u8 index, u8 value)
{
	fGc[index] = value;
}


u8
VgaCore::VgaRead(u32 port)
{
	bool color = (fMisc & 1) != 0;
	if ((port >= 0x3b0 && port <= 0x3bf && color)
		|| (port >= 0x3d0 && port <= 0x3df && !color))
		return 0xff;

	switch (port) {
		case 0x3c0:
			return fAtcIndex;
		case 0x3c1:
			return fAtc[fAtcIndex & 0x1f];
		case 0x3c2:
			return 0x10;		// input status 0: switch sense
		case 0x3c3:
			return 0x01;
		case 0x3c4:
			return fSeqIndex;
		case 0x3c5:
			return SequencerRead(fSeqIndex);
		case 0x3c6:
			return DacMaskRead();
		case 0x3c7:
			DacAccessed();
			return fDacReading ? 0x03 : 0x00;
		case 0x3c8:
			DacAccessed();
			return fDacWriteIndex;
		case 0x3c9:
		{
			DacAccessed();
			u8 value = fDacExtended
				? fExtPalette[(fDacReadIndex & 15) * 3 + fDacComponent]
				: fPalette[fDacReadIndex * 3 + fDacComponent]
					& (fDac8Bit ? 0xff : 0x3f);
			if (++fDacComponent == 3) {
				fDacComponent = 0;
				fDacReadIndex++;
			}
			return value;
		}
		case 0x3ca:
			return fFeature;
		case 0x3cc:
			return fMisc;
		case 0x3ce:
			return fGcIndex;
		case 0x3cf:
			return GraphicsRead(fGcIndex);
		case 0x3b4: case 0x3d4:
			return fCrtcIndex;
		case 0x3b5: case 0x3d5:
			return CrtcRead(fCrtcIndex);
		case 0x3ba: case 0x3da:
		{
			fAtcFlipFlop = false;
			u64 phase = (fScheduler.Now() - fFrameStart) % fFrameTicks;
			u64 lineTicks = fFrameTicks / 525;
			bool vretrace = phase < fFrameTicks / 40;
			bool hblank = (phase % lineTicks) >= lineTicks * 4 / 5;
			u8 status = 0;
			if (vretrace)
				status |= 0x08;
			if (vretrace || hblank)
				status |= 0x01;
			return status;
		}
	}
	return 0xff;
}


void
VgaCore::VgaWrite(u32 port, u8 value)
{
	bool color = (fMisc & 1) != 0;
	if ((port >= 0x3b0 && port <= 0x3bf && color)
		|| (port >= 0x3d0 && port <= 0x3df && !color))
		return;

	switch (port) {
		case 0x3c0:
			if (!fAtcFlipFlop) {
				fAtcIndex = value & 0x3f;
			} else {
				u8 index = fAtcIndex & 0x1f;
				if (index < 0x15)
					fAtc[index] = value;
			}
			fAtcFlipFlop = !fAtcFlipFlop;
			break;
		case 0x3c2:
			fMisc = value;
			break;
		case 0x3c4:
			fSeqIndex = value;
			break;
		case 0x3c5:
			SequencerWrite(fSeqIndex, value);
			break;
		case 0x3c6:
			DacMaskWrite(value);
			break;
		case 0x3c7:
			DacAccessed();
			fDacReadIndex = value;
			fDacComponent = 0;
			fDacReading = true;
			break;
		case 0x3c8:
			DacAccessed();
			fDacWriteIndex = value;
			fDacComponent = 0;
			fDacReading = false;
			break;
		case 0x3c9:
			DacAccessed();
			BB_LOG(kLogVideo, kLogTrace, "dac %02x.%d = %02x", fDacWriteIndex,
				fDacComponent, value);
			if (fDacExtended)
				fExtPalette[(fDacWriteIndex & 15) * 3 + fDacComponent]
					= value & 0x3f;
			else {
				fPalette[fDacWriteIndex * 3 + fDacComponent]
					= fDac8Bit ? value : value & 0x3f;
			}
			if (++fDacComponent == 3) {
				fDacComponent = 0;
				fDacWriteIndex++;
			}
			break;
		case 0x3ce:
			fGcIndex = value;
			break;
		case 0x3cf:
			GraphicsWrite(fGcIndex, value);
			break;
		case 0x3b4: case 0x3d4:
			fCrtcIndex = value;
			break;
		case 0x3b5: case 0x3d5:
			BB_LOG(kLogVideo, kLogTrace, "CR%02x = %02x", fCrtcIndex, value);
			CrtcWrite(fCrtcIndex, value);
			break;
		case 0x3ba: case 0x3da:
			fFeature = value;
			break;
	}
}


// #pragma mark - legacy memory window


u8
VgaCore::LegacyRead(u32 offset)
{
	u32 window = (fGc[6] >> 2) & 3;
	static const u32 kBase[4] = { 0, 0, 0x10000, 0x18000 };
	static const u32 kSize[4] = { 0x20000, 0x10000, 0x8000, 0x8000 };
	if (offset < kBase[window] || offset >= kBase[window] + kSize[window])
		return 0xff;
	offset -= kBase[window];
	u32 bank = LegacyBankOffset(offset);
	u32 mask = (u32)fVram.size() - 1;

	if ((fSeq[4] & 0x08) != 0) {
		// chain 4
		return fVram[(offset + bank) & mask];
	}
	if ((fGc[5] & 0x10) != 0) {
		// odd/even
		u32 plane = (fGc[4] & 2) | (offset & 1);
		u32 address = (((offset & ~1u) << 1) | plane) + bank * 4;
		return fVram[address & mask];
	}
	u32 address = (offset * 4 + bank * 4) & mask & ~3u;
	for (int i = 0; i < 4; i++)
		fLatch[i] = fVram[address + i];
	if ((fGc[5] & 0x08) == 0)
		return fLatch[fGc[4] & 3];
	// read mode 1: color compare
	u8 result = 0;
	for (int bit = 0; bit < 8; bit++) {
		bool match = true;
		for (int plane = 0; plane < 4; plane++) {
			if ((fGc[7] & (1 << plane)) == 0)
				continue;
			bool pixel = (fLatch[plane] >> bit) & 1;
			bool compare = (fGc[2] >> plane) & 1;
			if (pixel != compare)
				match = false;
		}
		if (match)
			result |= 1 << bit;
	}
	return result;
}


void
VgaCore::LegacyWrite(u32 offset, u8 value)
{
	u32 window = (fGc[6] >> 2) & 3;
	static const u32 kBase[4] = { 0, 0, 0x10000, 0x18000 };
	static const u32 kSize[4] = { 0x20000, 0x10000, 0x8000, 0x8000 };
	if (offset < kBase[window] || offset >= kBase[window] + kSize[window])
		return;
	offset -= kBase[window];
	u32 bank = LegacyBankOffset(offset);
	u32 mask = (u32)fVram.size() - 1;
	u8 mapMask = fSeq[2];

	if ((fSeq[4] & 0x08) != 0) {
		u32 address = (offset + bank) & mask;
		if ((mapMask & (1 << (address & 3))) != 0)
			fVram[address] = value;
		return;
	}
	if ((fGc[5] & 0x10) != 0) {
		u32 plane = offset & 1;
		for (u32 p = plane; p < 4; p += 2) {
			if ((mapMask & (1 << p)) == 0)
				continue;
			u32 address = (((offset & ~1u) << 1) | p) + bank * 4;
			fVram[address & mask] = value;
		}
		return;
	}

	int writeMode = fGc[5] & 3;
	u8 rotate = fGc[3] & 7;
	u8 rotated = (u8)((value >> rotate) | (value << (8 - rotate)));
	u8 bitMask = fGc[8];
	u8 planes[4];
	switch (writeMode) {
		case 0:
			for (int p = 0; p < 4; p++) {
				planes[p] = (fGc[1] & (1 << p)) != 0
					? ((fGc[0] & (1 << p)) != 0 ? 0xff : 0x00) : rotated;
			}
			break;
		case 1:
			for (int p = 0; p < 4; p++)
				planes[p] = fLatch[p];
			break;
		case 2:
			for (int p = 0; p < 4; p++)
				planes[p] = (value & (1 << p)) != 0 ? 0xff : 0x00;
			break;
		default:
			bitMask &= rotated;
			for (int p = 0; p < 4; p++)
				planes[p] = (fGc[0] & (1 << p)) != 0 ? 0xff : 0x00;
			break;
	}
	if (writeMode != 1) {
		int function = (fGc[3] >> 3) & 3;
		for (int p = 0; p < 4; p++) {
			u8 v = planes[p];
			switch (function) {
				case 1: v &= fLatch[p]; break;
				case 2: v |= fLatch[p]; break;
				case 3: v ^= fLatch[p]; break;
			}
			planes[p] = (v & bitMask) | (fLatch[p] & ~bitMask);
		}
	}
	u32 address = (offset * 4 + bank * 4) & mask & ~3u;
	for (int p = 0; p < 4; p++) {
		if ((mapMask & (1 << p)) != 0)
			fVram[address + p] = planes[p];
	}
}


// #pragma mark - display


u32
VgaCore::PaletteColor(int index) const
{
	const u8* entry = &fPalette[(index & 0xff) * 3];
	if (fDac8Bit)
		return (entry[0] << 16) | (entry[1] << 8) | entry[2];
	u32 r = (entry[0] << 2) | (entry[0] >> 4);
	u32 g = (entry[1] << 2) | (entry[1] >> 4);
	u32 b = (entry[2] << 2) | (entry[2] >> 4);
	return (r << 16) | (g << 8) | b;
}


int
VgaCore::_StandardWidth() const
{
	return (fCrtc[1] + 1) * 8;
}


int
VgaCore::_StandardHeight() const
{
	int lines = fCrtc[0x12] | ((fCrtc[7] & 0x02) << 7)
		| ((fCrtc[7] & 0x40) << 3);
	return lines + 1;
}


u32
VgaCore::_StandardStart() const
{
	return (fCrtc[0x0c] << 8) | fCrtc[0x0d];
}


int
VgaCore::_StandardLineRepeat() const
{
	int repeat = (fCrtc[9] & 0x1f) + 1;
	if ((fCrtc[9] & 0x80) != 0)
		repeat *= 2;
	return repeat;
}


void
VgaCore::ComputeMode(DisplayMode& mode)
{
	mode.height = _StandardHeight();
	mode.width = _StandardWidth();
	if ((fGc[6] & 1) == 0) {
		mode.format = kText;
		mode.start = _StandardStart();
		mode.pitch = fCrtc[0x13] * 2;
		mode.lineRepeat = 1;
		return;
	}
	int repeat = _StandardLineRepeat();
	mode.lineRepeat = repeat;
	mode.height /= repeat;
	int unit = (fCrtc[0x14] & 0x40) != 0 ? 4 : (fCrtc[0x17] & 0x40) != 0
		? 1 : 2;
	if ((fGc[5] & 0x40) != 0) {
		mode.format = kIndexed8;
		mode.pixelDoubling = 2;
		mode.width /= 2;
		mode.start = _StandardStart() * unit;
		mode.pitch = fCrtc[0x13] * 2 * unit;
	} else {
		mode.format = kPlanar4;
		mode.start = _StandardStart();
		mode.pitch = fCrtc[0x13] * 2 * (unit == 4 ? 4 : unit == 2 ? 2 : 1);
	}
}


void
VgaCore::RenderFrame()
{
	DisplayMode mode;
	ComputeMode(mode);
	fMode = mode;
	bool blanked = (fSeq[1] & 0x20) != 0 || (fAtcIndex & 0x20) == 0;
	int width = std::min(std::max(mode.width, 0), 2048);
	int height = std::min(std::max(mode.height, 0), 1536);
	if (width < 8 || height < 8 || blanked) {
		if (fSink != nullptr) {
			// keep the last size, show black
			int w = std::max(width, 640);
			int h = std::max(height, 480);
			fFrame.assign((size_t)w * h, 0);
			fSink->FrameReady(fFrame.data(), w, h, w);
		}
		return;
	}
	fFrame.resize((size_t)width * height);
	u32* out = fFrame.data();
	const u8* vram = fVram.data();
	u32 mask = (u32)fVram.size() - 1;

	switch (mode.format) {
		case kIndexed8:
		{
			u32 palette[256];
			for (int i = 0; i < 256; i++)
				palette[i] = PaletteColor(i & fDacMask);
			for (int y = 0; y < height; y++) {
				u32 line = mode.start + (u32)y * mode.pitch;
				u32* row = out + (size_t)y * width;
				for (int x = 0; x < width; x++)
					row[x] = palette[vram[(line + x) & mask]];
			}
			break;
		}
		case kRgb555:
		case kRgb565:
			for (int y = 0; y < height; y++) {
				u32 line = mode.start + (u32)y * mode.pitch;
				u32* row = out + (size_t)y * width;
				for (int x = 0; x < width; x++) {
					u32 a = (line + x * 2) & mask;
					u16 p = (u16)(vram[a] | (vram[(a + 1) & mask] << 8));
					u32 r, g, b;
					if (mode.format == kRgb565) {
						r = (p >> 11) & 0x1f;
						g = (p >> 5) & 0x3f;
						b = p & 0x1f;
						g = (g << 2) | (g >> 4);
					} else {
						r = (p >> 10) & 0x1f;
						g = (p >> 5) & 0x1f;
						b = p & 0x1f;
						g = (g << 3) | (g >> 2);
					}
					r = (r << 3) | (r >> 2);
					b = (b << 3) | (b >> 2);
					row[x] = (r << 16) | (g << 8) | b;
				}
			}
			break;
		case kRgb888:
			for (int y = 0; y < height; y++) {
				u32 line = mode.start + (u32)y * mode.pitch;
				u32* row = out + (size_t)y * width;
				for (int x = 0; x < width; x++) {
					u32 a = line + x * 3;
					row[x] = (vram[(a + 2) & mask] << 16)
						| (vram[(a + 1) & mask] << 8) | vram[a & mask];
				}
			}
			break;
		case kRgbx8888:
			for (int y = 0; y < height; y++) {
				u32 line = mode.start + (u32)y * mode.pitch;
				u32* row = out + (size_t)y * width;
				for (int x = 0; x < width; x++) {
					u32 a = (line + x * 4) & mask;
					row[x] = (vram[(a + 2) & mask] << 16)
						| (vram[(a + 1) & mask] << 8) | vram[a];
				}
			}
			break;
		case kPlanar4:
		{
			for (int y = 0; y < height; y++) {
				u32 line = mode.start + (u32)y * mode.pitch;
				u32* row = out + (size_t)y * width;
				for (int x = 0; x < width; x++) {
					u32 address = ((line + x / 8) * 4) & mask;
					int bit = 7 - (x & 7);
					int index = 0;
					for (int p = 0; p < 4; p++)
						index |= ((vram[address + p] >> bit) & 1) << p;
					index &= fAtc[0x12] & 0x0f;
					u8 attribute = fAtc[index] & 0x3f;
					if ((fAtc[0x10] & 0x80) != 0)
						attribute = (attribute & 0x0f) | ((fAtc[0x14] & 0x0f)
							<< 4);
					else
						attribute |= (fAtc[0x14] & 0x0c) << 4;
					row[x] = PaletteColor(attribute & fDacMask);
				}
			}
			break;
		}
		case kText:
		{
			int charHeight = (fCrtc[9] & 0x1f) + 1;
			int charWidth = (fSeq[1] & 1) != 0 ? 8 : 9;
			int columns = fCrtc[1] + 1;
			width = columns * charWidth;
			height = std::min(height, 1536);
			fFrame.assign((size_t)width * height, 0);
			out = fFrame.data();
			int rows = height / charHeight;
			u8 mapSelect = fSeq[3];
			u32 fontA = (((mapSelect >> 2) & 3) | ((mapSelect >> 3) & 4))
				* 0x4000;
			u32 fontB = ((mapSelect & 3) | ((mapSelect >> 2) & 4)) * 0x4000;
			int cursorStart = fCrtc[0x0a] & 0x1f;
			int cursorEnd = fCrtc[0x0b] & 0x1f;
			bool cursorOn = (fCrtc[0x0a] & 0x20) == 0;
			u32 cursorAddress = (fCrtc[0x0e] << 8) | fCrtc[0x0f];
			bool blinkPhase = ((fScheduler.Now() / (fTicksPerSecond / 4))
				& 1) != 0;
			for (int row = 0; row < rows; row++) {
				for (int column = 0; column < columns; column++) {
					u32 cell = mode.start + row * (mode.pitch / 2) + column;
					u8 character = vram[(cell * 4) & mask];
					u8 attribute = vram[(cell * 4 + 1) & mask];
					u32 font = (attribute & 0x08) != 0 ? fontA : fontB;
					u8 fg = attribute & 0x0f;
					u8 bg = attribute >> 4;
					if ((fAtc[0x10] & 0x08) != 0) {
						bg &= 7;
						if ((attribute & 0x80) != 0 && blinkPhase)
							fg = bg;
					}
					u32 fgColor = PaletteColor(fAtc[fg] & 0x3f);
					u32 bgColor = PaletteColor(fAtc[bg] & 0x3f);
					for (int line = 0; line < charHeight; line++) {
						u8 bits = vram[((font + character * 32 + line) * 4
							+ 2) & mask];
						bool cursorLine = cursorOn && cell == cursorAddress
							&& line >= cursorStart && line <= cursorEnd
							&& blinkPhase;
						u32* pixel = out + (size_t)(row * charHeight + line)
							* width + column * charWidth;
						for (int bit = 0; bit < charWidth; bit++) {
							bool on;
							if (bit < 8)
								on = (bits & (0x80 >> bit)) != 0;
							else
								on = character >= 0xc0 && character <= 0xdf
									&& (bits & 1) != 0;
							pixel[bit] = (on || cursorLine) ? fgColor
								: bgColor;
						}
					}
				}
			}
			break;
		}
	}

	if (mode.pixelDoubling > 1 && mode.format == kIndexed8) {
		// stretch 320 wide modes horizontally for display
		std::vector<u32> doubled((size_t)width * 2 * height);
		for (int y = 0; y < height; y++) {
			for (int x = 0; x < width; x++) {
				u32 c = out[(size_t)y * width + x];
				doubled[(size_t)y * width * 2 + 2 * x] = c;
				doubled[(size_t)y * width * 2 + 2 * x + 1] = c;
			}
		}
		fFrame.swap(doubled);
		width *= 2;
		out = fFrame.data();
	}

	DrawOverlays(out, width, height, width);
	if (fSink != nullptr)
		fSink->FrameReady(out, width, height, width);
}

}	// namespace bebox

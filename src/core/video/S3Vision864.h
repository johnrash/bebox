/*
 * BeBox emulator - S3 Vision864 PCI graphics card (Number Nine #9 GXE64).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Supported by the BeBox boot ROM (PCI 5333:88c0) and by BeOS DR8 and
 * later (accelerated). VGA core plus the S3 extensions: register locks
 * (CR38, CR39), the enhanced display pipeline (4AE8h, CR3A, CR50), linear
 * addressing (CR58-CR5A, PCI BAR 0), banking (CR31, CR35, CR51, CR6A), the
 * hardware cursor (CR45-CR4F) and the 8514/A style graphics engine
 * (rectangle fill, BitBLT, pattern fill, lines, short stroke vectors and
 * image transfers) at the x2E8h I/O ports or memory mapped at A8000h.
 *
 * The board's AT&T 21C498 RAMDAC has a 16-bit pixel port; its command
 * register (reached through RS2 or by reading 3C6h four times) selects the
 * pixel format. The ICD2061A clock synthesizer is programmed serially
 * through the clock select bits; the emulation only accepts that.
 */
#ifndef BEBOX_S3_VISION864_H
#define BEBOX_S3_VISION864_H

#include "VgaCore.h"
#include "../bus/IoSpace.h"
#include "../bus/PciBus.h"
#include "../bus/PhysBus.h"

namespace bebox {

class S3Vision864 : public PciDevice, public VgaCore {
public:
	static const u32 kBarSize = 8 << 20;

								S3Vision864(PhysBus& bus, IoSpace& io,
									Scheduler& scheduler,
									u64 ticksPerSecond, u32 vramSize);
								~S3Vision864();

			void				Reset();

	virtual	bool				HardwareCursor(int& x, int& y) const;

	// PciDevice
	virtual	const char*			PciName() const { return "s3-vision864"; }

protected:
	virtual	void				BarsChanged();

	// VgaCore hooks
	virtual	void				ComputeMode(DisplayMode& mode);
	virtual	u8					CrtcRead(u8 index);
	virtual	void				CrtcWrite(u8 index, u8 value);
	virtual	u8					DacMaskRead();
	virtual	void				DacMaskWrite(u8 value);
	virtual	void				DacAccessed() { fDacReads = 0; }
	virtual	u32					LegacyBankOffset(u32 offset);
	virtual	void				DrawOverlays(u32* frame, int width,
									int height, int stride);

private:
	class Ports;
	class EnginePorts;
	class LegacyWindow;

	// Graphics engine state (8514/A registers)
	struct Engine {
		u16 curX, curY;
		u16 destX, destY;		// DESTX_DIASTP, DESTY_AXSTP
		u16 errTerm;
		u16 majAxis;			// MAJ_AXIS_PCNT
		u16 minAxis;			// MIN_AXIS_PCNT (BEE8h index 0)
		u16 scissorTop, scissorLeft, scissorBottom, scissorRight;
		u16 pixCntl;			// BEE8h index A
		u16 multMisc;			// BEE8h index E
		u16 multMisc2;			// BEE8h index D
		u16 readSel;			// BEE8h index F
		u32 bkgdColor, frgdColor, wrtMask, rdMask, colorCmp;
		u16 bkgdMix, frgdMix;
		u16 cmd;
		u16 subsysCntl;
		u8 subsysStat;
		u16 advFunc;			// 4AE8h
		bool highHalf;			// RSF toggle for 16-bit color accesses
		u32 pendingPort;		// low byte written, waiting for the high
		u8 pendingLow;
	};

	// A command waiting for its data through PIX_TRANS.
	struct Transfer {
		bool active;
		bool mono;				// across plane: 1 bit per pixel
		int x, y;				// current pixel
		int left;				// first column (in the direction)
		int width, height;		// pixels, lines
		int column, line;
		int dx, dy;
		int busBits;			// 8, 16, 32
		int bitsInWord;			// mono bits of the current bus word left
	};

			bool				_S3Unlocked() const
									{ return (fCrtc[0x38] & 0xcc) == 0x48; }
			bool				_SystemUnlocked() const
									{ return (fCrtc[0x39] & 0xe0) == 0xa0; }
			void				_UpdateBank();
			void				_UpdateMappings();
			bool				_EnhancedMapping() const
									{ return (fCrtc[0x31] & 0x08) != 0; }
			bool				_MmioEnabled() const
									{ return (fCrtc[0x53] & 0x10) != 0
										|| (fEngine.advFunc & 0x20) != 0; }
			bool				_LinearEnabled() const
									{ return (fCrtc[0x58] & 0x10) != 0
										|| (fEngine.advFunc & 0x10) != 0; }
			int					_DacMode() const
									{ return (fDacCommand >> 4) & 0x0f; }
			int					_HorizontalFactor() const;
			int					_CursorUnitsPerPixel() const;

			// legacy window (a0000-bffff relative offset)
			u8					_WindowRead(u32 offset);
			void				_WindowWrite(u32 offset, u8 value);

			// engine registers (little endian values)
			u32					_EngineRead(u32 port, int size);
			void				_EngineWrite(u32 port, u32 value, int size);
			void				_EngineWrite8(u32 port, u8 value);
			void				_EngineWrite16(u32 port, u16 value);
			void				_ColorWrite(u32& reg, u32 value, int size);
			u16					_ColorRead(u32 reg);
			void				_MultifunctionWrite(u16 value);
			u16					_MultifunctionRead();
			void				_PixTransWrite(u32 value, int size);

			// engine operations
			int					_BytesPerPixel() const;
			u32					_EngineStride() const;	// pixels
			u32					_PixelAddress(int x, int y) const;
			u32					_ReadPixel(int x, int y) const;
			void				_WritePixel(int x, int y, u32 source,
									bool foreground);
			bool				_Clipped(int x, int y) const;
			u32					_Mix(u16 mix, u32 source, u32 dest) const;
			u32					_MixSource(u16 mix, int x, int y,
									u32 cpuPixel) const;
			void				_Command(u16 cmd);
			void				_RectOrBlt(u16 cmd);
			void				_Line(u16 cmd);
			void				_ShortStroke(u8 vector);
			void				_StartTransfer(u16 cmd);
			void				_TransferPixel(u32 cpuPixel, bool cpuBit);

			PhysBus&			fBus;
			IoSpace&			fIo;
			Ports*				fPorts;
			EnginePorts*		fEnginePorts;
			LegacyWindow*		fLegacy;
			bool				fLegacyMapped;
			u32					fLinearBase;	// 0: not mapped
			u32					fLinearSize;

			u32					fBank;			// 64 KB units
			u8					fDacCommand;	// 21C498 command register
			int					fDacReads;		// of 3C6h in a row
			u8					fCursorFg[4];
			u8					fCursorBg[4];
			int					fCursorFgIndex;
			int					fCursorBgIndex;

			Engine				fEngine;
			Transfer			fTransfer;
			u32					fTransferBytes;	// collects a bus word
			int					fTransferCount;
};

}	// namespace bebox

#endif	// BEBOX_S3_VISION864_H

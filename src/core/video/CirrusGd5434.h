/*
 * BeBox emulator - Cirrus Logic CL-GD5434 ("Alpine") PCI graphics card.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Supported by the BeBox boot ROM (PCI 1013:00a8) and by BeOS DR8 and
 * later (unaccelerated). VGA core plus the Cirrus extensions: extension
 * lock (SR6), extended pixel modes (SR7 and the hidden DAC register),
 * display start/pitch extensions (CR1B, CR1D), bank registers (GR9-GRB),
 * the hardware cursor (SR10-SR13), the BitBLT engine (GR20-GR37) and a
 * 16 MB linear aperture with
 * byte swapping windows for big endian hosts:
 *   0-4 MB no swap, 4-8 MB bytes swapped in 16-bit words,
 *   8-12 MB bytes swapped in 32-bit words, 12-16 MB no swap.
 */
#ifndef BEBOX_CIRRUS_GD5434_H
#define BEBOX_CIRRUS_GD5434_H

#include "VgaCore.h"
#include "../bus/IoSpace.h"
#include "../bus/PciBus.h"
#include "../bus/PhysBus.h"

#include <vector>

namespace bebox {

class CirrusGd5434 : public PciDevice, public VgaCore {
public:
	static const u32 kApertureSize = 16 << 20;

								CirrusGd5434(PhysBus& bus, IoSpace& io,
									Scheduler& scheduler,
									u64 ticksPerSecond, u32 vramSize);
								~CirrusGd5434();

			void				Reset();

	virtual	bool				HardwareCursor(int& x, int& y) const;

	// PciDevice
	virtual	const char*			PciName() const { return "cirrus-gd5434"; }

protected:
	virtual	void				BarsChanged();

	// VgaCore hooks
	virtual	void				ComputeMode(DisplayMode& mode);
	virtual	u8					SequencerRead(u8 index);
	virtual	void				SequencerWrite(u8 index, u8 value);
	virtual	u8					CrtcRead(u8 index);
	virtual	void				CrtcWrite(u8 index, u8 value);
	virtual	u8					GraphicsRead(u8 index);
	virtual	void				GraphicsWrite(u8 index, u8 value);
	virtual	u8					DacMaskRead();
	virtual	void				DacMaskWrite(u8 value);
	virtual	void				DacAccessed() { fHiddenDacCounter = 0; }
	virtual	u32					LegacyBankOffset(u32 offset);
	virtual	void				DrawOverlays(u32* frame, int width,
									int height, int stride);

private:
	class Ports;
	class LegacyWindow;
	class SwapAperture;
	class BltSink;

	struct Blt {
		u32 width;			// bytes
		u32 height;
		u32 dstPitch;
		u32 srcPitch;
		u32 dst;
		u32 src;
		u8 mode;			// GR30
		u8 rop;				// GR32
		int bpp;			// bytes per pixel (expansion, patterns)
		u32 foreground;
		u32 background;
		int skipLeft;		// GR2F: pixels skipped at the left
	};

			bool				_Unlocked() const { return fSeq[6] == 0x12; }
			void				_StartBlt();
			void				_FinishBlt();
			void				_BltVideoToVideo(const Blt& blt);
			void				_BltPattern(const Blt& blt);
			void				_BltExpandLine(const Blt& blt, u32 dst,
									const u8* bits);
			void				_BltCopyLine(const Blt& blt, u32 dst,
									const u8* source);
			void				_BltFeed(u8 byte);
			void				_RouteApertureToBlt(bool toBlt);
			u8					_Rop(u8 src, u8 dst) const;

			PhysBus&			fBus;
			IoSpace&			fIo;
			Ports*				fPorts;
			LegacyWindow*		fLegacy;
			SwapAperture*		fSwap16;
			SwapAperture*		fSwap32;
			BltSink*			fBltSink;
			u32					fMappedBase;	// 0: not mapped
			bool				fLegacyMapped;
			u8					fHiddenDac;
			u32					fCursorX;
			u32					fCursorY;

			// system memory source BLT in progress
			bool				fBltActive;
			Blt					fBlt;
			std::vector<u8>		fBltLine;		// source line being received
			u32					fBltLineBytes;	// bytes per source line
			u32					fBltLinesDone;
			u32					fBltReceived;	// source bytes so far
			bool				fBltDraining;	// eating dword padding
};

}	// namespace bebox

#endif	// BEBOX_CIRRUS_GD5434_H

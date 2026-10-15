/*
 * BeBox emulator - VGA core shared by the emulated graphics cards.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Standard VGA registers (sequencer, CRTC, graphics controller, attribute
 * controller, DAC), the legacy memory window (planar and chain-4 access)
 * and display rendering. Video memory is stored linearly; plane p of
 * address a is byte a * 4 + p (so chain-4 and packed pixel modes see
 * plain linear memory). Cards add their extensions by overriding the
 * hooks.
 */
#ifndef BEBOX_VGA_CORE_H
#define BEBOX_VGA_CORE_H

#include "../Types.h"
#include "../board/Scheduler.h"
#include "../host/VideoSink.h"

#include <memory>
#include <vector>

namespace bebox {

class VgaCore {
public:
	enum PixelFormat {
		kText,
		kPlanar4,
		kIndexed8,
		kRgb555,
		kRgb565,
		kRgb888,		// 3 bytes per pixel: B, G, R
		kRgbx8888		// 4 bytes per pixel: B, G, R, X
	};

	struct DisplayMode {
		PixelFormat format = kText;
		int width = 0;
		int height = 0;
		u32 start = 0;			// byte offset in video memory
		u32 pitch = 0;			// bytes per line
		int pixelDoubling = 1;	// horizontal (mode 13h)
		int lineRepeat = 1;		// vertical (double scan / max scan line)
	};

								VgaCore(Scheduler& scheduler,
									u64 ticksPerSecond, u32 vramSize);
	virtual						~VgaCore();

			void				SetVideoSink(VideoSink* sink)
									{ fSink = sink; }
			void				ResetVga();

			u8*					Vram() { return fVram.data(); }
			u32					VramSize() const
									{ return (u32)fVram.size(); }

			// VGA I/O ports (0x3b0-0x3df).
			u8					VgaRead(u32 port);
			void				VgaWrite(u32 port, u8 value);

			// Legacy memory window (offset from 0xa0000).
			u8					LegacyRead(u32 offset);
			void				LegacyWrite(u32 offset, u8 value);

			// Renders the current frame into fFrame and passes it to the
			// sink. Normally called by the retrace timer.
			void				RenderFrame();
			const DisplayMode&	CurrentMode() const { return fMode; }
			// Position of a visible hardware cursor (its hot spot is the
			// guest's business), for tests driving the mouse.
	virtual	bool				HardwareCursor(int& x, int& y) const
									{ return false; }

protected:
	// Extension hooks
	virtual	void				ComputeMode(DisplayMode& mode);
	virtual	u8					SequencerRead(u8 index);
	virtual	void				SequencerWrite(u8 index, u8 value);
	virtual	u8					CrtcRead(u8 index);
	virtual	void				CrtcWrite(u8 index, u8 value);
	virtual	u8					GraphicsRead(u8 index);
	virtual	void				GraphicsWrite(u8 index, u8 value);
	virtual	u8					DacMaskRead() { return fDacMask; }
	// Any access to 0x3c7-0x3c9 (resets the hidden DAC unlock sequence).
	virtual	void				DacAccessed() {}
	virtual	void				DacMaskWrite(u8 value) { fDacMask = value; }
	// Bank offset for legacy window accesses (Cirrus offset registers).
	virtual	u32					LegacyBankOffset(u32 offset) { return 0; }
	// Overlays (hardware cursor) drawn on the rendered frame.
	virtual	void				DrawOverlays(u32* frame, int width,
									int height, int stride) {}
	// The 256 entry palette as 0x00RRGGBB.
			u32					PaletteColor(int index) const;

			int					_StandardWidth() const;
			int					_StandardHeight() const;
			u32					_StandardStart() const;
			int					_StandardLineRepeat() const;

			void				_Retrace();

			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			VideoSink*			fSink;
			std::vector<u8>		fVram;
			std::vector<u32>	fFrame;
			DisplayMode			fMode;
			std::unique_ptr<Timer> fRetraceTimer;
			u64					fFrameStart;
			u64					fFrameTicks;

			u8					fMisc;
			u8					fFeature;
			u8					fSeqIndex;
			u8					fSeq[256];
			u8					fCrtcIndex;
			u8					fCrtc[256];
			u8					fGcIndex;
			u8					fGc[256];
			u8					fAtcIndex;
			bool				fAtcFlipFlop;
			u8					fAtc[32];
			u8					fDacMask;
			u8					fDacReadIndex;
			u8					fDacWriteIndex;
			int					fDacComponent;
			bool				fDacReading;
			u8					fPalette[256 * 3];	// 6-bit values
			// Cirrus: extended 16 entry palette (cursor colors) selected
			// by SR12 bit 1.
			u8					fExtPalette[16 * 3];
			bool				fDacExtended;
			// 8-bit palette entries (RAMDAC command register), else 6-bit.
			bool				fDac8Bit;
			int					fHiddenDacCounter;	// reads of 0x3c6
			u8					fLatch[4];
};

}	// namespace bebox

#endif	// BEBOX_VGA_CORE_H

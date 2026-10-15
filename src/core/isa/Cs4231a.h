/*
 * BeBox emulator - Crystal CS4231A audio codec (Windows Sound System
 * compatible), on the BeBox I/O card at 0x830 (Be interrupt bit 14).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Four direct registers (index/address, indexed data, status, PIO data)
 * and 32 indexed registers (mode 2). Playback pulls sample frames through
 * an ISA DMA channel at the programmed sample rate and hands them to the
 * host as 16-bit stereo; capture delivers silence. The sample counters,
 * the timer and the interrupt status work as on the chip.
 */
#ifndef BEBOX_CS4231A_H
#define BEBOX_CS4231A_H

#include "../bus/IoSpace.h"
#include "../board/Scheduler.h"

#include <functional>
#include <memory>
#include <vector>

namespace bebox {

class AudioSink;
class Dma8237;

class Cs4231a : public ByteIoDevice {
public:
	typedef std::function<void(bool)> IrqCallback;

								Cs4231a(Scheduler& scheduler,
									u64 ticksPerSecond, Dma8237& dma,
									int playbackDma, int captureDma);

			void				Reset();
			void				SetIrqCallback(IrqCallback callback)
									{ fIrq = callback; }
			void				SetAudioSink(AudioSink* sink)
									{ fSink = sink; }

			u32					SampleRate() const;
			bool				IsPlaying() const;

	virtual	u8					IoRead8(u32 port);
	virtual	void				IoWrite8(u32 port, u8 value);
	virtual	const char*			IoName() const { return "cs4231a"; }

private:
			u8					_ReadIndexed(int index);
			void				_WriteIndexed(int index, u8 value);
			void				_UpdateIrq();
			void				_Interrupt(u8 bit);
			void				_Tick();
			void				_Schedule();
			u32					_PlayFrames(u32 count);
			u32					_CaptureFrames(u32 count);
			int					_BytesPerSample(u8 format) const;
			s16					_Decode(const u8* data, u8 format) const;

			Scheduler&			fScheduler;
			u64					fTicksPerSecond;
			Dma8237&			fDma;
			int					fPlaybackDma;
			int					fCaptureDma;
			IrqCallback			fIrq;
			AudioSink*			fSink;
			std::unique_ptr<Timer> fTimer;
			std::unique_ptr<Timer> fCalibration;

			u8					fIndex;			// R0 bits 0-4
			bool				fModeChange;	// MCE
			bool				fTransferDisable;	// TRD
			u8					fRegs[32];
			bool				fInterrupt;		// status INT
			bool				fIrqLevel;
			u16					fPlaybackCount;	// current counters
			u16					fCaptureCount;
			u32					fTimerCount;	// in 10 us units
			u64					fLastTick;
			u64					fFraction;		// frame accumulator
			u32					fPlayDue;		// frames due, not played yet
			u32					fCaptureDue;
			std::vector<s16>	fOutput;
};

}	// namespace bebox

#endif	// BEBOX_CS4231A_H

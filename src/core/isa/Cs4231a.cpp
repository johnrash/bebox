/*
 * BeBox emulator - Crystal CS4231A audio codec.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "Cs4231a.h"

#include "Dma8237.h"
#include "../Log.h"
#include "../host/AudioSink.h"

#include <algorithm>
#include <cstring>

namespace bebox {

enum {
	// R0
	kIndexInit = 0x80,
	kIndexModeChange = 0x40,
	kIndexTransferDisable = 0x20,

	// R2 status
	kStatusInt = 0x01,
	kStatusPrdy = 0x02,
	kStatusSampleError = 0x10,

	// I9 interface configuration
	kConfigPlay = 0x01,
	kConfigCapture = 0x02,
	kConfigCalibrate = 0x18,
	kConfigPlayPio = 0x40,
	kConfigCapturePio = 0x80,

	// I10 pin control
	kPinIrqEnable = 0x02,

	// I11 error status
	kErrorAutoCalibrating = 0x20,
	kErrorPlaybackUnderrun = 0x40,
	kErrorCaptureOverrun = 0x80,

	// I12
	kMode2 = 0x40,

	// I16 alternate feature enable I
	kTimerEnable = 0x40,

	// I24 alternate feature status
	kStatusPlayback = 0x10,
	kStatusCapture = 0x20,
	kStatusTimer = 0x40,
	kStatusInterrupts = 0x70,

	// formats (I8/I28 bits 5-7)
	kFormatLinear8 = 0,
	kFormatMuLaw = 1,
	kFormatLinear16 = 2,
	kFormatALaw = 3,
	kFormatAdpcm = 5,
	kFormatLinear16Big = 6
};

// Sample rates for the clock frequency divide select (I8 bits 1-3) with
// the 24.576 MHz (XTAL1, I8 bit 0 clear) and 16.9344 MHz crystals.
static const u32 kRates[2][8] = {
	{ 8000, 16000, 27429, 32000, 32000, 32000, 48000, 9600 },
	{ 5513, 11025, 18900, 22050, 37800, 44100, 33075, 6615 }
};


static s16
MuLawToLinear(u8 value)
{
	value = ~value;
	int sign = value & 0x80;
	int exponent = (value >> 4) & 7;
	int mantissa = value & 0x0f;
	int sample = (((mantissa << 3) + 0x84) << exponent) - 0x84;
	return (s16)(sign ? -sample : sample);
}


static s16
ALawToLinear(u8 value)
{
	value ^= 0x55;
	int sign = value & 0x80;
	int exponent = (value >> 4) & 7;
	int mantissa = value & 0x0f;
	int sample = exponent == 0 ? (mantissa << 4) + 8
		: ((mantissa << 4) + 0x108) << (exponent - 1);
	return (s16)(sign ? sample : -sample);
}


Cs4231a::Cs4231a(Scheduler& scheduler, u64 ticksPerSecond, Dma8237& dma,
	int playbackDma, int captureDma)
	:
	fScheduler(scheduler),
	fTicksPerSecond(ticksPerSecond),
	fDma(dma),
	fPlaybackDma(playbackDma),
	fCaptureDma(captureDma),
	fSink(nullptr),
	fIrqLevel(false)
{
	fTimer.reset(new Timer(scheduler, [this]() { _Tick(); }, "cs4231a"));
	fCalibration.reset(new Timer(scheduler, [this]() {
		fRegs[11] &= ~kErrorAutoCalibrating;
	}, "cs4231a-calibration"));
	Reset();
}


void
Cs4231a::Reset()
{
	fIndex = 0;
	fModeChange = true;
	fTransferDisable = false;
	memset(fRegs, 0, sizeof(fRegs));
	fRegs[0] = fRegs[1] = 0x00;
	fRegs[2] = fRegs[3] = fRegs[4] = fRegs[5] = 0x80;	// aux muted
	fRegs[6] = fRegs[7] = 0x80;		// DAC muted
	fRegs[9] = kConfigCalibrate | 0x04;
	fRegs[12] = 0x8a;
	fRegs[25] = 0xa0;				// CS4231A
	fRegs[26] = 0x80;
	fInterrupt = false;
	fPlaybackCount = fCaptureCount = 0;
	fTimerCount = 0;
	fFraction = 0;
	fPlayDue = fCaptureDue = 0;
	fTimer->Cancel();
	fCalibration->Cancel();
	_UpdateIrq();
}


u32
Cs4231a::SampleRate() const
{
	u8 format = fRegs[8];
	return kRates[format & 1][(format >> 1) & 7];
}


bool
Cs4231a::IsPlaying() const
{
	return (fRegs[9] & (kConfigPlay | kConfigPlayPio)) == kConfigPlay;
}


int
Cs4231a::_BytesPerSample(u8 format) const
{
	switch ((format >> 5) & 7) {
		case kFormatLinear16:
		case kFormatLinear16Big:
			return 2;
	}
	return 1;
}


s16
Cs4231a::_Decode(const u8* data, u8 format) const
{
	switch ((format >> 5) & 7) {
		case kFormatLinear8:
			return (s16)((data[0] - 128) << 8);
		case kFormatMuLaw:
			return MuLawToLinear(data[0]);
		case kFormatALaw:
			return ALawToLinear(data[0]);
		case kFormatLinear16:
			return (s16)(data[0] | (data[1] << 8));
		case kFormatLinear16Big:
			return (s16)((data[0] << 8) | data[1]);
	}
	return 0;		// ADPCM is not emulated
}


// #pragma mark - registers


u8
Cs4231a::IoRead8(u32 port)
{
	BB_LOG(kLogAudio, kLogTrace, "cs4231a: read R%d (index %d)", port & 3,
		fIndex);
	switch (port & 3) {
		case 0:
			return fIndex | (fModeChange ? kIndexModeChange : 0)
				| (fTransferDisable ? kIndexTransferDisable : 0);
		case 1:
			return _ReadIndexed(fIndex);
		case 2:
			return (fInterrupt ? kStatusInt : 0) | kStatusPrdy
				| ((fRegs[11] & (kErrorPlaybackUnderrun
					| kErrorCaptureOverrun)) != 0 ? kStatusSampleError : 0);
		case 3:
			return 0x80;	// PIO capture data (silence)
	}
	return 0xff;
}


void
Cs4231a::IoWrite8(u32 port, u8 value)
{
	switch (port & 3) {
		case 0:
		{
			bool wasModeChange = fModeChange;
			fIndex = value & ((fRegs[12] & kMode2) != 0 ? 0x1f : 0x0f);
			fModeChange = (value & kIndexModeChange) != 0;
			fTransferDisable = (value & kIndexTransferDisable) != 0;
			if (wasModeChange && !fModeChange
				&& (fRegs[9] & kConfigCalibrate) != 0) {
				// auto calibration takes a few hundred sample periods
				fRegs[11] |= kErrorAutoCalibrating;
				fCalibration->ArmIn(fTicksPerSecond / 500);	// 2 ms
			}
			break;
		}
		case 1:
			_WriteIndexed(fIndex, value);
			break;
		case 2:
			// any write clears all interrupt conditions
			fRegs[24] &= ~kStatusInterrupts;
			fRegs[11] &= ~(kErrorPlaybackUnderrun | kErrorCaptureOverrun);
			_UpdateIrq();
			break;
		case 3:
			break;		// PIO playback data: not emulated
	}
}


u8
Cs4231a::_ReadIndexed(int index)
{
	switch (index) {
		case 11:
			return fRegs[11] | 0x10;	// DRQ status: ready
		case 12:
			return (fRegs[12] & kMode2) | 0x8a;
		case 25:
			return 0xa0;
	}
	return fRegs[index];
}


void
Cs4231a::_WriteIndexed(int index, u8 value)
{
	BB_LOG(kLogAudio, kLogTrace, "cs4231a: I%d = %02x", index, value);
	switch (index) {
		case 9:
		{
			u8 old = fRegs[9];
			fRegs[9] = value;
			if ((value & kConfigPlay) != 0 && (old & kConfigPlay) == 0) {
				fPlaybackCount = fRegs[14] << 8 | fRegs[15];
				BB_LOG(kLogAudio, kLogDebug, "cs4231a: playback on at %.3f s, "
					"%u Hz, format %02x, count %u",
					(double)fScheduler.Now() / fTicksPerSecond, SampleRate(),
					fRegs[8], fPlaybackCount);
			}
			if ((value & kConfigPlay) == 0 && (old & kConfigPlay) != 0)
				BB_LOG(kLogAudio, kLogDebug, "cs4231a: playback off at %.3f s",
					(double)fScheduler.Now() / fTicksPerSecond);
			if ((value & kConfigCapture) != 0
				&& (old & kConfigCapture) == 0)
				fCaptureCount = fRegs[30] << 8 | fRegs[31];
			_Schedule();
			return;
		}
		case 10:
			fRegs[10] = value;
			_UpdateIrq();
			return;
		case 11:
		case 25:
			return;		// read only
		case 12:
			fRegs[12] = (value & kMode2) | 0x8a;
			return;
		case 16:
			fRegs[16] = value;
			if ((value & kTimerEnable) != 0)
				fTimerCount = fRegs[21] << 8 | fRegs[20];
			_Schedule();
			return;
		case 24:
			// writing 0 clears interrupt status bits
			fRegs[24] &= value | ~kStatusInterrupts;
			_UpdateIrq();
			return;
	}
	fRegs[index] = value;
}


void
Cs4231a::_UpdateIrq()
{
	fInterrupt = (fRegs[24] & kStatusInterrupts) != 0;
	bool level = fInterrupt && (fRegs[10] & kPinIrqEnable) != 0;
	if (level != fIrqLevel) {
		fIrqLevel = level;
		if (fIrq)
			fIrq(level);
	}
}


void
Cs4231a::_Interrupt(u8 bit)
{
	fRegs[24] |= bit;
	_UpdateIrq();
}


// #pragma mark - sample clock


void
Cs4231a::_Schedule()
{
	bool active = (fRegs[9] & (kConfigPlay | kConfigCapture)) != 0
		|| (fRegs[16] & kTimerEnable) != 0;
	if (!active) {
		fTimer->Cancel();
		return;
	}
	if (!fTimer->IsArmed()) {
		fLastTick = fScheduler.Now();
		fFraction = 0;
		fPlayDue = fCaptureDue = 0;
		fTimer->ArmIn(fTicksPerSecond / 1000);
	}
}


// Every millisecond: the frames of the elapsed time.
void
Cs4231a::_Tick()
{
	u64 now = fScheduler.Now();
	u64 elapsed = now - fLastTick;
	fLastTick = now;
	fFraction += elapsed * SampleRate();
	u32 frames = (u32)(fFraction / fTicksPerSecond);
	fFraction %= fTicksPerSecond;

	// A sample counter interrupt stops the batch: the driver gets to
	// queue the next buffer before the codec goes on.
	bool interrupted = false;
	if (IsPlaying()) {
		fPlayDue = std::min<u32>(fPlayDue + frames, SampleRate() / 10);
		u32 played = fPlayDue > 0 ? _PlayFrames(fPlayDue) : 0;
		interrupted |= played < fPlayDue;
		fPlayDue -= played;
	} else
		fPlayDue = 0;
	if ((fRegs[9] & (kConfigCapture | kConfigCapturePio))
			== kConfigCapture) {
		fCaptureDue = std::min<u32>(fCaptureDue + frames, SampleRate() / 10);
		u32 captured = fCaptureDue > 0 ? _CaptureFrames(fCaptureDue) : 0;
		interrupted |= captured < fCaptureDue;
		fCaptureDue -= captured;
	} else
		fCaptureDue = 0;
	if ((fRegs[16] & kTimerEnable) != 0) {
		// 10 us per timer count
		u64 counts = elapsed * 100000 / fTicksPerSecond;
		u32 reload = fRegs[21] << 8 | fRegs[20];
		while (counts > 0 && reload > 0) {
			if (counts < fTimerCount) {
				fTimerCount -= (u32)counts;
				break;
			}
			counts -= fTimerCount;
			fTimerCount = reload;
			_Interrupt(kStatusTimer);
		}
	}
	_Schedule();
	if ((fRegs[9] & (kConfigPlay | kConfigCapture)) != 0
		|| (fRegs[16] & kTimerEnable) != 0) {
		fTimer->ArmIn(interrupted || fPlayDue > 0 || fCaptureDue > 0
			? fTicksPerSecond / 20000 : fTicksPerSecond / 1000);
	}
}


// Plays up to 'count' frames; stops after the frame that raises the
// playback interrupt. Returns the number of frames played.
u32
Cs4231a::_PlayFrames(u32 count)
{
	u32 untilInterrupt = (u32)fPlaybackCount + 1;
	if (count > untilInterrupt)
		count = untilInterrupt;
	u8 format = fRegs[8];
	int channels = (format & 0x10) != 0 ? 2 : 1;
	int sampleBytes = _BytesPerSample(format);
	int frameBytes = sampleBytes * channels;
	std::vector<u8> data((size_t)count * frameBytes);
	bool terminalCount = false;
	u32 got = fDma.Transfer(fPlaybackDma, data.data(), (u32)data.size(),
		terminalCount);
	if (got < data.size()) {
		memset(data.data() + got, (format >> 5) == kFormatLinear8 ? 0x80 : 0,
			data.size() - got);
		fRegs[11] |= kErrorPlaybackUnderrun;
	}
	fOutput.resize((size_t)count * 2);
	bool muteLeft = (fRegs[6] & 0x80) != 0;
	bool muteRight = (fRegs[7] & 0x80) != 0;
	// DAC attenuation: 1.5 dB per step (6 bits)
	auto gain = [](u8 control) {
		int steps = control & 0x3f;
		int scale = 4096;
		for (int i = 0; i < steps; i++)
			scale = scale * 841 / 1000;		// -1.5 dB
		return scale;
	};
	int leftGain = gain(fRegs[6]);
	int rightGain = gain(fRegs[7]);
	for (u32 i = 0; i < count; i++) {
		const u8* frame = &data[(size_t)i * frameBytes];
		s16 left = _Decode(frame, format);
		s16 right = channels == 2 ? _Decode(frame + sampleBytes, format)
			: left;
		fOutput[i * 2] = muteLeft ? 0 : (s16)(left * leftGain / 4096);
		fOutput[i * 2 + 1] = muteRight ? 0 : (s16)(right * rightGain / 4096);
		if (fPlaybackCount == 0) {
			fPlaybackCount = fRegs[14] << 8 | fRegs[15];
			_Interrupt(kStatusPlayback);
		} else
			fPlaybackCount--;
	}
	if (fSink != nullptr)
		fSink->Play(fOutput.data(), count, SampleRate());
	return count;
}


u32
Cs4231a::_CaptureFrames(u32 count)
{
	u32 untilInterrupt = (u32)fCaptureCount + 1;
	if (count > untilInterrupt)
		count = untilInterrupt;
	u8 format = (fRegs[12] & kMode2) != 0 ? fRegs[28] : fRegs[8];
	int channels = (format & 0x10) != 0 ? 2 : 1;
	int frameBytes = _BytesPerSample(format) * channels;
	std::vector<u8> silence((size_t)count * frameBytes,
		(format >> 5) == kFormatLinear8 ? 0x80 : 0x00);
	bool terminalCount = false;
	u32 put = fDma.Transfer(fCaptureDma, silence.data(),
		(u32)silence.size(), terminalCount);
	if (put < silence.size())
		fRegs[11] |= kErrorCaptureOverrun;
	for (u32 i = 0; i < count; i++) {
		if (fCaptureCount == 0) {
			fCaptureCount = fRegs[30] << 8 | fRegs[31];
			_Interrupt(kStatusCapture);
		} else
			fCaptureCount--;
	}
	return count;
}

}	// namespace bebox

/*
 * BeBox emulator for Haiku - sound output through BSoundPlayer.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "HaikuAudio.h"

#include "BeBox.h"

#include <SoundPlayer.h>

#include <algorithm>
#include <cstring>

using bebox::s16;
using bebox::u32;

static const uint32 kRingFrames = 1 << 15;		// 0.7 s at 44.1 kHz
// Start playing once this much is buffered (smooths the start, and is
// the steady state latency).
static const uint32 kPrimeFrames = 4096;
// Rate control: the sound card's clock and the clock the emulation is
// paced by drift apart (by more than 1% in some virtual machines), which
// would drain or fill the ring. The resampling step is corrected by up to
// kMaxCorrection to keep the (smoothed) fill level near kPrimeFrames.
static const double kFillSmoothing = 0.02;		// per player buffer
static const double kRateGain = 0.04;			// per kPrimeFrames of error
static const double kMaxCorrection = 0.02;


HaikuAudio::HaikuAudio()
	:
	fPlayer(nullptr),
	fRing(kRingFrames * 2),
	fCapacity(kRingFrames),
	fWrite(0),
	fRead(0),
	fSourceRate(44100),
	fPosition(0),
	fVolume(1.0f),
	fMuted(false),
	fPrimed(false),
	fAverageFill(0),
	fReceived(0),
	fDropped(0),
	fConsumed(0),
	fOutput(0),
	fUnderruns(0),
	fOutputRate(0),
	fBufferFrames(0),
	fCorrection(0)
{
}


HaikuAudio::~HaikuAudio()
{
	Stop();
}


status_t
HaikuAudio::Start()
{
	if (fPlayer != nullptr)
		return B_OK;
	media_raw_audio_format format = media_raw_audio_format::wildcard;
	format.frame_rate = 44100;
	format.channel_count = 2;
	format.format = media_raw_audio_format::B_AUDIO_FLOAT;
	format.byte_order = B_MEDIA_HOST_ENDIAN;
	format.buffer_size = 512 * 2 * sizeof(float);
	fPlayer = new BSoundPlayer(&format, kAppName, &_PlayBuffer, nullptr,
		this);
	status_t status = fPlayer->InitCheck();
	if (status != B_OK) {
		delete fPlayer;
		fPlayer = nullptr;
		return status;
	}
	fPlayer->SetHasData(true);
	status = fPlayer->Start();
	if (status != B_OK) {
		delete fPlayer;
		fPlayer = nullptr;
	}
	return status;
}


void
HaikuAudio::Stop()
{
	if (fPlayer == nullptr)
		return;
	fPlayer->Stop();
	delete fPlayer;
	fPlayer = nullptr;
}


// Emulation thread.
void
HaikuAudio::Play(const s16* frames, size_t count, u32 sampleRate)
{
	fSourceRate.store(sampleRate, std::memory_order_relaxed);
	uint32 write = fWrite.load(std::memory_order_relaxed);
	uint32 read = fRead.load(std::memory_order_acquire);
	uint32 space = fCapacity - (write - read);
	fReceived.fetch_add(count, std::memory_order_relaxed);
	if (count > space) {
		// the host is not keeping up: drop
		fDropped.fetch_add(count - space, std::memory_order_relaxed);
		count = space;
	}
	for (size_t i = 0; i < count; i++) {
		uint32 slot = (write + i) & (fCapacity - 1);
		fRing[slot * 2] = frames[i * 2];
		fRing[slot * 2 + 1] = frames[i * 2 + 1];
	}
	fWrite.store(write + (uint32)count, std::memory_order_release);
}


void
HaikuAudio::_PlayBuffer(void* cookie, void* buffer, size_t size,
	const media_raw_audio_format& format)
{
	HaikuAudio* audio = (HaikuAudio*)cookie;
	if (format.format != media_raw_audio_format::B_AUDIO_FLOAT
		|| format.channel_count != 2) {
		memset(buffer, 0, size);
		return;
	}
	audio->fOutputRate.store(format.frame_rate, std::memory_order_relaxed);
	audio->fBufferFrames.store(size / (2 * sizeof(float)),
		std::memory_order_relaxed);
	audio->_Fill((float*)buffer, size / (2 * sizeof(float)),
		format.frame_rate);
}


HaikuAudio::Stats
HaikuAudio::GetStats() const
{
	Stats stats;
	stats.received = fReceived.load(std::memory_order_relaxed);
	stats.dropped = fDropped.load(std::memory_order_relaxed);
	stats.consumed = fConsumed.load(std::memory_order_relaxed);
	stats.output = fOutput.load(std::memory_order_relaxed);
	stats.underruns = fUnderruns.load(std::memory_order_relaxed);
	stats.buffered = fWrite.load(std::memory_order_relaxed)
		- fRead.load(std::memory_order_relaxed);
	stats.outputRate = fOutputRate.load(std::memory_order_relaxed);
	stats.bufferFrames = fBufferFrames.load(std::memory_order_relaxed);
	stats.correction = fCorrection.load(std::memory_order_relaxed);
	return stats;
}


// Sound player thread: linear interpolation between ring frames.
void
HaikuAudio::_Fill(float* out, size_t frames, float outputRate)
{
	fOutput.fetch_add(frames, std::memory_order_relaxed);
	uint32 read = fRead.load(std::memory_order_relaxed);
	uint32 write = fWrite.load(std::memory_order_acquire);
	uint32 available = write - read;
	uint32 start = read;
	if (!fPrimed) {
		if (available < kPrimeFrames) {
			memset(out, 0, frames * 2 * sizeof(float));
			return;
		}
		fPrimed = true;
		fAverageFill = available;
	}
	fAverageFill += (available - fAverageFill) * kFillSmoothing;
	double correction = std::max(-kMaxCorrection, std::min(kMaxCorrection,
		(fAverageFill - kPrimeFrames) / kPrimeFrames * kRateGain));
	fCorrection.store((float)correction, std::memory_order_relaxed);
	double step = (double)fSourceRate.load(std::memory_order_relaxed)
		/ outputRate * (1.0 + correction);
	float scale = fMuted ? 0.0f : fVolume.load() / 32768.0f;
	for (size_t i = 0; i < frames; i++) {
		if (available < 2) {
			// ran dry: silence until enough is buffered again
			memset(out + i * 2, 0, (frames - i) * 2 * sizeof(float));
			fPrimed = false;
			fUnderruns.fetch_add(1, std::memory_order_relaxed);
			break;
		}
		uint32 a = read & (fCapacity - 1);
		uint32 b = (read + 1) & (fCapacity - 1);
		float t = (float)fPosition;
		out[i * 2] = (fRing[a * 2] + (fRing[b * 2] - fRing[a * 2]) * t)
			* scale;
		out[i * 2 + 1] = (fRing[a * 2 + 1]
			+ (fRing[b * 2 + 1] - fRing[a * 2 + 1]) * t) * scale;
		fPosition += step;
		while (fPosition >= 1.0 && available >= 2) {
			fPosition -= 1.0;
			read++;
			available--;
		}
	}
	fConsumed.fetch_add(read - start, std::memory_order_relaxed);
	fRead.store(read, std::memory_order_release);
}

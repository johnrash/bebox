/*
 * BeBox emulator for Haiku - sound output through BSoundPlayer.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * The emulation thread writes the codec's frames into a single producer,
 * single consumer ring; the sound player's thread reads them, resampling
 * linearly from the guest rate to the player's rate, a little faster or
 * slower to keep the ring's fill level steady while the two clocks drift.
 * An empty ring plays silence (the emulation is behind), a full one drops
 * frames.
 */
#ifndef HAIKU_AUDIO_H
#define HAIKU_AUDIO_H

#include "core/host/AudioSink.h"

#include <MediaDefs.h>
#include <SupportDefs.h>

#include <atomic>
#include <vector>

class BSoundPlayer;

class HaikuAudio : public bebox::AudioSink {
public:
	struct Stats {
		uint64			received;		// frames from the codec
		uint64			dropped;		// of those, dropped (ring full)
		uint64			consumed;		// frames taken from the ring
		uint64			output;			// frames the player asked for
		uint32			underruns;		// times the ring ran dry
		uint32			buffered;		// frames in the ring now
		float			outputRate;
		uint32			bufferFrames;	// per player buffer
		float			correction;		// of the resampling step
		uint32			primeFrames;	// the fill level aimed at
	};

								HaikuAudio();
	virtual						~HaikuAudio();

			status_t			Start();
			void				Stop();
			bool				IsRunning() const { return fPlayer != nullptr; }
			void				SetVolume(float volume) { fVolume = volume; }
			void				SetMuted(bool muted) { fMuted = muted; }

	virtual	void				Play(const bebox::s16* frames, size_t count,
									bebox::u32 sampleRate);

			Stats				GetStats() const;

private:
	static	void				_PlayBuffer(void* cookie, void* buffer,
									size_t size,
									const media_raw_audio_format& format);
			void				_Fill(float* out, size_t frames,
									float outputRate);

			BSoundPlayer*		fPlayer;
			std::vector<bebox::s16> fRing;	// stereo frames
			uint32				fCapacity;		// frames, a power of two
			std::atomic<uint32>	fWrite;
			std::atomic<uint32>	fRead;
			std::atomic<uint32>	fSourceRate;
			double				fPosition;		// between fRead and +1
			std::atomic<float>	fVolume;
			std::atomic<bool>	fMuted;
			bool				fPrimed;
			std::atomic<uint32>	fPrimeFrames;	// grows after running dry
			double				fAverageFill;	// frames

			std::atomic<uint64>	fReceived;
			std::atomic<uint64>	fDropped;
			std::atomic<uint64>	fConsumed;
			std::atomic<uint64>	fOutput;
			std::atomic<uint32>	fUnderruns;
			std::atomic<float>	fOutputRate;
			std::atomic<uint32>	fBufferFrames;
			std::atomic<float>	fCorrection;
};

#endif	// HAIKU_AUDIO_H

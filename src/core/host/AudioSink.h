/*
 * BeBox emulator - host side of the emulated sound output.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 *
 * Play() is called from the emulation thread with stereo 16-bit frames at
 * the codec's sample rate, in emulated real time; the host buffers and
 * resamples them for its own audio output.
 */
#ifndef BEBOX_AUDIO_SINK_H
#define BEBOX_AUDIO_SINK_H

#include "../Types.h"

#include <cstddef>

namespace bebox {

class AudioSink {
public:
	virtual						~AudioSink() {}

	// 'frames' holds 'count' interleaved left/right samples.
	virtual	void				Play(const s16* frames, size_t count,
									u32 sampleRate) = 0;
};

}	// namespace bebox

#endif	// BEBOX_AUDIO_SINK_H

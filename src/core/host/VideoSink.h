/*
 * BeBox emulator - host side of the emulated display.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#ifndef BEBOX_VIDEO_SINK_H
#define BEBOX_VIDEO_SINK_H

#include "../Types.h"

namespace bebox {

class VideoSink {
public:
	virtual ~VideoSink() {}

	// The picture changed: called at the emulated vertical retrace on the
	// emulation thread (not when nothing changed). 'pixels' holds the whole
	// picture as 0x00RRGGBB host-endian values; 'stride' is in pixels.
	// Lines 'top' to 'bottom' - 1 changed, the others are as in the last
	// call (all of them when the size changed). The buffer is only valid
	// during the call.
	virtual void FrameReady(const u32* pixels, int width, int height,
		int stride, int top, int bottom) = 0;
};

}	// namespace bebox

#endif	// BEBOX_VIDEO_SINK_H

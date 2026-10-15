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

	// A complete frame, called at the emulated vertical retrace on the
	// emulation thread. 'pixels' holds 0x00RRGGBB host-endian values;
	// 'stride' is in pixels. The buffer is only valid during the call.
	// width or height 0 means the display is off.
	virtual void FrameReady(const u32* pixels, int width, int height,
		int stride) = 0;
};

}	// namespace bebox

#endif	// BEBOX_VIDEO_SINK_H

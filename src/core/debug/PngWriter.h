/*
 * BeBox emulator - minimal PNG writer (uncompressed deflate) for
 * screenshots of the emulated display.
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#ifndef BEBOX_PNG_WRITER_H
#define BEBOX_PNG_WRITER_H

#include "../Types.h"

#include <string>

namespace bebox {

// 'pixels' are 0x00RRGGBB values, 'stride' in pixels.
bool WritePng(const std::string& path, const u32* pixels, int width,
	int height, int stride);

}	// namespace bebox

#endif	// BEBOX_PNG_WRITER_H

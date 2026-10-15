/*
 * BeBox emulator - Intel 8237 DMA controllers (as in the 82378 SIO).
 * Copyright 2026, the BeBox emulator authors. MIT License.
 */
#include "Dma8237.h"

#include "../Log.h"

#include <algorithm>

namespace bebox {

// Page register port for each channel (0x80-0x8f; 0x480-0x48f for the
// high page).
static const u8 kPagePort[8] = { 0x87, 0x83, 0x81, 0x82, 0x8f, 0x8b, 0x89,
	0x8a };


Dma8237::Dma8237()
{
	Reset();
}


void
Dma8237::Reset()
{
	memset(fChannels, 0, sizeof(fChannels));
	for (int c = 0; c < 2; c++) {
		Controller& controller = fControllers[c];
		controller.flipFlop = false;
		controller.command = 0;
		controller.status = 0;
		controller.mask = 0x0f;
		controller.request = 0;
		memset(controller.extendedMode, 0, sizeof(controller.extendedMode));
		// transfer size after reset: 8 bit for channels 0-3, 16 bit
		// counted in words for 5-7
		if (&controller == &fControllers[1]) {
			for (int i = 1; i < 4; i++)
				controller.extendedMode[i] = (u8)(i | 0x04);
		}
	}
}


bool
Dma8237::IsMasked(int channel) const
{
	return (fControllers[channel >> 2].mask & (1 << (channel & 3))) != 0;
}


bool
Dma8237::IsWriteToMemory(int channel) const
{
	return ((fChannels[channel].mode >> 2) & 3) == 1;
}


u32
Dma8237::Remaining(int channel) const
{
	const Channel& ch = fChannels[channel];
	u32 units = (u32)ch.count + 1;
	int size = (fControllers[channel >> 2].extendedMode[channel & 3] >> 2)
		& 3;
	return size == 1 ? units * 2 : units;
}


u32
Dma8237::Transfer(int channel, void* buffer, u32 length, bool& terminalCount)
{
	terminalCount = false;
	if (IsMasked(channel))
		return 0;
	Channel& ch = fChannels[channel];
	Controller& controller = _ControllerOf(channel);
	int type = (ch.mode >> 2) & 3;		// 0 verify, 1 write, 2 read
	bool decrement = (ch.mode & 0x20) != 0;
	bool autoInit = (ch.mode & 0x10) != 0;
	// 82378 extended mode (bits 2-3): 0 8-bit, 1 16-bit counted in words
	// with the address shifted, 2 32-bit counted in bytes, 3 16-bit
	// counted in bytes
	int size = (controller.extendedMode[channel & 3] >> 2) & 3;
	bool word = size == 1;
	u32 unit = size == 0 ? 1 : size == 2 ? 4 : 2;
	// the count (and address) step per transfer in register units
	u16 step = word ? 1 : (u16)unit;

	u8* data = (u8*)buffer;
	u32 moved = 0;
	while (moved + unit <= length) {
		u32 address;
		if (word) {
			address = ((u32)ch.highPage << 24) | ((u32)(ch.page & 0xfe) << 16)
				| ((u32)ch.address << 1);
		} else {
			address = ((u32)ch.highPage << 24) | ((u32)ch.page << 16)
				| ch.address;
		}
		if (type == 1 && fWrite)
			fWrite(address, data + moved, unit);
		else if (type == 2 && fRead)
			fRead(address, data + moved, unit);
		moved += unit;
		ch.address += decrement ? -step : step;
		bool done = ch.count < step;
		ch.count -= step;
		if (done) {
			terminalCount = true;
			controller.status |= 1 << (channel & 3);
			if (autoInit) {
				ch.address = ch.baseAddress;
				ch.count = ch.baseCount;
			} else {
				controller.mask |= 1 << (channel & 3);
			}
			break;
		}
	}
	BB_LOG(kLogDma, kLogTrace, "dma channel %d: %u bytes%s", channel, moved,
		terminalCount ? " (terminal count)" : "");
	return moved;
}


void
Dma8237::_WriteController(int c, int reg, u8 value)
{
	Controller& controller = fControllers[c];
	int base = c * 4;
	switch (reg) {
		case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
		{
			Channel& ch = fChannels[base + reg / 2];
			bool isCount = (reg & 1) != 0;
			u16& baseValue = isCount ? ch.baseCount : ch.baseAddress;
			u16& current = isCount ? ch.count : ch.address;
			if (!controller.flipFlop)
				baseValue = (baseValue & 0xff00) | value;
			else
				baseValue = (baseValue & 0x00ff) | (value << 8);
			current = baseValue;
			controller.flipFlop = !controller.flipFlop;
			// writing the low address byte clears the high page (82378)
			if (!isCount && controller.flipFlop)
				ch.highPage = 0;
			break;
		}
		case 8:
			controller.command = value;
			break;
		case 9:
			if ((value & 4) != 0)
				controller.request |= 1 << (value & 3);
			else
				controller.request &= ~(1 << (value & 3));
			break;
		case 10:
			if ((value & 4) != 0)
				controller.mask |= 1 << (value & 3);
			else
				controller.mask &= ~(1 << (value & 3));
			{
				const Channel& ch = fChannels[base + (value & 3)];
				BB_LOG(kLogDma, kLogDebug, "dma channel %d %s (address %02x%02x"
					"%04x count %04x)", base + (value & 3),
					(value & 4) != 0 ? "masked" : "unmasked", ch.highPage,
					ch.page, ch.address, ch.count);
			}
			break;
		case 11:
			fChannels[base + (value & 3)].mode = value;
			BB_LOG(kLogDma, kLogDebug, "dma channel %d mode %02x",
				base + (value & 3), value);
			break;
		case 12:
			controller.flipFlop = false;
			break;
		case 13:	// master clear
			controller.flipFlop = false;
			controller.command = 0;
			controller.status = 0;
			controller.request = 0;
			controller.mask = 0x0f;
			break;
		case 14:
			controller.mask = 0;
			break;
		case 15:
			controller.mask = value & 0x0f;
			break;
	}
}


u8
Dma8237::_ReadController(int c, int reg)
{
	Controller& controller = fControllers[c];
	int base = c * 4;
	if (reg < 8) {
		Channel& ch = fChannels[base + reg / 2];
		u16 value = (reg & 1) != 0 ? ch.count : ch.address;
		u8 result = controller.flipFlop ? value >> 8 : value & 0xff;
		controller.flipFlop = !controller.flipFlop;
		return result;
	}
	if (reg == 8) {
		u8 status = controller.status | (controller.request << 4);
		controller.status &= 0xf0;	// terminal count bits clear on read
		return status;
	}
	if (reg == 15)
		return controller.mask | 0xf0;
	return 0xff;
}


u8
Dma8237::IoRead8(u32 port)
{
	if (port < 0x10)
		return _ReadController(0, port);
	if (port >= 0xc0 && port < 0xe0)
		return _ReadController(1, (port - 0xc0) >> 1);
	for (int channel = 0; channel < 8; channel++) {
		if (port == kPagePort[channel])
			return fChannels[channel].page;
		if (port == 0x400u + kPagePort[channel])
			return fChannels[channel].highPage;
	}
	return 0xff;
}


void
Dma8237::IoWrite8(u32 port, u8 value)
{
	if (port < 0x10) {
		_WriteController(0, port, value);
		return;
	}
	if (port >= 0xc0 && port < 0xe0) {
		_WriteController(1, (port - 0xc0) >> 1, value);
		return;
	}
	if (port == 0x40b || port == 0x4d6) {
		// 82378 extended mode registers
		fControllers[port == 0x4d6 ? 1 : 0].extendedMode[value & 3] = value;
		return;
	}
	for (int channel = 0; channel < 8; channel++) {
		if (port == kPagePort[channel]) {
			fChannels[channel].page = value;
			return;
		}
		if (port == 0x400u + kPagePort[channel]) {
			fChannels[channel].highPage = value;
			return;
		}
	}
}

}	// namespace bebox
